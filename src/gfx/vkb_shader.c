// Shaders of the NVN replayer (milestone 3.3 step 4): the game's Maxwell shaders, translated to SPIR-V by Ryujinx's
// translator (MacNativeGames' totk_xlate), become Vulkan shader objects (VK_EXT_shader_object): no pipelines to build
// at draw time, every other state is dynamic.
//  - A shader is identified as MacNativeGames does: SHA-256 of its control section (0x880 bytes) and its code (length
//    at control+0x704); the stage is at control+0x714 (0 vertex, 1 fragment, 5 compute).
//  - [temporary] The translation is the default variant from the translator comparison (work/shaders/v143/ryujinx: every
//    texture 2D-typed by its own reflection, attributes and outputs float). The runtime translation service with
//    variants per bound state (MacNativeGames' totk_shader_rt) replaces it before the open world.
//  - Descriptors: one universal layout. Each stage owns 64 bindings of set 0 (vertex 0-63, fragment 64-127; compute its
//    own layout): +0 Ryujinx support buffer, +1..17 constant buffers c0-c16, +18..33 storage buffers, +34..57 combined
//    textures, +58..61 images. The translator numbers bindings densely per shader; they are rewritten in the SPIR-V
//    when it is loaded, so any vertex shader combines with any fragment shader under one pipeline layout.
#include <windows.h>
#include <bcrypt.h>   // after windows.h (its types)
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "compat/gfx_compat.h"
#include "host/hle.h"
#include "vkb.h"
#include "vkb_shader.h"
#include "vkb_xnotes.h"
#include "vkb_spv.h"
#include "vkb_shbin.h"
#include "vkb_shasync.h"
#include "vkb_shqueue.h"

#define VLOG(...) LOG("[vk] " __VA_ARGS__)

static VkDescriptorSetLayout g_layout[2];   // 0 graphics, 1 compute
static VkPipelineLayout g_playout[2];
static uint64_t g_layout_fp[2];   // [UIT 0034] the layouts' fingerprint: part of the shader binary cache's key

static void make_layouts(void) {
    for (int compute = 0; compute < 2; compute++) {
        VkDescriptorSetLayoutBinding b[128];
        VkDescriptorBindingFlags fl[128];
        uint32_t n = 0;
        for (int st = 0; st < (compute ? 1 : 2); st++) {
            VkShaderStageFlags sf = compute ? VK_SHADER_STAGE_COMPUTE_BIT : st ? VK_SHADER_STAGE_FRAGMENT_BIT : VK_SHADER_STAGE_VERTEX_BIT;
            uint32_t base = (uint32_t)st * VKB_STAGE_BINDINGS;
            struct { uint32_t off, count; VkDescriptorType type; } r[] = {
                {VKB_B_SUPPORT, 1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER}, {VKB_B_CBUF, VKB_NCBUF, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER},
                {VKB_B_SBUF, VKB_NSBUF, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER}, {VKB_B_TEX, VKB_NTEX, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER},
                {VKB_B_IMG, VKB_NIMG, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE}};
            for (unsigned k = 0; k < sizeof r / sizeof *r; k++)
                for (uint32_t i = 0; i < r[k].count; i++) {
                    b[n] = (VkDescriptorSetLayoutBinding){base + r[k].off + i, r[k].type, 1, sf, NULL};
                    fl[n++] = VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT;   // only what the shaders use is written
                }
        }
        g_layout_fp[compute] = shbin_fnv(b, n * sizeof *b, shbin_fnv(fl, n * sizeof *fl, (uint64_t)vkb_db_on() + 1));   // [UIT 0034] (pointers in b are NULL)
        VkDescriptorSetLayoutBindingFlagsCreateInfo fi = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO,
                                                          .bindingCount = n, .pBindingFlags = fl};
        VkDescriptorSetLayoutCreateInfo ci = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, &fi, .bindingCount = n, .pBindings = b};
        if (vkb_db_on()) { ci.pNext = NULL; ci.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_DESCRIPTOR_BUFFER_BIT_EXT; }   // vkb_desc.c
        if (!vk_check(vk.vkCreateDescriptorSetLayout(vkc.device, &ci, NULL, &g_layout[compute]), "descriptor set layout")) die("layout");
        VkPipelineLayoutCreateInfo pi = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &g_layout[compute]};
        if (!vk_check(vk.vkCreatePipelineLayout(vkc.device, &pi, NULL, &g_playout[compute]), "pipeline layout")) die("layout");
    }
    if (vkb_db_on()) vkb_db_layouts(g_layout);
}
// Made once, under pthread_once: the shader warm-up threads (create_object) and the recording threads ask for the layouts
// at the same moment at the start of a run; unguarded, a thread could see the graphics layout made and the compute one
// not yet (a compute shader object created with a null set layout), or two threads made and overwrote them.
static pthread_once_t g_layout_once = PTHREAD_ONCE_INIT;
VkDescriptorSetLayout vkb_set_layout(int compute) { pthread_once(&g_layout_once, make_layouts); return g_layout[compute]; }
VkPipelineLayout vkb_pipeline_layout(int compute) { pthread_once(&g_layout_once, make_layouts); return g_playout[compute]; }

// ---------------- the reflection (<key>.res.json): a small reader for its fixed shape ----------------
static const char *json_array(const char *j, const char *name) {   // just after the '[' of "name": [
    char pat[64];
    snprintf(pat, sizeof pat, "\"%s\"", name);
    const char *p = strstr(j, pat);
    if (!p) return NULL;
    p = strchr(p + strlen(pat), '[');
    return p ? p + 1 : NULL;
}
// next {...} object of an array: its bounds; NULL at the closing ']'
static const char *json_next(const char *p, const char **end) {
    while (*p && *p != '{' && *p != ']') p++;
    if (*p != '{') return NULL;
    int depth = 0;
    for (const char *q = p; *q; q++) {
        if (*q == '{') depth++;
        else if (*q == '}' && !--depth) { *end = q + 1; return p; }
    }
    return NULL;
}
static long json_int(const char *obj, const char *end, const char *key, long dflt) {
    char pat[64];
    snprintf(pat, sizeof pat, "\"%s\":", key);
    const char *p = strstr(obj, pat);
    if (!p || p >= end) return dflt;
    p += strlen(pat);
    while (*p == ' ') p++;
    if (!strncmp(p, "true", 4)) return 1;
    if (!strncmp(p, "false", 5)) return 0;
    return strtol(p, NULL, 10);
}
static long json_top_int(const char *j, const char *key, long dflt) { return json_int(j, j + strlen(j), key, dflt); }

static char *read_file(const char *path, size_t *n) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *b = malloc((size_t)len + 1);
    if (b && fread(b, 1, (size_t)len, f) != (size_t)len) { free(b); b = NULL; }
    if (b) b[len] = 0;
    fclose(f);
    if (n) *n = (size_t)len;
    return b;
}

// ---------------- shaders by key ----------------
typedef struct shentry { char key[65]; int c1_set, prewarmed, early; vkb_shader sh; struct shentry *next; } shentry;   // prewarmed: made ahead (prewarm_thread)
static shentry *g_tab[4096];
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static uint64_t g_loaded, g_missing;

static BCRYPT_ALG_HANDLE g_sha;   // opened once (opening it per hash cost more than the hash)
static void sha_open(void) { if (BCryptOpenAlgorithmProvider(&g_sha, BCRYPT_SHA256_ALGORITHM, NULL, 0)) die("SHA-256 provider"); }
static void sha256_hex(const void *a, size_t na, const void *b, size_t nb, char out[65]) {
    static pthread_once_t once = PTHREAD_ONCE_INIT;
    pthread_once(&once, sha_open);
    BCRYPT_ALG_HANDLE alg = g_sha;
    BCRYPT_HASH_HANDLE h;
    uint8_t md[32];
    if (BCryptCreateHash(alg, &h, NULL, 0, NULL, 0, 0)) die("SHA-256 provider");
    BCryptHashData(h, (PUCHAR)a, (ULONG)na, 0);
    BCryptHashData(h, (PUCHAR)b, (ULONG)nb, 0);
    BCryptFinishHash(h, md, sizeof md, 0);
    BCryptDestroyHash(h);
    static const char hex[] = "0123456789abcdef";
    for (int i = 0; i < 32; i++) { out[2 * i] = hex[md[i] >> 4]; out[2 * i + 1] = hex[md[i] & 15]; }
    out[64] = 0;
}

static const char *cache_dir(void) {
    static const char *d;
    if (!d) { d = getenv("TOTK_SHADER_DIR"); if (!d) d = "data/shaders/ryujinx"; }
    return d;
}

// The translator declares gl_PerVertex with ClipDistance[8] and CullDistance[1]: 9 > maxCombinedClipAndCullDistances (8)
// on NVIDIA, though no shader writes the cull distance. The CullDistance member (the block's last) is dropped: its
// OpTypeStruct operand, OpMemberName and OpMemberDecorate. Returns the new word count.
static size_t drop_cull_distance(uint32_t *w, size_t n) {
    uint32_t st = 0, mem = 0;
    for (size_t i = 5; i < n;) {
        uint32_t op = w[i] & 0xffff, wc = w[i] >> 16;
        if (!wc || i + wc > n) return n;
        if (op == 72 && wc == 5 && w[i + 3] == 11 && w[i + 4] == 4) { st = w[i + 1]; mem = w[i + 2]; }   // BuiltIn CullDistance
        i += wc;
    }
    if (!st) return n;
    size_t o = 5;
    for (size_t i = 5; i < n;) {
        uint32_t op = w[i] & 0xffff, wc = w[i] >> 16;
        int drop = (op == 72 || op == 6) && wc >= 3 && w[i + 1] == st && w[i + 2] == mem;   // OpMemberDecorate / OpMemberName
        if (op == 30 && w[i + 1] == st && wc - 2 == mem + 1) {   // OpTypeStruct: the member is the last operand
            memmove(&w[o], &w[i], (wc - 1) * 4);
            w[o] = ((wc - 1) << 16) | 30;
            o += wc - 1;
        } else if (!drop) {
            memmove(&w[o], &w[i], wc * 4);
            o += wc;
        }
        i += wc;
    }
    return o;
}

