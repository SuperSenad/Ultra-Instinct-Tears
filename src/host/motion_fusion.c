/* From MacNativeGames games/totk/runtime/motion_fusion.c @b75be8c (MIT). [port]: fed by the SDL3 controller
 * sensors (window_sdl.c), which are converted to the frame below there; the code itself is unchanged. */
/* Six-axis states from the Mac controller's motion samples (2026-10-17; fed by runtime/gamepad_feedback.m, read by the
   game through runtime/hid_feedback.c, only with TOTK_GYRO=1).
   Input per sample: acceleration in G including gravity and rotation rate in rad/s, in the controller's frame (GCMotion:
   x to the right, y toward the top edge, z out of the face; lying face up it reads z = -1 G). That is the Switch's
   six-axis frame, so by default nothing is remapped; TOTK_GYRO_AXES="-x,y,-z" style remaps (a reflection also flips the
   rotation's sense). Output per sample, like the Switch: acceleration in G, angular velocity in revolutions per second,
   angle = the integrated revolutions per axis, direction = the controller's three axes in a reference frame whose z is
   up. The orientation is integrated from the gyro and, while the controller is not being accelerated (|a| within 15 %
   of 1 G), pulled toward the measured gravity (Mahony, gain 0.5/s: about 2 s), as the Switch's sensor fusion does; yaw
   has no reference and drifts with the gyro's bias (well under a degree a minute on a DualSense at rest). The newest 64
   samples are kept; the game reads up to 16, newest first, and goes by their sampling numbers.
   Dead zone (2026-10-19): rotation slower than TOTK_GYRO_DEADZONE deg/s counts as none, and the dead zone is taken off
   faster rotation, so the rate never jumps at its edge. Default 0, except 1.2 while a controller's motion comes from its
   HID reports (runtime_motion_deadzone_default, gamepad_feedback.m): an 8BitDo Pro Controller lying on the desk read
   0.83 deg/s on average (bias 0.05, 0.73, 0.38 deg/s after its factory calibration), at most 1.07 deg/s, in 616 samples,
   enough for the aim to creep; a DualSense through GameController needs none. */
#include "host.h"  // [port] runtime.h
#include "uit_live.h"
#include "mouse_aim.h"   // [UIT 0454]
#include "../runtime/platform.h"   // [UIT 0454] plat_sleep_ns_precise
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

enum { MOTION_RING = 64 };
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static RuntimeMotionState ring[MOTION_RING];
static uint64_t produced, last_ns, logged, log_at, reads_logged;
static uint64_t first;   /* [UIT 0398] produced at the last StartSixAxisSensor: older samples are not read */
static double R[3][3];          /* controller -> reference; row i = reference axis i in controller coordinates */
static double angle[3];
static int restart = 1, live;
static int from[3] = {0, 1, 2};
static double sign[3] = {1, 1, 1}, det = 1;
static double deadzone = -1;   /* rad/s; -1 until read from TOTK_GYRO_DEADZONE */
static double deadzone_default;   /* deg/s, when TOTK_GYRO_DEADZONE is unset */
static int deadzone_forced;   /* [UIT 0170] lock; 1 = the overlay chose deadzone_forced_deg, 2 = automatic */
static double deadzone_forced_deg;
/* [UIT 0454] the bow aimed with the mouse (mouse_aim.h): the rotation the mouse asked for and not yet in a sample (rad: yaw about the
   world's up, pitch about the controller's x), when a controller's own sensor last fed a sample, and the pose of the
   controller the mouse turns when there is none (yaw, pitch in rad; pose 0 lying flat, 1 upright as held in handheld). */
static double mouse_yaw, mouse_pitch, pose_yaw, pose_pitch;
static uint64_t pad_fed_ns;
static int mouse_pose;

