// Box test of vkb_early.h and a model of the TOTK_VK_EARLY_PARTS protocol (backend_vk.c vkb_mt_submit / early_try):
// chunks finish recording in random order, each asks for prologue work at random moments (into the segment open
// then); early parts are cut by vkb_early_plan. Checked: every chunk submitted once and in order, never before it is
// recorded; each chunk's prologue work is in a segment submitted before it; at most parts-1 early parts; the fence is
// signalled by the last part only, after everything; parts = 1 is one submission as before. [UIT 0236] The same over
// vkb_early_stream_plan (TOTK_VK_EARLY_STREAM), plus its unit cases and vkb_early_mode; the threads are in
// test_early_stream.c.
// cc -O1 -Wall -Isrc/gfx tools/tests/test_early.c -o /tmp/test_early && /tmp/test_early
#include "vkb_early.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; if (fails < 20) { printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } } while (0)

enum { MAXCH = 96, MAXQ = 512 };
typedef struct { int kind, id; } qent;   // kind 0: prologue segment id, 1: chunk id, 2: fence signal
static uint64_t rng = 88172645463325252ull;
static uint64_t rnd(void) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return rng; }

static void run(int parts, int nch, int stream) {   // stream: [UIT 0236] vkb_early_stream_plan, parts = the most
    unsigned char done[MAXCH] = {0};
    uint64_t draws[MAXCH], total = 0;
    int need_seg[MAXCH];   // the latest prologue segment the chunk put work in (-1: none)
    for (int k = 0; k < nch; k++) { draws[k] = rnd() % 900; total += draws[k]; need_seg[k] = -1; }
    int order[MAXCH];
    for (int k = 0; k < nch; k++) order[k] = k;
    for (int k = nch - 1; k > 0; k--) { int j = (int)(rnd() % (uint64_t)(k + 1)); int t = order[k]; order[k] = order[j]; order[j] = t; }
    qent q[MAXQ]; int nq = 0, seg = 0, next = 0, sent = 0, submits = 0;
    for (int step = 0; step < nch; step++) {
        int k = order[step];
        int works = (int)(rnd() % 3);   // prologue work while recording: into the segment open now
        for (int w = 0; w < works; w++) need_seg[k] = seg;
        // chunks still recording (their recording may straddle a cut): work into the segment open now
        for (int w = 0; w < 2; w++) { int o = order[step + (int)(rnd() % (uint64_t)(nch - step))]; if (!done[o]) need_seg[o] = seg; }
        done[k] = 1;
        int to = stream ? vkb_early_stream_plan(next, nch, done, draws, parts, sent, VKB_EARLY_MIN_DRAWS)
                        : vkb_early_plan(next, nch, done, draws, total, parts, sent, VKB_EARLY_MIN_DRAWS);
        if (stream && to > next) {   // a stream part: the finished prefix, >= min draws, never the last chunk
            uint64_t cum = 0;
            for (int c = next; c < to; c++) cum += draws[c];
            CHECK(cum >= VKB_EARLY_MIN_DRAWS, "stream part of %llu draws", (unsigned long long)cum);
            CHECK(to < nch, "stream part took the last chunk");
            CHECK(to == nch || !done[to], "stream part stopped before a recorded chunk");
        }
        CHECK(to >= next, "plan went back");
        if (to > next) {   // send: the open segment, then chunks [next, to); a new segment opens
            for (int c = next; c < to; c++) CHECK(done[c], "chunk %d sent before it was recorded", c);
            q[nq++] = (qent){0, seg};
            for (int c = next; c < to; c++) q[nq++] = (qent){1, c};
            seg++; next = to; sent++; submits++;
        }
    }
    // the last part: the open segment, the rest, the fence
    q[nq++] = (qent){0, seg};
    for (int c = next; c < nch; c++) q[nq++] = (qent){1, c};
    q[nq++] = (qent){2, 0};
    submits++;
    CHECK(sent <= parts - 1, "%d early parts with parts=%d", sent, parts);
    if (parts == 1) CHECK(submits == 1, "parts=1 made %d submissions", submits);
    // checks over the queue order
    int pos_seg[MAXQ], last_chunk = -1, nfence = 0;
    for (int i = 0; i < MAXQ; i++) pos_seg[i] = -1;
    for (int i = 0; i < nq; i++) {
        if (q[i].kind == 0) pos_seg[q[i].id] = i;
        else if (q[i].kind == 1) {
            CHECK(q[i].id == last_chunk + 1, "chunk %d after %d", q[i].id, last_chunk);
            last_chunk = q[i].id;
            int s = need_seg[q[i].id];
            CHECK(s < 0 || (pos_seg[s] >= 0 && pos_seg[s] < i), "chunk %d needs segment %d not in front of it", q[i].id, s);
        } else { nfence++; CHECK(i == nq - 1, "fence not last"); }
    }
    CHECK(last_chunk == nch - 1, "chunks missing: last %d of %d", last_chunk, nch);
    CHECK(nfence == 1, "fence signalled %d times", nfence);
}

