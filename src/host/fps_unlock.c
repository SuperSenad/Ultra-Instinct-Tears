/* Frame-rate unlock (TOTK_FPS=30|60|120 in ultrainstincttears.cfg; default 30 = the game's own timing, nothing changes).
   From MacNativeGames games/totk/runtime/game_patches_v143.c (MIT). [port] marks the Windows changes: the game-code
   patches are in tools/lifter/patches.py (applied when the code is translated, each checked against the original
   instruction words, each choosing at run time between the new and the original instructions with the functions
   below), and the frame-step data the patched code reads is totk_frame_step[] here instead of a page after the module.

   What the target does:
   - TOTK_FPS=60 or 120: the game's adaptive present-interval controller (main+0x2db174) may choose 1 as well as 2..3,
     the present clock (src/gfx/nvn/hle_nvn.c present_cfg) ticks at TOTK_FPS, so interval 1 = TOTK_FPS, 2 = half of it;
     game time, cloth and hair advance by the measured frame time instead of by a fixed 1/30 s per frame; the trampoline
     bounce and the LOD / actor fade counters are corrected for short frames.
   - The measured step: the time between presents, averaged over the last 5, a hitch cut at 1/15 s per frame, gaps
     outside 1 ms..250 ms (loading, a paused window) dropped. Every value derives from the measured time, none from
     a fixed 1/60 or 1/120, so any target (and any frame the machine cannot hold) keeps the game at real speed.
   [UIT 0067] TOTK_FRAME_STEP=fixed: the fallback. Every frame advances game time, cloth and hair by exactly 1/TOTK_FPS
   (1/60 s at 60), the way a console 60 FPS patch does: steady when the machine holds the target, but the game runs
   slow whenever it cannot (at 45 FPS the game runs at 75 % speed). measured (default) is the measured step above.
   Opt-outs for comparison (with TOTK_FPS above 30): TOTK_REALTIME=0 (the game clock stays per frame: the game runs
   fast), TOTK_CLOTH_REALTIME=0, TOTK_HIGHFPS_FIX=0 (bounce and fade), TOTK_FADE_FIX=0. */
#include <math.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "uit_live.h"

/* {seconds * 30 (the 30 Hz ratio), seconds, seconds * 60, unused}; read by the translated game code (patches.py) */
_Alignas(16) float totk_frame_step[4];

static _Atomic int g_fps = -1;
static int off(const char *name) { const char *e = getenv(name); return e && !strcmp(e, "0"); }

static void frame_step_store(float seconds) {
    float values[2] = {seconds * 30.0f, seconds};
    uint64_t bits; memcpy(&bits, values, 8);
    float sixty = seconds * 60.0f; uint32_t b60; memcpy(&b60, &sixty, 4);
    atomic_store_explicit((_Atomic uint64_t *)(void *)totk_frame_step, bits, memory_order_release);
    atomic_store_explicit((_Atomic uint32_t *)(void *)(totk_frame_step + 2), b60, memory_order_release);
}

/* [UIT 0067] TOTK_FRAME_STEP: measured (default) or fixed; read once */
static int frame_step_fixed(void) {
    static _Atomic int s = -1;
    int v = atomic_load_explicit(&s, memory_order_relaxed);
    if (v < 0) {
        const char *e = getenv("TOTK_FRAME_STEP");
        v = e && !strcmp(e, "fixed");
        if (e && *e && !v && strcmp(e, "measured"))
            fprintf(stderr, "[totk-fps] TOTK_FRAME_STEP=%s is not measured or fixed: measured\n", e);
        atomic_store_explicit(&s, v, memory_order_relaxed);
    }
    return v;
}

