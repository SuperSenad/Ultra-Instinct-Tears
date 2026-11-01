// [UIT 0141] Box test for src/gfx/vkb_umerge.h (the uniform capture's merge, one hold of the pool lock per 64 lookups):
//  1. same output as the loop before 0141 (each lookup taking the lock itself, as mtl_addr does) on random pool tables
//     and sorted ranges: overlapping, touching, nested, outside every pool, over the merge limit; hold limits 1..64
//  2. the lock taken once per run of at most 64 lookups (+1 for the two of one range), always released, no lookup outside it
//  3. speed with 4 other threads taking the same reader-writer lock for their own lookups (the recording threads) and
//     one taking it to write now and then: 20000 ranges, per-lookup lock vs held
// cc -O2 -pthread -Isrc/gfx tools/tests/test_umerge.c -o /tmp/t/um && /tmp/t/um
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "vkb_rsort.h"
#include "vkb_umerge.h"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)
static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e3 + t.tv_nsec / 1e6; }
static uint64_t s = 88172645463325252ull;
static uint64_t rnd(void) { s ^= s << 13; s ^= s >> 7; s ^= s << 17; return s; }

typedef struct { uint64_t lo, hi; } pool;
static pool g_pools[256];
static int g_np;
static pthread_rwlock_t g_mu = PTHREAD_RWLOCK_INITIALIZER;
static int g_holds, g_held, g_bad_release;
static long g_looks;
static int find(uint64_t a, uint64_t len) {   // backend_vk.c pool_find: sorted by lo, the closest few below
    int lo = 0, hi = g_np - 1, f = -1;
    while (lo <= hi) { int m = (lo + hi) / 2; if (g_pools[m].lo <= a) { f = m; lo = m + 1; } else hi = m - 1; }
    for (int i = f; i >= 0 && i > f - 4; i--) if (a >= g_pools[i].lo && a + len <= g_pools[i].hi) return 1;
    return 0;
}
static int addr_locked(uint64_t a, uint64_t len) {   // mtl_addr: the lock for each lookup
    pthread_rwlock_rdlock(&g_mu);
    int r = find(a, len);
    pthread_rwlock_unlock(&g_mu);
    return r;
}
static int hold(void) { pthread_rwlock_rdlock(&g_mu); g_holds++; g_held = 1; return 1; }
static int hold_off(void) { return 0; }
static void release(void) { if (!g_held) g_bad_release++; g_held = 0; pthread_rwlock_unlock(&g_mu); }
static int in_held(uint64_t a, uint64_t len) { if (!g_held) g_bad_release++; g_looks++; return find(a, len); }

static int merge_ref(ucap_rng *v, int nv, uint64_t merge_max) {   // vkb_ucap.c before UIT 0141
    int k = -1;
    for (int i = 0; i < nv; i++) {
        if (k >= 0 && v[i].lo <= v[k].hi) {
            if (v[i].hi <= v[k].hi) { v[k].geo |= v[i].geo; continue; }
            if (v[i].hi - v[k].lo <= merge_max && addr_locked(v[k].lo, v[i].hi - v[k].lo)) { v[k].hi = v[i].hi; v[k].geo |= v[i].geo; continue; }
        }
        if (!addr_locked(v[i].lo, v[i].hi - v[i].lo)) continue;
        v[++k] = v[i];
    }
    return k + 1;
}
static void make_pools(void) {
    g_np = 1 + (int)(rnd() % 200);
    uint64_t a = 0x10000000;
    for (int i = 0; i < g_np; i++) {
        a += (rnd() % 4 ? 0 : rnd() % (1 << 20)) & ~0xfffull;   // touching or with a gap
        uint64_t len = (1 + rnd() % 64) << 16;
        g_pools[i] = (pool){a, a + len};
        a += len;
    }
}
static void make_ranges(ucap_rng *v, int n) {
    uint64_t span = g_pools[g_np - 1].hi - g_pools[0].lo + (8 << 20);
    for (int i = 0; i < n; i++) {
        uint64_t lo = g_pools[0].lo - (4 << 20) + rnd() % span, len = rnd() % 3 ? 16 + rnd() % 4096 : rnd() % (6 << 20);
        if (rnd() % 8 == 0 && i) lo = v[i - 1].lo;                       // starting together
        if (rnd() % 8 == 0 && i) lo = v[i - 1].hi;                       // touching
        v[i] = (ucap_rng){lo, lo + len, rnd() % 5 == 0};
    }
    ucap_rng_sort(v, n);
}

