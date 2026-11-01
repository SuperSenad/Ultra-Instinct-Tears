// Box test for [UIT 0310] (only the written rows of a changed texture uploaded again, src/gfx/vkb_texrows.h, vkb_tex.c
// tex_upload_rows) and [UIT 0311] (WriteTexels laying out only the region, nvn_swizzle_region):
//   cc -O2 -Wall -Isrc/gfx tools/tests/test_texrows.c src/gfx/tex/nvn_deswizzle.c src/gfx/nvn/nvn_tex.c src/gfx/nvn/nvn_dirty_win.c -lpthread -o /tmp/t/texrows && /tmp/t/texrows
// 1. nvn_deswizzle_rows gives exactly those rows of nvn_deswizzle.
// 2. nvn_swizzle_region leaves the storage byte for byte as the old way (whole level read out, region put in, whole
//    level laid out again), and changes nothing outside the GOBs under the region.
// 3. texrows_mark marks every GOB row a written byte falls in (brute force over nvn_bl_offset).
// 4. The upload model: after region writes, the pages that changed -> GOB rows -> those rows uploaded into an old copy
//    of the texture give the texture exactly (the field-log case: 5120x5120 BC4, 7 levels), and far fewer bytes.
// 5. nvn_dirty_runs_fresh sees a write made after this epoch's scan (nvn_dirty_runs does not until the next epoch).
// 6. The pick and the queue's size (UIT 0256 limit): the field-log writes are queued now, a whole-level write is not.
// TOTK_PROBE_NEGATIVE=1: the checks are fed a wrong answer each (the last run dropped, a mark missed, the fresh scan
// replaced by the plain one, the old size for the queue) and must fail.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tex/nvn_deswizzle.h"
#include "nvn/nvn_tex.h"
#include "nvn/nvn_dirty.h"
#include "vkb_texrows.h"
#include "nvn/nvn_wtq.h"

static int fails, neg;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

// ---- mock write watch (as test_dirty_bigscan.c): pages flagged by w()
#define NP 8192
static uint8_t *g_mem;
static uint8_t g_bit[NP];
int plat_written_pages(uint64_t addr, uint64_t size, uint64_t *pages, int max, uint64_t *page) {
    *page = 4096;
    uintptr_t b = (uintptr_t)g_mem;
    if (addr < b || addr + size > b + (uint64_t)NP * 4096) return 0;
    int n = 0;
    uint64_t p0 = (addr - b) / 4096, p1 = (addr + size - b + 4095) / 4096;
    for (uint64_t p = p0; p < p1 && n < max; p++) if (g_bit[p]) pages[n++] = b + p * 4096;
    for (uint64_t p = p0; p < p1; p++) if (n >= max) g_bit[p] = 0;
    for (int i = 0; i < n; i++) g_bit[(pages[i] - b) / 4096] = 0;
    return n;
}

static uint32_t rng = 12345;
static uint32_t rnd(void) { rng = rng * 1664525u + 1013904223u; return rng >> 8; }

static nvn_texdesc tex(int target, int format, uint32_t w, uint32_t h, uint32_t depth, uint32_t levels) {
    nvn_texdesc d; memset(&d, 0, sizeof d);
    d.target = target; d.format = format; d.width = w; d.height = h; d.depth = depth; d.levels = levels;
    return d;
}
typedef struct { uint32_t bx, by, ld, pitch; } lvl;
static lvl lv(const nvn_texdesc *d, unsigned level) {
    int bw, bh, bpb = nvn_fmt_bpb(d->format, &bw, &bh);
    uint32_t w = d->width >> level ? d->width >> level : 1, h = d->height >> level ? d->height >> level : 1;
    uint32_t dd = d->target == 2 ? (d->depth >> level ? d->depth >> level : 1) : 1;
    lvl o = {(w + bw - 1) / bw, (h + bh - 1) / bh, dd, 0};
    if (d->target == 0) o.by = 1;
    o.pitch = o.bx * (uint32_t)bpb;
    return o;
}
static uint32_t nlayers(const nvn_texdesc *d) { return d->target == 2 || d->target == 0 || d->target == 1 ? 1 : d->depth; }

