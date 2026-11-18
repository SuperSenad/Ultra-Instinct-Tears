// [UIT 0387] src/host/dlss_scene.h: the DLSS scene share per TOTK_DLSS_MODE. DLAA is the full output size (DLSS 1:1),
// "own" the game's own size, unknown values Quality.
// cc -O2 -Isrc/host tools/tests/test_dlss_scene.c -o /tmp/dls && /tmp/dls
#include "dlss_scene.h"
#include <math.h>
#include <stdio.h>

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

int main(void) {
    CHECK(uit_dlss_scene_share("dlaa") == 1.0);
    CHECK(uit_dlss_scene_share("DLAA") == 1.0);
    CHECK(uit_dlss_scene_share("own") == 0.0);
    CHECK(fabs(uit_dlss_scene_share("quality") - 1 / 1.5) < 1e-9);
    CHECK(fabs(uit_dlss_scene_share("balanced") - 1 / 1.724) < 1e-9);
    CHECK(uit_dlss_scene_share("performance") == 0.5);
    CHECK(fabs(uit_dlss_scene_share("ultra_performance") - 1 / 3.0) < 1e-9);
    CHECK(fabs(uit_dlss_scene_share("bogus") - 1 / 1.5) < 1e-9 && fabs(uit_dlss_scene_share("") - 1 / 1.5) < 1e-9 &&
          fabs(uit_dlss_scene_share(0) - 1 / 1.5) < 1e-9);
    // 2160p: DLAA plans the whole 3840x2160 scene, 44 % more pixels than the game's own 3200x1800
    double px = 3840.0 * 2160 * uit_dlss_scene_share("dlaa") * uit_dlss_scene_share("dlaa");
    CHECK(fabs(px / (3200.0 * 1800) - 1.44) < 1e-9);
    printf(fails ? "FAIL\n" : "PASS\n");
    return fails != 0;
}
