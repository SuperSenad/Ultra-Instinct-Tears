// [UIT 0451] Mouse camera and aiming: the mouse turns the camera (and the bow, Ultrahand and every other aim) directly,
// as in a PC game, through the right stick the game reads. Portable, header only: tools/tests/test_mouse_aim.c checks it.
//
// Why the right stick: the game's camera and aim code is the game's own (lifted) code; it reads the controller and
// turns by stick * its turn speed * the frame's time. Writing angles into the game's camera would mean patching game
// code per aim mode (bow, Ultrahand, Recall, first person, vehicles ...), which this port does not know. The stick is
// the one input every mode reads, so the mouse is turned into the exact stick value that moves the camera by what the
// mouse moved in that frame:
//   - the window thread adds raw mouse counts (SDL relative mode: Windows raw input, no pointer acceleration) as they
//     come (sub-frame: every 1 ms on a 1000 Hz mouse), uit_mouse_add;
//   - once per game frame (the first controller read after a present, overrides_input.c) uit_mouse_latch turns the
//     counts gathered since the last latch into a stick value: counts / seconds * sensitivity / UIT_MOUSE_FULL_RATE.
//     The game then turns stick * speed * frame time = counts * sensitivity * speed / FULL_RATE: the turn follows the
//     distance the mouse moved, at any frame rate, with no deadzone, no curve of ours and nothing lost to rounding.
//   - a stick cannot go past full tilt, so a flick faster than full tilt is clipped there and the rest is carried into
//     the next frames (at most UIT_MOUSE_CARRY_S seconds of full tilt): the camera ends up where the mouse went, a
//     moment later, instead of short of it.
//   - a gap longer than UIT_MOUSE_IDLE_NS between latches (loading, a pause, the window in the background) drops what
//     was gathered meanwhile, so nothing turns on its own afterwards.
//   - two corrections for what the game may do to a stick, both off by default: a response curve (the game turning by
//     stick^p: the mouse sends mag^(1/p), so the game's curve is undone) and a centre deadzone (a moving mouse sends at
//     least min_push). Game > More mouse settings, for the player to set if slow movements feel wrong.
// What it cannot change: the game's own top turn speed (the clip above) and any smoothing the game puts on its camera.
//
// [UIT 0454] The bow is aimed directly, not through the stick: while ZR is held (the bow drawn) and the game has its
// motion sensor (Game > Controller > Motion controls), the mouse turns the controller's motion sensor by an exact
// angle (uit_mouse_aim_angles: counts * UIT_MOUSE_DEG_PER_COUNT * sensitivity) and the game's own motion aiming turns
// the aim by that angle: no stick, no top speed, no clip, and nothing that depends on the frame rate (motion_fusion.c
// spreads the angle over its sensor samples with real time steps; uit_mouse_gyro_rate is the rate of one sample).
// Without the motion sensor the bow aims through the stick as above.
#pragma once
#include <math.h>
#include <stdint.h>

#define UIT_MOUSE_FULL_RATE 4000.0       // mouse counts a second that make full tilt at sensitivity 1
#define UIT_MOUSE_CARRY_S 0.25           // at most this much full-tilt turning is carried past a clipped frame
#define UIT_MOUSE_IDLE_NS 250000000LL    // a longer gap between game frames drops the motion gathered meanwhile
#define UIT_MOUSE_MIN_DT_NS 1000000LL    // reads closer together than 1 ms count as 1 ms (no huge stick values)
#define UIT_MOUSE_SENS_MIN 0.05
#define UIT_MOUSE_SENS_MAX 10.0
#define UIT_MOUSE_MIN_PUSH_MAX 30        // % (Mouse+ > Slow movement boost)
#define UIT_MOUSE_CURVE_MAX 3.0          // Mouse+ > Fine aim correction (1 = off)
#define UIT_MOUSE_DEG_PER_COUNT 0.05     // [UIT 0454] the bow: degrees per count at sensitivity 1 (40 an inch at 800 DPI)
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

typedef struct {
    double ax, ay;      // counts gathered since the last latch (x right, y down, as the mouse reports them)
    double cx, cy;      // counts carried from clipped frames, still to turn
    int64_t last_ns;    // the last latch (0: none yet)
    int32_t rx, ry;     // the stick value of the last latch (y up, as the game reads it)
} UitMouseAim;

typedef struct {
    double sens;        // multiplier, UIT_MOUSE_SENS_MIN..MAX (the aim sensitivity already folded in by the caller)
    double min_push;    // 0..0.3: the smallest stick push a moving mouse makes (for a game deadzone); 0 = linear
    double curve;       // 1..3: undo a game response curve stick^curve (0 or 1 = linear)
    int invert_y;       // 1: mouse forward looks down
} UitMouseCfg;

static inline void uit_mouse_add(UitMouseAim *m, double dx, double dy) {
    if (!(dx == dx) || !(dy == dy)) return;   // NaN: ignore
    m->ax += dx;
    m->ay += dy;
}

