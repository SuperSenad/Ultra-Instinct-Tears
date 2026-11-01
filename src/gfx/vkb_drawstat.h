// [UIT 0053] Why draws were not recorded, and which shader pairs stop (or start) being drawn: the "[draws]" lines
// every TOTK_VK_OCCL_LOG_EVERY presents (backend_vk.c occl_log). The user's video (2026-10-30): all dense grass vanishes
// in one frame, at the same instant as the lighting changes, and never returns; Test A's log shows the game issuing ~530
// fewer draws a frame from that moment on. These lines show whether the port dropped them (a reason counter rises) or
// the game stopped issuing them, and which shaders (vertex / fragment key prefixes) the vanished draws used.
// Plain C, no Vulkan: recording threads count (atomics), the translation thread reads (box test: tools/tests/test_drawstat.c).
#pragma once
#include <stdint.h>
#include <string.h>

enum {   // draws reaching vkb_draw that were not recorded, and draws recorded oddly
    DS_NO_PROGRAM,   // no program bound / a shader without translation
    DS_REFUSED,      // the driver refused a shader (asynchronous compile)
    DS_TOPOLOGY,     // a primitive with no Vulkan topology
    DS_NO_TARGETS,   // no render pass could begin
    DS_INDEX,        // the index buffer was not found in game memory
    DS_SKIP_LIST,    // TOTK_VK_SKIP (development)
    DS_COMPILING,    // TOTK_SHADER_ASYNC: a shader still compiling
    DS_UPSCALER,     // replaced by the upscaler's own pass (DLSS / FSR input draw): not lost
    DS_ZERO_COUNT,   // recorded with 0 vertices / indices (draws nothing)
    DS_ZERO_INST,    // instance count 0, recorded as 1 instance (as before)
    DS_N
};
static const char *const ds_names[DS_N] = {"no program", "shader refused", "topology", "no targets", "index buffer",
                                           "skip list", "compiling", "upscaler pass", "0 vertices", "0 instances (drawn as 1)"};

#define DS_SLOTS 4096u
#define DS_NAME 13
typedef struct {
    uint64_t key[DS_SLOTS];          // 0: free; else the pair's hash (vertex, fragment shader)
    uint64_t n[DS_SLOTS];            // draws recorded with that pair
    int ready[DS_SLOTS];             // names written
    char vs[DS_SLOTS][DS_NAME], fs[DS_SLOTS][DS_NAME];
    uint64_t full;                   // draws not counted: table full
    // reader side (one thread)
    uint64_t seen[DS_SLOTS];         // n at the last window
    double rate[DS_SLOTS];           // draws per frame in the last window
    int windows;                     // windows read so far
} ds_table;

static inline uint64_t ds_key(const void *vs, const void *fs) {
    uint64_t k = (uint64_t)(uintptr_t)vs * 0x9E3779B97F4A7C15ull ^ ((uint64_t)(uintptr_t)fs + 0x632BE59BD9B4E019ull) * 0xC2B2AE3D27D4EB4Full;
    k ^= k >> 29;
    return k ? k : 1;
}
static inline void ds_name(char *d, const char *s) {
    size_t i = 0;
    for (; s && s[i] && i < DS_NAME - 1; i++) d[i] = s[i];
    if (!s) { memcpy(d, "-", 2); return; }
    d[i] = 0;
}
// A recorded draw with this pair (any thread).
static inline void ds_hit(ds_table *t, const void *vs, const void *fs, const char *vs_key, const char *fs_key) {
    uint64_t k = ds_key(vs, fs);
    for (uint32_t i = (uint32_t)(k % DS_SLOTS), probe = 0; probe < 64; probe++, i = (i + 1) % DS_SLOTS) {
        uint64_t cur = __atomic_load_n(&t->key[i], __ATOMIC_ACQUIRE);
        if (cur == 0) {
            uint64_t zero = 0;
            if (__atomic_compare_exchange_n(&t->key[i], &zero, k, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
                ds_name(t->vs[i], vs_key);
                ds_name(t->fs[i], fs_key);
                __atomic_store_n(&t->ready[i], 1, __ATOMIC_RELEASE);
                cur = k;
            } else cur = zero;
        }
        if (cur == k) { __atomic_fetch_add(&t->n[i], 1, __ATOMIC_RELAXED); return; }
    }
    __atomic_fetch_add(&t->full, 1, __ATOMIC_RELAXED);
}

// One window of `frames` presents (reader thread): pairs that were drawn at least `min` times a frame and fell below a
// tenth of that ("gone", sign -1), and pairs not drawn in the last window and now at least `min` a frame ("new", +1).
// Calls out(arg, sign, before, now, vs, fs) for up to `limit` of each; returns how many changed in all.
typedef void (*ds_out_fn)(void *arg, int sign, double before, double now, const char *vs, const char *fs);
static inline int ds_window(ds_table *t, double frames, double min, int limit, ds_out_fn out, void *arg) {
    int changed = 0, gone = 0, born = 0;
    for (uint32_t i = 0; i < DS_SLOTS; i++) {
        if (!__atomic_load_n(&t->key[i], __ATOMIC_ACQUIRE)) continue;
        uint64_t n = __atomic_load_n(&t->n[i], __ATOMIC_RELAXED);
        double now = frames > 0 ? (double)(n - t->seen[i]) / frames : 0, before = t->rate[i];
        int ready = __atomic_load_n(&t->ready[i], __ATOMIC_ACQUIRE);
        if (t->windows > 0 && ready) {
            if (before >= min && now < before / 10) { changed++; if (gone++ < limit && out) out(arg, -1, before, now, t->vs[i], t->fs[i]); }
            else if (before == 0 && now >= min) { changed++; if (born++ < limit && out) out(arg, 1, before, now, t->vs[i], t->fs[i]); }
        }
        t->seen[i] = n;
        t->rate[i] = now;
    }
    t->windows++;
    return changed;
}
