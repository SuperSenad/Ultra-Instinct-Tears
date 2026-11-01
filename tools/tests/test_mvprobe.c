// [perf-mt] tests for src/gfx/vkb_mvprobe.h (TOTK_DLSS_MV_PROBE) and dlss_mv_conv (src/vk/dlss.h).
// cc -O2 -Isrc/gfx -Isrc/vk -I<Vulkan-Headers>/include tools/tests/test_mvprobe.c -o /tmp/t/mvp && /tmp/t/mvp
#include "vkb_mvprobe.h"
#include "dlss.h"
#include <stdlib.h>

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

static mvp_rt set(int n, const uint32_t *fmt, uint32_t w, uint32_t h, uint64_t ser0, uint32_t dfmt) {
    mvp_rt r; memset(&r, 0, sizeof r);
    r.n = n;
    for (int i = 0; i < n; i++) { r.fmt[i] = fmt[i]; r.w[i] = fmt[i] ? w : 0; r.h[i] = fmt[i] ? h : 0; r.ser[i] = fmt[i] ? ser0 + (uint64_t)i : 0; }
    if (dfmt) { r.dfmt = dfmt; r.dw = w; r.dh = h; r.dser = ser0 + 100; }
    return r;
}

int main(void) {
    char out[640], sum[256];
    // which frames
    CHECK(!mvp_frame_wanted(0, 0)); CHECK(!mvp_frame_wanted(599, 0)); CHECK(mvp_frame_wanted(600, 0)); CHECK(mvp_frame_wanted(601, 0));
    CHECK(!mvp_frame_wanted(602, 0)); CHECK(mvp_frame_wanted(4200, 2)); CHECK(!mvp_frame_wanted(4201, 2)); CHECK(!mvp_frame_wanted(4200, MVP_FRAMES));
    // formats
    CHECK(mvp_velocity_format(0x11) && mvp_velocity_format(0x13) && mvp_velocity_format(0x16));
    CHECK(!mvp_velocity_format(0x29) && !mvp_velocity_format(0x12) && !mvp_velocity_format(0x34));
    CHECK(mvp_candidate(0x11, 1600, 896, 1600, 896)); CHECK(mvp_candidate(0x11, 800, 448, 1600, 896));
    CHECK(!mvp_candidate(0x11, 400, 224, 1600, 896)); CHECK(!mvp_candidate(0x11, 0, 0, 1600, 896)); CHECK(mvp_candidate(0x13, 64, 64, 0, 0));
    char b[16];
    CHECK(!strcmp(mvp_fmt_name(0x3F, b, sizeof b), "R11G11B10F")); CHECK(!strcmp(mvp_fmt_name(0x99, b, sizeof b), "fmt 0x99"));

    // one probed frame: a G-buffer (with an RG16F target), the same set again, lighting, the upscale
    mvp_state s; memset(&s, 0, sizeof s);
    uint32_t gb[4] = {0x25, 0x3D, 0x11, 0x25}, lit[1] = {0x29}, two[2] = {0x29, 0};
    mvp_rt g = set(4, gb, 1600, 896, 10, 0x34), l = set(1, lit, 1600, 896, 20, 0), t = set(2, two, 1600, 896, 30, 0x34);
    CHECK(!mvp_draw(&s, 599, &g, "k", 0, 0, out, sizeof out));             // not a probed frame
    CHECK(!mvp_frame_end(&s, 599, sum, sizeof sum));
    CHECK(mvp_draw(&s, 600, &g, "0123456789abcdefXX", 1600, 896, out, sizeof out));
    CHECK(strstr(out, "#2 RG16F 1600x896 (velocity candidate)") && strstr(out, "depth D32F 1600x896") && strstr(out, "fs 0123456789abcdef,") == NULL
          && strstr(out, "fs 0123456789abcdef") && !strstr(out, "XX"));
    CHECK(!mvp_draw(&s, 600, &g, "k", 1600, 896, out, sizeof out));          // same set: once
    CHECK(mvp_draw(&s, 600, &l, NULL, 1600, 896, out, sizeof out) && strstr(out, "no depth") && strstr(out, "fs -") && !strstr(out, "candidate"));
    CHECK(mvp_draw(&s, 600, &t, "k", 1600, 896, out, sizeof out) && strstr(out, "#1 -"));
    CHECK(mvp_draw(&s, 600, &g, "k", 1600, 896, out, sizeof out));           // back to the G-buffer: a change again
    CHECK(mvp_frame_end(&s, 600, sum, sizeof sum) && strstr(sum, "4 render target sets up") && strstr(sum, "2 with a velocity candidate"));
    CHECK(!mvp_draw(&s, 600, &l, "k", 1600, 896, out, sizeof out));          // after the upscale (HUD): not probed
    CHECK(!mvp_frame_end(&s, 600, sum, sizeof sum));
    CHECK(s.probed == 1);
    // 601: probed; no candidate
    CHECK(mvp_draw(&s, 601, &l, "k", 1600, 896, out, sizeof out));
    CHECK(mvp_frame_end(&s, 601, sum, sizeof sum) && strstr(sum, "1 render target sets up to the upscale, 0 with") && strstr(sum, "camera-only"));
    // a probed frame without an upscale draw (a menu): the next frame starts afresh, no summary for the old one
    CHECK(mvp_draw(&s, 4200, &g, "k", 1600, 896, out, sizeof out));
    CHECK(!mvp_draw(&s, 4201, &g, "k", 1600, 896, out, sizeof out));
    CHECK(!mvp_frame_end(&s, 4200, sum, sizeof sum));
    CHECK(s.probed == 3);
    // the line cap: many distinct sets in one frame
    memset(&s, 0, sizeof s);
    int said = 0;
    for (int i = 0; i < 500; i++) { mvp_rt x = set(1, lit, 1600, 896, 1000 + (uint64_t)i, 0); said += mvp_draw(&s, 600, &x, "k", 0, 0, out, sizeof out); }
    CHECK(said == MVP_LINES && s.sets == 500);
    // the longest line: 8 targets, all named, all candidates, fits
    uint32_t eight[8] = {0x16, 0x16, 0x16, 0x16, 0x16, 0x16, 0x16, 0x16};
    mvp_rt e8 = set(8, eight, 65535, 65535, 5000, 0x36);
    memset(&s, 0, sizeof s);
    CHECK(mvp_draw(&s, 600, &e8, "0123456789abcdef", 65535, 65535, out, sizeof out) && strstr(out, "fs 0123456789abcdef"));
    char tiny[24];   // a short buffer: truncated, never overrun
    memset(&s, 0, sizeof s);
    memset(tiny, 'Z', sizeof tiny);
    CHECK(mvp_draw(&s, 600, &e8, "k", 0, 0, tiny, 20) && tiny[19] == 0 && tiny[20] == 'Z');

    // dlss_mv_conv: a zeroed frame gives the constants dlss_sl.c always sent
    dlss_frame f; memset(&f, 0, sizeof f);
    f.in_w = 1280; f.in_h = 720;
    dlss_mv_conv_t c = dlss_mv_conv(&f);
    CHECK(c.scale_x == 1.0f / 1280.0f && c.scale_y == 1.0f / 720.0f && !c.jittered && c.camera_included && !c.dilated && !c.depth_inverted);
    f.mv_scale_x = 0.5f; f.mv_scale_y = -0.5f; f.mv_jittered = 1; f.mv_object_only = 1; f.mv_dilated = 7; f.depth_inverted = 1;
    c = dlss_mv_conv(&f);
    CHECK(c.scale_x == 0.5f && c.scale_y == -0.5f && c.jittered && !c.camera_included && c.dilated == 1 && c.depth_inverted);
    memset(&f, 0, sizeof f);
    c = dlss_mv_conv(&f);
    CHECK(c.scale_x == 0.0f && c.scale_y == 0.0f);   // no size: no division by zero

    printf(fails ? "mvprobe: %d FAILED\n" : "mvprobe: all passed\n", fails);
    return fails != 0;
}