static inline void uit_mouse_reset(UitMouseAim *m) {
    m->ax = m->ay = m->cx = m->cy = 0;
    m->rx = m->ry = 0;
}

static inline int32_t uit_mouse_axis(double v) {
    long r = lround(v * 32767.0);
    return r > 32767 ? 32767 : r < -32767 ? -32767 : (int32_t)r;
}

// Once per game frame: the stick for the counts gathered since the last call. now_ns: a monotonic clock.
static inline void uit_mouse_latch(UitMouseAim *m, int64_t now_ns, const UitMouseCfg *c) {
    int first = !m->last_ns;
    int64_t gap = now_ns - m->last_ns;
    m->last_ns = now_ns;
    if (first || gap <= 0 || gap > UIT_MOUSE_IDLE_NS || !(c->sens > 0)) {   // first frame, or after a gap
        uit_mouse_reset(m);
        return;
    }
    if (gap < UIT_MOUSE_MIN_DT_NS) gap = UIT_MOUSE_MIN_DT_NS;
    double sens = c->sens < UIT_MOUSE_SENS_MIN ? UIT_MOUSE_SENS_MIN : c->sens > UIT_MOUSE_SENS_MAX ? UIT_MOUSE_SENS_MAX : c->sens;
    double dt = (double)gap * 1e-9;
    double tx = m->ax + m->cx, ty = m->ay + m->cy;   // counts to turn this frame
    m->ax = m->ay = 0;
    double k = sens / (UIT_MOUSE_FULL_RATE * dt);    // counts -> stick
    double vx = tx * k, vy = ty * k;
    double mag = sqrt(vx * vx + vy * vy);
    if (mag < 1e-6) { m->cx = m->cy = 0; m->rx = m->ry = 0; return; }
    double use = mag > 1.0 ? 1.0 / mag : 1.0;        // the share of this frame's counts that fits in full tilt
    m->cx = tx * (1.0 - use);
    m->cy = ty * (1.0 - use);
    double cmax = UIT_MOUSE_FULL_RATE * UIT_MOUSE_CARRY_S / sens, cm = sqrt(m->cx * m->cx + m->cy * m->cy);
    if (cm > cmax) { m->cx *= cmax / cm; m->cy *= cmax / cm; }
    vx *= use;
    vy *= use;
    mag *= use;
    if (c->curve > 1.0) {   // undo the game's stick^p: send mag^(1/p) (direction kept; 0 and full tilt unchanged)
        double p = c->curve > UIT_MOUSE_CURVE_MAX ? UIT_MOUSE_CURVE_MAX : c->curve, s = pow(mag, 1.0 / p) / mag;
        vx *= s;
        vy *= s;
        mag *= s;
    }
    double mp = c->min_push < 0 ? 0 : c->min_push > UIT_MOUSE_MIN_PUSH_MAX / 100.0 ? UIT_MOUSE_MIN_PUSH_MAX / 100.0 : c->min_push;
    if (mp > 0) {   // lift a moving mouse past the game's own deadzone: mag -> mp + (1 - mp) * mag (direction kept)
        double s = (mp + (1.0 - mp) * mag) / mag;
        vx *= s;
        vy *= s;
    }
    m->rx = uit_mouse_axis(vx);
    m->ry = uit_mouse_axis(c->invert_y ? vy : -vy);   // mouse y grows downward; the stick's y is up
}

// [UIT 0454] The bow: mouse counts (x right, y down, as the mouse reports them) to an angle in radians for the motion
// sensor; yaw > 0 turns right, pitch > 0 looks up. sens: the bow's sensitivity (camera's * the bow's multiplier).
static inline void uit_mouse_aim_angles(double dx, double dy, double sens, int invert_y, double *yaw, double *pitch) {
    if (!(sens == sens)) sens = 1.0;   // NaN
    sens = sens < UIT_MOUSE_SENS_MIN ? UIT_MOUSE_SENS_MIN : sens > UIT_MOUSE_SENS_MAX ? UIT_MOUSE_SENS_MAX : sens;
    double k = sens * UIT_MOUSE_DEG_PER_COUNT * M_PI / 180.0;
    *yaw = dx * k;
    *pitch = (invert_y ? dy : -dy) * k;
}

// [UIT 0454] The rotation rate (rad/s, the Switch's six-axis frame) that turns the controller by yaw about the world's
// up and by pitch about its own x axis (to the right) over dt seconds. up = the world's up in the controller's frame
// (unit). Turning right is a clockwise turn seen from above: negative about up. Looking up lifts the top edge:
// positive about x.
static inline void uit_mouse_gyro_rate(double yaw, double pitch, const double up[3], double dt, double out[3]) {
    out[0] = out[1] = out[2] = 0;
    if (!(dt > 0)) return;
    for (int i = 0; i < 3; i++) out[i] = -yaw * up[i] / dt;
    out[0] += pitch / dt;
}
