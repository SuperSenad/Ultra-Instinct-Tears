// [UIT 0160] The CPU side of DLSS's inputs (vkb_dlss.c), pure C so tools/tests/test_dlss_math.c can test it:
// the Halton jitter, the reprojection from this frame's clip space to the last frame's, the size of the camera motion
// it gives in render pixels (the status line), the jitter reported to DLSS, and the reasons DLSS kept the game's
// upscale on a frame.
//
// Conventions (checked by the test):
//  - matrices are row major, clip = M * (x, y, z, 1) (GL-style clip: x right, y up, z -1..1), as the game's camera has
//    them (vertex uniform c1, floats 12..27);
//  - the jitter is a viewport shift of (jx, jy) render pixels, x right and y down: the picture moves by (jx, jy);
//  - DLSS's jitterOffset is that same shift (pixel space, x right, y down: the picture's move, the way the camera
//    offset is the negative of it; FSR 2 uses the same convention). TOTK_DLSS_JITTER_SIGN=-1 negates it;
//  - motion vectors are render pixels, previous minus current position, y down, without the jitter
//    (Streamline: mvecScale 1 / size, motionVectorsJittered false).
#pragma once
#include <math.h>
#include <stdint.h>
#include <string.h>

static inline double dlm_halton(unsigned i, unsigned b) { double f = 1, r = 0; while (i) { f /= b; r += f * (i % b); i /= b; } return r; }

// The jitter phase count for a scene cw x ch upscaled to ow x oh: 8 x the area ratio (NVIDIA's guide: at least 8 x
// the scale squared), 8..72.
static inline unsigned dlm_phases(uint32_t cw, uint32_t ch, uint32_t ow, uint32_t oh) {
    if (!cw || !ch) return 8;
    double k2 = ((double)ow * oh) / ((double)cw * ch);
    unsigned ph = (unsigned)ceil(8.0 * k2 - 1e-9);
    return ph < 8 ? 8 : ph > 72 ? 72 : ph;
}
// Phase i (1-based) of the sequence: the viewport shift in render pixels, each -0.5..0.5.
static inline void dlm_jitter(unsigned i, double *jx, double *jy) { *jx = dlm_halton(i, 2) - 0.5; *jy = dlm_halton(i, 3) - 0.5; }
// What DLSS is told for a viewport shift (jx, jy): the same shift, times sign (+1, or -1 from TOTK_DLSS_JITTER_SIGN).
static inline void dlm_jitter_report(double jx, double jy, float sign, float *ox, float *oy) { *ox = (float)(sign * jx); *oy = (float)(sign * jy); }

