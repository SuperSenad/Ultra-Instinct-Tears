// Box test for [UIT 0443] hitch attribution (src/gfx/uit_hitch.h, used by hle_nvn.c with TOTK_SYNC_LOG):
//   cc -O2 -Wall -Wextra -Isrc/gfx tools/tests/test_hitch.c -o /tmp/h/hitch && /tmp/h/hitch
// 1) uit_hw_add: a new frame number starts the frame's record afresh; every blocked fence wait adds to its thread's sum,
//    only those over 4 ms are counted and the longest keeps its split; drains go apart; no record: marked; a bad
//    thread index counts as "other".
// 2) uit_h_classify against a steady normal: a shader frame, the main thread blocked longer, the main thread running
//    longer (game work), a fence wait on the frame thread, a GPU-bound frame, a texture frame, the frame thread blocked
//    on another thread, waiting for cores, a steadily slow frame ("steady"), a frame whose excess is spread thinly
//    ("spread"); "involved" holds every part over a quarter; the frame thread's running time is the interval minus its
//    waits and never below 0.
// 3) The window and the run: counts by cause add up to the hitches; the window's worst 3 and the run's worst 8 are the
//    worst, in order; more hitches than the ring holds are counted and the top comes from what the ring still has;
//    the window resets, the run does not; the normal is not dragged by spikes.
// 4) The lines: every field present, no cut at the largest values.
// 5) A model: random scenes, each hitch frame given one dominant cause at random size; the cause found is that one in
//    at least 95% of them.
// TOTK_PROBE_NEGATIVE=1: the main thread's figures are dropped (as if it were not split) - case 2's main-blocked
// check must fail.
#include "uit_hitch.h"
#include <stdlib.h>

static int fails, neg;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static uint64_t rng = 88172645463325252ull;
static uint64_t xr(void) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return rng; }
static double ur(double a, double b) { return a + (b - a) * (double)(xr() % 1000000) / 1e6; }

// A normal frame at Lookout Landing (b8on): 13 ms, the frame thread waits ~9 ms on the main thread.
static uit_hrec normal(uint64_t f) {
    uit_hrec r; memset(&r, 0, sizeof r);
    r.f = f; r.fr = 13; r.gpu = 9; r.draws = 2000;
    r.ft_block = 9.5f; r.ft_main = 9; r.ft_core = 0.3f; r.ft_fence = 1; r.ft_acq = 0.2f;
    r.mn_known = 1; r.mn_block = 3; r.mn_core = 0.5f; r.mn_fence = 0.5f;
    return r;
}
static uit_hbase steady_base(void) {
    uit_hbase b; memset(&b, 0, sizeof b);
    for (int i = 0; i < 400; i++) { uit_hrec r = normal((uint64_t)i); uit_h_run(&r); uit_hb_update(&b, &r); }
    return b;
}
static int cause_of(uit_hrec r, const uit_hbase *b) { if (neg) r.mn_known = 0; uit_h_run(&r); uit_h_classify(&r, b); return r.cause; }

