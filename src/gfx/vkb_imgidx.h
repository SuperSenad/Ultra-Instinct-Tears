// [UIT 0230] Compact indexes of the images (vkb_tex.c): the scans that looked at every image under the images lock
// look at a dense array instead.
//
// Why: the images lock (g_mu, "lock waits ... images" in the per-frame log line) was held for whole walks of the image
// list (g_all, every entry ever made and not destroyed yet: 2,500 standing in a busy area, 4,000 and more while
// running) by the copy-back paths (buffer reads over GPU-drawn memory, explicit reads, the end of each submission, game
// reads) and by every texture's first upload (which looks for GPU-drawn images over its memory). An entry is ~1.3 KB
// (the image, 32 inline views and their keys) and each one is its own allocation, so a walk misses the cache two or
// three times per entry: 0.3-0.7 ms with the lock held while the six recording threads queue for it. 2026-10-31 log
// (36fb2fc, 26,400 frames): the images lock wait per contended acquisition grew with the number of images alive
// (62 us at 2,459 images standing, 160 us at 4,002 running) and frame time followed the lock wait (r = 0.92 over the
// 44 busy 300-frame blocks), not the draw count (r = -0.19).
//
// What: two dense arrays kept under the same lock (the callers hold g_mu; nothing here locks):
//  - every entry with game memory: its range [lo, hi) (the texture's whole layout, as the scans compute it), so a scan
//    for images over a range compares 16-byte records in one array and touches only the entries that overlap;
//  - the pending entries (drawn, not copied back yet: tens, not thousands), for the copy-back paths.
// Each entry remembers its place in each array (swap-remove). A query returns the matches in the order the old walk
// met them (newest entry first: g_all is a push-front list and entries never move in it), so every caller sees the
// same entries in the same order as before; it then applies its own test unchanged. The range is only a prefilter
// (a superset of what overlaps: the same layout size the old tests used).
// TOTK_VK_IMAGE_INDEX=0 (Performance > CPU > Faster image bookkeeping: Off): the old walks (A/B).
// Box test: tools/tests/test_imgidx.c (random add / remove / pend / unpend / queries against a reference list walk,
// and the cost of both).
#pragma once
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct { uint64_t lo, hi; } vkb_ix_range;
typedef struct {
    vkb_ix_range *r;   // [n] ranges (the prefilter reads only these)
    void **obj;        // [n] the entries
    uint64_t *seq;     // [n] creation order (larger: newer, earlier in the old walk)
    int **where;       // [n] each entry's own copy of its index here (updated when it moves)
    int n, cap;
    struct vkb_ix_match *tmp; int tcap;   // query scratch
} vkb_ix;

static inline int vkb_ix_add(vkb_ix *x, void *obj, uint64_t seq, uint64_t lo, uint64_t hi, int *where) {
    if (x->n == x->cap) {
        int c = x->cap ? x->cap * 2 : 256;
        vkb_ix_range *r = (vkb_ix_range *)realloc(x->r, sizeof *r * (size_t)c);
        if (!r) return 0;
        x->r = r;
        void **o = (void **)realloc(x->obj, sizeof *o * (size_t)c);
        if (!o) return 0;
        x->obj = o;
        uint64_t *s = (uint64_t *)realloc(x->seq, sizeof *s * (size_t)c);
        if (!s) return 0;
        x->seq = s;
        int **w = (int **)realloc(x->where, sizeof *w * (size_t)c);
        if (!w) return 0;
        x->where = w;
        x->cap = c;
    }
    int i = x->n++;
    x->r[i] = (vkb_ix_range){lo, hi};
    x->obj[i] = obj; x->seq[i] = seq; x->where[i] = where;
    *where = i + 1;   // 0: not in this index
    return 1;
}
static inline void vkb_ix_del(vkb_ix *x, int *where) {
    int i = *where - 1;
    if (i < 0 || i >= x->n) return;
    int last = --x->n;
    if (i != last) {
        x->r[i] = x->r[last]; x->obj[i] = x->obj[last]; x->seq[i] = x->seq[last]; x->where[i] = x->where[last];
        *x->where[i] = i + 1;
    }
    *where = 0;
}
typedef struct vkb_ix_match { uint64_t seq; void *obj; } vkb_ix_match;
static int vkb_ix_newer(const void *a, const void *b) {
    uint64_t x = ((const vkb_ix_match *)a)->seq, y = ((const vkb_ix_match *)b)->seq;
    return x < y ? 1 : x > y ? -1 : 0;
}
// Entries whose range overlaps [lo, hi) (hi <= lo: every entry), newest first (the old walk's order), into *out (grown
// as needed; the caller keeps it for the next query and frees it). Returns how many; -1 when out of memory (the caller
// then walks the old way). Uses x's scratch: callers hold the same lock as for add / del.
static inline int vkb_ix_query(vkb_ix *x, uint64_t lo, uint64_t hi, void ***out, int *cap) {
    int m = 0, all = hi <= lo;
    for (int i = 0; i < x->n; i++) {
        if (!all && !(x->r[i].lo < hi && lo < x->r[i].hi)) continue;
        if (m == x->tcap) {
            int c = x->tcap ? x->tcap * 2 : 64;
            vkb_ix_match *t = (vkb_ix_match *)realloc(x->tmp, sizeof *t * (size_t)c);
            if (!t) return -1;
            x->tmp = t; x->tcap = c;
        }
        x->tmp[m].seq = x->seq[i]; x->tmp[m].obj = x->obj[i]; m++;
    }
    if (m > *cap) {
        void **o = (void **)realloc(*out, sizeof *o * (size_t)m);
        if (!o) return -1;
        *out = o; *cap = m;
    }
    if (m <= 24) {   // insertion sort for the usual few
        for (int i = 1; i < m; i++)
            for (int k = i; k > 0 && x->tmp[k - 1].seq < x->tmp[k].seq; k--) { vkb_ix_match t = x->tmp[k]; x->tmp[k] = x->tmp[k - 1]; x->tmp[k - 1] = t; }
    } else qsort(x->tmp, (size_t)m, sizeof x->tmp[0], vkb_ix_newer);
    for (int i = 0; i < m; i++) (*out)[i] = x->tmp[i].obj;
    return m;
}
