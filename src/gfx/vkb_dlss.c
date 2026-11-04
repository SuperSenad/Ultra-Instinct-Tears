// DLSS in the replay (see src/vk/dlss.h): the game's own upscale draw (FSR 1 EASU, scene -> the 1920x1080 canvas,
// before the HUD) becomes camera motion vectors + DLSS + a blit into the canvas.
//
// What DLSS needs, and where it comes from (the method follows MacNativeGames' TOTK temporal upscaler,
// games/totk/runtime/totk_taa.m and nvn_mtl.m, MIT; this is a new Vulkan implementation):
//  - colour: the composed scene (shader 0ac9a549..., before FXAA), else the EASU draw's own source;
//  - depth: the G-buffer pass's D32 depth (the first pass with 3+ colour targets and a same-size D32 depth);
//  - motion: TOTK has none; made from depth and the camera (vertex uniform c1 of that first G-buffer draw: its
//    view-projection, near, far, fov) by src/vk/shaders/dlss_mv.comp; moving objects get no motion of their own;
//  - jitter: Halton (2, 3), added to the viewport of every draw on the G-buffer depth (no shader changes): the picture
//    moves by (jx, jy) render pixels; DLSS is told that same shift (vkb_dlss_math.h: the sign was checked against a
//    running sl.dlss by others and matches FSR 2's convention; TOTK_DLSS_JITTER_SIGN=-1 flips it for an A/B), and the
//    motion vectors are taken at the unjittered position (dlss_mv.comp, [UIT 0161]);
//  - exposure: DLSS's auto exposure for the HDR variant of the upscale shader.
//
// Threads (vkb_mt.c): the walk sees every draw in order before the chunks are recorded, possibly on several threads.
// The camera, the jitter phase and the reprojection are decided in the walk (role 1) and handed to the recording
// (role 0) through a small table keyed by the draw's payload; one thread (role 2) does both. What the draw state
// carries (vkb_dl_state) is updated identically by all of them, so each chunk starts with the right one.
//
// [UIT 0160] The walk has no images (it runs before the chunks are recorded, and images are made by the recording), so
// it checks the upscale draw by the textures' descriptors (sizes from the texture registry); the recording checks it
// again with the images. Before 0160 the walk required the source image, never had it, and so never accepted the
// draw: with more than one recording thread (the default from 4 processors) DLSS never ran and said nothing, while
// the walk still jittered the scene (the game's own upscale then showed the jitter as a shake). Now:
//  - every frame DLSS keeps the game's upscale has a reason (vkb_dlss_math.h DLR_*), logged once each and counted;
//  - the scene is jittered only while DLSS replaces the upscale (the last frame was accepted, no failure since);
//  - when the recording refuses a frame the walk accepted, the walk resets the history and waits ~5 s.
#include "vkb.h"
#include "../vk/dlss.h"
#include "../vk/dlss_mv_spv.h"
#include "vkb_mvprobe.h"
#include "vkb_dlss_math.h"

#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../runtime/logline.h"   // [UIT 0362] uit_logline: one write per line
#define DLOG(...) uit_logline("[totk-dlss] " __VA_ARGS__)

static const char K_EASU_HDR[] = "ac58fc18eda5f387", K_EASU[] = "a3a570eb159202f1", K_COMPOSE[] = "0ac9a54938974396";
enum { NVN_FMT_D32F = 0x34 };

int vkb_dl_on(void) { return dlss_available(); }
int vkb_dl_easu(const char *key) {
    if (!key) return 0;
    if (!strncmp(key, K_EASU_HDR, 16)) return 2;
    return !strncmp(key, K_EASU, 16);
}

static int env_int(const char *n, int def) { const char *e = getenv(n); return e && *e ? atoi(e) : def; }
// TOTK_DLSS_DEBUG_MV=1: a line every 300 frames (camera source, history resets and why, camera speed, jitter);
// 2: also shows the motion vectors in place of the DLSS picture (0.5 + motion / 32 px in red / green, see dlss_mv.comp)
static int debug_mv(void) { static int v = -1; if (v < 0) { v = env_int("TOTK_DLSS_DEBUG_MV", 0); if (v) DLOG("TOTK_DLSS_DEBUG_MV=%d", v); } return v; }

// [perf-mt] TOTK_DLSS_MV_PROBE=1: log the frame's render target sets on a few frames (vkb_mvprobe.h); log only
static int mv_probe(void) { static int v = -1; if (v < 0) { v = env_int("TOTK_DLSS_MV_PROBE", 0) == 1; if (v) DLOG("TOTK_DLSS_MV_PROBE=1: the render targets of frames 600, 601 and every 3600th are logged (log only)"); } return v; }
static mvp_state g_probe;
static pthread_mutex_t g_probe_mu = PTHREAD_MUTEX_INITIALIZER;
static void probe_draw(const vkb_dl_state *st, const vkb_dl_in *d) {
    const p_set_rt *rt = d->rt;
    mvp_rt r;
    memset(&r, 0, sizeof r);
    r.n = rt->n < 0 ? 0 : rt->n > 8 ? 8 : rt->n;
    for (int i = 0; i < r.n; i++) if (rt->color[i]) {
        const tex_t *t = rt->color[i];
        r.fmt[i] = (uint32_t)t->d.format; r.w[i] = t->d.width; r.h[i] = t->d.height; r.ser[i] = t->serial;
    }
    if (rt->depth) { r.dfmt = (uint32_t)rt->depth->d.format; r.dw = rt->depth->d.width; r.dh = rt->depth->d.height; r.dser = rt->depth->serial; }
    uint32_t sw = st->has_gdepth ? st->gdepth.d.width : 0, sh = st->has_gdepth ? st->gdepth.d.height : 0;
    char line[640], sum[256];
    pthread_mutex_lock(&g_probe_mu);
    int say = mvp_draw(&g_probe, vkb_frame(), &r, d->fs_key, sw, sh, line, sizeof line);
    int end = d->easu && mvp_frame_end(&g_probe, vkb_frame(), sum, sizeof sum);
    pthread_mutex_unlock(&g_probe_mu);
    if (say) DLOG("%s", line);
    if (end) DLOG("%s", sum);
}

