// DLSS settings, the scene-size override and the on/off switch (see dlss.h). Streamline itself: dlss_sl.c.
#include "dlss.h"
#include "../host/uit_display.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <windows.h>

#define LOG(...) (fprintf(stderr, "[totk-dlss] " __VA_ARGS__), fputc('\n', stderr))

static const char *const MODES[] = {"dlaa", "quality", "balanced", "performance", "ultraperf"};
// The scene size over the 1920x1080 canvas DLSS writes, per mode (NVIDIA's ratios: 1/1.5, 1/1.72, 1/2, 1/3). DLAA keeps
// the game's own size (1600x896 docked: DLSS anti-aliases and scales by 1.2).
static const float SCALE[] = {0.0f, 1.0f / 1.5f, 1.0f / 1.724f, 0.5f, 1.0f / 3.0f};

static int env_is(const char *name, const char *v) { const char *e = getenv(name); return e && !_stricmp(e, v); }

int dlss_upscaler(void) {
    static int u = -2;
    if (u != -2) return u;
    const char *e = getenv("TOTK_UPSCALER");
    if (!e || !*e) u = UPSCALER_LEGACY;
    else if (!_stricmp(e, "dlss")) u = UPSCALER_DLSS;
    else if (!_stricmp(e, "fsr")) u = UPSCALER_FSR;
    else if (!_stricmp(e, "off") || !strcmp(e, "0")) u = UPSCALER_OFF;
    else { LOG("TOTK_UPSCALER=%s is not fsr, dlss or off: ignored", e); u = UPSCALER_LEGACY; }
    return u;
}

int dlss_mode(void) {
    static int m = -1;
    if (m >= 0) return m;
    m = DLSS_QUALITY;
    const char *e = getenv("TOTK_DLSS_MODE");
    if (e && *e) {
        int found = 0;
        for (int i = 0; i < 5; i++) if (!_stricmp(e, MODES[i])) { m = i; found = 1; }
        if (env_is("TOTK_DLSS_MODE", "ultraperformance") || env_is("TOTK_DLSS_MODE", "ultra_performance")) { m = DLSS_ULTRAPERF; found = 1; }
        if (!found) LOG("TOTK_DLSS_MODE=%s is not dlaa, quality, balanced, performance or ultraperf: quality", e);
    }
    return m;
}

const char *dlss_mode_name(int mode) { return mode >= 0 && mode < 5 ? MODES[mode] : "?"; }

// 1 when the lift applied the render-size patch (tools/lifter/patches.py writes it into main_table.c). 0: the words at
// main+0x585e48.. were not the 1.4.3 ones and the lift kept the game's code, so the scene stays at the game's own size
// (1600x896) and every mode runs as DLAA (Streamline is told the ratio it really gets).
extern const int main_render_res_patch;

// the mode Streamline runs: the one asked, or DLAA when the scene cannot be resized
static int run_mode(void) { return main_render_res_patch ? dlss_mode() : DLSS_DLAA; }

// TOTK_DLSS_PRESET=auto (default: NVIDIA's recommended transformer models, K for DLAA/Quality/Balanced, M for
// Performance, L for Ultra Performance), default (the driver's choice) or a letter J..O.
static int preset(void) {
    static const int REC[] = {11, 11, 11, 13, 12};
    static int p = -1;
    if (p >= 0) return p;
    const char *e = getenv("TOTK_DLSS_PRESET");
    p = REC[run_mode()];
    if (!e || !*e || !_stricmp(e, "auto")) return p;
    if (!_stricmp(e, "default")) p = 0;
    else if (strlen(e) == 1 && (*e | 32) >= 'j' && (*e | 32) <= 'o') p = 10 + (*e | 32) - 'j';
    else LOG("TOTK_DLSS_PRESET=%s is not auto, default or J..O: auto", e);
    return p;
}

static atomic_int g_state;        // 0 not tried, 1 Streamline loaded, 2 DLSS supported, -1 none
static atomic_int g_off;          // F10
static atomic_int g_failures;

