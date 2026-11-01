// [UIT 0034] The shader binary cache's file format (TOTK_SHADER_BINARY_CACHE, vkb_shader.c): what the driver made from a
// shader's SPIR-V (vkGetShaderBinaryDataEXT), kept in <shader cache>/bin/ so later runs skip the SPIR-V compile.
// Everything this machine makes itself from the player's own translated shaders: nothing is shipped or downloaded.
// A file is used only when everything that decides the binary matches:
//   - the key (the file name and in the header): a 128-bit hash of the final SPIR-V, its size, the stage, the next stage
//     and the descriptor set layout's fingerprint (its bindings and flags: a change of layout in a later version, or
//     descriptor buffers on/off, makes new files);
//   - the driver: shaderBinaryUUID and shaderBinaryVersion (VK_EXT_shader_object; a driver update changes them);
//   - the file's own size and a hash of the binary (a truncated or damaged file is not used).
// Otherwise, or when the driver refuses the binary (VK_INCOMPATIBLE_SHADER_BINARY_EXT), the SPIR-V is compiled as before
// and the file is written again. Pure logic: tools/tests/test_shbin.c.
#ifndef VKB_SHBIN_H
#define VKB_SHBIN_H
#include <stdint.h>
#include <stddef.h>
#include <string.h>

// [UIT 0136] "UIB2": binaries written before 0136 (keys without the device fingerprint) fail the check and are made
// again from SPIR-V, whatever TOTK_SHADER_BINARY_DEVKEY says.
// [UIT 0138] "UIB3": the files of 0136 / 0137 ("UIB2") are never loaded again. The 2026-10-31 09:30 run wrote 8703 of
// them; the next two runs loaded 9317 shaders from them and both lost the device at the same draw, while the run with
// the cache off (and every run with shaders compiled from SPIR-V) went into Hyrule Field. See vkb_shader.c bin_init.
#define SHBIN_MAGIC 0x33424955u   // "UIB3" (0136-0137: "UIB2"; 0135 and before: "UIB1")
typedef struct { uint64_t a, b; } shbin_key;
typedef struct {
    uint32_t magic, hdr_size;
    uint8_t uuid[16];
    uint32_t drv_version, stage;
    shbin_key key;
    uint64_t data_size, data_hash;
} shbin_hdr;

static inline uint64_t shbin_fnv(const void *p, size_t n, uint64_t h) {
    const uint8_t *b = (const uint8_t *)p;
    for (size_t i = 0; i < n; i++) { h ^= b[i]; h *= 0x100000001b3ull; }
    return h;
}
// 64-bit mix (splitmix64's finaliser), so the two halves of the key are independent hashes
static inline uint64_t shbin_mix(uint64_t x) {
    x ^= x >> 30; x *= 0xbf58476d1ce4e5b9ull; x ^= x >> 27; x *= 0x94d049bb133111ebull; return x ^ (x >> 31);
}
static inline shbin_key shbin_make_key(const void *spv, size_t bytes, uint32_t stage, uint32_t next, uint64_t layout_fp) {
    uint64_t meta[4] = {bytes, stage, next, layout_fp};
    uint64_t a = shbin_fnv(meta, sizeof meta, shbin_fnv(spv, bytes, 0xcbf29ce484222325ull));
    // the second half: FNV from another start, over the words in reverse order, then mixed
    uint64_t b = 0x84222325cbf29ce4ull ^ shbin_mix(bytes);
    const uint8_t *p = (const uint8_t *)spv;
    for (size_t i = bytes; i >= 4; i -= 4) b = shbin_mix(b ^ ((uint64_t)p[i - 4] | (uint64_t)p[i - 3] << 8 | (uint64_t)p[i - 2] << 16 | (uint64_t)p[i - 1] << 24));
    for (size_t i = 0; i < (bytes & 3); i++) b = shbin_mix(b ^ p[i]);
    b = shbin_mix(b ^ shbin_fnv(meta, sizeof meta, 0x9e3779b97f4a7c15ull));
    return (shbin_key){a, b};
}
static inline void shbin_fill(shbin_hdr *h, const uint8_t uuid[16], uint32_t drv, uint32_t stage, shbin_key k,
                              const void *data, size_t n) {
    memset(h, 0, sizeof *h);
    h->magic = SHBIN_MAGIC; h->hdr_size = sizeof *h;
    memcpy(h->uuid, uuid, 16);
    h->drv_version = drv; h->stage = stage; h->key = k;
    h->data_size = n; h->data_hash = shbin_fnv(data, n, 0xcbf29ce484222325ull);
}
// The binary in a file read whole (f, n), or NULL when it may not be used.
static inline const void *shbin_check(const void *f, size_t n, const uint8_t uuid[16], uint32_t drv, uint32_t stage,
                                      shbin_key k, size_t *data_size) {
    shbin_hdr h;
    if (!f || n < sizeof h) return NULL;
    memcpy(&h, f, sizeof h);
    if (h.magic != SHBIN_MAGIC || h.hdr_size != sizeof h || memcmp(h.uuid, uuid, 16) || h.drv_version != drv ||
        h.stage != stage || h.key.a != k.a || h.key.b != k.b || !h.data_size || h.data_size != n - sizeof h)
        return NULL;
    const uint8_t *d = (const uint8_t *)f + sizeof h;
    if (shbin_fnv(d, (size_t)h.data_size, 0xcbf29ce484222325ull) != h.data_hash) return NULL;
    *data_size = (size_t)h.data_size;
    return d;
}
#endif