// ---------------------------------------------------------------- walk -> recording
enum { R_GB = 1, R_EASU = 2, RING = 32 };
typedef struct {
    const uint8_t *a; uint64_t gen; int kind;
    int jit_on; float jx, jy;                                   // R_GB
    int ok, reset, why; float reproj[16], inv[16], nz, fz, fov; // R_EASU (why: DLR_*, 0 when ok)
    float ejx, ejy;
} ring_e;
static ring_e g_ring[RING];
static unsigned g_ring_next;
static pthread_mutex_t g_ring_mu = PTHREAD_MUTEX_INITIALIZER;
static void ring_put(const ring_e *e) {
    pthread_mutex_lock(&g_ring_mu);
    for (int i = 0; i < RING; i++) if (g_ring[i].a == e->a) g_ring[i].a = NULL;
    g_ring[g_ring_next++ % RING] = *e;
    pthread_mutex_unlock(&g_ring_mu);
}
static int ring_take(const uint8_t *a, int kind, ring_e *out) {
    int found = 0;
    pthread_mutex_lock(&g_ring_mu);
    for (int i = 0; i < RING; i++)
        if (g_ring[i].a == a && g_ring[i].kind == kind) { *out = g_ring[i]; g_ring[i].a = NULL; found = 1; break; }
    pthread_mutex_unlock(&g_ring_mu);
    return found;
}

// ---------------------------------------------------------------- the walk's camera and jitter (one thread)
static struct {
    double cam[84], prev[84]; int cam_ok, prev_ok;
    unsigned jit_i, phases; double jx, jy; int jit_now;
    int easu_live;          // [UIT 0160] the last upscale draw was accepted: the scene may be jittered
    uint64_t gen, last_easu_frame; int backoff;
    uint32_t last_cw, last_ch;
    // statistics (TOTK_DLSS_DEBUG_MV): where the camera came from (0 the submit-time copy, 1 not captured, 2 dropped by a
    // GPU write, 3 another slot: 1..3 read live memory, which may already hold a later frame's camera), resets by reason
    unsigned src[4], frames, rs_prev, rs_gap, rs_size, rs_cut, jit; double speed, speed_max;
    // [UIT 0162] the status line (always, every STATUS_EVERY upscale draws while DLSS is the upscaler)
    struct { unsigned draws, acc, jit, mv_n, rs[4]; double jmax, mvmax; float rjx, rjy; unsigned evals0, kept0[DLR_N]; } st;
} W;

// [UIT 0160] recording -> walk: the recording refused a frame the walk accepted (reset and wait), and the counts of
// the status line (vkb_dlss.c status_line)
static atomic_int g_rec_bad;
static atomic_uint g_evals, g_kept[DLR_N];
static float jitter_sign(void);
static int jitter_wanted(void);

// [UIT 0162] One line every STATUS_EVERY upscale draws (~30 s at 60 fps), from the walk, whatever TOTK_DLSS_DEBUG_MV says:
//   "[totk-dlss] status: 1800 upscale draws: DLSS 1800, kept 0 (-); accepted 1800, jittered 1800 (max |j| 0.49 px, 18
//    phases, told (0.21, -0.17)); camera motion: 1650 frames moving, max 37.2 px; history resets: start 1, gap 0, size 0,
//    cut 2"
// DLSS is evaluated with non-zero motion when "camera motion" counts frames, and with jitter when "jittered" does. The
// motion is the camera's at 20 units (dlm_motion_max), what the motion-vector pass writes for a still world.
enum { STATUS_EVERY = 1800 };
static void status_line(void) {
    if (++W.st.draws < STATUS_EVERY) return;
    unsigned ev = atomic_load(&g_evals), kept = 0;
    char ks[256] = "";
    size_t kl = 0;
    for (int r = 1; r < DLR_N; r++) {
        unsigned k = atomic_load(&g_kept[r]), dk = k - W.st.kept0[r];
        W.st.kept0[r] = k;
        if (!dk) continue;
        kept += dk;
        int w = snprintf(ks + kl, sizeof ks - kl, "%s%s %u", kl ? ", " : "", dlm_reason(r), dk);
        if (w > 0 && (size_t)w < sizeof ks - kl) kl += (size_t)w;
    }
    DLOG("status: %u upscale draws: DLSS %u, kept %u (%s); accepted %u, jittered %u (max |j| %.2f px, %u phases, told "
         "(%.3f, %.3f), jitter %s, sign %+d); camera motion: %u frames moving, max %.1f px; history resets: start %u, gap %u, "
         "size %u, cut %u", W.st.draws, ev - W.st.evals0, kept, kl ? ks : "-", W.st.acc, W.st.jit, W.st.jmax, W.phases,
         W.st.rjx, W.st.rjy, jitter_wanted() ? "on" : "off (TOTK_DLSS_JITTER=0)", (int)jitter_sign(), W.st.mv_n, W.st.mvmax,
         W.st.rs[0], W.st.rs[1], W.st.rs[2], W.st.rs[3]);
    W.st.evals0 = ev;
    W.st.draws = W.st.acc = W.st.jit = W.st.mv_n = 0;
    memset(W.st.rs, 0, sizeof W.st.rs);
    W.st.jmax = W.st.mvmax = 0;
}

