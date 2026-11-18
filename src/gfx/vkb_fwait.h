// [UIT 0441] / [UIT 0442] What the game's fence waits wait on (backend_vk.c, hle_nvn.c nvnSyncWait). The record of each
// submission and the rules, without Vulkan or locks, so that tools/tests/test_fwait.c can check them on the box.
//
// Per submission (its fence f), kept in a ring of FW_RING:
//   - [UIT 0441] when it was handed to the GPU (vkQueueSubmit2), when the retire thread saw the GPU finish it, and when
//     it was marked complete (after the copy back of GPU results into game memory and the event and counter writes):
//     a long wait splits into translation thread / queue submit / GPU / copy back / wake-up (fw_split);
//   - [UIT 0442] FW_GM: the GPU reads or writes game memory while it runs (pool copies taken from game memory, draws
//     and copies on memory with no device copy, mirrors written back). Once its wait returns the game may write that
//     memory again: to the game, a signalled fence means "the GPU is done with it";
//   - FW_RB: it has GPU results the game may read once its wait returns: buffer copy backs, image copy backs (also the
//     UIT 0438 fence read-backs), event values and counter reports, the shadow check's copies. They reach game memory
//     on the retire thread before the submission is complete (UIT 0439's held ones later, at a present: unchanged).
// TOTK_VK_FENCE_EARLY (fw_target): a wait for f has to reach the newest submission at or before f with either mark,
// and the last one of the frame before f's (so the game is never more than one frame ahead of the GPU). The ones
// after that touch nothing the game can see until they are complete, so it cannot tell that they still run; with
// none of them marked the wait returns once f is translated and submitted. A submission with no record (not
// committed yet, or the ring passed it) counts as marked, as do more than FW_SCAN submissions to look at: the wait
// is then the full one.
#pragma once
#include <stdatomic.h>
#include <stdint.h>

enum { FW_RING = 4096, FW_SCAN = 64, FW_GM = 1, FW_RB = 2 };
enum { FW_T_SUB, FW_T_GPU, FW_T_DONE, FW_T_N };
// fw_target's *why: what the target submission had (FW_WHY_EARLY: nothing, the target is the frame before or what is
// complete; FW_WHY_FRAME: no frame before f known, the full wait)
enum { FW_WHY_EARLY, FW_WHY_GM, FW_WHY_RB, FW_WHY_UNKNOWN, FW_WHY_FRAME, FW_WHY_N };

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
// The fence a wait for f has to reach. done: complete so far; prev: the last fence of the frame before (the newest
// present's fence below f; 0 = none known: the full wait). Never above f, never below what it must cover.
static inline uint64_t fw_target(fw_ring *r, uint64_t f, uint64_t done, uint64_t prev, int *why) {
    *why = FW_WHY_EARLY;
    if (f <= done) return f;
    if (!prev || prev >= f) { *why = FW_WHY_FRAME; return f; }
    uint64_t lo = prev > done ? prev : done;   // everything up to here is waited for anyway
    if (f - lo > FW_SCAN) { *why = FW_WHY_UNKNOWN; return f; }
    for (uint64_t s = f; s > lo; s--) {
        uint64_t fl;
        if (!fw_flags(r, s, &fl)) { *why = FW_WHY_UNKNOWN; return s; }
        if (fl & FW_GM) { *why = FW_WHY_GM; return s; }
        if (fl & FW_RB) { *why = FW_WHY_RB; return s; }
    }
    return lo;
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