int main(void) {
    // plan unit cases
    unsigned char d[4] = {1, 1, 0, 0};
    uint64_t dr[4] = {300, 300, 300, 300};
    CHECK(vkb_early_plan(0, 4, d, dr, 1200, 1, 0, 200) == 0, "parts=1 sends nothing early");
    CHECK(vkb_early_plan(0, 4, d, dr, 1200, 2, 0, 200) == 2, "half recorded: first part sent");
    CHECK(vkb_early_plan(0, 4, d, dr, 1200, 2, 1, 200) == 0, "parts=2: one early part only");
    CHECK(vkb_early_plan(0, 4, d, dr, 1200, 3, 0, 200) == 2, "parts=3: first third");
    CHECK(vkb_early_plan(2, 4, d, dr, 1200, 3, 1, 200) == 2, "nothing new recorded");
    unsigned char all[4] = {1, 1, 1, 1};
    CHECK(vkb_early_plan(0, 4, all, dr, 1200, 3, 0, 200) == 0, "all recorded: the last part takes it all");
    unsigned char gap[4] = {0, 1, 1, 0};
    CHECK(vkb_early_plan(0, 4, gap, dr, 1200, 2, 0, 200) == 0, "first chunk not recorded: nothing");
    uint64_t small[4] = {50, 50, 50, 50};
    CHECK(vkb_early_plan(0, 4, d, small, 200, 2, 0, 200) == 0, "too small a part");
    CHECK(vkb_early_parts_parse(NULL) == 1 && vkb_early_parts_parse("-") == 1 && vkb_early_parts_parse("2") == 2 &&
          vkb_early_parts_parse("3") == 3 && vkb_early_parts_parse("9") == 3 && vkb_early_parts_parse("0") == 1 &&
          vkb_early_parts_parse("x") == 1 && vkb_early_parts_parse("") == 1, "parse");
    // [UIT 0236] stream plan unit cases
    CHECK(vkb_early_stream_plan(0, 4, d, dr, 8, 0, 200) == 2, "stream: the finished prefix goes");
    CHECK(vkb_early_stream_plan(0, 4, d, dr, 1, 0, 200) == 0, "stream: 1 part sends nothing early");
    CHECK(vkb_early_stream_plan(0, 4, d, dr, 8, 7, 200) == 0, "stream: 7 early parts of 8 is the most");
    CHECK(vkb_early_stream_plan(2, 4, d, dr, 8, 1, 200) == 2, "stream: nothing new recorded");
    CHECK(vkb_early_stream_plan(0, 4, all, dr, 8, 0, 200) == 0, "stream: all recorded: the last part takes it all");
    CHECK(vkb_early_stream_plan(0, 4, gap, dr, 8, 0, 200) == 0, "stream: first chunk not recorded: nothing");
    CHECK(vkb_early_stream_plan(0, 4, d, small, 8, 0, 200) == 0, "stream: too small a part");
    unsigned char one[4] = {1, 0, 1, 0};
    CHECK(vkb_early_stream_plan(0, 4, one, dr, 8, 0, 200) == 1, "stream: one finished chunk of 300 draws is enough (no share of the total)");
    unsigned char three[4] = {1, 1, 1, 0};
    CHECK(vkb_early_stream_plan(1, 4, three, dr, 8, 1, 200) == 3, "stream: from where the last part ended");
    int st;
    CHECK(vkb_early_mode(NULL, NULL, &st) == VKB_EARLY_STREAM_MAX && st == 1, "mode: default is the stream");
    CHECK(vkb_early_mode("-", "-", &st) == VKB_EARLY_STREAM_MAX && st == 1, "mode: launcher automatic is the stream");
    CHECK(vkb_early_mode(NULL, "0", &st) == 1 && st == 0, "mode: stream 0 is one part");
    CHECK(vkb_early_mode("2", NULL, &st) == 2 && st == 0, "mode: parts 2 set explicitly is the old plan");
    CHECK(vkb_early_mode("3", "0", &st) == 3 && st == 0, "mode: parts 3 with stream 0");
    CHECK(vkb_early_mode("1", "1", &st) == VKB_EARLY_STREAM_MAX && st == 1, "mode: parts 1 leaves the stream on");
    CHECK(vkb_early_stream_parse(NULL) && vkb_early_stream_parse("") && vkb_early_stream_parse("1") && vkb_early_stream_parse("-") &&
          !vkb_early_stream_parse("0") && vkb_early_stream_parse("00"), "stream parse");
    for (int it = 0; it < 20000; it++) run(1 + it % 3, 1 + (int)(rnd() % MAXCH), 0);
    for (int it = 0; it < 20000; it++) run(it % 9 ? VKB_EARLY_STREAM_MAX : 1 + it % 8, 1 + (int)(rnd() % MAXCH), 1);
    if (fails) { printf("%d failures\n", fails); return 1; }
    printf("test_early: ok\n");
    return 0;
}