// [UIT 0034] TOTK_SHADER_BINARY_CACHE (off by default again since UIT 0138; =1 on): the driver's binary of each shader
// object is kept in <shader cache>/bin (vkb_shbin.h) and used instead of the SPIR-V in later runs. NVIDIA's own disk
// cache already makes SPIR-V compiles cheap on a second run; the log line "shader binary cache:" says how much time it saved.
// [UIT 0138] Why off: 2026-10-31, RTX 3080 Ti, driver 0x9a4a8000, same build and settings in all four runs. 09:30:
// 8704 shaders compiled from SPIR-V and 8703 binaries written: fine. 09:38 and 09:39: 9317 shaders from those binaries,
// 0 compiled, 0 refused, and both lost the device at the same draw of the same shader pair (VS 34f891cd... FS
// 04d180c1..., present 925-926: the checkpoint after its vertex stage never reached the bottom of the pipe). 09:41
// with the cache off: into the field. Checked on this side and not the cause: the key holds the final SPIR-V, stage,
// next stage, the set layout's fingerprint and the device fingerprint; shaders are made one by one, unlinked, flags 0,
// no specialisation or push constants, one set layout, the same create info for SPIR-V and binary; binaries are read
// back with vkGetShaderBinaryDataEXT after a successful create, 16-byte aligned, the file's hash checked on load,
// written atomically; 128-bit keys (no collision in 9317). So the binary the driver accepts back does not behave like
// the shader it was exported from, for at least that pair (descriptor buffers + shader objects from binaries: NVIDIA
// has fixed several bugs there in recent drivers). Nothing on this side can prove a binary behaves like its SPIR-V,
// so the cache is opt-in; with it on, a binary whose export after loading differs from the file is not used
// (bin_roundtrip below), which catches a driver that changes it on the way in, not one that runs it wrongly.
// The cost of off is small: 1.5-4 ms a shader with NVIDIA's own cache warm (the 09:30 line: 3.0 ms each).
static int g_bin_on = -1;
static uint8_t g_bin_uuid[16];
static uint32_t g_bin_ver;
static _Atomic uint64_t g_bin_mismatch;   // [UIT 0138] loaded, but the driver exported something else (bin_roundtrip)
static _Atomic uint64_t g_bin_hits, g_bin_writes, g_bin_refused, g_bin_hit_ns, g_bin_spv_ns, g_bin_spv_n;
static pthread_once_t g_bin_once = PTHREAD_ONCE_INIT;
static void bin_init(void) {
    const char *e = getenv("TOTK_SHADER_BINARY_CACHE");
    g_bin_on = 0;
    // [UIT 0138] off unless =1 (it was on unless =0 since UIT 0152): see above
    if (!(e && e[0] == '1' && !e[1])) { VLOG("shader binary cache: off (the default since UIT 0138; TOTK_SHADER_BINARY_CACHE=1 turns it on)"); return; }
    if (!vk.vkGetShaderBinaryDataEXT) { VLOG("shader binary cache: off: the driver has no vkGetShaderBinaryDataEXT"); return; }
    VkPhysicalDeviceShaderObjectPropertiesEXT so = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_OBJECT_PROPERTIES_EXT};
    VkPhysicalDeviceProperties2 p2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &so};
    vk.vkGetPhysicalDeviceProperties2(vkc.gpu, &p2);
    memcpy(g_bin_uuid, so.shaderBinaryUUID, 16);
    g_bin_ver = so.shaderBinaryVersion;
    char dir[1024];
    snprintf(dir, sizeof dir, "%s/bin", cache_dir());
    CreateDirectoryA(dir, NULL);   // exists already: fine
    g_bin_on = 1;
    VLOG("shader binary cache on (TOTK_SHADER_BINARY_CACHE=1; off by default): %s, driver binary version %u", dir, g_bin_ver);
}
static _Atomic uint64_t g_bw_queued, g_bw_full;   // the background writer (bin_queue below)
static void bin_report(void) {
    if (g_bin_on <= 0) return;
    uint64_t h = g_bin_hits, n = g_bin_spv_n;
    VLOG("shader binary cache: %llu shaders from binaries (%.2f ms each), %llu compiled from SPIR-V (%.2f ms each), "
         "%llu binaries written (%llu handed to the background writer, %llu written in place: its queue was full), "
         "%llu refused by the driver (made again; %llu of them changed by the driver on loading)",
         (unsigned long long)h, h ? g_bin_hit_ns / 1e6 / h : 0.0, (unsigned long long)n, n ? g_bin_spv_ns / 1e6 / n : 0.0,
         (unsigned long long)g_bin_writes, (unsigned long long)g_bw_queued, (unsigned long long)g_bw_full,
         (unsigned long long)g_bin_refused, (unsigned long long)g_bin_mismatch);
}
static void bin_store(const char *path, const uint8_t *buf, size_t n) {   // whole or not at all
    char tmp[1100];
    snprintf(tmp, sizeof tmp, "%s.%lu.tmp", path, (unsigned long)GetCurrentThreadId());
    FILE *f = fopen(tmp, "wb");
    if (!f) return;
    int ok = fwrite(buf, 1, n, f) == n;
    ok &= fclose(f) == 0;
    if (ok && MoveFileExA(tmp, path, MOVEFILE_REPLACE_EXISTING)) g_bin_writes++;
    else DeleteFileA(tmp);
}
// [UIT native] TOTK_SHADER_BIN_BG (on unless =0): the file is written by a background thread. The driver's binary is
// still read here (the shader object is the caller's), but the file create / write / rename (a few ms each on a busy
// disk, on the recording thread that waits for the compile) moves off the frame. A run that ends before the queue is
// written loses only those files: they are made again next time.
typedef struct { shq_job q; uint8_t *buf; char path[]; } bin_job;   // q.bytes: the file's size
static shq g_bwq = SHQ_INIT(1u << 16, 256u << 20);
static int g_bw_on = -1;
static void *bin_writer(void *unused) {
    (void)unused;
    pthread_setname_np("totk-shader-binwrite");
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_LOWEST);
    for (;;) {
        bin_job *j = (bin_job *)shq_pop(&g_bwq);
        size_t n = j->q.bytes;
        bin_store(j->path, j->buf, n);
        free(j->buf); free(j);
        shq_done(&g_bwq, n);
    }
    return NULL;
}
static void bin_writer_init(void) {
    const char *e = getenv("TOTK_SHADER_BIN_BG");
    g_bw_on = !(e && *e == '0');
    pthread_t t;
    if (g_bw_on && pthread_create(&t, NULL, bin_writer, NULL) == 0) pthread_detach(t);
    else g_bw_on = 0;
}
// 1: taken (buf is the queue's now); 0: write it here (off, or 256 MiB already waiting).
static int bin_queue(const char *path, uint8_t *buf, size_t n) {
    static pthread_once_t once = PTHREAD_ONCE_INIT;
    pthread_once(&once, bin_writer_init);
    if (g_bw_on <= 0) return 0;
    size_t pl = strlen(path) + 1;
    bin_job *j = malloc(sizeof *j + pl);
    if (!j) return 0;
    j->q.bytes = n; j->buf = buf;
    memcpy(j->path, path, pl);
    if (!shq_push(&g_bwq, &j->q)) { free(j); g_bw_full++; return 0; }
    g_bw_queued++;
    return 1;
}
static void bin_write(const char *path, VkShaderEXT obj, uint32_t stage, shbin_key k) {
    size_t n = 0;
    if (vk.vkGetShaderBinaryDataEXT(vkc.device, obj, &n, NULL) != VK_SUCCESS || !n || n > (64u << 20)) return;
    uint8_t *buf = malloc(sizeof(shbin_hdr) + n);
    if (!buf) return;
    if (vk.vkGetShaderBinaryDataEXT(vkc.device, obj, &n, buf + sizeof(shbin_hdr)) == VK_SUCCESS) {
        shbin_hdr h;
        shbin_fill(&h, g_bin_uuid, g_bin_ver, stage, k, buf + sizeof h, n);
        memcpy(buf, &h, sizeof h);
        if (bin_queue(path, buf, sizeof h + n)) return;
        bin_store(path, buf, sizeof h + n);
    }
    free(buf);
}

