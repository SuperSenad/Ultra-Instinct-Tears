// [UIT 0258] TOTK_VK_PRESYNC_BY_CHUNK (Performance > CPU+ > Bring the game's memory up to date chunk by chunk so the GPU
// starts sooner; [UIT 0312] default off, 1 = on). backend_vk.c vkb_mt_submit; box test tools/tests/test_presync_chunk.c.
//
// [UIT 0312] Off by default since the 2026-11-01 A/B at Lookout Landing (same build, same route, only this switch):
// On, the first work did not reach the GPU sooner (2.9 ms after the submission's start in both runs, at matched
// draw counts), while the walk's syncs, which take the pool table's write lock, made the recording threads wait
// 3.3 ms a frame for that lock (0.6 ms Off) and spend about 3 ms more CPU a frame; frames were about 0.75 ms longer
// and 39 % of them over 16.7 ms (26 % Off).
//
// Why: at Lookout Landing (2026-11-01 logs) the frame waits on the GPU ("the frame thread waits 4.4 ms a frame for the
// GPU after the translation thread is done"; GPU 11.96 ms of a 17.95 ms frame), and nothing reaches the GPU before a
// submission's prologue is over: 1.14-1.52 ms, most of it bringing ~440 pools up to date (the write-watch scans 0.96 ms
// on all recording threads, then the copies). Every chunk waited for all of them, though the first chunk uses only
// some.
//
// Now each pool remembers the first chunk that used it in the last submissions (a stamp: the submission and the
// chunk's index, the lowest one kept). The prologue brings up to date only the pools chunk 0 used and the ones with no
// stamp (as before, together: one pair of barriers, the scans on all threads); the others are brought up to date by
// the walk just before it hands out the chunk that used them first, the rest after the walk. With the stream on, the
// first parts may go to the GPU while the walk runs (vkb_early_stream_plan never sends the last cut chunk, so the last
// part stays the translation thread's).
//
// Why it is safe whatever the stamps say: every draw, dispatch and copy calls vkb_pool_use for the memory it reads or
// writes before it takes an address (vkb_gpu_addr), and a pool not current there is brought up to date on that thread
// with its copies in the open prologue segment, which goes to the GPU in front of every chunk recorded after it (the
// draw-path sync the stream already relies on). A wrong stamp costs a draw-path sync (counted: "later from draws"), not
// a stale read; a pool brought up to date by the walk after a chunk that used it was recorded is never possible,
// because that chunk's own lookup made it current first.
#ifndef VKB_PRESYNC_CHUNK_H
#define VKB_PRESYNC_CHUNK_H
#include <stdint.h>
#include <string.h>

enum { VKB_PBC_CHBITS = 7, VKB_PBC_NOCH = 127 };   // chunk indexes below 127 (MT_MAXCH is 96)

// [UIT 0312] on only when asked: 1 or on; unset, 0, off or anything else: off
static inline int vkb_pbc_parse(const char *e) { return e && (!strcmp(e, "1") || !strcmp(e, "on")); }

static inline uint64_t vkb_pbc_pack(uint64_t sub, int ch) {
    if (ch < 0) ch = 0;
    if (ch > VKB_PBC_NOCH - 1) ch = VKB_PBC_NOCH - 1;
    return sub << VKB_PBC_CHBITS | (uint64_t)ch;
}
// A use of the pool by chunk ch of submission sub: the stamp keeps that submission's lowest chunk (any thread, at once).
static inline void vkb_pbc_stamp(uint64_t *fc, uint64_t sub, int ch) {
    uint64_t want = vkb_pbc_pack(sub, ch), old = __atomic_load_n(fc, __ATOMIC_RELAXED);
    for (;;) {
        if (old >> VKB_PBC_CHBITS == sub && (old & VKB_PBC_NOCH) <= (want & VKB_PBC_NOCH)) return;   // as low already
        if (old >> VKB_PBC_CHBITS > sub) return;                                                    // a later one's
        if (__atomic_compare_exchange_n(fc, &old, want, 1, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) return;
    }
}
// The chunk before which submission sub brings the pool up to date: `lead` chunks before its stamp's chunk when the
// stamp is from one of the win submissions before; 0 (the prologue, as before) without one. The lead absorbs first
// uses that move a little between frames (each miss is a sync on a recording thread under the table's write lock).
#ifndef VKB_PBC_LEAD
#define VKB_PBC_LEAD 2
#endif
static inline int vkb_pbc_predict(uint64_t fc, uint64_t sub, uint64_t win, int lead) {
    uint64_t s = fc >> VKB_PBC_CHBITS;
    if (!fc || s >= sub || s + win < sub) return 0;
    int c = (int)(fc & VKB_PBC_NOCH) - lead;
    return c > 0 ? c : 0;
}

// A submission's plan: the pools to bring up to date in the order of the chunk they are wanted by (stable, so pools
// wanted by the same chunk keep the table's order, as the prologue's loop had them).
typedef struct { uintptr_t *addr; int *ch; int n, next, cap; } vkb_pbc_plan;
// Sorts the n entries (addr[i], ch[i]) by ch into p (tmp: room for n of each). Counting sort over 0..VKB_PBC_NOCH.
static inline void vkb_pbc_order(vkb_pbc_plan *p, const uintptr_t *addr, const int *ch, int n) {
    int cnt[VKB_PBC_NOCH + 2];
    memset(cnt, 0, sizeof cnt);
    for (int i = 0; i < n; i++) cnt[(ch[i] < 0 ? 0 : ch[i] > VKB_PBC_NOCH ? VKB_PBC_NOCH : ch[i]) + 1]++;
    for (int c = 1; c <= VKB_PBC_NOCH + 1; c++) cnt[c] += cnt[c - 1];
    for (int i = 0; i < n; i++) {
        int c = ch[i] < 0 ? 0 : ch[i] > VKB_PBC_NOCH ? VKB_PBC_NOCH : ch[i];
        int k = cnt[c]++;
        p->addr[k] = addr[i]; p->ch[k] = c;
    }
    p->n = n; p->next = 0;
}
// The entries wanted by chunks up to `upto` not taken yet: [*a, *b); 0 when none.
static inline int vkb_pbc_take(vkb_pbc_plan *p, int upto, int *a, int *b) {
    int i = p->next;
    while (p->next < p->n && p->ch[p->next] <= upto) p->next++;
    *a = i; *b = p->next;
    return *b > *a;
}
#endif
