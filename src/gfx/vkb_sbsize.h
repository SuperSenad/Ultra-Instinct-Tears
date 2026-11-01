// [UIT 0331] The size of a storage buffer the shader reaches through a pointer in a uniform buffer other than c0.
// The translator takes {address, size} from the uniform buffer, but only the driver's c0 table really holds a size
// after the address: in the Depths, c6 holds a bare global memory address and the word after it is other data (seen:
// 0, 67108864, 270545005). Taken as a size it bound an empty buffer (0, or a range past the end of the memory pool:
// every read 0) or a range of hundreds of MiB (GPU-drawn textures inside it copied back after each drawing).
// Kept: a word that fits in the pool and is not above `cap`. Otherwise: the rest of the pool, at most `fallback` bytes
// (1 MiB is enough for what the Depths shaders read there).
#ifndef VKB_SBSIZE_H
#define VKB_SBSIZE_H
#include <stdint.h>
#define VKB_SBPTR_CAP (64ull << 20)
#define VKB_SBPTR_FALLBACK (1ull << 20)
// word: the size word as read; pool_left: bytes from the address to the end of its memory pool (0: not in a pool).
// Returns the size to bind; *fixed is set to 1 when the word was not used.
static inline uint64_t vkb_sbptr_size(uint64_t word, uint64_t pool_left, uint64_t cap, uint64_t fallback, int *fixed) {
    *fixed = 0;
    if (!pool_left) return word;   // not game memory we know: unchanged (nothing to bind it to either way)
    if (word && word <= pool_left && word <= cap) return word;
    *fixed = 1;
    return pool_left < fallback ? pool_left : fallback;
}
#endif