PFN_vkGetInstanceProcAddr dlss_vk_loader(PFN_vkGetInstanceProcAddr loader) {
    const char *rr = getenv("TOTK_RENDER_RES");
    if (!main_render_res_patch && rr && *rr && strcmp(rr, "auto") && strcmp(rr, "0"))
        LOG("TOTK_RENDER_RES=%s ignored: render-res patch not applied (the lift skipped it)", rr);
    if (dlss_upscaler() != UPSCALER_DLSS) { atomic_store(&g_state, -1); return loader; }
    if (!dlss_sl_built()) {
        LOG("TOTK_UPSCALER=dlss, but this build was made with --no-dlss: FSR instead (run setup.py again without --no-dlss)");
        atomic_store(&g_state, -1);
        return loader;
    }
    wchar_t dir[MAX_PATH];
    DWORD n = GetModuleFileNameW(NULL, dir, MAX_PATH);
    while (n && dir[n - 1] != L'\\' && dir[n - 1] != L'/') n--;
    dir[n ? n - 1 : 0] = 0;
    PFN_vkGetInstanceProcAddr sl = dlss_sl_load(dir);
    if (!sl) { atomic_store(&g_state, -1); LOG("DLSS not available: FSR instead"); return loader; }
    atomic_store(&g_state, 1);
    return sl;
}

void dlss_device_created(VkPhysicalDevice gpu) {
    if (atomic_load(&g_state) != 1) return;
    int ok = dlss_sl_supported(gpu);
    atomic_store(&g_state, ok ? 2 : -1);
    if (ok) LOG("DLSS ready: mode %s, preset %c (F10 turns it off and on)", MODES[dlss_mode()], preset() ? 'A' + preset() - 1 : '-');
    if (ok && !main_render_res_patch)
        LOG("render-res patch not applied: DLAA size (the lift skipped it, see the lift's WARNING / main.log): the scene "
            "stays at the game's own size%s", dlss_mode() != DLSS_DLAA ? "; the mode asked runs as DLAA" : "");
    // (0014 and before: this else hung off the render-res check, so "not supported ... FSR instead" was printed right
    // after "DLSS ready" whenever the patch was applied, though DLSS was on: the 720p Quality scene size was DLSS's.)
    if (!ok) LOG("DLSS not supported on this GPU or driver (an NVIDIA RTX GPU and a current driver are needed): FSR instead");
}

int dlss_available(void) { return atomic_load(&g_state) == 2; }
int dlss_active(void) { return dlss_available() && !atomic_load(&g_off); }
void dlss_toggle(void) {
    if (!dlss_available()) { LOG("F10: DLSS is not available (%s)", dlss_upscaler() == UPSCALER_DLSS ? "see the log above" : "TOTK_UPSCALER is not dlss"); return; }
    int off = !atomic_fetch_xor(&g_off, 1) ? 1 : 0;
    LOG("DLSS %s (F10)%s", off ? "off: the game's own upscale" : "on", off && run_mode() != DLSS_DLAA ? "; the scene stays at the DLSS size" : "");
}
// Five failed evaluates in a row (each followed by ~5 s of back-off, gfx/vkb_dlss.c): DLSS is given up for the run, and
// the scene size goes back to the game's own where it was DLSS's (totk_render_res_on), so the fallback is not a 720p
// scene upscaled by the game's filter.
void dlss_note_failure(void) {
    if (atomic_fetch_add(&g_failures, 1) + 1 == 5 && atomic_load(&g_state) == 2) {
        atomic_store(&g_state, -1);
        LOG("DLSS failed 5 times in a row (the reason is logged above): off for this run, the game's own upscale at its "
            "own scene size from here on (TOTK_UPSCALER=fsr skips DLSS)");
    }
}

int dlss_evaluate(const dlss_frame *f) {
    if (!dlss_active()) return 0;
    int ok = dlss_sl_evaluate(f, run_mode(), preset());
    if (ok) atomic_store(&g_failures, 0);
    return ok;
}

// ---------------------------------------------------------------- the scene size (TOTK_RENDER_RES)
// The game scales the size it reads by canvas width / 1920 (MacNativeGames game_patches_v143.c, render_res_publish),
// so the value written is the wanted size divided by that (the docked 1920x1080 canvas: the size itself; a 2560x1440
// canvas, TOTK_OUTPUT_RES: 3/4 of it). Decided once, when the renderer starts;
// when DLSS is given up later (dlss_note_failure) the switch answers 0 again, so where the game picks its size again it
// gets its own (where it does not, the scene stays at the DLSS size until the next start; TOTK_UPSCALER=fsr then).
// [UIT 0068] TOTK_RENDER_RES=native: the scene at the canvas size, so the game's own upscale (its FSR 1 pass) is 1:1
// and only the port's upscaler / DLSS runs. The limit is what the texture heaps were sized for (uit_display.c, which
// grows them for a bigger canvas or scene: src/host/fs.c HeapDef, src/host/svc.c application pool).
float totk_render_size[2];
static atomic_int g_rr = -1;

