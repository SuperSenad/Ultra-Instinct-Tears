// Box test for [UIT 0438] TOTK_VK_FENCE_READBACK (vkb_imgwb.h iwb_pick_fence, iwb_run_note):
//   cc -O2 -Wall -Isrc/gfx tools/tests/test_fence_readback.c src/gfx/tex/nvn_deswizzle.c src/gfx/nvn/nvn_tex.c -o /tmp/t/fr && /tmp/t/fr
// 1) the choice: off never picks; up to IWB_SMALL_MAX always; larger up to IWB_FENCE_LARGE_MAX only while the run of
//    presents drawn in a row is shorter than IWB_FENCE_RUN; above that never.
// 2) the run: drawn every present it counts up, drawn twice in a present it counts once, a gap of a present starts over.
// 3) a model of the grass maps: the 128x128 RGBA8 cut map (64 KiB) drawn every present is copied with every drawing;
//    a 260x260 RGBA8 fire map (264,200 bytes) drawn now and then is copied each time it is drawn; a 512x512 RGBA8
//    map drawn every present is copied for its first 15 presents and then no more (left to explicit reads).
#include <stdio.h>
#include "vkb_imgwb.h"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

int main(void) {
    const uint64_t sizes[] = {1, 4, 4096, IWB_SMALL_MAX - 1, IWB_SMALL_MAX, IWB_SMALL_MAX + 1, 264200, 1 << 20,
                              IWB_FENCE_LARGE_MAX, IWB_FENCE_LARGE_MAX + 1, (uint64_t)64 << 20};
    for (unsigned k = 0; k < sizeof sizes / sizeof sizes[0]; k++)
        for (uint32_t run = 0; run < 40; run++) {
            uint64_t sz = sizes[k];
            CHECK(!iwb_pick_fence(0, sz, run));
            int want = sz <= IWB_SMALL_MAX || (sz <= IWB_FENCE_LARGE_MAX && run < IWB_FENCE_RUN);
            CHECK(iwb_pick_fence(1, sz, run) == want);
        }
    uint64_t last = 0; uint32_t run = 0;
    for (uint64_t f = 1; f <= 20; f++) { iwb_run_note(&last, &run, f); iwb_run_note(&last, &run, f); CHECK(run == f); }
    iwb_run_note(&last, &run, 22); CHECK(run == 1);
    iwb_run_note(&last, &run, 23); CHECK(run == 2);
    iwb_run_note(&last, &run, 23); CHECK(run == 2);
    uint64_t l2 = 0; uint32_t r2 = 0;   // a fresh image first drawn at present 0 and 1
    iwb_run_note(&l2, &r2, 0); CHECK(r2 == 1);
    iwb_run_note(&l2, &r2, 1); CHECK(r2 == 2);
    // the model
    uint64_t lc = 0, lf = 0, lb = 0; uint32_t rc = 0, rf = 0, rb = 0;
    int cut = 0, fire = 0, fire_drawn = 0, big = 0;
    for (uint64_t f = 1; f <= 600; f++) {
        iwb_run_note(&lc, &rc, f); cut += iwb_pick_fence(1, 65536, rc);
        if (f % 7 == 0) { iwb_run_note(&lf, &rf, f); fire_drawn++; fire += iwb_pick_fence(1, 264200, rf); }
        iwb_run_note(&lb, &rb, f); big += iwb_pick_fence(1, 512 * 512 * 4, rb);
    }
    CHECK(cut == 600);
    CHECK(fire == fire_drawn);
    CHECK(big == IWB_FENCE_RUN - 1);
    printf("%s: fence read-back choice and runs (%d failures)\n", fails ? "FAIL" : "ok", fails);
    return fails != 0;
}