// [UIT 0138] A shader made from a binary exports the same binary again (VK_EXT_shader_object: the data of a shader
// created from binary data is that data), unless the driver changed it on the way in: then it is not used.
static int bin_roundtrip(VkShaderEXT obj, const void *d, size_t dn) {
    size_t n = 0;
    if (vk.vkGetShaderBinaryDataEXT(vkc.device, obj, &n, NULL) != VK_SUCCESS || n != dn) return 0;
    void *b = NULL;
#ifdef _WIN32
    b = _aligned_malloc(n, 16);
#else
    if (posix_memalign(&b, 16, n)) b = NULL;
#endif
    if (!b) return 1;   // (no memory to check with: as before)
    int same = vk.vkGetShaderBinaryDataEXT(vkc.device, obj, &n, b) == VK_SUCCESS && n == dn && !memcmp(b, d, dn);
#ifdef _WIN32
    _aligned_free(b);
#else
    free(b);
#endif
    static _Atomic int said;
    if (!same && said++ < 4) VLOG("shader binary cache: a binary read back after loading differs from its file (%zu bytes): made from SPIR-V again", dn);
    return same;
}
static VkResult create_object_raw(VkShaderStageFlagBits stage, const uint32_t *spv, size_t bytes, VkShaderEXT *obj) {
    int compute = stage == VK_SHADER_STAGE_COMPUTE_BIT;
    VkDescriptorSetLayout layout = vkb_set_layout(compute);
    VkShaderCreateInfoEXT ci = {VK_STRUCTURE_TYPE_SHADER_CREATE_INFO_EXT, .stage = stage,
        .nextStage = stage == VK_SHADER_STAGE_VERTEX_BIT ? VK_SHADER_STAGE_FRAGMENT_BIT : 0,
        .codeType = VK_SHADER_CODE_TYPE_SPIRV_EXT, .codeSize = bytes, .pCode = spv, .pName = "main",
        .setLayoutCount = 1, .pSetLayouts = &layout};
    pthread_once(&g_bin_once, bin_init);
    if (g_bin_on <= 0) return vk.vkCreateShadersEXT(vkc.device, 1, &ci, NULL, obj);
    // [UIT 0135] binaries made on a device with robust buffer access (TOTK_VK_ROBUST=1) carry the driver's bounds checks
    // and those made without do not: the key now tells them apart. Until now the D2 run of 2026-10-31 08:53 (ROBUST=1)
    // took all 9206 shaders from binaries made without it ("0 compiled from SPIR-V"), so its "robust did not help" did
    // not test robust access at all.
    // [UIT 0136] More generally the key named only the shader (SPIR-V, stage, set layout) and the header the driver's
    // binary UUID and version, not the device the binary was made on: binaries made with VK_EXT_depth_bias_control
    // (82c70c7 and later) or without it (2026-10-30 21:08, TOTK_VK_DEPTH_BIAS_FIX=0 since 0134), with or without the
    // checkpoints extension or robust access, were all loaded on whatever device the next run made. The key now holds
    // vkc.dev_fp (src/vk/vk.c device_fingerprint: GPU, driver version, every enabled extension and feature bit), which
    // takes in the robust bit too. TOTK_SHADER_BINARY_DEVKEY=0 (comparison only): keys without it, as before 0135; the
    // header's magic changed in 0136 either way, so no binary from before 0136 is ever loaded.
    static int devkey = -1;
    if (devkey < 0) { const char *e = getenv("TOTK_SHADER_BINARY_DEVKEY"); devkey = !(e && e[0] == '0' && !e[1]); }
    uint64_t lfp = g_layout_fp[compute];
    if (devkey) lfp = shbin_fnv(&vkc.dev_fp, sizeof vkc.dev_fp, shbin_fnv("UIT-0136", 8, lfp));
    shbin_key k = shbin_make_key(spv, bytes, stage, ci.nextStage, lfp);
    char path[1100];
    snprintf(path, sizeof path, "%s/bin/%016llx%016llx.bin", cache_dir(), (unsigned long long)k.a, (unsigned long long)k.b);
    uint64_t t0 = runtime_now_ns();
    size_t fn = 0, dn = 0;
    char *f = read_file(path, &fn);
    const void *d = shbin_check(f, fn, g_bin_uuid, g_bin_ver, stage, k, &dn);
    if (d) {
        VkShaderCreateInfoEXT bi = ci;
        bi.codeType = VK_SHADER_CODE_TYPE_BINARY_EXT; bi.codeSize = dn; bi.pCode = d;
        *obj = VK_NULL_HANDLE;
        VkResult r = vk.vkCreateShadersEXT(vkc.device, 1, &bi, NULL, obj);
        if (r == VK_SUCCESS && *obj && !bin_roundtrip(*obj, d, dn)) {   // [UIT 0138] the driver changed it: not used
            vk.vkDestroyShaderEXT(vkc.device, *obj, NULL);
            *obj = VK_NULL_HANDLE;
            r = VK_INCOMPATIBLE_SHADER_BINARY_EXT;
            g_bin_mismatch++;
        }
        if (r == VK_SUCCESS && *obj) {
            free(f);
            g_bin_hits++; g_bin_hit_ns += runtime_now_ns() - t0;
            return r;
        }
        if (r >= 0 && *obj) vk.vkDestroyShaderEXT(vkc.device, *obj, NULL);   // VK_INCOMPATIBLE_SHADER_BINARY_EXT is a "success" code
        static _Atomic int said;
        if (g_bin_refused++ == 0 || said++ < 4) VLOG("shader binary cache: the driver refused %s (%d): made from SPIR-V again", path, (int)r);
    }
    free(f);
    t0 = runtime_now_ns();
    VkResult r = vk.vkCreateShadersEXT(vkc.device, 1, &ci, NULL, obj);
    g_bin_spv_ns += runtime_now_ns() - t0;
    uint64_t nspv = ++g_bin_spv_n;
    if (r == VK_SUCCESS && *obj) bin_write(path, *obj, stage, k);
    if (!(nspv & 63)) bin_report();   // [UIT native] also during play (the warm-up's line alone said "0 written")
    return r;
}
// [UIT native] Where shader compiles happen, for the long-frame log (mtl_create_stats / mtl_create_ns, which were the
// null backend's stubs): on a thread the frame waits for (drawing / recording / the game: "new pipelines" in the
// [long frame] line) or in the background (warm-up and TOTK_SHADER_ASYNC threads: "shader libraries").
static __thread int t_bg_compile;
static _Atomic uint64_t g_mk_fg, g_mk_bg, g_mk_fg_ns, g_mk_bg_ns;
static VkResult create_object(VkShaderStageFlagBits stage, const uint32_t *spv, size_t bytes, VkShaderEXT *obj) {
    uint64_t t0 = runtime_now_ns();
    VkResult r = create_object_raw(stage, spv, bytes, obj);
    uint64_t dt = runtime_now_ns() - t0;
    if (t_bg_compile) { atomic_fetch_add_explicit(&g_mk_bg, 1, memory_order_relaxed); atomic_fetch_add_explicit(&g_mk_bg_ns, dt, memory_order_relaxed); }
    else { atomic_fetch_add_explicit(&g_mk_fg, 1, memory_order_relaxed); atomic_fetch_add_explicit(&g_mk_fg_ns, dt, memory_order_relaxed); }
    return r;
}
// lib: background compiles; pso: compiles on a thread the frame waits for. The time is the waited-for ones only
// (lib_ns 0): background compiles do not hold the frame.
void mtl_create_stats(uint64_t *lib, uint64_t *pso) { *lib = atomic_load(&g_mk_bg); *pso = atomic_load(&g_mk_fg); }
void mtl_create_ns(uint64_t *lib_ns, uint64_t *pso_ns) { *lib_ns = 0; *pso_ns = atomic_load(&g_mk_fg_ns); }
uint64_t vkb_shader_bg_ns(void) { return atomic_load(&g_mk_bg_ns); }

uint64_t vkb_shader_load_ns;   // shaders made when first drawn with (long submission log, backend_vk.c); g_mu
static void variant_list_add(const char *key, uint32_t intmask, uint32_t sintmask);

// A vertex shader for integer attributes at the locations in intmask (signed where sintmask has the bit): those
// inputs read the raw bits (vkb_spv.c). Made on first use and kept; 0 masks give the shader itself. The variants made
// are listed (<cache>/variants.txt) and made ahead by the warm-up in later runs (prewarm below).
// Two recording threads can ask for the same new variant: the second one made is destroyed and the first one kept.
static VkShaderEXT variant_make(vkb_shader *s, uint32_t intmask, uint32_t sintmask, int from_draw) {
    intmask &= s->in_mask;
    sintmask &= intmask;
    if (!intmask || !s->spv) return s->obj;
    pthread_mutex_lock(&g_mu);
    for (int i = 0; i < s->nvariants; i++)
        if (s->variants[i].intmask == intmask && s->variants[i].sintmask == sintmask) { pthread_mutex_unlock(&g_mu); return s->variants[i].obj; }
    pthread_mutex_unlock(&g_mu);
    uint64_t t0 = runtime_now_ns();
    uint32_t *out = NULL;
    size_t nw = 0;
    VkShaderEXT obj = VK_NULL_HANDLE;
    VkResult r = VK_SUCCESS;
    if (vkb_spv_int_inputs(s->spv, s->nspv_words, intmask, sintmask, &out, &nw)) {
        if (getenv("TOTK_SPV_DUMP")) {   // development: the variant's SPIR-V for spirv-val / spirv-dis
            char path[256];
            snprintf(path, sizeof path, "%s/%.16s-i%x-s%x.spv", getenv("TOTK_SPV_DUMP"), s->key ? s->key : "x", intmask, sintmask);
            FILE *f = fopen(path, "wb");
            if (f) { fwrite(out, 4, nw, f); fclose(f); }
        }
        if ((r = create_object(VK_SHADER_STAGE_VERTEX_BIT, out, nw * 4, &obj)) != VK_SUCCESS) obj = VK_NULL_HANDLE;
        free(out);
    }
    int made = obj != VK_NULL_HANDLE;
    if (!made) {
        static _Atomic int said;
        if (said++ < 8) VLOG("[temporary] shader %.12s: no variant for integer inputs %#x (vkCreateShadersEXT %d): the float reading stays",
                             s->key ? s->key : "?", intmask, (int)r);
        obj = s->obj;
    }
    pthread_mutex_lock(&g_mu);
    if (from_draw) __atomic_fetch_add(&vkb_shader_load_ns, runtime_now_ns() - t0, __ATOMIC_RELAXED);   // a compile on the recording thread: counted as "shaders made"
    for (int i = 0; i < s->nvariants; i++)
        if (s->variants[i].intmask == intmask && s->variants[i].sintmask == sintmask) {   // made by another thread meanwhile
            VkShaderEXT kept = s->variants[i].obj;
            pthread_mutex_unlock(&g_mu);
            if (made) vk.vkDestroyShaderEXT(vkc.device, obj, NULL);
            return kept;
        }
    if (s->nvariants < VKB_VARIANTS) {
        s->variants[s->nvariants++] = (vkb_variant){intmask, sintmask, obj};
        if (made && from_draw) variant_list_add(s->key, intmask, sintmask);
    } else if (made && !from_draw) {   // no room: the warm-up's is not kept
        pthread_mutex_unlock(&g_mu);
        vk.vkDestroyShaderEXT(vkc.device, obj, NULL);
        return s->obj;
    }
    pthread_mutex_unlock(&g_mu);
    if (made && from_draw) {
        static _Atomic int said;
        if (said++ < 16) VLOG("shader %.12s: variant for integer inputs %#x (signed %#x)", s->key ? s->key : "?", intmask, sintmask);
    }
    return obj;
}
VkShaderEXT vkb_shader_variant(const vkb_shader *cs, uint32_t intmask, uint32_t sintmask) {
    return variant_make((vkb_shader *)cs, intmask, sintmask, 1);
}

// [UIT 0330] What the translation left out of a shader (vkb_xnotes.h): said once per shader (the first 64 such shaders),
// counted in vkb_shader_xlate_incomplete. Such a shader draws untextured or with zeroed data while every draw counter
// here stays clean, as the Depths ground did in the 2026-11-01 report.
_Atomic uint32_t vkb_shader_xlate_incomplete;
static void report_notes(const char *key, const char *j) {
    vkb_xnotes r = vkb_xnotes_scan(j);
    if (!vkb_xnotes_any(r)) return;
    atomic_fetch_add_explicit(&vkb_shader_xlate_incomplete, 1, memory_order_relaxed);
    static _Atomic int said;
    if (said++ < 64)
        VLOG("shader %.16s: translation incomplete: %d texture reads removed (bindless handle not found), %d global memory "
             "accesses read as 0, %d textures with a handle outside c2, %d texture arrays bound as one texture [UIT 0330]; "
             "%d textures sampled as 2D for want of a recorded type, %d images assumed R8G8B8A8Unorm [UIT 0337]",
             key, r.tex_removed, r.gmem_zero, r.tex_other_cb, r.tex_arrays, r.tex_type_guessed, r.img_fmt_guessed);
}

