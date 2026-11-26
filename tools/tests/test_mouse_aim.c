// [UIT 0451] src/host/mouse_aim.h: the mouse camera's stick. The game is modelled as a stick integrator (it turns by
// stick * its top speed * the frame's time), so a turn is measured in "seconds of full tilt".
// cc -O2 -I src/host tools/tests/test_mouse_aim.c -lm && ./a.out ; -DPROBE=1 maps counts per frame with no frame time
// (what a plain "mouse = stick" mapping does) and must fail the frame-rate checks.
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include "mouse_aim.h"

#ifdef PROBE
static inline void probe_latch(UitMouseAim *m, int64_t now, const UitMouseCfg *c) {
    m->last_ns = now;
    m->rx = uit_mouse_axis(fmin(1.0, m->ax * c->sens / 60.0));
    m->ry = uit_mouse_axis(fmin(1.0, -m->ay * c->sens / 60.0));
    m->ax = m->ay = 0;
}
#define uit_mouse_latch probe_latch
#endif

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { if (fails++ < 16) { printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } } while (0)

static uint32_t rng = 12345;
static double frand(void) { rng = rng * 1664525u + 1013904223u; return (rng >> 8) / 16777216.0; }

// A mouse moving at vx, vy counts/s, reported at 1000 Hz, for secs; game frames at fps (jitter: share of a frame);
// the game reads the stick once a frame and turns stick * frame time (jitter 0: every frame as long). Returns the turn (x, y) in full-tilt seconds.
static void run(double fps, double jitter, double vx, double vy, double secs, double tail, const UitMouseCfg *c, double *tx, double *ty, int *maxabs) {
    UitMouseAim m = {0};
    int64_t t = 1000000000LL, end = t + (int64_t)(secs * 1e9), stop = end + (int64_t)(tail * 1e9), next_ev = t, last_frame = t;
    uit_mouse_latch(&m, t, c);   // the first frame only starts the clock
    *tx = *ty = 0;
    if (maxabs) *maxabs = 0;
    double ex = 0, ey = 0;
    while (t < stop) {
        int64_t frame = (int64_t)(1e9 / fps * (1 + jitter * (frand() * 2 - 1)));
        int64_t tn = t + frame;
        for (; next_ev < tn; next_ev += 1000000) {   // the 1000 Hz mouse reports whole counts
            if (next_ev >= end) continue;
            ex += vx / 1000.0; ey += vy / 1000.0;
            double ix = trunc(ex), iy = trunc(ey);
            ex -= ix; ey -= iy;
            if (ix || iy) uit_mouse_add(&m, ix, iy);
        }
        t = tn;
        uit_mouse_latch(&m, t, c);
        double dt = (t - last_frame) / 1e9;   // the game turns by this frame's stick * the frame's time
        last_frame = t;
        *tx += m.rx / 32767.0 * dt;
        *ty += m.ry / 32767.0 * dt;
        if (maxabs) { int a = abs(m.rx) > abs(m.ry) ? abs(m.rx) : abs(m.ry); if (a > *maxabs) *maxabs = a; }
    }
}

