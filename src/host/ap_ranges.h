// [UIT 0257] Exact host code ranges of the lifted functions for asynchronous preemption (src/host/preempt_win.c).
// uit_ap_redirect may move a thread's rip only while it runs lifted game code. The old check took one range, from the
// first to the last lifted function start, and refused if a runtime function lay inside it; with ThinLTO (setup's
// default) lld places the native objects first, and tools/build_game.py compiles the huge -O0 game files without
// -flto, so they come before the runtime and the other game files after it: the runtime always lay inside and the
// mode never started. This keeps one [begin, end) per lifted function instead (Windows: from the exe's unwind table),
// sorted and merged, and a rip counts as game code only inside one of them. Pure C, tested by tools/tests/test_ap_ranges.c.
#ifndef UIT_AP_RANGES_H
#define UIT_AP_RANGES_H
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

typedef struct { uintptr_t b, e; } ap_range;   // [b, e)

static inline int apr_cmp(const void *x, const void *y) {
    const ap_range *a = (const ap_range *)x, *c = (const ap_range *)y;
    return a->b < c->b ? -1 : a->b > c->b ? 1 : (a->e < c->e ? -1 : a->e > c->e);
}

// Sorts r[0..n), drops empty ranges, merges overlapping and touching ones. Returns the new count.
static inline size_t apr_build(ap_range *r, size_t n) {
    size_t k = 0;
    for (size_t i = 0; i < n; i++) if (r[i].e > r[i].b) r[k++] = r[i];
    if (!k) return 0;
    qsort(r, k, sizeof *r, apr_cmp);
    size_t m = 0;
    for (size_t i = 1; i < k; i++) {
        if (r[i].b <= r[m].e) { if (r[i].e > r[m].e) r[m].e = r[i].e; }
        else r[++m] = r[i];
    }
    return m + 1;
}

// 1 if x lies in one of the n sorted, disjoint ranges.
static inline int apr_find(const ap_range *r, size_t n, uintptr_t x) {
    size_t lo = 0, hi = n;
    while (lo < hi) {   // first range with e > x
        size_t mid = lo + (hi - lo) / 2;
        if (r[mid].e <= x) lo = mid + 1; else hi = mid;
    }
    return lo < n && r[lo].b <= x;
}

// Index of the first of host[0..nh) inside the ranges, or -1 if none is.
static inline int apr_first_inside(const ap_range *r, size_t n, const uintptr_t *host, int nh) {
    for (int i = 0; i < nh; i++) if (apr_find(r, n, host[i])) return i;
    return -1;
}
#endif
