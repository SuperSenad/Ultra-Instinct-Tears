// [UIT 0454] The bow aimed with the mouse: src/host/mouse_aim.h (the angle, the sensor rate) and the mouse-turned
// sensor of src/host/motion_fusion.c (exact angles at any sample timing, with or without a controller's own sensor).
//   clang -O2 -pthread -I src/host tools/tests/test_mouse_motion.c src/host/motion_fusion.c -lm && ./a.out
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "host.h"
#include "mouse_aim.h"

// stubs for motion_fusion.c
uint64_t runtime_now_ns(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}
_Noreturn void runtime_fail(const char *m) { fprintf(stderr, "runtime_fail: %s\n", m); exit(2); }
void runtime_hid_feedback_stats(uint64_t *v, uint64_t *r) { *v = *r = 0; }
int plat_sleep_ns_precise(uint64_t ns) { (void)ns; return 0; }

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { if (fails++ < 16) { printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } } while (0)

static uint32_t rng = 12345;
static double rnd(void) { rng = rng * 1664525u + 1013904223u; return (rng >> 8) / 16777216.0; }

static void test_math(void) {
    // the bow's angles: sign, scale, invert, sensitivity clamp (the camera's 0.1..5 times the bow's 0.1..2)
    const double k = UIT_MOUSE_DEG_PER_COUNT * M_PI / 180;
    double y, p;
    uit_mouse_aim_angles(100, 0, 1, 0, &y, &p);
    CHECK(fabs(y - 100 * k) < 1e-12 && p == 0, "yaw %g pitch %g", y, p);
    uit_mouse_aim_angles(0, -50, 2, 0, &y, &p);   // mouse forward (y up the screen) looks up
    CHECK(p > 0 && fabs(p - 2 * 50 * k) < 1e-12, "pitch %g", p);
    uit_mouse_aim_angles(0, -50, 2, 1, &y, &p);
    CHECK(p < 0, "inverted pitch %g", p);
    uit_mouse_aim_angles(10, 0, 0.1 * 0.1, 0, &y, &p);
    CHECK(fabs(y - 10 * UIT_MOUSE_SENS_MIN * k) < 1e-12, "low clamp %g", y);
    uit_mouse_aim_angles(10, 0, 99, 0, &y, &p);
    CHECK(fabs(y - 10 * UIT_MOUSE_SENS_MAX * k) < 1e-12, "high clamp %g", y);
    uit_mouse_aim_angles(10, 0, NAN, 0, &y, &p);
    CHECK(fabs(y - 10 * k) < 1e-12, "NaN sens %g", y);
    // linear: two halves turn as far as the whole
    double y1, p1, y2, p2;
    uit_mouse_aim_angles(37, -11, 0.7, 0, &y, &p);
    uit_mouse_aim_angles(20, -5, 0.7, 0, &y1, &p1);
    uit_mouse_aim_angles(17, -6, 0.7, 0, &y2, &p2);
    CHECK(fabs(y - y1 - y2) < 1e-12 && fabs(p - p1 - p2) < 1e-12, "split");
    // the sensor rate: yaw about the world's up, pitch about x
    double up_flat[3] = {0, 0, 1}, up_upr[3] = {0, 1, 0}, r[3];
    uit_mouse_gyro_rate(0.3, 0, up_flat, 0.005, r);
    CHECK(fabs(r[2] + 0.3 / 0.005) < 1e-9 && r[0] == 0 && r[1] == 0, "flat yaw %g %g %g", r[0], r[1], r[2]);
    uit_mouse_gyro_rate(0.3, 0, up_upr, 0.005, r);
    CHECK(fabs(r[1] + 0.3 / 0.005) < 1e-9 && r[0] == 0 && r[2] == 0, "upright yaw %g %g %g", r[0], r[1], r[2]);
    uit_mouse_gyro_rate(0, 0.2, up_flat, 0.004, r);
    CHECK(fabs(r[0] - 50) < 1e-9, "pitch rate %g", r[0]);
    uit_mouse_gyro_rate(1, 1, up_flat, 0, r);
    CHECK(r[0] == 0 && r[1] == 0 && r[2] == 0, "dt 0 gives no rate");
}

