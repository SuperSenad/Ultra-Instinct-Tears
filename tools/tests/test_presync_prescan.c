// [UIT 0249] Box test of TOTK_VK_PRESYNC_PARALLEL: the job hand-out of backend_vk.c (cut from the source by
// test_presync_prescan.py into ps_job.inc) with the real write-watch tracking (src/gfx/nvn/nvn_dirty_win.c) and a
// stand-in for GetWriteWatch. The translation thread's prologue is modelled as shadow_presync does it: a new epoch,
// then (switch on) presync_prescan with three recording threads waiting as mt_thread does, then the same loop as
// before over the pools (nvn_dirty_runs_stamp, copy the runs, new stamp).
//  mode "same":  writes only between submissions; prints a hash of every run copied (pool, offset, bytes) and the
//                write-watch calls the loop itself made: on and off must copy exactly the same runs.
//  mode "race":  game threads write all the time, texture checks scan parts of the pools; at the end every pool's
//                copy must equal memory (a page written but never copied: stale vertices in the game).
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "../../src/gfx/nvn/nvn_dirty.h"

#define NPOOL 64
#define PP 32   // pages a pool
#define NP (NPOOL * PP)
static uint8_t *g_mem;
static _Atomic uint8_t g_bit[NP];
static _Atomic uint64_t g_wcalls;
static __thread uint64_t t_wcalls;
int plat_written_pages(uint64_t addr, uint64_t size, uint64_t *pages, int max, uint64_t *page) {
    *page = 4096;
    t_wcalls++;
    atomic_fetch_add(&g_wcalls, 1);
    uintptr_t b = (uintptr_t)g_mem;
    if (addr < b || addr >= b + (uint64_t)NP * 4096) return 0;
    int n = 0;
    for (uint64_t a = addr; a < addr + size && n < max; a += 4096) {
        uint64_t p = (a - b) / 4096;
        if (atomic_exchange(&g_bit[p], 0)) pages[n++] = a;
        if ((p & 15) == 0) sched_yield();   // a system call: not instant
    }
    return n;
}
// what presync_prescan uses from backend_vk.c
typedef struct { uint32_t sh; uint64_t sync_sub, used_sub, lo, stamp; } pool_t;
static pool_t g_pools[NPOOL];
static int g_npools = NPOOL, g_mt_threads = 4;
static pthread_mutex_t g_mt_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_mt_cv = PTHREAD_COND_INITIALIZER;
static int g_mt_next, g_mt_avail, g_quit;
#define VLOG(...) ((void)0)
static uint64_t runtime_now_ns(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec; }
static _Atomic uint64_t g_ps_jobs, g_ps_items, g_ps_scanned, g_ps_helped, g_ps_ns;
// every item scanned exactly once: the hand-out's calls are counted (an item taken twice is invisible otherwise, the
// second scan finds the epoch already marked)
static _Atomic uint64_t g_ps_calls;
static int counted_prescan(const void *a, uint64_t e) { atomic_fetch_add(&g_ps_calls, 1); return nvn_dirty_epoch_prescan(a, e); }
#define nvn_dirty_epoch_prescan counted_prescan
#include "ps_job.inc"
#undef nvn_dirty_epoch_prescan

