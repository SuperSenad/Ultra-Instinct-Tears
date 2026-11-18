// Box test for [UIT 0441] the split of the game's long fence waits and [UIT 0442] TOTK_VK_FENCE_EARLY's targets
// (src/gfx/vkb_fwait.h, used by backend_vk.c and hle_nvn.c nvnSyncWait):
//   cc -O2 -Wall -Isrc/gfx tools/tests/test_fwait.c -o /tmp/t/fwait && /tmp/t/fwait
// 1) the ring: a record is found only for its own fence (not before its commit, not once the ring passed it); a time
//    set for a fence the ring no longer holds is dropped; a new commit clears the times.
// 2) fw_split: the parts always add up to the wait; each part is the stretch between its two marks; marks before the
//    wait (the GPU was done before the translation thread) give 0; a missing time is 0 and never negative; marks after
//    the wait returned are cut at its end.
// 3) fw_bucket: the edges 1, 2, 4, 8 ms.
// 4) [UIT 0442] marks: a touch counts for its own submission number only; fw_touched over a span; too long a span is
//    touched. Recordings that overlap (begin / touch / commit interleaved at random, numbers taken as the backend does:
//    the last fence + 1) never lose a touch: every recording that touched commits with FW_GM.
// 5) fw_target, cases: done already; no frame before; the newest marked; one in the middle marked; none marked (the
//    frame before, or what is done); a record missing; too many to look at.
// 6) The model: frames of 1-4 submissions with random marks, a GPU that finishes them in order at random times, a game
//    that waits for its frame's last fence each frame and then writes game memory and reads results. Whenever a wait
//    returns, every submission up to its fence that is marked, and every one of the frame before, is complete (so the
//    game never writes memory the GPU still uses, never reads a result not copied back, and is at most a frame ahead);
//    and the early return happens (the test fails if it never ends a wait before its fence).
// TOTK_PROBE_NEGATIVE=1: fw_split without the clamp to the wait's end, and a target that ignores FW_GM - the sum check
// and the model's check must fail.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "vkb_fwait.h"

static int fails, checks;
#define CHECK(c) do { checks++; if (!(c)) { if (fails++ < 10) printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); } } while (0)
static fw_ring g_r;
static uint64_t rng = 88172645463325252ull;
static uint64_t rnd(uint64_t n) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return rng % n; }

static void split_neg(uint64_t t0, uint64_t t1, uint64_t tx, const uint64_t t[FW_T_N], uint64_t seg[FW_SEG_N]) {
    uint64_t m[FW_SEG_N + 1] = {t0}, in[FW_SEG_N - 1] = {tx, t[0], t[1], t[2]};
    for (int i = 0; i < FW_SEG_N - 1; i++) m[i + 1] = in[i] > m[i] ? in[i] : m[i];   // no cut at t1
    m[FW_SEG_N] = t1 > m[FW_SEG_N - 1] ? t1 : m[FW_SEG_N - 1];
    for (int i = 0; i < FW_SEG_N; i++) seg[i] = m[i + 1] - m[i];
}

// the backend's numbering: touches during a recording go to the last fence + 1
static uint64_t m_fence;
typedef struct { int open, touched; uint64_t begin; } rec;
static uint64_t target_neg(fw_ring *r, uint64_t f, uint64_t done, uint64_t prev, int *why) {   // ignores FW_GM
    *why = FW_WHY_EARLY;
    if (f <= done) return f;
    if (!prev || prev >= f) { *why = FW_WHY_FRAME; return f; }
    uint64_t lo = prev > done ? prev : done;
    for (uint64_t s = f; s > lo; s--) { uint64_t fl; if (!fw_flags(r, s, &fl) || (fl & FW_RB)) return s; }
    return lo;
}

