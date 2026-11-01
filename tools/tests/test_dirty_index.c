// [UIT 0240] Box test of TOTK_DIRTY_INDEX (src/gfx/nvn/nvn_dirty_win.c): memory pools found by address through a sorted
// index instead of going through every slot.
//  1. model: random adds and removes (nested, overlapping and same-base ranges too); every lookup's answer is a live
//     range holding the address, and "none" only where the slot search finds none too; without overlaps the very
//     same range as the slot search.
//  2. nesting deeper than the index walks: still found (the slots answer).
//  3. threads: a writer adds and removes pools while readers look up addresses of pools that stay: never "none",
//     never a range not holding the address (run under -fsanitize=thread too).
//  4. timing at Lookout Landing's pool count (2,921 live of ~3,500 slots): index vs slots, and TOTK_DIRTY_INDEX=0.
// cc -O2 -pthread -Wall -Werror -Isrc/gfx/nvn tools/tests/test_dirty_index.c -o /tmp/t && /tmp/t
// cc -O1 -g -pthread -fsanitize=thread -Isrc/gfx/nvn tools/tests/test_dirty_index.c -o /tmp/t && /tmp/t quick
#include "../../src/gfx/nvn/nvn_dirty_win.c"
#include <stdio.h>
#include <time.h>

int plat_written_pages(uint64_t addr, uint64_t size, uint64_t *pages, int max, uint64_t *page) {
    (void)addr; (void)size; (void)pages; (void)max; *page = 4096; return 0;   // addresses are never dereferenced here
}
static uint64_t tnow(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec; }
static __thread uint64_t rs = 88172645463325252ull;
static uint64_t rnd(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return rs; }
static int fails;
#define CHECK(c, ...) do { if (!(c)) { if (fails++ < 20) { printf("FAIL: " __VA_ARGS__); printf("\n"); } } } while (0)

// all live slots: does any hold a? (the old answer's existence)
static int any_holds(uintptr_t a) { return find_slow(a) != NULL; }
static void check_one(uintptr_t a, int no_overlap) {
    t_last = MAX_RANGE;   // no cached answer: the index (or the slots) decides
    range_t *r = find(a);
    if (r) CHECK(atomic_load(&r->live) && a >= r->base && a < r->end, "found a range not holding %#llx", (unsigned long long)a);
    else CHECK(!any_holds(a), "index says none for %#llx but a live range holds it", (unsigned long long)a);
    if (no_overlap) { t_last = MAX_RANGE; range_t *s = find_slow(a); CHECK(r == s, "index and slots differ at %#llx", (unsigned long long)a); }
}
static void reset_all(void) {   // (test only) forget every range
    pthread_mutex_lock(&g_slot_mu);
    int n = atomic_load(&g_nr);
    for (int i = 0; i < n; i++) if (atomic_load(&g_r[i].live)) { atomic_store(&g_r[i].live, 0); atomic_fetch_sub(&g_live, 1); ix_remove(i); }
    pthread_mutex_unlock(&g_slot_mu);
    CHECK(atomic_load(&g_ix_n) == 0, "index not empty after removing all (%d)", atomic_load(&g_ix_n));
}

static void model(int overlap, int steps) {
    enum { MAXP = 3000 };
    static uintptr_t base[MAXP], len[MAXP]; static int live[MAXP];
    memset(live, 0, sizeof live);
    const uintptr_t W0 = 0x0900000000ull, SPAN = 64ull << 20;   // 64 MiB of address space: dense, so neighbours touch
    for (int s = 0; s < steps; s++) {
        int i = (int)(rnd() % MAXP);
        if (!live[i]) {
            uintptr_t b = W0 + (rnd() % (SPAN / 4096)) * 4096, l = (1 + rnd() % 64) * 4096;
            if (!overlap) {   // keep ranges apart: skip if it would overlap a live one
                int clash = 0;
                for (int k = 0; k < MAXP && !clash; k++) if (live[k] && b < base[k] + len[k] && base[k] < b + l) clash = 1;
                if (clash) continue;
            } else if (rnd() % 8 == 0) {   // same base as another live one
                for (int k = 0; k < MAXP; k++) if (live[k]) { b = base[k]; break; }
            }
            int dup = 0;   // (remove_range finds by base: keep bases unique among live ones for the model)
            for (int k = 0; k < MAXP; k++) if (live[k] && base[k] == b) dup = 1;
            if (dup) continue;
            base[i] = b; len[i] = l; live[i] = 1;
            nvn_dirty_add_range((void *)b, l);
        } else if (!overlap) {
            nvn_dirty_remove_range((void *)base[i]); live[i] = 0;
        } else {   // overlapping: remove_range removes whichever range find gives for the base; mirror that
            t_last = MAX_RANGE;
            range_t *r = find(base[i]);
            nvn_dirty_remove_range((void *)base[i]);
            for (int k = 0; k < MAXP; k++) if (live[k] && r && base[k] == r->base && base[k] + len[k] == r->end) { live[k] = 0; break; }
        }
        for (int q = 0; q < 8; q++) check_one(W0 - 8192 + rnd() % (SPAN + 65 * 4096 + 16384), !overlap);
    }
    reset_all();
}