/* TOTK_GYRO_AXES: reference axis i = sign * controller axis from[i]. 0 if unset, -1 if malformed. */
int runtime_motion_axes(const char *spec) {
    int f[3], used[3] = {0}; double s[3], parity = 1; const char *p = spec;
    if (!spec) return 0;
    for (int i = 0; i < 3; i++) {
        s[i] = 1; if (*p == '-') { s[i] = -1; p++; } else if (*p == '+') p++;
        if (*p < 'x' || *p > 'z' || used[*p - 'x']) return -1;
        f[i] = *p - 'x'; used[f[i]] = 1; p++;
        if (i < 2 && *p++ != ',') return -1;
    }
    if (*p) return -1;
    for (int i = 0; i < 3; i++) for (int j = i + 1; j < 3; j++) if (f[i] > f[j]) parity = -parity;
    pthread_mutex_lock(&lock);
    for (int i = 0; i < 3; i++) { from[i] = f[i]; sign[i] = s[i]; }
    det = parity * s[0] * s[1] * s[2];
    pthread_mutex_unlock(&lock);
    return 1;
}
static double norm3(const double v[3]) { return sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]); }
static void cross3(const double a[3], const double b[3], double out[3]) {
    out[0] = a[1] * b[2] - a[2] * b[1]; out[1] = a[2] * b[0] - a[0] * b[2]; out[2] = a[0] * b[1] - a[1] * b[0];
}
static void orthonormalize(void) {   /* rows, from the up row (the one the gravity correction keeps right) */
    double n = norm3(R[2]); for (int j = 0; j < 3; j++) R[2][j] /= n;
    double d = R[0][0] * R[2][0] + R[0][1] * R[2][1] + R[0][2] * R[2][2];
    for (int j = 0; j < 3; j++) R[0][j] -= d * R[2][j];
    n = norm3(R[0]); for (int j = 0; j < 3; j++) R[0][j] /= n;
    cross3(R[2], R[0], R[1]);
}
/* A fresh orientation: z up against the measured gravity, x as close to the controller's x as that allows. */
static void align_to(const double acc[3]) {
    double n = norm3(acc), up[3] = {0, 0, 1}, x[3] = {1, 0, 0};
    if (n > 0.5) for (int j = 0; j < 3; j++) up[j] = -acc[j] / n;
    if (fabs(up[0]) > 0.9) { x[0] = 0; x[1] = 1; }
    memcpy(R[2], up, sizeof up); memcpy(R[0], x, sizeof x); orthonormalize();
}
static void integrate(const double gyro[3], const double acc[3], double dt) {
    double w[3] = {gyro[0], gyro[1], gyro[2]}, n = norm3(acc);
    if (fabs(n - 1) < 0.15) {   /* not being accelerated: turn the estimated down toward the measured one */
        const double kp = 0.5;
        double measured[3] = {acc[0] / n, acc[1] / n, acc[2] / n}, estimated[3] = {-R[2][0], -R[2][1], -R[2][2]}, e[3];
        cross3(measured, estimated, e);
        for (int j = 0; j < 3; j++) w[j] += kp * e[j];
    }
    double rate = norm3(w), theta = rate * dt;
    if (theta < 1e-12) return;
    double k[3] = {w[0] / rate, w[1] / rate, w[2] / rate}, s = sin(theta), c = 1 - cos(theta);
    double K[3][3] = {{0, -k[2], k[1]}, {k[2], 0, -k[0]}, {-k[1], k[0], 0}}, E[3][3], out[3][3];
    for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) {
        double kk = 0; for (int m = 0; m < 3; m++) kk += K[i][m] * K[m][j];
        E[i][j] = (i == j) + s * K[i][j] + c * kk;
    }
    static int negative = -1; if (negative < 0) { const char *e = getenv("TOTK_PROBE_NEGATIVE"); negative = e && atoi(e) == 20; }
    for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) {   /* body-frame rate: R * E (probe negative 20: E * R, a reference-frame rate) */
        double v = 0; for (int m = 0; m < 3; m++) v += negative ? E[i][m] * R[m][j] : R[i][m] * E[m][j];
        out[i][j] = v;
    }
    memcpy(R, out, sizeof R); orthonormalize();
}
static void log_samples(const RuntimeMotionState *s, uint64_t now);
void runtime_motion_feed(const double raw_acc[3], const double raw_gyro[3], uint64_t now) {
    double acc[3], gyro[3];
    pthread_mutex_lock(&lock);
    for (int i = 0; i < 3; i++) { acc[i] = sign[i] * raw_acc[from[i]]; gyro[i] = det * sign[i] * raw_gyro[from[i]]; }
    if (deadzone < 0) {
        const char *e = getenv("TOTK_GYRO_DEADZONE"); double v = deadzone_default;
        if (e) { char *end; v = strtod(e, &end); if (end == e || *end || !(v >= 0) || v > 20) runtime_fail("TOTK_GYRO_DEADZONE must be a number of degrees a second in [0, 20]"); }
        deadzone = v * M_PI / 180;
        if (e) fprintf(stderr, "[gyro] dead zone %.2f deg/s (TOTK_GYRO_DEADZONE)\n", v);
    }
    double rate = norm3(gyro), keep = rate > deadzone ? (rate - deadzone) / rate : 0;
    for (int i = 0; i < 3; i++) gyro[i] *= keep;
    double dt = last_ns && now > last_ns ? (now - last_ns) / 1e9 : 0.004;
    if (dt > 0.05) dt = 0.004;   /* a gap (sensors were off, the app was in the background): no integration across it */
    last_ns = now; live = 1; pad_fed_ns = now;
    if (mouse_yaw != 0 || mouse_pitch != 0) {   /* [UIT 0454] the mouse turns this controller too, past the dead zone */
        double m[3];
        uit_mouse_gyro_rate(mouse_yaw, mouse_pitch, R[2], dt, m);
        for (int i = 0; i < 3; i++) gyro[i] += m[i];
        mouse_yaw = mouse_pitch = 0;
    }
    if (restart) { align_to(acc); memset(angle, 0, sizeof angle); restart = 0; }
    else integrate(gyro, acc, dt);
    RuntimeMotionState *s = &ring[produced % MOTION_RING];
    *s = (RuntimeMotionState){.sampling = produced + 1, .delta_ns = (uint64_t)(dt * 1e9)};
    for (int i = 0; i < 3; i++) {
        double rps = gyro[i] / (2 * M_PI); angle[i] += rps * dt;
        s->acc[i] = (float)acc[i]; s->gyro[i] = (float)rps; s->angle[i] = (float)angle[i];
        for (int j = 0; j < 3; j++) s->dir[i * 3 + j] = (float)R[j][i];   /* direction i = controller axis i in the reference frame */
    }
    produced++;
    log_samples(s, now);
    pthread_mutex_unlock(&lock);
}
static void log_samples(const RuntimeMotionState *s, uint64_t now) {   /* lock held */
    if (!log_at) log_at = now;
    if (now - log_at >= 30000000000ull) {
        uint64_t values, reads; runtime_hid_feedback_stats(&values, &reads);
        fprintf(stderr, "[gyro] last 30 s: %llu samples (%.0f a second), the game read the states %llu times; now acc (%+.2f %+.2f %+.2f) G, gyro (%+.3f %+.3f %+.3f) rev/s\n",
                (unsigned long long)(produced - logged), (produced - logged) / ((now - log_at) / 1e9), (unsigned long long)(reads - reads_logged),
                s->acc[0], s->acc[1], s->acc[2], s->gyro[0], s->gyro[1], s->gyro[2]);
        logged = produced; reads_logged = reads; log_at = now;
    }
}
int runtime_motion_read(RuntimeMotionState *out, int max) {
    if (max <= 0) return 0;
    int n = 0;
    pthread_mutex_lock(&lock);
    if (live && produced > first && runtime_now_ns() - last_ns < 200000000ull) {   /* no sample for 200 ms: no motion */
        uint64_t have = produced - first < MOTION_RING ? produced - first : MOTION_RING;   /* [UIT 0398] */
        n = (int)(have < (uint64_t)max ? have : (uint64_t)max);
        for (int i = 0; i < n; i++) out[i] = ring[(produced - 1 - (uint64_t)i) % MOTION_RING];
    }
    pthread_mutex_unlock(&lock);
    return n;
}
/* [UIT 0398] StartSixAxisSensor (the bow drawn): a restart only flagged the next sample to start a fresh frame, so a
   read in the frame of the Start returned the previous session's states as live and the angle jumped back between
   reads: motion aim was dead when the bow was first drawn, then lagged ~60 ms. The samples from before it are now
   hidden and the angle zeroed; reads return nothing until the next sample (a few ms), which starts a fresh frame and
   angle; sampling numbers go on. TOTK_GYRO_RESTART=0: only the flag, as before. From upstream UltraTears 477ffc5
   (kuraeow), after totk-pc-recomp 7d89fde (MIT). */
