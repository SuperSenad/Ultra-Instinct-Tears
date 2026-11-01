// [UIT 0161] tests for src/gfx/vkb_dlss_math.h: DLSS's camera motion vectors, jitter and reasons (vkb_dlss.c, dlss_mv.comp).
// cc -O2 -Isrc/gfx tools/tests/test_dlss_math.c -o /tmp/t/dlm -lm && /tmp/t/dlm
#include "vkb_dlss_math.h"
#include <stdio.h>

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)
#define NEAR(a, b, e) (fabs((a) - (b)) <= (e))

// A GL-style camera: row major, clip = VP * world. Camera at (cx, cy, cz), turned yaw radians about y (0: looking -z).
static void camera(double cx, double cy, double cz, double yaw, double fovy, double aspect, double n, double f, double *vp) {
    double t = 1.0 / tan(fovy / 2), P[16] = {t / aspect, 0, 0, 0, 0, t, 0, 0, 0, 0, (f + n) / (n - f), 2 * f * n / (n - f), 0, 0, -1, 0};
    double c = cos(yaw), s = sin(yaw);
    // view = R(-yaw) * T(-c): world to camera
    double V[16] = {c, 0, -s, 0, 0, 1, 0, 0, s, 0, c, 0, 0, 0, 0, 1}, T[16] = {1, 0, 0, -cx, 0, 1, 0, -cy, 0, 0, 1, -cz, 0, 0, 0, 1}, VT[16];
    dlm_mul4(V, T, VT);
    dlm_mul4(P, VT, vp);
}
static void xform(const double *m, const double *p, double *o) {
    for (int r = 0; r < 4; r++) o[r] = m[r * 4] * p[0] + m[r * 4 + 1] * p[1] + m[r * 4 + 2] * p[2] + m[r * 4 + 3];
}
// Where the rasteriser puts a clip-space point, with vkb_draw.c's viewport: the jitter added to x / y, then the
// negative-height flip (Vulkan: x_f = o_x + w/2 (1 + x), y_f = o_y + h/2 (1 + y) with o the viewport's centre).
static void raster(const double *clip, double W, double H, double jx, double jy, double *fx, double *fy, double *z) {
    double vx = 0 + jx, vy = 0 + jy, vw = W, vh = H;
    vy += vh; vh = -vh;
    double nx = clip[0] / clip[3], ny = clip[1] / clip[3], nz = clip[2] / clip[3];
    *fx = vx + vw / 2 + vw / 2 * nx;
    *fy = vy + vh / 2 + vh / 2 * ny;
    *z = nz * 0.5 + 0.5;   // the translator's GL depth range: the depth buffer holds (z + 1) / 2
}