static int camera_cut(void) {   // more than 8 units or 25 degrees in one frame
    double dx = W.cam[47] - W.prev[47], dy = W.cam[51] - W.prev[51], dz = W.cam[55] - W.prev[55];
    double f = W.cam[8] * W.prev[8] + W.cam[9] * W.prev[9] + W.cam[10] * W.prev[10];
    return sqrt(dx * dx + dy * dy + dz * dz) > 8.0 || f < cos(25.0 * M_PI / 180.0);
}

static int jitter_wanted(void) { static int v = -1; if (v < 0) v = env_int("TOTK_DLSS_JITTER", 1) != 0; return v; }
static float jitter_sign(void) { static int v; if (!v) v = env_int("TOTK_DLSS_JITTER_SIGN", 1) < 0 ? -1 : 1; return (float)v; }

static void walk_gb(const vkb_dl_in *d, ring_e *e) {   // the first G-buffer draw of a frame
    W.gen++;
    float c[84] = {0};
    W.cam_ok = 0;
    if (d->cam.addr && d->cam.size >= sizeof c) {
        vkb_ucap_read(d->cam.addr, c, sizeof c);
        int why = vkb_ucap_on() ? vkb_ucap_why(d->cam.addr, sizeof c) : 1;
        W.src[why & 3]++;
        if (why && vkb_ucap_on()) {
            static unsigned told;
            if (told++ == 0)
                DLOG("the camera of a frame was read from live memory (%s), not the submit-time copy: if this repeats, DLSS motion "
                     "can lag a frame behind the picture (TOTK_DLSS_DEBUG_MV=1 counts it)",
                     why == 1 ? "not captured" : why == 2 ? "rewritten by the GPU" : "another slot");
        }
        int fin = 1;
        for (int i = 0; i < 84; i++) { if (!isfinite(c[i])) fin = 0; W.cam[i] = c[i]; }
        W.cam_ok = fin && c[56] > 0 && c[57] > c[56] && c[62] > 0 && c[70] > 0;
    }
    if (!W.cam_ok && d->cam.addr) {
        static unsigned told;
        if (told++ == 0)
            DLOG("the G-buffer pass's camera uniform does not look like a camera (near %g, far %g, c62 %g, fov %g, %u bytes): "
                 "DLSS keeps the game's upscale on such frames", c[56], c[57], c[62], c[70], (unsigned)d->cam.size);
    }
    if (W.backoff > 0) W.backoff--;
    int bad = atomic_exchange(&g_rec_bad, 0);
    if (bad) { W.prev_ok = 0; W.easu_live = 0; if (W.backoff < 300) W.backoff = 300; }
    // jitter only while DLSS replaces the game's upscale: the last upscale draw was accepted
    W.jit_now = dlss_active() && jitter_wanted() && W.cam_ok && !W.backoff && W.easu_live;
    if (W.jit_now) {
        unsigned ph = W.phases ? W.phases : 16;
        W.jit_i = W.jit_i % ph + 1;
        dlm_jitter(W.jit_i, &W.jx, &W.jy);
    } else W.jx = W.jy = 0;
    e->kind = R_GB; e->jit_on = W.jit_now; e->jx = (float)W.jx; e->jy = (float)W.jy;
}