static int restart_drops(void) {
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("TOTK_GYRO_RESTART");
        on = !(e && !strcmp(e, "0"));
        fprintf(stderr, "[gyro] [UIT 0398] sensor start: %s\n", on ? "older samples dropped, angle zeroed" : "only a fresh frame at the next sample (TOTK_GYRO_RESTART=0)");
    }
    return on;
}
void runtime_motion_start(void) {
    int drop = restart_drops();
    pthread_mutex_lock(&lock);
    restart = 1;
    if (drop) { first = produced; last_ns = 0; memset(angle, 0, sizeof angle); }
    pthread_mutex_unlock(&lock);
}
/* The dead zone in deg/s when TOTK_GYRO_DEADZONE is unset: the HID reader sets 1.2, a GameController controller 0. */
void runtime_motion_deadzone_default(double deg) {
    pthread_mutex_lock(&lock);
    deadzone_default = deg;
    if (deadzone_forced == 1) deadzone = deadzone_forced_deg * M_PI / 180;   /* [UIT 0170] the overlay's choice stays */
    else if (deadzone_forced == 2) deadzone = deg * M_PI / 180;
    else deadzone = -1;
    pthread_mutex_unlock(&lock);
}
/* [UIT 0170] the in-game overlay: a dead zone in deg/s (0..20), or < 0 = automatic (TOTK_GYRO_DEADZONE unset: the
   controller's default). Taken at the next motion sample. */
