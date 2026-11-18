// [UIT 0443] Hitch attribution (TOTK_SYNC_LOG, with the sync wait statistics): for each frame over 16.7 ms, a compact
// record of where its time went, so that the hitch frames can be counted by cause directly (b8on: the 300-frame
// windows could only show that long fence waits do not follow the hitch rate inside Lookout Landing). The rules and
// the log lines, without threads or Vulkan, so that tools/tests/test_hitch.c can check them on the box.
//
// A record (hle_nvn.c, at present, 8 frames late so that the frame's GPU time is in):
//   - the frame interval (present to present, the frame thread's clock);
//   - the frame thread's waits in it: the game's blocking waits (of which ended by the game's main thread: the frame
//     thread waiting on the main thread), waits for a virtual core, sleeps and yields (src/host/threads.c
//     runtime_frame_waits), its fence waits and queue drains, its acquire wait; the rest is its own running;
//   - the main thread's: blocking waits, with the thread that ended most of them and its longest single wait (what it
//     waited on: mutex / condition variable / address / handle, the object and who ended it), core waits, sleeps,
//     yields, fence waits and drains;
//   - the fence waits over 4 ms that blocked in it (any game thread): how many, the longest and its UIT 0441 split
//     (translation thread / queue submit / GPU / copy back / wake-up), and whose it was;
//   - the frame's GPU time and draw count; shaders compiled on a thread the frame waits for (ms, count), background
//     shader compiles, texture uploads and new images (ms, count).
// The cause: each part's excess over its own recent normal (a slow running mean, spikes clamped) against the frame's
// excess over its normal. The frame thread waiting longer on the main thread is split into the main thread's own
// blocking / core / fence waits taking longer and, for the rest, the main thread running longer (the game's own frame
// work). The GPU's extra time counts as far as the frame waited for it (fence and acquire waits). The cause is the
// largest part that covers at least a third of the excess (and 1 ms), looking first at the root causes (shaders,
// textures, the GPU: their time also shows as the waits of whoever waited for them); none: "spread".
// A frame over 16.7 ms less than 1 ms above its normal (a steadily slow stretch) is "steady". Every part that covers a
// quarter of the excess is also counted under "involved" (a frame can count under several).
#pragma once
#include <stdint.h>
#include <stdio.h>
#include <string.h>

// One game thread's waits over one frame (src/host/threads.c runtime_frame_waits): ns since the last call.
typedef struct {
    uint64_t core_ns, block_ns, sleep_ns, yield_ns;
    uint64_t main_ns;                    // of block_ns: ended by the game's main thread
    uint32_t top_h; uint64_t top_ns;     // the thread that ended most of block_ns (0 timeouts / host threads, ~0 others)
    uint64_t big_ns, big_obj;            // the longest single blocking wait: how long, its object (address / key / handle)
    uint32_t big_kind, big_h;            // 1 mutex, 2 condition variable, 3 address, 4 handle (WaitSynchronization); who ended it
} uit_tframe;
#ifndef UIT_HITCH_TFRAME_ONLY   // src/host/threads.c needs only uit_tframe

// Fence waits and queue drains that blocked during one frame (filled by any game thread, under a lock).
enum { HW_FRAME, HW_MAIN, HW_OTHER, HW_N };   // whose wait
typedef struct {
    uint64_t f;
    uint64_t fence_ns[HW_N], drain_ns[HW_N];
    uint32_t long_n[HW_N];               // fence waits over 4 ms
    uint64_t long_ns; float long_seg[5]; int long_who, long_rec;   // the longest over 4 ms; its split (ms), 0: no record
} uit_hwait;
static inline void uit_hw_reset(uit_hwait *w, uint64_t f) { memset(w, 0, sizeof *w); w->f = f; w->long_who = -1; }
// A blocked wait of d ns in frame f by who; fence: 1 fence wait (seg: its fw_split in ns, NULL = no record), 0 drain.
static inline void uit_hw_add(uit_hwait *w, uint64_t f, int who, int fence, uint64_t d, const uint64_t *seg) {
    if (w->f != f) uit_hw_reset(w, f);
    if (who < 0 || who >= HW_N) who = HW_OTHER;
    if (!fence) { w->drain_ns[who] += d; return; }
    w->fence_ns[who] += d;
    if (d <= 4000000ull) return;
    w->long_n[who]++;
    if (d > w->long_ns) {
        w->long_ns = d; w->long_who = who; w->long_rec = seg != NULL;
        for (int i = 0; i < 5; i++) w->long_seg[i] = seg ? (float)(seg[i] / 1e6) : 0.0f;
    }
}