static void t_hw(void) {
    uit_hwait w; uit_hw_reset(&w, 5);
    uint64_t seg[5] = {1000000, 0, 3000000, 500000, 500000}, seg2[5] = {0, 0, 9000000, 0, 0};
    uit_hw_add(&w, 5, HW_FRAME, 1, 2000000, NULL);
    CHECK(w.fence_ns[HW_FRAME] == 2000000 && w.long_n[HW_FRAME] == 0 && w.long_who == -1, "short wait: summed, not long");
    uit_hw_add(&w, 5, HW_MAIN, 1, 5000000, seg);
    CHECK(w.long_n[HW_MAIN] == 1 && w.long_ns == 5000000 && w.long_who == HW_MAIN && w.long_rec == 1 && w.long_seg[2] == 3.0f, "long wait kept with its split");
    uit_hw_add(&w, 5, HW_OTHER, 1, 4500000, NULL);
    CHECK(w.long_n[HW_OTHER] == 1 && w.long_ns == 5000000 && w.long_who == HW_MAIN, "a shorter long wait does not replace the longest");
    uit_hw_add(&w, 5, HW_FRAME, 1, 9000000, seg2);
    CHECK(w.long_ns == 9000000 && w.long_who == HW_FRAME && w.long_seg[2] == 9.0f && w.long_seg[0] == 0, "the longest replaces");
    uit_hw_add(&w, 5, HW_FRAME, 1, 12000000, NULL);
    CHECK(w.long_rec == 0 && w.long_seg[2] == 0, "no record: marked, split cleared");
    uit_hw_add(&w, 5, HW_MAIN, 0, 7000000, NULL);
    CHECK(w.drain_ns[HW_MAIN] == 7000000 && w.long_n[HW_MAIN] == 1, "drains apart, never long fence waits");
    uit_hw_add(&w, 5, 9, 1, 1000, NULL);
    CHECK(w.fence_ns[HW_OTHER] == 4500000 + 1000, "a bad index counts as other");
    uit_hw_add(&w, 6, HW_FRAME, 1, 1000, NULL);
    CHECK(w.f == 6 && w.fence_ns[HW_FRAME] == 1000 && w.long_ns == 0 && w.drain_ns[HW_MAIN] == 0 && w.long_who == -1, "a new frame starts afresh");
    CHECK(5000000ull > 4000000ull, "edge");
    uit_hw_reset(&w, 7); uit_hw_add(&w, 7, HW_FRAME, 1, 4000000, seg);
    CHECK(w.long_n[HW_FRAME] == 0, "exactly 4 ms is not over 4 ms");
}

