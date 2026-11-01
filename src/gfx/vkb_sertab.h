// [UIT 0233] The images table (texture serial -> image entry, vkb_tex.c) with a lookup that takes no lock.
//
// Why: moving, the recording threads waited 128.7 ms a frame (800 waits, ~160 us each) for the images lock, and the
// translation thread's largest chunk (25.9 ms, the frame's critical path) is inflated by it. Most acquisitions are a
// recording thread looking up an image that exists already (its per-thread cache is empty at each submission's start).
// A found serial needs no lock: this header lets readers probe the table while one writer (holding the lock) changes it.
//
// The table is exactly the one before (same hash, same linear probing, same growth at half full, removed slots kept
// for their serial until the next growth); the locked operations do the same things in the same order, the stores
// that readers can see are atomic. The lock-free read (vst_get_lockfree) returns either the entry a locked lookup would
// have returned at some instant during the call, or NULL = "don't know" (the caller takes the lock and looks up as
// before). Why it is exact:
//   * a slot's serial is written once per table (0 -> serial) and never changes there; its entry pointer is published
//     with a release store after the entry is filled in (the reader's acquire load sees it whole);
//   * a growth builds a new table privately and publishes it (release); the old one is not freed while readers can be
//     in it (vst_retire: kept until vst_quiesce, which the owner calls when no reader is running);
//   * a removal bumps `epoch` (seq_cst) BEFORE it clears the slot. A reader reads the epoch, the table pointer, the
//     slot, then the epoch again, and answers only when it did not change. A reader still in an old table (grown past
//     meanwhile) cannot return an entry removed since: either it read the epoch after that removal (then, by the
//     acquire on the epoch, it also saw the new table, which the removal came after), or the epoch changed during its
//     probe (it answers NULL). A slot cleared then filled again for the same serial: the epoch changed too.
// Standalone (tools/tests/test_sertab.c: randomized differential test against the locked table, and threads reading
// while one thread inserts, removes and grows).
#pragma once
#include <stdint.h>
#include <stdlib.h>
#ifndef VST_READ_HOOK
#define VST_READ_HOOK(t)   // the test runs writer steps here (a reader between loading the table and probing it)
#endif

typedef struct { uint64_t serial; void *e; } vst_slot;   // serial 0: free; e NULL: removed (the serial stays until growth)
typedef struct vst_tab { uint32_t cap; struct vst_tab *retired_next; vst_slot s[]; } vst_tab;
typedef struct {
    vst_tab *tab;          // published (atomic); NULL until the first growth
    uint32_t n, tomb;      // live serials, removed slots (writer only)
    uint64_t epoch;        // removals so far (atomic)
    vst_tab *retired;      // old tables not freed yet (writer only)
    int keep_old;          // 1: lock-free readers may run (old tables wait for vst_quiesce); 0: freed at once, as before
} vst;

static inline uint32_t vst_hash(uint64_t serial, uint32_t cap) { return (uint32_t)(serial * 0x9E3779B97F4A7C15ull >> 40) & (cap - 1); }
static inline uint32_t vst_cap(const vst *t) { return t->tab ? t->tab->cap : 0; }
// Writer (lock held): the slot of a serial, or the free slot it would go in. The table must exist.
static inline vst_slot *vst_slot_of(vst *t, uint64_t serial) {
    vst_tab *tb = t->tab;
    uint32_t m = tb->cap - 1, h = vst_hash(serial, tb->cap);
    while (tb->s[h].serial && tb->s[h].serial != serial) h = (h + 1) & m;
    return &tb->s[h];
}
static inline void *vst_entry(const vst_slot *s) { return s->e; }   // writer: plain read
// Writer: 0 = out of memory (the table is unchanged)
static inline int vst_grow(vst *t) {
    vst_tab *old = t->tab;
    uint32_t n = old ? old->cap : 0, cap = n ? n * 2 : 1024;
    vst_tab *nt = (vst_tab *)calloc(1, sizeof *nt + (size_t)cap * sizeof nt->s[0]);
    if (!nt) return 0;
    nt->cap = cap;
    for (uint32_t i = 0; i < n; i++)
        if (old->s[i].serial && old->s[i].e) {
            uint32_t h = vst_hash(old->s[i].serial, cap);
            while (nt->s[h].serial) h = (h + 1) & (cap - 1);   // serials are distinct: never equal here
            nt->s[h] = old->s[i];
        }
    t->tomb = 0;
    __atomic_store_n(&t->tab, nt, __ATOMIC_RELEASE);
    if (old) {
        if (t->keep_old) { old->retired_next = t->retired; t->retired = old; }
        else free(old);
    }
    return 1;
}
// Writer: grows when half full (counting removed slots), as before. 0 = out of memory.
static inline int vst_reserve(vst *t) { return (t->n + t->tomb + 1) * 2 >= vst_cap(t) ? vst_grow(t) : 1; }
// Writer: puts e at s (from vst_slot_of(serial), no live entry there)
static inline void vst_put(vst *t, vst_slot *s, uint64_t serial, void *e) {
    if (s->serial == serial) t->tomb--;
    else __atomic_store_n(&s->serial, serial, __ATOMIC_RELAXED);
    __atomic_store_n(&s->e, e, __ATOMIC_RELEASE);
    t->n++;
}
// Writer: removes the live entry at s
static inline void vst_remove(vst *t, vst_slot *s) {
    __atomic_add_fetch(&t->epoch, 1, __ATOMIC_SEQ_CST);
    __atomic_store_n(&s->e, (void *)0, __ATOMIC_SEQ_CST);
    t->n--; t->tomb++;
}
// Writer, when no reader can be running: frees the old tables. Returns how many.
static inline int vst_quiesce(vst *t) {
    int k = 0;
    for (vst_tab *r = t->retired, *nx; r; r = nx) { nx = r->retired_next; free(r); k++; }
    t->retired = NULL;
    return k;
}
// Any thread, no lock: the live entry of `serial`, or NULL = not known (look up with the lock).
static inline void *vst_get_lockfree(vst *t, uint64_t serial) {
    uint64_t ep0 = __atomic_load_n(&t->epoch, __ATOMIC_SEQ_CST);
    vst_tab *tb = __atomic_load_n(&t->tab, __ATOMIC_ACQUIRE);
    if (!tb || !serial) return NULL;
    VST_READ_HOOK(t);
    uint32_t m = tb->cap - 1, h = vst_hash(serial, tb->cap);
    for (uint32_t i = 0; i <= m; i++, h = (h + 1) & m) {
        uint64_t s = __atomic_load_n(&tb->s[h].serial, __ATOMIC_ACQUIRE);
        if (!s) return NULL;
        if (s != serial) continue;
        void *e = __atomic_load_n(&tb->s[h].e, __ATOMIC_ACQUIRE);
        if (!e || __atomic_load_n(&t->epoch, __ATOMIC_SEQ_CST) != ep0) return NULL;
        return e;
    }
    return NULL;
}
