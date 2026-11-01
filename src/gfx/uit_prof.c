// [UIT 0019] TOTK_PROFILE=1: a per-frame split of the CPU time (uit_prof.h). Every 300 frames one line, averages per frame:
//   [profile] 300 frames, 17.48 ms/frame (57.2 fps); process CPU 61.3 ms/frame (3.5 cores)
//     game: submit 1.6 (uniforms 1.3, handles 0.1), acquire: pacing 0.0, behind 3.1; fence waits 2.0, queue drains 0.0
//     translation thread: busy 15.0 = submits 14.8 (uniforms 0.2; prologue 0.3, walk + own chunks 4.0, waiting for
//     other threads 9.5, moves + submit 1.0) + presents 0.2; idle 2.4; recording threads 26.4 ms in 4 threads
//     -> limit: ...
// The "limit" names what the frame most likely waits for: the translation thread when it is busy for most of the frame
// (GPU command recording: TOTK_VK_RECORD_THREADS, TOTK_VK_CHUNK_SPLIT), else the game's own code (the recompiled game:
// the PGO / ThinLTO builds of setup.py), or the frame-rate limit when the game sleeps to it.
#include "uit_prof.h"
#include "uit_crit.h"

#include <stdio.h>
#include <stdlib.h>
#include <windows.h>

#include "host/dm.h"

_Atomic uint64_t uit_prof_ns[UP_N];
int uit_prof_level = -1;
// [UIT 0232] TOTK_CRITPATH (uit_crit.h)
_Atomic uint64_t uit_cp_ns[CPT_N][CP_K], uit_cp_n[CPT_N][CP_K], uit_dr_ns[DR_N], uit_dr_n[DR_N], uit_cx[CX_N];
__thread int uit_cp_frame_thread;
_Atomic uintptr_t uit_cp_frame_key;
int uit_cp_level = -1;
int uit_cp_init(void) {
    const char *e = getenv("TOTK_CRITPATH");
    int v = !(e && *e == '0');
    uit_cp_level = v;
    return v;
}
extern int rt_dispatch_count;                       // runtime/dispatch.c: dispatch cache misses (indirect calls)
extern _Atomic unsigned long long rt_dispatch_misses;

int uit_prof_init(void) {
    const char *e = getenv("TOTK_PROFILE");
    int v = e && *e && atoi(e) > 0;
    if (v) LOG("[profile] TOTK_PROFILE=1: a per-frame split of the CPU time every 300 frames ([profile] lines)");
    uit_prof_level = v;
    rt_dispatch_count = v;
    return v;
}

static uint64_t proc_cpu_ns(void) {
    FILETIME c, x, k, u;
    if (!GetProcessTimes(GetCurrentProcess(), &c, &x, &k, &u)) return 0;
    uint64_t kk = ((uint64_t)k.dwHighDateTime << 32) | k.dwLowDateTime, uu = ((uint64_t)u.dwHighDateTime << 32) | u.dwLowDateTime;
    return (kk + uu) * 100;
}

