// [UIT 0290] Stick deadzones and [UIT 0291] the trigger press point, applied to the controller in window_sdl.c's
// pad_update before the game sees it. Portable, header only: tools/tests/test_stick_dz.c checks the math.
//
// Radial scaled deadzone: m = the stick's distance from the centre (0..1 at the gate; up to ~1.4 in a square gate's
// corner). Below the inner deadzone the stick reads centred; from there the distance is rescaled so the output starts
// at 0 (no jump) and reaches 1 at 1 - outer (the outer deadzone: full tilt a little before the edge). The direction is
// kept. Past a distance of 1 (the corners some controllers report) the input is passed as it is, so with both at 0
// nothing changes at all (the function returns at once), and with any setting there is no jump at the edge:
//   r = min((m - inner) / (1 - inner - outer), max(m, 1))
// Cost: one sqrt and a few float operations per stick per poll, only when a deadzone is set.
#pragma once
#include <math.h>
#include <stdint.h>

#define UIT_STICK_DZ_MAX 50     /* % inner */
#define UIT_STICK_OUTER_MAX 30  /* % outer */

// x, y: the stick in -32767..32767 (as pad_update has them). inner, outer: fractions (0..0.5, 0..0.3).
static inline void uit_stick_dz(int32_t *x, int32_t *y, float inner, float outer) {
    if (inner <= 0.f && outer <= 0.f) return;
    if (inner < 0.f) inner = 0.f;
    if (outer < 0.f) outer = 0.f;
    if (inner > UIT_STICK_DZ_MAX / 100.f) inner = UIT_STICK_DZ_MAX / 100.f;
    if (outer > UIT_STICK_OUTER_MAX / 100.f) outer = UIT_STICK_OUTER_MAX / 100.f;
    float fx = (float)*x / 32767.f, fy = (float)*y / 32767.f;
    float m = sqrtf(fx * fx + fy * fy);
    if (m <= inner) { *x = 0; *y = 0; return; }
    float r = (m - inner) / (1.f - inner - outer), cap = m > 1.f ? m : 1.f;
    if (r > cap) r = cap;
    float k = r / m * 32767.f;
    long ox = lroundf(fx * k), oy = lroundf(fy * k);
    *x = ox > 32767 ? 32767 : ox < -32767 ? -32767 : (int32_t)ox;
    *y = oy > 32767 ? 32767 : oy < -32767 ? -32767 : (int32_t)oy;
}

// A trigger counts as pressed above this axis value (0..32767). pct 50 = 16384, the threshold before 0291.
static inline int uit_trigger_threshold(int pct) {
    if (pct < 5) pct = 5;
    if (pct > 95) pct = 95;
    return pct * 32768 / 100;
}