// Reads the reflection, rewrites the SPIR-V bindings into the universal layout, creates the shader object.
// t_create_failed: the VkResult when the driver refused it (not a missing translation): the warm-up stops after many.
static _Thread_local int t_create_failed;
static int load(vkb_shader *s, const char *key, int nvn_stage, int defer) {
    char path[1024];
    snprintf(path, sizeof path, "%s/%s.res.json", cache_dir(), key);
    char *j = read_file(path, NULL);
    snprintf(path, sizeof path, "%s/%s.spv", cache_dir(), key);
    size_t nspv = 0;
    uint32_t *spv = (uint32_t *)read_file(path, &nspv);
    if (!j || !spv || nspv < 20 || (nspv & 3) || spv[0] != 0x07230203) { free(j); free(spv); return 0; }
    s->stage = nvn_stage;
    uint32_t base = nvn_stage == 1 ? VKB_STAGE_BINDINGS : 0;
    uint32_t remap[256];   // translator binding -> universal binding (UINT32_MAX: unknown)
    for (int i = 0; i < 256; i++) remap[i] = UINT32_MAX;
    remap[0] = base + VKB_B_SUPPORT;
    const char *p, *e;
    // constant buffers: slot = NVN constant buffer index
    if ((p = json_array(j, "cbufs")))
        for (; (p = json_next(p, &e)); p = e) {
            long b = json_int(p, e, "binding", -1), slot = json_int(p, e, "slot", -1);
            if (b < 0 || b > 255 || slot < 0 || slot >= VKB_NCBUF) continue;
            remap[b] = base + VKB_B_CBUF + (uint32_t)slot;
            s->cbuf_mask |= 1u << slot;
        }
    // storage buffers: the address comes from the NVN driver bank c0, 16 bytes per slot: the stage's storage buffer
    // table (vertex 0x110, fragment 0x510, compute 0x310) or, for compute, its uniform buffer table (0x210): TOTK's
    // compute shaders also use uniform buffers as storage (MacNativeGames' "ubo_ptr")
    int sb_base = nvn_stage == 0 ? 0x110 : nvn_stage == 1 ? 0x510 : 0x310;
    if ((p = json_array(j, "sbufs")))
        for (; (p = json_next(p, &e)); p = e) {
            long b = json_int(p, e, "binding", -1), off = json_int(p, e, "cb_offset", -1) * 4;
            if (b < 0 || b > 255 || s->nsbuf >= VKB_NSBUF) { s->unsupported = 1; continue; }
            long cb = json_int(p, e, "cb_slot", 0);
            int write = (json_int(p, e, "flags", 0) & 1) != 0;   // Ryujinx BufferUsageFlags.Write
            if (write) s->stores = 1;
            int slot = -1, kind = VKB_SB_TABLE;
            if (cb == 0 && off >= sb_base && off < sb_base + 0x100 && !(off & 15)) slot = (int)(off - sb_base) / 16;
            else if (cb == 0 && nvn_stage == 5 && off >= 0x210 && off < 0x310 && !(off & 15)) { slot = (int)(off - 0x210) / 16; kind = VKB_SB_UBO; }
            else if (cb >= 3 && cb <= 16 && off >= 0 && off < 0x10000) { slot = (int)cb - 3; kind = VKB_SB_UBO_PTR; }
            if (slot < 0 || slot >= VKB_NSBUF) {
                static int said; if (said++ < 8) VLOG("shader %s: storage buffer address from c%ld+%#lx: unknown source", key, cb, off);
                slot = 0xFF;
            }
            remap[b] = base + VKB_B_SBUF + (uint32_t)s->nsbuf;
            s->sbuf[s->nsbuf].slot = (uint8_t)slot;
            s->sbuf[s->nsbuf].kind = (uint8_t)kind;
            s->sbuf[s->nsbuf].write = (uint8_t)write;
            s->sbuf[s->nsbuf++].off = (uint16_t)(kind == VKB_SB_UBO_PTR ? off : 0);
        }
    // textures: the handle is a word offset into the NVN driver bank c2 - combined textures at 0x20 (8 bytes per slot),
    // separate textures at 0x168 (MacNativeGames' totk_xlate Service.cs)
    if ((p = json_array(j, "textures")))
        for (; (p = json_next(p, &e)); p = e) {
            long b = json_int(p, e, "binding", -1), h = json_int(p, e, "handle", -1) * 4, type = json_int(p, e, "type", 3);
            if (b < 0 || b > 255 || s->ntex >= VKB_NTEX || h < 0x20) { s->unsupported = 1; continue; }
            vkb_tex_ref *t = &s->tex[s->ntex];
            t->separate = h >= 0x168;
            t->slot = (uint8_t)((h - (t->separate ? 0x168 : 0x20)) / 8);
            t->type = (uint16_t)type;
            remap[b] = base + VKB_B_TEX + (uint32_t)s->ntex++;
        }
    if ((p = json_array(j, "images")))
        for (; (p = json_next(p, &e)); p = e) {
            long b = json_int(p, e, "binding", -1), h = json_int(p, e, "handle", -1) * 4, type = json_int(p, e, "type", 3);
            if (b < 0 || b > 255 || s->nimg >= VKB_NIMG || h < 0x120) { s->unsupported = 1; continue; }
            s->img[s->nimg] = (vkb_tex_ref){(uint8_t)((h - 0x120) / 8), 0, (uint16_t)type};
            s->stores = 1;   // [temporary] images are taken as written (the reflection has no read-only mark)
            remap[b] = base + VKB_B_IMG + (uint32_t)s->nimg++;
        }
    s->frag_outputs = (uint32_t)json_top_int(j, "frag_out_map", 0);
    report_notes(key, j);
    free(j);
    // rewrite Binding decorations (OpDecorate %id Binding n: opcode 71, decoration 33)
    size_t nw = drop_cull_distance(spv, nspv / 4);
    nspv = nw * 4;
    for (size_t i = 5; i < nw;) {
        uint32_t op = spv[i] & 0xffff, wc = spv[i] >> 16;
        if (!wc || i + wc > nw) break;
        if (op == 71 && wc == 4 && spv[i + 2] == 33) {
            uint32_t old = spv[i + 3];
            if (old > 255 || remap[old] == UINT32_MAX) {
                VLOG("shader %s: binding %u not in its reflection", key, old);
                s->unsupported = 1;
            } else spv[i + 3] = remap[old];
        }
        i += wc;
    }
    VkShaderStageFlagBits stage = nvn_stage == 0 ? VK_SHADER_STAGE_VERTEX_BIT : nvn_stage == 1 ? VK_SHADER_STAGE_FRAGMENT_BIT : VK_SHADER_STAGE_COMPUTE_BIT;
    if (defer) {   // [UIT 0039] everything but the driver compile: async_compile makes the object
        s->in_mask = vkb_spv_input_mask(spv, nspv / 4);
        s->out_mask = vkb_spv_output_mask(spv, nspv / 4);
        uint32_t reads = vkb_spv_sbuf_reads(spv, nspv / 4, base + VKB_B_SBUF, s->nsbuf);
        for (int i = 0; i < s->nsbuf; i++) s->sbuf[i].read = (uint8_t)(reads >> i & 1);
        s->spv = spv;
        s->nspv_words = nspv / 4;
        s->vk_stage = stage;
        s->obj = VK_NULL_HANDLE;
        __atomic_store_n(&s->pending, 1, __ATOMIC_RELEASE);
        return 1;
    }
    VkResult r = create_object(stage, spv, nspv, &s->obj);
    if (r == VK_SUCCESS) {   // input / output locations: vertex variants; fragment inputs no vertex shader writes (audit)
        s->in_mask = vkb_spv_input_mask(spv, nspv / 4);
        s->out_mask = vkb_spv_output_mask(spv, nspv / 4);
        uint32_t reads = vkb_spv_sbuf_reads(spv, nspv / 4, base + VKB_B_SBUF, s->nsbuf);
        for (int i = 0; i < s->nsbuf; i++) s->sbuf[i].read = (uint8_t)(reads >> i & 1);
    }
    if (stage == VK_SHADER_STAGE_VERTEX_BIT && r == VK_SUCCESS) {   // kept for attribute variants (vkb_shader_variant)
        s->spv = spv;
        s->nspv_words = nspv / 4;
    } else free(spv);
    if (r != VK_SUCCESS) { VLOG("shader %s: vkCreateShadersEXT %d", key, (int)r); s->obj = VK_NULL_HANDLE; t_create_failed = (int)r; return 0; }
    if (getenv("TOTK_VK_VALIDATION")) VLOG("shader %.12s = VkShaderEXT %#llx", key, (unsigned long long)(uintptr_t)s->obj);   // names validation messages
    s->vk_stage = stage;
    return 1;
}

// ---- shaders made ahead (the warm-up): creating a shader object compiles it in the driver, ~17% of the recording
// thread on the way into the world, where new shaders arrive in bursts (submissions held up to 2.3 s, Great Sky Island,
// 2026-10-24). Every shader loaded is listed (<cache>/used.txt: key and NVN stage, appended), and every integer-input
// vertex variant made (<cache>/variants.txt: key, masks); at the first shader lookup of a run, warm-up threads create
// the listed ones that are not loaded yet (as SavageWind's pipeline list, and MacNativeGames' warm-up before the title:
// keys only, nothing of the game's data is in the lists). The constants c1 come from the game's program at the first
// real lookup (c1_set).
//  TOTK_SHADER_PREWARM   =0: off (shaders made when first drawn with); =all: also every translated shader of the build
//                        (<cache>/../shaders.tsv, ~28,800: the first run several minutes, later the driver's own cache)
//  TOTK_SHADER_WARM_WAIT seconds the first lookup (the boot, before the title) waits for the warm-up; 0: in the
//                        background only (the old order). Default 90 (all: 600). Past it the rest goes on in the background.
//  TOTK_SHADER_WARM_THREADS  threads (default: the logical CPUs - 2, at most 6; 1..12 when set; below normal priority
//                        once the wait is over). 12 threads with =all crashed in the NVIDIA driver (nvoglv64.dll reading
//                        0x10 inside vkCreateShadersEXT after ~23,000 shaders, 2026-10-29, after one -13 VK_ERROR_UNKNOWN).
// Crash-safe: each warm-up thread writes the key it is about to make into its slot of <cache>/warm-journal.txt
// (positioned writes: in the OS's file cache at once, kept when the process dies) and blanks it after. A crash
// (rt_fault_note, runtime/dispatch.c) marks the journal; the next run adds the keys still in their slots to
// <cache>/warm-skip.txt, as does a create the driver refuses with anything but out of memory. The warm-up skips
// those keys (they are made when first drawn with, as without the warm-up). Delete warm-skip.txt to try them again.
// The warm-up also stops when the system's free commit (RAM + page file) drops below 2 GiB.
typedef struct { char key[65]; uint8_t stage; } warm_job;
typedef struct { char key[65]; uint32_t intmask, sintmask; } warm_var;
static uint64_t g_prewarm_hits;
static _Atomic uint64_t g_prewarm_made, g_prewarm_vars, g_prewarm_fail;
static warm_job *g_jobs;
static size_t g_njobs;
static warm_var *g_vars;
static size_t g_nvars;
static _Atomic size_t g_job_at, g_var_at, g_jobs_done;
static _Atomic int g_threads_left, g_threads_in_list, g_waiting, g_stop;