static void t_classify(void) {
    uit_hbase b = steady_base();
    CHECK(b.have && b.v[HB_FR] > 12.9 && b.v[HB_FR] < 13.1 && b.v[HB_FT_MAIN] > 8.9 && b.v[HB_FT_MAIN] < 9.1, "the normal settles: fr %.2f", b.v[HB_FR]);
    uit_hrec r;
    r = normal(1); r.fr = 26; r.sh_ms = 12; r.sh_n = 3; r.ft_block = 21.5f; r.ft_main = 21; r.mn_block = 15;   // the main thread waited for a compile
    { int c = cause_of(r, &b); CHECK(c == HC_SHADER, "shader frame: %s", c >= 0 ? uit_hc_name[c] : "-"); }
    r = normal(2); r.fr = 24; r.ft_block = 20.5f; r.ft_main = 20; r.mn_block = 14;
    { int c = cause_of(r, &b); CHECK(c == HC_MAIN_BLOCK, "main thread blocked: %s", c >= 0 ? uit_hc_name[c] : "-"); }
    r = normal(3); r.fr = 24; r.ft_block = 20.5f; r.ft_main = 20;
    { int c = cause_of(r, &b); CHECK(c == HC_GAME, "main thread running longer: %s", c >= 0 ? uit_hc_name[c] : "-"); }
    r = normal(4); r.fr = 23; r.ft_fence = 11;
    { int c = cause_of(r, &b); CHECK(c == HC_FENCE, "fence wait on the frame thread: %s", c >= 0 ? uit_hc_name[c] : "-"); }
    r = normal(5); r.fr = 24; r.gpu = 23; r.ft_fence = 7; r.ft_acq = 4;
    { int c = cause_of(r, &b); CHECK(c == HC_GPU, "GPU-bound: %s", c >= 0 ? uit_hc_name[c] : "-"); }
    r = normal(6); r.fr = 21; r.tex_ms = 7; r.ft_block = 16.5f; r.ft_main = 16; r.mn_block = 3.5f;   // the main thread itself uploaded
    { int c = cause_of(r, &b); CHECK(c == HC_TEXTURE, "texture: %s", c >= 0 ? uit_hc_name[c] : "-"); }
    r = normal(7); r.fr = 22; r.ft_block = 18.5f; r.ft_top_h = 0x1037; r.ft_top = 9;
    { int c = cause_of(r, &b); CHECK(c == HC_FT_BLOCK, "frame thread blocked on another: %s", c >= 0 ? uit_hc_name[c] : "-"); }
    r = normal(8); r.fr = 21; r.ft_core = 8.3f;
    { int c = cause_of(r, &b); CHECK(c == HC_CORE, "cores: %s", c >= 0 ? uit_hc_name[c] : "-"); }
    r = normal(9); r.fr = 17.6f; r.ft_block = 13.5f; r.ft_main = 13; r.mn_core = 4.5f;   // the main thread waited for a core
    { int c = cause_of(r, &b); CHECK(c == HC_CORE, "the main thread's core waits: %s", c >= 0 ? uit_hc_name[c] : "-"); }
    r = normal(10); r.fr = 20; r.ft_acq = 7.2f;
    { int c = cause_of(r, &b); CHECK(c == HC_ACQ, "acquire: %s", c >= 0 ? uit_hc_name[c] : "-"); }
    r = normal(11); r.fr = 22;   // the frame thread running longer itself
    { int c = cause_of(r, &b); CHECK(c == HC_FT_WORK, "frame thread work: %s", c >= 0 ? uit_hc_name[c] : "-"); }
    // spread: 10 ms over, five parts of 2 ms (under a third each, under a quarter each)
    r = normal(12); r.fr = 23; r.ft_core += 2; r.ft_fence += 2; r.ft_acq += 2; r.ft_block += 2; r.ft_main += 2; r.gpu += 2;   // + 2 ms running
    { uit_hrec q = r; uit_h_run(&q); uit_h_classify(&q, &b);
      CHECK(q.cause == HC_SPREAD && q.any == (1u << HC_SPREAD), "spread: %s any %#x", uit_hc_name[q.cause], q.any); }
    // steady: a slow stretch, the normal at 18 ms
    { uit_hbase s; memset(&s, 0, sizeof s);
      for (int i = 0; i < 400; i++) { uit_hrec q = normal((uint64_t)i); q.fr = 18; uit_h_run(&q); uit_hb_update(&s, &q); }
      r = normal(13); r.fr = 18.5f; int c = cause_of(r, &s); CHECK(c == HC_STEADY, "steady: %s", c >= 0 ? uit_hc_name[c] : "-"); }
    // involved: shader 8 and main blocked 6 on a 14 ms excess -> cause shader, both involved
    r = normal(14); r.fr = 27; r.sh_ms = 8; r.ft_block = 23.5f; r.ft_main = 23; r.mn_block = 9;
    { uit_hrec q = r; uit_h_run(&q); uit_h_classify(&q, &b);
      CHECK(q.cause == HC_SHADER && (q.any >> HC_SHADER & 1) && (q.any >> HC_MAIN_BLOCK & 1) && (q.any >> HC_GAME & 1), "involved: %s %#x", uit_hc_name[q.cause], q.any);
      CHECK(q.ex > 13.9f && q.ex < 14.1f, "excess %.2f", q.ex);
      CHECK(q.cost[HC_MAIN_BLOCK] > 5.9f && q.cost[HC_MAIN_BLOCK] < 6.1f && q.cost[HC_GAME] > 7.9f && q.cost[HC_GAME] < 8.1f, "main split: blocked %.2f game %.2f", q.cost[HC_MAIN_BLOCK], q.cost[HC_GAME]); }
    // the main thread's blocking grows but the frame thread did not wait longer on it: not the cause
    r = normal(15); r.fr = 21; r.mn_block = 15;
    { uit_hrec q = r; uit_h_run(&q); uit_h_classify(&q, &b); CHECK(q.cost[HC_MAIN_BLOCK] == 0 && q.cause == HC_FT_WORK, "main blocked off the path: %s", uit_hc_name[q.cause]); }
    // running time: never below 0
    r = normal(16); r.fr = 5; r.ft_block = 9; uit_h_run(&r); CHECK(r.ft_run == 0, "run clamped at 0: %.2f", r.ft_run);
    r = normal(17); uit_h_run(&r); CHECK(r.ft_run > 1.99f && r.ft_run < 2.01f, "run = fr - waits: %.2f", r.ft_run);
    // no normal yet: the excess is over 16.7 ms
    { uit_hbase z; memset(&z, 0, sizeof z); r = normal(18); r.fr = 30; r.sh_ms = 12; uit_h_run(&r); uit_h_classify(&r, &z);
      CHECK(r.ex > 13.2f && r.ex < 13.4f && r.cause == HC_SHADER, "no normal: ex %.2f %s", r.ex, uit_hc_name[r.cause]); }
    // the GPU longer but nothing waited for it: not the cause
    r = normal(20); r.fr = 22; r.gpu = 20;
    { uit_hrec q = r; uit_h_run(&q); uit_h_classify(&q, &b); CHECK(q.cost[HC_GPU] == 0 && q.cause == HC_FT_WORK, "GPU off the path: %s", uit_hc_name[q.cause]); }
    // the GPU time not measured (< 0) is never the cause
    r = normal(19); r.fr = 25; r.gpu = -1; r.ft_fence = 13;
    { uit_hrec q = r; uit_h_run(&q); uit_h_classify(&q, &b); CHECK(q.cost[HC_GPU] == 0 && q.cause == HC_FENCE, "GPU not measured"); }
}

