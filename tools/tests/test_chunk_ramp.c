// [UIT 0241] Box test of TOTK_VK_CHUNK_RAMP (src/gfx/vkb_chunk_ramp.h) with the real stream plan (src/gfx/vkb_early.h).
//  1. parsing and limits: unset = 200, 0 / off = off, small values clamped to 100; off or split 0 = every chunk as before.
//  2. cutting: the walk's rule from backend_vk.c walk_op over a recorded frame shape; ramp off gives exactly the old
//     chunks; ramp on only cuts more at the start, never more than MT_MAXCH, and every draw is in exactly one chunk.
//  3. a model of one Lookout Landing submission (the 2026-10-31 20:48 log: 6,273 draws, 246 passes, walk 0.8 ms,
//     prologue 1.2 ms, chunks 4.6 us a draw on average and 8-9 us in the first 1,200 draws, 6 recording threads + the
//     translation thread after the walk, GPU 11 ms spread over the draws): when the GPU gets its first part and when it
//     is done, ramp off vs on (the stream gets vkb_ramp_parts: 14 parts with the ramp, 8 without); also a frame with even
//     draw costs, where the ramp must not lose. A model, not the game: the log line "[UIT 0236] ... early parts" tells.
// cc -O2 -Wall -Werror -Isrc/gfx tools/tests/test_chunk_ramp.c -o /tmp/t && /tmp/t
#include "vkb_chunk_ramp.h"
#include "vkb_early.h"
#include <stdio.h>
#include <string.h>

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL: " __VA_ARGS__); printf("\n"); } } while (0)
enum { MT_MAXCH = 96, MT_DRAWS = 400, NDRAW = 6273, NTHR = 7 };

// the frame: per command, a draw or a render target change
static unsigned char is_rt[NDRAW + 300];
static double cost_us[NDRAW + 300];
static int nops;
static int UNIFORM;   // 1: every draw costs the average (no heavy start): the ramp must not make things worse
static void make_frame(void) {
    unsigned s = 12345;
    nops = 0;
    int draws = 0;
    while (draws < NDRAW) {
        s = s * 1103515245u + 12345u;
        if (nops && (s >> 16) % 26 == 0) { is_rt[nops++] = 1; continue; }   // ~246 passes
        double c = UNIFORM ? 4.6 : draws < 1200 ? 8.6 : 3.8;   // the heavy shadow / G-buffer draws first
        cost_us[nops] = c * (0.5 + ((s >> 8) & 255) / 255.0);
        is_rt[nops++] = 0;
        draws++;
    }
}
// walk_op: chunk boundaries (op index where each chunk starts); returns the chunk count
static int cut(int split, int ramp, int *start, int *draws) {
    int nch = 0, d = 0;
    start[0] = 0;
    for (int i = 0; i < nops; i++) {
        uint64_t lim = vkb_chunk_limit(nch, split, ramp);
        if (nch < MT_MAXCH - 1 && ((is_rt[i] && d >= MT_DRAWS) || (split && !is_rt[i] && (uint64_t)d >= lim))) {
            draws[nch++] = d; start[nch] = i; d = 0;
        }
        if (!is_rt[i]) d++;
    }
    draws[nch++] = d;
    return nch;
}

// the model: chunk k is cut during the walk at the time its first op is reached; threads take chunks in order; the
// stream plan sends finished prefixes (after the walk); the GPU runs the parts in order
static void model(int split, int ramp, double *first_ms, double *done_ms, int *nchunks) {
    static int start[MT_MAXCH + 1], dr[MT_MAXCH];
    int nch = cut(split, ramp, start, dr);
    start[nch] = nops;
    const double pro = 1.2, walk = 0.8, gpu_total = 11.0, glue_us = 15.0;
    double rec[MT_MAXCH], cutt[MT_MAXCH], fin[MT_MAXCH], gpu[MT_MAXCH];
    for (int k = 0; k < nch; k++) {
        rec[k] = 0;
        for (int i = start[k]; i < start[k + 1]; i++) rec[k] += cost_us[i] / 1000.0;
        cutt[k] = pro + walk * (double)start[k + 1] / nops;   // handed out when closed
        gpu[k] = gpu_total * dr[k] / NDRAW + glue_us / 1000.0;
    }
    double free_at[NTHR];
    for (int t = 0; t < NTHR; t++) free_at[t] = t == 0 ? pro + walk : pro;   // thread 0: the translation thread
    for (int k = 0; k < nch; k++) {   // the earliest free thread takes the next chunk once cut
        int b = 0;
        for (int t = 1; t < NTHR; t++) if (free_at[t] < free_at[b]) b = t;
        double s = free_at[b] > cutt[k] ? free_at[b] : cutt[k];
        fin[k] = s + rec[k];
        free_at[b] = fin[k];
    }
    // events in time order: each chunk's finish (and the walk's end) is a chance to send (vkb_early_stream_plan)
    double gpu_free = 0, first = -1;
    int next = 0, sent = 0;
    unsigned char done[MT_MAXCH];
    uint64_t d64[MT_MAXCH];
    for (int k = 0; k < nch; k++) d64[k] = (uint64_t)dr[k];
    double ev[MT_MAXCH + 1];
    int nev = 0;
    for (int k = 0; k < nch; k++) ev[nev++] = fin[k];
    ev[nev++] = pro + walk;
    for (int i = 1; i < nev; i++) for (int j = i; j > 0 && ev[j] < ev[j - 1]; j--) { double t = ev[j]; ev[j] = ev[j - 1]; ev[j - 1] = t; }
    for (int e = 0; e < nev; e++) {
        double now = ev[e];
        if (now < pro + walk) continue;   // the gate opens at the walk's end
        for (int k = 0; k < nch; k++) done[k] = fin[k] <= now;
        int to = vkb_early_stream_plan(next, nch, done, d64, vkb_ramp_parts(VKB_EARLY_STREAM_MAX, 1, ramp), sent, VKB_EARLY_MIN_DRAWS);
        if (to > next) {
            for (int k = next; k < to; k++) { double s = gpu_free > now ? gpu_free : now; if (first < 0) first = s; gpu_free = s + gpu[k]; }
            next = to; sent++;
        }
    }
    double last = 0;   // the last part: everything left, once all chunks are recorded
    for (int k = 0; k < nch; k++) if (fin[k] > last) last = fin[k];
    for (int k = next; k < nch; k++) { double s = gpu_free > last ? gpu_free : last; if (first < 0) first = s; gpu_free = s + gpu[k]; }
    *first_ms = first; *done_ms = gpu_free; *nchunks = nch;
}