enum { HC_STEADY, HC_SPREAD, HC_GAME, HC_MAIN_BLOCK, HC_FT_BLOCK, HC_FT_WORK, HC_CORE, HC_FENCE, HC_ACQ, HC_GPU, HC_SHADER,
       HC_TEXTURE, HC_N };
static const char *const uit_hc_name[HC_N] = {"steady", "spread", "game-work", "main-blocked", "frame-blocked", "frame-work",
                                              "cores", "fence", "acquire", "gpu", "shader", "texture"};

typedef struct {
    uint64_t f; float t_s, fr, gpu; uint32_t draws;
    float ft_block, ft_main, ft_core, ft_sleep, ft_fence, ft_acq, ft_run;   // the frame thread (ms; fence: fence waits + drains)
    uint32_t ft_top_h; float ft_top;
    float mn_block, mn_core, mn_sleep, mn_fence;                            // the main thread (ms)
    uint32_t mn_top_h; float mn_top;
    float mn_big; uint64_t mn_big_obj; uint32_t mn_big_kind, mn_big_h;
    int mn_known;                                                            // 0: no main thread figures
    uint32_t fl_n; float fl_ms, fl_seg[5]; int fl_who, fl_rec;              // fence waits over 4 ms (any thread)
    float sh_ms, bg_ms, tex_ms; uint32_t sh_n, tex_n;
    float ex, cost[HC_N]; int cause; uint32_t any;                           // any: bit per cause
} uit_hrec;

// The recent normal: a slow running mean of each part over all frames (hitches too, spikes clamped to 3x + 1 ms).
enum { HB_FR, HB_GPU, HB_DRAWS, HB_FT_MAIN, HB_FT_OBLK, HB_FT_CORE, HB_FT_FENCE, HB_FT_ACQ, HB_FT_RUN, HB_MN_BLK, HB_MN_CORE,
       HB_MN_FENCE, HB_N };