int main(void) {
    int neg = getenv("TOTK_PROBE_NEGATIVE") && atoi(getenv("TOTK_PROBE_NEGATIVE")) == 1;
    uint64_t t[FW_T_N];
    // 1) ring
    CHECK(!fw_times(&g_r, 5, t));
    fw_time(&g_r, 5, FW_T_SUB, 100);   // before its commit: dropped
    fw_commit(&g_r, 5, 0);
    CHECK(fw_times(&g_r, 5, t) && t[FW_T_SUB] == 0 && t[FW_T_GPU] == 0 && t[FW_T_DONE] == 0);
    fw_time(&g_r, 5, FW_T_SUB, 100); fw_time(&g_r, 5, FW_T_GPU, 200); fw_time(&g_r, 5, FW_T_DONE, 300);
    CHECK(fw_times(&g_r, 5, t) && t[FW_T_SUB] == 100 && t[FW_T_GPU] == 200 && t[FW_T_DONE] == 300);
    fw_commit(&g_r, 5 + FW_RING, 0);   // the ring passed 5
    CHECK(!fw_times(&g_r, 5, t));
    fw_time(&g_r, 5, FW_T_GPU, 999);   // a late time of 5: dropped, 5 + FW_RING untouched
    CHECK(fw_times(&g_r, 5 + FW_RING, t) && t[FW_T_GPU] == 0);
    for (uint64_t f = 1; f < 3 * FW_RING; f++) { fw_commit(&g_r, f, 0); fw_time(&g_r, f, FW_T_DONE, f * 10); }
    int okall = 1;
    for (uint64_t f = 2 * FW_RING; f < 3 * FW_RING; f++) okall &= fw_times(&g_r, f, t) && t[FW_T_DONE] == f * 10;
    for (uint64_t f = 1; f < 2 * FW_RING; f++) okall &= !fw_times(&g_r, f, t);
    CHECK(okall);
    // 2) split: exact cases
    uint64_t seg[FW_SEG_N];
    uint64_t tt[FW_T_N] = {12, 20, 23};
    fw_split(10, 25, 11, tt, seg);   // x 1, submit 1, GPU 8, copy back 3, wake-up 2
    CHECK(seg[FW_SEG_X] == 1 && seg[FW_SEG_SUB] == 1 && seg[FW_SEG_GPU] == 8 && seg[FW_SEG_CB] == 3 && seg[FW_SEG_WAKE] == 2);
    uint64_t early[FW_T_N] = {2, 4, 0};   // submitted and GPU-done before the wait, complete time missing
    fw_split(10, 30, 10, early, seg);
    CHECK(seg[FW_SEG_X] == 0 && seg[FW_SEG_SUB] == 0 && seg[FW_SEG_GPU] == 0 && seg[FW_SEG_CB] == 0 && seg[FW_SEG_WAKE] == 20);
    uint64_t late[FW_T_N] = {12, 50, 60};   // marks after the return (a racy read): cut at the end
    fw_split(10, 30, 10, late, seg);
    CHECK(seg[FW_SEG_SUB] == 2 && seg[FW_SEG_GPU] == 18 && seg[FW_SEG_CB] == 0 && seg[FW_SEG_WAKE] == 0);
    // 2) split: random, the parts add up and none is "negative" (wrapped)
    int sum_ok = 1, range_ok = 1;
    for (int i = 0; i < 200000; i++) {
        uint64_t t0 = 1000 + rnd(1000), t1 = t0 + rnd(20000), tx = rnd(3) ? t0 + rnd(25000) : rnd(t0);
        uint64_t r3[FW_T_N];
        for (int k = 0; k < FW_T_N; k++) r3[k] = rnd(5) ? rnd(30000) : 0;
        if (neg) split_neg(t0, t1, tx, r3, seg); else fw_split(t0, t1, tx, r3, seg);
        uint64_t s = 0;
        for (int k = 0; k < FW_SEG_N; k++) { s += seg[k]; if (seg[k] > t1 - t0) range_ok = 0; }
        if (s != t1 - t0) sum_ok = 0;
    }
    CHECK(sum_ok);
    CHECK(range_ok);
    // 3) buckets
    CHECK(fw_bucket(0) == 0 && fw_bucket(999999) == 0 && fw_bucket(1000000) == 1 && fw_bucket(1999999) == 1);
    CHECK(fw_bucket(2000000) == 2 && fw_bucket(3999999) == 2 && fw_bucket(4000000) == 3 && fw_bucket(7999999) == 3);
    CHECK(fw_bucket(8000000) == 4 && fw_bucket(30000000) == 4);
    // 4) marks
    static fw_ring r2;
    fw_touch(&r2, 10);
    CHECK(fw_touched(&r2, 10, 10) && !fw_touched(&r2, 11, 11) && !fw_touched(&r2, 9, 9) && fw_touched(&r2, 8, 12));
    CHECK(!fw_touched(&r2, 10 + FW_RING, 10 + FW_RING));   // a number the ring reuses: not the old touch
    CHECK(fw_touched(&r2, 100, 100 + FW_SCAN) && !fw_touched(&r2, 100, 100 + FW_SCAN - 1) && !fw_touched(&r2, 5, 4));
    {   // overlapping recordings
        static fw_ring r3;
        int lost = 0, marked = 0;
        for (int round = 0; round < 20000; round++) {
            rec a[3] = {{0}};
            int alive = 0;
            for (int step = 0; step < 40; step++) {
                int i = (int)rnd(3);
                rec *x = &a[i];
                int op = (int)rnd(3);
                if (!x->open) { if (alive < 2 || rnd(4) == 0) { x->open = 1; x->touched = 0; x->begin = m_fence + 1; alive++; } continue; }
                if (op == 0) { fw_touch(&r3, m_fence + 1); x->touched = 1; }
                else if (op == 1) {
                    uint64_t f = ++m_fence;
                    if (rnd(4) == 0) { fw_touch(&r3, m_fence + 1); x->touched = 1; }   // written at commit (mirrors)
                    uint64_t fl = fw_touched(&r3, x->begin, f + 1) ? FW_GM : 0;
                    fw_commit(&r3, f, fl);
                    if (x->touched && !(fl & FW_GM)) lost++;
                    marked += (fl & FW_GM) != 0;
                    x->open = 0; alive--;
                }
            }
        }
        CHECK(lost == 0);
        CHECK(marked > 0);
    }
    // 5) fw_target cases
    {
        static fw_ring r4;
        int why;
        for (uint64_t f = 1; f <= 40; f++) fw_commit(&r4, f, 0);
        fw_commit(&r4, 25, FW_GM); fw_commit(&r4, 28, FW_RB);
        CHECK(fw_target(&r4, 20, 22, 18, &why) == 20 && why == FW_WHY_EARLY);   // done already
        CHECK(fw_target(&r4, 30, 10, 0, &why) == 30 && why == FW_WHY_FRAME);   // no frame before known
        CHECK(fw_target(&r4, 28, 10, 20, &why) == 28 && why == FW_WHY_RB);   // the newest has results
        CHECK(fw_target(&r4, 25, 10, 20, &why) == 25 && why == FW_WHY_GM);   // the newest uses game memory
        CHECK(fw_target(&r4, 32, 10, 20, &why) == 28 && why == FW_WHY_RB);   // one in the middle
        CHECK(fw_target(&r4, 27, 10, 20, &why) == 25 && why == FW_WHY_GM);
        CHECK(fw_target(&r4, 40, 10, 30, &why) == 30 && why == FW_WHY_EARLY);   // none after the frame before
        CHECK(fw_target(&r4, 40, 33, 30, &why) == 33 && why == FW_WHY_EARLY);   // none after what is done
        CHECK(fw_target(&r4, 24, 10, 22, &why) == 22 && why == FW_WHY_EARLY);
        CHECK(fw_target(&r4, 41, 10, 30, &why) == 41 && why == FW_WHY_UNKNOWN);   // 41 not committed yet
        fw_commit(&r4, 36 + FW_RING, 0);   // 36's record overwritten
        CHECK(fw_target(&r4, 40, 10, 30, &why) == 36 && why == FW_WHY_UNKNOWN);
        CHECK(fw_target(&r4, 1000, 10, 900, &why) == 1000 && why == FW_WHY_UNKNOWN);   // over FW_SCAN to look at
    }
    // 6) the model
    {
        static fw_ring r5;
        enum { FRAMES = 20000 };
        uint64_t fence = 0, done = 0, pf[8] = {0};
        static uint64_t frame_of[FRAMES * 4 + 8], flags_of[FRAMES * 4 + 8];
        static double fin[FRAMES * 4 + 8];
        double gpu_t = 0, now = 0;
        int bad = 0, early_n = 0, ahead_bad = 0;
        for (int n = 1; n < FRAMES; n++) {
            int m = 1 + (int)rnd(4);
            for (int i = 0; i < m; i++) {
                uint64_t f = ++fence, fl = 0;
                if (rnd(100) < 45) fl |= FW_GM;
                if (rnd(100) < 30) fl |= FW_RB;
                fw_commit(&r5, f, fl);
                frame_of[f] = (uint64_t)n; flags_of[f] = fl;
                gpu_t = (gpu_t > now ? gpu_t : now) + 0.5 + (double)rnd(1000) / 1000.0 * (rnd(10) ? 2.0 : 9.0) / m;   // in order, jittery
                fin[f] = gpu_t;
            }
            pf[n % 8] = fence;   // the present of frame n handled after its submissions
            now += 1.0 + (double)rnd(1000) / 1000.0 * 1.5;   // the game's own work
            while (done < fence && fin[done + 1] <= now) done++;
            uint64_t prev = 0;
            for (int i = 0; i < 8; i++) if (pf[i] < fence && pf[i] > prev) prev = pf[i];
            int why;
            uint64_t tg = neg ? target_neg(&r5, fence, done, prev, &why) : fw_target(&r5, fence, done, prev, &why);
            if (tg > fence) bad++;
            if (tg < fence) early_n++;
            if (tg > done) { now = fin[tg] > now ? fin[tg] : now; while (done < tg) done++; }   // the wait returns
            while (done < fence && fin[done + 1] <= now) done++;
            // the check: everything not complete now must be unmarked and of this frame
            for (uint64_t s = done + 1; s <= fence; s++) {
                if (flags_of[s]) bad++;
                if (frame_of[s] != (uint64_t)n) ahead_bad++;
            }
        }
        CHECK(bad == 0);
        CHECK(ahead_bad == 0);
        CHECK(early_n > FRAMES / 20);   // the switch does something in this model
        if (!neg) printf("model: %d of %d frame waits ended before their fence\n", early_n, FRAMES - 1);
    }
    printf("%s: %d checks, %d failed%s\n", fails ? "FAIL" : "ok", checks, fails, neg ? " (TOTK_PROBE_NEGATIVE=1)" : "");
    return fails != 0;
}
