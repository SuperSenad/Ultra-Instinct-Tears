// [UIT 0027] Box test for the GPU-write checks of the copies taken at the submit call (src/gfx/vkb_gpuw.c).
//   gcc -O2 -Wall tools/tests/test_ucap_gpuw.c src/gfx/vkb_gpuw.c -Isrc/gfx -o /tmp/tgw && /tmp/tgw
// A pipeline where the game submits ahead of the translation thread and the GPU completes submissions in between:
// a copy may be served only if no GPU write it predates overlaps it (the GPU timeline would show that write), and a
// vertex / index copy also not after a recent GPU write (whose bytes may exist only in the pool's device copy).
// The 0018 logic (pruned by the replay's completion point) is run beside it: it serves stale copies.
// 4 (0038): GPU-written vertex ranges with no forgetting window (TOTK_GEO_GPU_KEEP unset).
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "vkb_gpuw.h"

enum { KEEP = 600, NR = 64 };
typedef struct { uint64_t lo, hi, fence; } w_t;
static w_t old_p[1 << 16];
static int old_n;
static int old_check(uint64_t lo, uint64_t hi, uint64_t cdone, uint64_t done_now) {   // vkb_ucap.c before 0027
    int k = 0;
    for (int i = 0; i < old_n; i++) if (old_p[i].fence > done_now) old_p[k++] = old_p[i];
    old_n = k;
    for (int j = 0; j < old_n; j++) if (old_p[j].fence > cdone && old_p[j].lo < hi && lo < old_p[j].hi) return 1;
    return 0;
}

static uint64_t rnd(void) { static uint64_t x = 88172645463325252ull; x ^= x << 13; x ^= x >> 7; x ^= x << 17; return x; }

static void each_cb(void *ctx, uint64_t lo, uint64_t hi) {   // [UIT 0157] a listed write must lie in the asked range
    struct { uint64_t lo, hi; int bad; } *c = ctx;
    if (lo < c->lo || hi > c->hi || hi <= lo) c->bad = 1;
}

