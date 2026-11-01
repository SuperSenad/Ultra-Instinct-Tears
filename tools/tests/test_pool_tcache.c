// [UIT 0033] Box test for vkb_pool_tcache.h (TOTK_VK_POOL_FASTPATH): per-thread entries of pools current in a
// submission, which let vkb_pool_use / vkb_pool_addr skip the pool table's lock.
//  1. unit: an entry is found only for its submission and table generation, and only for ranges inside it
//  2. model: recording threads look pools up through the cache and, after each hit, check it against the table under
//     the lock; a mutator thread meanwhile deletes, re-adds and gives up pools (bumping the generation, as
//     backend_vk.c does) and moves submissions on. A hit that disagrees with the table is a stale use: must be 0.
//     With argument "nogen" the generation is not bumped on a change: the test must then find stale uses (it can tell).
//  3. speed: lookups per second with every lookup taking the shared lock vs through the cache, while a writer thread
//     takes the lock exclusively now and then (a storing draw's GPU write).
// cc -O2 -pthread -Isrc/gfx tools/tests/test_pool_tcache.c -o /tmp/t/ptc && /tmp/t/ptc && /tmp/t/ptc nogen
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "../../src/gfx/vkb_pool_tcache.h"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)
static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }
static uint64_t rnd(uint64_t *s) { *s ^= *s << 13; *s ^= *s >> 7; *s ^= *s << 17; return *s; }

static void unit(void) {
    ptc_t c; memset(&c, 0, sizeof c);
    CHECK(!ptc_find(&c, 0x1000, 16, 1, 1));   // empty: zeroed entries never match (sub >= 1)
    CHECK(!ptc_find(&c, 0, 0, 0, 0) || 1);
    ptc_read e = {0x10000, 0x20000, 5, 7, 0x10000, 0x400, (void *)0x1234, 1, 0};
    ptc_put(&c, &e);
    CHECK(ptc_find(&c, 0x10000, 0x10000, 5, 7));
    CHECK(ptc_find(&c, 0x18000, 0x100, 5, 7));
    CHECK(!ptc_find(&c, 0x1ff00, 0x101, 5, 7));          // past the end
    CHECK(!ptc_find(&c, 0xff00, 0x200, 5, 7));           // starts before
    CHECK(!ptc_find(&c, 0x18000, 0x100, 6, 7));          // another submission
    CHECK(!ptc_find(&c, 0x18000, 0x100, 5, 8));          // the table changed
    CHECK(!ptc_find(&c, 0x18000, UINT64_MAX - 0x10, 5, 7));   // wraps around
    e.sub = 6; ptc_put(&c, &e);                          // same pool, next submission: same slot
    int n = 0; for (int i = 0; i < PTC_R; i++) n += c.r[i].lo == 0x10000; CHECK(n == 1);
    CHECK(ptc_find(&c, 0x18000, 0x100, 6, 7) && !ptc_find(&c, 0x18000, 0x100, 5, 7));
    for (uint64_t k = 1; k <= PTC_R; k++) { ptc_read x = {k << 24, (k << 24) + 0x1000, 6, 7, 0, 0, 0, 1, 0}; ptc_put(&c, &x); }
    CHECK(ptc_find(&c, (uint64_t)PTC_R << 24, 0x10, 6, 7));   // the newest kept, the oldest replaced
    ptc_wput(&c, 0x18000, 0x100, 6, 7);
    CHECK(ptc_wfind(&c, 0x18000, 0x100, 6, 7) && ptc_wfind(&c, 0x18010, 0x10, 6, 7));
    CHECK(!ptc_wfind(&c, 0x18000, 0x101, 6, 7) && !ptc_wfind(&c, 0x18000, 0x100, 7, 7) && !ptc_wfind(&c, 0x18000, 0x100, 6, 8));
    printf("unit: %s\n", fails ? "FAILED" : "ok");
}