// ---- the journal and the skip list
#define WJ_HEAD 16   // "UTWJ <state>\n": R running, C crashed
#define WJ_SLOT 66   // 64 hex + ' ' or '\n' padding
#define WJ_SLOTS 12
static HANDLE g_journal = INVALID_HANDLE_VALUE;
static _Atomic int g_slot_next;
static _Thread_local int t_slot = -1;
static char (*g_skip)[65];
static size_t g_nskip;
static uint64_t g_skipped;
static void wj_write(DWORD off, const void *p, DWORD n) {
    if (g_journal == INVALID_HANDLE_VALUE) return;
    OVERLAPPED o = {0};
    o.Offset = off;
    DWORD w;
    WriteFile(g_journal, p, n, &w, &o);
}
static void wj_mark(const char *key) {   // before a create: the key in this thread's slot
    if (t_slot < 0) return;
    char b[WJ_SLOT];
    memcpy(b, key, 64);
    b[64] = ' '; b[65] = '\n';
    wj_write(WJ_HEAD + (DWORD)t_slot * WJ_SLOT, b, WJ_SLOT);
}
static void wj_clear(void) {
    if (t_slot < 0) return;
    char b[WJ_SLOT];
    memset(b, '-', 64);
    b[64] = ' '; b[65] = '\n';
    wj_write(WJ_HEAD + (DWORD)t_slot * WJ_SLOT, b, WJ_SLOT);
}
static void wj_fault_note(void) { wj_write(5, "C", 1); }   // the crash handler: no allocation, one write
extern void (*rt_fault_note)(void);
static void skip_append(const char *key, const char *why) {
    char path[1024];
    snprintf(path, sizeof path, "%s/warm-skip.txt", cache_dir());
    FILE *f = fopen(path, "ab");
    if (!f) return;
    fprintf(f, "%.64s %s\n", key, why);
    fclose(f);
}
static int is_key(const char *p);
static void skip_read(void) {
    char path[1024];
    snprintf(path, sizeof path, "%s/warm-skip.txt", cache_dir());
    char *t = read_file(path, NULL);
    if (!t) return;
    size_t cap = 0;
    for (char *l = t; *l;) {
        char *nl = strchr(l, '\n');
        size_t len = nl ? (size_t)(nl - l) : strlen(l);
        if (len >= 64 && is_key(l)) {
            if (g_nskip == cap) {
                cap = cap ? cap * 2 : 16;
                if (!(g_skip = realloc(g_skip, cap * sizeof *g_skip))) die("shader warm-up skip list");
            }
            memcpy(g_skip[g_nskip], l, 64);
            g_skip[g_nskip++][64] = 0;
        }
        l += len + (nl != NULL);
    }
    free(t);
}
static int skipped(const char *key) {
    for (size_t i = 0; i < g_nskip; i++)
        if (!memcmp(g_skip[i], key, 64)) return 1;
    return 0;
}
// The last run's journal: when it crashed, the keys being made then go to the skip list. Then a fresh journal.
static void journal_start(void) {
    char path[1024];
    snprintf(path, sizeof path, "%s/warm-journal.txt", cache_dir());
    size_t n = 0;
    char *t = read_file(path, &n);
    if (t && n >= WJ_HEAD && !memcmp(t, "UTWJ ", 5) && t[5] == 'C') {
        int any = 0;
        for (size_t at = WJ_HEAD; at + 64 <= n; at += WJ_SLOT)
            if (is_key(t + at)) {
                skip_append(t + at, "being made when the last run crashed");
                VLOG("shader warm-up: %.64s was being made when the last run crashed: the warm-up skips it from now on "
                     "(made when first drawn with; <cache>/warm-skip.txt)", t + at);
                any = 1;
            }
        if (!any) VLOG("shader warm-up: the last run crashed, not while the warm-up was making a shader");
    }
    free(t);
    g_journal = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                            CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (g_journal == INVALID_HANDLE_VALUE) { VLOG("shader warm-up: %s: cannot write (no crash journal)", path); return; }
    char head[WJ_HEAD];
    memset(head, ' ', sizeof head);
    memcpy(head, "UTWJ R", 6);
    head[WJ_HEAD - 1] = '\n';
    wj_write(0, head, WJ_HEAD);
    rt_fault_note = wj_fault_note;
}
static int commit_low(void) {   // free commit (RAM + page file) below 2 GiB
    MEMORYSTATUSEX m = {.dwLength = sizeof m};
    return GlobalMemoryStatusEx(&m) && m.ullAvailPageFile < (2ull << 30);
}
static double commit_free_gib(void) {
    MEMORYSTATUSEX m = {.dwLength = sizeof m};
    return GlobalMemoryStatusEx(&m) ? m.ullAvailPageFile / 1073741824.0 : -1;
}

static void list_add(const char *key, int stage) {   // g_mu held
    char path[1024];
    snprintf(path, sizeof path, "%s/used.txt", cache_dir());
    FILE *f = fopen(path, "ab");
    if (!f) return;
    fprintf(f, "%s %d\n", key, stage);
    fclose(f);
}
static void variant_list_add(const char *key, uint32_t intmask, uint32_t sintmask) {   // g_mu held
    if (!key) return;
    char path[1024];
    snprintf(path, sizeof path, "%s/variants.txt", cache_dir());
    FILE *f = fopen(path, "ab");
    if (!f) return;
    fprintf(f, "%s %x %x\n", key, intmask, sintmask);
    fclose(f);
}

static int is_key(const char *p) {
    for (int i = 0; i < 64; i++)
        if (!((p[i] >= '0' && p[i] <= '9') || (p[i] >= 'a' && p[i] <= 'f'))) return 0;
    return 1;
}
static void job_add(const char *key, int stage, size_t *cap) {
    if (stage != 0 && stage != 1 && stage != 5) return;
    if (g_nskip && skipped(key)) { g_skipped++; return; }
    if (g_njobs == *cap) {
        *cap = *cap ? *cap * 2 : 1024;
        if (!(g_jobs = realloc(g_jobs, *cap * sizeof *g_jobs))) die("shader warm-up list");
    }
    memcpy(g_jobs[g_njobs].key, key, 64);
    g_jobs[g_njobs].key[64] = 0;
    g_jobs[g_njobs++].stage = (uint8_t)stage;
}
// The lists line by line (a line cut short by a crash, or a CRLF from an editor, is skipped alone; the old reader went
// by 67-byte records, and one short line lost every line after it).
static size_t read_used(size_t *cap) {
    char path[1024];
    snprintf(path, sizeof path, "%s/used.txt", cache_dir());
    char *t = read_file(path, NULL);
    if (!t) return 0;
    size_t n0 = g_njobs;
    for (char *l = t; *l;) {
        char *nl = strchr(l, '\n');
        size_t len = nl ? (size_t)(nl - l) : strlen(l);
        if (len >= 66 && is_key(l) && l[64] == ' ') job_add(l, l[65] - '0', cap);
        l += len + (nl != NULL);
    }
    free(t);
    return g_njobs - n0;
}
static size_t read_all(size_t *cap) {   // the build's shader table (tools/export_shaders.py): key, stage vs/ps/cs, ...
    char path[1024];
    snprintf(path, sizeof path, "%s/../shaders.tsv", cache_dir());
    char *t = read_file(path, NULL);
    if (!t) { VLOG("shader warm-up: no %s: only the list is made ahead", path); return 0; }
    size_t n0 = g_njobs;
    for (char *l = t; *l;) {
        char *nl = strchr(l, '\n');
        size_t len = nl ? (size_t)(nl - l) : strlen(l);
        if (len >= 67 && is_key(l) && l[64] == '\t') {
            int st = !strncmp(l + 65, "vs", 2) ? 0 : !strncmp(l + 65, "ps", 2) ? 1 : !strncmp(l + 65, "cs", 2) ? 5 : -1;
            job_add(l, st, cap);
        }
        l += len + (nl != NULL);
    }
    free(t);
    return g_njobs - n0;
}
static void read_variants(void) {
    char path[1024];
    snprintf(path, sizeof path, "%s/variants.txt", cache_dir());
    char *t = read_file(path, NULL);
    if (!t) return;
    size_t cap = 0;
    for (char *l = t; *l;) {
        char *nl = strchr(l, '\n');
        size_t len = nl ? (size_t)(nl - l) : strlen(l);
        unsigned im = 0, sm = 0;
        if (len >= 68 && is_key(l) && l[64] == ' ' && sscanf(l + 65, "%x %x", &im, &sm) == 2 && im && !(g_nskip && skipped(l))) {
            if (g_nvars == cap) {
                cap = cap ? cap * 2 : 64;
                if (!(g_vars = realloc(g_vars, cap * sizeof *g_vars))) die("shader warm-up variants");
            }
            memcpy(g_vars[g_nvars].key, l, 64);
            g_vars[g_nvars].key[64] = 0;
            g_vars[g_nvars].intmask = im;
            g_vars[g_nvars++].sintmask = sm;
        }
        l += len + (nl != NULL);
    }
    free(t);
}

