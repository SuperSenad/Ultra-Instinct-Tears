/* [UIT 0068] Display settings shared by the file system (HeapDef), the kernel (application pool), the lift's switches and
 * DLSS. See uit_display.h. The memory formula is MacNativeGames' (game_patches_v143.c runtime_output_res_extra_bytes,
 * MIT): the canvas-sized and the scene-sized render targets share DynamicTextureAllocatorMem (113664 KiB at the 1080p
 * canvas and the 1600x900 scene); each grows it by its own pixel ratio, a bigger scene with a 1.6 margin (MNG measured a
 * crash in the allocator with less). */
#include "uit_display.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "uit_str.h"   /* [UIT 0104] uit_stricmp: no <strings.h> on Windows */
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#define LOG(...) (fprintf(stderr, "[totk] " __VA_ARGS__), fputc('\n', stderr))

/* tools/lifter/patches.py writes 1 into main_table.c when the output-size sites matched 1.4.3 (output_res) */
extern const int main_output_res_patch;
/* ... and for the four camera / viewport aspect sites (aspect) */
extern const int main_aspect_patch;
/* [UIT 0070] ... and for the shadow-size site the game confirmed (tools/lifter/scan.py, src/host/uit_lift.c) */
extern const int main_shadow_patch;

static int parse_wh(const char *e, unsigned *w, unsigned *h) {
    char rest;
    return sscanf(e, "%ux%u%c", w, h, &rest) == 2;
}

/* [native-1031] TOTK_OUTPUT_RES=display: the main display's current mode in real pixels (EnumDisplaySettings does not
 * depend on Windows' scaling), read once; the game window opens there. Off Windows (box tests): UIT_TEST_DISPLAY=<w>x<h>. */
int uit_display_size(unsigned *w, unsigned *h) {
    static unsigned dw, dh;
    static int done;
    if (!done) {
#ifdef _WIN32
        DEVMODEW dm;
        memset(&dm, 0, sizeof dm);
        dm.dmSize = sizeof dm;
        if (EnumDisplaySettingsW(NULL, ENUM_CURRENT_SETTINGS, &dm)) { dw = dm.dmPelsWidth; dh = dm.dmPelsHeight; }
#else
        const char *t = getenv("UIT_TEST_DISPLAY");
        if (!t || !parse_wh(t, &dw, &dh)) dw = dh = 0;
#endif
        done = 1;
    }
    if (!dw || !dh) return 0;
    *w = dw; *h = dh;
    return 1;
}

int uit_output_res_setting(unsigned *w, unsigned *h) {
    const char *e = getenv("TOTK_OUTPUT_RES");
    unsigned cw = 1920, ch = 1080;
    if (!e || !*e || !strcmp(e, "0") || !uit_stricmp(e, "1080p") || !uit_stricmp(e, "off")) return 0;
    if (!uit_stricmp(e, "display")) {
        if (!uit_display_size(&cw, &ch)) {
            static int said; if (!said++) LOG("TOTK_OUTPUT_RES=display: the display's size could not be read: 1080p");
            return 0;
        }
        if (cw < 1280 || ch < 720 || cw > 7680 || ch > 4320) {
            static int said; if (!said++) LOG("TOTK_OUTPUT_RES=display: the display is %ux%u, outside 1280x720..7680x4320: 1080p", cw, ch);
            return 0;
        }
    }
    else if (!uit_stricmp(e, "1440p")) { cw = 2560; ch = 1440; }
    else if (!uit_stricmp(e, "2160p") || !uit_stricmp(e, "4k")) { cw = 3840; ch = 2160; }
    else if (!parse_wh(e, &cw, &ch) || cw < 1280 || ch < 720 || cw > 7680 || ch > 4320) {
        static int said; if (!said++) LOG("TOTK_OUTPUT_RES=%s is not 1080p, 1440p, 2160p, display or <w>x<h> within 1280x720..7680x4320: 1080p", e);
        return 0;
    }
    cw &= ~1u; ch &= ~1u;
    if (cw == 1920 && ch == 1080) return 0;
    *w = cw; *h = ch;
    return 1;
}

static volatile int heap_refused;
static int output_on = -1;   /* totk_output_res_on's decision, once made */
void uit_heap_refused(void) {
    heap_refused = 1;
    if (output_on == 1) LOG("WARNING: the texture heaps were refused after the canvas was already set larger: expect "
                            "missing textures; set Output resolution back to 1080p and report this log");
}

int uit_output_res(unsigned *w, unsigned *h) {
    unsigned cw, ch;
    if (!uit_output_res_setting(&cw, &ch)) return 0;
    if (heap_refused) {
        static int said; if (!said++) LOG("TOTK_OUTPUT_RES set, but the texture heaps could not be grown (see the romfix line): 1080p");
        return 0;
    }
    if (!main_output_res_patch) {
        static int said; if (!said++) LOG("TOTK_OUTPUT_RES set, but the output-size patch is not in this build (lifted before it, or the "
                                          "lift found other code there: see the lift's WARNING): 1080p");
        return 0;
    }
    *w = cw; *h = ch;
    return 1;
}

