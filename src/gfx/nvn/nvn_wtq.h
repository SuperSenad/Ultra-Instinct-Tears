// [UIT 0256] TOTK_VK_WRITETEXELS_QUEUED (default on; =0 as before): small WriteTexels are queued to the translation
// thread in order instead of waiting for the whole queue first (hle_nvn.c write_texels). Pure parts, for the tests.
#pragma once
#include <stddef.h>
#include <stdint.h>
enum { WTQ_LEVEL_MAX = 256 * 1024 };   // the level's bytes (the copy de-lays out the whole level / layer on the translation thread)
// Queued (1) or the old drain + write on the calling thread (0): on, the replay asynchronous, not the translation thread
// itself (it runs everything in order already), and a level small enough for the translation thread to rewrite.
static inline int wtq_pick(int on, int async, int is_x, uint64_t level_bytes) {
    return on && async && !is_x && level_bytes && level_bytes <= WTQ_LEVEL_MAX;
}
// Bytes of the caller's data the write reads: rows of wb bytes, by rows a slice (row apart), nz slices (img apart).
static inline size_t wtq_src_bytes(size_t wb, uint32_t by, uint32_t nz, size_t row, size_t img) {
    if (!wb || !by || !nz) return 0;
    return (size_t)(nz - 1) * img + (size_t)(by - 1) * row + wb;
}