static shentry *find_locked(const char *key, uint32_t h) {   // g_mu held
    shentry *e = g_tab[h];
    while (e && strcmp(e->key, key)) e = e->next;
    return e;
}
static void warm_one(const warm_job *j) {
    uint32_t h = (uint32_t)strtoul(j->key + 56, NULL, 16) & 4095;
    pthread_mutex_lock(&g_mu);
    shentry *e = find_locked(j->key, h);
    pthread_mutex_unlock(&g_mu);
    if (e) return;
    shentry *n = calloc(1, sizeof *n);
    if (!n) die("shader entry");
    memcpy(n->key, j->key, 65);
    n->sh.key = n->key;
    t_create_failed = 0;
    wj_mark(j->key);
    int ok = load(&n->sh, n->key, j->stage, 0);
    wj_clear();
    if (!ok) {   // no translation (a key the translator failed on): skipped
        free(n);
        if (t_create_failed && t_create_failed != VK_ERROR_OUT_OF_HOST_MEMORY && t_create_failed != VK_ERROR_OUT_OF_DEVICE_MEMORY) {
            char why[48];
            snprintf(why, sizeof why, "vkCreateShadersEXT %d", t_create_failed);
            skip_append(j->key, why);   // refused for itself, not for memory: not tried ahead again
        }
        if (t_create_failed && g_prewarm_fail++ >= 64) {   // the driver refusing (out of memory): the rest is made when first drawn with
            if (!atomic_exchange(&g_stop, 1)) VLOG("shader warm-up: stopped after 64 shaders that could not be made");
        }
        return;
    }
    n->prewarmed = 1;
    pthread_mutex_lock(&g_mu);
    e = find_locked(j->key, h);
    if (!e) { n->next = g_tab[h]; g_tab[h] = n; g_loaded++; }
    pthread_mutex_unlock(&g_mu);
    if (e) { vk.vkDestroyShaderEXT(vkc.device, n->sh.obj, NULL); free(n->sh.spv); free(n); }   // loaded meanwhile
    else g_prewarm_made++;
}
static void *prewarm_thread(void *unused) {
    (void)unused;
    pthread_setname_np("totk-shader-warm");
    { int plat_thread_ecores(void); plat_thread_ecores(); }   // [UIT 0089] TOTK_HYBRID_PLACEMENT=1: efficiency cores (hybrid CPUs)
    t_bg_compile = 1;
    t_slot = atomic_fetch_add(&g_slot_next, 1);
    if (t_slot >= WJ_SLOTS) t_slot = -1;
    int low = 0;
    if (!g_waiting) { SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL); low = 1; }
    for (;;) {   // the shaders
        if (!low && !g_waiting) { SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL); low = 1; }
        size_t at = atomic_fetch_add(&g_job_at, 1);
        if (at >= g_njobs || g_stop) break;
        if ((at & 63) == 0 && commit_low() && !atomic_exchange(&g_stop, 1)) {
            VLOG("shader warm-up: stopped at %zu of %zu: less than 2 GiB of memory (RAM + page file) left; the rest is "
                 "made when first drawn with", at, g_njobs);
            break;
        }
        warm_one(&g_jobs[at]);
        g_jobs_done++;
    }
    g_threads_in_list--;
    while (g_threads_in_list > 0) Sleep(2);   // the variants need their vertex shaders made: after every thread's last shader
    for (;;) {   // the integer-input vertex variants
        size_t at = atomic_fetch_add(&g_var_at, 1);
        if (at >= g_nvars || g_stop) break;
        const warm_var *v = &g_vars[at];
        uint32_t h = (uint32_t)strtoul(v->key + 56, NULL, 16) & 4095;
        pthread_mutex_lock(&g_mu);
        shentry *e = find_locked(v->key, h);
        pthread_mutex_unlock(&g_mu);
        if (e && !vkb_shader_pending(&e->sh) && e->sh.obj && e->sh.stage == 0) {   // pending: compiling (early / async): skipped
            wj_mark(v->key);
            if (variant_make(&e->sh, v->intmask, v->sintmask, 0) != e->sh.obj) g_prewarm_vars++;
            wj_clear();
        }
    }
    if (--g_threads_left == 0) {
        VLOG("shaders made ahead: %llu of %zu listed, %llu integer-input variants of %zu",
             (unsigned long long)g_prewarm_made, g_njobs, (unsigned long long)g_prewarm_vars, g_nvars);
        bin_report();
    }
    return NULL;
}
static int g_warm_only;   // [UIT 0081] TOTK_WARM_ONLY=1 (tools/setup.py --warm-shaders): warm up, then exit (below)
static void prewarm_run(void) {
    const char *e = getenv("TOTK_SHADER_PREWARM");   // =0: shaders are made only when first drawn with (A/B)
    if (e && *e == '0') { VLOG("shader warm-up: off (TOTK_SHADER_PREWARM=0)"); return; }
    int all = e && !strcmp(e, "all");
    journal_start();
    skip_read();
    size_t cap = 0;
    size_t listed = read_used(&cap);
    size_t everything = all ? read_all(&cap) : 0;
    read_variants();
    if (!g_njobs && !g_nvars) return;   // the first run: the lists fill as the game goes
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    int n = (int)si.dwNumberOfProcessors - 2;
    if (n > 6) n = 6;   // 12 crashed the NVIDIA driver with =all (above); the compile is mostly the driver's
    if ((e = getenv("TOTK_SHADER_WARM_THREADS")) && *e) n = atoi(e);
    if (n < 1) n = 1;
    if (n > WJ_SLOTS) n = WJ_SLOTS;
    double wait_s = all ? 600 : 90;
    if ((e = getenv("TOTK_SHADER_WARM_WAIT")) && *e) wait_s = atof(e);
    if (g_warm_only) wait_s = 1e9;   // nothing else to wait for: every listed shader
    VLOG("shader list: %zu shaders to make ahead (%zu used before, %zu of the build's), %zu integer-input variants, %d threads, waiting up to %.0f s",
         g_njobs, listed, everything, g_nvars, n, wait_s);
    if (g_skipped) VLOG("shader warm-up: %llu skipped (<cache>/warm-skip.txt: crashed or refused before; made when first drawn with)",
                        (unsigned long long)g_skipped);
    if (all) VLOG("shader warm-up: every shader (TOTK_SHADER_PREWARM=all) keeps ~30,000 shader objects in the driver; "
                  "the default (the shaders you have seen) is the recommended one");
    g_threads_left = n;
    g_threads_in_list = n;
    g_waiting = wait_s > 0;
    int started = 0;
    for (int i = 0; i < n; i++) {
        pthread_t t;
        if (pthread_create(&t, NULL, prewarm_thread, NULL) == 0) { pthread_detach(t); started++; }
    }
    if (started < n) {   // the counters the threads wait on count only the threads that run
        g_threads_left -= n - started;
        g_threads_in_list -= n - started;
    }
    if (!started || !g_waiting) { g_waiting = 0; return; }
    uint64_t t0 = runtime_now_ns(), last = t0, limit = (uint64_t)(wait_s * 1e9);
    while (g_threads_left > 0) {   // before the title: the game's first frames wait, its window stays responsive (window_sdl.c)
        Sleep(10);
        uint64_t now = runtime_now_ns();
        if (now - last >= 2000000000ull) {
            last = now;
            VLOG("shader warm-up: %zu of %zu shaders, %.0f s, %.1f GiB memory free (RAM + page file)", (size_t)g_jobs_done,
                 g_njobs, (now - t0) / 1e9, commit_free_gib());
        }
        if (now - t0 >= limit) { VLOG("shader warm-up: %.0f s waited, the rest goes on in the background", wait_s); break; }
    }
    g_waiting = 0;
    VLOG("shader warm-up: %llu shaders and %llu variants made in %.1f s before the game went on",
         (unsigned long long)g_prewarm_made, (unsigned long long)g_prewarm_vars, (runtime_now_ns() - t0) / 1e9);
}
// [UIT 0081] Re-warm after an update or a rebuild (tools/setup.py --warm-shaders starts the game with TOTK_WARM_ONLY=1):
// the listed shaders are made exactly as at a normal start, which fills the graphics driver's own shader cache (and the
// binary cache when on), then the game exits before the title screen. The next normal start finds them made. The
// progress lines above ("shader warm-up: N of M shaders") are what setup.py shows; the last line says it is done.
// [UIT 0091] TOTK_WARM_ONLY: before the exit, wait for the shader binaries still waiting to be written. On this branch
// they are written in place (nothing waits). native-1031's TOTK_SHADER_BIN_BG writes them on a background thread
// (its queue g_bwq, vkb_shqueue.h); without this wait the _Exit below would drop the files still queued and the re-warm
// would have made those shaders for nothing. Compiled in only once both are merged (SHQ_INIT is vkb_shqueue.h's); if
// the queue is renamed there, this stops compiling rather than silently skipping the wait. At most 5 minutes.
static void warm_only_drain(void) {
#ifdef SHQ_INIT
    uint64_t t0 = runtime_now_ns();
    size_t left = 0;
    for (int i = 0; i < 30000; i++) {
        pthread_mutex_lock(&g_bwq.mu);
        left = g_bwq.n + g_bwq.bytes;   // n: queued; bytes: queued or being written
        size_t files = g_bwq.n;
        pthread_mutex_unlock(&g_bwq.mu);
        if (!left) break;
        if (i == 0) VLOG("shader warm-up only (TOTK_WARM_ONLY): waiting for %zu shader binaries to be written", files);
        Sleep(10);
    }
    if (left) VLOG("shader warm-up only (TOTK_WARM_ONLY): shader binaries still being written after %.0f s; exiting anyway "
                   "(those are made again next time)", (runtime_now_ns() - t0) / 1e9);
#endif
}
static void prewarm_start(void) {
    const char *e = getenv("TOTK_WARM_ONLY");
    g_warm_only = e && *e && strcmp(e, "0");
    uint64_t t0 = runtime_now_ns();
    prewarm_run();
    if (!g_warm_only) return;
    while (g_threads_left > 0) Sleep(10);
    warm_only_drain();
    VLOG("shader warm-up only (TOTK_WARM_ONLY): done, %llu shaders and %llu variants made in %.1f s; the game exits",
         (unsigned long long)g_prewarm_made, (unsigned long long)g_prewarm_vars, (runtime_now_ns() - t0) / 1e9);
    fflush(NULL);
    _Exit(0);   // as closing the window does (window_sdl.c)
}