void uit_prof_present(uint64_t now) {
    if (!uit_prof_on()) return;
    static uint64_t t_start, cpu_start, frames;
    if (!t_start) { t_start = now; cpu_start = proc_cpu_ns(); for (int k = 0; k < UP_N; k++) atomic_store(&uit_prof_ns[k], 0); return; }
    if (++frames < 300) return;
    uint64_t v[UP_N];
    for (int k = 0; k < UP_N; k++) v[k] = atomic_exchange(&uit_prof_ns[k], 0);
    uint64_t cpu = proc_cpu_ns();
    double nf = (double)frames, fr = (now - t_start) / 1e6 / nf, pc = (cpu - cpu_start) / 1e6 / nf;
#define M(k) (v[k] / 1e6 / nf)
    int vkb_mt_threads(void);
    double xbusy = M(UP_X_SUBMIT) + M(UP_X_PRESENT);
    LOG("[profile] %.0f frames, %.2f ms/frame (%.1f fps); process CPU %.1f ms/frame (%.1f cores busy)", nf, fr, fr > 0 ? 1000.0 / fr : 0, pc,
        fr > 0 ? pc / fr : 0);
    LOG("[profile]   game: submit %.2f (uniforms + vertex data %.2f, deferred handles %.2f), acquire: pacing sleep %.2f, behind the "
        "translation thread / GPU %.2f; fence waits %.2f (for the translation thread %.2f, then the GPU %.2f), queue drains %.2f (all game threads together)", M(UP_G_SUBMIT),
        M(UP_G_UCAP), M(UP_G_DEFER), M(UP_G_ACQ_PACE), M(UP_G_ACQ_BEHIND), M(UP_G_SYNC), M(UP_G_SYNC_X), M(UP_G_SYNC) - M(UP_G_SYNC_X), M(UP_G_DRAIN));
    LOG("[profile]   translation thread: busy %.2f = submits %.2f (uniforms %.2f; prologue %.2f, walk + own chunks %.2f, waiting for the other "
        "recording threads %.2f, moves + submit %.2f) + presents %.2f; idle %.2f; recording threads %.2f ms in %d threads",
        xbusy, M(UP_X_SUBMIT), M(UP_X_UCAP), M(UP_X_PROLOGUE), M(UP_X_WALK), M(UP_X_RECWAIT), M(UP_X_COMMIT), M(UP_X_PRESENT), M(UP_X_IDLE),
        M(UP_R_REC), vkb_mt_threads());
    const char *lim;
    int vkb_shadow_forced_off(void);
    // [UIT 0026] the GPU first: with fence waits a quarter of the frame or more the game waits for the GPU, whatever
    // pacing sleep is left over (the verdict said "frame-rate limit" at 24-28 fps with 20-30 ms of fence waits)
    // [UIT 0232] a fence wait is first a wait for the translation thread (UP_G_SYNC_X), then for the GPU: only the GPU
    // part is the GPU's (the moving frames: 20.7 ms of fence waits with the GPU busy 10 ms a frame were called "the GPU");
    // the translation part and the queue drains count for the translation thread
    double sync_gpu = M(UP_G_SYNC) - M(UP_G_SYNC_X), wait_x = M(UP_G_SYNC_X) + M(UP_G_DRAIN);
    if (sync_gpu > 0.25 * fr && sync_gpu >= wait_x)
        lim = vkb_shadow_forced_off() ? "the GPU (fence waits): TOTK_VK_POOL_SHADOW=0 makes every draw read game memory across PCIe - set it back to Auto"
                                      : "the GPU (fence waits: lower the resolution / use DLSS or FSR, or GPU-heavy settings)";
    else if (M(UP_G_ACQ_PACE) > 0.1 * fr) lim = "the frame-rate limit (the game sleeps to TOTK_FPS: there is headroom)";
    else if (xbusy > 0.8 * fr || wait_x > 0.25 * fr)
        lim = M(UP_X_RECWAIT) > 0.3 * xbusy ? "the translation thread, waiting for the slowest recording thread (TOTK_VK_RECORD_THREADS, TOTK_VK_CHUNK_SPLIT)"
                                             : "the translation thread (GPU command recording)";
    else lim = "the game's own code (recompiled game: the PGO / ThinLTO builds of setup.py help here)";
    unsigned long long miss = atomic_exchange(&rt_dispatch_misses, 0);
    LOG("[profile]   indirect calls: %.0f dispatch cache misses/frame (each a table search of ~0.1-0.3 us)", miss / nf);
    LOG("[profile]   -> the frame most likely waits for %s", lim);
#undef M
    t_start = now; cpu_start = cpu; frames = 0;
}