static void *worker(void *arg) {   // mt_thread's wait loop (the source's is checked by the .py)
    (void)arg;
    uint32_t ps_seen = 0;
    for (;;) {
        pthread_mutex_lock(&g_mt_mu);
        while (g_mt_next >= g_mt_avail && g_ps_wake == ps_seen && !g_quit) pthread_cond_wait(&g_mt_cv, &g_mt_mu);
        if (g_quit) { pthread_mutex_unlock(&g_mt_mu); return NULL; }
        if (g_mt_next >= g_mt_avail) {
            ps_seen = g_ps_wake;
            pthread_mutex_unlock(&g_mt_mu);
            if (ps_work(ps_seen)) atomic_fetch_add_explicit(&g_ps_helped, 1, memory_order_relaxed);
            continue;
        }
        g_mt_next++;
        pthread_mutex_unlock(&g_mt_mu);
    }
}
static uint64_t g_shadow[NP], g_hash = 1469598103934665603ull, g_loop_wcalls;
static int g_cur;
static void mix(uint64_t v) { g_hash = (g_hash ^ v) * 1099511628211ull; }
static void copy_run(void *ctx, size_t off, size_t n) {
    int pi = (int)(uintptr_t)ctx;
    mix((uint64_t)g_cur); mix((uint64_t)pi); mix(off); mix(n);
    for (size_t p = off / 4096; p < (off + n + 4095) / 4096; p++) {
        size_t gp = (size_t)pi * PP + p;
        g_shadow[gp] = atomic_load((_Atomic uint64_t *)(g_mem + gp * 4096));
    }
}
static void submission(uint64_t sub, uint64_t win) {   // the translation thread's prologue (backend_vk.c)
    nvn_dirty_epoch_begin();
    presync_prescan(sub, win, NULL, 0);   // shadow_presync with t_ps_lead
    uint64_t c0 = t_wcalls;
    for (int i = 0; i < g_npools; i++)
        if (g_pools[i].sh && g_pools[i].sync_sub != sub && g_pools[i].used_sub + win >= sub) {
            nvn_dirty_runs_stamp((const void *)(uintptr_t)g_pools[i].lo, (size_t)PP * 4096, g_pools[i].stamp, 0, copy_run, (void *)(uintptr_t)i, &g_pools[i].stamp);
            g_pools[i].sync_sub = sub;
        }
    g_loop_wcalls += t_wcalls - c0;
}
static uint64_t rnd(uint64_t *s) { *s ^= *s << 13; *s ^= *s >> 7; *s ^= *s << 17; return *s; }
static void write_page(uint64_t p) {
    atomic_fetch_add((_Atomic uint64_t *)(g_mem + p * 4096), 1);
    atomic_store(&g_bit[p], 1);
}
static _Atomic int g_stop_w, g_stop;
static void *writer(void *arg) {
    uint64_t s = (uintptr_t)arg * 0x9E3779B97F4A7C15ull + 1;
    while (!atomic_load(&g_stop_w)) write_page(rnd(&s) % NP);
    return NULL;
}
static void *checker(void *arg) {   // texture checks: parts of pools, each scan resetting the bits
    uint64_t s = (uintptr_t)arg * 77 + 3;
    while (!atomic_load(&g_stop)) {
        uint64_t p = rnd(&s) % NP, n = 1 + rnd(&s) % 16;
        if (p + n > NP) n = NP - p;
        nvn_dirty_since(g_mem + p * 4096, n * 4096, nvn_dirty_now() - rnd(&s) % 4);
    }
    return NULL;
}
int main(int argc, char **argv) {
    int race = argc > 1 && !strcmp(argv[1], "race");
    int rounds = argc > 2 ? atoi(argv[2]) : 300;
    if (posix_memalign((void **)&g_mem, 4096, (size_t)NP * 4096)) return 2;
    memset(g_mem, 0, (size_t)NP * 4096);
    for (int i = 0; i < NPOOL; i++) {
        nvn_dirty_add_range(g_mem + (size_t)i * PP * 4096, (size_t)PP * 4096);
        nvn_dirty_since(g_mem + (size_t)i * PP * 4096, (size_t)PP * 4096, UINT64_MAX);
        g_pools[i] = (pool_t){.sh = (i % 9) != 4, .lo = (uintptr_t)(g_mem + (size_t)i * PP * 4096), .stamp = nvn_dirty_now()};
    }
    pthread_t wk[3], w[2], c[2];
    for (int i = 0; i < 3; i++) pthread_create(&wk[i], NULL, worker, NULL);
    if (race) {
        for (int i = 0; i < 2; i++) pthread_create(&w[i], NULL, writer, (void *)(uintptr_t)i);
        for (int i = 0; i < 2; i++) pthread_create(&c[i], NULL, checker, (void *)(uintptr_t)i);
    }
    uint64_t s = 12345;
    for (int r = 1; r <= rounds; r++) {
        g_cur = r;
        if (!race) for (int k = 0; k < 200; k++) write_page(rnd(&s) % NP);
        for (int i = 0; i < NPOOL; i++) if (rnd(&s) % 4) g_pools[i].used_sub = (uint64_t)r;   // used in this submission
        submission((uint64_t)r, 2);
    }
    if (race) {
        atomic_store(&g_stop_w, 1);
        for (int i = 0; i < 2; i++) pthread_join(w[i], NULL);
        atomic_store(&g_stop, 1);
        for (int i = 0; i < 2; i++) pthread_join(c[i], NULL);
    }
    for (int k = 1; k <= 2; k++) {   // nothing running: every pool once more
        for (int i = 0; i < NPOOL; i++) g_pools[i].used_sub = (uint64_t)(rounds + k);
        g_cur = rounds + k;
        submission((uint64_t)(rounds + k), 2);
    }
    pthread_mutex_lock(&g_mt_mu); g_quit = 1; pthread_cond_broadcast(&g_mt_cv); pthread_mutex_unlock(&g_mt_mu);
    for (int i = 0; i < 3; i++) pthread_join(wk[i], NULL);
    int bad = 0;
    for (int p = 0; p < NP; p++) if (g_pools[p / PP].sh) bad += g_shadow[p] != *(uint64_t *)(g_mem + p * 4096);
    printf("hash=%016llx stale=%d loop_wcalls=%llu wcalls=%llu jobs=%llu items=%llu scanned=%llu helped=%llu calls=%llu\n",
           (unsigned long long)g_hash, bad, (unsigned long long)g_loop_wcalls, (unsigned long long)atomic_load(&g_wcalls),
           (unsigned long long)g_ps_jobs, (unsigned long long)g_ps_items, (unsigned long long)g_ps_scanned, (unsigned long long)g_ps_helped, (unsigned long long)atomic_load(&g_ps_calls));
    return bad != 0;
}