static void nesting(void) {   // 40 ranges inside each other: deeper than IX_WALK
    uintptr_t b = 0x2000000000ull;
    for (int i = 0; i < 40; i++) nvn_dirty_add_range((void *)(b + (uintptr_t)i * 4096), (uintptr_t)(200 - 2 * i) * 4096);
    nvn_dirty_add_range((void *)(b + 300 * 4096), 4096);
    for (uintptr_t a = b - 4096; a < b + 302 * 4096; a += 2048) check_one(a, 0);
    reset_all();
}

static _Atomic int g_stop;
static uintptr_t g_keep[512];
static void *reader(void *arg) {
    (void)arg;
    uint64_t n = 0;
    while (!atomic_load(&g_stop)) {
        int i = (int)(rnd() % 512);
        uintptr_t a = g_keep[i] + rnd() % 4096;
        t_last = MAX_RANGE;
        range_t *r = find(a);
        CHECK(r && a >= r->base && a < r->end, "reader: pool %d at %#llx not found while it stays", i, (unsigned long long)a);
        t_last = MAX_RANGE;
        CHECK(!find(0x3f00000000ull + rnd() % 65536), "reader: found a range where none ever is");
        n++;
    }
    return (void *)(uintptr_t)n;
}
static void threads(int quick) {
    uintptr_t b = 0x3000000000ull;
    for (int i = 0; i < 512; i++) { g_keep[i] = b + (uintptr_t)i * 3 * 65536; nvn_dirty_add_range((void *)g_keep[i], 65536); }
    pthread_t th[4];
    atomic_store(&g_stop, 0);
    for (int i = 0; i < 4; i++) pthread_create(&th[i], NULL, reader, NULL);
    int rounds = quick ? 3000 : 40000;
    for (int k = 0; k < rounds; k++) {   // pools coming and going between the ones that stay
        uintptr_t a = b + (uintptr_t)(rnd() % 512) * 3 * 65536 + 65536 + (rnd() % 16) * 4096;
        nvn_dirty_add_range((void *)a, 4096 * (1 + rnd() % 16));
        nvn_dirty_remove_range((void *)a);
    }
    atomic_store(&g_stop, 1);
    uint64_t looks = 0;
    for (int i = 0; i < 4; i++) { void *r; pthread_join(th[i], &r); looks += (uintptr_t)r; }
    printf("threads: %d adds + removes against %llu lookups of pools that stay\n", rounds, (unsigned long long)looks);
    reset_all();
}

static double timing(int frames) {   // Lookout Landing: 3,524 pools made, 603 deleted; 441 looked up in address order
    enum { N = 3524 };
    static uintptr_t B[N], L[N]; static int dead[N], live[N];
    uintptr_t a = 0x0900000000ull;
    for (int i = 0; i < N; i++) { L[i] = (1 + rnd() % 1024) * 4096; B[i] = a; a += L[i] + (rnd() % 64) * 4096; }
    int ord[N];
    for (int i = 0; i < N; i++) ord[i] = i;
    for (int i = N - 1; i > 0; i--) { int j = (int)(rnd() % (uint64_t)(i + 1)), t = ord[i]; ord[i] = ord[j]; ord[j] = t; }
    for (int i = 0; i < N; i++) nvn_dirty_add_range((void *)B[ord[i]], L[ord[i]]);
    for (int i = 0; i < 603; i++) { int k = ord[(i * 7) % N]; if (!dead[k]) { dead[k] = 1; nvn_dirty_remove_range((void *)B[k]); } }
    int nl = 0;
    for (int i = 0; i < N; i++) if (!dead[i]) live[nl++] = i;
    volatile int sink = 0;
    uint64_t t0 = tnow();
    for (int f = 0; f < frames; f++)
        for (int i = 0; i < 441; i++) { int p = live[(i * 6) % nl]; sink += nvn_dirty_written_after((void *)B[p], 4096, ~0ull); }
    double ms = (double)(tnow() - t0) / 1e6 / frames;
    (void)sink;
    reset_all();
    return ms;
}

int main(int argc, char **argv) {
    int quick = argc > 1 && !strcmp(argv[1], "quick");
    nvn_dirty_add_range((void *)0x0800000000ull, 4096);   // (the first add asks TOTK_DIRTY_INDEX)
    reset_all();
    if (ix_on()) {
        model(0, quick ? 3000 : 20000);
        model(1, quick ? 3000 : 20000);
        nesting();
        threads(quick);
        uint64_t h, none, slow; nvn_dirty_index_stats(&h, &none, &slow);
        printf("index: %llu found, %llu none, %llu left to the slots (raced a writer or nested deep)\n", (unsigned long long)h, (unsigned long long)none, (unsigned long long)slow);
        CHECK(h > 0 && none > 0, "the index answered nothing");
    }
    double t = timing(quick ? 20 : 200);
    printf("441 pool lookups in address order (a submission's pool copies) among 2,921 live pools: %.3f ms (TOTK_DIRTY_INDEX=%d)\n", t, ix_on());
    if (ix_on() && !quick) CHECK(t < 0.25, "the index is not faster than the slot search was (~1 ms on this box)");
    printf(fails ? "FAIL (%d)\n" : "ok\n", fails);
    return fails != 0;
}