// 1 + 2: rows and region against the whole-level way
static void rows_and_region(const char *name, nvn_texdesc d, int writes) {
    nvn_texlayout L; nvn_tex_layout(&d, &L);
    uint8_t *a = malloc(L.size), *b = malloc(L.size);
    for (uint64_t i = 0; i < L.size; i++) a[i] = (uint8_t)rnd();
    memcpy(b, a, L.size);
    int bad_rows = 0, bad_reg = 0, bad_out = 0, unsup = 0;
    for (unsigned level = 0; level < d.levels; level++)
        for (uint32_t layer = 0; layer < nlayers(&d); layer++) {
            lvl v = lv(&d, level);
            if (nvn_tex_is_linear(&d) && (level || layer)) continue;
            size_t lb = (size_t)v.pitch * v.by * v.ld;
            uint8_t *full = malloc(lb), *part = malloc(lb);
            CHECK(nvn_deswizzle(&d, level, layer, a, full, v.pitch) == 0);
            for (int k = 0; k < 6; k++) {
                uint32_t y0 = rnd() % v.by, y1 = y0 + 1 + rnd() % (v.by - y0);
                if (k == 0) { y0 = 0; y1 = v.by; }
                memset(part, 0xEE, lb);
                CHECK(nvn_deswizzle_rows(&d, level, layer, a, part, v.pitch, y0, y1) == 0);
                for (uint32_t z = 0; z < v.ld; z++)
                    if (memcmp(part + (size_t)z * (y1 - y0) * v.pitch, full + ((size_t)z * v.by + y0) * v.pitch, (size_t)(y1 - y0) * v.pitch)) bad_rows++;
            }
            // region writes: whole-level way into a, straight in into b
            for (int k = 0; k < writes; k++) {
                int bw, bh, bpb = nvn_fmt_bpb(d.format, &bw, &bh);
                uint32_t rx = rnd() % v.bx, ry = rnd() % v.by, rz = rnd() % v.ld;
                uint32_t rw = 1 + rnd() % (v.bx - rx < 9 ? v.bx - rx : 9), rh = 1 + rnd() % (v.by - ry < 9 ? v.by - ry : 9);
                size_t srow = (size_t)rw * bpb + (rnd() % 3) * 4;
                uint8_t *src = malloc(srow * rh);
                for (size_t i = 0; i < srow * rh; i++) src[i] = (uint8_t)rnd();
                CHECK(nvn_deswizzle(&d, level, layer, a, full, v.pitch) == 0);
                for (uint32_t r = 0; r < rh; r++) memcpy(full + ((size_t)rz * v.by + ry + r) * v.pitch + (size_t)rx * bpb, src + r * srow, (size_t)rw * bpb);
                CHECK(nvn_swizzle(&d, level, layer, full, v.pitch, a) == 0);
                uint8_t *before = malloc(L.size); memcpy(before, b, L.size);
                if (nvn_swizzle_region(&d, level, layer, rx * (uint32_t)bpb, ry, rz, rw * (uint32_t)bpb, rh, src, srow, b)) { unsup++; memcpy(b, a, L.size); free(before); free(src); continue; }
                if (memcmp(a, b, L.size)) bad_reg++;
                // nothing outside the region's own bytes changed (so outside its GOBs neither)
                if (!nvn_tex_is_linear(&d)) {
                    uint32_t rb, rows, sl, gbpb; uint64_t base; unsigned th, td;
                    nvn_deswizzle_geom(&d, level, layer, &rb, &rows, &sl, &base, &th, &td, &gbpb);
                    uint8_t *mine = calloc(1, L.size);
                    for (uint32_t r = 0; r < rh; r++) for (uint32_t x = 0; x < rw * (uint32_t)bpb; x++)
                        mine[base + nvn_bl_offset(rx * (uint32_t)bpb + x, ry + r, rz, rb, rows, th, td)] = 1;
                    for (uint64_t i = 0; i < L.size; i++) if (!mine[i] && before[i] != b[i]) bad_out++;
                    free(mine);
                }
                free(before); free(src);
            }
            free(full); free(part);
        }
    printf("%-44s rows %s, region %s%s\n", name, bad_rows ? "WRONG" : "ok", bad_reg || bad_out ? "WRONG" : "ok", unsup ? " (some unsupported)" : "");
    CHECK(!bad_rows); CHECK(!bad_reg); CHECK(!bad_out); CHECK(!unsup);
    free(a); free(b);
}