static int parse_size(const char *e, unsigned *w, unsigned *h) {
    char rest;
    return sscanf(e, "%ux%u%c", w, h, &rest) == 2 && *w >= 320 && *h >= 180 && *w <= 7680 && *h <= 4320;
}

int dlss_render_size(unsigned *w, unsigned *h) {
    unsigned cw = 1920, ch = 1080;
    uit_output_res(&cw, &ch);
    const char *e = getenv("TOTK_RENDER_RES");
    if (e && *e && strcmp(e, "auto")) {
        if (!strcmp(e, "0")) return 0;
        if (!_stricmp(e, "native")) { *w = cw; *h = ch; }
        else if (!parse_size(e, w, h)) { LOG("TOTK_RENDER_RES=%s is not native or <w>x<h> within 320x180..7680x4320: ignored", e); return 0; }
    } else {
        if (!dlss_available() || dlss_mode() == DLSS_DLAA) return 0;
        float s = SCALE[dlss_mode()];
        *w = ((unsigned)(cw * s + 1)) & ~1u;
        *h = ((unsigned)(ch * s + 1)) & ~1u;
        uint32_t ow = 0, oh = 0;
        if (dlss_sl_optimal(dlss_mode(), cw, ch, &ow, &oh) && ow >= 320 && oh >= 180 && ow <= cw && oh <= ch) { *w = ow & ~1u; *h = oh & ~1u; }
    }
    // the scene targets live in heaps sized for the planned scene (1600x900 docked, grown with the canvas)
    uint64_t budget = uit_scene_budget();
    if ((uint64_t)*w * *h > budget) {
        LOG("render size %ux%u is more than the %llu pixels the game's memory is sized for: the game's own", *w, *h,
            (unsigned long long)budget);
        return 0;
    }
    return 1;
}

int totk_render_res_on(void) {
    const char *rr = getenv("TOTK_RENDER_RES");
    int explicit_size = rr && *rr && strcmp(rr, "auto");
    int v = atomic_load_explicit(&g_rr, memory_order_acquire);
    if (v >= 0) return v && (explicit_size || atomic_load(&g_state) == 2);   // DLSS given up: the game's own size again

    int st = atomic_load(&g_state);
    if (!explicit_size && st >= 0 && st < 2 && dlss_upscaler() == UPSCALER_DLSS) return 0;   // the device is not up yet: ask again
    unsigned w = 0, h = 0;
    int on = dlss_render_size(&w, &h);
    if (on) {
        double k = uit_output_scale();   // the game multiplies what it reads by this
        totk_render_size[0] = (float)(w / k);
        totk_render_size[1] = (float)(h / k);
        LOG("scene render size %ux%u (%s)", w, h, getenv("TOTK_RENDER_RES") && *getenv("TOTK_RENDER_RES") ? "TOTK_RENDER_RES" : dlss_mode_name(dlss_mode()));
        unsigned ow = 1920, oh = 1080;
        uit_output_res(&ow, &oh);
        if (explicit_size && dlss_upscaler() == UPSCALER_DLSS && w >= ow && h >= oh)   // [UIT 0239]
            LOG("[UIT 0239] DLSS will not run: the scene (TOTK_RENDER_RES=%s, %ux%u) is as large as the output (%ux%u), so there is nothing "
                "to upscale and the game's own 1:1 copy is kept (the log says \"the game's upscale draw kept (textures)\"): the whole frame is drawn at the "
                "output size without DLSS's anti-aliasing. For DLSS, clear \"Scene resolution\" (Graphics > Image quality > Scene resolution, in the "
                "folded Image quality group: open it) and pick a DLSS mode (quality: about %ux%u, the GPU's biggest saving)", rr, w, h, ow, oh,
                ((unsigned)(ow / 1.5 + 1)) & ~1u, ((unsigned)(oh / 1.5 + 1)) & ~1u);
    }
    int expect = -1;
    if (!atomic_compare_exchange_strong(&g_rr, &expect, on)) return atomic_load(&g_rr);
    return on;
}
