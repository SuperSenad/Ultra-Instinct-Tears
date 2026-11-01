// [perf-mt] TOTK_DLSS_MV_PROBE=1: a log-only survey of the game's render targets, to answer one question with a capture
// from the user's own machine: does TOTK write a motion (velocity) buffer of its own that DLSS could use instead of
// the camera-only vectors of dlss_mv.comp? (Design option A.)
//
// On a few frames (600, 601, then every 3600th up to MVP_FRAMES) the walk (vkb_dlss.c, role 1 or 2) passes every
// draw's render target set here; each change of set between the frame's start and the game's upscale draw is logged
// once: the colour targets' NVN formats and sizes, the depth, and the first draw's fragment shader key. A two-channel
// 16/32-bit float or snorm target at the scene's size is marked "velocity candidate". Nothing is drawn, tagged or
// changed: with the probe off this costs one load and a branch per draw, and only while DLSS runs.
// Pure C (no Vulkan) so tools/tests/test_mvprobe.c can test it.
#ifndef VKB_MVPROBE_H
#define VKB_MVPROBE_H
#include <stdint.h>
#include <stdio.h>
#include <string.h>

enum { MVP_FRAMES = 8, MVP_LINES = 96 };

typedef struct {                 // one render target set, as the probe sees it
    int n;                       // colour slots (0..8)
    uint32_t fmt[8], w[8], h[8]; // NVN formats (0: an empty slot)
    uint64_t ser[8];             // tex_t serials: identify the textures
    uint32_t dfmt, dw, dh;       // depth (dfmt 0: none)
    uint64_t dser;
} mvp_rt;

typedef struct {
    uint64_t frame;              // the frame being probed (0: none)
    uint64_t done;               // the last frame whose upscale draw was seen (its later draws are not probed)
    int probed;                  // frames probed so far
    int lines, sets, cands;      // this frame
    mvp_rt last;
    int have_last;
} mvp_state;

static inline int mvp_frame_wanted(uint64_t f, int probed) {
    if (probed >= MVP_FRAMES) return 0;
    return f == 600 || f == 601 || (f > 601 && (f - 600) % 3600 == 0);
}

static inline const char *mvp_fmt_name(uint32_t f, char *buf, size_t n) {
    static const struct { uint32_t f; const char *s; } t[] = {
        {0x01, "R8"}, {0x02, "R8SN"}, {0x05, "R16F"}, {0x06, "R16"}, {0x07, "R16SN"}, {0x0A, "R32F"}, {0x0B, "R32UI"},
        {0x0D, "RG8"}, {0x0E, "RG8SN"}, {0x11, "RG16F"}, {0x12, "RG16"}, {0x13, "RG16SN"}, {0x14, "RG16UI"},
        {0x15, "RG16I"}, {0x16, "RG32F"}, {0x25, "RGBA8"}, {0x26, "RGBA8SN"}, {0x29, "RGBA16F"}, {0x2A, "RGBA16"},
        {0x2E, "RGBA32F"}, {0x32, "D16"}, {0x33, "D24"}, {0x34, "D32F"}, {0x35, "D24S8"}, {0x36, "D32FS8"},
        {0x38, "RGBA8_SRGB"}, {0x3D, "RGB10A2"}, {0x3F, "R11G11B10F"}, {0x40, "RGB9E5F"}};
    for (size_t i = 0; i < sizeof t / sizeof *t; i++) if (t[i].f == f) return t[i].s;
    snprintf(buf, n, "fmt 0x%x", (unsigned)f);
    return buf;
}

// a format a velocity buffer is normally kept in: two channels, signed (float or snorm), 16 or 32 bits
static inline int mvp_velocity_format(uint32_t f) { return f == 0x11 || f == 0x13 || f == 0x16; }

// a velocity candidate: such a format, at least half the scene's size (some engines keep it at half resolution)
static inline int mvp_candidate(uint32_t f, uint32_t w, uint32_t h, uint32_t sw, uint32_t sh) {
    if (!mvp_velocity_format(f) || !w || !h) return 0;
    if (!sw || !sh) return 1;
    return w * 2 >= sw && h * 2 >= sh && w <= sw * 2 && h <= sh * 2;
}

static inline int mvp_same(const mvp_rt *a, const mvp_rt *b) {
    if (a->n != b->n || a->dser != b->dser || a->dfmt != b->dfmt) return 0;
    for (int i = 0; i < a->n && i < 8; i++) if (a->ser[i] != b->ser[i] || a->fmt[i] != b->fmt[i]) return 0;
    return 1;
}

// one draw of the walk. frame: vkb_frame(). sw/sh: the scene size (the G-buffer depth's, 0 before it is seen).
// Writes a log line into out (and returns 1) when the set is new for this probed frame; 0 otherwise.
static inline int mvp_draw(mvp_state *s, uint64_t frame, const mvp_rt *rt, const char *fs_key, uint32_t sw, uint32_t sh,
                           char *out, size_t outn) {
    if (frame == s->done) return 0;
    if (s->frame != frame) {
        if (!mvp_frame_wanted(frame, s->probed)) { s->frame = 0; return 0; }
        s->frame = frame; s->probed++; s->lines = s->sets = s->cands = 0; s->have_last = 0;
    }
    if (s->have_last && mvp_same(&s->last, rt)) return 0;
    s->last = *rt; s->have_last = 1; s->sets++;
    if (s->lines >= MVP_LINES) return 0;
    s->lines++;
    size_t k = 0;
    int cand = 0;
    k += (size_t)snprintf(out + k, outn - k, "mv-probe frame %llu set %d:", (unsigned long long)frame, s->sets);
    for (int i = 0; i < rt->n && i < 8 && k < outn; i++) {
        char b[16];
        if (!rt->fmt[i]) { k += (size_t)snprintf(out + k, outn - k, " #%d -", i); continue; }
        int c = mvp_candidate(rt->fmt[i], rt->w[i], rt->h[i], sw, sh);
        cand |= c;
        k += (size_t)snprintf(out + k, outn - k, " #%d %s %ux%u%s", i, mvp_fmt_name(rt->fmt[i], b, sizeof b), rt->w[i], rt->h[i],
                              c ? " (velocity candidate)" : "");
    }
    if (k < outn) {
        char b[16];
        if (rt->dfmt) k += (size_t)snprintf(out + k, outn - k, ", depth %s %ux%u", mvp_fmt_name(rt->dfmt, b, sizeof b), rt->dw, rt->dh);
        else k += (size_t)snprintf(out + k, outn - k, ", no depth");
    }
    if (k < outn) snprintf(out + k, outn - k, ", fs %.16s", fs_key ? fs_key : "-");
    s->cands += cand;
    return 1;
}

// the game's upscale draw: the frame's summary line (1), once per probed frame
static inline int mvp_frame_end(mvp_state *s, uint64_t frame, char *out, size_t outn) {
    if (!s->frame || s->frame != frame) return 0;
    snprintf(out, outn, "mv-probe frame %llu: %d render target sets up to the upscale, %d with a velocity candidate%s",
             (unsigned long long)frame, s->sets, s->cands,
             s->cands ? " (send this log: the candidate's shader keys tell which pass writes it)"
                      : " (no velocity-like target: the camera-only vectors stay the only source)");
    s->frame = 0; s->done = frame;
    return 1;
}
#endif
