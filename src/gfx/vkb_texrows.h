// [UIT 0310] TOTK_VK_TEX_DIRTY_ROWS (default on; =0 as before): a texture the game changed in a few places is uploaded
// again only in the rows of GOBs whose pages were written, not whole (vkb_tex.c tex_upload_rows). Pure parts, for the
// tests: which GOB rows of a level / layer a written byte span of the texture's storage falls in.
// [UIT 0311] TOTK_VK_WRITETEXELS_REGION (default on; =0 as before): WriteTexels writes only the region's GOBs into game
// memory (nvn_swizzle_region), not the whole level read out and laid out again; wt_queue_bytes is what the queue's
// size limit (nvn_wtq.h) then measures.
#pragma once
#include <stdint.h>

// One level / layer of a block-linear texture: where it starts in the storage, its bytes there, its GOB columns, block
// rows, slices and block height / depth (log2 GOBs), as nvn_deswizzle_geom gives them.
typedef struct { uint64_t base, bytes; uint32_t gobs_x, rows, slices; unsigned th, td; } texrows_geom;

static inline texrows_geom texrows_make(uint64_t base, uint32_t row_bytes, uint32_t rows, uint32_t slices, unsigned th, unsigned td) {
    texrows_geom g = {base, 0, (row_bytes + 63) / 64, rows, slices ? slices : 1, th, td};
    uint32_t bh = 1u << th, bd = 1u << td;
    uint64_t blocks_y = (rows + 8ull * bh - 1) / (8ull * bh), blocks_z = (g.slices + bd - 1) / bd;
    g.bytes = (uint64_t)g.gobs_x * blocks_y * blocks_z * 512ull * bh * bd;
    return g;
}
static inline uint32_t texrows_gob_rows(const texrows_geom *g) { return (g->rows + 7) / 8; }

// Marks (mark[i] = 1) the GOB rows (8 block rows each) holding a byte of [o0, o1) (offsets from the storage start).
// Every GOB is 64 bytes x 8 rows, so a written byte changes exactly the rows of its GOB (any slice: the rows are the
// same in each). 0: the span misses this level / layer.
static inline int texrows_mark(const texrows_geom *g, uint64_t o0, uint64_t o1, uint8_t *mark) {
    if (o1 <= g->base || o0 >= g->base + g->bytes || !g->gobs_x) return 0;
    uint64_t a = o0 > g->base ? o0 - g->base : 0, b = (o1 < g->base + g->bytes ? o1 : g->base + g->bytes) - g->base;
    uint32_t bh = 1u << g->th, bd = 1u << g->td, ngr = texrows_gob_rows(g);
    uint64_t per_block = (uint64_t)bh * bd, blocks_y = (g->rows + 8ull * bh - 1) / (8ull * bh);
    int any = 0;
    for (uint64_t gob = a / 512; gob * 512 < b; gob++) {
        uint64_t blk = gob / per_block, gi = gob % per_block;   // block (x, then y, then z), GOB in it (y, then z)
        uint64_t by = blk / g->gobs_x % blocks_y, gr = by * bh + gi % bh;
        if (gr < ngr) { mark[gr] = 1; any = 1; }
    }
    return any;
}

// Bytes WriteTexels lays out for a region of by block rows of wb bytes, nz layers / slices: the region itself when it
// is written straight in (region), else the level's bytes as before (level_bytes).
static inline uint64_t wt_queue_bytes(int region, uint64_t wb, uint64_t by, uint64_t nz, uint64_t level_bytes) {
    return region ? wb * by * (nz ? nz : 1) : level_bytes;
}
// Straight in (1) or the whole level read out and laid out again (0): on, a block-linear or linear layout the region
// writer takes, and a region of at most half the level (above that the level-wide pass is as cheap and faster per byte).
static inline int wt_region_pick(int on, int layout_ok, uint64_t region_bytes, uint64_t level_bytes) {
    return on && layout_ok && region_bytes && level_bytes && region_bytes * 2 <= level_bytes;
}
