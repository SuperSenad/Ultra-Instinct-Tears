// [UIT 0441] / [UIT 0449] What the game's fence waits wait on (backend_vk.c, hle_nvn.c nvnSyncWait). The record of each
// submission and the rules, without Vulkan or locks, so that tools/tests/test_fwait.c can check them on the box.
//
// Per submission (its fence f), kept in a ring of FW_RING:
//   - [UIT 0441] when it was handed to the GPU (vkQueueSubmit2), when the retire thread saw the GPU finish it, and when
//     it was marked complete (after the copy back of GPU results into game memory and the event and counter writes):
//     a long wait splits into translation thread / queue submit / GPU / copy back / wake-up (fw_split);
//   - [UIT 0449] what of it the game can see before it is complete (FW_*, below).
// TOTK_VK_FENCE_EARLY (UIT 0449; it replaces UIT 0442's rules, which never let a wait end early: every submission's
// pool copies read game memory on the GPU). To the game a signalled fence means two things: the GPU is done with the
// game memory that work used (the game may write it again), and the GPU's results are in game memory (the game may
// read them). With the switch on:
//   - the pool copies are taken from game memory on the CPU when they are recorded (a snapshot in upload memory) and
//     the GPU copies the snapshot: what the GPU reads is no longer game memory. What the GPU still reads or writes in
//     game memory itself marks the submission FW_GM (pool copies too large to snapshot, memory without a device copy,
//     mirrors written back);
//   - the image copy backs (also the UIT 0438 fence read-backs) are recorded right after the last chunk that draws
//     those images instead of at the end, and a second timeline semaphore is signalled after them (FW_VIS): the wait
//     waits for that, lays them into game memory itself and returns. When it cannot tell which chunk drew an image
//     last (drawn outside the chunks), they stay at the end (FW_IWB): the full wait;
//   - event values and counter reports (no GPU results in them: TotK's counters are written from the CPU clock, the
//     samples-passed word is fixed unless TOTK_VK_OCCLUSION=gpu) are written by the wait itself; GPU occlusion
//     results (FW_OQ) and the shadow check (FW_NVF) mean the full wait;
//   - buffer copy backs go with the image copy backs when every GPU write to a pool's copy was in a chunk at or before
//     that place (FW_WBV): the wait merges them into game memory as the retire thread would (UIT 0439 holds the
//     GPU-only ones as before). Left at the end (FW_WB), they are fine only when every range is GPU-only (the CPU
//     has not written it since) and UIT 0439 will hold it: the game then sees the same, older result it sees today;
//     else the full wait.
// The wait for f first waits for f - 1 in full (the game is never more than one submission ahead of the GPU) and
// only then applies the rules to f; anything it cannot tell (no record) is the full wait.
#pragma once
#include <stdatomic.h>
#include <stdint.h>

enum { FW_RING = 4096, FW_SCAN = 64 };
// [UIT 0449] the record's flags (fw_commit)
enum { FW_GM = 1, FW_IWB = 2, FW_VIS = 4, FW_WB = 8, FW_OQ = 16, FW_NVF = 32, FW_POSTS = 64, FW_WBV = 128 };
enum { FW_T_SUB, FW_T_GPU, FW_T_DONE, FW_T_N };

typedef struct { _Atomic uint64_t f, flags, t[FW_T_N]; } fw_ent;
typedef struct { fw_ent e[FW_RING]; _Atomic uint64_t gm[FW_RING]; } fw_ring;