int main(void) {
    const double W = 2560, H = 1440, n = 0.5, f = 4000, fov = 0.87;
    // the jitter: the picture moves by the viewport shift, x right, y down
    {
        double vp[16], p[3] = {1, 2, -30}, c[4], x0, y0, x1, y1, z;
        camera(0, 0, 0, 0, fov, W / H, n, f, vp);
        xform(vp, p, c);
        raster(c, W, H, 0, 0, &x0, &y0, &z);
        raster(c, W, H, 0.25, -0.375, &x1, &y1, &z);
        CHECK(NEAR(x1 - x0, 0.25, 1e-9) && NEAR(y1 - y0, -0.375, 1e-9));
        float ox, oy;
        dlm_jitter_report(0.25, -0.375, 1.0f, &ox, &oy);   // DLSS is told the picture's shift
        CHECK(ox == 0.25f && oy == -0.375f);
        dlm_jitter_report(0.25, -0.375, -1.0f, &ox, &oy);
        CHECK(ox == -0.25f && oy == 0.375f);
        // y down: a point above the centre is in the upper half of the picture
        CHECK(y0 < H / 2 && x0 > W / 2);
    }
    // the sequence: Halton (2, 3) - 0.5, inside +-0.5, mean near 0
    {
        double sx = 0, sy = 0, jx, jy;
        dlm_jitter(1, &jx, &jy);
        CHECK(NEAR(jx, 0.0, 1e-12) && NEAR(jy, 1.0 / 3 - 0.5, 1e-12));
        dlm_jitter(2, &jx, &jy);
        CHECK(NEAR(jx, -0.25, 1e-12) && NEAR(jy, 2.0 / 3 - 0.5, 1e-12));
        for (unsigned i = 1; i <= 18; i++) { dlm_jitter(i, &jx, &jy); CHECK(fabs(jx) <= 0.5 && fabs(jy) <= 0.5); sx += jx; sy += jy; }
        CHECK(fabs(sx / 18) < 0.06 && fabs(sy / 18) < 0.06);
        CHECK(dlm_phases(2560, 1440, 3840, 2160) == 18);   // Quality 1.5x: 8 x 2.25
        CHECK(dlm_phases(1920, 1080, 3840, 2160) == 32);   // Performance
        CHECK(dlm_phases(2227, 1253, 3840, 2160) == 24);   // Balanced (1/1.724)
        CHECK(dlm_phases(1280, 720, 3840, 2160) == 72);    // Ultra Performance
        CHECK(dlm_phases(3840, 2160, 3840, 2160) == 8 && dlm_phases(0, 0, 1, 1) == 8);
    }
    // the inverse
    {
        double vp[16], iv[16], id[16];
        camera(3, 1, -2, 0.3, fov, W / H, n, f, vp);
        CHECK(dlm_inv4(vp, iv));
        dlm_mul4(vp, iv, id);
        double e = 0;
        for (int i = 0; i < 16; i++) e = fmax(e, fabs(id[i] - (i % 5 == 0)));
        CHECK(e < 1e-9);
        double z[16] = {0};
        CHECK(!dlm_inv4(z, iv));
    }
    // the depth of a distance
    {
        double vp[16], p[3] = {0, 0, -20}, c[4], fx, fy, z;
        camera(0, 0, 0, 0, fov, W / H, n, f, vp);
        xform(vp, p, c);
        raster(c, W, H, 0, 0, &fx, &fy, &z);
        CHECK(NEAR(dlm_depth_at(n, f, 20), z, 1e-12));
        CHECK(dlm_depth_at(n, f, 0.01) == dlm_depth_at(n, f, n) && NEAR(dlm_depth_at(n, f, n), 0, 1e-12));
    }
    // the motion of world points, against their true positions, still and moving cameras, with and without jitter
    {
        struct { double c0[4], c1[4]; } moves[] = {
            {{0, 0, 0, 0}, {0, 0, 0, 0}},              // still
            {{0, 0, 0, 0}, {0.4, 0, 0, 0}},            // strafe right
            {{0, 0, 0, 0}, {0, 0, -1.5, 0}},           // forward
            {{5, 2, 1, 0.2}, {5, 2, 1, 0.23}},         // pan
            {{5, 2, 1, 0.2}, {5.3, 2.1, 0.6, 0.17}},   // all of it
        };
        double pts[][3] = {{0, 0, -20}, {3, 1, -8}, {-12, -2, -60}, {40, 10, -400}, {-1, 0.5, -2.5}, {2, -1, -1500}};
        for (unsigned mi = 0; mi < sizeof moves / sizeof moves[0]; mi++)
            for (unsigned ji = 0; ji < 4; ji++) {
                double jx = 0, jy = 0, jpx, jpy;
                if (ji) dlm_jitter(ji * 5, &jx, &jy);
                dlm_jitter(ji * 5 + 1, &jpx, &jpy);   // last frame's jitter: must not matter
                double prev[16], cur[16], m[16];
                camera(moves[mi].c0[0], moves[mi].c0[1], moves[mi].c0[2], moves[mi].c0[3], fov, W / H, n, f, prev);
                camera(moves[mi].c1[0], moves[mi].c1[1], moves[mi].c1[2], moves[mi].c1[3], fov, W / H, n, f, cur);
                CHECK(dlm_reproj(cur, prev, m));
                for (unsigned pi = 0; pi < sizeof pts / sizeof pts[0]; pi++) {
                    double w[3] = {pts[pi][0] + moves[mi].c0[0], pts[pi][1] + moves[mi].c0[1], pts[pi][2] + moves[mi].c0[2]};
                    double cc[4], cp[4], x, y, z, ux, uy, uz, px, py, pz, mx, my;
                    xform(cur, w, cc); xform(prev, w, cp);
                    if (cc[3] <= 0 || cp[3] <= 0) continue;
                    raster(cc, W, H, jx, jy, &x, &y, &z);        // where the sample lands this frame (jittered)
                    raster(cc, W, H, 0, 0, &ux, &uy, &uz);       // ... and without the jitter
                    raster(cp, W, H, jpx, jpy, &px, &py, &pz);   // last frame, as drawn
                    raster(cp, W, H, 0, 0, &px, &py, &pz);       // ... and without its jitter: what DLSS wants
                    CHECK(dlm_motion(m, W, H, x, y, z, jx, jy, &mx, &my));
                    double tol = 2e-3 * (1 + fabs(px - ux) + fabs(py - uy));
                    if (!NEAR(mx, px - ux, tol) || !NEAR(my, py - uy, tol)) {
                        printf("FAIL move %u jitter %u point %u: %.4f %.4f want %.4f %.4f\n", mi, ji, pi, mx, my, px - ux, py - uy);
                        fails++;
                    }
                    if (mi == 0) CHECK(fabs(mx) < 1e-3 && fabs(my) < 1e-3);   // still camera, jittered: no motion
                    // without the correction the vector is the motion of the neighbouring surface point (the one
                    // under the unjittered centre), wrong by the motion's change over the jitter: small, not zero
                    if (mi == 4 && ji) {
                        double ox, oy;
                        dlm_motion(m, W, H, x, y, z, 0, 0, &ox, &oy);
                        CHECK(fabs(ox - mx) + fabs(oy - my) < 0.5);
                    }
                }
            }
        // a known pan: yaw by one pixel's angle at the centre moves the centre by about one pixel, to the right in
        // the previous frame for a camera turning left... (yaw > 0 turns towards -x; a point ahead was further right)
        double prev[16], cur[16], m[16], mx, my;
        double ang = atan(tan(fov / 2) * (W / H) / (W / 2));   // one pixel at the centre
        camera(0, 0, 0, 0, fov, W / H, n, f, prev);
        camera(0, 0, 0, ang, fov, W / H, n, f, cur);
        dlm_reproj(cur, prev, m);
        dlm_motion(m, W, H, W / 2, H / 2, dlm_depth_at(n, f, 50), 0, 0, &mx, &my);
        CHECK(NEAR(fabs(mx), 1.0, 1e-3) && NEAR(my, 0, 1e-6));
        double mm = dlm_motion_max(m, W, H, dlm_depth_at(n, f, 20));   // more at the edges than at the centre
        CHECK(mm >= 1.0 && mm < 1.6);
        camera(0, 0, 0, 0, fov, W / H, n, f, cur);
        dlm_reproj(cur, prev, m);
        CHECK(dlm_motion_max(m, W, H, dlm_depth_at(n, f, 20)) < 1e-6);
    }
    // the reasons have names
    for (int r = 0; r < DLR_N; r++) CHECK(dlm_reason(r) && dlm_reason(r)[0]);
    printf(fails ? "test_dlss_math: %d FAILED\n" : "test_dlss_math: ok\n", fails);
    return fails != 0;
}
