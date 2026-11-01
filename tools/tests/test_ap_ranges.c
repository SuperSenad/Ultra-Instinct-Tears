// [UIT 0257] Tests src/host/ap_ranges.h (the exact lifted-code ranges of asynchronous preemption, TOTK_AP_EXACT_RANGES).
// The layout cases are the ones a ThinLTO link gives (tools/tests/check_windows_toolchain.py shows it on a real link):
// native -O0 game objects first, then the runtime, then the other game files. Brute-force cross-check on random sets.
// TOTK_PROBE_NEGATIVE=1: use the old single range instead (must fail the layout case); =2: drop the merge (must fail).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../src/host/ap_ranges.h"

static int fails, checks, probe;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; if (fails < 20) { printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } } while (0)

static size_t build(ap_range *r, size_t n) {
    if (probe == 1) {   // the old check: one range from the first to the last function start
        uintptr_t lo = UINTPTR_MAX, hi = 0;
        for (size_t i = 0; i < n; i++) { if (r[i].b < lo) lo = r[i].b; if (r[i].b > hi) hi = r[i].b; }
        if (!n) return 0;
        r[0].b = lo; r[0].e = hi + 1;
        return 1;
    }
    if (probe == 2) { qsort(r, n, sizeof *r, apr_cmp); return n; }   // sorted, not merged
    return apr_build(r, n);
}

static int brute(const ap_range *orig, size_t n, uintptr_t x) {
    for (size_t i = 0; i < n; i++) if (orig[i].b <= x && x < orig[i].e) return 1;
    return 0;
}

int main(void) {
    const char *e = getenv("TOTK_PROBE_NEGATIVE");
    probe = e ? atoi(e) : 0;
    // 1. The ThinLTO layout of the toolchain check's link (RVAs from build/check-link.map): the -O0 stand-in at 0x1470
    //    (its unwind entry ends at 0x106075), the runtime at 0x6cc000-0x6dc000, the LTO game functions at 0x7d2cf0+.
    ap_range r[8] = {{0x1470, 0x106075}, {0x7d2cf0, 0x7d2cf1}, {0x7d2d00, 0x7d2d01}, {0x7d2d10, 0x7d2d11}};
    size_t n = build(r, 4);
    const uintptr_t host[] = {0x6db840, 0x6cc8d0, 0x6d3870, 0x6d75b0, 0x6cc980, 0x6d77c0, 0x6d7790};
    CHECK(apr_first_inside(r, n, host, 7) < 0, "a runtime function counts as lifted code (%d)", apr_first_inside(r, n, host, 7));
    CHECK(apr_find(r, n, 0x1470) && apr_find(r, n, 0x106074) && !apr_find(r, n, 0x106075), "the -O0 function's range");
    CHECK(apr_find(r, n, 0x7d2d00) && !apr_find(r, n, 0x7d2d01) && !apr_find(r, n, 0x146f), "the LTO functions' ranges");
    // 2. A host function inside a lifted function's range is found (ICF folding, or a bad table): the mode refuses.
    ap_range r2[3] = {{0x1000, 0x2000}, {0x3000, 0x4000}};
    n = build(r2, 2);
    const uintptr_t h2[] = {0x2800, 0x3fff};
    CHECK(apr_first_inside(r2, n, h2, 2) == 1, "host code inside a range not found (%d)", apr_first_inside(r2, n, h2, 2));
    // 3. Edges: empty ranges are dropped, touching and overlapping ones merge, nothing at all gives 0.
    ap_range r3[5] = {{0x500, 0x500}, {0x200, 0x300}, {0x300, 0x380}, {0x100, 0x250}, {0x900, 0x901}};
    n = build(r3, 5);
    CHECK(n == 2 && r3[0].b == 0x100 && r3[0].e == 0x380 && r3[1].b == 0x900, "merge: %zu ranges, first %#lx..%#lx", n,
          (unsigned long)r3[0].b, (unsigned long)r3[0].e);
    CHECK(!apr_find(r3, 0, 0x100), "no ranges must hold nothing");
    // 4. Random sets against brute force.
    srand(257);
    static ap_range orig[4000], w[4000];
    for (int round = 0; round < 300; round++) {
        size_t m = 1 + (size_t)rand() % 4000;
        for (size_t i = 0; i < m; i++) {
            uintptr_t b = (uintptr_t)(rand() % 200000) * 16;
            orig[i].b = b; orig[i].e = b + (uintptr_t)(rand() % 600);
        }
        memcpy(w, orig, m * sizeof *w);
        size_t k = build(w, m);
        for (size_t i = 1; i < k && probe != 1; i++) CHECK(w[i - 1].e < w[i].b, "ranges not disjoint and sorted at %zu", i);
        for (int q = 0; q < 2000; q++) {
            uintptr_t x = (uintptr_t)(rand() % 3300000);
            CHECK(apr_find(w, k, x) == brute(orig, m, x), "round %d: %#lx", round, (unsigned long)x);
        }
    }
    printf("test_ap_ranges: %d checks, %d failed%s\n", checks, fails, probe ? " (negative probe)" : "");
    return fails != 0;
}
