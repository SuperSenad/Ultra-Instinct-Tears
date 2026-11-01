// [UIT 0331, reworked in UIT 0333] The size of a storage buffer the shader reaches through a pointer in a uniform
// buffer other than c0: {address, size word} as the translator reads it.
// UIT 0331 took a word above 64 MiB for "not a size" and bound 1 MiB. In the Depths that word is a real size: c6 points
// at the ground's vertex data (about 258 MiB: 270560515 bytes, the ground draws use base vertices up to ~4.19 million),
// and the vertex shaders read far past 1 MiB. Reads past the bound range return 0, so the ground lost its vertices
// (see-through or floating ground, missing pieces), only in the Depths.
// Now: a word that fits in the pool is kept, whatever its size (as before UIT 0331). A word past the end of the pool:
// what the pool holds (before UIT 0331 an empty buffer, every read 0). A 0 word: the rest of the pool, at most
// `fallback` bytes (before UIT 0331 also an empty buffer). The result is never above `limit` (the device's largest
// storage buffer range; 0: none).
#ifndef VKB_SBSIZE_H
#define VKB_SBSIZE_H
#include <stdint.h>
#define VKB_SBPTR_FALLBACK (1ull << 20)
// word: the size word as read; pool_left: bytes from the address to the end of its memory pool (0: not in a pool).
// Returns the size to bind; *fixed is set to 1 when the word was not used as it is.
static inline uint64_t vkb_sbptr_size(uint64_t word, uint64_t pool_left, uint64_t fallback, uint64_t limit, int *fixed) {
    uint64_t s = word;
    *fixed = 0;
    if (pool_left) {   // not game memory we know: unchanged (nothing to bind it to either way)
        if (!word) { s = pool_left < fallback ? pool_left : fallback; *fixed = 1; }
        else if (word > pool_left) { s = pool_left; *fixed = 1; }
    }
    if (limit && s > limit) { s = limit; *fixed = 1; }
    return s;
}
#endif