// 3: texrows_mark against brute force
static void marks(const char *name, nvn_texdesc d) {
    nvn_texlayout L; nvn_tex_layout(&d, &L);
    int missed = 0, extra = 0, spans = 0;
    for (unsigned level = 0; level < d.levels; level++)
        for (uint32_t layer = 0; layer < nlayers(&d); layer++) {
            uint32_t rb, rows, sl, gbpb; uint64_t base; unsigned th, td;
            if (nvn_deswizzle_geom(&d, level, layer, &rb, &rows, &sl, &base, &th, &td, &gbpb)) continue;
            texrows_geom g = texrows_make(base, rb, rows, sl, th, td);
            uint32_t ngr = texrows_gob_rows(&g);
            for (int k = 0; k < 40; k++) {
                uint64_t o0 = rnd() % L.size, o1 = o0 + 1 + rnd() % (k < 20 ? 64 : 9000);
                if (o1 > L.size) o1 = L.size;
                uint8_t *m = calloc(ngr + 1, 1), *want = calloc(ngr + 1, 1);
                texrows_mark(&g, o0, o1, m);
                if (neg && ngr > 0) for (uint32_t i = 0; i < ngr; i++) if (m[i]) { m[i] = 0; break; }
                for (uint32_t z = 0; z < sl; z++) for (uint32_t y = 0; y < rows; y++) for (uint32_t x = 0; x < rb; x++) {
                    uint64_t o = base + nvn_bl_offset(x, y, z, rb, rows, th, td);
                    if (o >= o0 && o < o1) want[y / 8] = 1;
                }
                for (uint32_t i = 0; i < ngr; i++) { missed += want[i] && !m[i]; extra += m[i] && !want[i] && rb % 64 == 0 && rows % 8 == 0; }
                spans++;
                free(m); free(want);
            }
        }
    printf("%-44s %d spans: %d GOB rows missed, %d extra\n", name, spans, missed, extra);
    CHECK(missed == 0); CHECK(extra == 0);
}