void runtime_motion_set_deadzone(double deg) {
    pthread_mutex_lock(&lock);
    if (deg < 0) { deadzone_forced = 2; deadzone = deadzone_default * M_PI / 180; }   /* 2: automatic, the env no longer read */
    else { if (deg > 20) deg = 20; deadzone_forced = 1; deadzone_forced_deg = deg; deadzone = deg * M_PI / 180; }
    pthread_mutex_unlock(&lock);
}
double runtime_motion_deadzone(int *automatic) {
    pthread_mutex_lock(&lock);
    double v; int a;
    if (deadzone_forced == 1) { v = deadzone_forced_deg; a = 0; }
    else if (deadzone_forced == 2) { v = deadzone_default; a = 1; }
    else {
        const char *e = getenv("TOTK_GYRO_DEADZONE"); char *end = NULL;
        double d = e && *e ? strtod(e, &end) : -1;
        if (e && *e && end != e && !*end && d >= 0 && d <= 20) { v = d; a = 0; } else { v = deadzone_default; a = 1; }
    }
    pthread_mutex_unlock(&lock);
    if (automatic) *automatic = a;
    return v;
}
void runtime_motion_live(int on) { pthread_mutex_lock(&lock); live = on; if (on) restart = 1; pthread_mutex_unlock(&lock); }

/* [UIT 0454] The bow aimed with the mouse without a controller's sensor: a controller only the mouse turns. A thread makes a sample
   every 4 ms (the Switch's sensor makes one every 5 ms) while the mouse camera is on and no controller with motion fed one
   in the last 30 ms; each sample carries the rotation asked for since the one before, at the rate that gives exactly
   that angle over its real time step, so the game's motion aiming turns by the mouse's angle at any frame rate.
   The pose is kept as yaw about the world's up and pitch about the controller's x (pitch held within +-75 degrees so
   the pose never tips over; the rate reported is the full one), and the measured gravity always matches it, so the
   fusion's gravity pull has nothing to correct. */