/* TOTK_FPS, read once (ultrainstincttears.cfg is applied in main, before the game's first frame). */
int totk_fps_target(void) {
    int v = atomic_load_explicit(&g_fps, memory_order_acquire);
    if (v >= 0) return v;
    const char *e = getenv("TOTK_FPS");
    v = 30;
    if (e && *e) {
        int n = atoi(e);
        if (n == 30 || n == 60 || n == 120) v = n;
        else fprintf(stderr, "[totk-fps] TOTK_FPS=%s is not 30, 60 or 120: using 30 (the game's own timing)\n", e);
    }
    int expected = -1;
    if (atomic_compare_exchange_strong(&g_fps, &expected, v)) {
        frame_step_store(1.0f / (float)v);
        if (v > 30)
            fprintf(stderr, "[totk-fps] target %d fps: 60 FPS controller patch, real-time game clock%s%s%s%s%s\n", v,
                    frame_step_fixed() ? " (fixed step 1/target, TOTK_FRAME_STEP=fixed: slow when frames run long)" : " (measured step)",
                    off("TOTK_REALTIME") ? " OFF (TOTK_REALTIME=0: the game will run fast)" : "",
                    !off("TOTK_REALTIME") && !off("TOTK_CLOTH_REALTIME") ? ", cloth/hair clock" : "",
                    !off("TOTK_HIGHFPS_FIX") ? ", bounce fix" : "",
                    !off("TOTK_HIGHFPS_FIX") && !off("TOTK_FADE_FIX") ? ", fade fix" : "");
        if (v > 60)
            fprintf(stderr, "[totk-fps] 120 is experimental: physics, UI timers and some effects may misbehave above 60 "
                            "(see BUILD-WINDOWS-FSR.md)\n");
    } else v = expected;
    return v;
}

/* [UIT 0170] The in-game overlay: 60 <-> 120 while the game runs. Only when the run started above 30: the game
   patches below (60 FPS controller, real-time clock, bounce / fade fixes) are chosen once from "above 30", and stay
   chosen; between 60 and 120 only the target itself changes (the present clock, hle_nvn.c nvn_fps_retarget, and the
   fixed frame step; the measured step follows by itself). 30 <-> 60 / 120 needs a restart. */
static _Atomic int g_fps_start = -1;
int totk_fps_start(void) {
    int v = atomic_load_explicit(&g_fps_start, memory_order_acquire);
    if (v < 0) { v = totk_fps_target(); int e = -1; if (!atomic_compare_exchange_strong(&g_fps_start, &e, v)) v = e; }
    return v;
}
int totk_fps_retarget(int v) {
    if (totk_fps_start() <= 30 || (v != 60 && v != 120)) return 0;
    if (atomic_exchange(&g_fps, v) == v) return 1;
    if (frame_step_fixed()) frame_step_store(1.0f / (float)v);
    fprintf(stderr, "[totk-fps] target %d fps (overlay)\n", v);
    return 1;
}

/* Run-time switches of the translated patches (tools/lifter/patches.py). Cached: they run inside game code. */
#define SWITCH(name, expr) \
    int name(void) { static _Atomic int s = -1; int v = atomic_load_explicit(&s, memory_order_relaxed); \
                     if (v < 0) { v = (expr) ? 1 : 0; atomic_store_explicit(&s, v, memory_order_relaxed); } return v; }
SWITCH(totk_fps_controller, totk_fps_target() > 30)
SWITCH(totk_fps_realtime, totk_fps_target() > 30 && !off("TOTK_REALTIME"))
SWITCH(totk_fps_cloth, totk_fps_realtime() && !off("TOTK_CLOTH_REALTIME"))
SWITCH(totk_fps_highfps_fix, totk_fps_target() > 30 && !off("TOTK_HIGHFPS_FIX"))
SWITCH(totk_fps_fade_fix, totk_fps_highfps_fix() && !off("TOTK_FADE_FIX"))

/* Called at every present (hle_nvn.c nvnQueuePresentTexture, the game's thread). */
static uint64_t last_present_ns;
static double frame_samples[5];
static unsigned frame_sample_count, frame_sample_next;
void runtime_frame_step_update(uint64_t now_ns) {
    if (!totk_fps_realtime() || !now_ns || frame_step_fixed()) return;   /* fixed: 1/TOTK_FPS stays (stored above) */
    if (!last_present_ns) { last_present_ns = now_ns; return; }
    double dt = (double)(now_ns - last_present_ns) * 1e-9; last_present_ns = now_ns;
    if (dt > 0.25 || dt < 0.001) { frame_sample_count = frame_sample_next = 0; return; }
    if (dt > 1.0 / 15.0) dt = 1.0 / 15.0;   /* a hitch: at most 1/15 s of game time per frame (MacNativeGames) */
    frame_samples[frame_sample_next++ % 5] = dt; if (frame_sample_count < 5) frame_sample_count++;
    double sum = 0; for (unsigned i = 0; i < frame_sample_count; i++) sum += frame_samples[i];
    frame_step_store((float)(sum / frame_sample_count));
}
