// Shaders of the Vulkan NVN replayer (vkb_shader.c): translated SPIR-V as shader objects, universal descriptor layout.
#pragma once
#include <stdint.h>

#include "vkb.h"

// Universal layout: per stage VKB_STAGE_BINDINGS bindings of set 0 (vertex from 0, fragment from 64; compute: own layout).
enum {
    VKB_STAGE_BINDINGS = 64,
    VKB_B_SUPPORT = 0,                   // Ryujinx support buffer
    VKB_B_CBUF = 1, VKB_NCBUF = 17,      // c0..c16 at +1..17
    VKB_B_SBUF = 18, VKB_NSBUF = 16,     // storage buffers in the shader's order
    VKB_B_TEX = 34, VKB_NTEX = 24,       // combined textures in the shader's order
    VKB_B_IMG = 58, VKB_NIMG = 4,        // images in the shader's order
};

enum { VKB_SB_TABLE, VKB_SB_UBO, VKB_SB_UBO_PTR };
typedef struct { uint8_t slot, separate; uint16_t type; } vkb_tex_ref;   // NVN binding slot; Ryujinx SamplerType

enum { VKB_VARIANTS = 8 };
typedef struct { uint32_t intmask, sintmask; VkShaderEXT obj; } vkb_variant;

typedef struct {
    VkShaderEXT obj;
    VkShaderStageFlagBits vk_stage;
    int stage;                           // NVN: 0 vertex, 1 fragment, 5 compute
    uint32_t cbuf_mask;                  // hardware constant buffer slots read (binding +1+slot): c3..c16 = game uniform
                                         // buffer slot-3, c1 = the shader's own constants (c1 below), c0 = NVN driver bank
    const uint8_t *c1; uint32_t c1_size; // code[control+0x700 .. length): the shader's constants (a copy)
    uint64_t c1_rec; VkBuffer c1_buf; VkDeviceSize c1_off;   // its copy in the submission being recorded (vkb_draw.c)
    // storage buffers, binding +18+i. Where the address comes from (MacNativeGames' totk_xlate Service.cs):
    //  VKB_SB_TABLE: the NVN storage buffer slot (driver table in c0); VKB_SB_UBO: (compute) the uniform buffer slot, its
    //  range used as storage; VKB_SB_UBO_PTR: a 64-bit address stored in the game's uniform buffer `slot` at byte `off`;
    //  slot 0xFF: unknown (an empty buffer is bound)
    struct { uint8_t slot, kind, write, read; uint16_t off; } sbuf[VKB_NSBUF]; int nsbuf;   // write / read: stores to / loads from it
    vkb_tex_ref tex[VKB_NTEX]; int ntex; // binding +34+i
    vkb_tex_ref img[VKB_NIMG]; int nimg; // binding +58+i
    uint32_t frag_outputs;               // colour outputs written (bit per target)
    int stores;                          // writes storage buffers or images (later work must wait for it)
    int unsupported;                     // a resource outside the layout: draws with it are skipped
    const char *key;                     // SHA-256 key of the translation (traces)
    uint32_t *spv; size_t nspv_words;    // vertex shaders: the module as loaded (bindings rewritten), for variants
    uint32_t in_mask, out_mask;          // input / output locations
    vkb_variant variants[VKB_VARIANTS]; int nvariants;   // integer-attribute variants made so far
    // [UIT 0039] TOTK_SHADER_ASYNC: the driver compile of a new shader runs in the background. Until it is done obj is
    // NULL and pending is 1 (queued) or 2 (being compiled); everything else above is already filled in. Read with
    // vkb_shader_pending (acquire): obj is valid once it returns 0.
    int pending;
    uint64_t async_since;                // the frame the first draw with it was skipped (0: none yet)
} vkb_shader;

const vkb_shader *vkb_shader_get(const NVNshaderData *sd);   // NULL: no translation (logged)
// [UIT 0039] Asynchronous shader compilation (TOTK_SHADER_ASYNC=1, off by default): vkb_shader_get may return a shader
// whose driver compile is still running (vkb_shader_pending). A draw that may be skipped counts it with
// vkb_shader_async_skipped; one that may not waits with vkb_shader_wait (compiling it on this thread if no background
// thread has started it). Compute shaders are never deferred.
int vkb_shader_async_on(void);
static inline int vkb_shader_pending(const vkb_shader *s) { return s && __atomic_load_n(&s->pending, __ATOMIC_ACQUIRE) != 0; }
void vkb_shader_wait(const vkb_shader *s);
void vkb_shader_async_skipped(void);
// [UIT native] TOTK_SHADER_EARLY (vkb_shader.c): start the compile of a program's shaders when the game hands them over
// (nvnProgramSetShaders); a draw that comes first waits for it. No-op when off.
int vkb_shader_early_on(void);
void vkb_shader_early(const NVNshaderData *sd, int n);
// The vertex shader object for integer attributes at intmask (signed: sintmask): see vkb_spv.c.
VkShaderEXT vkb_shader_variant(const vkb_shader *vs, uint32_t intmask, uint32_t sintmask);
VkDescriptorSetLayout vkb_set_layout(int compute);
VkPipelineLayout vkb_pipeline_layout(int compute);
void vkb_shader_stats(uint64_t *loaded, uint64_t *missing);