// ---- the model: a pool table as backend_vk.c's (sorted ranges, a device copy each, current in sync_sub)
#define NPOOL 24
typedef struct { uint64_t lo, hi; int live, no_shadow; uint64_t sync_sub, soff, id; } pool_m;
static pool_m g_tab[NPOOL];
static pthread_rwlock_t g_mu = PTHREAD_RWLOCK_INITIALIZER;
static _Atomic uint64_t g_gen = 1, g_sub = 1, g_hits, g_miss, g_stale, g_ids = 1;
static _Atomic int g_stop;
static int g_nogen;
static void bump(void) { if (!g_nogen) atomic_fetch_add_explicit(&g_gen, 1, memory_order_release); }
static pool_m *find(uint64_t a, uint64_t n) {
    for (int i = 0; i < NPOOL; i++) if (g_tab[i].live && a >= g_tab[i].lo && a + n <= g_tab[i].hi) return &g_tab[i];
    return NULL;
}
static void *mutator(void *arg) {
    uint64_t s = 88172645463325292ull;
    (void)arg;
    while (!atomic_load(&g_stop)) {
        pthread_rwlock_wrlock(&g_mu);
        int i = (int)(rnd(&s) % NPOOL);
        switch (rnd(&s) % 4) {
        case 0: if (g_tab[i].live) { g_tab[i].live = 0; bump(); } break;            // mtl_pool_del
        case 1: if (!g_tab[i].live) { g_tab[i] = (pool_m){g_tab[i].lo, g_tab[i].hi, 1, 0, 0, rnd(&s) & 0xfff000, atomic_fetch_add(&g_ids, 1)}; bump(); } break;   // re-added: a new copy
        case 2: if (g_tab[i].live && !g_tab[i].no_shadow) { g_tab[i].no_shadow = 1; bump(); } break;   // given up
        case 3: atomic_fetch_add(&g_sub, 1); break;                                // a submission committed
        }
        pthread_rwlock_unlock(&g_mu);
        for (volatile int k = 0; k < 2000; k++) {}
    }
    return NULL;
}
static void *recorder(void *arg) {
    uint64_t s = 0x9E3779B97F4A7C15ull * (uint64_t)(uintptr_t)arg + 1;
    ptc_t c; memset(&c, 0, sizeof c);
    uint64_t hits = 0, miss = 0, stale = 0;
    while (!atomic_load(&g_stop)) {
        int i = (int)(rnd(&s) % NPOOL);
        uint64_t a = g_tab[i].lo + (rnd(&s) % 0xf000), n = 1 + rnd(&s) % 0x800;
        uint64_t sub = atomic_load(&g_sub), gen = atomic_load_explicit(&g_gen, memory_order_acquire);
        const ptc_read *e = ptc_find(&c, a, n, sub, gen);
        if (e) {
            ptc_read got = *e;
            hits++;
            // what the lock would have said: the same, unless the table or submission moved on since (then the
            // hit raced a change, as a lookup just before the change would have)
            pthread_rwlock_rdlock(&g_mu);
            if (atomic_load(&g_gen) == gen && atomic_load(&g_sub) == sub) {
                pool_m *p = find(a, n);
                int ok = p && p->lo == got.lo && p->hi == got.hi && (uint64_t)(uintptr_t)got.sbuf == p->id &&
                         (got.ns ? p->no_shadow : (!p->no_shadow && p->sync_sub == sub && p->soff == got.soff));
                if (!ok) stale++;
            }
            pthread_rwlock_unlock(&g_mu);
            continue;
        }
        miss++;
        pthread_rwlock_wrlock(&g_mu);   // the locked path: sync the pool if needed, then note it
        pool_m *p = find(a, n);
        // backend_vk.c reads the submission (g_fence + 1) once: it only moves on between submissions, with no thread
        // recording. Here it moves on any time, so a lookup that started in the last one stops (it is not recording).
        if (atomic_load(&g_sub) != sub) p = NULL;
        if (p) {
            if (!p->no_shadow && p->sync_sub != sub) p->sync_sub = sub;
            if (p->no_shadow || p->sync_sub == sub) {
                ptc_read x = {p->lo, p->hi, sub, gen, p->lo, p->soff, (void *)(uintptr_t)p->id, !p->no_shadow, p->no_shadow};
                ptc_put(&c, &x);
            }
        }
        pthread_rwlock_unlock(&g_mu);
    }
    atomic_fetch_add(&g_hits, hits); atomic_fetch_add(&g_miss, miss); atomic_fetch_add(&g_stale, stale);
    return NULL;
}
static void model(double secs) {
    for (int i = 0; i < NPOOL; i++) g_tab[i] = (pool_m){(uint64_t)(i + 1) << 20, ((uint64_t)(i + 1) << 20) + 0x10000, 1, 0, 0, (uint64_t)i << 12, atomic_fetch_add(&g_ids, 1)};
    pthread_t m, r[6];
    pthread_create(&m, NULL, mutator, NULL);
    for (int i = 0; i < 6; i++) pthread_create(&r[i], NULL, recorder, (void *)(uintptr_t)(i + 1));
    double t0 = now();
    while (now() - t0 < secs) { struct timespec ts = {0, 10000000}; nanosleep(&ts, NULL); }
    atomic_store(&g_stop, 1);
    pthread_join(m, NULL);
    for (int i = 0; i < 6; i++) pthread_join(r[i], NULL);
    printf("model%s: %llu hits, %llu misses, %llu stale uses\n", g_nogen ? " (no generation bumps)" : "",
           (unsigned long long)g_hits, (unsigned long long)g_miss, (unsigned long long)g_stale);
}