typedef struct { uint64_t lo[256], hi[256]; int n; } runs_t;
static void run(void *ctx, size_t off, size_t n) { runs_t *r = ctx; if (r->n < 256) { r->lo[r->n] = off; r->hi[r->n] = off + n; r->n++; } }
// 4: the upload model (tex_upload_rows' plan on the texture's storage in the watched memory)
static void upload_model(const char *name, nvn_texdesc d, int nwrites, double max_frac) {
    nvn_texlayout L; nvn_tex_layout(&d, &L);
    if (L.size > (uint64_t)NP * 4096) { printf("FAIL %s: too big for the mock\n", name); fails++; return; }
    uint8_t *st = g_mem;
    for (uint64_t i = 0; i < L.size; i++) st[i] = (uint8_t)rnd();
    // the uploaded copy: every level / layer deswizzled
    uint32_t nl = d.levels, ny = nlayers(&d);
    uint8_t **img = calloc((size_t)nl * ny, sizeof *img);
    for (uint32_t layer = 0; layer < ny; layer++) for (uint32_t level = 0; level < nl; level++) {
        lvl v = lv(&d, level);
        img[layer * nl + level] = malloc((size_t)v.pitch * v.by);
        nvn_deswizzle(&d, level, layer, st, img[layer * nl + level], v.pitch);
    }
    memset(g_bit, 0, sizeof g_bit);
    nvn_dirty_epoch_begin();
    uint64_t stamp = nvn_dirty_now();
    // region writes (as 0311 does them): pages whose bytes changed are flagged written
    uint8_t *old = malloc(L.size);
    int bw, bh, bpb = nvn_fmt_bpb(d.format, &bw, &bh);
    for (int k = 0; k < nwrites; k++) {
        uint32_t level = k % 3 == 2 ? 1 + rnd() % (nl - 1) : 0, layer = rnd() % ny;
        lvl v = lv(&d, level);
        uint32_t rw = 1 + rnd() % (v.bx < 4 ? v.bx : 4), rh = 1 + rnd() % (v.by < 4 ? v.by : 4);
        uint32_t rx = rnd() % (v.bx - rw + 1), ry = rnd() % (v.by - rh + 1);
        uint8_t src[4 * 4 * 16];
        for (size_t i = 0; i < sizeof src; i++) src[i] = (uint8_t)rnd();
        memcpy(old, st, L.size);
        CHECK(nvn_swizzle_region(&d, level, layer, rx * (uint32_t)bpb, ry, 0, rw * (uint32_t)bpb, rh, src, (size_t)rw * bpb, st) == 0);
        for (uint64_t p = 0; p * 4096 < L.size; p++) if (memcmp(old + p * 4096, st + p * 4096, L.size - p * 4096 < 4096 ? L.size - p * 4096 : 4096)) g_bit[p] = 1;
    }
    free(old);
    // the plan
    runs_t rr = {.n = 0};
    uint64_t now = 0;
    int r = nvn_dirty_runs_fresh(g_mem, L.size, stamp, 0, run, &rr, &now);
    CHECK(r >= 0 && rr.n > 0 && now > stamp);
    if (neg && rr.n) rr.n--;
    uint64_t bytes = 0;
    for (uint32_t layer = 0; layer < ny; layer++) for (uint32_t level = 0; level < nl; level++) {
        uint32_t rb, rows, sl, gbpb; uint64_t base; unsigned th, td;
        nvn_deswizzle_geom(&d, level, 0, &rb, &rows, &sl, &base, &th, &td, &gbpb);
        texrows_geom g = texrows_make(base + (uint64_t)layer * L.layer_size, rb, rows, sl, th, td);
        uint32_t ngr = texrows_gob_rows(&g);
        uint8_t *m = calloc(ngr + 1, 1);
        for (int k = 0; k < rr.n; k++) texrows_mark(&g, rr.lo[k], rr.hi[k], m);
        lvl v = lv(&d, level);
        for (uint32_t a = 0; a < ngr;) {
            if (!m[a]) { a++; continue; }
            uint32_t b = a; while (b < ngr && m[b]) b++;
            uint32_t y0 = a * 8, y1 = b * 8 < rows ? b * 8 : rows;
            CHECK(nvn_deswizzle_rows(&d, level, layer, st, img[layer * nl + level] + (size_t)y0 * v.pitch, v.pitch, y0, y1) == 0);
            bytes += (uint64_t)(y1 - y0) * v.pitch;
            a = b;
        }
        free(m);
    }
    int same = 1;
    for (uint32_t layer = 0; layer < ny; layer++) for (uint32_t level = 0; level < nl; level++) {
        lvl v = lv(&d, level);
        uint8_t *want = malloc((size_t)v.pitch * v.by);
        nvn_deswizzle(&d, level, layer, st, want, v.pitch);
        if (memcmp(want, img[layer * nl + level], (size_t)v.pitch * v.by)) same = 0;
        free(want); free(img[layer * nl + level]);
    }
    free(img);
    printf("%-44s %d writes: %d runs, %llu KiB uploaded of %llu KiB (%.1f%%), texture %s\n", name, nwrites, rr.n,
           (unsigned long long)(bytes >> 10), (unsigned long long)(L.size >> 10), 100.0 * bytes / L.size, same ? "right" : "WRONG");
    CHECK(same);
    CHECK(bytes * 1.0 <= max_frac * L.size);
}
static void count_run(void *ctx, size_t off, size_t n) { (void)off; (void)n; ++*(int *)ctx; }

