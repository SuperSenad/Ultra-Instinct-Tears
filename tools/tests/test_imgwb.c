// Box test for src/gfx/vkb_imgwb.h ([UIT 0054] copying GPU-drawn images back to game memory):
//   cc -O2 -Wall -Isrc/gfx tools/tests/test_imgwb.c src/gfx/tex/nvn_deswizzle.c src/gfx/nvn/nvn_tex.c -o /tmp/t/iwb && /tmp/t/iwb
// For each texture: a host copy laid out as iwb_make_plan says (as vkCmdCopyImageToBuffer writes it), iwb_apply into
// storage filled with a marker; every texel must land where nvn_bl_offset (or the linear pitch) says, nothing outside
// the texture's levels / layers may change, and nvn_deswizzle must give the host rows back.
#include <stdio.h>
#include "vkb_imgwb.h"

static int fails, cases;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

static uint8_t val(int l, uint32_t k, uint64_t i) { return (uint8_t)(i * 131u + (uint64_t)l * 29u + k * 71u + 1u); }

static void one(const char *name, nvn_texdesc d, uint32_t levels, uint32_t layers) {
    cases++;
    iwb_plan p;
    if (iwb_make_plan(&d, levels, layers, &p)) { printf("FAIL %s: no plan\n", name); fails++; return; }
    nvn_texlayout L; nvn_tex_layout(&d, &L);
    uint8_t *host = malloc(p.total), *st = malloc(L.size), *cover = calloc(1, L.size);
    for (int l = 0; l < p.levels; l++) {
        CHECK(p.lv[l].off % 16 == 0);
        CHECK(p.lv[l].off + p.lv[l].layer_bytes * p.layers <= p.total);
        for (uint32_t k = 0; k < p.layers; k++)
            for (uint64_t i = 0; i < p.lv[l].layer_bytes; i++) host[p.lv[l].off + k * p.lv[l].layer_bytes + i] = val(l, k, i);
    }
    memset(st, 0xCD, L.size);
    CHECK(iwb_apply(&d, &p, host, st) == 0);
    int bad = 0;
    for (int l = 0; l < p.levels && !bad; l++)
        for (uint32_t k = 0; k < p.layers && !bad; k++) {
            const iwb_level *v = &p.lv[l];
            uint64_t base; uint32_t rb, rows, slices, bpb; unsigned th, td;
            if (nvn_tex_is_linear(&d)) { base = 0; rb = nvn_tex_linear_stride(&d); rows = v->rows; th = td = 0; }
            else if (nvn_deswizzle_geom(&d, (unsigned)l, k, &rb, &rows, &slices, &base, &th, &td, &bpb)) { bad = 1; break; }
            for (uint32_t y = 0; y < v->rows && !bad; y++)
                for (uint32_t x = 0; x < v->pitch; x++) {
                    uint64_t o = base + (nvn_tex_is_linear(&d) ? (uint64_t)y * rb + x : nvn_bl_offset(x, y, 0, rb, rows, th, td));
                    if (o >= L.size || st[o] != val(l, k, (uint64_t)y * v->pitch + x)) { bad = 1; break; }
                    cover[o] = 1;
                }
        }
    CHECK(!bad);
    uint64_t outside = 0;
    for (uint64_t i = 0; i < L.size; i++) if (!cover[i] && st[i] != 0xCD) outside++;
    CHECK(outside == 0);
    // nvn_deswizzle gives the host rows back
    uint8_t *back = malloc(p.total);
    for (int l = 0; l < p.levels; l++)
        for (uint32_t k = 0; k < p.layers; k++)
            CHECK(nvn_deswizzle(&d, (unsigned)l, k, st, back + p.lv[l].off + k * p.lv[l].layer_bytes, p.lv[l].pitch) == 0);
    int same = 1;
    for (int l = 0; l < p.levels; l++)
        if (memcmp(back + p.lv[l].off, host + p.lv[l].off, p.lv[l].layer_bytes * p.layers)) same = 0;
    CHECK(same);
    if (bad || outside || !same) printf("  in %s (outside %llu)\n", name, (unsigned long long)outside);
    free(host); free(st); free(cover); free(back);
}
static nvn_texdesc tex(int target, int format, uint32_t w, uint32_t h, uint32_t depth, uint32_t levels) {
    nvn_texdesc d; memset(&d, 0, sizeof d);
    d.target = target; d.format = format; d.width = w; d.height = h; d.depth = depth; d.levels = levels; d.samples = 0;
    return d;
}
int main(void) {
    // TotK's grass cut-reaction target: 2D 128x128 RGBA8, one level (64 KiB: the "All CPU-read targets" size)
    nvn_texdesc g = tex(1, 0x25, 128, 128, 1, 1);
    one("grass 128x128 RGBA8", g, 1, 1);
    { nvn_texlayout L; nvn_tex_layout(&g, &L); CHECK(L.size == IWB_SMALL_MAX); }
    // terrain tile normal maps: 260x260 RG8, 9 levels (209.5 KiB of storage: under the snapshot size)
    // packaged layout, block height 1 GOB: 214528 bytes, the size the Metal port logged for them
    nvn_texdesc t = tex(1, 0x0D, 260, 260, 1, 9); t.pkg_block_h = 1;
    { nvn_texlayout L; nvn_tex_layout(&t, &L); CHECK(L.size == 214528); CHECK(L.size <= IWB_SNAP_MAX); }
    one("terrain 260x260 RG8 9 levels (packaged, 1 GOB)", t, 9, 1);
    one("terrain, first 4 levels", t, 4, 1);
    one("260x260 RG8 9 levels, automatic block height", tex(1, 0x0D, 260, 260, 1, 9), 9, 1);
    one("NPOT 100x37 R32F", tex(1, 0x0B, 100, 37, 1, 1), 1, 1);
    one("NPOT 333x77 RGBA16F 3 levels", tex(1, 0x2D, 333, 77, 1, 3), 3, 1);
    one("2D array 64x64 RGBA8 x5 layers 2 levels", tex(4, 0x25, 64, 64, 5, 2), 2, 5);
    one("cube 32x32 RGBA8 3 levels", tex(8, 0x25, 32, 32, 6, 3), 3, 6);
    one("R8 17x9", tex(1, 0x01, 17, 9, 1, 1), 1, 1);
    one("RGBA32F 40x40 2 levels", tex(1, 0x2E, 40, 40, 1, 2), 2, 1);
    one("BGRA8 640x360", tex(1, 0x9A, 640, 360, 1, 1), 1, 1);
    one("1D 256 RGBA8", tex(0, 0x25, 256, 1, 1, 1), 1, 1);
    nvn_texdesc lin = tex(1, 0x25, 100, 50, 1, 1); lin.flags = 0x40;   // LINEAR_RENDER_TARGET: pitch storage
    one("linear render target 100x50 RGBA8", lin, 1, 1);
    // [UIT 0056] the images the game read every frame in the 1be1def report: 1x1 linear render targets, RGBA8 and RGBA32F
    nvn_texdesc one8 = tex(1, 0x25, 1, 1, 1, 1); one8.flags = 0x40;
    nvn_texdesc one32 = tex(1, 0x2E, 1, 1, 1, 1); one32.flags = 0x40;
    one("1x1 linear RGBA8 (image 3313)", one8, 1, 1);
    one("1x1 linear RGBA32F (image 3314)", one32, 1, 1);
    // [UIT 0056] the read-range cache and the window sets
    {
        static iwb_rc_ent t[IWB_RC_CAP];
        int fr;
        iwb_rc_ent *e = iwb_rc_get(t, 0x1037f87000, 0x1037f87004, 1, 100, 60, &fr);
        CHECK(fr && e->lo == 0x1037f87000 && e->hi == 0x1037f87004);
        uint64_t s3[3] = {3313, 3314, 3315};
        iwb_rc_fill(e, 1, 100, 3, s3);
        iwb_rc_ent *f = iwb_rc_get(t, 0x1037f87000, 0x1037f87004, 1, 120, 60, &fr);
        CHECK(f == e && !fr && f->n == 3 && f->serial[2] == 3315);
        CHECK(iwb_rc_get(t, 0x1037f87000, 0x1037f87004, 2, 120, 60, &fr) == e && fr);    // the image set changed
        CHECK(iwb_rc_get(t, 0x1037f87000, 0x1037f87004, 1, 160, 60, &fr) == e && fr);    // a window old
        CHECK(iwb_rc_get(t, 0x1037f87000, 0x1037f87008, 1, 120, 60, &fr) != e && fr);    // another range
        // many more ranges than the table holds: every lookup gives an entry for its own range, the table stays usable
        int bad = 0;
        for (uint64_t i = 0; i < 4 * IWB_RC_CAP; i++) {
            uint64_t lo = 0x2000000000ull + i * 0x80;
            iwb_rc_ent *x = iwb_rc_get(t, lo, lo + 0x10, 1, 200 + i, 60, &fr);
            if (!x || x->lo != lo || x->hi != lo + 0x10 || !fr) bad++;
            uint64_t one = i;
            iwb_rc_fill(x, 1, 200 + i, 1, &one);
            x = iwb_rc_get(t, lo, lo + 0x10, 1, 200 + i, 60, &fr);
            if (!x || fr || x->serial[0] != i) bad++;
        }
        CHECK(bad == 0);
        iwb_wset a = {0}, b = {0};
        iwb_wset_add(&a, 3313); iwb_wset_add(&a, 3314); iwb_wset_add(&a, 3313);
        iwb_wset_add(&b, 3314); iwb_wset_add(&b, 3313);
        CHECK(a.n == 2 && iwb_wset_same(&a, &b));
        iwb_wset_add(&b, 9000);
        CHECK(!iwb_wset_same(&a, &b));
        for (uint64_t i = 0; i < 20; i++) iwb_wset_add(&a, 100 + i);
        CHECK(a.n == IWB_WSET && a.over && !iwb_wset_same(&a, &b));
        iwb_wset z1 = {0}, z2 = {0};
        CHECK(iwb_wset_same(&z1, &z2));
    }
    // refused
    iwb_plan p;
    CHECK(iwb_make_plan(&(nvn_texdesc){ .target = 2, .format = 0x25, .width = 8, .height = 8, .depth = 8, .levels = 1 }, 1, 1, &p) == -1);   // 3D
    CHECK(iwb_make_plan(&(nvn_texdesc){ .target = 5, .format = 0x25, .width = 8, .height = 8, .depth = 1, .levels = 1, .samples = 4 }, 1, 1, &p) == -1);   // MS
    CHECK(iwb_make_plan(&(nvn_texdesc){ .target = 1, .format = 0x42, .width = 8, .height = 8, .depth = 1, .levels = 1 }, 1, 1, &p) == -1);   // BC1
    CHECK(iwb_make_plan(&(nvn_texdesc){ .target = 1, .format = 0x1C, .width = 8, .height = 8, .depth = 1, .levels = 1 }, 1, 1, &p) == -1);   // RGB 3-comp
    CHECK(iwb_make_plan(&(nvn_texdesc){ .target = 1, .format = 0x33, .width = 8, .height = 8, .depth = 1, .levels = 1 }, 1, 1, &p) == -1);   // depth
    CHECK(iwb_make_plan(&(nvn_texdesc){ .target = 1, .format = 0x3A, .width = 8, .height = 8, .depth = 1, .levels = 1 }, 1, 1, &p) == -1);   // packed 16-bit
    CHECK(iwb_make_plan(&g, 2, 1, &p) == -1);   // more levels than the texture
    CHECK(iwb_make_plan(&lin, 1, 2, &p) == -1); // a linear texture's second layer
    CHECK(iwb_format_ok(0x25) && iwb_format_ok(0x0D) && iwb_format_ok(0x3F) && iwb_format_ok(0x9B) && iwb_format_ok(0x9C) && !iwb_format_ok(0x98) && !iwb_format_ok(0x95) && !iwb_format_ok(0x79));
    CHECK(iwb_flags_ok(0) && iwb_flags_ok(0x40) && !iwb_flags_ok(0x8) && !iwb_flags_ok(0x1));
    CHECK(iwb_pool_ok(0x2 | 0x8) && iwb_pool_ok(0x4) && !iwb_pool_ok(0x1 | 0x8));
    CHECK(iwb_mode_parse(NULL) == IWB_EXPLICIT && iwb_mode_parse("") == IWB_EXPLICIT && iwb_mode_parse("-") == IWB_EXPLICIT &&
          iwb_mode_parse("1") == IWB_EXPLICIT && iwb_mode_parse("explicit") == IWB_EXPLICIT && iwb_mode_parse("x") == IWB_EXPLICIT);
    CHECK(iwb_mode_parse("0") == IWB_OFF && iwb_mode_parse("off") == IWB_OFF && iwb_mode_parse("Off") == IWB_OFF && iwb_mode_parse("no") == IWB_OFF);
    CHECK(iwb_mode_parse("2") == IWB_ALL && iwb_mode_parse("all") == IWB_ALL);
    CHECK(iwb_overlap(100, 10, 109, 1) && !iwb_overlap(100, 10, 110, 5) && iwb_overlap(0, 1000, 500, 1) && !iwb_overlap(5, 0, 5, 10));
    printf("%s: %d layouts, %d failures\n", fails ? "FAIL" : "ok", cases, fails);
    return fails != 0;
}
