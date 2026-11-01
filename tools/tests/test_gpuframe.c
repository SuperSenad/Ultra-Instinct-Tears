// [UIT native] Box test: the per-frame GPU time ring (src/gfx/vkb_gpuframe.h). Frames' submissions summed into their
// frame, an entry reused by a later frame reads as that frame only, unknown frames read -1, the span covers all.
// cc -O2 -Isrc/gfx tools/tests/test_gpuframe.c -o /tmp/test_gpuframe && /tmp/test_gpuframe
#include <stdio.h>
#include <math.h>
#include "vkb_gpuframe.h"
static gpu_frame g[GFN];
static int bad;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); bad++; } } while (0)
int main(void) {
    CHECK(gpu_frame_ms(g, 0) == -1 && gpu_frame_ms(g, 5) == -1);
    gpu_frame_put(g, 0, 100, 200, 1000000);            // frame 0: 1 ms
    CHECK(fabs(gpu_frame_ms(g, 0) - 1.0) < 1e-9);
    for (uint64_t f = 1; f < 3 * GFN; f++)            // three submissions a frame: 2 + 3 + 4 ms
        for (int k = 0; k < 3; k++) gpu_frame_put(g, f, f * 1000 + k * 10, f * 1000 + k * 10 + 5, (uint64_t)(2 + k) * 1000000);
    CHECK(fabs(gpu_frame_ms(g, 3 * GFN - 1) - 9.0) < 1e-9);
    CHECK(gpu_frame_ms(g, GFN - 1) == -1);             // overwritten by 2 * GFN - 1, then 3 * GFN - 1
    CHECK(gpu_frame_ms(g, 0) == -1);
    uint64_t a, b;
    CHECK(gpu_frame_span(g, 2 * GFN + 7, &a, &b) && a == (2 * GFN + 7) * 1000ull && b == (2 * GFN + 7) * 1000ull + 25);
    CHECK(!gpu_frame_span(g, 7, &a, &b) && a == 0 && b == 0);
    gpu_frame_put(g, 3 * GFN, 50, 60, 500000);          // a submission that ended late, began early: the span widens
    gpu_frame_put(g, 3 * GFN, 10, 90, 500000);
    CHECK(gpu_frame_span(g, 3 * GFN, &a, &b) && a == 10 && b == 90 && fabs(gpu_frame_ms(g, 3 * GFN) - 1.0) < 1e-9);
    printf(bad ? "test_gpuframe: %d failure(s)\n" : "test_gpuframe: ok\n", bad);
    return bad != 0;
}
