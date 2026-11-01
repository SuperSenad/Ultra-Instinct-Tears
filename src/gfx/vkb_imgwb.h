// [UIT 0054] Copying GPU-drawn images back to game memory ("reactive flushing", TOTK_VK_IMAGE_WRITEBACK): the parts
// with no Vulkan in them, so that tools/tests/test_imgwb.c can check them on the box. vkb_tex.c does the rest.
//
// An image is copied (vkCmdCopyImageToBuffer) into host memory as rows of blocks, level after level, the layers of a
// level one after the other (iwb_make_plan says where). The CPU then lays every level and layer out in the
// texture's own storage (nvn_swizzle: Tegra block-linear, or pitch-linear) in a scratch copy of its game memory, and
// that copy goes to game memory page by page (vkb_tex.c: pages the CPU wrote since the rendering stay the CPU's).
#pragma once
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include "tex/nvn_deswizzle.h"

enum { IWB_OFF = 0, IWB_EXPLICIT = 1, IWB_ALL = 2 };
enum { IWB_MAX_LEVELS = 16 };
enum { IWB_SMALL_MAX = 64 * 1024 };    // "All CPU-read targets": copied back at the end of every submission that draws them
enum { IWB_SNAP_MAX = 256 * 1024 };    // a snapshot of the storage when drawn: its bytes unchanged = the CPU left it alone
enum { IWB_EXPLICIT_MAX = 64 << 20 };  // larger: not copied back (a stall of tens of ms; logged)

// [UIT 0057] Unset / "-" / "1" / "explicit" (and anything unknown): at the game's explicit reads, the default (the clean
// run: grass stays, about 0.2 ms of waiting a frame in the world). "0" / "off" / "no": off. "2" / "all": also every
// submission's small targets.
static inline int iwb_mode_parse(const char *s) {
    if (!s || !*s || *s == '-') return IWB_EXPLICIT;
    if (*s == '0' || *s == 'o' || *s == 'O' || *s == 'n' || *s == 'N') return IWB_OFF;
    if (*s == '2' || *s == 'a' || *s == 'A') return IWB_ALL;
    return IWB_EXPLICIT;
}

// At the end of a submission that drew it (vkb_iwb_record, why 0): copied back now (1) or left pending for an explicit
// read (0). All CPU-read targets: the small ones; [UIT 0137] read through another texture's shape (alias); [UIT 0255]
// read back by the CPU before (hot, set only with TOTK_VK_READBACK_AHEAD on): the small ones.
static inline int iwb_pick_end(int mode, int alias, int hot, uint64_t size) {
    return (mode == IWB_ALL && size <= IWB_SMALL_MAX) || (alias && size <= IWB_EXPLICIT_MAX) || (hot && size <= IWB_SMALL_MAX);
}
// [UIT 0255] After an explicit read copied it back: marked hot (copied with each drawing submission from then on).
static inline int iwb_mark_hot(int on, int hot, uint64_t size, uint64_t nhot, uint64_t max) {
    return on && !hot && size <= IWB_SMALL_MAX && nhot < max;
}

// NVN formats whose bytes are the same in the Vulkan image (vkb_format): R8..RG32 (0x01-0x18), RGBA8..RGBA32
// (0x25-0x30), RGBX8 / RGBA8 sRGB (0x37, 0x38), RGB10A2 (0x3D, 0x3E), R11G11B10F (0x3F), RGB9E5 (0x40), BGRX8 /
// BGRA8 and their sRGB (0x99-0x9C; [UIT 0340] 0x98 is A1BGR5, 16-bit). Not: the 3-component formats (expanded on upload), depth / stencil, the 16-bit
// packed formats (bit order differs from Vulkan's PACK16), block-compressed and ASTC (never drawn into here).
static inline int iwb_format_ok(int f) {
    return (f >= 0x01 && f <= 0x18) || (f >= 0x25 && f <= 0x30) || f == 0x37 || f == 0x38 || (f >= 0x3D && f <= 0x40) ||
           (f >= 0x99 && f <= 0x9C);
}
// 1D, 2D, 1D / 2D arrays, rectangle, cube, cube array. Not 3D, multisample, buffer.
static inline int iwb_target_ok(int target) {
    return target == 0 || target == 1 || target == 3 || target == 4 || target == 7 || target == 8 || target == 9;
}
// NVN texture flags that keep a texture out: DISPLAY, COMPRESSIBLE, ADAPTIVE_ZCULL, DEPTH16_PREFER_FAST_CLEAR,
// ZCULL_SUPPORT_STENCIL (as the Metal port: GPU-only render targets of the frame, never read by the CPU).
static inline int iwb_flags_ok(int flags) { return !(flags & (0x1 | 0x8 | 0x80 | 0x100 | 0x400)); }
// Memory pool flags CPU_UNCACHED / CPU_CACHED: the CPU can read the pool (else the game cannot read the image at all).
static inline int iwb_pool_ok(uint32_t pool_flags) { return (pool_flags & (0x2 | 0x4)) != 0; }

typedef struct {
    uint32_t w, h;          // texels of the level
    uint32_t pitch, rows;   // bytes per row of blocks, rows of blocks
    uint64_t off;           // where the level starts in host memory (16-byte aligned)
    uint64_t layer_bytes;   // pitch * rows: layer k of the level at off + k * layer_bytes
} iwb_level;
typedef struct {
    int levels;
    uint32_t layers, bpb, bw, bh;
    uint64_t total;         // host memory for all of it
    iwb_level lv[IWB_MAX_LEVELS];
} iwb_plan;