static void walk_easu(uint32_t cw, uint32_t ch, uint32_t ow, uint32_t oh, ring_e *e) {
    e->kind = R_EASU; e->ok = 0;
    uint64_t fr = vkb_frame();
    e->why = !dlss_active() ? DLR_OFF : !W.cam_ok ? DLR_CAMERA : W.backoff ? DLR_BACKOFF : 0;
    if (e->why) { W.prev_ok = 0; W.easu_live = 0; return; }
    W.phases = dlm_phases(cw, ch, ow, oh);
    int r_prev = !W.prev_ok, r_gap = !r_prev && fr > W.last_easu_frame + 2, r_size = !r_prev && (cw != W.last_cw || ch != W.last_ch);
    int r_cut = !r_prev && camera_cut();
    int reset = r_prev || r_gap || r_size || r_cut;
    if (debug_mv()) {
        W.frames++; W.rs_prev += r_prev; W.rs_gap += r_gap; W.rs_size += r_size; W.rs_cut += r_cut; W.jit += W.jit_now;
        if (!r_prev) {
            double dx = W.cam[47] - W.prev[47], dy = W.cam[51] - W.prev[51], dz = W.cam[55] - W.prev[55], v = sqrt(dx * dx + dy * dy + dz * dz);
            W.speed += v; if (v > W.speed_max) W.speed_max = v;
        }
        if (W.frames >= 300) {
            DLOG("[debug-mv] %u frames: camera from the copy %u, live (not captured %u, GPU-written %u, other slot %u); resets: "
                 "start %u, frame gap %u, size change %u, camera cut %u; camera %.3f units/frame (max %.3f); jitter on %u, "
                 "%u phases, scene %ux%u", W.frames, W.src[0], W.src[1], W.src[2], W.src[3], W.rs_prev, W.rs_gap, W.rs_size,
                 W.rs_cut, W.speed / W.frames, W.speed_max, W.jit, W.phases, cw, ch);
            memset(W.src, 0, sizeof W.src);
            W.frames = W.rs_prev = W.rs_gap = W.rs_size = W.rs_cut = W.jit = 0; W.speed = W.speed_max = 0;
        }
    }
    double vc[16], vp[16], ic[16], m[16], mi[16];
    for (int i = 0; i < 16; i++) { vc[i] = W.cam[12 + i]; vp[i] = reset ? W.cam[12 + i] : W.prev[12 + i]; }
    if (!dlm_inv4(vc, ic)) { W.prev_ok = 0; W.easu_live = 0; e->why = DLR_MATRIX; return; }
    dlm_mul4(vp, ic, m);
    if (!dlm_inv4(m, mi)) for (int i = 0; i < 16; i++) mi[i] = i % 5 == 0;
    for (int i = 0; i < 16; i++) { e->reproj[i] = (float)m[i]; e->inv[i] = (float)mi[i]; }
    e->nz = (float)W.cam[56]; e->fz = (float)W.cam[57];
    e->fov = (float)(2.0 * atan(1.0 / fabs(W.cam[17] != 0 ? W.cam[17] : 1.0)));   // from the projection's y scale when it is there
    if (W.cam[70] > 0 && W.cam[70] < 3.2) e->fov = (float)W.cam[70];
    e->reset = reset; e->ok = 1; W.easu_live = 1;
    e->ejx = W.jit_now ? (float)W.jx : 0; e->ejy = W.jit_now ? (float)W.jy : 0;
    // [UIT 0162] the status line's counts
    W.st.acc++; W.st.rs[0] += r_prev; W.st.rs[1] += r_gap; W.st.rs[2] += r_size; W.st.rs[3] += r_cut;
    if (W.jit_now) {
        W.st.jit++;
        double j = fmax(fabs(W.jx), fabs(W.jy));
        if (j > W.st.jmax) W.st.jmax = j;
        dlm_jitter_report(W.jx, W.jy, jitter_sign(), &W.st.rjx, &W.st.rjy);
    }
    if (!reset) {
        double mv = dlm_motion_max(m, cw, ch, dlm_depth_at(W.cam[56], W.cam[57], 20));
        if (mv > 0.01) W.st.mv_n++;
        if (mv > W.st.mvmax) W.st.mvmax = mv;
    }
    memcpy(W.prev, W.cam, sizeof W.cam); W.prev_ok = 1;
    W.last_easu_frame = fr; W.last_cw = cw; W.last_ch = ch;
}

void vkb_dl_failed(void) {   // from the recording: an evaluate failed; the walk stays off for ~5 s
    dlss_note_failure();
    W.backoff = 300;   // (a benign race with the walk thread: it only delays or shortens the back-off by a frame)
}

// ---------------------------------------------------------------- Vulkan objects (lazily, any recording thread)
typedef struct { VkImage img; VkDeviceMemory mem; VkImageView view; VkFormat fmt; VkImageUsageFlags usage; uint32_t w, h; } own_img;
enum { SETS = 16 };
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static int g_ready;                       // 0 not tried, 1 ready, -1 failed
static VkDescriptorSetLayout g_dsl;
static VkPipelineLayout g_pl;
static VkPipeline g_pipe;
static VkSampler g_samp;
static VkDescriptorPool g_pool;
static VkDescriptorSet g_sets[SETS];
static unsigned g_set_i;
static own_img g_mv, g_mask, g_dc[2], g_out;
static int g_dc_i, g_imgs_new;
// flags bit 0: the debug picture; jitter [UIT 0161]: this frame's viewport shift (render pixels, x right, y down)
typedef struct { float reproj[16]; int32_t size[2]; int32_t prev_valid, flags; float jitter[2]; } mv_push;
_Static_assert(sizeof(mv_push) == 88, "dlss_mv.comp push constants");