typedef struct { double v[HB_N]; int have; } uit_hbase;
static inline float uit_pos(double v) { return v > 0 ? (float)v : 0.0f; }
static inline float uit_min(float a, float b) { return a < b ? a : b; }
static inline void uit_h_run(uit_hrec *r) {   // the frame thread's own running: the interval minus its waits
    r->ft_run = uit_pos(r->fr - r->ft_block - r->ft_core - r->ft_sleep - r->ft_fence - r->ft_acq);
}
static inline void uit_hb_parts(const uit_hrec *r, double p[HB_N]) {
    p[HB_FR] = r->fr; p[HB_GPU] = r->gpu > 0 ? r->gpu : 0; p[HB_DRAWS] = r->draws; p[HB_FT_MAIN] = r->ft_main;
    p[HB_FT_OBLK] = r->ft_block > r->ft_main ? r->ft_block - r->ft_main : 0; p[HB_FT_CORE] = r->ft_core; p[HB_FT_FENCE] = r->ft_fence;
    p[HB_FT_ACQ] = r->ft_acq; p[HB_FT_RUN] = r->ft_run; p[HB_MN_BLK] = r->mn_block; p[HB_MN_CORE] = r->mn_core; p[HB_MN_FENCE] = r->mn_fence;
}
static inline void uit_hb_update(uit_hbase *b, const uit_hrec *r) {
    double p[HB_N]; uit_hb_parts(r, p);
    for (int i = 0; i < HB_N; i++) {
        if (!b->have) { b->v[i] = p[i]; continue; }
        double c = 3 * b->v[i] + (i == HB_DRAWS ? 100 : 1), x = p[i] < c ? p[i] : c;
        b->v[i] += (x - b->v[i]) / 32;
    }
    b->have = 1;
}
// The record's costs, cause and flags (b: the normal before this frame; ft_run set).
static inline void uit_h_classify(uit_hrec *r, const uit_hbase *b) {
    memset(r->cost, 0, sizeof r->cost); r->any = 0;
    double p[HB_N], nm[HB_N]; uit_hb_parts(r, p);
    for (int i = 0; i < HB_N; i++) nm[i] = b->have ? b->v[i] : 0;
    r->ex = b->have ? uit_pos(p[HB_FR] - nm[HB_FR]) : uit_pos(r->fr - 16.7);
    if (r->ex < 1.0f) { r->cause = HC_STEADY; r->any = 1u << HC_STEADY; return; }
    float left = uit_pos(p[HB_FT_MAIN] - nm[HB_FT_MAIN]);   // the frame thread's extra wait on the main thread
    if (r->mn_known) {
        float x;
        x = uit_min(left, uit_pos(p[HB_MN_BLK] - nm[HB_MN_BLK])); r->cost[HC_MAIN_BLOCK] = x; left -= x;
        x = uit_min(left, uit_pos(p[HB_MN_CORE] - nm[HB_MN_CORE])); r->cost[HC_CORE] += x; left -= x;
        x = uit_min(left, uit_pos(p[HB_MN_FENCE] - nm[HB_MN_FENCE])); r->cost[HC_FENCE] += x; left -= x;
    }
    r->cost[HC_GAME] = left;
    r->cost[HC_FT_BLOCK] = uit_pos(p[HB_FT_OBLK] - nm[HB_FT_OBLK]);
    r->cost[HC_FT_WORK] = uit_pos(p[HB_FT_RUN] - nm[HB_FT_RUN]);
    r->cost[HC_CORE] += uit_pos(p[HB_FT_CORE] - nm[HB_FT_CORE]);
    r->cost[HC_FENCE] += uit_pos(p[HB_FT_FENCE] - nm[HB_FT_FENCE]);
    r->cost[HC_ACQ] = uit_pos(p[HB_FT_ACQ] - nm[HB_FT_ACQ]);
    // the GPU's extra time counts only as far as the frame waited for it (its fence waits, the main thread's, acquire)
    r->cost[HC_GPU] = r->gpu > 0 ? uit_min(uit_pos(p[HB_GPU] - nm[HB_GPU]), r->cost[HC_FENCE] + r->cost[HC_ACQ]) : 0;
    r->cost[HC_SHADER] = r->sh_ms;
    r->cost[HC_TEXTURE] = r->tex_ms;
    // The root causes first (a compile or an upload the frame waited for, the GPU behind a wait): their time also shows
    // as the waits of whoever waited for them. Then the largest of the rest.
    int best = HC_SPREAD;
    float top = 0, need = r->ex / 3 > 1.0f ? r->ex / 3 : 1.0f;
    for (int c = HC_GAME; c < HC_N; c++) if (r->cost[c] >= 0.25f * r->ex && r->cost[c] >= 1.0f) r->any |= 1u << c;
    for (int c = HC_GPU; c < HC_N; c++) if (r->cost[c] >= need && r->cost[c] > top) { top = r->cost[c]; best = c; }
    if (best == HC_SPREAD) for (int c = HC_GAME; c < HC_GPU; c++) if (r->cost[c] >= need && r->cost[c] > top) { top = r->cost[c]; best = c; }
    r->cause = best;
    if (r->cause == HC_SPREAD) r->any |= 1u << HC_SPREAD;
}