int main(void) {
    int fails = 0;
    // 1. The case behind torn meshes: S10 copies into vertex buffer X on the GPU; the game submits S11 (drawing X) while
    //    S10 is still queued (capture: done = 9); by the replay of S11 the GPU has completed S10 (done = 10).
    {
        gpuw_set s = {0};
        old_n = 0;
        gpuw_add(&s, 0x1000, 0x2000, 10); old_p[old_n++] = (w_t){0x1000, 0x2000, 10};
        uint64_t cdone = 9, now = 10;
        gpuw_retire(&s, cdone, KEEP);
        int nw = gpuw_inflight(&s, 0x1800, 0x1900, cdone), od = old_check(0x1800, 0x1900, cdone, now);
        printf("1. GPU write completed between capture and replay: 0018 %s, 0027 %s\n", od ? "reads memory" : "serves the stale copy", nw ? "reads memory" : "serves the stale copy");
        if (!nw) fails++;
    }
    // 2. A GPU write long completed whose bytes the CPU-wins merge left out of game memory: a vertex copy reads memory,
    //    a uniform copy (CPU-written data) keeps the copy; after KEEP submissions it is forgotten.
    {
        gpuw_set s = {0};
        gpuw_add(&s, 0x10000, 0x11000, 5);
        gpuw_retire(&s, 20, KEEP);
        int g = gpuw_recent(&s, 0x10800, 0x10900, 20, KEEP), u = gpuw_inflight(&s, 0x10800, 0x10900, 20);
        gpuw_retire(&s, 5 + KEEP, KEEP);
        int late = gpuw_recent(&s, 0x10800, 0x10900, 5 + KEEP, KEEP), other = 0;
        gpuw_add(&s, 0x20000, 0x21000, 700);
        gpuw_retire(&s, 800, KEEP);
        other = gpuw_recent(&s, 0x30000, 0x30100, 800, KEEP);
        printf("2. vertex range GPU-written 15 submissions ago: %s; as a uniform range: %s; %d submissions later: %s; an unwritten range: %s\n",
               g ? "reads memory" : "copy", u ? "reads memory" : "copy", KEEP, late ? "reads memory" : "copy", other ? "reads memory" : "copy");
        if (!g || u || late || other || s.nrec != 1) fails++;
    }
    // 3. Random pipelines: game up to 3 submissions ahead of the replay, the GPU completing at random, GPU writes to random
    //    ranges. A served copy is wrong if a GPU write of a submission later than the capture's completion point and
    //    before this submission overlaps it.
    {
        gpuw_set s = {0};
        old_n = 0;
        static w_t all[1 << 20];
        int nall = 0;
        uint64_t done = 0, cap_done[1 << 16];
        long served_bad_old = 0, served_bad_new = 0, served_new = 0, served_old = 0, checks = 0;
        int sub_cap = 0;
        for (int sub = 1; sub < 20000; sub++) {
            while (sub_cap < sub + 1 + (int)(rnd() % 3)) { cap_done[++sub_cap & 0xffff] = done; if (rnd() % 2 && done + 1 < (uint64_t)sub) done++; }
            if (done + 1 < (uint64_t)sub && rnd() % 2) done++;   // the GPU completes while this one is replayed
            uint64_t cdone = cap_done[sub & 0xffff];
            gpuw_retire(&s, cdone, KEEP);
            for (int q = 0; q < 8; q++) {   // the copies of this submission
                uint64_t lo = (rnd() % NR) * 0x1000 + (rnd() % 0x800), hi = lo + 0x100;
                checks++;
                int truth = 0;
                for (int j = 0; j < nall; j++) if (all[j].fence > cdone && all[j].fence < (uint64_t)sub && all[j].lo < hi && lo < all[j].hi) { truth = 1; break; }
                int nw = gpuw_inflight(&s, lo, hi, cdone), od = old_check(lo, hi, cdone, done);
                if (!nw) { served_new++; if (truth) served_bad_new++; }
                if (!od) { served_old++; if (truth) served_bad_old++; }
            }
            for (int q = 0; q < 3; q++) {   // its GPU writes (recorded in this replay)
                uint64_t lo = (rnd() % NR) * 0x1000, hi = lo + 0x200 + rnd() % 0x1000;
                gpuw_add(&s, lo, hi, (uint64_t)sub);
                if (old_n < (1 << 16)) old_p[old_n++] = (w_t){lo, hi, (uint64_t)sub};
                all[nall++ & ((1 << 20) - 1)] = (w_t){lo, hi, (uint64_t)sub};
            }
            if (nall > 4000) { int k = 0; for (int j = 0; j < nall; j++) if (all[j].fence + 8 > (uint64_t)sub) all[k++] = all[j]; nall = k; }
        }
        printf("3. random pipelines, %ld copies: stale copies served: 0018 %ld of %ld, 0027 %ld of %ld\n", checks, served_bad_old, served_old, served_bad_new, served_new);
        if (served_bad_new) fails++;
        if (!served_bad_old) printf("   (0018 served none: the test does not reproduce the race)\n");
    }
    // 4. [UIT 0038] The grass case: the GPU fills a vertex range once; the CPU keeps writing a neighbouring page, so the
    //    copy at submit would be game memory without the GPU's bytes. With GPUW_FOREVER the range reads memory for good
    //    (no overflow in fence + keep); with the 0027-0037 window of 600 it was copied again after 600 submissions.
    {
        gpuw_set f = {0}, w = {0};
        gpuw_add(&f, 0x40000, 0x48000, 3); gpuw_add(&w, 0x40000, 0x48000, 3);
        int f_late = 1, w_late = 0, f_other = 0;
        for (uint64_t cd = 10; cd < 200000; cd += 97) {
            gpuw_retire(&f, cd, GPUW_FOREVER); gpuw_retire(&w, cd, KEEP);
            f_late &= gpuw_recent(&f, 0x44000, 0x44100, cd, GPUW_FOREVER);
            f_other |= gpuw_recent(&f, 0x50000, 0x50100, cd, GPUW_FOREVER);
            if (cd > 3 + KEEP) w_late |= gpuw_recent(&w, 0x44000, 0x44100, cd, KEEP);
        }
        int k1 = gpuw_kept(5, GPUW_FOREVER, UINT64_MAX - 1), k2 = gpuw_kept(5, 600, 605), k3 = gpuw_kept(5, 600, 604);
        printf("4. GPU-filled vertex range, 200000 submissions on: always %s, 600-window %s; an unwritten range: %s; kept(): %d %d %d\n",
               f_late ? "reads memory" : "copy", w_late ? "reads memory" : "copy", f_other ? "reads memory" : "copy", k1, k2, k3);
        if (!f_late || w_late || f_other || !k1 || k2 || !k3 || f.nrec != 1) fails++;
    }
    // 5. [UIT 0157] gpuw_each: the writes it lists agree with gpuw_inflight / gpuw_recent, clipped to the asked range, and
    //    the bytes they cover are exactly the written ones (a fine drop keeps the rest of a merged range on the copy)
    {
        gpuw_set s = {0};
        long bad = 0, asked = 0;
        for (int sub = 1; sub < 3000; sub++) {
            uint64_t cdone = sub > 3 ? (uint64_t)sub - 1 - rnd() % 3 : 0;
            gpuw_retire(&s, cdone, KEEP);
            for (int q = 0; q < 6; q++) {
                uint64_t lo = (rnd() % 64) * 0x100, hi = lo + 0x40 + rnd() % 0x400;
                for (int recent = 0; recent < 2; recent++) {
                    struct { uint64_t lo, hi; int bad; } c = {lo, hi, 0};
                    int n = gpuw_each(&s, lo, hi, cdone, recent, KEEP, each_cb, &c);
                    int want = recent ? gpuw_recent(&s, lo, hi, cdone, KEEP) : gpuw_inflight(&s, lo, hi, cdone);
                    asked++;
                    if ((n > 0) != want || c.bad) bad++;
                    // every byte: covered by a listed write <=> some single-byte check says written
                    for (uint64_t b = lo; b < hi && !bad; b += 0x10) {
                        int in = recent ? gpuw_recent(&s, b, b + 1, cdone, KEEP) : gpuw_inflight(&s, b, b + 1, cdone);
                        struct { uint64_t lo, hi; int bad; } d = {b, b + 1, 0};
                        if ((gpuw_each(&s, b, b + 1, cdone, recent, KEEP, each_cb, &d) > 0) != in) bad++;
                    }
                }
            }
            gpuw_add(&s, (rnd() % 64) * 0x100 + rnd() % 0x80, (rnd() % 64) * 0x100 + 0x80, (uint64_t)sub);
        }
        printf("5. gpuw_each against gpuw_inflight / gpuw_recent, %ld ranges: %ld disagree\n", asked, bad);
        if (bad) fails++;
    }
    puts(fails ? "FAIL" : "ok");
    return fails != 0;
}