// The copy's layout for `levels` levels and `layers` layers of a texture (the image's, at most the texture's).
// -1: not supported (format, target, samples, a linear texture with more than one level or layer, bytes per block
// that do not divide 16: Vulkan wants buffer offsets that are multiples of the texel size).
static inline int iwb_make_plan(const nvn_texdesc *d, uint32_t levels, uint32_t layers, iwb_plan *p) {
    memset(p, 0, sizeof *p);
    if (!d || !iwb_format_ok(d->format) || !iwb_target_ok(d->target) || d->samples > 1) return -1;
    int bw = 1, bh = 1, bpb = nvn_fmt_bpb(d->format, &bw, &bh);
    if (bpb <= 0 || 16 % bpb || bw != 1 || bh != 1) return -1;
    uint32_t dl = d->levels ? d->levels : 1;
    nvn_texlayout L;
    nvn_tex_layout(d, &L);
    if (!levels || levels > dl || levels > IWB_MAX_LEVELS || !layers || layers > L.layers) return -1;
    if (nvn_tex_is_linear(d) && (levels != 1 || layers != 1)) return -1;
    int one_d = d->target == 0 || d->target == 3;
    uint64_t at = 0;
    for (uint32_t l = 0; l < levels; l++) {
        iwb_level *v = &p->lv[l];
        v->w = d->width >> l ? d->width >> l : 1;
        v->h = one_d ? 1 : (d->height >> l ? d->height >> l : 1);
        v->pitch = v->w * (uint32_t)bpb;
        v->rows = v->h;
        v->off = at;
        v->layer_bytes = (uint64_t)v->pitch * v->rows;
        at = (at + v->layer_bytes * layers + 15) & ~(uint64_t)15;
    }
    p->levels = (int)levels; p->layers = layers; p->bpb = (uint32_t)bpb; p->bw = 1; p->bh = 1; p->total = at;
    return 0;
}
// Lay the copied levels and layers out in `storage` (the texture's whole storage: a scratch copy of its game memory,
// L.size bytes). Bytes of the storage that no level or layer covers are left as they are. 0, or -1 if a level could
// not be laid out (then storage may be partly written: the caller drops it).
static inline int iwb_apply(const nvn_texdesc *d, const iwb_plan *p, const uint8_t *host, uint8_t *storage) {
    for (int l = 0; l < p->levels; l++)
        for (uint32_t k = 0; k < p->layers; k++)
            if (nvn_swizzle(d, (unsigned)l, k, host + p->lv[l].off + k * p->lv[l].layer_bytes, p->lv[l].pitch, storage)) return -1;
    return 0;
}
static inline int iwb_overlap(uint64_t a, uint64_t an, uint64_t b, uint64_t bn) { return a < b + bn && b < a + an; }

// [UIT 0056] Game reads (invalidates) over GPU-drawn images, watched in every mode for the whole run (0054 logged only
// the first 64 within the first 4000 reads, all before the game was played). A read range's images are found by a scan
// of every image, so the ranges are cached: an entry is scanned again when the image set changed (gen) or it is older
// than a window (frames). Each window's distinct images read go in a small set to show when they change.
#define IWB_RC_CAP 128                   // cached read ranges (the game reads ~4-12 ranges a frame)
#define IWB_RC_IMGS 4                    // images remembered per range
typedef struct iwb_rc_ent {
    uint64_t lo, hi, gen, frame;         // hi 0: empty
    int n;                               // GPU-drawn images under the range (up to IWB_RC_IMGS kept)
    uint64_t serial[IWB_RC_IMGS];
} iwb_rc_ent;
// The entry for [lo, hi): *fresh 1 when the caller must scan and fill it (new, image set changed, or older than `age`
// frames). A full table drops the entry looked at least recently among the probed ones.
static inline iwb_rc_ent *iwb_rc_get(iwb_rc_ent *t, uint64_t lo, uint64_t hi, uint64_t gen, uint64_t frame, uint64_t age, int *fresh) {
    uint64_t h = (lo >> 7) ^ (hi >> 3) ^ (lo >> 19);
    iwb_rc_ent *victim = NULL;
    for (int i = 0; i < 8; i++) {
        iwb_rc_ent *e = &t[(h + (uint64_t)i) % IWB_RC_CAP];
        if (e->hi && e->lo == lo && e->hi == hi) {
            *fresh = e->gen != gen || frame - e->frame >= age;
            return e;
        }
        if (!e->hi) { if (!victim || victim->hi) victim = e; }
        else if (!victim || (victim->hi && e->frame < victim->frame)) victim = e;
    }
    *victim = (iwb_rc_ent){.lo = lo, .hi = hi};
    *fresh = 1;
    return victim;
}
static inline void iwb_rc_fill(iwb_rc_ent *e, uint64_t gen, uint64_t frame, int n, const uint64_t *serial) {
    e->gen = gen; e->frame = frame; e->n = n;
    for (int i = 0; i < n && i < IWB_RC_IMGS; i++) e->serial[i] = serial[i];
}
#define IWB_WSET 8
typedef struct iwb_wset { int n, over; uint64_t s[IWB_WSET]; } iwb_wset;   // over: more than IWB_WSET were read
static inline int iwb_wset_has(const iwb_wset *w, uint64_t s) {
    for (int i = 0; i < w->n; i++) if (w->s[i] == s) return 1;
    return 0;
}
static inline void iwb_wset_add(iwb_wset *w, uint64_t s) {
    if (iwb_wset_has(w, s)) return;
    if (w->n < IWB_WSET) w->s[w->n++] = s; else w->over = 1;
}
static inline int iwb_wset_same(const iwb_wset *a, const iwb_wset *b) {   // in any order
    if (a->n != b->n || a->over != b->over) return 0;
    for (int i = 0; i < a->n; i++) if (!iwb_wset_has(b, a->s[i])) return 0;
    return 1;
}
