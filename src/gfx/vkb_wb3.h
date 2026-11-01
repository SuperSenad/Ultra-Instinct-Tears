// [UIT 0035] TOTK_VK_WB_MERGE3 (on by default since UIT 0139; =0 off): a three-way merge for pages both the CPU and the GPU
// wrote in one submission. Before (and with this off), such a page is left to the CPU's writes whole and the GPU's
// bytes in it are lost (the per-frame "GPU bytes not copied back there"). With a snapshot of the page as it was when
// the GPU's copy was made (base), each 4-byte word is decided on its own:
//   - the CPU changed it (memory != base): the CPU's (its writes are newer: the game writes a buffer for its next use
//     after the GPU's);
//   - else the GPU changed it (gpu != base): the GPU's;
//   - else nothing to do.
// The one case this gets wrong: the CPU rewrote a word with the value it already had, after the GPU wrote it: memory
// still equals base and the GPU's older value goes in. [UIT 0139] On by default anyway: without it every GPU byte of
// such a page is lost (600-1,400 a frame in the 2026-10-31 field runs), which is the bigger error.
// Pure logic: tools/tests/test_wb3.c.
#ifndef VKB_WB3_H
#define VKB_WB3_H
#include <stdint.h>
#include <stddef.h>
#include <string.h>
// For each run of words to take from the GPU: put(ctx, offset, bytes) (the caller writes gpu+offset there). n: bytes
// (a multiple of 4 or not: the tail is one short word). Returns the bytes taken from the GPU.
static inline size_t wb3_merge(const uint8_t *mem, const uint8_t *gpu, const uint8_t *base, size_t n,
                               void (*put)(void *ctx, size_t off, size_t len), void *ctx) {
    size_t taken = 0, run = 0, at = 0;
    for (size_t o = 0; o < n; o += 4) {
        size_t w = n - o < 4 ? n - o : 4;
        int take = !memcmp(mem + o, base + o, w) && memcmp(gpu + o, base + o, w);
        if (take) { if (!run) at = o; run += w; }
        else if (run) { put(ctx, at, run); taken += run; run = 0; }
    }
    if (run) { put(ctx, at, run); taken += run; }
    return taken;
}
#endif
