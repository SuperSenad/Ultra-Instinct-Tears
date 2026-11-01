// [UIT 0244] TOTK_VK_UCAP_FASTSORT: the vertex / index ranges of a submission's uniform capture (vkb_ucap.c, game thread,
// in the submit call) without sorting all of them. Before, every range a draw bound (~10000 a frame at Lookout Landing,
// log t254: "vertex / index ranges copied at submit 336.2 of 10132.4") was radix-sorted only to drop exact duplicates,
// then checked against the write watch; ~3% are copied. Now the duplicates go through a hash set (one pass, first
// occurrences in input order), each distinct range is checked, and only the ranges to copy are sorted (the order the
// 32 MiB cut-off and the merge used before). The same ranges in the same order: tools/tests/test_ucap_fastsort.c.
#ifndef VKB_GEODEDUP_H
#define VKB_GEODEDUP_H
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "vkb_rsort.h"

typedef struct { uint32_t *slot; uint32_t cap; } ucap_dset;   // slot: index + 1 of the kept entry, 0 empty; cap a power of 2

static inline uint32_t ucap_dhash(uint64_t lo, uint64_t hi, uint32_t mask) {
    uint64_t x = (lo * 0x9E3779B97F4A7C15ull) ^ (hi + 0x632BE59BD9B4E019ull);
    x ^= x >> 29; x *= 0xBF58476D1CE4E5B9ull; x ^= x >> 32;
    return (uint32_t)x & mask;
}
// Keeps the first of each (lo, hi) in v[0..n), in input order; returns the new count, -1 if the table could not be
// allocated (v untouched). The table (s) is kept for the next call.
static inline int ucap_dedup(ucap_rng *v, int n, ucap_dset *s) {
    if (n < 2) return n;
    uint32_t need = 64;
    while (need < (uint32_t)n * 2u) need <<= 1;
    if (s->cap < need) {
        uint32_t *t = realloc(s->slot, sizeof *t * need);
        if (!t) return -1;
        s->slot = t; s->cap = need;
    }
    uint32_t cap = need, mask = cap - 1;   // only the part sized for n: less to clear
    memset(s->slot, 0, sizeof *s->slot * cap);
    int m = 0;
    for (int i = 0; i < n; i++) {
        uint64_t lo = v[i].lo, hi = v[i].hi;
        uint32_t h = ucap_dhash(lo, hi, mask);
        for (;;) {
            uint32_t k = s->slot[h];
            if (!k) { v[m] = v[i]; s->slot[h] = (uint32_t)++m; break; }
            if (v[k - 1].lo == lo && v[k - 1].hi == hi) break;   // a duplicate: dropped
            h = (h + 1) & mask;
        }
    }
    return m;
}
#endif