// The window (300 frames) and the run: counts, a ring of the latest hitch records, the worst ones.
enum { HR_RING = 256, HR_TOPW = 3, HR_TOPS = 8 };
typedef struct {
    uit_hbase base;
    uint64_t w_from, w_frames, w_hitch, w_cause[HC_N], w_any[HC_N], w_long, w_long_who[HW_N], w_long_part[5], w_dense, w_ring_lost;
    float w_worst, w_sum;
    uit_hrec ring[HR_RING]; uint64_t rn;        // records, newest at (rn - 1) % HR_RING
    uint64_t w_first;                            // rn at the window's start
    uit_hrec top[HR_TOPS]; int ntop;             // the run's worst, worst first
    uint64_t s_frames, s_hitch, s_cause[HC_N];
} uit_hstate;
static inline int uit_h_is_dense(const uit_hrec *r, const uit_hbase *b) { return b->have && r->draws > 1.15 * b->v[HB_DRAWS] + 1; }
// Keep the n worst (by frame interval, worst first) in top[0..*k): insert r.
static inline void uit_h_top_put(const uit_hrec **top, int *k, int n, const uit_hrec *r) {
    int i = *k;
    if (i == n) { if (r->fr <= top[n - 1]->fr) return; i = n - 1; } else (*k)++;
    while (i > 0 && top[i - 1]->fr < r->fr) { top[i] = top[i - 1]; i--; }
    top[i] = r;
}
// One frame's record (r filled, not classified; every frame, hitch or not). Returns 1 when it was a hitch.
static inline int uit_h_frame(uit_hstate *s, uit_hrec *r) {
    if (!s->w_frames) s->w_from = r->f;
    s->w_frames++; s->s_frames++;
    uit_h_run(r);
    int hitch = r->fr > 16.7f;
    r->cause = -1;
    if (hitch) {
        uit_h_classify(r, &s->base);
        s->w_hitch++; s->s_hitch++; s->w_cause[r->cause]++; s->s_cause[r->cause]++;
        for (int c = 0; c < HC_N; c++) if (r->any >> c & 1) s->w_any[c]++;
        if (r->fl_n) {
            s->w_long++; s->w_long_who[r->fl_who >= 0 && r->fl_who < HW_N ? r->fl_who : HW_OTHER]++;
            if (r->fl_rec) { int k = 0; for (int i = 1; i < 5; i++) if (r->fl_seg[i] > r->fl_seg[k]) k = i; s->w_long_part[k]++; }
        }
        if (uit_h_is_dense(r, &s->base)) s->w_dense++;
        if (r->fr > s->w_worst) s->w_worst = r->fr;
        s->w_sum += r->fr;
        if (s->rn - s->w_first >= HR_RING) s->w_ring_lost++;
        s->ring[s->rn++ % HR_RING] = *r;
        if (s->ntop < HR_TOPS || r->fr > s->top[HR_TOPS - 1].fr) {   // the run's worst, by value
            int i = s->ntop < HR_TOPS ? s->ntop++ : HR_TOPS - 1;
            while (i > 0 && s->top[i - 1].fr < r->fr) { s->top[i] = s->top[i - 1]; i--; }
            s->top[i] = *r;
        }
    }
    uit_hb_update(&s->base, r);
    return hitch;
}
// The window's worst records still in the ring (up to n, worst first); returns how many.
static inline int uit_h_window_top(const uit_hstate *s, const uit_hrec **out, int n) {
    int k = 0;
    uint64_t from = s->rn - s->w_first > HR_RING ? s->rn - HR_RING : s->w_first;
    for (uint64_t j = from; j < s->rn; j++) uit_h_top_put(out, &k, n, &s->ring[j % HR_RING]);
    return k;
}
static inline void uit_h_window_reset(uit_hstate *s) {
    s->w_frames = s->w_hitch = s->w_long = s->w_dense = s->w_ring_lost = 0;
    memset(s->w_cause, 0, sizeof s->w_cause); memset(s->w_any, 0, sizeof s->w_any);
    memset(s->w_long_who, 0, sizeof s->w_long_who); memset(s->w_long_part, 0, sizeof s->w_long_part);
    s->w_worst = s->w_sum = 0; s->w_first = s->rn;
}