static void pose_matrix(void) {   /* lock held: R = Rz(yaw) * B * Rx(pitch), columns = controller axes in the reference */
    double cy = cos(pose_yaw), sy = sin(pose_yaw), cp = cos(pose_pitch), sp = sin(pose_pitch);
    double B[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}}, X[3][3] = {{1, 0, 0}, {0, cp, -sp}, {0, sp, cp}},
           Z[3][3] = {{cy, -sy, 0}, {sy, cy, 0}, {0, 0, 1}}, T[3][3];
    if (mouse_pose == 1) {   /* upright: the top edge (y) up, the face (z) toward the player (reference -y) */
        double U[3][3] = {{1, 0, 0}, {0, 0, -1}, {0, 1, 0}};
        memcpy(B, U, sizeof B);
    }
    for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) {
        double v = 0; for (int m = 0; m < 3; m++) v += B[i][m] * X[m][j];
        T[i][j] = v;
    }
    for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) {
        double v = 0; for (int m = 0; m < 3; m++) v += Z[i][m] * T[m][j];
        R[i][j] = v;
    }
}
static void mouse_tick(uint64_t now) {   /* lock held */
    double dt = last_ns && now > last_ns ? (now - last_ns) / 1e9 : 0.004;
    if (dt > 0.05) dt = 0.004;
    last_ns = now; live = 1;
    if (restart) { pose_yaw = pose_pitch = 0; memset(angle, 0, sizeof angle); restart = 0; }
    double rate[3];
    pose_matrix();
    uit_mouse_gyro_rate(mouse_yaw, mouse_pitch, R[2], dt, rate);   /* about the pose before this step */
    const double lim = 75 * M_PI / 180;
    pose_yaw = fmod(pose_yaw - mouse_yaw, 2 * M_PI);
    pose_pitch += mouse_pitch;
    if (pose_pitch > lim) pose_pitch = lim;
    if (pose_pitch < -lim) pose_pitch = -lim;
    mouse_yaw = mouse_pitch = 0;
    pose_matrix();
    RuntimeMotionState *s = &ring[produced % MOTION_RING];
    *s = (RuntimeMotionState){.sampling = produced + 1, .delta_ns = (uint64_t)(dt * 1e9)};
    for (int i = 0; i < 3; i++) {
        double rps = rate[i] / (2 * M_PI); angle[i] += rps * dt;
        s->acc[i] = (float)-R[2][i]; s->gyro[i] = (float)rps; s->angle[i] = (float)angle[i];
        for (int j = 0; j < 3; j++) s->dir[i * 3 + j] = (float)R[j][i];
    }
    produced++;
    log_samples(s, now);
}
static pthread_cond_t mouse_cv = PTHREAD_COND_INITIALIZER;
static int mouse_on, mouse_thread_up;
static void *mouse_thread(void *arg) {
    (void)arg;
    pthread_mutex_lock(&lock);
    for (;;) {
        while (!mouse_on) pthread_cond_wait(&mouse_cv, &lock);
        uint64_t now = runtime_now_ns();
        if (!pad_fed_ns || now - pad_fed_ns > 30000000ull) mouse_tick(now);
        pthread_mutex_unlock(&lock);
        if (!plat_sleep_ns_precise(4000000)) { struct timespec ts = {0, 4000000}; nanosleep(&ts, NULL); }
        pthread_mutex_lock(&lock);
    }
    return NULL;
}
/* on: the mouse camera is on and the game has its six-axis sensor (window thread; pose: 0 flat, 1 upright). */
void runtime_motion_mouse_enable(int on, int pose) {
    pthread_mutex_lock(&lock);
    mouse_on = on;
    mouse_pose = pose == 1;
    if (!on) mouse_yaw = mouse_pitch = 0;
    if (on && !mouse_thread_up) {
        pthread_t t;
        if (pthread_create(&t, NULL, mouse_thread, NULL) == 0) { pthread_detach(t); mouse_thread_up = 1; }
        else fprintf(stderr, "[gyro] mouse camera: no sample thread; the bow aims through the stick instead\n");
    }
    if (on) pthread_cond_signal(&mouse_cv);
    pthread_mutex_unlock(&lock);
}
int runtime_motion_mouse_ready(void) { pthread_mutex_lock(&lock); int r = mouse_on && mouse_thread_up; pthread_mutex_unlock(&lock); return r; }
/* An angle to turn (rad; yaw > 0 right, pitch > 0 up), taken by the next sample (window thread). */
void runtime_motion_mouse(double yaw, double pitch) {
    pthread_mutex_lock(&lock);
    if (mouse_on) { mouse_yaw += yaw; mouse_pitch += pitch; }
    pthread_mutex_unlock(&lock);
}
