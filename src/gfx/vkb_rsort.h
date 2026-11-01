// [UIT 0128] Sorting the ranges a submission's uniform capture (vkb_ucap.c, game thread, in the submit call) collects:
// one entry per uniform / storage buffer binding and buffer copy of the submission, ~1-2 MiB a frame in busy scenes
// (logs, 2026-10-30: 1.4-2.4 ms of the game thread per frame for the whole capture). It was qsort with a comparator
// called through a pointer for every comparison; this is a radix sort (11 bits a pass, only the bits that differ),
// O(n) and with no calls. The order: by lo, then the longest first, ties in input order - one of the orders qsort's
// comparator (lo only) already allowed, and the best one for the merge that follows: of ranges starting at one address
// the longest comes first and holds the others (before, a shorter one first could leave the longer one a second,
// overlapping entry when it could not be extended, and bindings past the shorter one's end read live memory).
// Pure logic: tools/tests/test_rsort.c (the same order as a stable comparison sort, random and adversarial inputs).
#ifndef VKB_RSORT_H
#define VKB_RSORT_H
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct { uint64_t lo, hi; uint64_t geo; } ucap_rng;   // geo: holds vertex / index data (vkb_ucap.c)

enum { RS_BITS = 11, RS_SMALL = 96 };

static inline int rs_less(const ucap_rng *a, const ucap_rng *b) { return a->lo < b->lo || (a->lo == b->lo && a->hi > b->hi); }

// One stable radix sort of v[0..n) by a key (field 0: lo ascending, 1: hi descending) into the sorted order; tmp: n entries.
// Returns the array holding the result (v or tmp).
static inline ucap_rng *rs_by(ucap_rng *v, ucap_rng *tmp, int n, int field) {
    uint64_t mn = UINT64_MAX, mx = 0;
    for (int i = 0; i < n; i++) { uint64_t k = field ? ~v[i].hi : v[i].lo; if (k < mn) mn = k; if (k > mx) mx = k; }
    uint64_t span = mx - mn;
    int bits = 0;
    while (bits < 64 && (span >> bits)) bits++;
    static _Thread_local uint32_t cnt[1u << RS_BITS];
    ucap_rng *src = v, *dst = tmp;
    for (int sh = 0; sh < bits; sh += RS_BITS) {
        memset(cnt, 0, sizeof cnt);
        for (int i = 0; i < n; i++) cnt[(((field ? ~src[i].hi : src[i].lo) - mn) >> sh) & ((1u << RS_BITS) - 1)]++;
        uint32_t s = 0;
        for (uint32_t d = 0; d < (1u << RS_BITS); d++) { uint32_t c = cnt[d]; cnt[d] = s; s += c; }
        for (int i = 0; i < n; i++) dst[cnt[(((field ? ~src[i].hi : src[i].lo) - mn) >> sh) & ((1u << RS_BITS) - 1)]++] = src[i];
        ucap_rng *t = src; src = dst; dst = t;
    }
    return src;
}

// [UIT 0144] ucap_rng_sort_with: the same with the caller's scratch (tmp: room for n entries), so no allocation.
static inline void ucap_rng_sort_with(ucap_rng *v, ucap_rng *tmp, int n) {
    ucap_rng *r = rs_by(v, tmp, n, 1);           // by hi (descending), then (stable) by lo
    ucap_rng *o = r == v ? tmp : v;
    r = rs_by(r, o, n, 0);
    if (r != v) memcpy(v, r, sizeof *v * (size_t)n);
}
// [UIT 0244] TOTK_VK_UCAP_FASTSORT (vkb_ucap.c): the same order with half the passes. ucap_rng_sort_with sorts twice
// (by hi, then stably by lo: ~8 passes of 11 bits over guest addresses); here one radix sort by lo (stable: ties keep
// the input order), then each run of equal lo is put in hi-descending order by a stable insertion sort. Runs are short
// (the bindings of one buffer start at one address a few times a frame); a run longer than RS_TIE_MAX goes back to the
// two-key sort over the whole array (v is then sorted by lo with ties in input order, so the stable two-key sort of it
// gives exactly the order it gives the input). Box: 10000 ranges 176 -> 108 us, 25000 ranges 437 -> 319 us.
// tools/tests/test_ucap_fastsort.c: the same array as ucap_rng_sort_with on random and adversarial inputs.
enum { RS_TIE_MAX = 64 };
#ifdef TOTK_PROBE_UNSTABLE_TIES   // the test's negative probe: an unstable tie sort must be caught
#define RS_TIE_MOVES(prev, x) ((prev).hi <= (x).hi)
#else
#define RS_TIE_MOVES(prev, x) ((prev).hi < (x).hi)
#endif
static inline void ucap_rng_sort1_with(ucap_rng *v, ucap_rng *tmp, int n) {
    if (n < 2) return;
    ucap_rng *r = rs_by(v, tmp, n, 0);
    if (r != v) memcpy(v, r, sizeof *v * (size_t)n);
    for (int i = 0; i < n;) {
        int j = i + 1;
        while (j < n && v[j].lo == v[i].lo) j++;
        if (j - i > RS_TIE_MAX) { ucap_rng_sort_with(v, tmp, n); return; }
        for (int a = i + 1; a < j; a++) {
            ucap_rng x = v[a];
            int b = a - 1;
            while (b >= i && RS_TIE_MOVES(v[b], x)) { v[b + 1] = v[b]; b--; }
            v[b + 1] = x;
        }
        i = j;
    }
}
static inline int ucap_rng_small(ucap_rng *v, int n) {   // insertion sort (stable) for n <= RS_SMALL: 1 if done
    if (n > RS_SMALL) return 0;
    for (int i = 1; i < n; i++) {
        ucap_rng x = v[i];
        int j = i - 1;
        while (j >= 0 && rs_less(&x, &v[j])) { v[j + 1] = v[j]; j--; }
        v[j + 1] = x;
    }
    return 1;
}
// Sorts v[0..n) by lo, then hi descending, stable. Returns 0 on success, -1 if the scratch could not be allocated (v untouched).
static inline int ucap_rng_sort(ucap_rng *v, int n) {
    if (n < 2) return 0;
    if (ucap_rng_small(v, n)) return 0;
    ucap_rng *tmp = malloc(sizeof *tmp * (size_t)n);
    if (!tmp) return -1;
    ucap_rng_sort_with(v, tmp, n);
    free(tmp);
    return 0;
}
#endif