int main(void) {
    CHECK(vkb_ramp_parse(NULL) == 200 && vkb_ramp_parse("") == 200 && vkb_ramp_parse("-") == 200, "default");
    CHECK(vkb_ramp_parse("0") == 0 && vkb_ramp_parse("off") == 0 && vkb_ramp_parse("OFF") == 0 && vkb_ramp_parse("-5") == 0, "off");
    CHECK(vkb_ramp_parse("50") == 100 && vkb_ramp_parse("300") == 300 && vkb_ramp_parse("x") == 200, "values");
    CHECK(vkb_chunk_limit(0, 600, 200) == 200 && vkb_chunk_limit(5, 600, 200) == 200 && vkb_chunk_limit(6, 600, 200) == 400 &&
          vkb_chunk_limit(7, 600, 200) == 600 && vkb_chunk_limit(40, 600, 200) == 600, "ramp 200 x 6, 400, 600, 600");
    CHECK(vkb_chunk_limit(6, 1000, 100) == 200 && vkb_chunk_limit(8, 1000, 100) == 800 && vkb_chunk_limit(9, 1000, 100) == 1000 &&
          vkb_chunk_limit(90, 1000, 100) == 1000, "ramp 100 to split 1000");
    CHECK(vkb_ramp_parts(8, 1, 200) == 14 && vkb_ramp_parts(8, 1, 0) == 8 && vkb_ramp_parts(3, 0, 200) == 3 &&
          vkb_ramp_parts(1, 0, 200) == 1, "parts: 6 more with the ramp and the stream only");
    CHECK(vkb_chunk_limit(0, 600, 0) == 600 && vkb_chunk_limit(0, 0, 200) == 0 && vkb_chunk_limit(0, 150, 200) == 150, "off / split 0 / ramp >= split");
    make_frame();
    static int s0[MT_MAXCH + 1], d0[MT_MAXCH], s1[MT_MAXCH + 1], d1[MT_MAXCH];
    // ramp off: the old rule exactly (vkb_chunk_limit == split for every chunk)
    int n0 = cut(600, 0, s0, d0), n1 = cut(600, 200, s1, d1);
    int sum0 = 0, sum1 = 0;
    for (int k = 0; k < n0; k++) sum0 += d0[k];
    for (int k = 0; k < n1; k++) sum1 += d1[k];
    CHECK(sum0 == NDRAW && sum1 == NDRAW, "every draw in one chunk (%d, %d)", sum0, sum1);
    CHECK(n1 >= n0 && n1 <= n0 + 6 && n1 <= MT_MAXCH, "ramp adds at most a few chunks (%d -> %d)", n0, n1);
    CHECK(d1[0] <= 200 + 0 && d1[0] >= 1, "first chunk at most 200 draws (%d)", d1[0]);
    int tiny = 1;   // [the same count at split 0: chunks cut at target changes only, ramp off]
    int n2 = cut(0, 200, s1, d1), n3 = cut(0, 0, s0, d0);
    CHECK(n2 == n3, "split 0: ramp changes nothing (%d vs %d)", n2, n3);
    (void)tiny;
    double f0, e0, f1, e1;
    int c0, c1;
    model(600, 0, &f0, &e0, &c0);
    model(600, 200, &f1, &e1, &c1);
    printf("model, Lookout Landing submission: ramp off: %d chunks, GPU starts at %.2f ms, done at %.2f ms\n", c0, f0, e0);
    printf("model, Lookout Landing submission: ramp 200: %d chunks, GPU starts at %.2f ms, done at %.2f ms (%.2f ms sooner)\n", c1, f1, e1, e0 - e1);
    CHECK(f1 < f0 - 1.0, "the ramp does not start the GPU 1 ms sooner in the model");
    CHECK(e1 < e0 - 1.0, "the ramp does not finish the GPU 1 ms sooner in the model");
    for (int r = 100; r <= 400; r += 100) { double a, b; int c; model(600, r, &a, &b, &c); printf("  ramp %d: %d chunks, GPU start %.2f, done %.2f ms\n", r, c, a, b); }
    UNIFORM = 1;   // a frame without the heavy start
    make_frame();
    model(600, 0, &f0, &e0, &c0);
    model(600, 200, &f1, &e1, &c1);
    printf("model, even draw costs: ramp off: %d chunks, GPU %.2f -> %.2f ms; ramp 200: %d chunks, GPU %.2f -> %.2f ms\n", c0, f0, e0, c1, f1, e1);
    CHECK(e1 <= e0 + 0.1, "the ramp makes an even frame slower in the model");
    printf(fails ? "FAIL (%d)\n" : "ok\n", fails);
    return fails != 0;
}
