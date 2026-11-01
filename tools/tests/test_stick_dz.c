// [UIT 0290] / [UIT 0291] src/host/stick_dz.h: the radial scaled stick deadzone and the trigger press point.
// cc -O2 -I src/host tools/tests/test_stick_dz.c -lm && ./a.out ; -DPROBE=1 uses an unscaled deadzone (must fail).
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include "stick_dz.h"

#ifdef PROBE   // negative probe: a plain cut-off (no rescale) jumps at the inner edge and must be caught
#undef uit_stick_dz
static inline void probe_dz(int32_t *x, int32_t *y, float in, float out) {
    (void)out;
    if (in <= 0) return;
    float fx = *x / 32767.f, fy = *y / 32767.f;
    if (sqrtf(fx * fx + fy * fy) <= in) *x = *y = 0;
}
#define uit_stick_dz probe_dz
#endif

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { if (fails++ < 12) { printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } } while (0)

static double mag(int32_t x, int32_t y) { return sqrt((double)x * x + (double)y * y) / 32767.0; }

int main(void) {
    // 1. both 0: every input passes untouched (the default changes nothing), corners included
    for (int x = -32767; x <= 32767; x += 97)
        for (int y = -32767; y <= 32767; y += 389) {
            int32_t a = x, b = y;
            uit_stick_dz(&a, &b, 0, 0);
            CHECK(a == x && b == y, "0/0 changed %d,%d to %d,%d", x, y, a, b);
        }
    static const float ins[] = {0.02f, 0.05f, 0.1f, 0.2f, 0.35f, 0.5f}, outs[] = {0, 0.05f, 0.1f, 0.3f};
    for (unsigned i = 0; i < sizeof ins / sizeof *ins; i++)
        for (unsigned o = 0; o < sizeof outs / sizeof *outs; o++) {
            float in = ins[i], out = outs[o];
            for (int deg = 0; deg < 360; deg += 7) {
                double c = cos(deg * M_PI / 180), s = sin(deg * M_PI / 180), prev = 0;
                for (int k = 0; k <= 1000; k++) {   // 2. along a ray from the centre to the gate (m = 0..1)
                    double m = k / 1000.0;
                    int32_t x = (int32_t)lround(m * c * 32767), y = (int32_t)lround(m * s * 32767), x0 = x, y0 = y;
                    double mi = mag(x, y);
                    uit_stick_dz(&x, &y, in, out);
                    double r = mag(x, y);
                    if (mi <= in) CHECK(x == 0 && y == 0, "inside %.3f: %d,%d (in %.2f)", mi, x, y, in);
                    double want = fmin((mi - in) / (1 - in - out), 1.0);
                    if (mi > in) CHECK(fabs(r - want) < 1.5e-4, "m %.4f in %.2f out %.2f: %.5f want %.5f", mi, in, out, r, want);
                    CHECK(r <= 1.0 + 1e-4, "over 1: %.5f", r);
                    CHECK(r >= prev - 1e-4, "not monotone at m %.4f: %.5f < %.5f", mi, r, prev);   // 3. no step back
                    CHECK(r - prev < 2.0 / (1000 * (1 - in - out)) + 1e-3, "jump at m %.4f: %.5f -> %.5f", mi, prev, r);   // 4. no jump
                    if (r > 1e-3) {   // 5. the direction is kept (to the integer rounding)
                        double cross = (double)x * y0 - (double)y * x0, dot = (double)x * x0 + (double)y * y0;
                        CHECK(fabs(atan2(cross, dot)) < 1.0 / (r * 32767) + 1e-4, "direction moved at %d deg m %.3f", deg, mi);
                    }
                    if (mi >= 1 - out + 1e-4) CHECK(fabs(r - 1) < 1.5e-4, "full tilt at m %.4f (out %.2f): %.5f", mi, out, r);
                    prev = r;
                }
                // 6. past the gate (square-gate corners): continuous at 1, then passed as it is
                for (double m = 1.0; m <= 1.40; m += 0.01) {
                    double fx = m * c, fy = m * s;
                    if (fabs(fx) > 1 || fabs(fy) > 1) continue;
                    int32_t x = (int32_t)lround(fx * 32767), y = (int32_t)lround(fy * 32767);
                    double mi = mag(x, y);
                    uit_stick_dz(&x, &y, in, out);
                    CHECK(fabs(mag(x, y) - fmax(mi, 1.0)) < 2e-4, "corner m %.3f: %.5f", mi, mag(x, y));
                }
            }
        }
    // 7. out-of-range settings are clamped, never divide by zero or produce NaN
    int32_t x = 30000, y = 0;
    uit_stick_dz(&x, &y, 5.f, 5.f);
    CHECK(x >= 0 && x <= 32767 && y == 0, "clamped settings: %d,%d", x, y);
    x = -32767, y = -32767;
    uit_stick_dz(&x, &y, 0.5f, 0.3f);
    CHECK(x == -32767 && y == -32767, "corner with max settings: %d,%d", x, y);
    // 8. the trigger press point: 50 % is the old fixed threshold; the range is clamped and increasing
    CHECK(uit_trigger_threshold(50) == 16384, "50%% = %d", uit_trigger_threshold(50));
    CHECK(uit_trigger_threshold(0) == uit_trigger_threshold(5) && uit_trigger_threshold(100) == uit_trigger_threshold(95), "clamp");
    for (int p = 5; p < 95; p += 5) CHECK(uit_trigger_threshold(p) < uit_trigger_threshold(p + 5), "not increasing at %d", p);
    CHECK(uit_trigger_threshold(95) < 32767, "95%% reachable");
    printf("test_stick_dz: %d checks, %d failed%s\n", checks, fails,
#ifdef PROBE
           " (negative probe)"
#else
           ""
#endif
    );
    return fails != 0;
}
