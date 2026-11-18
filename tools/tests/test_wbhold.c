// Box test for [UIT 0439] TOTK_VK_WB_HOLD (src/gfx/vkb_wbhold.h, used by backend_vk.c wb_merge / mtl_wb_held_present):
//   cc -O2 -Wall -Isrc/gfx tools/tests/test_wbhold.c -o /tmp/t/wbh && /tmp/t/wbh
// 1) the list: ranges come out in the order they went in, only when due (label <= upto) or all; the byte count follows;
//    overlaps are found; a GPU-only range is held only while there is room, a range over a held one always.
// 2) wbh_present_due: nothing before WBH_FRAMES presents or before anything was held; labels <= n - 2 at present n;
//    no more waits WBH_RECENT presents after the last held range.
// 3) The shrine model: frame n's GPU result (value n, label n) retires at a random time 0.1-1.9 frames after present
//    n, one in ten 2.0-2.7 frames after it (later than the present two frames on); a game thread reads it in the middle of every frame without waiting.
//    Before (copy back at retirement) the age of what it reads changes from frame to frame (the flicker). With the hold
//    (the present of frame n waits for label n - 2 to retire, then applies labels <= n - 2) every read finds n - 2.
//    TOTK_PROBE_NEGATIVE=1: the present applies without waiting for the GPU - the reads must vary again (run fails).
#include <stdio.h>
#include <string.h>
#include "vkb_wbhold.h"

static int fails, checks;
#define CHECK(c) do { checks++; if (!(c)) { if (fails++ < 10) printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); } } while (0)

static wb_held *mk(uint64_t frame, uint64_t lo, uint64_t len) {
    wb_held *h = malloc(sizeof *h + 1);
    *h = (wb_held){frame, lo, len, 0, NULL};
    return h;
}
static uint64_t rng = 88172645463325252ull;
static double frand(void) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return (double)(rng >> 11) / 9007199254740992.0; }

static int model(int hold, int wait, int frames, int *changes) {
    enum { N = 4096 };
    static double ret[N];
    wbh_list l = WBH_LIST_INIT(l);
    uint64_t mem = 0, last_held = 0;
    int prev_age = -1, bad = 0;
    *changes = 0;
    for (int n = 1; n < frames && n < N; n++) ret[n] = n + 0.1 + (frand() < 0.1 ? 1.9 + 0.8 * frand() : 1.8 * frand());   // one in ten past two frames
    int next_ret = 1;   // retirements in order (one queue)
    for (int n = 1; n + 1 < frames && n + 1 < N; n++) {
        double t = n;   // present n (the game thread)
        uint64_t upto;
        if (hold && wbh_present_due((uint64_t)n, last_held, &upto) && wait && next_ret <= (int)upto) t = ret[upto] > t ? ret[upto] : t;   // waits for label upto
        double read_at = (t > n + 0.5 ? t : n + 0.5) + 0.01;
        // retirements up to the present (held or written), then the present applies, then retirements up to the read
        for (int pass = 0; pass < 2; pass++) {
            double until = pass ? read_at : t;
            while (next_ret < frames - 1 && ret[next_ret] <= until) {
                uint64_t label = (uint64_t)next_ret;
                if (hold && wbh_should_hold(&l, wbh_overlaps(&l, 0, 64), 1, 64)) { wb_held *h = mk(label, 0, 64); wbh_push(&l, h); last_held = label; }
                else mem = label;
                next_ret++;
            }
            if (!pass && hold && wbh_present_due((uint64_t)n, last_held, &upto)) {
                wb_held *h;
                while ((h = wbh_pop_due(&l, upto, 0))) { mem = h->frame; free(h); }
            }
        }
        if (n > 4) {
            int age = (int)((uint64_t)n - mem);
            if (prev_age >= 0 && age != prev_age) (*changes)++;
            if (hold && age != WBH_FRAMES) bad++;
            prev_age = age;
        }
    }
    wb_held *h;
    while ((h = wbh_pop_due(&l, 0, 1))) free(h);
    return bad;
}

int main(void) {
    wbh_list l = WBH_LIST_INIT(l);
    CHECK(!wbh_pop_due(&l, 100, 1));
    wbh_push(&l, mk(5, 0x1000, 0x100)); wbh_push(&l, mk(6, 0x2000, 0x100)); wbh_push(&l, mk(9, 0x3000, 0x10));
    CHECK(l.bytes == 0x210);
    CHECK(wbh_overlaps(&l, 0x10ff, 2) && !wbh_overlaps(&l, 0x1100, 0x100) && wbh_overlaps(&l, 0x2fff, 2) && !wbh_overlaps(&l, 0x3010, 1));
    CHECK(wbh_should_hold(&l, 1, 0, 1ull << 40));   // over a held range: always
    CHECK(!wbh_should_hold(&l, 0, 0, 16));           // the CPU wrote it, nothing held there: now
    CHECK(wbh_should_hold(&l, 0, 1, 16) && !wbh_should_hold(&l, 0, 1, WBH_MAX_BYTES));
    CHECK(!wbh_pop_due(&l, 4, 0));
    wb_held *h = wbh_pop_due(&l, 6, 0); CHECK(h && h->frame == 5); free(h);
    h = wbh_pop_due(&l, 6, 0); CHECK(h && h->frame == 6); free(h);
    CHECK(!wbh_pop_due(&l, 6, 0) && l.bytes == 0x10);
    h = wbh_pop_due(&l, 0, 1); CHECK(h && h->frame == 9); free(h);
    CHECK(!l.head && l.tail == &l.head && l.bytes == 0);
    wbh_push(&l, mk(10, 0, 1)); h = wbh_pop_due(&l, 10, 0); CHECK(h && l.tail == &l.head); free(h);   // the tail after emptying
    uint64_t up = 0;
    CHECK(!wbh_present_due(2, 1, &up) && !wbh_present_due(50, 0, &up));
    CHECK(wbh_present_due(3, 1, &up) && up == 1);
    CHECK(wbh_present_due(100, 98, &up) && up == 98);
    CHECK(wbh_present_due(2 + 50 + WBH_RECENT, 50, &up) && !wbh_present_due(3 + 50 + WBH_RECENT, 50, &up));
    int neg = getenv("TOTK_PROBE_NEGATIVE") && *getenv("TOTK_PROBE_NEGATIVE") == '1';
    int c0, c1;
    model(0, 0, 3000, &c0);
    int bad = model(1, !neg, 3000, &c1);
    printf("shrine model, 3000 frames: age of the result read changed %d times before, %d times with the hold (%d reads not two frames old)\n", c0, c1, bad);
    CHECK(c0 > 100);
    CHECK(bad == 0 && c1 == 0);
    int c2, bad2 = model(1, 0, 3000, &c2);   // (the hold without the present's wait: what the wait is for)
    if (!neg) CHECK(bad2 > 0);
    printf("%s: test_wbhold, %d checks, %d failed\n", fails ? "FAIL" : "ok", checks, fails);
    return fails != 0;
}