double uit_output_scale(void) {
    unsigned w, h;
    return uit_output_res(&w, &h) ? w / 1920.0 : 1.0;
}

double uit_aspect(void) {
    static double a = -1;
    if (a >= 0) return a;
    const char *e = getenv("TOTK_ASPECT");
    a = 0;
    if (e && *e && strcmp(e, "0") && uit_stricmp(e, "off") && uit_stricmp(e, "16:9")) {
        unsigned x = 0, y = 0; double v = 0; char rest;
        if ((sscanf(e, "%u:%u%c", &x, &y, &rest) == 2 || sscanf(e, "%ux%u%c", &x, &y, &rest) == 2) && x && y) v = (double)x / y;
        else if (sscanf(e, "%lf%c", &v, &rest) != 1) v = 0;
        if (v < 1.2 || v > 4.0) { LOG("TOTK_ASPECT=%s is not a ratio between 1.2 and 4 (21:9, 3440x1440, 2.37...): 16:9", e); v = 0; }
        if (v && fabs(v / (16.0 / 9.0) - 1) < 0.01) v = 0;   // 16:9 within 1 %: the game's own
        a = v;
    }
    return a;
}

/* the scene the settings ask for, before the game's canvas scaling (memory planning only; dlss.c decides the real one) */
void uit_scene_plan(unsigned *w, unsigned *h) {
    *w = *h = 0;
    if (heap_refused) return;
    unsigned cw = 1920, ch = 1080;
    uit_output_res(&cw, &ch);
    const char *rr = getenv("TOTK_RENDER_RES");
    if (rr && *rr && strcmp(rr, "auto")) {
        if (!strcmp(rr, "0")) return;
        if (!uit_stricmp(rr, "native")) { *w = cw; *h = ch; return; }
        unsigned x, y;
        if (parse_wh(rr, &x, &y) && x >= 320 && y >= 180 && x <= 7680 && y <= 4320) { *w = x; *h = y; }
        return;
    }
    const char *up = getenv("TOTK_UPSCALER");
    if (up && !uit_stricmp(up, "dlss")) {
        static const struct { const char *n; double s; } M[] = {{"dlaa", 0}, {"quality", 1 / 1.5}, {"balanced", 1 / 1.724},
                                                                 {"performance", 0.5}, {"ultraperf", 1 / 3.0}};
        const char *m = getenv("TOTK_DLSS_MODE"); double s = 1 / 1.5;
        if (m && *m) for (unsigned i = 0; i < 5; i++) if (!uit_stricmp(m, M[i].n)) s = M[i].s;
        if (s > 0) { *w = (unsigned)(cw * s) + 2; *h = (unsigned)(ch * s) + 2; }   // DLSS's optimal size can be a little larger
        return;
    }
    // the game's own 1600x900 on a bigger canvas grows with the canvas (counted by the canvas term)
}

uint64_t uit_heap_extra_kib_for(unsigned ow, unsigned oh, unsigned sw, unsigned sh) {
    double grow = 0;
    if ((double)ow * oh > 1920.0 * 1080.0) grow += (double)ow * oh / (1920.0 * 1080.0) - 1;
    int scene = (double)sw * sh > 1600.0 * 900.0;
    if (scene) grow += (double)sw * sh / (1600.0 * 900.0) - 1;
    if (grow <= 0) return 0;
    if (scene) grow *= 1.6;
    return (uint64_t)ceil(113664.0 * (1 + grow) / 1024.0) * 1024 - 113664;
}

/* [UIT 0070] TOTK_SHADOW_RES (Graphics > Detail and pop-in > Shadow resolution): 512, 1024, 2048 or 4096; 0 = the game's own */
unsigned uit_shadow_res_setting(void) {
    const char *e = getenv("TOTK_SHADOW_RES");
    if (!e || !*e || !strcmp(e, "-") || !strcmp(e, "0") || !uit_stricmp(e, "off") || !uit_stricmp(e, "game")) return 0;
    char rest;
    unsigned v;
    if (sscanf(e, "%u%c", &v, &rest) == 1 && (v == 512 || v == 1024 || v == 2048 || v == 4096)) return v;
    static int said; if (!said++) LOG("TOTK_SHADOW_RES=%s is not 512, 1024, 2048 or 4096: the game's own shadow size", e);
    return 0;
}

/* the shadow maps' extra memory: up to 6 square 32-bit layers (cascades and a margin) of the new size over the
   smallest size the game's own could be (512, so that a bigger own size only leaves room spare), whole MiB */
uint64_t uit_shadow_extra_kib_for(unsigned size) {
    if (size <= 512) return 0;
    uint64_t bytes = 6ull * 4 * ((uint64_t)size * size - 512ull * 512);
    return (bytes + (1u << 20) - 1) / (1u << 20) * 1024;
}