static inline int dlm_inv4(const double *m, double *o) {
    double inv[16];
    inv[0] = m[5]*m[10]*m[15] - m[5]*m[11]*m[14] - m[9]*m[6]*m[15] + m[9]*m[7]*m[14] + m[13]*m[6]*m[11] - m[13]*m[7]*m[10];
    inv[4] = -m[4]*m[10]*m[15] + m[4]*m[11]*m[14] + m[8]*m[6]*m[15] - m[8]*m[7]*m[14] - m[12]*m[6]*m[11] + m[12]*m[7]*m[10];
    inv[8] = m[4]*m[9]*m[15] - m[4]*m[11]*m[13] - m[8]*m[5]*m[15] + m[8]*m[7]*m[13] + m[12]*m[5]*m[11] - m[12]*m[7]*m[9];
    inv[12] = -m[4]*m[9]*m[14] + m[4]*m[10]*m[13] + m[8]*m[5]*m[14] - m[8]*m[6]*m[13] - m[12]*m[5]*m[10] + m[12]*m[6]*m[9];
    inv[1] = -m[1]*m[10]*m[15] + m[1]*m[11]*m[14] + m[9]*m[2]*m[15] - m[9]*m[3]*m[14] - m[13]*m[2]*m[11] + m[13]*m[3]*m[10];
    inv[5] = m[0]*m[10]*m[15] - m[0]*m[11]*m[14] - m[8]*m[2]*m[15] + m[8]*m[3]*m[14] + m[12]*m[2]*m[11] - m[12]*m[3]*m[10];
    inv[9] = -m[0]*m[9]*m[15] + m[0]*m[11]*m[13] + m[8]*m[1]*m[15] - m[8]*m[3]*m[13] - m[12]*m[1]*m[11] + m[12]*m[3]*m[9];
    inv[13] = m[0]*m[9]*m[14] - m[0]*m[10]*m[13] - m[8]*m[1]*m[14] + m[8]*m[2]*m[13] + m[12]*m[1]*m[10] - m[12]*m[2]*m[9];
    inv[2] = m[1]*m[6]*m[15] - m[1]*m[7]*m[14] - m[5]*m[2]*m[15] + m[5]*m[3]*m[14] + m[13]*m[2]*m[7] - m[13]*m[3]*m[6];
    inv[6] = -m[0]*m[6]*m[15] + m[0]*m[7]*m[14] + m[4]*m[2]*m[15] - m[4]*m[3]*m[14] - m[12]*m[2]*m[7] + m[12]*m[3]*m[6];
    inv[10] = m[0]*m[5]*m[15] - m[0]*m[7]*m[13] - m[4]*m[1]*m[15] + m[4]*m[3]*m[13] + m[12]*m[1]*m[7] - m[12]*m[3]*m[5];
    inv[14] = -m[0]*m[5]*m[14] + m[0]*m[6]*m[13] + m[4]*m[1]*m[14] - m[4]*m[2]*m[13] - m[12]*m[1]*m[6] + m[12]*m[2]*m[5];
    inv[3] = -m[1]*m[6]*m[11] + m[1]*m[7]*m[10] + m[5]*m[2]*m[11] - m[5]*m[3]*m[10] - m[9]*m[2]*m[7] + m[9]*m[3]*m[6];
    inv[7] = m[0]*m[6]*m[11] - m[0]*m[7]*m[10] - m[4]*m[2]*m[11] + m[4]*m[3]*m[10] + m[8]*m[2]*m[7] - m[8]*m[3]*m[6];
    inv[11] = -m[0]*m[5]*m[11] + m[0]*m[7]*m[9] + m[4]*m[1]*m[11] - m[4]*m[3]*m[9] - m[8]*m[1]*m[7] + m[8]*m[3]*m[5];
    inv[15] = m[0]*m[5]*m[10] - m[0]*m[6]*m[9] - m[4]*m[1]*m[10] + m[4]*m[2]*m[9] + m[8]*m[1]*m[6] - m[8]*m[2]*m[5];
    double det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];
    if (!isfinite(det) || fabs(det) < 1e-30) return 0;
    for (int i = 0; i < 16; i++) o[i] = inv[i] / det;
    return 1;
}
static inline void dlm_mul4(const double *a, const double *b, double *o) {
    for (int r = 0; r < 4; r++) for (int c = 0; c < 4; c++) {
        double s = 0; for (int k = 0; k < 4; k++) s += a[r * 4 + k] * b[k * 4 + c];
        o[r * 4 + c] = s;
    }
}
// clip_to_prev = prev_VP * inverse(cur_VP); 0 when cur_VP has no inverse.
static inline int dlm_reproj(const double *cur_vp, const double *prev_vp, double *clip_to_prev) {
    double ic[16];
    if (!dlm_inv4(cur_vp, ic)) return 0;
    dlm_mul4(prev_vp, ic, clip_to_prev);
    return 1;
}
// The motion vector of one sample, exactly as dlss_mv.comp makes it: (px, py) the pixel centre (x right, y down, in
// pixels), z the depth there (0..1), (sx, sy) the viewport shift (jitter) the frame was drawn with. 0 when the point
// is behind the last frame's camera.
static inline int dlm_motion(const double *m, double w, double h, double px, double py, double z, double sx, double sy,
                             double *mx, double *my) {
    double u = (px - sx) / w, v = (py - sy) / h;   // where the sample is without the jitter
    double c[4], p[4] = {u * 2 - 1, 1 - v * 2, z * 2 - 1, 1};
    for (int r = 0; r < 4; r++) c[r] = m[r * 4] * p[0] + m[r * 4 + 1] * p[1] + m[r * 4 + 2] * p[2] + m[r * 4 + 3] * p[3];
    if (!(c[3] > 1e-7)) { *mx = *my = 0; return 0; }
    double pu = c[0] / c[3] * 0.5 + 0.5, pv = 0.5 - 0.5 * c[1] / c[3];
    *mx = (pu - u) * w; *my = (pv - v) * h;
    return 1;
}
// The depth (0..1) of a point d units in front of a GL-style perspective camera with near n, far f.
static inline double dlm_depth_at(double n, double f, double d) {
    if (d < n) d = n;
    if (d > f) d = f;
    double zn = (f + n) / (f - n) - 2.0 * f * n / ((f - n) * d);
    return zn * 0.5 + 0.5;
}
// The largest camera motion over a 3 x 3 grid of the screen at the given depth, in render pixels (the status line's
// "camera motion": what the motion vectors hold for a still world).
static inline double dlm_motion_max(const double *m, double w, double h, double z) {
    double best = 0;
    for (int j = 0; j < 3; j++) for (int i = 0; i < 3; i++) {
        double mx, my;
        if (!dlm_motion(m, w, h, (0.1 + 0.4 * i) * w, (0.1 + 0.4 * j) * h, z, 0, 0, &mx, &my)) continue;
        double l = sqrt(mx * mx + my * my);
        if (isfinite(l) && l > best) best = l;
    }
    return best;
}

// Why DLSS kept the game's upscale on a frame (the log says each the first time, the status line counts them).
enum {
    DLR_OK = 0, DLR_TARGETS, DLR_BLEND, DLR_TEXTURES, DLR_VIEWPORT, DLR_CONSTANTS, DLR_NO_DEPTH,
    DLR_OFF,          // F10 or DLSS not active
    DLR_CAMERA,       // the camera uniform is missing or not a camera
    DLR_BACKOFF,      // a failed evaluate: the game's upscale for ~5 s
    DLR_MATRIX,       // the camera's view-projection has no inverse
    DLR_UNPAIRED,     // the recording found no walk entry for the draw
    DLR_DEPTH_FMT,    // the G-buffer depth is not D32 or smaller than the scene
    DLR_FAILED,       // DLSS was asked and could not record
    DLR_N
};
static inline const char *dlm_reason(int r) {
    static const char *const s[DLR_N] = {"-", "targets", "blend / depth test", "textures", "viewport / scissor",
        "constants", "no G-buffer depth this frame", "DLSS off (F10)", "no camera this frame",
        "backing off after a failed frame", "camera matrix not invertible", "draw not seen by the walk",
        "G-buffer depth not usable", "DLSS could not record"};
    return r >= 0 && r < DLR_N ? s[r] : "?";
}