// ---- the lines ----
static const char *const uit_hw_who[HW_N] = {"frame", "main", "other"};
static const char *const uit_wk_kind[5] = {"-", "mutex", "condvar", "address", "handle"};
// [hitch] window line: every 300 frames (only when the window had a hitch).
static inline int uit_h_fmt_window(const uit_hstate *s, uint64_t last, char *b, int cap) {
    int o = snprintf(b, (size_t)cap, "[hitch] frames %llu-%llu: %llu of %llu over 16.7 ms (worst %.1f, mean %.1f); cause:",
                     (unsigned long long)s->w_from, (unsigned long long)last, (unsigned long long)s->w_hitch,
                     (unsigned long long)s->w_frames, s->w_worst, s->w_hitch ? s->w_sum / s->w_hitch : 0.0);
    for (int c = 0; c < HC_N && o < cap; c++)
        if (s->w_cause[c]) o += snprintf(b + o, (size_t)(cap - o), " %s %llu", uit_hc_name[c], (unsigned long long)s->w_cause[c]);
    if (o < cap) o += snprintf(b + o, (size_t)(cap - o), "; involved:");
    for (int c = HC_GAME; c < HC_N && o < cap; c++)
        if (s->w_any[c]) o += snprintf(b + o, (size_t)(cap - o), " %s %llu", uit_hc_name[c], (unsigned long long)s->w_any[c]);
    if (o < cap)
        o += snprintf(b + o, (size_t)(cap - o), "; with a fence wait > 4 ms %llu (frame %llu, main %llu, other %llu; longest part x %llu sub %llu gpu %llu cb %llu wake %llu); dense (draws +15%%) %llu",
                      (unsigned long long)s->w_long, (unsigned long long)s->w_long_who[HW_FRAME], (unsigned long long)s->w_long_who[HW_MAIN],
                      (unsigned long long)s->w_long_who[HW_OTHER], (unsigned long long)s->w_long_part[0], (unsigned long long)s->w_long_part[1],
                      (unsigned long long)s->w_long_part[2], (unsigned long long)s->w_long_part[3], (unsigned long long)s->w_long_part[4],
                      (unsigned long long)s->w_dense);
    if (o < cap && s->w_ring_lost) o += snprintf(b + o, (size_t)(cap - o), "; %llu records past the ring", (unsigned long long)s->w_ring_lost);
    return o >= cap ? cap - 1 : o;
}
// [hitch] record line: the window's worst 3 ("w1".."w3"), the run's worst 8 every 3000 frames ("s1".."s8").
static inline int uit_h_fmt_rec(const uit_hrec *r, const char *tag, char *b, int cap) {
    int o = snprintf(b, (size_t)cap, "[hitch] %s f=%llu t=%.2f fr=%.1f ex=%.1f cause=%s | ft main=%.1f blk=%.1f by=%#x/%.1f core=%.1f fence=%.1f acq=%.1f run=%.1f",
                     tag, (unsigned long long)r->f, r->t_s, r->fr, r->ex, r->cause >= 0 && r->cause < HC_N ? uit_hc_name[r->cause] : "-",
                     r->ft_main, r->ft_block, r->ft_top_h, r->ft_top, r->ft_core, r->ft_fence, r->ft_acq, r->ft_run);
    if (o < cap) {
        if (r->mn_known)
            o += snprintf(b + o, (size_t)(cap - o), " | main blk=%.1f by=%#x/%.1f big=%s:%#llx/%.1f/%#x core=%.1f fence=%.1f",
                          r->mn_block, r->mn_top_h, r->mn_top, uit_wk_kind[r->mn_big_kind < 5 ? r->mn_big_kind : 0],
                          (unsigned long long)r->mn_big_obj, r->mn_big, r->mn_big_h, r->mn_core, r->mn_fence);
        else o += snprintf(b + o, (size_t)(cap - o), " | main -");
    }
    if (o < cap) {
        if (r->fl_n)
            o += snprintf(b + o, (size_t)(cap - o), " | fl n=%u ms=%.1f who=%s x=%.1f sub=%.1f gpu=%.1f cb=%.1f wake=%.1f%s", r->fl_n, r->fl_ms,
                          r->fl_who >= 0 && r->fl_who < HW_N ? uit_hw_who[r->fl_who] : "-", r->fl_seg[0], r->fl_seg[1], r->fl_seg[2],
                          r->fl_seg[3], r->fl_seg[4], r->fl_rec ? "" : " norec");
        else o += snprintf(b + o, (size_t)(cap - o), " | fl n=0");
    }
    if (o < cap)
        o += snprintf(b + o, (size_t)(cap - o), " | gpu=%.1f draws=%u | sh=%.1f/%u bg=%.1f tex=%.1f/%u", r->gpu, r->draws, r->sh_ms, r->sh_n,
                      r->bg_ms, r->tex_ms, r->tex_n);
    return o >= cap ? cap - 1 : o;
}
#endif