// ---- [UIT 0039] asynchronous shader compilation (TOTK_SHADER_ASYNC=1, off by default; Performance > Stutter > Shaders)
// As Yuzu's and Ryujinx's asynchronous shaders: a shader the game draws with for the first time (and the warm-up has not
// made) is handed to background threads instead of being compiled on the recording thread, which held the submission
// for the compile. vkb_draw.c skips the draws with it while it compiles, but only ordinary colour draws into targets drawn
// every frame, for at most ASYNC_MAX_SKIP frames; any other draw waits for it (vkb_shader_wait). Compute shaders are
// compiled at once as before. State per shader: pending 1 queued, 2 compiling, 0 done (obj, or NULL when the driver
// refused it). Whoever moves it from 1 to 2 (a background thread, or a draw that cannot wait) compiles it.
enum { ASYNC_THREADS = 2 };
static int g_async = -1;
static pthread_mutex_t g_amu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_aq_cv = PTHREAD_COND_INITIALIZER;   // a job was queued
static pthread_cond_t g_done_cv = PTHREAD_COND_INITIALIZER; // a compile finished
static vkb_shader **g_aq;
static size_t g_aq_n, g_aq_cap, g_aq_at;
static _Atomic uint64_t g_as_queued, g_as_bg, g_as_fg, g_as_waited, g_as_skipped, g_as_failed, g_as_log_frame;
static _Atomic uint64_t g_as_bg_ns, g_as_fg_ns, g_early_wait_ns;
int vkb_shader_early_on(void);

static void async_log(void) {   // once per 120 presents at most, when something changed
    if (g_async <= 0) return;   // TOTK_SHADER_EARLY alone: its own line (early_log)
    uint64_t f = vkb_frame(), last = atomic_load(&g_as_log_frame);
    if (f < last + 120 || !atomic_compare_exchange_strong(&g_as_log_frame, &last, f)) return;
    static _Atomic uint64_t said_sig;
    uint64_t sig = atomic_load(&g_as_queued) * 31 + atomic_load(&g_as_skipped) * 7 + atomic_load(&g_as_fg);
    if (atomic_exchange(&said_sig, sig) == sig) return;
    uint64_t bg = atomic_load(&g_as_bg), fg = atomic_load(&g_as_fg);
    VLOG("[shaders] async (TOTK_SHADER_ASYNC): %llu new shaders queued, %llu compiled in the background (%.1f ms each), "
         "%llu draws skipped while compiling, %llu compiled on the drawing thread (a draw that is not skipped, %.1f ms each), "
         "%llu waits for a background compile, %llu refused by the driver",
         (unsigned long long)atomic_load(&g_as_queued), (unsigned long long)bg, bg ? atomic_load(&g_as_bg_ns) / 1e6 / bg : 0.0,
         (unsigned long long)atomic_load(&g_as_skipped), (unsigned long long)fg, fg ? atomic_load(&g_as_fg_ns) / 1e6 / fg : 0.0,
         (unsigned long long)atomic_load(&g_as_waited), (unsigned long long)atomic_load(&g_as_failed));
}

// The driver compile of a shader in state 2 (the caller moved it there).
static void async_compile(vkb_shader *s, int background) {
    uint64_t t0 = runtime_now_ns();
    VkShaderEXT obj = VK_NULL_HANDLE;
    uint32_t *spv = s->spv;
    VkResult r = create_object(s->vk_stage, spv, s->nspv_words * 4, &obj);
    if (r != VK_SUCCESS || !obj) {
        obj = VK_NULL_HANDLE;
        atomic_fetch_add(&g_as_failed, 1);
        VLOG("shader %s: vkCreateShadersEXT %d (asynchronous compile): its draws are skipped", s->key ? s->key : "?", (int)r);
    }
    if (s->vk_stage != VK_SHADER_STAGE_VERTEX_BIT || !obj) { s->spv = NULL; s->nspv_words = 0; free(spv); }   // as load()
    s->obj = obj;
    uint64_t dt = runtime_now_ns() - t0;
    if (background) { atomic_fetch_add(&g_as_bg, 1); atomic_fetch_add(&g_as_bg_ns, dt); }
    else { atomic_fetch_add(&g_as_fg, 1); atomic_fetch_add(&g_as_fg_ns, dt); }
    shasync_publish(&s->pending, &g_amu, &g_done_cv);   // obj and spv above are visible to whoever then reads 0
    async_log();
}
static int async_claim(vkb_shader *s) { return shasync_claim(&s->pending); }
static void *async_thread(void *unused) {
    (void)unused;
    t_bg_compile = 1;
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    { int plat_thread_ecores(void); plat_thread_ecores(); }   // [UIT 0089] TOTK_HYBRID_PLACEMENT=1: efficiency cores (hybrid CPUs)
    for (;;) {
        pthread_mutex_lock(&g_amu);
        while (g_aq_at == g_aq_n) pthread_cond_wait(&g_aq_cv, &g_amu);
        vkb_shader *s = g_aq[g_aq_at++];
        if (g_aq_at == g_aq_n) g_aq_at = g_aq_n = 0;
        pthread_mutex_unlock(&g_amu);
        if (async_claim(s)) async_compile(s, 1);   // else a draw took it meanwhile
    }
    return NULL;
}
static void async_init(void) {
    const char *e = getenv("TOTK_SHADER_ASYNC");
    g_async = e && atoi(e) > 0;
    if (!g_async) return;
    int started = 0;
    for (int i = 0; i < ASYNC_THREADS; i++) {
        pthread_t t;
        if (pthread_create(&t, NULL, async_thread, NULL) == 0) { pthread_detach(t); started++; }
    }
    if (!started) { g_async = 0; VLOG("[shaders] async (TOTK_SHADER_ASYNC): no thread could be started: off"); return; }
    VLOG("[shaders] async (TOTK_SHADER_ASYNC=1): new shaders compile on %d background threads; ordinary colour draws with "
         "them are skipped for up to a few frames meanwhile, every other draw waits for its shader", started);
}
int vkb_shader_async_on(void) {
    static pthread_once_t once = PTHREAD_ONCE_INIT;
    pthread_once(&once, async_init);
    return g_async > 0;
}
static void async_queue(vkb_shader *s) {
    pthread_mutex_lock(&g_amu);
    if (g_aq_n == g_aq_cap) {
        size_t cap = g_aq_cap ? g_aq_cap * 2 : 256;
        vkb_shader **q = realloc(g_aq, cap * sizeof *q);
        if (!q) die("async shader queue");
        g_aq = q; g_aq_cap = cap;
    }
    g_aq[g_aq_n++] = s;
    pthread_cond_signal(&g_aq_cv);
    pthread_mutex_unlock(&g_amu);
    atomic_fetch_add(&g_as_queued, 1);
}
void vkb_shader_wait(const vkb_shader *cs) {
    vkb_shader *s = (vkb_shader *)cs;
    if (!s || !vkb_shader_pending(s)) return;
    uint64_t t0 = runtime_now_ns();
    if (async_claim(s)) {   // still queued: compiled here, as without TOTK_SHADER_ASYNC
        async_compile(s, 0);
        __atomic_fetch_add(&vkb_shader_load_ns, runtime_now_ns() - t0, __ATOMIC_RELAXED);   // "shaders made" (long submission)
        return;
    }
    shasync_wait_done(&s->pending, &g_amu, &g_done_cv);
    uint64_t dt = runtime_now_ns() - t0;
    atomic_fetch_add(&g_as_waited, 1);
    atomic_fetch_add(&g_as_fg_ns, dt);
    atomic_fetch_add(&g_early_wait_ns, dt);
    __atomic_fetch_add(&vkb_shader_load_ns, dt, __ATOMIC_RELAXED);
}
void vkb_shader_async_skipped(void) { atomic_fetch_add(&g_as_skipped, 1); async_log(); }