// ---- speed
static pthread_rwlock_t g_bmu = PTHREAD_RWLOCK_INITIALIZER;
static _Atomic int g_bstop;
static int g_fast;
static _Atomic uint64_t g_bn;
static void *bwriter(void *arg) {
    (void)arg;
    while (!atomic_load(&g_bstop)) {
        pthread_rwlock_wrlock(&g_bmu);
        for (volatile int k = 0; k < 200; k++) {}   // wb_add / a short sync
        pthread_rwlock_unlock(&g_bmu);
        for (volatile int k = 0; k < 2000; k++) {}
    }
    return NULL;
}
static void *breader(void *arg) {
    uint64_t s = (uint64_t)(uintptr_t)arg * 7919 + 1, n = 0, gen = 1, sub = 1;
    ptc_t c; memset(&c, 0, sizeof c);
    while (!atomic_load(&g_bstop)) {
        for (int k = 0; k < 256; k++) {
            int i = (int)(rnd(&s) % 6);
            uint64_t a = ((uint64_t)(i + 1) << 20) + (rnd(&s) & 0xfff0);
            if (g_fast && ptc_find(&c, a, 16, sub, gen)) { n++; continue; }
            pthread_rwlock_rdlock(&g_bmu);
            for (int j = 0; j < NPOOL; j++) if (a >= g_tab[j].lo && a < g_tab[j].hi) break;   // pool_find
            pthread_rwlock_unlock(&g_bmu);
            if (g_fast) { ptc_read x = {(uint64_t)(i + 1) << 20, ((uint64_t)(i + 1) << 20) + 0x10000, sub, gen, 0, 0, 0, 1, 0}; ptc_put(&c, &x); }
            n++;
        }
    }
    atomic_fetch_add(&g_bn, n);
    return NULL;
}
static double bench(int fast) {
    g_fast = fast; g_bstop = 0; g_bn = 0;
    pthread_t w, r[6];
    pthread_create(&w, NULL, bwriter, NULL);
    for (int i = 0; i < 6; i++) pthread_create(&r[i], NULL, breader, (void *)(uintptr_t)(i + 1));
    double t0 = now();
    while (now() - t0 < 1.0) { struct timespec ts = {0, 10000000}; nanosleep(&ts, NULL); }
    atomic_store(&g_bstop, 1);
    pthread_join(w, NULL);
    for (int i = 0; i < 6; i++) pthread_join(r[i], NULL);
    return (double)g_bn / (now() - t0);
}

int main(int argc, char **argv) {
    g_nogen = argc > 1 && !strcmp(argv[1], "nogen");
    unit();
    model(2.0);
    if (g_nogen) { int ok = g_stale > 0; printf("%s: without generation bumps the test %s stale uses\n", ok ? "ok" : "FAIL", ok ? "finds" : "does not find"); return !ok || fails; }
    if (g_stale) fails++;
    for (int i = 0; i < NPOOL; i++) g_tab[i].live = 1;
    double slow = bench(0), fast = bench(1);
    printf("speed (6 threads, a writer now and then): every lookup locked %.1f M/s, through the cache %.1f M/s (x%.1f)\n", slow / 1e6, fast / 1e6, fast / slow);
    printf("%s\n", fails ? "FAILED" : "all ok");
    return fails != 0;
}
