// [UIT 0025] Box test of the write-watch page tracking (src/gfx/nvn/nvn_dirty_win.c) under concurrency, as the
// recording threads use it: game threads write pages, one thread keeps a copy current the way a memory pool's device
// copy is (sync_gather: nvn_dirty_runs since the copy's stamp, copy those pages, new stamp), and other threads check
// textures (nvn_dirty_since on parts of the same pool, each scan resetting the watch bits), one of them the epoch
// thread (whole-pool scans). Afterwards the copy must equal memory: a page written but never copied is a stale vertex
// in a pool's device copy (exploded geometry) or a texture never uploaded again.
// cc -O2 -pthread -DNEW_API tools/tests/test_dirty_race.c src/gfx/nvn/nvn_dirty_win.c -o /tmp/t && /tmp/t
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "../../src/gfx/nvn/nvn_dirty.h"

#define NP 2048
static uint8_t *g_mem;                  // the pool: NP pages, a counter at the start of each
static _Atomic uint8_t g_bit[NP];       // the OS's write-watch bits
static _Atomic int g_stop, g_stop_w;
static uint64_t g_shadow[NP];

int plat_written_pages(uint64_t addr, uint64_t size, uint64_t *pages, int max, uint64_t *page) {
    *page = 4096;
    uintptr_t b = (uintptr_t)g_mem;
    if (addr < b || addr >= b + (uint64_t)NP * 4096) return 0;
    int n = 0;
    for (uint64_t a = addr; a < addr + size && n < max; a += 4096) {
        uint64_t p = (a - b) / 4096;
        if (atomic_exchange(&g_bit[p], 0)) pages[n++] = a;
        if ((p & 63) == 0) sched_yield();   // GetWriteWatch is a system call: not instant
    }
    return n;
}
static uint64_t rnd(uint64_t *s) { *s ^= *s << 13; *s ^= *s >> 7; *s ^= *s << 17; return *s; }
static void *writer(void *arg) {
    uint64_t s = (uintptr_t)arg * 0x9E3779B97F4A7C15ull + 1;
    while (!atomic_load(&g_stop_w)) {
        uint64_t p = rnd(&s) % NP;
        _Atomic uint64_t *v = (_Atomic uint64_t *)(g_mem + p * 4096);
        atomic_fetch_add(v, 1);
        atomic_store(&g_bit[p], 1);
    }
    return NULL;
}
static void *checker(void *arg) {   // texture checks: parts of the pool
    uint64_t s = (uintptr_t)arg * 77 + 3;
    int epoch = (uintptr_t)arg == 0;
    while (!atomic_load(&g_stop)) {
        if (epoch) nvn_dirty_epoch_begin();
        uint64_t p = rnd(&s) % NP, n = 1 + rnd(&s) % 64;
        if (p + n > NP) n = NP - p;
        nvn_dirty_since(g_mem + p * 4096, n * 4096, nvn_dirty_now() - rnd(&s) % 4);
    }
    return NULL;
}
static uint64_t g_stamp;
static void copy_run(void *ctx, size_t off, size_t n) {
    (void)ctx;
    for (size_t p = off / 4096; p < (off + n + 4095) / 4096; p++) g_shadow[p] = atomic_load((_Atomic uint64_t *)(g_mem + p * 4096));
}
static void sync_once(void) {
#ifdef NEW_API
    nvn_dirty_runs_stamp(g_mem, (size_t)NP * 4096, g_stamp, 0, copy_run, NULL, &g_stamp);
#else
    nvn_dirty_runs(g_mem, (size_t)NP * 4096, g_stamp, 0, copy_run, NULL);
    g_stamp = nvn_dirty_now();   // as sync_gather took it before (after the scan)
#endif
}
static void *syncer(void *arg) {
    (void)arg;
    while (!atomic_load(&g_stop)) sync_once();
    return NULL;
}
int main(int argc, char **argv) {
    int secs = argc > 1 ? atoi(argv[1]) : 2;
    if (posix_memalign((void **)&g_mem, 4096, (size_t)NP * 4096)) return 2;
    memset(g_mem, 0, (size_t)NP * 4096);
    nvn_dirty_add_range(g_mem, (size_t)NP * 4096);
    nvn_dirty_since(g_mem, (size_t)NP * 4096, UINT64_MAX);
    g_stamp = nvn_dirty_now();
    pthread_t w[2], c[5], y;
    for (int i = 0; i < 2; i++) pthread_create(&w[i], NULL, writer, (void *)(uintptr_t)i);
    for (int i = 0; i < 5; i++) pthread_create(&c[i], NULL, checker, (void *)(uintptr_t)i);
    pthread_create(&y, NULL, syncer, NULL);
    struct timespec ts = {secs, 0};
    nanosleep(&ts, NULL);
    atomic_store(&g_stop_w, 1);
    for (int i = 0; i < 2; i++) pthread_join(w[i], NULL);
    ts.tv_sec = 0; ts.tv_nsec = 200000000; nanosleep(&ts, NULL);   // checkers and the copy go on a little
    atomic_store(&g_stop, 1);
    for (int i = 0; i < 5; i++) pthread_join(c[i], NULL);
    pthread_join(y, NULL);
    sync_once(); sync_once();   // nothing running: whatever is still missing was lost
    int bad = 0;
    for (int p = 0; p < NP; p++) bad += g_shadow[p] != *(uint64_t *)(g_mem + p * 4096);
    printf("%s: %d of %d pages stale in the copy\n", bad ? "FAIL" : "ok", bad, NP);
    return bad != 0;
}