unsigned uit_shadow_res(void) { return main_shadow_patch ? uit_shadow_res_setting() : 0; }

/* [UIT 0154] TOTK_HEAP_EXTRA_MB (troubleshooting, empty = 0): this many MiB more in the texture heaps and the application
   pool on top of what the canvas, scene and shadow sizes need, 1..2048. With a 1080p canvas it gives the game the
   memory layout a 4K canvas has (cc3add4 field log: +811 MiB) without the bigger canvas or scene, to tell which of the
   two the floating ground follows. */
uint64_t uit_heap_extra_setting_kib(void) {
    const char *e = getenv("TOTK_HEAP_EXTRA_MB");
    if (!e || !*e || !strcmp(e, "-")) return 0;
    char rest;
    unsigned v;
    if (sscanf(e, "%u%c", &v, &rest) == 1 && v <= 2048) return (uint64_t)v * 1024;
    static int said; if (!said++) LOG("TOTK_HEAP_EXTRA_MB=%s is not 0..2048: no extra heap", e);
    return 0;
}

uint64_t uit_heap_extra_kib(void) {
    unsigned ow = 1920, oh = 1080, sw, sh;
    uit_output_res(&ow, &oh);
    uit_scene_plan(&sw, &sh);
    uint64_t add = uit_heap_extra_setting_kib();
    static int said;
    if (add && !said++) LOG("texture heaps +%llu MiB more (TOTK_HEAP_EXTRA_MB)", (unsigned long long)(add >> 10));
    return uit_heap_extra_kib_for(ow, oh, sw, sh) + uit_shadow_extra_kib_for(uit_shadow_res()) + add;
}

/* the largest scene (pixels, after the game's canvas scaling) the planned heaps hold: the planned scene, or the game's
   own 1600x900 grown with the canvas */
uint64_t uit_scene_budget(void) {
    unsigned ow = 1920, oh = 1080, sw, sh;
    uit_output_res(&ow, &oh);
    uit_scene_plan(&sw, &sh);
    double k = ow / 1920.0;
    uint64_t own = (uint64_t)(1600.0 * 900.0 * k * k), plan = (uint64_t)sw * sh;
    return plan > own ? plan : own;
}

/* the picture's shape: TOTK_ASPECT, else the canvas's own when TOTK_OUTPUT_RES is not 16:9 (0: 16:9) */
double uit_picture_aspect(void) {
    double a = uit_aspect();
    unsigned w, h;
    if (!a && uit_output_res(&w, &h)) {
        a = (double)w / h;
        if (fabs(a / (16.0 / 9.0) - 1) < 0.01) a = 0;
    }
    return a;
}

/* ---- the lift's run-time switches (tools/lifter/patches.py) ---- */
/* the canvas, packed as the game keeps it (h << 32 | w); set by totk_output_res_on before any site reads it */
uint64_t totk_output_size = (uint64_t)1080 << 32 | 1920;

int totk_output_res_on(void) {
    int on = output_on;
    if (on < 0) {
        unsigned w, h;
        int v = uit_output_res(&w, &h);
        if (v) {
            totk_output_size = (uint64_t)h << 32 | w;
            uint64_t kib = uit_heap_extra_kib();
            LOG("output canvas %ux%u (TOTK_OUTPUT_RES); texture heaps +%llu MiB", w, h, (unsigned long long)(kib >> 10));
        }
        on = output_on = v;
    }
    return on;
}

/* the ratio the camera's projection gets instead of 16:9 */
float totk_aspect_ratio;

int totk_aspect_on(void) {
    static int on = -1;
    if (on < 0) {
        double a = uit_picture_aspect();
        if (a && !main_aspect_patch) {
            LOG("aspect ratio %.4f asked, but the camera patch is not in this build (lifted before it, or the lift found "
                "other code there: see the lift's WARNING): 16:9, stretched to the window", a);
            a = 0;
        }
        if (a && a < 16.0 / 9.0) {   // Vert+ (16:10, 4:3) needs other UI layouts; MacNativeGames' are experimental
            LOG("aspect ratio %.4f is narrower than 16:9: not supported (only wider), 16:9", a);
            a = 0;
        }
        if (a) LOG("aspect ratio %.4f: the camera sees more at the sides (Hor+)", a);
        totk_aspect_ratio = (float)a;
        on = a > 0;
    }
    return on;
}

/* the shape the present pass gives the game's picture (0: the picture's own): the camera's ratio when the camera
   patch runs; a canvas that is not 16:9 without it holds a 16:9 view, which is shown at 16:9 so shapes stay round */
double uit_present_aspect(void) {
    if (totk_aspect_on()) return totk_aspect_ratio;
    unsigned w, h;
    if (uit_output_res(&w, &h) && fabs((double)w / h / (16.0 / 9.0) - 1) >= 0.01) return 16.0 / 9.0;
    return 0;
}