static void t_state(void) {
    static uit_hstate s; memset(&s, 0, sizeof s);
    uint64_t want[HC_N] = {0}, hitches = 0;
    float worst[400]; int nw = 0;
    for (uint64_t f = 1; f <= 300; f++) {
        uit_hrec r = normal(f); r.fr = (float)ur(12, 14);
        if (f % 10 == 0) { r.fr = 20 + (float)(f % 70) / 10; r.sh_ms = r.fr - 13; want[HC_SHADER]++; hitches++; worst[nw++] = r.fr; }
        else if (f % 10 == 5) { r.fr = 22; r.ft_block = 18.5f; r.ft_main = 18; r.mn_block = 12; want[HC_MAIN_BLOCK]++; hitches++; worst[nw++] = r.fr;
                                r.fl_n = 1; r.fl_who = HW_MAIN; r.fl_rec = 1; r.fl_ms = 5; r.fl_seg[2] = 4; }
        int h = uit_h_frame(&s, &r);
        CHECK(h == (r.fr > 16.7f), "frame %llu hitch flag", (unsigned long long)f);
    }
    CHECK(s.w_frames == 300 && s.w_hitch == hitches && s.w_from == 1, "window: %llu frames %llu hitches", (unsigned long long)s.w_frames, (unsigned long long)s.w_hitch);
    uint64_t sum = 0; for (int c = 0; c < HC_N; c++) sum += s.w_cause[c];
    CHECK(sum == s.w_hitch, "causes add up");
    CHECK(s.w_cause[HC_SHADER] == want[HC_SHADER] && s.w_cause[HC_MAIN_BLOCK] == want[HC_MAIN_BLOCK], "counts: shader %llu/%llu main %llu/%llu",
          (unsigned long long)s.w_cause[HC_SHADER], (unsigned long long)want[HC_SHADER], (unsigned long long)s.w_cause[HC_MAIN_BLOCK], (unsigned long long)want[HC_MAIN_BLOCK]);
    CHECK(s.w_long == want[HC_MAIN_BLOCK] && s.w_long_who[HW_MAIN] == want[HC_MAIN_BLOCK] && s.w_long_part[2] == want[HC_MAIN_BLOCK], "long waits by whose / part");
    const uit_hrec *top[HR_TOPW]; int n = uit_h_window_top(&s, top, HR_TOPW);
    // the expected worst 3
    for (int i = 0; i < nw; i++) for (int j = i + 1; j < nw; j++) if (worst[j] > worst[i]) { float t = worst[i]; worst[i] = worst[j]; worst[j] = t; }
    CHECK(n == 3 && top[0]->fr == worst[0] && top[1]->fr == worst[1] && top[2]->fr == worst[2], "window top 3: %.1f %.1f %.1f vs %.1f %.1f %.1f",
          n > 0 ? top[0]->fr : 0, n > 1 ? top[1]->fr : 0, n > 2 ? top[2]->fr : 0, worst[0], worst[1], worst[2]);
    CHECK(s.ntop == HR_TOPS, "run top filled");
    for (int i = 0; i < s.ntop; i++) CHECK(s.top[i].fr == worst[i], "run top %d: %.1f vs %.1f", i, s.top[i].fr, worst[i]);
    CHECK(s.base.v[HB_FR] < 15.5, "the normal (20%% hitches of 20-27 ms) stays below 15.5: %.2f", s.base.v[HB_FR]);
    char b[2048]; uit_h_fmt_window(&s, 300, b, (int)sizeof b);
    CHECK(strstr(b, "[hitch] frames 1-300: 60 of 300 over 16.7 ms") && strstr(b, "shader 30") && strstr(b, "main-blocked 30") &&
          strstr(b, "with a fence wait > 4 ms 30 (frame 0, main 30, other 0; longest part x 0 sub 0 gpu 30"), "window line: %s", b);
    uit_h_window_reset(&s);
    CHECK(s.w_frames == 0 && s.w_hitch == 0 && s.w_first == s.rn && s.s_hitch == hitches, "window reset, run kept");
    CHECK(uit_h_window_top(&s, top, HR_TOPW) == 0, "empty window: no top");
    // a window with more hitches than the ring: all counted, the top from the ring
    for (uint64_t f = 301; f <= 600; f++) { uit_hrec r = normal(f); r.fr = f == 320 ? 50 : f == 590 ? 45 : 18 + (float)(f % 7) / 10; r.sh_ms = r.fr - 13; uit_h_frame(&s, &r); }
    CHECK(s.w_hitch == 300 && s.w_ring_lost == 300 - HR_RING, "ring overflow counted: %llu lost", (unsigned long long)s.w_ring_lost);
    n = uit_h_window_top(&s, top, HR_TOPW);
    CHECK(n == 3 && top[0]->f == 590 && top[0]->fr == 45, "top from the ring (frame 320 is past it): %llu", n ? (unsigned long long)top[0]->f : 0ull);
    CHECK(s.top[0].f == 320 && s.top[1].f == 590, "the run's worst keeps frame 320");
    uit_h_fmt_window(&s, 600, b, (int)sizeof b);
    CHECK(strstr(b, "records past the ring"), "ring loss in the line");
}