// A GPU access to game memory being recorded for submission sub (the fence it will get: the last one + 1).
static inline void fw_touch(fw_ring *r, uint64_t sub) {
    _Atomic uint64_t *g = &r->gm[sub % FW_RING];
    if (atomic_load_explicit(g, memory_order_relaxed) != sub) atomic_store_explicit(g, sub, memory_order_release);
}
// Recorded from submission number `from` to `to` (both inclusive): any touch? Too long a span counts as touched.
static inline int fw_touched(fw_ring *r, uint64_t from, uint64_t to) {
    if (to < from) return 0;
    if (to - from >= FW_SCAN) return 1;
    for (uint64_t s = from; s <= to; s++) if (atomic_load_explicit(&r->gm[s % FW_RING], memory_order_acquire) == s) return 1;
    return 0;
}
// Submission f committed with its marks: the record, the times cleared, then f (readers check f last).
static inline void fw_commit(fw_ring *r, uint64_t f, uint64_t flags) {
    fw_ent *e = &r->e[f % FW_RING];
    atomic_store_explicit(&e->f, 0, memory_order_relaxed);
    atomic_thread_fence(memory_order_release);
    for (int i = 0; i < FW_T_N; i++) atomic_store_explicit(&e->t[i], 0, memory_order_relaxed);
    atomic_store_explicit(&e->flags, flags, memory_order_relaxed);
    atomic_store_explicit(&e->f, f, memory_order_release);
}
static inline void fw_time(fw_ring *r, uint64_t f, int which, uint64_t t) {
    fw_ent *e = &r->e[f % FW_RING];
    if (atomic_load_explicit(&e->f, memory_order_acquire) == f) atomic_store_explicit(&e->t[which], t, memory_order_relaxed);
}
// The record of f: 1 and its flags; 0 none (not committed yet, or overwritten).
static inline int fw_flags(fw_ring *r, uint64_t f, uint64_t *flags) {
    fw_ent *e = &r->e[f % FW_RING];
    if (atomic_load_explicit(&e->f, memory_order_acquire) != f) return 0;
    uint64_t v = atomic_load_explicit(&e->flags, memory_order_relaxed);
    atomic_thread_fence(memory_order_acquire);
    if (atomic_load_explicit(&e->f, memory_order_relaxed) != f) return 0;
    *flags = v;
    return 1;
}
// Its times (0 = not reached or no record); 1 when the record is f's.
static inline int fw_times(fw_ring *r, uint64_t f, uint64_t t[FW_T_N]) {
    fw_ent *e = &r->e[f % FW_RING];
    if (atomic_load_explicit(&e->f, memory_order_acquire) != f) return 0;
    for (int i = 0; i < FW_T_N; i++) t[i] = atomic_load_explicit(&e->t[i], memory_order_relaxed);
    atomic_thread_fence(memory_order_acquire);
    return atomic_load_explicit(&e->f, memory_order_relaxed) == f;
}
// [UIT 0441] A wait that blocked from t0 to t1, for submission f (times t: fw_times) whose translation ended at tx
// (t0 when there is no translation thread or it was done): seg[0] translation thread, [1] queue submit, [2] GPU,
// [3] copy back (GPU done -> complete), [4] wake-up and the rest. "GPU done" is when the retire thread saw it: it takes
// the submissions in order, so time it spent copying back the one before shows as GPU time. Each mark is clamped into
// [the one before, t1], so the parts add up to t1 - t0; a missing time (0) counts as reached at the mark before it.
enum { FW_SEG_X, FW_SEG_SUB, FW_SEG_GPU, FW_SEG_CB, FW_SEG_WAKE, FW_SEG_N };
static inline void fw_split(uint64_t t0, uint64_t t1, uint64_t tx, const uint64_t t[FW_T_N], uint64_t seg[FW_SEG_N]) {
    uint64_t m[FW_SEG_N + 1];
    m[0] = t0;
    uint64_t in[FW_SEG_N - 1] = {tx, t[FW_T_SUB], t[FW_T_GPU], t[FW_T_DONE]};
    if (t1 < t0) t1 = t0;
    for (int i = 0; i < FW_SEG_N - 1; i++) {   // m[i] <= t1 throughout
        uint64_t v = in[i] > m[i] ? in[i] : m[i];
        m[i + 1] = v < t1 ? v : t1;
    }
    m[FW_SEG_N] = t1;
    for (int i = 0; i < FW_SEG_N; i++) seg[i] = m[i + 1] - m[i];
}
// [UIT 0441] the histogram of blocked waits: 0-1, 1-2, 2-4, 4-8, over 8 ms
enum { FW_HIST_N = 5 };
static inline int fw_bucket(uint64_t ns) { return ns < 1000000 ? 0 : ns < 2000000 ? 1 : ns < 4000000 ? 2 : ns < 8000000 ? 3 : 4; }