// [UIT 0238] src/host/threads.c (absent in graphics-only builds: then the split is not shown)
__attribute__((weak)) void runtime_thread_waits(uint64_t v[8]) { for (int i = 0; i < 8; i++) v[i] = ~0ull; }
__attribute__((weak)) void runtime_glock_stats(uint64_t v[3]) { v[0] = v[1] = v[2] = ~0ull; }   // [UIT 0246]
__attribute__((weak)) void runtime_syncwake_stats(uint64_t v[2]) { v[0] = v[1] = ~0ull; }   // [UIT 0247]
static uint64_t thread_cpu_ns(void) {
    FILETIME c, x, k, u;
    if (!GetThreadTimes(GetCurrentThread(), &c, &x, &k, &u)) return ~0ull;
    uint64_t kk = ((uint64_t)k.dwHighDateTime << 32) | k.dwLowDateTime, uu = ((uint64_t)u.dwHighDateTime << 32) | u.dwLowDateTime;
    return (kk + uu) * 100;
}
// [UIT 0232] TOTK_CRITPATH: the [critical path] lines (uit_crit.h). Game thread, at present, every 300 frames.
void uit_cp_report(uint64_t frame, uint64_t frames, uint64_t wall_ns, double gpu_ms) {
    if (!uit_cp_on() || !frames) return;
    static uint64_t cpu_prev;
    uint64_t cpu = proc_cpu_ns();
    uint64_t c[CPT_N][CP_K], cn[CPT_N][CP_K], d[DR_N], dn[DR_N], x[CX_N];
    for (int t = 0; t < CPT_N; t++)
        for (int k = 0; k < CP_K; k++) { c[t][k] = atomic_exchange(&uit_cp_ns[t][k], 0); cn[t][k] = atomic_exchange(&uit_cp_n[t][k], 0); }
    for (int k = 0; k < DR_N; k++) { d[k] = atomic_exchange(&uit_dr_ns[k], 0); dn[k] = atomic_exchange(&uit_dr_n[k], 0); }
    for (int k = 0; k < CX_N; k++) x[k] = atomic_exchange(&uit_cx[k], 0);
    double nf = (double)frames, fr = wall_ns / 1e6 / nf, pc = cpu_prev ? (cpu - cpu_prev) / 1e6 / nf : 0;
    cpu_prev = cpu;
#define F(t, k) (c[t][k] / 1e6 / nf)
#define X(k) (x[k] / 1e6 / nf)
    double waits = F(CPT_FRAME, CP_SYNC_X) + F(CPT_FRAME, CP_SYNC_GPU) + F(CPT_FRAME, CP_DRAIN) + F(CPT_FRAME, CP_ACQ_PACE) + F(CPT_FRAME, CP_ACQ_BEHIND);
    double own = fr - waits;
    char g[64];
    if (gpu_ms >= 0) snprintf(g, sizeof g, "%.2f ms a frame", gpu_ms); else snprintf(g, sizeof g, "not measured");
    LOG("[critical path] through frame %llu (%.0f frames): %.2f ms a frame (%.1f fps), process CPU %.1f ms a frame; GPU %s",
        (unsigned long long)frame, nf, fr, fr > 0 ? 1000.0 / fr : 0, pc, g);
    LOG("[critical path]   frame thread: own work %.2f (the game's code, its submit calls %.2f, its waits for its own threads); waits %.2f = "
        "fence waits for the translation thread %.2f (%.1f a frame) + for the GPU %.2f + queue drains %.2f (%.1f a frame) + frame-rate limit "
        "sleep %.2f + behind the last present of the image %.2f", own, F(CPT_FRAME, CP_SUBMIT), waits, F(CPT_FRAME, CP_SYNC_X),
        cn[CPT_FRAME][CP_SYNC_X] / nf, F(CPT_FRAME, CP_SYNC_GPU), F(CPT_FRAME, CP_DRAIN), cn[CPT_FRAME][CP_DRAIN] / nf, F(CPT_FRAME, CP_ACQ_PACE),
        F(CPT_FRAME, CP_ACQ_BEHIND));
    {   // [UIT 0238] own work split (the frame thread's counters: this runs on it; the first report after a change of
        // frame thread only takes the starting values)
        static uint64_t pw[8], pcpu; static DWORD ptid;
        uint64_t w[8], tc = thread_cpu_ns(); runtime_thread_waits(w);
        DWORD tid = GetCurrentThreadId();
        if (w[0] != ~0ull && tid == ptid) {
            char sb[512];
            double cpu = tc != ~0ull && pcpu != ~0ull ? (tc - pcpu) / 1e6 / nf : -1;
            LOG("[critical path]   frame thread: %s; %.1f core waits, %.1f blocking waits, %.1f yields a frame", uit_cp_own_split(own,
                (w[0] - pw[0]) / 1e6 / nf, (w[1] - pw[1]) / 1e6 / nf, (w[2] - pw[2]) / 1e6 / nf, (w[3] - pw[3]) / 1e6 / nf, cpu, sb, (int)sizeof sb),
                (w[4] - pw[4]) / nf, (w[5] - pw[5]) / nf, (w[7] - pw[7]) / nf);
        }
        for (int i = 0; i < 8; i++) pw[i] = w[i];
        pcpu = tc; ptid = tid;
    }
    {   // [UIT 0246] the guest scheduling lock (threads.c G), all game threads together
        static uint64_t pg[3]; static int have;
        uint64_t g[3]; runtime_glock_stats(g);
        if (g[0] != ~0ull && have)
            LOG("[critical path]   guest scheduling lock (all game threads, a frame): busy and taken after retrying %.1f, slept for %.1f (%.3f ms)",
                (g[0] - pg[0]) / nf, (g[1] - pg[1]) / nf, (g[2] - pg[2]) / 1e6 / nf);
        for (int i = 0; i < 3; i++) pg[i] = g[i];
        have = 1;
    }
    {   // [UIT 0247] WaitSynchronization wake-ups by event signals and thread exits
        static uint64_t pw[2]; static int have;
        uint64_t w[2]; runtime_syncwake_stats(w);
        if (w[0] != ~0ull && have)
            LOG("[critical path]   guest event signals (a frame): woke %.1f waiting threads, skipped %.1f not waiting on that event",
                (w[0] - pw[0]) / nf, (w[1] - pw[1]) / nf);
        pw[0] = w[0]; pw[1] = w[1];
        have = 1;
    }
    LOG("[critical path]   other game threads (together, not on the frame's path unless the frame thread waits for them): fence waits for the "
        "translation thread %.2f, for the GPU %.2f, queue drains %.2f (%.1f a frame), submit calls %.2f", F(CPT_OTHER, CP_SYNC_X),
        F(CPT_OTHER, CP_SYNC_GPU), F(CPT_OTHER, CP_DRAIN), cn[CPT_OTHER][CP_DRAIN] / nf, F(CPT_OTHER, CP_SUBMIT));
    static const char *const dname[DR_N] = {"pool deletes", "ReadTexels", "WriteTexels", "program deletes", "QueueFinish", "suspend",
                                            "graphics layer (buffer reads of GPU-drawn memory)", "GPU copies for game threads"};
    char b[1024]; int o = 0;
    for (int k = 0; k < DR_N; k++) {
        if (!dn[k]) continue;
        o += snprintf(b + o, sizeof b - (size_t)o, "%s%s %.2f / %.2f ms", o ? ", " : "", dname[k], dn[k] / nf, d[k] / 1e6 / nf);
        if (o >= (int)sizeof b) { o = (int)sizeof b - 1; break; }
    }
    LOG("[critical path]   queue drains by cause (all game threads, a frame: count / ms waiting): %s", o ? b : "none");
    double subs = x[CX_SUBS] / nf, xbusy = X(CX_SUBMIT) + X(CX_PRESENT);
    LOG("[critical path]   translation thread: busy %.2f = submissions %.2f + presents %.2f; idle %.2f; %.1f submissions a frame: prologue %.2f, "
        "walk + own chunks %.2f, waiting for the other recording threads %.2f, moves + submit %.2f; largest chunk %.2f ms a frame (%.0f draws)",
        xbusy, X(CX_SUBMIT), X(CX_PRESENT), X(CX_IDLE), subs, X(CX_REC_PRO), X(CX_REC_WALK), X(CX_REC_WAIT), X(CX_REC_COMMIT), X(CX_REC_BIG),
        x[CX_REC_BIGDRAWS] / nf);
    char v[512];
    LOG("[critical path]   -> the frame waits on %s", uit_cp_verdict(fr, F(CPT_FRAME, CP_SYNC_X), F(CPT_FRAME, CP_SYNC_GPU), F(CPT_FRAME, CP_DRAIN),
                                                               F(CPT_FRAME, CP_ACQ_PACE), xbusy, X(CX_REC_WAIT),
                                                               F(CPT_OTHER, CP_SYNC_X) + F(CPT_OTHER, CP_DRAIN), v, (int)sizeof v));
#undef F
#undef X
}