static uint32_t memory_type(uint32_t bits, VkMemoryPropertyFlags want) {
    for (uint32_t i = 0; i < vkc.memory.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (vkc.memory.memoryTypes[i].propertyFlags & want) == want) return i;
    return UINT32_MAX;
}
static void free_img(own_img *t) {
    if (t->img) vkb_destroy_later(t->view, t->img, t->mem);
    *t = (own_img){0};
}
static int make_img(own_img *t, uint32_t w, uint32_t h, VkFormat f, VkImageUsageFlags usage) {
    *t = (own_img){0};
    VkImageCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = f, .extent = {w, h, 1},
        .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = usage,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
    if (!vk_check(vk.vkCreateImage(vkc.device, &ci, NULL, &t->img), "DLSS image")) { t->img = VK_NULL_HANDLE; return 0; }
    VkMemoryRequirements req;
    vk.vkGetImageMemoryRequirements(vkc.device, t->img, &req);
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = req.size,
                               .memoryTypeIndex = memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)};
    if (ai.memoryTypeIndex == UINT32_MAX || !vk_check(vk.vkAllocateMemory(vkc.device, &ai, NULL, &t->mem), "DLSS memory")) {
        vk.vkDestroyImage(vkc.device, t->img, NULL); *t = (own_img){0}; return 0;
    }
    vk.vkBindImageMemory(vkc.device, t->img, t->mem, 0);
    VkImageViewCreateInfo vi = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = t->img, .viewType = VK_IMAGE_VIEW_TYPE_2D,
                                .format = f, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    if (!vk_check(vk.vkCreateImageView(vkc.device, &vi, NULL, &t->view), "DLSS view")) {
        vk.vkFreeMemory(vkc.device, t->mem, NULL); vk.vkDestroyImage(vkc.device, t->img, NULL); *t = (own_img){0}; return 0;
    }
    t->fmt = f; t->usage = usage; t->w = w; t->h = h;
    return 1;
}

static int init_objects(void) {
    VkDescriptorSetLayoutBinding b[5] = {{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL}};
    for (uint32_t i = 1; i < 5; i++) b[i] = (VkDescriptorSetLayoutBinding){i, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL};
    VkDescriptorSetLayoutCreateInfo di = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 5, .pBindings = b};
    if (!vk_check(vk.vkCreateDescriptorSetLayout(vkc.device, &di, NULL, &g_dsl), "DLSS set layout")) return 0;
    VkPushConstantRange pr = {VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(mv_push)};
    VkPipelineLayoutCreateInfo li = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &g_dsl,
                                     .pushConstantRangeCount = 1, .pPushConstantRanges = &pr};
    if (!vk_check(vk.vkCreatePipelineLayout(vkc.device, &li, NULL, &g_pl), "DLSS pipeline layout")) return 0;
    VkShaderModuleCreateInfo mi = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = sizeof dlss_mv_spv, .pCode = dlss_mv_spv};
    VkShaderModule m;
    if (!vk_check(vk.vkCreateShaderModule(vkc.device, &mi, NULL, &m), "DLSS shader module")) return 0;
    VkComputePipelineCreateInfo ci = {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = m, .pName = "main"},
        .layout = g_pl};
    int ok = vk_check(vk.vkCreateComputePipelines(vkc.device, VK_NULL_HANDLE, 1, &ci, NULL, &g_pipe), "DLSS pipeline");
    vk.vkDestroyShaderModule(vkc.device, m, NULL);
    if (!ok) return 0;
    VkSamplerCreateInfo si = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, .magFilter = VK_FILTER_NEAREST, .minFilter = VK_FILTER_NEAREST,
        .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .maxLod = 0};
    if (!vk_check(vk.vkCreateSampler(vkc.device, &si, NULL, &g_samp), "DLSS sampler")) return 0;
    VkDescriptorPoolSize ps[2] = {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, SETS}, {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, SETS * 4}};
    VkDescriptorPoolCreateInfo pi = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = SETS, .poolSizeCount = 2, .pPoolSizes = ps};
    if (!vk_check(vk.vkCreateDescriptorPool(vkc.device, &pi, NULL, &g_pool), "DLSS descriptor pool")) return 0;
    VkDescriptorSetLayout ls[SETS];
    for (int i = 0; i < SETS; i++) ls[i] = g_dsl;
    VkDescriptorSetAllocateInfo ai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = g_pool, .descriptorSetCount = SETS, .pSetLayouts = ls};
    return vk_check(vk.vkAllocateDescriptorSets(vkc.device, &ai, g_sets), "DLSS descriptor sets");
}

static int size_images(uint32_t cw, uint32_t ch, uint32_t ow, uint32_t oh) {
    if (g_mv.img && g_mv.w == cw && g_mv.h == ch && g_out.w == ow && g_out.h == oh) return 1;
    free_img(&g_mv); free_img(&g_mask); free_img(&g_dc[0]); free_img(&g_dc[1]); free_img(&g_out);
    const VkImageUsageFlags su = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    if (!make_img(&g_mv, cw, ch, VK_FORMAT_R16G16_SFLOAT, su | VK_IMAGE_USAGE_TRANSFER_SRC_BIT) || !make_img(&g_mask, cw, ch, VK_FORMAT_R16_SFLOAT, su) ||
        !make_img(&g_dc[0], cw, ch, VK_FORMAT_R32_SFLOAT, su) || !make_img(&g_dc[1], cw, ch, VK_FORMAT_R32_SFLOAT, su) ||
        !make_img(&g_out, ow, oh, VK_FORMAT_R16G16B16A16_SFLOAT, su | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT)) {
        free_img(&g_mv); free_img(&g_mask); free_img(&g_dc[0]); free_img(&g_dc[1]); free_img(&g_out);
        return 0;
    }
    g_imgs_new = 1;
    DLOG("DLSS images: scene %ux%u, output %ux%u (RGBA16F)", cw, ch, ow, oh);
    return 1;
}