// ---- [UIT 0449] TOTK_VK_FENCE_EARLY's rules ----
// fe_rule: what a wait for f may do once f - 1 is complete, from f's record (have: there is one). FE_NOW: return at
// once (after writing its events and counters); FE_VIS: wait for its "image copy backs done" signal, lay them into
// game memory, then return; the others: the full wait, and why (the log line counts them).
enum { FE_NOW, FE_VIS, FE_B_NOREC, FE_B_GM, FE_B_IWB, FE_B_OQ, FE_B_NVF, FE_B_WB, FE_B_BUSY, FE_B_TIME, FE_N };
static inline int fe_rule(int have, uint64_t fl) {
    if (!have) return FE_B_NOREC;
    if ((fl & FW_WBV) && !(fl & FW_VIS)) return FE_B_NOREC;   // (cannot be: they would have no signal)
    if (fl & FW_GM) return FE_B_GM;
    if (fl & FW_NVF) return FE_B_NVF;
    if (fl & FW_OQ) return FE_B_OQ;
    if ((fl & FW_IWB) && !(fl & FW_VIS)) return FE_B_IWB;
    return (fl & FW_VIS) ? FE_VIS : FE_NOW;
}
// The buffer copy backs of f, at the wait: 1 when it may still return early. Each range must be GPU-only now
// (gpu_only[i]) and fit in what UIT 0439 holds (held + the ranges <= max, so the retire thread holds them too), and no
// range may overlap an image copy back the wait lays down itself (overlap_iw: their order would change).
static inline int fe_wb_ok(int n, const int *gpu_only, const uint64_t *len, uint64_t held, uint64_t max, int hold_on, int overlap_iw) {
    if (!n) return 1;
    if (!hold_on || overlap_iw) return 0;
    uint64_t sum = held;
    for (int i = 0; i < n; i++) { if (!gpu_only[i]) return 0; sum += len[i]; }
    return sum <= max;
}
// Where an image was last written in submission sub, as one word (atomic max: a later submission, then a later chunk,
// wins; FE_POS_UNKNOWN, written outside the chunks, wins over every chunk): sub << 8 | chunk + 1.
enum { FE_POS_UNKNOWN = 255, FE_POS_MAXCH = 254 };
static inline uint64_t fe_pos_word(uint64_t sub, int ch) {
    return sub << 8 | (uint64_t)(ch < 0 || ch >= FE_POS_MAXCH ? FE_POS_UNKNOWN : ch + 1);
}
static inline void fe_pos_note(uint64_t *w, uint64_t sub, int ch) {   // (__atomic: a plain field of vkb_image)
    uint64_t v = fe_pos_word(sub, ch), o = __atomic_load_n(w, __ATOMIC_RELAXED);
    while (o < v && !__atomic_compare_exchange_n(w, &o, v, 1, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {}
}
// Where submission sub's image copy backs go, from the words of the images it drew (n of them): FE_SPLIT_END (-2) at
// the end as before (one was drawn outside the chunks, or its word is not this submission's); -1 in front of the
// chunks not sent yet (after the last prologue segment: all were drawn in chunks already on the GPU, early_next: the
// first chunk not sent, or none were drawn); else k: right after chunk k.
enum { FE_SPLIT_END = -2 };
static inline int fe_split_at(const uint64_t *w, int n, uint64_t sub, int early_next) {
    int k = -1;
    for (int i = 0; i < n; i++) {
        if (w[i] >> 8 != sub) return FE_SPLIT_END;
        int p = (int)(w[i] & 255);
        if (p == FE_POS_UNKNOWN || p == 0) return FE_SPLIT_END;
        if (p - 1 > k) k = p - 1;
    }
    return k < early_next ? -1 : k;
}
// The hand-over of f's late work (its image copy backs laid down, events and counters written) between the wait and
// the retire thread: done exactly once, by one of them. FE_ST_FREE: nobody yet; FE_ST_WAIT: the wait is doing it;
// FE_ST_EARLY: the wait did it; FE_ST_RETIRE: the retire thread does it.
enum { FE_ST_FREE, FE_ST_RETIRE, FE_ST_EARLY, FE_ST_WAIT };
static inline int fe_claim(_Atomic int *st) {   // the wait: 1 = it is its now
    int o = FE_ST_FREE;
    return atomic_compare_exchange_strong(st, &o, FE_ST_WAIT);
}
static inline void fe_release(_Atomic int *st, int did) {   // the wait: did it (FE_ST_EARLY) or gave it back
    atomic_store(st, did ? FE_ST_EARLY : FE_ST_FREE);
}
// The retire thread: 1 = the wait did it (skip it); 0 = the retire thread does it. While the wait is on it, this
// returns -1: call again (it is short: the retire thread gets here after the GPU, so the wait's GPU part is over).
static inline int fe_retire_take(_Atomic int *st) {
    int o = atomic_load(st);
    if (o == FE_ST_EARLY) return 1;
    if (o == FE_ST_WAIT) return -1;
    if (o == FE_ST_RETIRE) return 0;
    return atomic_compare_exchange_strong(st, &o, FE_ST_RETIRE) ? 0 : -1;
}