// The sensor only the mouse turns: the thread runs on the real clock, and the test checks what the samples add up to.
static void sleep_ms(int ms) { struct timespec t = {ms / 1000, (ms % 1000) * 1000000L}; nanosleep(&t, NULL); }
static void test_motion(void) {
    RuntimeMotionState m[RUNTIME_MOTION_MAX];
    for (int pose = 0; pose < 2; pose++) {
        runtime_motion_start();
        runtime_motion_mouse_enable(1, pose);
        CHECK(runtime_motion_mouse_ready(), "thread up");
        sleep_ms(30);
        int n = runtime_motion_read(m, 1);
        CHECK(n == 1, "samples while still: %d", n);
        double a0[3] = {m[0].angle[0], m[0].angle[1], m[0].angle[2]};
        // 0.6 rad right and 0.3 rad up, in 300 uneven pieces over about 0.3 s
        double yaw = 0, pitch = 0;
        for (int i = 0; i < 300; i++) {
            double y = 0.002 * (0.5 + rnd()), p = 0.001 * (0.5 + rnd());
            if (i == 299) y = 0.6 - yaw, p = 0.3 - pitch;
            runtime_motion_mouse(y, p);
            yaw += y, pitch += p;
            if (i % 7 == 0) sleep_ms(7);
        }
        sleep_ms(40);
        n = runtime_motion_read(m, 1);
        CHECK(n == 1, "samples after: %d", n);
        int yaw_axis = pose ? 1 : 2;
        double dyaw = (m[0].angle[yaw_axis] - a0[yaw_axis]) * 2 * M_PI, dpitch = (m[0].angle[0] - a0[0]) * 2 * M_PI;
        // the pitch moves the up axis off the yaw axis, so the yaw shares into the other axis: check the whole turn
        CHECK(fabs(dpitch - 0.3) < 1e-4, "pose %d: pitch turned %.6f of 0.3", pose, dpitch);
        double other = (m[0].angle[3 - yaw_axis] - a0[3 - yaw_axis]) * 2 * M_PI;
        double yaw_total = sqrt(dyaw * dyaw + other * other);
        CHECK(dyaw < 0 && fabs(yaw_total - 0.6) < 0.02, "pose %d: yaw turned %.4f (%.4f, %.4f) of 0.6", pose, yaw_total, dyaw, other);
        // gravity matches the pose: acc = -up, |acc| = 1 G
        double g = sqrt(m[0].acc[0] * m[0].acc[0] + m[0].acc[1] * m[0].acc[1] + m[0].acc[2] * m[0].acc[2]);
        CHECK(fabs(g - 1) < 1e-4, "pose %d: |acc| %.5f", pose, g);
        double tilt = asin(fmin(1, fmax(-1, pose ? m[0].acc[2] : -m[0].acc[1])));   // the top edge lifted by the pitch
        CHECK(fabs(tilt - 0.3) < 1e-3, "pose %d: pose pitch %.4f", pose, tilt);
        // after: still samples with no rotation
        sleep_ms(20);
        n = runtime_motion_read(m, 3);
        CHECK(n == 3 && m[0].gyro[0] == 0 && m[0].gyro[1] == 0 && m[0].gyro[2] == 0, "still again");
        CHECK(m[0].sampling == m[1].sampling + 1 && m[0].delta_ns > 1000000 && m[0].delta_ns < 60000000,
              "sampling %llu %llu delta %llu", (unsigned long long)m[0].sampling, (unsigned long long)m[1].sampling,
              (unsigned long long)m[0].delta_ns);
        // the pitch never tips the pose over: 3 rad up holds it at 75 degrees, while the rate carries the full turn
        sleep_ms(5);
        n = runtime_motion_read(m, 1);
        double p0 = m[0].angle[0];
        runtime_motion_mouse(0, 3.0);
        sleep_ms(30);
        n = runtime_motion_read(m, 1);
        double tilt2 = asin(fmin(1, fmax(-1, pose ? m[0].acc[2] : -m[0].acc[1])));
        CHECK(fabs(tilt2 - 75 * M_PI / 180) < 1e-3, "pose %d: held at %.4f", pose, tilt2 * 180 / M_PI);
        CHECK(fabs((m[0].angle[0] - p0) * 2 * M_PI - 3.0) < 1e-4, "pose %d: full pitch rate %.5f", pose, (m[0].angle[0] - p0) * 2 * M_PI);
    }
    // off: no more samples (after 200 ms the game sees no motion), and motion asked for is dropped
    runtime_motion_mouse_enable(0, 0);
    runtime_motion_mouse(1, 1);
    sleep_ms(250);
    CHECK(runtime_motion_read(m, 1) == 0, "off: no samples");
    // with a controller's own sensor feeding (here every 5 ms), the mouse turn rides on its samples, past its dead zone
    runtime_motion_mouse_enable(1, 0);
    runtime_motion_start();
    double acc[3] = {0, 0, -1}, gyro[3] = {0, 0, 0};
    runtime_motion_feed(acc, gyro, runtime_now_ns());
    runtime_motion_read(m, 1);
    double a2 = m[0].angle[2];
    uint64_t s0 = m[0].sampling;
    for (int i = 0; i < 60; i++) {
        runtime_motion_mouse(0.01, 0);
        sleep_ms(5);
        runtime_motion_feed(acc, gyro, runtime_now_ns());
    }
    runtime_motion_read(m, 1);
    CHECK(fabs((m[0].angle[2] - a2) * 2 * M_PI + 0.6) < 1e-3, "controller: yaw %.5f of -0.6", (m[0].angle[2] - a2) * 2 * M_PI);
    CHECK(m[0].sampling - s0 <= 60 + 2, "controller: the mouse thread made no samples of its own (%llu)", (unsigned long long)(m[0].sampling - s0));
    runtime_motion_mouse_enable(0, 0);
}

int main(void) {
    test_math();
    test_motion();
    printf("%s: %d checks, %d failed\n", fails ? "FAIL" : "ok", checks, fails);
    return fails != 0;
}