static void img_barrier(VkCommandBuffer cb, VkImage img, VkImageLayout from) {
    VkImageMemoryBarrier2 b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2, .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
        .srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT, .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
        .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT, .oldLayout = from, .newLayout = VK_IMAGE_LAYOUT_GENERAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = img,
        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    VkDependencyInfo d = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &b};
    vk.vkCmdPipelineBarrier2(cb, &d);
}
static void mem_barrier(VkCommandBuffer cb) {   // everything before (this and earlier submissions) -> everything after
    VkMemoryBarrier2 b = {VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
        .srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT, .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
        .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT};
    VkDependencyInfo d = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &b};
    vk.vkCmdPipelineBarrier2(cb, &d);
}

static dlss_image as_dl(VkImage img, VkImageView view, VkDeviceMemory mem, VkFormat f, uint32_t w, uint32_t h, VkImageLayout l, VkImageUsageFlags u) {
    return (dlss_image){img, view, mem, f, w, h, l, u};
}
static dlss_image own_dl(const own_img *t) { return as_dl(t->img, t->view, t->mem, t->fmt, t->w, t->h, VK_IMAGE_LAYOUT_GENERAL, t->usage); }

// ---------------------------------------------------------------- the upscale draw
// The checks of MacNativeGames' easu_site: one colour target (level 0, layer 0), no blending, a 2D source, a full
// viewport, the constants' output size = the canvas, and a scene smaller than the canvas. 0 when it is the draw, else
// DLR_*. [UIT 0160] images: 0 in the walk (it has none; the sizes come from the textures' descriptors), 1 in the
// recording (the images themselves, *dst_out set).
static int easu_site(const vkb_dl_in *d, int images, vkb_image **dst_out, uint32_t *cw_out, uint32_t *ch_out,
                     uint32_t *ow_out, uint32_t *oh_out) {
    const p_set_rt *rt = d->rt;
    if (rt->n < 1 || !rt->color[0]) return DLR_TARGETS;
    for (int i = 1; i < rt->n && i < 8; i++) if (rt->color[i]) return DLR_TARGETS;
    if (d->blend || d->a2c || (rt->depth && d->ds_test)) return DLR_BLEND;
    const view_t *cv = &rt->cview[0];
    if (rt->has_cview[0] && ((cv->set_levels && cv->base_level) || (cv->set_layers && cv->min_layer))) return DLR_TARGETS;
    uint32_t sw = d->src_w, sh = d->src_h, dw = rt->color[0]->d.width, dh = rt->color[0]->d.height;
    vkb_image *dst = NULL;
    if (images) {
        vkb_image *src = d->src;
        dst = vkb_image_of(rt->color[0]);
        if (!src || !dst || src->depth > 1 || dst->depth > 1 || (src->aspect & ~VK_IMAGE_ASPECT_COLOR_BIT)) return DLR_TEXTURES;
        sw = src->width; sh = src->height; dw = dst->width; dh = dst->height;
    }
    if (!sw || !sh || !dw || !dh) return DLR_TEXTURES;
    if (d->nvp && !(d->vp[0] == 0 && d->vp[1] == 0 && d->vp[2] == (float)dw && d->vp[3] == (float)dh)) return DLR_VIEWPORT;
    if (d->has_sc && !(d->sc.x <= 0 && d->sc.y <= 0 && d->sc.x + d->sc.w >= (int32_t)dw && d->sc.y + d->sc.h >= (int32_t)dh)) return DLR_VIEWPORT;
    float k[20] = {0};
    if (!d->con.addr || d->con.size < sizeof k) return DLR_CONSTANTS;
    vkb_ucap_read(d->con.addr, k, sizeof k);
    uint32_t ow, oh;
    memcpy(&ow, &k[16], 4); memcpy(&oh, &k[17], 4);
    double fw = (double)k[0] * ow, fh = (double)k[1] * oh;
    uint32_t cw = (uint32_t)llround(fw), ch = (uint32_t)llround(fh);
    if (ow != dw || oh != dh || !cw || !ch || cw > sw || ch > sh || fabs(fw - cw) > 0.02 ||
        fabs(fh - ch) > 0.02 || fabsf(k[2] - (0.5f * k[0] - 0.5f)) > 1e-4f || fabsf(k[3] - (0.5f * k[1] - 0.5f)) > 1e-4f) return DLR_CONSTANTS;
    if (cw > dw || ch > dh || (cw == dw && ch == dh)) return DLR_TEXTURES;
    *dst_out = dst; *cw_out = cw; *ch_out = ch; *ow_out = dw; *oh_out = dh;
    return 0;
}