static _Atomic int g_stop;
static void *reader(void *a) {   // a recording thread: its own pool lookups through the same lock
    uint64_t x = (uintptr_t)a * 7919 + 1;
    while (!atomic_load(&g_stop)) { x = x * 6364136223846793005ull + 1; addr_locked(g_pools[0].lo + (x >> 40) % 0x100000, 64); }
    return NULL;
}
static void *writer(void *a) {   // a storing draw: the table written now and then
    (void)a;
    struct timespec ts = {0, 200000};
    while (!atomic_load(&g_stop)) { pthread_rwlock_wrlock(&g_mu); pthread_rwlock_unlock(&g_mu); nanosleep(&ts, NULL); }
    return NULL;
}

int main(void) {
    int cases = 0, diffs = 0, holdbad = 0;
    for (int t = 0; t < 3000; t++) {
        make_pools();
        int n = 1 + (int)(rnd() % 3000);
        ucap_rng *a = malloc(sizeof *a * n), *b = malloc(sizeof *b * n);
        make_ranges(a, n);
        memcpy(b, a, sizeof *a * n);
        uint64_t mm = t % 3 ? (4 << 20) : 1 + rnd() % (1 << 20);
        int hm = 1 + t % 64;
        int na = merge_ref(a, n, mm);
        g_holds = 0; g_looks = 0;
        int nb = ucap_merge(b, n, mm, hm, hold, release, in_held);
        if (na != nb || memcmp(a, b, sizeof *a * na)) diffs++;
        // a run is at most hm + 1 lookups (two in one range) and at least hm before the next hold
        if (g_held || g_holds < 1 || g_holds > (int)(g_looks / hm) + 1 || g_holds < (int)(g_looks / (hm + 1))) holdbad++;
        cases++;
        free(a); free(b);
    }
    CHECK(diffs == 0 && holdbad == 0 && g_bad_release == 0);
    printf("1-2. %d random cases (hold limits 1..64): %d different from the loop before 0141, %d with a wrong number of holds, %d lookups outside a hold\n",
           cases, diffs, holdbad, g_bad_release);
    ucap_rng one = {1, 2, 0};
    CHECK(ucap_merge(&one, 1, 64, 64, hold_off, release, in_held) == 0 && g_bad_release == 0);   // not running: nothing, no release

    make_pools();
    int n = 20000;
    ucap_rng *base = malloc(sizeof *base * n), *w = malloc(sizeof *w * n);
    make_ranges(base, n);
    pthread_t th[5];
    for (int i = 0; i < 4; i++) pthread_create(&th[i], NULL, reader, (void *)(uintptr_t)(i + 1));
    pthread_create(&th[4], NULL, writer, NULL);
    double ref = 1e9, held = 1e9;
    for (int r = 0; r < 30; r++) {
        memcpy(w, base, sizeof *w * n);
        double t0 = now();
        merge_ref(w, n, 4 << 20);
        double t1 = now();
        memcpy(w, base, sizeof *w * n);
        double t2 = now();
        ucap_merge(w, n, 4 << 20, 64, hold, release, in_held);
        double t3 = now();
        if (t1 - t0 < ref) ref = t1 - t0;
        if (t3 - t2 < held) held = t3 - t2;
    }
    atomic_store(&g_stop, 1);

    for (int i = 0; i < 5; i++) pthread_join(th[i], NULL);
    free(base); free(w);
    printf("3. merge of %d ranges, 4 readers + 1 writer on the lock: per-lookup lock %.3f ms, held per 64 %.3f ms (best of 30)\n", n, ref, held);
    printf(fails ? "FAILED\n" : "ok\n");
    return fails != 0;
}
