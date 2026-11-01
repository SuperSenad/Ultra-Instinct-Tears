// [UIT 0232] TOTK_CRITPATH (default on; 0 = off): what a frame waits on, one log line every 300 frames
// ("[critical path]"). TOTK_PROFILE's [profile] lines sum a kind of wait over every thread, which is not the frame
// time (six threads waiting 20 ms each are 120 ms of "waits" in a 40 ms frame); these counters keep the frame thread
// (the game thread that acquires and presents: its time is the frame time) apart from the other game threads, and
// split each wait by what it waited for:
//   fence waits (nvnSyncWait)  -> until the translation thread had replayed the submission, then until the GPU was done
//   queue drains (x_drain)     -> by cause: pool deletes, ReadTexels / WriteTexels, program deletes, QueueFinish, ...
//   acquire                    -> frame-rate limit sleep, waiting for that swapchain image's last present
// and the translation thread's busy / idle time with its recording split (prologue, walk, waiting for the slowest
// recording thread, moves + submit; the largest chunk). Cost: a relaxed atomic add where a wait already happens (a
// clock read per wait) and a few per submission; nothing per draw.
#pragma once
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>

enum { CPT_FRAME, CPT_OTHER, CPT_N };   // the frame (acquire / present) thread, the other game threads
enum { CP_SYNC_X, CP_SYNC_GPU, CP_DRAIN, CP_SUBMIT, CP_ACQ_PACE, CP_ACQ_BEHIND, CP_K };
enum { DR_POOL, DR_READTEX, DR_WRITETEX, DR_PROGRAM, DR_FINISH, DR_SUSPEND, DR_GFX, DR_CALL, DR_N };
enum { CX_SUBMIT, CX_PRESENT, CX_IDLE, CX_REC_PRO, CX_REC_WALK, CX_REC_WAIT, CX_REC_COMMIT, CX_REC_BIG, CX_REC_BIGDRAWS, CX_SUBS, CX_N };
extern _Atomic uint64_t uit_cp_ns[CPT_N][CP_K], uit_cp_n[CPT_N][CP_K], uit_dr_ns[DR_N], uit_dr_n[DR_N], uit_cx[CX_N];
extern __thread int uit_cp_frame_thread;   // set at acquire / present
// The frame thread is the thread that acquired or presented last (its uit_cp_frame_thread's address), not every thread
// that ever did: before, the flag stayed on a thread that had presented once (boot, loading), and its later fence
// waits were counted as the frame thread's while the frame thread itself sat in a blocking wait for it (t263: the
// frame thread's "blocking waits" held 0.8-0.9 of its "fence waits" in every run, which one thread cannot do).
extern _Atomic uintptr_t uit_cp_frame_key;
static inline void uit_cp_frame_mark(void) {
    atomic_store_explicit(&uit_cp_frame_key, (uintptr_t)&uit_cp_frame_thread, memory_order_relaxed);
}
static inline int uit_cp_is_frame(void) {
    return uit_cp_frame_thread && atomic_load_explicit(&uit_cp_frame_key, memory_order_relaxed) == (uintptr_t)&uit_cp_frame_thread;
}
extern int uit_cp_level;                   // -1: not read yet
int uit_cp_init(void);
static inline int uit_cp_on(void) { int v = uit_cp_level; return v >= 0 ? v : uit_cp_init(); }
static inline void uit_cp_add(int k, uint64_t ns) {
    if (!uit_cp_on()) return;
    int c = uit_cp_is_frame() ? CPT_FRAME : CPT_OTHER;
    atomic_fetch_add_explicit(&uit_cp_ns[c][k], ns, memory_order_relaxed);
    atomic_fetch_add_explicit(&uit_cp_n[c][k], 1, memory_order_relaxed);
}
static inline void uit_dr_add(int why, uint64_t ns) {
    if (!uit_cp_on()) return;
    atomic_fetch_add_explicit(&uit_dr_ns[why], ns, memory_order_relaxed);
    atomic_fetch_add_explicit(&uit_dr_n[why], 1, memory_order_relaxed);
}
static inline void uit_cx_add(int k, uint64_t v) {
    if (uit_cp_on()) atomic_fetch_add_explicit(&uit_cx[k], v, memory_order_relaxed);
}
// The line (game thread, at present, every 300 frames): frame_ns = the 300 frames' wall time.
void uit_cp_report(uint64_t frame, uint64_t frames, uint64_t wall_ns, double gpu_ms);
// The verdict: what the frame thread waited on, from its per-frame averages (ms). fr: frame time; sync_x / sync_gpu:
// its fence waits for the translation thread / for the GPU; drain: its queue drains (all for the translation thread);
// pace: frame-rate limit sleep; xbusy: the translation thread's busy time; rec_wait: of which waiting for the other
// recording threads (the slowest chunk); other_wx: the other game threads' waits for the translation thread (together). Before 0232 the [profile] line called any
// fence wait over a quarter of the frame "the GPU", although the fence waits first wait for the translation thread
// (the 40 ms moving frames: 20.7 ms "fence waits" with the GPU busy 10 ms a frame).
static inline const char *uit_cp_verdict(double fr, double sync_x, double sync_gpu, double drain, double pace, double xbusy,
                                         double rec_wait, double other_wx, char *buf, int cap) {
    double wx = sync_x + drain;
    if (pace > 0.1 * fr)
        snprintf(buf, (size_t)cap, "the frame-rate limit (the frame thread sleeps %.1f ms a frame to TOTK_FPS: there is headroom)", pace);
    else if (wx > 0.15 * fr && wx >= sync_gpu)
        snprintf(buf, (size_t)cap, "the translation thread (the frame thread waits %.1f ms a frame for it: fence waits %.1f + queue drains %.1f; it is busy "
                 "%.1f of %.1f ms%s)", wx, sync_x, drain, xbusy, fr,
                 rec_wait > 0.3 * xbusy ? ", much of it waiting for the slowest recording thread: the largest chunk" : "");
    else if (sync_gpu > 0.15 * fr)
        snprintf(buf, (size_t)cap, "the GPU (the frame thread waits %.1f ms a frame for the GPU after the translation thread is done)", sync_gpu);
    else if (other_wx > 0.25 * fr && xbusy > 0.7 * fr)
        snprintf(buf, (size_t)cap, "the game's own threads, which wait %.1f ms a frame (together) for the translation thread (busy %.1f of %.1f ms): "
                 "the frame thread waits %.1f ms itself, the rest of its time is the game's code and its waits for those threads", other_wx, xbusy, fr,
                 wx + sync_gpu);
    else
        snprintf(buf, (size_t)cap, "the game's own code (the frame thread waits %.1f ms of %.1f; the rest is the recompiled game and its "
                 "own waits for its worker threads)", wx + sync_gpu, fr);
    return buf;
}
// [UIT 0238] the frame thread's "own work" (frame time minus its measured graphics waits) split further, from the
// game-thread wait counters of src/host/threads.c (runtime_thread_waits) and the thread's CPU time, all ms a frame:
// core: waiting for one of the 3 virtual application cores; block: the game's own blocking waits (mutexes, condition
// variables, events); sleep: SleepThread; yield: the game's polling yields; cpu: the thread's CPU time (< 0: unknown).
// What is left (own - the four) is the time it was runnable: running (cpu, which also counts spinning in any of its
// waits, so this is an upper bound) or ready but not running (Windows gave its CPU to another thread: a lower bound).
// Before 0238 these were all one number ("own work 15.13" at Lookout Landing, 2026-10-31 20:48).
static inline const char *uit_cp_own_split(double own, double core, double block, double sleep, double yield, double cpu,
                                           char *buf, int cap) {
    double run = own - core - block - sleep - yield;
    if (run < 0) run = 0;
    int o = snprintf(buf, (size_t)cap, "own work %.2f = waiting for a virtual core %.2f + the game's blocking waits %.2f + sleeps %.2f + "
                     "polling yields %.2f + runnable %.2f", own, core, block, sleep, yield, run);
    if (cpu >= 0 && o > 0 && o < cap) {
        double ready = run - cpu;
        snprintf(buf + o, (size_t)(cap - o), " (its CPU time %.2f; ready but not running at least %.2f%s)", cpu, ready > 0 ? ready : 0,
                 ready > 0.15 * (run > 0 ? run : 1) ? ": other threads take its CPU, see TOTK_FRAME_CORE" : "");
    }
    return buf;
}
