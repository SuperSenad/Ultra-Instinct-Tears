// [UIT 0255] TOTK_VK_READBACK_AHEAD: images the CPU reads back are copied back with each submission that draws them.
// 1) iwb_pick_end / iwb_mark_hot (vkb_imgwb.h, used by vkb_iwb_record) against the formula before 0255 (hot 0: the
//    same answer everywhere) and their bounds. 2) A timeline model: the game submits frames that draw a 1x1 image
//    (its value = the frame number), the translation thread replays them behind, the GPU completes them behind that,
//    the retire thread lands copies before it marks a submission's fence; the game waits for the fence of frame k-LAG
//    and reads the image (invalidate: an explicit read if it is pending). Off: the same reads, values and waits as
//    the model of the old code. On: one explicit read (the first), then none; every value read is at least that of the
//    fenced frame (never staler than the console would show) and at most the old value. TOTK_PROBE_NEGATIVE=1: the
//    copy lands after the fence is marked (the order the retire thread must not have) - the run must fail.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "vkb_imgwb.h"

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { if (fails++ < 10) { printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); } } } while (0)

static void table(void) {
    uint64_t sizes[] = {0, 1, 4, IWB_SMALL_MAX - 1, IWB_SMALL_MAX, IWB_SMALL_MAX + 1, IWB_SNAP_MAX, IWB_EXPLICIT_MAX, IWB_EXPLICIT_MAX + 1};
    for (int mode = 0; mode < 3; mode++)
        for (int alias = 0; alias < 2; alias++)
            for (int hot = 0; hot < 2; hot++)
                for (unsigned k = 0; k < sizeof sizes / sizeof *sizes; k++) {
                    uint64_t sz = sizes[k];
                    int old = (mode == IWB_ALL && sz <= IWB_SMALL_MAX) || (alias && sz <= IWB_EXPLICIT_MAX);
                    int now = iwb_pick_end(mode, alias, hot, sz);
                    if (!hot) CHECK(now == old, "pick mode %d alias %d size %llu: %d, before %d", mode, alias, (unsigned long long)sz, now, old);
                    else CHECK(now == (old || sz <= IWB_SMALL_MAX), "pick hot mode %d alias %d size %llu: %d", mode, alias, (unsigned long long)sz, now);
                }
    for (unsigned k = 0; k < sizeof sizes / sizeof *sizes; k++)
        for (int on = 0; on < 2; on++)
            for (int hot = 0; hot < 2; hot++)
                for (uint64_t n = 254; n < 258; n++) {
                    int m = iwb_mark_hot(on, hot, sizes[k], n, 256);
                    CHECK(m == (on && !hot && sizes[k] <= IWB_SMALL_MAX && n < 256), "mark on %d hot %d size %llu n %llu: %d", on, hot,
                          (unsigned long long)sizes[k], (unsigned long long)n, m);
                    if (!on) CHECK(!m, "marked with the setting off");
                }
}

