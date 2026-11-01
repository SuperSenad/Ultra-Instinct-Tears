// [UIT 0104] The free list of one device memory block (src/gfx/vkb_mem.c's own suballocator, which replaced AMD's
// VMA): the free ranges of the block, sorted by offset and never touching (a freed range merges with its neighbours).
// Pure C on plain integers, so the box test (tools/tests/test_suballoc.c) runs it against a model.
//   vkb_fl_init   a block of `size` bytes, all free
//   vkb_fl_alloc  best fit: the smallest free range that holds `size` bytes at an offset aligned to `align` (a power
//                 of two); the bytes before the aligned offset stay free. 1 and *off, or 0 (no range fits)
//   vkb_fl_free   gives [off, off + size) back
//   vkb_fl_empty  nothing allocated
// Callers keep offsets and sizes multiples of the bufferImageGranularity, so no two allocations share a page.
#ifndef VKB_SUBALLOC_H
#define VKB_SUBALLOC_H
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct { uint64_t off, size; } vkb_range;
typedef struct {
    vkb_range *r;
    uint32_t n, cap;
    uint64_t size, used;   // the block's size; bytes allocated
    uint32_t live;         // allocations in the block
} vkb_freelist;

static inline int vkb_fl_reserve(vkb_freelist *f, uint32_t n) {
    if (n <= f->cap) return 1;
    uint32_t cap = f->cap ? f->cap * 2 : 16;
    while (cap < n) cap *= 2;
    vkb_range *r = (vkb_range *)realloc(f->r, (size_t)cap * sizeof *r);
    if (!r) return 0;
    f->r = r; f->cap = cap;
    return 1;
}
static inline int vkb_fl_init(vkb_freelist *f, uint64_t size) {
    memset(f, 0, sizeof *f);
    if (!vkb_fl_reserve(f, 16)) return 0;
    f->size = size; f->n = 1;
    f->r[0].off = 0; f->r[0].size = size;
    return 1;
}
static inline void vkb_fl_destroy(vkb_freelist *f) { free(f->r); memset(f, 0, sizeof *f); }
static inline int vkb_fl_empty(const vkb_freelist *f) { return f->live == 0; }

static inline int vkb_fl_alloc(vkb_freelist *f, uint64_t size, uint64_t align, uint64_t *off) {
    if (!size || (align & (align - 1))) return 0;
    if (!align) align = 1;
    uint32_t best = UINT32_MAX;
    uint64_t best_waste = UINT64_MAX, best_at = 0;
    for (uint32_t i = 0; i < f->n; i++) {
        uint64_t o = f->r[i].off, end = o + f->r[i].size, at = (o + align - 1) & ~(align - 1);
        if (at < o || at > end || end - at < size) continue;
        uint64_t waste = f->r[i].size - size;
        if (waste < best_waste) { best = i; best_waste = waste; best_at = at; if (!waste) break; }
    }
    if (best == UINT32_MAX) return 0;
    vkb_range r = f->r[best];
    uint64_t head = best_at - r.off, tail = r.off + r.size - (best_at + size);
    if (head && tail) {   // split in two: one more range
        if (!vkb_fl_reserve(f, f->n + 1)) return 0;
        memmove(&f->r[best + 2], &f->r[best + 1], (size_t)(f->n - best - 1) * sizeof *f->r);
        f->n++;
        f->r[best].size = head;
        f->r[best + 1].off = best_at + size; f->r[best + 1].size = tail;
    } else if (head) {
        f->r[best].size = head;
    } else if (tail) {
        f->r[best].off = best_at + size; f->r[best].size = tail;
    } else {
        memmove(&f->r[best], &f->r[best + 1], (size_t)(f->n - best - 1) * sizeof *f->r);
        f->n--;
    }
    f->used += size; f->live++;
    *off = best_at;
    return 1;
}

static inline int vkb_fl_free(vkb_freelist *f, uint64_t off, uint64_t size) {
    uint32_t lo = 0, hi = f->n;   // the first range after off
    while (lo < hi) { uint32_t m = (lo + hi) / 2; if (f->r[m].off < off) lo = m + 1; else hi = m; }
    int prev = lo > 0 && f->r[lo - 1].off + f->r[lo - 1].size == off;
    int next = lo < f->n && off + size == f->r[lo].off;
    if (prev && next) {
        f->r[lo - 1].size += size + f->r[lo].size;
        memmove(&f->r[lo], &f->r[lo + 1], (size_t)(f->n - lo - 1) * sizeof *f->r);
        f->n--;
    } else if (prev) {
        f->r[lo - 1].size += size;
    } else if (next) {
        f->r[lo].off = off; f->r[lo].size += size;
    } else {
        if (!vkb_fl_reserve(f, f->n + 1)) return 0;   // (the range stays lost; the block is then never empty)
        memmove(&f->r[lo + 1], &f->r[lo], (size_t)(f->n - lo) * sizeof *f->r);
        f->r[lo].off = off; f->r[lo].size = size;
        f->n++;
    }
    f->used -= size; f->live--;
    return 1;
}
#endif