static int record_dlss(vkb_dl_state *st, const vkb_dl_in *d, const ring_e *e, vkb_image *dst, uint32_t cw, uint32_t ch) {
    vkb_image *depth = st->has_gdepth ? vkb_image_of(&st->gdepth) : NULL;
    if (!depth || depth->width < cw || depth->height < ch || depth->format != VK_FORMAT_D32_SFLOAT) return 0;
    vkb_image *color = d->src;
    if (st->has_comp) {   // the composed scene before FXAA, when it is the same picture
        vkb_image *c = vkb_image_of(&st->comp);
        if (c && c->width == color->width && c->height == color->height && c->format == color->format) color = c;
    }
    pthread_mutex_lock(&g_mu);
    if (!g_ready) g_ready = init_objects() ? 1 : -1;
    int ok = g_ready == 1 && size_images(cw, ch, dst->width, dst->height);
    if (!ok) { pthread_mutex_unlock(&g_mu); return 0; }
    VkCommandBuffer cb = vkb_cmd();
    VkImageView dview = vkb_view(depth, NULL, 0, VK_IMAGE_VIEW_TYPE_2D), cview = vkb_view(color, NULL, 0, VK_IMAGE_VIEW_TYPE_2D);
    if (!dview || !cview) { pthread_mutex_unlock(&g_mu); return 0; }
    vkb_pass_end_r(PE_OTHER);
    vkb_image_use(cb, depth, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_SHADER_READ_BIT);
    vkb_image_use(cb, color, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_SHADER_READ_BIT);
    if (g_imgs_new) {
        own_img *all[] = {&g_mv, &g_mask, &g_dc[0], &g_dc[1], &g_out};
        for (unsigned i = 0; i < 5; i++) img_barrier(cb, all[i]->img, VK_IMAGE_LAYOUT_UNDEFINED);
        g_imgs_new = 0;
    } else mem_barrier(cb);
    // motion vectors, this frame's depth copy and the reactive hint
    VkDescriptorSet set = g_sets[g_set_i++ % SETS];
    own_img *prev = &g_dc[g_dc_i ^ 1], *cur = &g_dc[g_dc_i];
    g_dc_i ^= 1;
    VkDescriptorImageInfo ii[5] = {{g_samp, dview, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}, {VK_NULL_HANDLE, prev->view, VK_IMAGE_LAYOUT_GENERAL},
        {VK_NULL_HANDLE, g_mv.view, VK_IMAGE_LAYOUT_GENERAL}, {VK_NULL_HANDLE, cur->view, VK_IMAGE_LAYOUT_GENERAL},
        {VK_NULL_HANDLE, g_mask.view, VK_IMAGE_LAYOUT_GENERAL}};
    VkWriteDescriptorSet w[5];
    for (uint32_t i = 0; i < 5; i++)
        w[i] = (VkWriteDescriptorSet){VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .dstBinding = i, .descriptorCount = 1,
            .descriptorType = i ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &ii[i]};
    vk.vkUpdateDescriptorSets(vkc.device, 5, w, 0, NULL);
    mv_push p = {.size = {(int32_t)cw, (int32_t)ch}, .prev_valid = !e->reset, .jitter = {e->ejx, e->ejy}};
    for (int c = 0; c < 4; c++) for (int r = 0; r < 4; r++) p.reproj[c * 4 + r] = e->reproj[r * 4 + c];   // GLSL mat4: column major
    vk.vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, g_pipe);
    vk.vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, g_pl, 0, 1, &set, 0, NULL);
    vk.vkCmdPushConstants(cb, g_pl, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof p, &p);
    vk.vkCmdDispatch(cb, (cw + 7) / 8, (ch + 7) / 8, 1);
    mem_barrier(cb);
    // DLSS
    dlss_frame f = {0};
    f.cb = cb; f.frame = (uint32_t)vkb_frame();
    f.color = as_dl(color->img, cview, color->mem, color->format, color->width, color->height, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, color->usage);
    f.depth = as_dl(depth->img, dview, depth->mem, depth->format, depth->width, depth->height, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, depth->usage);
    f.motion = own_dl(&g_mv); f.bias = own_dl(&g_mask); f.output = own_dl(&g_out);
    f.in_w = cw; f.in_h = ch;
    dlm_jitter_report(e->ejx, e->ejy, jitter_sign(), &f.jitter_x, &f.jitter_y);   // [UIT 0161] see vkb_dlss_math.h
    f.reset = e->reset; f.hdr = d->easu == 2;
    memcpy(f.clip_to_prev, e->reproj, sizeof f.clip_to_prev);
    memcpy(f.prev_to_clip, e->inv, sizeof f.prev_to_clip);
    f.near_z = e->nz; f.far_z = e->fz; f.fov_y = e->fov;
    ok = dlss_evaluate(&f);
    vkb_rec_invalidate();   // Streamline bound its own pipeline and descriptors
    if (ok && debug_mv() >= 2) {   // the motion vectors as a picture in place of the DLSS output
        mem_barrier(cb);
        p.flags = 1;
        vk.vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, g_pipe);
        vk.vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, g_pl, 0, 1, &set, 0, NULL);
        vk.vkCmdPushConstants(cb, g_pl, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof p, &p);
        vk.vkCmdDispatch(cb, (cw + 7) / 8, (ch + 7) / 8, 1);
        mem_barrier(cb);
        VkImageBlit vr = {{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {{0, 0, 0}, {(int32_t)cw, (int32_t)ch, 1}},
                          {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {{0, 0, 0}, {(int32_t)g_out.w, (int32_t)g_out.h, 1}}};
        vk.vkCmdBlitImage(cb, g_mv.img, VK_IMAGE_LAYOUT_GENERAL, g_out.img, VK_IMAGE_LAYOUT_GENERAL, 1, &vr, VK_FILTER_NEAREST);
        vkb_rec_invalidate();
    }
    if (ok) {
        mem_barrier(cb);
        vkb_image_use(cb, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
        VkImageBlit r = {{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {{0, 0, 0}, {(int32_t)g_out.w, (int32_t)g_out.h, 1}},
                         {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {{0, 0, 0}, {(int32_t)dst->width, (int32_t)dst->height, 1}}};
        vk.vkCmdBlitImage(cb, g_out.img, VK_IMAGE_LAYOUT_GENERAL, dst->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r, VK_FILTER_NEAREST);
        vkb_image_written_by(dst, VKB_W_UPSCALE, 0);
    }
    pthread_mutex_unlock(&g_mu);
    static atomic_uint said;
    if (ok && atomic_fetch_add(&said, 1) == 0)
        DLOG("DLSS replaces the game's upscale: scene %ux%u -> canvas %ux%u, %s, colour from %s", cw, ch, dst->width, dst->height,
             d->easu == 2 ? "HDR" : "display colour", color == d->src ? "the upscale's source" : "the composed scene (before FXAA)");
    return ok;
}

// ---------------------------------------------------------------- per draw
int vkb_dl_draw(vkb_dl_state *st, const vkb_dl_in *d) {
    const p_set_rt *rt = d->rt;
    int walk = d->role != 0, rec = d->role != 1;
    if (walk && mv_probe()) probe_draw(st, d);
    // the composed scene
    if (d->fs_key && !strncmp(d->fs_key, K_COMPOSE, 16) && rt->n >= 1 && rt->color[0]) { st->comp = *rt->color[0]; st->has_comp = 1; }
    // the first G-buffer draw of a frame
    if (!st->gb_seen && rt->depth && rt->n >= 3 && rt->depth->d.format == NVN_FMT_D32F && rt->color[0] &&
        rt->depth->d.width == rt->color[0]->d.width && rt->depth->d.height == rt->color[0]->d.height && rt->depth->d.width >= 320) {
        int nc = 0;
        for (int i = 0; i < rt->n && i < 8; i++) nc += rt->color[i] != NULL;
        if (nc >= 3) {
            st->gb_seen = 1; st->gdepth = *rt->depth; st->has_gdepth = 1;
            ring_e e = {0};
            if (walk) { walk_gb(d, &e); e.a = d->a; e.gen = W.gen; if (d->role == 1) ring_put(&e); }
            else if (!ring_take(d->a, R_GB, &e)) e.jit_on = 0;
            st->jit_on = e.jit_on; st->jx = e.jx; st->jy = e.jy;
        }
    }
    if (!d->easu) return 0;
    // the upscale draw: the frame's scene is over
    vkb_image *dst = NULL;
    uint32_t cw = 0, ch = 0, ow = 0, oh = 0;
    int why = st->has_gdepth ? easu_site(d, rec, &dst, &cw, &ch, &ow, &oh) : DLR_NO_DEPTH;
    ring_e e = {0};
    if (walk) {
        if (!why) walk_easu(cw, ch, ow, oh, &e);
        else { W.prev_ok = 0; W.easu_live = 0; e.why = why; }
        e.a = d->a; e.gen = W.gen; e.kind = R_EASU;
        if (d->role == 1) ring_put(&e);
        status_line();
    } else if (!ring_take(d->a, R_EASU, &e)) { e.ok = 0; e.why = DLR_UNPAIRED; }
    st->gb_seen = 0; st->jit_on = 0;
    if (!rec) return 0;
    int reason = why ? why : !e.ok ? (e.why ? e.why : DLR_UNPAIRED) : !dlss_active() ? DLR_OFF : 0;
    // the walk accepted what the recording cannot do (or the two lost each other): the frame was jittered for nothing, so
    // the walk resets the history and waits
    if ((why && e.ok) || (reason == DLR_UNPAIRED && dlss_active())) atomic_store(&g_rec_bad, 1);
    if (reason) {
        if (reason < DLR_N) {
            if (atomic_fetch_add(&g_kept[reason], 1) == 0)
                DLOG("the game's upscale draw kept (%s)%s", dlm_reason(reason),
                     why && e.ok ? ": the walk accepted the draw and the recording did not; DLSS waits a few seconds" : "");
        }
        return 0;
    }
    if (record_dlss(st, d, &e, dst, cw, ch)) { atomic_fetch_add(&g_evals, 1); return 1; }
    atomic_fetch_add(&g_kept[DLR_FAILED], 1);
    vkb_dl_failed();
    static atomic_uint fails;
    if (atomic_fetch_add(&fails, 1) < 4) DLOG("DLSS could not record this frame: the game's upscale for a few seconds");
    return 0;
}

// count: vertices x instances. A full-screen pass (3..6 vertices, one instance) is not jittered; instanced quads (grass,
// foliage cards) are: left unjittered they sat still while DLSS took them for jittered samples, a sub-pixel wobble.
int vkb_dl_jittered(const vkb_dl_state *st, const p_set_rt *rt, int count) {
    return st->jit_on && rt->depth && rt->depth->serial == st->gdepth.serial && count > 6;
}