// ---- the timeline (ms ticks): one image, frames 1..NF
typedef struct { int reads; long wait; int vals[4096]; int nv; int stale; int newer; } res;
enum { NF = 2000 };
static unsigned rng = 1;
static unsigned rnd(unsigned n) { rng = rng * 1103515245u + 12345u; return (rng >> 16) % n; }
// t_x[k]: when the translation thread has replayed frame k's submission; t_g[k]: when the GPU completed it (retire
// marks the fence then). An explicit read at time t: waits until every queued submission is replayed (the latest
// frame submitted by t), then its own copy goes to the GPU behind all of them.
static long t_sub[NF + 2], t_x[NF + 2], t_g[NF + 2];
static void make_timeline(unsigned seed) {
    rng = seed;
    long tx = 0, tg = 0;
    for (int k = 1; k <= NF; k++) {
        t_sub[k] = k * 17 + (long)rnd(3);
        long xs = t_sub[k] > tx ? t_sub[k] : tx;
        tx = xs + 6 + (long)rnd(k % 97 == 0 ? 25 : 6);         // replay 6..11 ms, spikes
        t_x[k] = tx;
        long gs = tx > tg ? tx : tg;
        tg = gs + 10 + (long)rnd(k % 53 == 0 ? 12 : 5);       // GPU 10..14 ms, spikes
        t_g[k] = tg;
    }
}
static int latest_submitted(long t) { int k = 0; while (k < NF && t_sub[k + 1] <= t) k++; return k; }
static int latest_done(long t) { int k = 0; while (k < NF && t_g[k + 1] <= t) k++; return k; }
static int latest_replayed(long t) { int k = 0; while (k < NF && t_x[k + 1] <= t) k++; return k; }
// mode: 0 = old code; 1 = 0255 off; 2 = 0255 on
static void run(int mode, int lag, int negative, res *r) {
    memset(r, 0, sizeof *r);
    int hot = 0, mem = 0, landed_upto = 0;   // game memory holds frame mem's drawing
    for (int k = 1 + lag; k <= NF && r->nv < 4096; k++) {
        int f = k - lag;
        long t = t_sub[k] > t_g[f] ? t_sub[k] : t_g[f];   // the game waited for frame f's fence after submitting frame k
        if (hot) {   // copies landed with each drawing submission completed by t (before its fence; negative: after it)
            int d = latest_done(t);
            if (negative) d = d > 0 ? d - 1 : 0;   // the last one's copy not landed yet when its fence is seen
            if (d > landed_upto) { landed_upto = d; mem = d; }
        }
        int drawn_replayed = latest_replayed(t);
        int pending = drawn_replayed > mem;   // drawn since its last copy back (stamped when its submission was recorded)
        if (hot && !negative) pending = drawn_replayed > latest_done(t) ? 0 : pending;   // recorded with a copy: not pending
        if (hot && negative) pending = 0;
        long wait = 0;
        if (pending) {   // explicit read: the whole queue, then its copy behind the GPU
            int q = latest_submitted(t);
            long xe = t_x[q] > t ? t_x[q] : t;
            long ge = (t_g[q] > xe ? t_g[q] : xe) + 1;
            wait = ge - t;
            mem = q;   // the image as of every queued drawing
            r->reads++;
            if (mode == 2 && iwb_mark_hot(1, hot, 4, 0, 256)) { hot = 1; landed_upto = latest_done(t + wait); }
            if (mode == 1 && iwb_mark_hot(0, hot, 4, 0, 256)) hot = 1;
        }
        r->wait += wait;
        r->vals[r->nv++] = mem;
        if (mem < f) r->stale++;
    }
}
static void timeline(int negative) {
    for (unsigned seed = 1; seed <= 40; seed++) {
        make_timeline(seed);
        for (int lag = 0; lag <= 2; lag++) {
            res a, b, c;
            run(0, lag, 0, &a); run(1, lag, 0, &b); run(2, lag, negative, &c);
            CHECK(a.reads == b.reads && a.wait == b.wait && a.nv == b.nv && !memcmp(a.vals, b.vals, sizeof(int) * (size_t)a.nv),
                  "seed %u lag %d: off differs from before", seed, lag);
            CHECK(a.stale == 0, "seed %u lag %d: before, a read older than the fenced frame", seed, lag);
            CHECK(c.stale == 0, "seed %u lag %d: on, %d reads older than the fenced frame", seed, lag, c.stale);
            CHECK(c.reads <= 1, "seed %u lag %d: on, %d explicit reads", seed, lag, c.reads);
            for (int i = 0; i < c.nv && i < a.nv; i++) if (c.vals[i] > a.vals[i]) { CHECK(0, "seed %u lag %d read %d: on %d > before %d", seed, lag, i, c.vals[i], a.vals[i]); break; }
            if (seed == 1) printf("  lag %d: before %d explicit reads, %.1f ms waiting over %d reads (%.2f ms a read); on: %d reads, %.1f ms\n",
                                  lag, a.reads, (double)a.wait, a.nv, (double)a.wait / a.nv, c.reads, (double)c.wait);
        }
    }
}
int main(void) {
    int negative = getenv("TOTK_PROBE_NEGATIVE") && atoi(getenv("TOTK_PROBE_NEGATIVE"));
    table();
    timeline(negative);
    printf("test_readahead%s: %d checks, %d failed\n", negative ? " (negative probe)" : "", checks, fails);
    return fails != 0;
}