// ---- [UIT native] TOTK_SHADER_EARLY: shaders started when the game hands them over, not when first drawn with.
// The game gives a program its shaders (nvnProgramSetShaders) when it loads the model or effect, usually well before
// anything is drawn with it; without this the driver compile starts at the first draw, on a recording thread, and the
// frame waits for it (the "shaders made" of the long submissions: 9.1 s over 45 submissions, worst 889 ms, in an
// 86-minute play, 2026-10-30). With it: the shader's code is copied at SetShaders and handed to background threads
// that look it up, read its translation and compile it (the binary cache first). Nothing is ever skipped: a draw that
// comes first waits for that compile (or compiles it itself while still queued), exactly the TOTK_SHADER_ASYNC
// states without its skipping (async_skippable says no unless TOTK_SHADER_ASYNC is on). Also: a shader first met at a
// draw is compiled outside the shader table's lock (g_mu), so the other recording threads' lookups don't wait for it.
// [UIT 0146] Two steps. One sorting thread hashes each copy and looks it up (a few microseconds to ~0.1 ms): the
// shaders known already (most of them: 7,522 of 12,751 on the way into the world) are done there. Only the new ones go
// on to the build threads (g_ec). Before, every copy waited in one first-in-first-out queue behind builds of ~32 ms
// each, so the queue filled while the game loaded: 2,123 shaders were dropped (2026-10-30 log) and were built later,
// at their first draw (the "shaders made" pauses of 120-260 ms in the long submission lines).
typedef struct { shq_job q; uint32_t stage, length; uint8_t *buf; char key[65]; uint32_t h; } early_job;   // buf: control then code
static shq g_eq = SHQ_INIT(2048, 128u << 20);   // copies of shaders' code waiting to be looked up (tens of KiB each)
static shq g_ec = SHQ_INIT(4096, 256u << 20);   // new shaders waiting for a build thread
static int g_early = -1, g_early_threads;
static _Atomic uint64_t g_early_seen, g_early_new, g_early_known, g_early_dropped, g_early_none, g_early_log_frame;
static _Atomic uint64_t g_early_bdropped;   // [UIT 0146] new, but the build queue was full (built at the first draw)
static _Atomic uint64_t g_early_ready, g_early_inflight;   // at the first draw: compiled already / still compiling
static void early_log(void) {   // once per 600 presents at most, when something changed
    uint64_t f = vkb_frame(), last = atomic_load(&g_early_log_frame);
    if (f < last + 600 || !atomic_compare_exchange_strong(&g_early_log_frame, &last, f)) return;
    static _Atomic uint64_t said_sig;
    uint64_t sig = atomic_load(&g_early_seen) * 31 + atomic_load(&g_early_ready) * 7 + atomic_load(&g_early_inflight);
    if (atomic_exchange(&said_sig, sig) == sig) return;
    uint64_t bg = atomic_load(&g_as_bg), fg = atomic_load(&g_as_fg);
    VLOG("[shaders] early (TOTK_SHADER_EARLY): %llu shaders handed over by the game, %llu new started in the background "
         "(%llu known already, %llu without a translation, %llu not queued: queue full, %llu new ones not queued: build queue full); at their first draw %llu were ready "
         "and %llu still compiling (the draw waited %.0f ms in all); background compiles %llu (%.1f ms each), compiled by a "
         "draw %llu (%.1f ms each)",
         (unsigned long long)atomic_load(&g_early_seen), (unsigned long long)atomic_load(&g_early_new),
         (unsigned long long)atomic_load(&g_early_known), (unsigned long long)atomic_load(&g_early_none),
         (unsigned long long)atomic_load(&g_early_dropped), (unsigned long long)atomic_load(&g_early_bdropped),
         (unsigned long long)atomic_load(&g_early_ready),
         (unsigned long long)atomic_load(&g_early_inflight), atomic_load(&g_early_wait_ns) / 1e6,
         (unsigned long long)bg, bg ? atomic_load(&g_as_bg_ns) / 1e6 / bg : 0.0,
         (unsigned long long)fg, fg ? atomic_load(&g_as_fg_ns) / 1e6 / fg : 0.0);
}
static int early_sort(early_job *j) {   // 1: new (j goes on to a build thread); 0: known already
    sha256_hex(j->buf, 0x880, j->buf + 0x880, j->length, j->key);   // the key vkb_shader_get will compute
    j->h = (uint32_t)strtoul(j->key + 56, NULL, 16) & 4095;
    pthread_mutex_lock(&g_mu);
    shentry *e = find_locked(j->key, j->h);
    pthread_mutex_unlock(&g_mu);
    if (e) { atomic_fetch_add(&g_early_known, 1); return 0; }
    return 1;
}
static void early_one(early_job *j) {   // a build thread: after early_sort said new
    const char *key = j->key;
    uint32_t h = j->h;
    pthread_mutex_lock(&g_mu);
    shentry *e = find_locked(key, h);
    pthread_mutex_unlock(&g_mu);
    if (e) { atomic_fetch_add(&g_early_known, 1); return; }
    shentry *n = calloc(1, sizeof *n);
    if (!n) return;
    memcpy(n->key, key, 65);
    n->sh.key = n->key;
    int compute = j->stage == 5;
    int ok = load(&n->sh, n->key, (int)j->stage, !compute);   // graphics: parsed only (pending 1), compiled below
    if (!ok) { free(n); atomic_fetch_add(&g_early_none, 1); return; }   // no translation: the draw logs it as before
    n->early = 1;
    pthread_mutex_lock(&g_mu);
    e = find_locked(key, h);
    if (!e) { n->next = g_tab[h]; g_tab[h] = n; g_loaded++; list_add(key, (int)j->stage); }
    pthread_mutex_unlock(&g_mu);
    if (e) {   // a draw or the warm-up loaded it meanwhile
        if (n->sh.obj) vk.vkDestroyShaderEXT(vkc.device, n->sh.obj, NULL);
        free(n->sh.spv); free(n);
        atomic_fetch_add(&g_early_known, 1);
        return;
    }
    atomic_fetch_add(&g_early_new, 1);
    if (!compute && async_claim(&n->sh)) async_compile(&n->sh, 1);   // else a draw took it meanwhile
}
static void *early_thread(void *unused) {
    (void)unused;
    pthread_setname_np("totk-shader-early");
    t_bg_compile = 1;
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    for (;;) {
        early_job *j = (early_job *)shq_pop(&g_ec);
        size_t n = j->q.bytes;
        early_one(j);
        free(j->buf); free(j);
        shq_done(&g_ec, n);
        early_log();
    }
    return NULL;
}
static void *early_sorter(void *unused) {   // [UIT 0146] no driver calls here (the NVIDIA thread limit is the builds')
    (void)unused;
    pthread_setname_np("totk-shader-sort");
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    for (;;) {
        early_job *j = (early_job *)shq_pop(&g_eq);
        size_t n = j->q.bytes;
        int fresh = early_sort(j);
        shq_done(&g_eq, n);   // its bytes move to the build queue (or are freed)
        if (fresh && shq_push(&g_ec, &j->q)) continue;   // the build thread frees it
        if (fresh) atomic_fetch_add(&g_early_bdropped, 1);
        free(j->buf); free(j);
        early_log();
    }
    return NULL;
}
static void early_init(void) {
    const char *e = getenv("TOTK_SHADER_EARLY");
    g_early = e && atoi(e) > 0;
    if (!g_early) return;
    int n = atoi(e) > 1 ? atoi(e) : 3;   // =N: N threads (the warm-up's 6 plus these stay under the 12 that crashed NVIDIA)
    if (n > 6) n = 6;
    for (int i = 0; i < n; i++) {
        pthread_t t;
        if (pthread_create(&t, NULL, early_thread, NULL) == 0) { pthread_detach(t); g_early_threads++; }
    }
    pthread_t st;
    if (g_early_threads && pthread_create(&st, NULL, early_sorter, NULL) == 0) pthread_detach(st);
    else g_early_threads = 0;   // threads already started wait on an empty queue
    if (!g_early_threads) { g_early = 0; VLOG("[shaders] early (TOTK_SHADER_EARLY): no thread could be started: off"); return; }
    VLOG("[shaders] early (TOTK_SHADER_EARLY=%s): shaders start compiling on %d background threads when the game hands them "
         "over; a draw that comes first waits for its shader (nothing is skipped)", e, g_early_threads);
}
int vkb_shader_early_on(void) {
    static pthread_once_t once = PTHREAD_ONCE_INIT;
    pthread_once(&once, early_init);
    return g_early > 0;
}
// nvnProgramSetShaders (hle_nvn.c, the game's thread): the code is copied now (its memory may be reused later).
void vkb_shader_early(const NVNshaderData *sd, int n) {
    if (!vkb_shader_early_on()) return;
    for (int i = 0; i < n && i < 6; i++) {
        const uint8_t *control = sd[i].control;
        if (!control || !sd[i].data) continue;
        uint32_t length, stage;
        memcpy(&length, control + 0x704, 4);
        memcpy(&stage, control + 0x714, 4);
        if (!length || length > (16u << 20) || (stage != 0 && stage != 1 && stage != 5)) continue;
        atomic_fetch_add(&g_early_seen, 1);
        static _Atomic uint64_t seen[4096];   // the same code handed over again: not copied and hashed again (a hint)
        if (shq_seen(seen, shq_tag((uint64_t)sd[i].data, (uint64_t)(uintptr_t)control, length))) { atomic_fetch_add(&g_early_known, 1); continue; }
        early_job *j = malloc(sizeof *j);
        uint8_t *buf = malloc(0x880 + (size_t)length);
        if (!j || !buf) { free(j); free(buf); continue; }
        memcpy(buf, control, 0x880);
        memcpy(buf + 0x880, (const void *)(uintptr_t)sd[i].data, length);
        j->q.bytes = 0x880 + (size_t)length; j->stage = stage; j->length = length; j->buf = buf;
        if (!shq_push(&g_eq, &j->q)) { free(j); free(buf); atomic_fetch_add(&g_early_dropped, 1); }
    }
}

const vkb_shader *vkb_shader_get(const NVNshaderData *sd) {
    const uint8_t *control = sd->control;
    if (!control || !sd->data) return NULL;
    uint32_t length, stage;
    memcpy(&length, control + 0x704, 4);
    memcpy(&stage, control + 0x714, 4);
    if (!length || length > (16u << 20) || (stage != 0 && stage != 1 && stage != 5)) return NULL;
    char key[65];
    sha256_hex(control, 0x880, (const void *)(uintptr_t)sd->data, length, key);
    vkb_bc7_prefetch_start();   // the textures' blocks into memory meanwhile (vkb_bc7.c; once, in the background)
    static pthread_once_t once = PTHREAD_ONCE_INIT;
    pthread_once(&once, prewarm_start);
    uint32_t h = (uint32_t)strtoul(key + 56, NULL, 16) & 4095;
    pthread_mutex_lock(&g_mu);
    shentry *e = g_tab[h];
    while (e && strcmp(e->key, key)) e = e->next;
    if (!e) {
        e = calloc(1, sizeof *e);
        if (!e) die("shader entry");
        memcpy(e->key, key, sizeof key);
        e->sh.key = e->key;
        uint64_t t0 = runtime_now_ns();
        int defer = stage != 5 && (vkb_shader_async_on() || vkb_shader_early_on());   // [UIT 0039] compute: at once, as before
        int ok = load(&e->sh, key, (int)stage, defer);
        __atomic_fetch_add(&vkb_shader_load_ns, runtime_now_ns() - t0, __ATOMIC_RELAXED);
        if (ok && defer && g_async > 0) async_queue(&e->sh);   // early only (no async threads): the draw compiles it, outside g_mu
        if (ok) {
            g_loaded++;
            list_add(key, (int)stage);
        }
        else {
            e->sh.obj = VK_NULL_HANDLE;
            if (g_missing++ < 16) VLOG("shader %s (stage %u): no translation in %s", key, stage, cache_dir());
        }
        e->next = g_tab[h];
        g_tab[h] = e;
    }
    // [UIT 0039] or still compiling: pending first (acquire), so obj is read only after a background compile published it
    int usable = __atomic_load_n(&e->sh.pending, __ATOMIC_ACQUIRE) || e->sh.obj;
    if (usable && !e->c1_set) {   // loaded now or ahead (prewarm): the constants come with the game's program
        e->c1_set = 1;
        if (e->prewarmed) g_prewarm_hits++;
        if (e->early) { atomic_fetch_add(vkb_shader_pending(&e->sh) ? &g_early_inflight : &g_early_ready, 1); early_log(); }
        uint32_t c1off;
        memcpy(&c1off, control + 0x700, 4);   // the shader's constants: the code's tail (c1)
        if (c1off < length) {   // copied: the program's memory may be freed and reused while the key stays cached
            uint8_t *c = malloc(length - c1off);
            if (!c) die("shader constants");
            memcpy(c, (const uint8_t *)(uintptr_t)sd->data + c1off, length - c1off);
            e->sh.c1 = c;
            e->sh.c1_size = length - c1off;
        }
    }
    pthread_mutex_unlock(&g_mu);
    return usable ? &e->sh : NULL;
}

void vkb_shader_stats(uint64_t *loaded, uint64_t *missing) { *loaded = g_loaded; *missing = g_missing; }
