// [UIT 0128] src/gfx/vkb_rsort.h: the capture's range sort gives the order of a stable sort by lo, then hi descending, on random,
// duplicate-heavy, sorted, reversed and wide-address inputs; and the time against qsort (the code it replaces).
// cc -O2 -Isrc/gfx tools/tests/test_rsort.c -o /tmp/rs && /tmp/rs
#include <stdio.h>
#include <time.h>
#include "vkb_rsort.h"

static uint64_t s = 88172645463325252ull;
static uint64_t rnd(void) { s ^= s << 13; s ^= s >> 7; s ^= s << 17; return s; }
static int cmp_lo(const void *a, const void *b) { const ucap_rng *x = a, *y = b; return x->lo < y->lo ? -1 : x->lo > y->lo; }
static int cmp_ref(const void *a, const void *b) {   // (lo, hi descending, input index in geo): the stable order wanted
    const ucap_rng *x = a, *y = b;
    if (x->lo != y->lo) return x->lo < y->lo ? -1 : 1;
    if (x->hi != y->hi) return x->hi > y->hi ? -1 : 1;
    return x->geo < y->geo ? -1 : x->geo > y->geo;
}
static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e3 + t.tv_nsec / 1e6; }
static void fill(ucap_rng *v, int n, int kind) {
    for (int i = 0; i < n; i++) {
        uint64_t a, len = 256 + rnd() % 4096;
        switch (kind) {
        case 0: a = 0x1030000000ull + (rnd() % (64u << 20)) / 256 * 256; break;   // a pool, 256-aligned bindings
        case 1: a = 0x1030000000ull + (rnd() % 8) * 256; len = 256 * (1 + rnd() % 3); break;   // a few addresses, many repeats
        case 2: a = 0x1030000000ull + (uint64_t)i * 512; break;                    // in order
        case 3: a = 0x1030000000ull + (uint64_t)(n - i) * 512; break;              // reversed
        case 4: a = rnd() & 0xffffffffffffull; break;                              // the whole 48-bit space
        case 5: a = 0x1030000000ull; len = 256 * (1 + rnd() % 64); break;         // one address, sizes vary
        case 6: a = 0x1030000000ull + (rnd() % 4) * 256; len = 1 + (rnd() & 0xffffffffffull); break;   // few addresses, ends far apart
        case 7: a = 16; len = 984 + rnd() % 100; break;   // ends whose keys straddle a digit boundary (2048): radix key offsets
        default: a = 0x7f0000000000ull + (rnd() % 4) * 256; len = 1 + rnd() % (1ull << 46); break;      // ... high in the address space
        }
        v[i] = (ucap_rng){a, a + len, (uint64_t)i};
    }
}
int main(void) {
    int bad = 0, cases = 0;
    static ucap_rng a[70000], b[70000];
    int sizes[] = {0, 1, 2, 3, 17, 47, 95, 96, 97, 100, 1000, 5000, 20000, 65536};
    for (int rep = 0; rep < 20; rep++)
        for (int k = 0; k < 9; k++)
            for (unsigned z = 0; z < sizeof sizes / sizeof *sizes; z++) {
                int n = sizes[z];
                fill(a, n, k);
                memcpy(b, a, sizeof *a * (size_t)n);
                if (ucap_rng_sort(a, n)) { printf("alloc failed\n"); return 1; }
                qsort(b, (size_t)n, sizeof *b, cmp_ref);
                cases++;
                if (n && memcmp(a, b, sizeof *a * (size_t)n)) { bad++; if (bad < 5) printf("MISMATCH kind %d n %d\n", k, n); }
            }
    printf("%d cases, %d wrong (order: stable by lo, then longest first)\n", cases, bad);
    for (int k = 0; k < 3; k++) {
        int n = 20000;
        double tq = 0, tr = 0;
        for (int it = 0; it < 50; it++) {
            fill(a, n, k); memcpy(b, a, sizeof *a * (size_t)n);
            double t0 = now(); qsort(b, (size_t)n, sizeof *b, cmp_lo); double t1 = now(); ucap_rng_sort(a, n); double t2 = now();
            tq += t1 - t0; tr += t2 - t1;
        }
        printf("%s, %d ranges: qsort %.3f ms, radix %.3f ms\n", k == 0 ? "random" : k == 1 ? "repeats" : "in order", n, tq / 50, tr / 50);
    }
    return bad != 0;
}
