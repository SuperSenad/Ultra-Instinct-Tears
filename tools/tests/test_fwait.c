// Box test for [UIT 0441] the split of the game's long fence waits and [UIT 0449] TOTK_VK_FENCE_EARLY's rules
// (src/gfx/vkb_fwait.h, used by backend_vk.c, vkb_tex.c and hle_nvn.c nvnSyncWait):
//   cc -O2 -Wall -Isrc/gfx tools/tests/test_fwait.c -o /tmp/t/fwait -lpthread && /tmp/t/fwait
// 1) the ring: a record is found only for its own fence (not before its commit, not once the ring passed it); a time
//    set for a fence the ring no longer holds is dropped; a new commit clears the times.
// 2) fw_split: the parts always add up to the wait; each part is the stretch between its two marks; marks before the
//    wait (the GPU was done before the translation thread) give 0; a missing time is 0 and never negative; marks after
//    the wait returned are cut at its end.
// 3) fw_bucket: the edges 1, 2, 4, 8 ms.
// 4) marks: a touch counts for its own submission number only; fw_touched over a span; too long a span is touched.
//    Recordings that overlap (begin / touch / commit interleaved at random, numbers taken as the backend does: the
//    last fence + 1) never lose a touch: every recording that touched commits with FW_GM.
// 5) fe_rule and fe_wb_ok, case by case: no record, game memory, the shadow check, occlusion results, image copy backs
//    at the end, split ones, buffer copy backs with them, nothing; buffer copy backs left at the end GPU-only /
//    written by the CPU / over the hold budget / hold off / over an image copy back.
// 6) where image copy backs go: fe_pos_word / fe_pos_note (a later chunk or submission wins, unknown wins over any
//    chunk, from 4 threads at once), fe_split_at cases, and at random: the copies always come after the last write of
//    every image they copy (or stay at the end when a write was outside the chunks).
// 7) the hand-over of a submission's late work between the wait and the retire thread (fe_claim / fe_release /
//    fe_retire_take), two threads racing over 200,000 submissions: done exactly once each, never skipped.
// 8) The model: frames of 1-4 submissions with random hazards (game memory still read on the GPU, image copy backs at
//    the end or split at a random chunk, buffer copy backs with them or at the end, written by the CPU or not,
//    occlusion results, the shadow check, a missing record), a GPU that runs them in order with jitter, a game that
//    waits for its frame's last fence as the backend does (the one before in full, then the rules). Whenever a wait
//    returns, for every submission up to its fence the GPU is still running: it reads no game memory, its image copy
//    backs are done and laid down, its events and counters written, its buffer copy backs merged after their signal
//    or, left at the end, all GPU-only (held, as today), no occlusion results or shadow check pending; the game is
//    never more than one submission ahead. The early return must happen (in at least a tenth of the waits), and the
//    GPU time not waited for is printed.
// TOTK_PROBE_NEGATIVE=1: fw_split without the clamp to the wait's end, rules that ignore FW_GM, a wait that lays down
// the image copy backs without waiting for their signal, a split at the first write instead of the last, and a
// retire thread that does the late work whatever the wait did - the matching checks must fail.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <stdatomic.h>
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
static int rule_neg(int have, uint64_t fl) { return fe_rule(have, fl & ~(uint64_t)FW_GM); }   // ignores FW_GM
static int split_at_neg(const uint64_t *w, int n, uint64_t sub, int early_next) {   // the first write, not the last
    int k = 1 << 30;
    for (int i = 0; i < n; i++) {
        if (w[i] >> 8 != sub) return FE_SPLIT_END;
        int p = (int)(w[i] & 255);
        if (p == FE_POS_UNKNOWN || p == 0) return FE_SPLIT_END;
        if (p - 1 < k) k = p - 1;
    }
    return !n || k < early_next ? -1 : k;
}
// 6) notes from 4 threads
static uint64_t pn_word, pn_seed[4], pn_max[4];
static void *pn_thread(void *u) {
    uint64_t *seed = u, r = *seed, mx = 0;
    int idx = (int)(seed - pn_seed);
    for (int i = 0; i < 2000; i++) {
        r ^= r << 13; r ^= r >> 7; r ^= r << 17;
        int ch = (int)(r % 300) - 20;   // some outside the chunks, some past the word
        uint64_t sub = 5 + (r >> 40) % 2;
        fe_pos_note(&pn_word, sub, ch);
        if (fe_pos_word(sub, ch) > mx) mx = fe_pos_word(sub, ch);
    }
    pn_max[idx] = mx;
    return NULL;
}
// 7) the hand-over
enum { HN = 200000 };
static _Atomic int h_st[HN];
static _Atomic int h_done[HN];
static int h_neg;
static void *h_wait(void *u) {
    (void)u;
    uint64_t r = 12345;
    for (int i = 0; i < HN; i++) {
        r ^= r << 13; r ^= r >> 7; r ^= r << 17;
        if (r % 3 == 0) continue;   // this wait never came
        if (!fe_claim(&h_st[i])) continue;
        int did = r % 5 != 0;   // else gave it back (a CPU-written range, a timeout)
        if (did) atomic_fetch_add(&h_done[i], 1);
        fe_release(&h_st[i], did);
    }
    return NULL;
}
static void *h_retire(void *u) {
    (void)u;
    for (int i = 0; i < HN; i++) {
        int t;
        if (h_neg) t = 0;
        else while ((t = fe_retire_take(&h_st[i])) < 0) {}
        if (!t) atomic_fetch_add(&h_done[i], 1);
    }
    return NULL;
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
    // 5) fe_rule, fe_wb_ok
    CHECK(fe_rule(0, 0) == FE_B_NOREC);
    CHECK(fe_rule(1, 0) == FE_NOW && fe_rule(1, FW_POSTS) == FE_NOW && fe_rule(1, FW_WB | FW_POSTS) == FE_NOW);
    CHECK(fe_rule(1, FW_GM) == FE_B_GM && fe_rule(1, FW_GM | FW_VIS | FW_IWB) == FE_B_GM);
    CHECK(fe_rule(1, FW_NVF) == FE_B_NVF && fe_rule(1, FW_OQ | FW_POSTS) == FE_B_OQ);
    CHECK(fe_rule(1, FW_IWB) == FE_B_IWB && fe_rule(1, FW_VIS) == FE_VIS && fe_rule(1, FW_VIS | FW_WB | FW_POSTS) == FE_VIS);
    CHECK(fe_rule(1, FW_WBV | FW_VIS) == FE_VIS && fe_rule(1, FW_WBV | FW_VIS | FW_IWB) == FE_VIS);   // (FW_IWB with FW_VIS: split too)
    CHECK(fe_rule(1, FW_WBV) == FE_B_NOREC);   // buffer copy backs with no signal: cannot be, the full wait
    {
        int g1[3] = {1, 1, 1}, g0[3] = {1, 0, 1};
        uint64_t l[3] = {100, 200, 300};
        CHECK(fe_wb_ok(0, NULL, NULL, 0, 1000, 0, 1));   // none: fine whatever the rest
        CHECK(fe_wb_ok(3, g1, l, 0, 1000, 1, 0));
        CHECK(!fe_wb_ok(3, g0, l, 0, 1000, 1, 0));   // the CPU wrote one: the game reads the merge after its wait
        CHECK(!fe_wb_ok(3, g1, l, 500, 1000, 1, 0) && fe_wb_ok(3, g1, l, 400, 1000, 1, 0));   // over the hold budget: lands at retirement
        CHECK(!fe_wb_ok(3, g1, l, 0, 1000, 0, 0));   // UIT 0439 off: lands at retirement
        CHECK(!fe_wb_ok(3, g1, l, 0, 1000, 1, 1));   // over an image copy back the wait lays down
    }
    // 6) where image copy backs go
    {
        uint64_t w = 0;
        fe_pos_note(&w, 7, 3); CHECK(w == fe_pos_word(7, 3));
        fe_pos_note(&w, 7, 1); CHECK(w == fe_pos_word(7, 3));   // an earlier chunk does not win
        fe_pos_note(&w, 7, 5); CHECK(w == fe_pos_word(7, 5));
        fe_pos_note(&w, 7, -1); CHECK((w & 255) == FE_POS_UNKNOWN);   // outside the chunks
        fe_pos_note(&w, 7, 9); CHECK((w & 255) == FE_POS_UNKNOWN);
        fe_pos_note(&w, 8, 0); CHECK(w == fe_pos_word(8, 0));   // the next submission starts over
        CHECK((fe_pos_word(9, 300) & 255) == FE_POS_UNKNOWN);   // a chunk past the word: unknown
        uint64_t ws[3] = {fe_pos_word(8, 2), fe_pos_word(8, 6), fe_pos_word(8, 4)};
        CHECK(fe_split_at(ws, 3, 8, 0) == 6 && fe_split_at(ws, 3, 8, 6) == 6 && fe_split_at(ws, 3, 8, 7) == -1);
        CHECK(fe_split_at(ws, 0, 8, 3) == -1);   // none drawn
        CHECK(fe_split_at(ws, 3, 9, 0) == FE_SPLIT_END);   // a word of another submission
        uint64_t wu[2] = {fe_pos_word(8, 1), fe_pos_word(8, -1)};
        CHECK(fe_split_at(wu, 2, 8, 0) == FE_SPLIT_END);
        uint64_t wz[1] = {8ull << 8};   // (no chunk recorded)
        CHECK(fe_split_at(wz, 1, 8, 0) == FE_SPLIT_END);
        // random: the copies come after every write
        int late = 0, ends = 0, splits = 0;
        for (int round = 0; round < 50000; round++) {
            enum { NI = 6 };
            uint64_t sub = 100 + (uint64_t)round, wd[NI];
            int n = 1 + (int)rnd(NI), nch = 1 + (int)rnd(40), en = (int)rnd((uint64_t)nch + 1), lastw[NI], unk = 0;
            for (int i = 0; i < n; i++) {
                wd[i] = rnd(3) ? fe_pos_word(sub - 1, (int)rnd(10)) : 0;   // an older word first
                lastw[i] = -1;
                int nw = 1 + (int)rnd(4);
                for (int k = 0; k < nw; k++) {
                    int ch = rnd(60) ? (int)rnd((uint64_t)nch) : -1;
                    fe_pos_note(&wd[i], sub, ch);
                    if (ch < 0) unk = 1; else if (ch > lastw[i]) lastw[i] = ch;
                }
            }
            int at = neg ? split_at_neg(wd, n, sub, en) : fe_split_at(wd, n, sub, en);
            if (at == FE_SPLIT_END) { ends++; if (!unk) late++; continue; }   // (never at the end without a reason)
            splits++;
            if (unk) late++;
            int pos = at < 0 ? en - 1 : at;   // the copies run after chunk pos (or after every chunk already sent)
            for (int i = 0; i < n; i++) if (lastw[i] > pos) late++;
            if (at >= 0 && at < en) late++;
        }
        CHECK(late == 0);
        CHECK(splits > 1000 && ends > 100);
        // 4 threads noting at once: the word ends at the highest
        for (int round = 0; round < 200; round++) {
            pthread_t th[4];
            pn_word = 0;
            for (int i = 0; i < 4; i++) { pn_seed[i] = rnd(1u << 30) + 1; pthread_create(&th[i], NULL, pn_thread, &pn_seed[i]); }
            for (int i = 0; i < 4; i++) pthread_join(th[i], NULL);
            uint64_t want = 0;
            for (int i = 0; i < 4; i++) if (pn_max[i] > want) want = pn_max[i];
            CHECK(pn_word == want);
        }
    }
    // 7) the hand-over, two threads
    {
        h_neg = neg;
        pthread_t a, b;
        pthread_create(&a, NULL, h_wait, NULL);
        pthread_create(&b, NULL, h_retire, NULL);
        pthread_join(a, NULL); pthread_join(b, NULL);
        int bad = 0, early = 0;
        for (int i = 0; i < HN; i++) { bad += atomic_load(&h_done[i]) != 1; early += atomic_load(&h_st[i]) == FE_ST_EARLY; }
        CHECK(bad == 0);
        CHECK(neg || early > 0);
    }
    // 8) the model
    {
        static fw_ring r5;
        enum { FRAMES = 30000, MAXS = FRAMES * 4 + 8 };
        static uint64_t flags_of[MAXS];
        static int have_of[MAXS], wbcpu_of[MAXS], laid[MAXS], posted[MAXS];
        static double fin[MAXS], vis[MAXS];
        uint64_t fence = 0;
        double gpu_t = 0, now = 0, saved = 0, waited = 0;
        int bad_gm = 0, bad_iw = 0, bad_post = 0, bad_wb = 0, bad_misc = 0, ahead = 0, early_n = 0, waits = 0;
        for (int n = 1; n < FRAMES; n++) {
            int m = 1 + (int)rnd(4);
            for (int i = 0; i < m; i++) {
                uint64_t f = ++fence, fl = FW_POSTS;
                if (rnd(100) < 10) fl |= FW_GM;   // a big pool copy, memory without a device copy, a mirror
                int iwb = rnd(100) < 60, split = rnd(100) < 75;
                if (iwb) fl |= split ? FW_VIS : FW_IWB;
                int wb = rnd(100) < 40, wbsplit = rnd(100) < 70;
                if (wb && (wbsplit && (fl & FW_VIS || !iwb))) fl |= FW_WBV | FW_VIS;   // with the copy backs (or alone)
                else if (wb) fl |= FW_WB;
                if (rnd(100) < 4) fl |= FW_OQ;
                if (rnd(1000) < 15) fl |= FW_NVF;
                have_of[f] = rnd(1000) >= 10;
                if (have_of[f]) fw_commit(&r5, f, fl);
                flags_of[f] = fl; wbcpu_of[f] = (fl & (FW_WB | FW_WBV)) && rnd(100) < 15;
                laid[f] = posted[f] = 0;
                double start = gpu_t > now ? gpu_t : now, d = 0.5 + (double)rnd(1000) / 1000.0 * (rnd(10) ? 2.0 : 9.0) / m;
                gpu_t = start + d;
                fin[f] = gpu_t;
                vis[f] = start + d * (double)(1 + rnd(12)) / 13.0;   // after the chunk that draws them last
            }
            now += 1.0 + (double)rnd(1000) / 1000.0 * 1.5;   // the game's own work
            uint64_t f = fence;   // the wait for the frame's last submission, as mtl_fence_early_wait
            double t0 = now;
            waits++;
            if (f > 1 && fin[f - 1] > now) now = fin[f - 1];   // the one before, in full
            for (uint64_t s = 1; s < f; s++) laid[s] = posted[s] = 1;   // (the retire thread did those)
            uint64_t fl = 0;
            int have = fw_flags(&r5, f, &fl);
            int r = neg ? rule_neg(have, fl) : fe_rule(have, fl);
            if ((r == FE_NOW || r == FE_VIS) && (fl & FW_WB) && !(fl & FW_WBV) && wbcpu_of[f]) r = FE_B_WB;   // fe_wb_ok at the wait
            if (r == FE_NOW || r == FE_VIS) {
                if (r == FE_VIS && !neg && vis[f] > now) now = vis[f];   // its copy backs' signal
                laid[f] = 1; posted[f] = 1;   // laid down and written by the wait
                early_n += fin[f] > now;
            } else if (fin[f] > now) now = fin[f];
            if (fin[f] <= now) laid[f] = posted[f] = 1;
            waited += now - t0;
            if (fin[f] > now) saved += fin[f] - now;
            // the game now writes game memory and reads results: everything still running must be invisible to it
            for (uint64_t s = f > 8 ? f - 8 : 1; s <= f; s++) {
                if (fin[s] <= now) continue;
                if (s != f) ahead++;
                uint64_t x = flags_of[s];
                if (x & FW_GM) bad_gm++;
                if ((x & FW_IWB) || ((x & FW_VIS) && (vis[s] > now || !laid[s]))) bad_iw++;
                if (!posted[s]) bad_post++;
                if ((x & FW_WB) && !(x & FW_WBV) && wbcpu_of[s]) bad_wb++;   // the merge the game reads would come later
                if ((x & FW_WBV) && (vis[s] > now || !laid[s])) bad_wb++;   // merged by the wait after their signal
                if ((x & (FW_OQ | FW_NVF)) || !have_of[s]) bad_misc++;
            }
        }
        if (neg) CHECK(bad_gm == 0 && bad_iw == 0);   // must fail: the probe returns over game memory and copy backs not done
        else {
            CHECK(bad_gm == 0);
            CHECK(bad_iw == 0);
            CHECK(bad_post == 0);
            CHECK(bad_wb == 0);
            CHECK(bad_misc == 0);
            CHECK(ahead == 0);
            CHECK(early_n > waits / 10);
            printf("model: %d of %d frame waits ended before their submission was done; GPU time not waited for %.0f%% of what the full waits took\n",
                   early_n, waits, 100.0 * saved / (saved + waited));
        }
    }
    printf("%s: %d checks, %d failed%s\n", fails ? "FAIL" : "ok", checks, fails, neg ? " (TOTK_PROBE_NEGATIVE=1)" : "");
    return fails != 0;
}