static void t_lines(void) {
    uit_hrec r; memset(&r, 0, sizeof r);
    r.f = 18446744073709551615ull; r.t_s = 99999.99f; r.fr = 1e6f; r.ex = 1e6f; r.cause = HC_TEXTURE;
    r.ft_main = r.ft_block = r.ft_core = r.ft_fence = r.ft_acq = r.ft_run = r.ft_top = 1e6f; r.ft_top_h = 0xffffffffu;
    r.mn_known = 1; r.mn_block = r.mn_top = r.mn_big = r.mn_core = r.mn_fence = 1e6f; r.mn_top_h = r.mn_big_h = 0xffffffffu;
    r.mn_big_kind = 2; r.mn_big_obj = ~0ull; r.fl_n = 4000000000u; r.fl_ms = 1e6f; r.fl_who = HW_OTHER; r.fl_rec = 0;
    for (int i = 0; i < 5; i++) r.fl_seg[i] = 1e6f;
    r.gpu = 1e6f; r.draws = 4000000000u; r.sh_ms = r.bg_ms = r.tex_ms = 1e6f; r.sh_n = r.tex_n = 4000000000u;
    char b[2048]; int o = uit_h_fmt_rec(&r, "s8", b, (int)sizeof b);
    CHECK(o < (int)sizeof b - 1 && strstr(b, "tex=1000000.0/4000000000"), "record line not cut (%d): %s", o, b);
    const char *keys[] = {"[hitch] s8 f=", " t=", " fr=", " ex=", " cause=texture", "| ft main=", " blk=", " by=", " core=", " fence=", " acq=", " run=",
                          "| main blk=", " big=condvar:0xffffffffffffffff/", "| fl n=", " who=other", " x=", " sub=", " gpu=", " cb=", " wake=", " norec",
                          "| gpu=", " draws=", "| sh=", " bg=", " tex="};
    for (size_t i = 0; i < sizeof keys / sizeof *keys; i++) CHECK(strstr(b, keys[i]), "record line has %s", keys[i]);
    r.mn_known = 0; r.fl_n = 0; uit_h_fmt_rec(&r, "w1", b, (int)sizeof b);
    CHECK(strstr(b, "| main -") && strstr(b, "| fl n=0 |"), "no main / no long wait: %s", b);
    static uit_hstate s; memset(&s, 0, sizeof s);
    s.w_from = ~0ull; s.w_frames = s.w_hitch = ~0ull; s.w_worst = 1e6f; s.w_sum = 1e9f; s.w_long = s.w_dense = s.w_ring_lost = ~0ull;
    for (int c = 0; c < HC_N; c++) s.w_cause[c] = s.w_any[c] = ~0ull;
    for (int i = 0; i < 5; i++) s.w_long_part[i] = ~0ull;
    for (int i = 0; i < HW_N; i++) s.w_long_who[i] = ~0ull;
    o = uit_h_fmt_window(&s, ~0ull, b, (int)sizeof b);
    CHECK(o < (int)sizeof b - 1 && strstr(b, "records past the ring"), "window line not cut at the largest values (%d)", o);
}