int main(void) {
    neg = getenv("TOTK_PROBE_NEGATIVE") && getenv("TOTK_PROBE_NEGATIVE")[0] == '1';
    g_mem = aligned_alloc(4096, (size_t)NP * 4096);
    memset(g_mem, 0, (size_t)NP * 4096);
    nvn_dirty_add_range(g_mem, (size_t)NP * 4096);

    rows_and_region("BC4 640x640 7 levels", tex(1, 0x49, 640, 640, 1, 7), 12);
    rows_and_region("RGBA8 300x200 4 levels", tex(1, 0x25, 300, 200, 1, 4), 12);
    rows_and_region("R8 17x9", tex(1, 0x01, 17, 9, 1, 1), 8);
    rows_and_region("BC1 260x132 5 levels", tex(1, 0x42, 260, 132, 1, 5), 12);
    rows_and_region("2D array RG8 64x96 x3, 3 levels", tex(4, 0x0D, 64, 96, 3, 3), 8);
    rows_and_region("3D RGBA8 32x32x16, 2 levels", tex(2, 0x25, 32, 32, 16, 2), 8);
    rows_and_region("RGBA32F 40x70 2 levels", tex(1, 0x2E, 40, 70, 1, 2), 8);
    { nvn_texdesc p = tex(1, 0x0D, 260, 260, 1, 9); p.pkg_block_h = 1; rows_and_region("packaged RG8 260x260, 1 GOB", p, 10); }
    { nvn_texdesc l = tex(1, 0x25, 100, 50, 1, 1); l.flags = 0x40; rows_and_region("linear render target 100x50 RGBA8", l, 8); }

    marks("BC4 640x640 7 levels", tex(1, 0x49, 640, 640, 1, 7));
    marks("RGBA8 300x200 4 levels", tex(1, 0x25, 300, 200, 1, 4));
    marks("R8 17x9", tex(1, 0x01, 17, 9, 1, 1));
    marks("2D array RG8 64x96 x3", tex(4, 0x0D, 64, 96, 3, 3));
    { nvn_texdesc p = tex(1, 0x0D, 260, 260, 1, 9); p.pkg_block_h = 1; marks("packaged RG8 260x260, 1 GOB", p); }

    // the field-log texture: 5120x5120 BC4 (format 0x49), 7 levels, 16.67 MiB; small patches, ~2-5 per frame
    upload_model("tex 86: BC4 5120x5120, 7 levels", tex(1, 0x49, 5120, 5120, 1, 7), 4, 0.15);
    upload_model("tex 86, a busier frame", tex(1, 0x49, 5120, 5120, 1, 7), 12, 0.35);
    upload_model("RGBA8 1024x1024 2D array x4, 4 levels", tex(4, 0x25, 1024, 1024, 4, 4), 6, 0.35);

    // 5: a write after this epoch's scan
    {
        memset(g_bit, 0, sizeof g_bit);
        nvn_dirty_epoch_begin();
        uint64_t stamp = nvn_dirty_now();
        int n = 0;
        nvn_dirty_runs(g_mem, 65536, stamp, 0, count_run, &n);   // the epoch's scan: nothing yet
        CHECK(n == 0);
        g_mem[5 * 4096] ^= 1; g_bit[5] = 1;                      // the game writes after it
        n = 0; nvn_dirty_runs(g_mem, 65536, stamp, 0, count_run, &n);
        int plain = n;
        n = 0; uint64_t now = 0;
        if (neg) nvn_dirty_runs(g_mem, 65536, stamp, 0, count_run, &n);
        else nvn_dirty_runs_fresh(g_mem, 65536, stamp, 0, count_run, &n, &now);
        printf("a write after the epoch's scan: plain runs %d, fresh runs %d\n", plain, n);
        CHECK(n == 1);
        if (!neg) {   // and the stamp it gives is newer than that write: no run again from it
            int m = 0; uint64_t now2 = 0;
            nvn_dirty_runs_fresh(g_mem, 65536, now, 0, count_run, &m, &now2);
            CHECK(m == 0);
        }
    }

    // 6: the pick and the queue size. Field log: a 13x13 texel patch of level 0 (4x4 BC4 blocks of 8 bytes)
    {
        uint64_t lb = 1280ull * 1280 * 8;   // level 0: 12.5 MiB
        uint64_t wb = 4 * 8, by = 4;
        CHECK(wt_region_pick(1, 1, wb * by, lb));
        CHECK(!wt_region_pick(0, 1, wb * by, lb));
        CHECK(!wt_region_pick(1, 0, wb * by, lb));
        CHECK(!wt_region_pick(1, 1, lb, lb));              // the whole level: as before
        CHECK(wt_region_pick(1, 1, lb / 2, lb) && !wt_region_pick(1, 1, lb / 2 + 1, lb));
        uint64_t q = wt_queue_bytes(neg ? 0 : wt_region_pick(1, 1, wb * by, lb), wb, by, 1, lb);
        CHECK(q == wb * by);
        CHECK(wtq_pick(1, 1, 0, q));                       // queued now (UIT 0256)
        CHECK(!wtq_pick(1, 1, 0, wt_queue_bytes(0, wb, by, 1, lb)));   // before: over the limit, waited for
        CHECK(wt_queue_bytes(1, 8, 2, 3, 99) == 48 && wt_queue_bytes(0, 8, 2, 3, 99) == 99);
    }
    printf(fails ? "FAIL (%d)\n" : "ok\n", fails);
    return fails != 0;
}