int main(void) {
    UitMouseCfg c = {.sens = 1.0};
    // 1. the turn follows the distance the mouse moved, at any frame rate (no jitter: the game's frame time is the
    //    time between reads); 2000 counts/s for 1 s = half of full tilt for 1 s = 0.5 full-tilt seconds
    double fpss[] = {30, 60, 120, 144, 240};
    for (unsigned i = 0; i < sizeof fpss / sizeof *fpss; i++) {
        double tx, ty;
        run(fpss[i], 0, 2000, -1000, 1.0, 0.2, &c, &tx, &ty, NULL);
        double want_x = 2000 / UIT_MOUSE_FULL_RATE, want_y = 1000 / UIT_MOUSE_FULL_RATE;
        CHECK(fabs(tx - want_x) < 0.01 * want_x && fabs(ty - want_y) < 0.01 * want_y,
              "%g FPS: turned %.4f, %.4f, want %.4f, %.4f", fpss[i], tx, ty, want_x, want_y);
    }
    // 2. sensitivity scales the turn linearly; slow and fast moves of the same distance turn the same (no curve)
    {
        double a, b, d1, d2, y;
        UitMouseCfg c2 = {.sens = 2.0};
        run(60, 0, 1000, 0, 1.0, 0.2, &c, &a, &y, NULL);
        run(60, 0, 1000, 0, 1.0, 0.2, &c2, &b, &y, NULL);
        CHECK(fabs(b - 2 * a) < 0.01 * a, "sensitivity 2 turned %.4f, 1 turned %.4f", b, a);
        run(60, 0, 250, 0, 2.0, 0.2, &c, &d1, &y, NULL);    // 500 counts slowly
        run(60, 0, 2500, 0, 0.2, 0.2, &c, &d2, &y, NULL);   // the same 500 counts fast
        CHECK(fabs(d1 - d2) < 0.02 * d1, "500 counts slow turned %.4f, fast %.4f", d1, d2);
    }
    // 3. no deadzone: one count in one frame moves the stick, in the right direction
    {
        UitMouseAim m = {0};
        uit_mouse_latch(&m, 1000000000LL, &c);
        uit_mouse_add(&m, 1, 0);
        uit_mouse_latch(&m, 1000000000LL + 16666667, &c);
        CHECK(m.rx > 0 && m.ry == 0, "one count: rx %d ry %d", m.rx, m.ry);
        uit_mouse_add(&m, 0, -3);   // the mouse forward (y up on the desk is negative): the stick up
        uit_mouse_latch(&m, 1000000000LL + 2 * 16666667, &c);
        CHECK(m.ry > 0 && m.rx == 0, "forward: rx %d ry %d", m.rx, m.ry);
        UitMouseCfg ci = {.sens = 1.0, .invert_y = 1};
        uit_mouse_add(&m, 0, -3);
        uit_mouse_latch(&m, 1000000000LL + 3 * 16666667, &ci);
        CHECK(m.ry < 0, "inverted forward: ry %d", m.ry);
        uit_mouse_latch(&m, 1000000000LL + 4 * 16666667, &c);   // no motion: centred at once
        CHECK(m.rx == 0 && m.ry == 0, "still: rx %d ry %d", m.rx, m.ry);
    }
    // 4. a flick past full tilt is clipped there and carried: the whole turn arrives a little later, never more
    {
        double tx, ty;
        int mx;
        run(60, 0, 12000, 0, 0.05, 0.5, &c, &tx, &ty, &mx);   // 600 counts in 50 ms: 3x full tilt
        double want = 600 / UIT_MOUSE_FULL_RATE;
        CHECK(fabs(tx - want) < 0.03 * want && mx == 32767, "flick turned %.4f want %.4f (peak %d)", tx, want, mx);
        run(60, 0, 400000, 0, 0.05, 1.0, &c, &tx, &ty, &mx);   // 20000 counts in 50 ms: the carry is capped
        CHECK(tx <= 0.05 + UIT_MOUSE_CARRY_S + 0.04 && tx > UIT_MOUSE_CARRY_S, "huge flick turned %.4f (cap %.2f s)", tx, UIT_MOUSE_CARRY_S);
    }
    // 5. a gap (loading, pause) drops what was gathered: nothing turns on its own afterwards
    {
        UitMouseAim m = {0};
        uit_mouse_latch(&m, 1000000000LL, &c);
        uit_mouse_add(&m, 500, 500);
        uit_mouse_latch(&m, 1000000000LL + UIT_MOUSE_IDLE_NS + 1, &c);
        CHECK(m.rx == 0 && m.ry == 0 && m.ax == 0 && m.cx == 0, "after a gap: rx %d ry %d", m.rx, m.ry);
        uit_mouse_add(&m, NAN, 3);   // a bad value is ignored
        CHECK(m.ax == 0 && m.ay == 0, "NaN taken");
    }
    // 6. the slow movement boost: a moving mouse pushes at least that much, a still one not at all; full stays full
    {
        UitMouseCfg cb = {.sens = 1.0, .min_push = 0.1};
        UitMouseAim m = {0};
        uit_mouse_latch(&m, 1000000000LL, &cb);
        uit_mouse_add(&m, 1, 0);
        uit_mouse_latch(&m, 1000000000LL + 16666667, &cb);
        CHECK(m.rx >= 3276 && m.rx < 4500, "boosted one count: %d", m.rx);
        uit_mouse_latch(&m, 1000000000LL + 2 * 16666667, &cb);
        CHECK(m.rx == 0, "boost with no motion: %d", m.rx);
        uit_mouse_add(&m, 100000, 0);
        uit_mouse_latch(&m, 1000000000LL + 3 * 16666667, &cb);
        CHECK(m.rx == 32767, "boost at full: %d", m.rx);
    }
    // 7. many reports in a frame make the same stick as one report of their sum
    {
        UitMouseAim a = {0}, b = {0};
        uit_mouse_latch(&a, 1000000000LL, &c);
        uit_mouse_latch(&b, 1000000000LL, &c);
        for (int i = 0; i < 16; i++) uit_mouse_add(&a, 2.5, -1);
        uit_mouse_add(&b, 40, -16);
        uit_mouse_latch(&a, 1000000000LL + 16666667, &c);
        uit_mouse_latch(&b, 1000000000LL + 16666667, &c);
        CHECK(a.rx == b.rx && a.ry == b.ry, "split %d,%d whole %d,%d", a.rx, a.ry, b.rx, b.ry);
    }
    // 8. fine aim correction: a game that turns by stick^p turns by the distance again when curve = p
    {
        double p = 2.0;
        UitMouseCfg cc = {.sens = 1.0, .curve = p};
        double turn[2];
        double speeds[2] = {400, 3200};   // a slow and a quick sweep of the same 800 counts
        for (int k = 0; k < 2; k++) {
            UitMouseAim m = {0};
            int64_t t = 1000000000LL, fr = 16666667;
            uit_mouse_latch(&m, t, &cc);
            double secs = 800 / speeds[k], per = speeds[k] / 60.0, sent = 0;
            turn[k] = 0;
            for (int f = 0; f < (int)(secs * 60) + 30; f++) {
                if (sent < 800) { double d = per < 800 - sent ? per : 800 - sent; uit_mouse_add(&m, d, 0); sent += d; }
                t += fr;
                uit_mouse_latch(&m, t, &cc);
                turn[k] += pow(m.rx / 32767.0, p) * (fr / 1e9);   // the game's curve
            }
        }
        CHECK(fabs(turn[0] - turn[1]) < 0.02 * turn[1] && fabs(turn[1] - 800 / UIT_MOUSE_FULL_RATE) < 0.02 * turn[1],
              "curve 2: slow %.4f quick %.4f want %.4f", turn[0], turn[1], 800 / UIT_MOUSE_FULL_RATE);
        UitMouseCfg c1 = {.sens = 1.0, .curve = 1.0};   // 1 is off: the stick is the same as with no curve at all
        UitMouseAim a = {0}, b = {0};
        uit_mouse_latch(&a, 1000000000LL, &c1);
        uit_mouse_latch(&b, 1000000000LL, &c);
        uit_mouse_add(&a, 17, 5);
        uit_mouse_add(&b, 17, 5);
        uit_mouse_latch(&a, 1000000000LL + 16666667, &c1);
        uit_mouse_latch(&b, 1000000000LL + 16666667, &c);
        CHECK(a.rx == b.rx && a.ry == b.ry, "curve 1 %d,%d none %d,%d", a.rx, a.ry, b.rx, b.ry);
    }
    printf("%s: %d checks, %d failed\n", fails ? "FAIL" : "ok", checks, fails);
    return fails != 0;
}