static void t_model(void) {
    static uit_hstate s; memset(&s, 0, sizeof s);
    int right = 0, total = 0, conf[HC_N][HC_N] = {{0}};
    const int causes[] = {HC_SHADER, HC_TEXTURE, HC_MAIN_BLOCK, HC_GAME, HC_FENCE, HC_GPU, HC_FT_WORK, HC_CORE};
    for (uint64_t f = 1; f <= 60000; f++) {
        if (f % 3000 == 1) { /* a new scene: its normal */ }
        double scene = 12 + (double)((f / 3000) % 4);   // 12-15 ms scenes
        uit_hrec r = normal(f);
        r.fr = (float)(scene + ur(-0.5, 0.5)); r.ft_main = (float)(scene - 4 + ur(-0.3, 0.3)); r.ft_block = r.ft_main + 0.5f;
        r.mn_block = (float)ur(2.5, 3.5); r.gpu = (float)(scene - 4 + ur(-1, 1)); r.ft_fence = (float)ur(0.5, 1.5);
        int want = -1;
        if (xr() % 12 == 0) {
            want = causes[xr() % (sizeof causes / sizeof *causes)];
            double ex = ur(5, 25), main_part = 0;
            r.fr += (float)ex;
            switch (want) {
            case HC_SHADER: r.sh_ms = (float)(ex * ur(0.9, 1.1)); main_part = ex * ur(0.5, 0.9); r.mn_block += (float)main_part; break;
            case HC_TEXTURE: r.tex_ms = (float)(ex * ur(0.9, 1.1)); main_part = ex * ur(0.6, 0.9); break;
            case HC_MAIN_BLOCK: main_part = ex; r.mn_block += (float)(ex * ur(0.8, 1.0)); break;
            case HC_GAME: main_part = ex; r.mn_block += (float)(ex * ur(0, 0.2)); break;
            case HC_FENCE: r.ft_fence += (float)(ex * ur(0.8, 1.0)); break;
            case HC_GPU: r.gpu += (float)(ex * ur(1.0, 1.2)); r.ft_fence += (float)(ex * ur(0.5, 0.8)); break;
            case HC_FT_WORK: break;   // the frame thread running longer
            case HC_CORE: r.ft_core += (float)(ex * ur(0.8, 1.0)); break;
            }
            r.ft_main += (float)main_part; r.ft_block += (float)main_part;
        }
        int h = uit_h_frame(&s, &r);
        if (want >= 0 && h && r.ex >= 3) { total++; if (r.cause == want) right++; conf[want][r.cause]++; }
        if (s.w_frames >= 300) uit_h_window_reset(&s);
    }
    double share = total ? (double)right / total : 0;
    CHECK(total > 3000 && share >= 0.95, "model: %d of %d hitch frames given the right cause (%.1f%%)", right, total, 100 * share);
    if (share < 0.95)
        for (int a = 0; a < HC_N; a++) for (int c = 0; c < HC_N; c++) if (conf[a][c] && a != c) printf("  %s -> %s: %d\n", uit_hc_name[a], uit_hc_name[c], conf[a][c]);
    printf("model: %d of %d (%.1f%%)\n", right, total, 100 * share);
}

int main(void) {
    neg = getenv("TOTK_PROBE_NEGATIVE") && *getenv("TOTK_PROBE_NEGATIVE") == '1';
    t_hw(); t_classify(); t_state(); t_lines(); t_model();
    if (fails) { printf("test_hitch: %d FAILED%s\n", fails, neg ? " (negative probe: expected)" : ""); return neg ? 0 : 1; }
    if (neg) { printf("test_hitch: negative probe NOT caught\n"); return 1; }
    printf("test_hitch: all passed\n");
    return 0;
}
