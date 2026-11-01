// NVN textures as Vulkan images (see vkb.h). Contents uploaded from game memory come with drawing (milestone 3.3
// step 4/5); here images exist for render targets, clears, copies and presentation.
// The NVN format table follows LASRecomp runtime/nvn/vulkan/textures.c (MIT, refs/LASRecomp): verified on a Switch
// game on Vulkan. Changes: ASTC, which desktop GPUs lack, becomes BC7 / BC1 from the cache made at build time
// (vkb_bc7.c), else RGBA8 decoded on upload; DEPTH24_STENCIL8 is D24S8 where the GPU can attach it, D32S8 otherwise
// (AMD, Steam Deck).
#include <math.h>
#include <time.h>
#include <stdlib.h>
#include <string.h>
#include <direct.h>

#include "compat/gfx_compat.h"
#include "host/hle.h"
#include "vkb.h"
#include "nvn/nvn_dirty.h"
#include "tex/astc.h"
#include "../host/uit_live.h"
#include "tex/bc7.h"
#include "tex/nvn_deswizzle.h"
#include "nvn/totk_vpool.h"
#include "vkb_audit.h"
#include "vkb_lock.h"
#include "uit_crit.h"   // [UIT 0232] uit_cp_on: the images lock statistics
#include "vkb_imgwb.h"
#include "vkb_mem.h"   // [UIT 0075]
#include "vkb_aside.h"
#include "vkb_pfor.h"
#include "vkb_cpuw.h"   // [UIT 0140]
#include "vkb_viewkey.h"   // [UIT 0200]
#include "vkb_imgidx.h"   // [UIT 0230]
#include <stddef.h>

#define VLOG(...) LOG("[vk] " __VA_ARGS__)

static int supports(VkFormat f, VkFormatFeatureFlags want) {
    VkFormatProperties p;
    vk.vkGetPhysicalDeviceFormatProperties(vkc.gpu, f, &p);
    return (p.optimalTilingFeatures & want) == want;
}

// Depth buffers are kept as 32-bit float (D16 and D24 too). TOTK classifies lighting tiles with an EQUAL depth test:
// one pass writes gl_FragDepth = material id, the lighting passes draw quads at the same z. In a UNORM depth buffer
// the two values reach the buffer through different conversions (fragment depth export and rasterised z), whose
// rounding may differ by one step on PC GPUs - the tiles failed the test and the world was unlit (2026-10-23, D16).
// Maxwell rounds both the same way; a float buffer stores both exactly. The precision only grows.
static VkFormat depth24_stencil8(void) {
    static VkFormat f;
    if (!f) {
        VkFormatFeatureFlags need = VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
                                    VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
        f = supports(VK_FORMAT_D32_SFLOAT_S8_UINT, need) ? VK_FORMAT_D32_SFLOAT_S8_UINT : VK_FORMAT_D24_UNORM_S8_UINT;
        VLOG("DEPTH24_STENCIL8 -> %s", f == VK_FORMAT_D24_UNORM_S8_UINT ? "D24_UNORM_S8_UINT" : "D32_SFLOAT_S8_UINT");
    }
    return f;
}

VkFormat vkb_format(int f, VkImageAspectFlags *aspect, int *astc) {
    if (aspect) *aspect = f == 0x31 ? VK_IMAGE_ASPECT_STENCIL_BIT
                        : f == 0x35 || f == 0x36 ? VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT
                        : f >= 0x32 && f <= 0x34 ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
    if (astc) *astc = f >= 0x79 && f <= 0x94;
    if (f >= 0x79 && f <= 0x86) return VK_FORMAT_R8G8B8A8_UNORM;   // ASTC LDR
    if (f >= 0x87 && f <= 0x94) return VK_FORMAT_R8G8B8A8_SRGB;    // ASTC sRGB
    switch (f) {
    case 0x01: return VK_FORMAT_R8_UNORM;            case 0x02: return VK_FORMAT_R8_SNORM;
    case 0x03: return VK_FORMAT_R8_UINT;             case 0x04: return VK_FORMAT_R8_SINT;
    case 0x05: return VK_FORMAT_R16_SFLOAT;          case 0x06: return VK_FORMAT_R16_UNORM;
    case 0x07: return VK_FORMAT_R16_SNORM;           case 0x08: return VK_FORMAT_R16_UINT;
    case 0x09: return VK_FORMAT_R16_SINT;            case 0x0A: return VK_FORMAT_R32_SFLOAT;
    case 0x0B: return VK_FORMAT_R32_UINT;            case 0x0C: return VK_FORMAT_R32_SINT;
    case 0x0D: return VK_FORMAT_R8G8_UNORM;          case 0x0E: return VK_FORMAT_R8G8_SNORM;
    case 0x0F: return VK_FORMAT_R8G8_UINT;           case 0x10: return VK_FORMAT_R8G8_SINT;
    case 0x11: return VK_FORMAT_R16G16_SFLOAT;       case 0x12: return VK_FORMAT_R16G16_UNORM;
    case 0x13: return VK_FORMAT_R16G16_SNORM;        case 0x14: return VK_FORMAT_R16G16_UINT;
    case 0x15: return VK_FORMAT_R16G16_SINT;         case 0x16: return VK_FORMAT_R32G32_SFLOAT;
    case 0x17: return VK_FORMAT_R32G32_UINT;         case 0x18: return VK_FORMAT_R32G32_SINT;
    case 0x19: return VK_FORMAT_R8G8B8A8_UNORM;      // RGB8: expanded on upload
    case 0x25: return VK_FORMAT_R8G8B8A8_UNORM;      case 0x26: return VK_FORMAT_R8G8B8A8_SNORM;
    case 0x27: return VK_FORMAT_R8G8B8A8_UINT;       case 0x28: return VK_FORMAT_R8G8B8A8_SINT;
    case 0x29: return VK_FORMAT_R16G16B16A16_SFLOAT; case 0x2A: return VK_FORMAT_R16G16B16A16_UNORM;
    case 0x2B: return VK_FORMAT_R16G16B16A16_SNORM;  case 0x2C: return VK_FORMAT_R16G16B16A16_UINT;
    case 0x2D: return VK_FORMAT_R16G16B16A16_SINT;   case 0x2E: return VK_FORMAT_R32G32B32A32_SFLOAT;
    case 0x2F: return VK_FORMAT_R32G32B32A32_UINT;   case 0x30: return VK_FORMAT_R32G32B32A32_SINT;
    case 0x31: return VK_FORMAT_S8_UINT;             case 0x32: return VK_FORMAT_D32_SFLOAT;   // D16: see depth24_stencil8
    case 0x33: return VK_FORMAT_D32_SFLOAT;          case 0x34: return VK_FORMAT_D32_SFLOAT;
    case 0x35: return depth24_stencil8();            case 0x36: return VK_FORMAT_D32_SFLOAT_S8_UINT;
    case 0x37: case 0x38: return VK_FORMAT_R8G8B8A8_SRGB;
    case 0x39: return VK_FORMAT_R4G4B4A4_UNORM_PACK16;
    case 0x3A: return VK_FORMAT_R5G6B5_UNORM_PACK16;
    case 0x3B: return VK_FORMAT_R5G5B5A1_UNORM_PACK16;
    case 0x3C: return VK_FORMAT_R5G6B5_UNORM_PACK16;
    case 0x3D: return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
    case 0x3E: return VK_FORMAT_A2B10G10R10_UINT_PACK32;
    case 0x3F: return VK_FORMAT_B10G11R11_UFLOAT_PACK32;
    case 0x40: return VK_FORMAT_E5B9G9R9_UFLOAT_PACK32;
    case 0x41: case 0x42: return VK_FORMAT_BC1_RGBA_UNORM_BLOCK;
    case 0x43: return VK_FORMAT_BC2_UNORM_BLOCK;     case 0x44: return VK_FORMAT_BC3_UNORM_BLOCK;
    case 0x45: case 0x46: return VK_FORMAT_BC1_RGBA_SRGB_BLOCK;
    case 0x47: return VK_FORMAT_BC2_SRGB_BLOCK;      case 0x48: return VK_FORMAT_BC3_SRGB_BLOCK;
    case 0x49: return VK_FORMAT_BC4_UNORM_BLOCK;     case 0x4A: return VK_FORMAT_BC4_SNORM_BLOCK;
    case 0x4B: return VK_FORMAT_BC5_UNORM_BLOCK;     case 0x4C: return VK_FORMAT_BC5_SNORM_BLOCK;
    case 0x4D: return VK_FORMAT_BC7_UNORM_BLOCK;     case 0x4E: return VK_FORMAT_BC7_SRGB_BLOCK;
    case 0x4F: return VK_FORMAT_BC6H_SFLOAT_BLOCK;   case 0x50: return VK_FORMAT_BC6H_UFLOAT_BLOCK;
    case 0x6D: return VK_FORMAT_R8G8B8A8_UNORM;      case 0x71: return VK_FORMAT_R16G16B16A16_SFLOAT;
    case 0x76: return VK_FORMAT_R32G32B32A32_SFLOAT;
    case 0x95: case 0x96: case 0x97: return VK_FORMAT_R5G5B5A1_UNORM_PACK16;
    case 0x98: case 0x99: return VK_FORMAT_B8G8R8A8_UNORM;
    case 0x9A: case 0x9B: return VK_FORMAT_B8G8R8A8_SRGB;
    default: return VK_FORMAT_UNDEFINED;
    }
}

// ---------------- images: one per memory and shape ----------------
// The game re-initialises texture objects over the same memory with the same shape (its render targets every frame,
// copies of objects, pool entries of finalised ones): on the console that is one texture that keeps its contents. Each
// serial used to get an image of its own: ~30 new images a frame on the Great Sky Island (~8 ms of the replay thread in
// vkCreateImage / vkAllocateMemory), each starting empty where the console kept the previous frame's contents
// (2026-10-24; SavageWind found the same in BotW). Serials now map to entries shared by memory and shape. An entry no
// serial holds stays aside for RECYCLE_FRAMES presents (the next frame's object takes it back, contents included) and is
// then destroyed once the GPU is done with it. Taken back after the CPU wrote its memory meanwhile, it uploads again.
#define VIEWS VKB_VIEWS_INLINE   // per texture: levels of a mip chain drawn one by one, layers, formats, shader types, streamed mips
typedef struct { VkImageView view[VKB_VIEW_BLOCK]; uint8_t key[VKB_VIEW_BLOCK][VKB_VIEW_KEY]; } view_blk;   // [UIT 0200] past VIEWS
typedef struct entry {
    vkb_image im; VkImageView view[VIEWS]; uint8_t view_key[VIEWS][VKB_VIEW_KEY]; int nviews;
    view_blk *vblk[VKB_VIEW_BLOCKS];        // [UIT 0200] written before the count that covers them is published
    int refs, shaped;                       // serials holding it; in the shape index (it has memory)
    uint64_t released, released_stamp;      // aside since this present / dirty-tracking generation
    uint64_t released_gpu;                  // [UIT 0139] GPU-write generation when it was set aside (nvn_gpu_write_now)
    uint8_t has_layout, layout[8];
    struct entry *shape_next, *all_next, **all_pprev;
    uint64_t seq;                           // [UIT 0230] creation order (g_all's order: newest first)
    int ix_all, ix_pend;                    // [UIT 0230] places in g_ix_all / g_ix_pend (vkb_imgidx.h), 0: not there
} entry;
_Static_assert(offsetof(entry, im) == 0, "an image's entry starts with it (ent_of)");
static inline struct entry *ent_of(vkb_image *im) { return (struct entry *)im; }
static inline VkImageView *view_at(entry *e, int i) {
    return i < VIEWS ? &e->view[i] : &e->vblk[(i - VIEWS) / VKB_VIEW_BLOCK]->view[(i - VIEWS) % VKB_VIEW_BLOCK];
}
static inline uint8_t *view_key_at(entry *e, int i) {
    return i < VIEWS ? e->view_key[i] : e->vblk[(i - VIEWS) / VKB_VIEW_BLOCK]->key[(i - VIEWS) % VKB_VIEW_BLOCK];
}
// [UIT 0200] TOTK_VK_VIEW_KEY (Advanced > Compatibility switches > Image view cache fix, default On): 0 = the key
// and the 32-view limit as before (A/B)
static int view_key_fixed(void) {
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("TOTK_VK_VIEW_KEY");
        v = !(e && *e == '0');
        VLOG("TOTK_VK_VIEW_KEY=%d: image views are cached by %s, up to %d per texture", v,
             v ? "the fields the view sets and its use" : "their raw bytes (as before 0200)", v ? VKB_VIEW_MAX : VIEWS);
    }
    return v;
}
static _Atomic uint64_t g_views_made, g_views_dropped;   // [UIT 0200] views created / not kept (cache full)
void vkb_view_stats(uint64_t *made, uint64_t *dropped) {
    *made = atomic_exchange(&g_views_made, 0); *dropped = atomic_exchange(&g_views_dropped, 0);
}
static int gpu_writes_on(void);   // [UIT 0139] (below)
static uint64_t g_tb_gpu;          // [UIT 0139] taken back and uploaded again after other images' GPU writes
// [UIT 0233] the serial -> entry table (vkb_sertab.h): the same table as before (serial 0: free; e NULL: removed),
// readable without the lock (TOTK_VK_IMAGE_LOOKUP_LOCKFREE)
#include "vkb_sertab.h"
static vst g_st;
static entry *g_all;                  // every entry (dumps, alias logs)
enum { SHAPES = 4096, RECYCLE_FRAMES = 120, RECYCLE_MAX = VKB_ASIDE_SPREAD };
static entry *g_shape[SHAPES];
static entry *g_aside[RECYCLE_MAX];
static int g_naside;
static int aside_cap(void) {   // [UIT 0075] TOTK_TEX_RELEASE_SPREAD (default on): 2048 kept, old ones a few per call (vkb_aside.h)
    static int spread = -1;
    if (spread < 0) { const char *e = getenv("TOTK_TEX_RELEASE_SPREAD"); spread = !(e && *e == '0'); }
    return spread ? VKB_ASIDE_SPREAD : VKB_ASIDE_OLD;
}
static uint64_t g_images_made, g_images_shared, g_images_taken_back;
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
// [UIT 0230] TOTK_VK_IMAGE_INDEX (default on; =0 the old walks of every image): the scans for images over a range and
// for pending copy-backs read dense arrays (vkb_imgidx.h) instead of walking g_all with the lock held. Kept up to date
// either way (a few stores per image made / dropped / pended); g_ix_bad (out of memory once): the old walks from then on.
static vkb_ix g_ix_all, g_ix_pend;   // (g_mu) entries with game memory / pending entries
static uint64_t g_entry_seq;         // (g_mu)
static int g_ix_bad;
static int ix_on(void) {
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("TOTK_VK_IMAGE_INDEX");
        on = !(e && e[0] == '0' && !e[1]);
        VLOG("[UIT 0230] image scans: %s (TOTK_VK_IMAGE_INDEX%s)", on ? "indexed (only the images over the range, or the pending ones)" : "every image, as before",
             on ? ", default; =0 as before" : "=0");
    }
    return on && !g_ix_bad;
}
// Scans of the image list with the lock held, per kind: count, entries looked at, time (the per-frame log line)
enum { SC_UPLOAD, SC_BUFREAD, SC_WANTED, SC_RECORD, SC_INVAL, SC_N };
static _Atomic uint64_t g_sc_n[SC_N], g_sc_vis[SC_N], g_sc_ns[SC_N];
void vkb_tex_scan_stats(uint64_t n[5], uint64_t vis[5], uint64_t ns[5]) {
    for (int k = 0; k < SC_N; k++) { n[k] = atomic_load(&g_sc_n[k]); vis[k] = atomic_load(&g_sc_vis[k]); ns[k] = atomic_load(&g_sc_ns[k]); }
}
// An iteration over the entries a scan needs: the index's matches (newest first, as g_all) or, without the index,
// g_all itself. One at a time (the match list is shared; g_mu held).
static void **g_q;
static int g_qcap;
typedef struct { int n, i, kind; uint64_t vis, t0; entry *cur; } eiter;
static entry *it_first(eiter *it, int kind, vkb_ix *x, uint64_t lo, uint64_t hi) {
    it->kind = kind; it->t0 = runtime_now_ns(); it->i = 0; it->vis = 0;
    it->n = ix_on() ? vkb_ix_query(x, lo, hi, &g_q, &g_qcap) : -1;
    if (it->n < 0 && ix_on()) { g_ix_bad = 1; VLOG("[UIT 0230] image index: out of memory, the old walks from now on"); }
    it->cur = it->n >= 0 ? (it->n ? (entry *)g_q[0] : NULL) : g_all;
    if (it->cur) it->vis++;
    return it->cur;
}
static entry *it_next(eiter *it) {
    it->cur = it->n >= 0 ? (++it->i < it->n ? (entry *)g_q[it->i] : NULL) : it->cur->all_next;
    if (it->cur) it->vis++;
    return it->cur;
}
static void it_end(const eiter *it) {   // (also when the loop stopped early)
    atomic_fetch_add_explicit(&g_sc_n[it->kind], 1, memory_order_relaxed);
    atomic_fetch_add_explicit(&g_sc_vis[it->kind], it->vis, memory_order_relaxed);
    atomic_fetch_add_explicit(&g_sc_ns[it->kind], runtime_now_ns() - it->t0, memory_order_relaxed);
}
// [UIT 0254] TOTK_VK_BUFREAD_RANGE (default on; =0 as before): the pending index keeps each pending image's memory
// [storage, storage + layout size) (the same size iwb_over tests), and the scans for pending images over one range
// (buffer reads, vkb_iwb_wanted, explicit copy-backs) ask the index for that range only: the entries met are those the
// old walk would not have skipped at its first test, in the same order (vkb_ix_query keeps the old walk's order), and
// each loop still applies its own test unchanged. Before, every one of those scans looked at every pending image and
// worked out its layout size, with the images lock held: in the 2026-11-01 Lookout Landing logs (ccbb7f2) 250-420 buffer
// reads a frame looked at ~57,000 images for the first ~1,500 frames after a load (lock held ~5 ms, waited ~10 ms a
// frame by the recording threads) and not one of them overlapped the read. Off: ranges 0 and every pending image
// looked at, as before. Also lets 0243 (TOTK_VK_BUFREAD_ACTIONABLE) skip from the first read (see vkb_iwb_buf_read).
static int bufrange_on(void) {
    static _Atomic int on = -1;
    int v = atomic_load_explicit(&on, memory_order_relaxed);
    if (v < 0) {
        const char *e = getenv("TOTK_VK_BUFREAD_RANGE");
        int want = !(e && e[0] == '0' && !e[1]), init = -1;
        if (atomic_compare_exchange_strong(&on, &init, want))
            VLOG("[UIT 0254] scans for pending GPU drawings over a range: %s (TOTK_VK_BUFREAD_RANGE%s)",
                 want ? "only the drawings over that range (index by address)" : "every pending drawing, as before",
                 want ? ", default; =0 as before" : "=0");
        v = atomic_load_explicit(&on, memory_order_relaxed);
    }
    return v;
}
// The index query range of a scan for pending images over [lo, hi): 0, 0 (every pending one) when off or empty
static void pend_q(uint64_t lo, uint64_t hi, uint64_t *qlo, uint64_t *qhi) {
    if (bufrange_on() && hi > lo) *qlo = lo, *qhi = hi; else *qlo = *qhi = 0;
}
static uint64_t iwb_size(const vkb_image *im);
static void ix_pend_add(vkb_image *im) {   // g_mu held: it became pending
    entry *e = ent_of(im);
    uint64_t lo = 0, hi = 0;
    if (bufrange_on()) { lo = (uint64_t)(uintptr_t)im->storage; hi = lo + iwb_size(im); if (hi < lo) hi = UINT64_MAX; }   // [UIT 0254]
    if (!e->ix_pend && !vkb_ix_add(&g_ix_pend, e, e->seq, lo, hi, &e->ix_pend) && !g_ix_bad) {
        g_ix_bad = 1; VLOG("[UIT 0230] image index: out of memory, the old walks from now on");
    }
}
static void ix_pend_del(vkb_image *im) { entry *e = ent_of(im); if (e->ix_pend) vkb_ix_del(&g_ix_pend, &e->ix_pend); }
// [UIT 0231] TOTK_VK_IMAGE_CREATE_UNLOCKED (default on; =0 as before): a new image (vkCreateImage and its memory) and a
// new image view are made without the images lock, which is taken again to put them in; when another thread put the
// same one in meanwhile, that one is used and ours goes (never used by the GPU). Before, the six recording threads
// waited behind every image made (0.1-0.3 ms each, 5 per frame while running: "new images" in the per-frame log).
static _Atomic uint64_t g_unl_images, g_unl_views, g_unl_lost;
static int unlocked_on(void) {
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("TOTK_VK_IMAGE_CREATE_UNLOCKED");
        on = !(e && e[0] == '0' && !e[1]);
        VLOG("[UIT 0231] new images and image views made %s (TOTK_VK_IMAGE_CREATE_UNLOCKED%s)", on ? "without the images lock" : "with the images lock held, as before",
             on ? ", default; =0 as before" : "=0");
    }
    return on;
}
void vkb_tex_unlocked_stats(uint64_t *images, uint64_t *views, uint64_t *lost) {
    *images = atomic_load(&g_unl_images); *views = atomic_load(&g_unl_views); *lost = atomic_load(&g_unl_lost);
}

// [UIT 0232] who takes the images lock, how often, how long it is held and waited for (TOTK_CRITPATH, the per-frame
// "images lock by site" line). Before, only the total wait was known (moving: 128.7 ms a frame over 800 waits, with
// the translation thread's largest chunk 25.9 ms). Acquisitions: per recording thread counters (game threads share
// counter 0: approximate); hold time: every 16th acquisition of a thread timed (x16 in the line); waits: every
// contended acquisition (as vkb_lock).
enum { IS_LOOKUP, IS_CREATE, IS_VIEW, IS_RELEASE, IS_UPLOAD, IS_REFRESH, IS_BUFREAD, IS_WANTED, IS_RECORD, IS_INVAL, IS_OTHER, IS_N };
static vkb_cnt g_is_acq[IS_N];
static _Atomic uint64_t g_is_wn[IS_N], g_is_wns[IS_N], g_is_hold[IS_N];
static __thread int t_is_site;
static __thread uint64_t t_is_t0;
static __thread unsigned t_is_k;
// lookups (vkb_image_of): the recording thread's cache, the lock-free table read (UIT 0233), with the lock: found, made
enum { LU_CACHE, LU_LOCKFREE, LU_LOCKFREE_MISS, LU_FOUND, LU_MADE, LU_N };
static vkb_cnt g_lu[LU_N];
static int img_stats_on(void) { return uit_cp_on(); }
static void img_lock(int site) {
    if (pthread_mutex_trylock(&g_mu)) {
        uint64_t t0 = runtime_now_ns();
        pthread_mutex_lock(&g_mu);
        uint64_t dt = runtime_now_ns() - t0;
        atomic_fetch_add_explicit(&vkb_lock_wait_ns[LK_IMAGES], dt, memory_order_relaxed);
        atomic_fetch_add_explicit(&vkb_lock_waits[LK_IMAGES], 1, memory_order_relaxed);
        atomic_fetch_add_explicit(&g_is_wns[site], dt, memory_order_relaxed);
        atomic_fetch_add_explicit(&g_is_wn[site], 1, memory_order_relaxed);
    }
    t_is_site = site;
    t_is_t0 = 0;
    if (img_stats_on()) {
        vkb_cnt_add(g_is_acq[site], 1);
        if (!(++t_is_k & 15)) t_is_t0 = runtime_now_ns();
    }
}
static void img_unlock(void) {
    if (t_is_t0) atomic_fetch_add_explicit(&g_is_hold[t_is_site], runtime_now_ns() - t_is_t0, memory_order_relaxed);
    t_is_t0 = 0;
    // [UIT 0235] the unlock itself: 0232 replaced every "pthread_mutex_unlock(&g_mu)" in this file with img_unlock(),
    // this one included, so img_unlock called itself (at -O2 a jump to itself: the first thread to let go of the images
    // lock spun forever holding it, and the game booted to a black screen, with the four 0230-0234 switches on or off)
    pthread_mutex_unlock(&g_mu);
}
// The per-frame line (backend_vk.c), over the last f frames. 0: nothing to say.
static vkb_cnt g_bq_skip, g_bq_walk;   // [UIT 0234] buffer reads: known to change nothing / looked at with the lock
static vkb_cnt g_bq_noact;             // [UIT 0243] buffer reads with nothing a walk could change (TOTK_VK_BUFREAD_ACTIONABLE)
int vkb_tex_lock_line(char *b, int cap, double f) {
    static const char *const nm[IS_N] = {"lookups", "new images", "views", "releases", "uploads", "refreshes", "buffer reads", "copy-back checks",
                                         "copy-back records", "invalidations", "logs"};
    static uint64_t pa[IS_N], pw[IS_N], pt[IS_N], ph[IS_N], pl[LU_N], pbs, pbw, pbn;
    if (!img_stats_on() || f <= 0) return 0;
    int o = 0;
    for (int k = 0; k < IS_N; k++) {
        uint64_t a = vkb_cnt_sum(g_is_acq[k]), w = atomic_load(&g_is_wn[k]), t = atomic_load(&g_is_wns[k]), h = atomic_load(&g_is_hold[k]);
        if (a != pa[k] || w != pw[k])
            o += snprintf(b + o, (size_t)(cap - o), "%s%s %.0f taken / %.0f waited %.2f ms / held %.2f ms", o ? ", " : "", nm[k], (a - pa[k]) / f,
                          (w - pw[k]) / f, (t - pt[k]) / 1e6 / f, (h - ph[k]) * 16 / 1e6 / f);
        if (o >= cap) o = cap - 1;
        pa[k] = a; pw[k] = w; pt[k] = t; ph[k] = h;
    }
    uint64_t l[LU_N];
    for (int k = 0; k < LU_N; k++) l[k] = vkb_cnt_sum(g_lu[k]);
    o += snprintf(b + o, (size_t)(cap - o), "; image lookups: thread cache %.0f, lock-free %.0f (fell back %.0f), with the lock found %.0f / made %.0f",
                  (l[0] - pl[0]) / f, (l[1] - pl[1]) / f, (l[2] - pl[2]) / f, (l[3] - pl[3]) / f, (l[4] - pl[4]) / f);
    if (o >= cap) o = cap - 1;
    for (int k = 0; k < LU_N; k++) pl[k] = l[k];
    uint64_t bs = vkb_cnt_sum(g_bq_skip), bw = vkb_cnt_sum(g_bq_walk), bn = vkb_cnt_sum(g_bq_noact);
    o += snprintf(b + o, (size_t)(cap - o), "; buffer reads over GPU drawings: known unchanged %.0f, nothing to change %.0f, looked at %.0f",
                  (bs - pbs) / f, (bn - pbn) / f, (bw - pbw) / f);
    if (o >= cap) o = cap - 1;
    pbs = bs; pbw = bw; pbn = bn;
    return o;
}

// [UIT 0233] TOTK_VK_IMAGE_LOOKUP_LOCKFREE (default on; =0 as before): a recording thread whose cache misses reads the
// table without the images lock first (vkb_sertab.h: exact, else the locked lookup as before). Old tables are kept
// until vkb_tex_quiesce (the start of the next submission's recording: no recording thread is reading then).
static int lockfree_on(void) {
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("TOTK_VK_IMAGE_LOOKUP_LOCKFREE");
        on = !(e && e[0] == '0' && !e[1]);
        VLOG("[UIT 0233] image lookups by the recording threads: %s (TOTK_VK_IMAGE_LOOKUP_LOCKFREE%s)",
             on ? "without the images lock when the image exists" : "with the images lock, as before", on ? ", default; =0 as before" : "=0");
    }
    return on;
}
static void table_reserve(void) {   // g_mu held: grows at half full, as before
    if (!vst_cap(&g_st)) g_st.keep_old = lockfree_on();   // before the first table: lock-free readers need old tables kept
    if (!vst_reserve(&g_st)) die("image table");
}
// [UIT 0233] vkb_mt_submit, before the prologue: no recording thread is reading (the last submission's are done, the
// next ones have not started): the old tables are freed.
static int bufmemo_on(void);
void vkb_tex_quiesce(void) {
    lockfree_on(); bufmemo_on();   // (read once here, before any recording thread asks)
    img_lock(IS_OTHER);
    vst_quiesce(&g_st);
    img_unlock();
}
static uint32_t shape_of(const void *mem, const nvn_texdesc *d) {
    uint64_t h = (uint64_t)(uintptr_t)mem * 0x9E3779B97F4A7C15ull;
    h ^= ((uint64_t)(uint32_t)d->format << 32 | d->width) * 0xC2B2AE3D27D4EB4Full;
    h ^= ((uint64_t)d->height << 32 | d->levels) * 0x165667B19E3779F9ull;
    return (uint32_t)(h >> 52) & (SHAPES - 1);
}
// [UIT 0124] TOTK_TEX_SHARE_SWIZZLE (default on): texture objects that differ only in their swizzle share one image
// (the swizzle is a sampling property: vkb_sample_swizzle gives each its own at view time). The map screen samples
// its map render target through an (R,G,B,1) descriptor of the same memory; a separate image for that descriptor
// held the memory's CPU-side bytes, never the GPU-drawn map: black. Off: such descriptors get images of their own.
int vkb_tex_share_swizzle(void) {
    static int on = -1;
    if (on < 0) { const char *e = getenv("TOTK_TEX_SHARE_SWIZZLE"); on = !(e && e[0] == '0' && !e[1]); }
    return on;
}
static int same_shape(const entry *e, const tex_t *t) {
    const nvn_texdesc *a = &e->im.d, *b = &t->d;
    return e->im.storage == t->mem && a->target == b->target && a->format == b->format && a->flags == b->flags &&
           a->samples == b->samples && a->width == b->width && a->height == b->height && a->depth == b->depth &&
           a->levels == b->levels && a->stride == b->stride && a->pkg_block_h == b->pkg_block_h &&
           (vkb_tex_share_swizzle() || (a->swizzle_r == b->swizzle_r && a->swizzle_g == b->swizzle_g &&
                                         a->swizzle_b == b->swizzle_b && a->swizzle_a == b->swizzle_a)) &&
           e->has_layout == t->has_layout && (!t->has_layout || !memcmp(e->layout, t->layout, sizeof e->layout));
}
// [UIT 0124] The view a texture object t samples im through: v (may be NULL), or, when im was made for another
// descriptor with another swizzle and v sets none, a copy of v in *tmp carrying t's own swizzle (descriptors built
// without SetDefaults have all-zero swizzles: identity, as vkb_view reads them).
const view_t *vkb_sample_swizzle(const vkb_image *im, const tex_t *t, const view_t *v, view_t *tmp) {
    if (!im || !t || (v && v->set_swizzle)) return v;
    const nvn_texdesc *a = &im->d, *b = &t->d;
    uint8_t sa[4] = {(uint8_t)a->swizzle_r, (uint8_t)a->swizzle_g, (uint8_t)a->swizzle_b, (uint8_t)a->swizzle_a};
    uint8_t sb[4] = {(uint8_t)b->swizzle_r, (uint8_t)b->swizzle_g, (uint8_t)b->swizzle_b, (uint8_t)b->swizzle_a};
    static const uint8_t id[4] = {NVN_TEXTURE_SWIZZLE_R, NVN_TEXTURE_SWIZZLE_G, NVN_TEXTURE_SWIZZLE_B, NVN_TEXTURE_SWIZZLE_A};
    if (!(sa[0] | sa[1] | sa[2] | sa[3])) memcpy(sa, id, 4);
    if (!(sb[0] | sb[1] | sb[2] | sb[3])) memcpy(sb, id, 4);
    if (!memcmp(sa, sb, 4)) return v;
    if (v) *tmp = *v; else memset(tmp, 0, sizeof *tmp);
    tmp->set_swizzle = 1;
    memcpy(tmp->swz, sb, 4);
    static uint64_t said[8];   // the first few texture objects (each once)
    for (int i = 0; i < 8; i++) {
        if (said[i] == t->serial) break;
        if (said[i]) continue;
        said[i] = t->serial;
        VLOG("[UIT 0124] texture %llu samples image %llu (same memory, format and size) through its own swizzle %u%u%u%u (image's %u%u%u%u)",
             (unsigned long long)t->serial, (unsigned long long)im->serial, sb[0], sb[1], sb[2], sb[3], sa[0], sa[1], sa[2], sa[3]);
        break;
    }
    return tmp;
}
static void stream_cancel(vkb_image *im);
static void iwb_forget(vkb_image *im);
static int iwb_eligible(const tex_t *t, const vkb_image *im);
static int iwb_fit(const tex_t *t, const vkb_image *im);
// [UIT 0056/0057] images first GPU-drawn (the read cache). Not bumped when an image is made (not drawn yet, so no
// answer changes) or dropped (a cached answer then only lists one image too many: one more vkb_iwb_wanted look).
static _Atomic uint64_t g_iwb_img_gen;
// [UIT native-1031] g_iwb_eligible counts every image made eligible since the start (it only grows: 53 -> 12,023 in the
// 86-minute 2026-10-30 log was images made, not images kept); g_iwb_live is the ones that exist now (entry_drop).
static uint64_t g_iwb_live;   // (g_mu)
// [UIT 0137] TOTK_VK_ALIAS_COPYBACK (default on; =0 off): see tex_upload's alias scan and vkb_iwb_record
static _Atomic int g_iwb_alias_want;   // an image newly marked wb_alias was already pending: copy it at the next submission
static uint64_t g_iwb_alias_marked, g_iwb_alias_copies;   // (g_mu)
// [UIT 0255] TOTK_VK_READBACK_AHEAD (on; =0 as before): an image the game's CPU has read back (an explicit read: it
// invalidates memory the GPU drew, mtl_pool_invalidate) is copied back at the end of every later submission that draws
// it, as TOTK_VK_ALIAS_COPYBACK's images. Before, each such read went through the translation thread (nvn_x_call):
// everything queued meanwhile replayed first (the next frame's work), then a copy submission of its own and a wait for
// the GPU behind all of that: 2026-11-01 Lookout Landing logs, 1.5 reads a frame (three 1x1 images, the same each
// time), up to 29 ms each, in most long frames with "queue drain waits". Now the copy lands with the drawing
// submission (the retire thread, before that submission's fence is reached), so the game's fence wait already
// covers it and the read finds nothing pending. What the game reads: the image as of the last submission that drew
// it and has completed - as on the console; before, also the drawings of later queued submissions. At most
// IWB_SMALL_MAX bytes an image and READAHEAD_MAX images (the copy per drawing submission is tiny for these); larger
// ones and the rest: explicit reads as before.
enum { READAHEAD_MAX = 256 };
static uint64_t g_iwb_hot_n, g_iwb_hot_copies;   // (g_mu)
static int readahead_on(void);
void vkb_iwb_ahead_stats(uint64_t *n, uint64_t *copies) {   // [UIT 0255] for the [iwb] line (racy read: a log)
    *n = __atomic_load_n(&g_iwb_hot_n, __ATOMIC_RELAXED); *copies = __atomic_load_n(&g_iwb_hot_copies, __ATOMIC_RELAXED);
}
static int readahead_on(void) {
    static _Atomic int on = -1;
    int v = atomic_load_explicit(&on, memory_order_relaxed);
    if (v < 0) {
        const char *e = getenv("TOTK_VK_READBACK_AHEAD");
        int want = !(e && e[0] == '0' && !e[1]), init = -1;
        if (atomic_compare_exchange_strong(&on, &init, want))
            VLOG("[UIT 0255] images the CPU reads back: %s (TOTK_VK_READBACK_AHEAD%s)",
                 want ? "copied back with each submission that draws them (small ones)" : "copied back when read, after the queue and the GPU, as before",
                 want ? "=0 for that" : "=0");
        v = atomic_load_explicit(&on, memory_order_relaxed);
    }
    return v;
}
static int alias_copyback_on(void) {
    static int on = -1;
    if (on < 0) { const char *e = getenv("TOTK_VK_ALIAS_COPYBACK"); on = !(e && e[0] == '0' && !e[1]); }
    return on;
}
// [UIT 0153] TOTK_VK_ALIAS_RELEASED=1 (off by default): TOTK_VK_ALIAS_COPYBACK for released writers too. A GPU-drawn
// tile the game has let go (aside, refs 0) whose memory a new texture of another shape reads: its last drawing, never
// copied back (game memory holds what was there before), is copied back at the next submission like a live writer's,
// and the reader uploads again when it lands (TOTK_TEX_GPU_WRITES). 2026-10-31 cc3add4 field log: all 32 [alias] lines
// name released writers ("writer: released, copy back possible"), which 0137 skipped.
static int alias_released_on(void) {
    static int on = -1;
    if (on < 0) { const char *e = getenv("TOTK_VK_ALIAS_RELEASED"); on = e && e[0] == '1' && !e[1]; if (on) VLOG("[alias] released GPU-drawn images read by textures of another shape are copied back too (TOTK_VK_ALIAS_RELEASED=1)"); }
    return on;
}
// [UIT 0155] TOTK_VK_GPU_MEM_COHERENT (default on; =0 off, for comparison): game memory gets what the GPU drew there,
// as on the Switch, wherever something other than the drawn image itself may read it:
//  - released: a GPU-drawn image (copy-back eligible, at most IWB_REL_MAX) the game lets go whose last drawing is not
//    in game memory yet is copied back at the end of the submission (once: wb_rel, undone when it is taken back). On the
//    Switch its memory holds that drawing from then on; here a texture or buffer the game later places over the same
//    memory (the terrain tile pool re-places 260x260 tiles at shifted addresses) read what was there before the GPU
//    drew. Landing marks the pages GPU-written, so a texture already uploaded over them uploads again, and drawings
//    newer than it (write_seq) supersede it (vkb_iwb_record), so an older drawing never lands over a newer one.
//  - textures: TOTK_VK_ALIAS_COPYBACK for released writers too (TOTK_VK_ALIAS_RELEASED's behaviour).
//  - buffers: a vertex, index, uniform, storage or copy-source range over a GPU-drawn image's memory whose drawing is
//    not copied back (vkb_iwb_buf_read, from vkb_gpu_addr) marks that image like a texture reader does, and the copy
//    back goes into the pool's device copy too (TOTK_IWB_POOL_COPY's behaviour), where buffers are read from. Logged
//    as "[alias] buffer ..." (first 32) whether or not this is on.
// Same-submission limit: a reader recorded in the very submission that drew the image sees the drawing one
// submission later (the copy is made at its end); the GPU-side reinterpretation that would avoid that needs
// block-linear swizzle shaders (not done).
static int coherent_on(void) {
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("TOTK_VK_GPU_MEM_COHERENT");
        on = !(e && e[0] == '0' && !e[1]);
        VLOG("[alias] GPU drawings copied back when the game lets an image go or a buffer reads its memory: %s (TOTK_VK_GPU_MEM_COHERENT%s)",
             on ? "on" : "off", on ? ", default; =0 off" : "=0");
    }
    return on;
}
int vkb_gpu_coherent(void) { return coherent_on(); }
static int released_ok(void) { return alias_released_on() || coherent_on(); }
enum { IWB_REL_MAX = 1 << 20 };   // released images larger than this are not copied back when let go (big transient targets
                                  // taken back every frame would pay a copy and, through 0139, an upload each time; a terrain tile is 210 KiB)
static uint64_t g_iwb_rel_marked, g_iwb_rel_unmarked, g_iwb_buf_marked;   // (g_mu)
// Chunks (1 MiB) of game memory under pending drawings: the quick test of vkb_iwb_buf_read (counts, hashed; a
// collision only costs the exact look)
enum { BIDX_N = 4096 };
static _Atomic uint32_t g_bidx[BIDX_N];
static _Atomic uint64_t g_bidx_gen;
static unsigned bidx_h(uint64_t chunk) { return (unsigned)((chunk * 0x9E3779B97F4A7C15ull) >> 52) & (BIDX_N - 1); }
static uint64_t iwb_size(const vkb_image *im);
static void bidx_mod(const vkb_image *im, int d) {   // g_mu held
    uint64_t lo = (uint64_t)(uintptr_t)im->storage, sz = iwb_size(im);
    atomic_fetch_add(&g_bidx_gen, 1);   // (whatever the size: [UIT 0234] a drawing became pending / not)
    if (!lo || !sz) return;
    for (uint64_t c = lo >> 20; c <= (lo + sz - 1) >> 20; c++) atomic_fetch_add(&g_bidx[bidx_h(c)], (uint32_t)d);
}
// [UIT 0243] TOTK_VK_BUFREAD_ACTIONABLE (default on; =0 as before): buffer reads look only where a pending drawing could
// still be marked or cleared (vkb_bufact.h); bact_fix after every change of wb_pending / wb_alias / wb_rel (g_mu held).
#include "vkb_bufact.h"
static bact_tab g_bact;
static _Atomic int g_bq_said;   // the "[alias] buffer" log lines so far (written with g_mu held)
static int bufact_on(void) {
    static _Atomic int on = -1;
    int v = atomic_load_explicit(&on, memory_order_relaxed);
    if (v < 0) {
        const char *e = getenv("TOTK_VK_BUFREAD_ACTIONABLE");
        int want = !(e && e[0] == '0' && !e[1]), init = -1;
        if (atomic_compare_exchange_strong(&on, &init, want))
            VLOG("[UIT 0243] buffer reads over GPU drawings: %s (TOTK_VK_BUFREAD_ACTIONABLE%s)",
                 want ? "looked at only where a drawing could still be marked or cleared" : "looked at wherever a drawing is pending, as before",
                 want ? ", default; =0 as before" : "=0");
        v = atomic_load_explicit(&on, memory_order_relaxed);
    }
    return v;
}
static int coherent_on(void);
static void bact_fix(vkb_image *im) {   // g_mu held
    if (!im->wb_pending && !im->wb_act) return;
    uint64_t sz = iwb_size(im);
    int want = bufact_on() && bact_want(coherent_on(), im->wb_pending, im->wb_alias, im->wb_rel, sz, IWB_EXPLICIT_MAX);
    bact_set(&g_bact, &im->wb_act, want, (uint64_t)(uintptr_t)im->storage, sz);
}
static void bact_drop(vkb_image *im) {  // g_mu held: the entry goes
    if (im->wb_act) bact_set(&g_bact, &im->wb_act, 0, (uint64_t)(uintptr_t)im->storage, iwb_size(im));
}
static void entry_drop(entry *e) {   // g_mu held: out of the indexes, destroyed when the GPU is done with it
    stream_cancel(&e->im);   // its larger mips no longer wanted (waits for a level being made)
    if (e->shaped) {
        entry **p = &g_shape[shape_of(e->im.storage, &e->im.d)];
        while (*p && *p != e) p = &(*p)->shape_next;
        if (*p) *p = e->shape_next;
    }
    *e->all_pprev = e->all_next;
    if (e->all_next) e->all_next->all_pprev = e->all_pprev;
    iwb_forget(&e->im);   // [UIT 0054] out of the copy-back lists
    bact_drop(&e->im);    // [UIT 0243]
    if (e->ix_all) vkb_ix_del(&g_ix_all, &e->ix_all);   // [UIT 0230]
    ix_pend_del(&e->im);
    if (e->im.wb_ok && g_iwb_live) g_iwb_live--;   // [UIT native-1031] (wb_ok is set only by iwb_eligible)
    for (int i = 0; i < e->nviews; i++) vkb_destroy_later(*view_at(e, i), VK_NULL_HANDLE, VK_NULL_HANDLE);
    for (int b = 0; b < VKB_VIEW_BLOCKS; b++) free(e->vblk[b]);
    vkb_destroy_image_later(e->im.img, e->im.mem, e->im.alloc);
    free(e);
}
static void aside_purge(void) {   // g_mu held: entries aside too long (or too many) go
    uint64_t now = vkb_frame();
    int cap = aside_cap(), budget = cap == VKB_ASIDE_SPREAD ? VKB_ASIDE_PER_CALL : -1;
    while (vkb_aside_go(g_naside, cap, g_naside && g_aside[0]->released + RECYCLE_FRAMES < now, &budget)) {
        entry *e = g_aside[0];
        memmove(g_aside, g_aside + 1, (size_t)--g_naside * sizeof g_aside[0]);
        entry_drop(e);
    }
}
void vkb_tex_image_stats(uint64_t *made, uint64_t *shared, uint64_t *taken_back) {
    *made = g_images_made; *shared = g_images_shared; *taken_back = g_images_taken_back;
}

static int create(vkb_image *im, const tex_t *t) {
    const nvn_texdesc *d = &t->d;
    VkImageAspectFlags aspect;
    int astc;
    VkFormat f = vkb_format(d->format, &aspect, &astc);
    if (f == VK_FORMAT_UNDEFINED || d->target == NVN_TEXTURE_TARGET_BUFFER) return 0;
    nvn_texlayout L;
    nvn_tex_layout(d, &L);
    VkImageType type = d->target == NVN_TEXTURE_TARGET_3D ? VK_IMAGE_TYPE_3D
                     : d->target == NVN_TEXTURE_TARGET_1D || d->target == NVN_TEXTURE_TARGET_1D_ARRAY ? VK_IMAGE_TYPE_1D : VK_IMAGE_TYPE_2D;
    uint32_t w = d->width ? d->width : 1, h = type == VK_IMAGE_TYPE_1D ? 1 : (d->height ? d->height : 1);
    uint32_t depth = type == VK_IMAGE_TYPE_3D ? (d->depth ? d->depth : 1) : 1, layers = type == VK_IMAGE_TYPE_3D ? 1 : (L.layers ? L.layers : 1);
    uint32_t levels = d->levels ? d->levels : 1;
    int ms = d->target == NVN_TEXTURE_TARGET_2D_MULTISAMPLE || d->target == NVN_TEXTURE_TARGET_2D_MULTISAMPLE_ARRAY;
    VkImageUsageFlags usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    VkFormatFeatureFlags feat = 0;
    { VkFormatProperties p; vk.vkGetPhysicalDeviceFormatProperties(vkc.gpu, f, &p); feat = p.optimalTilingFeatures; }
    if (aspect & (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)) {
        if (feat & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) usage |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    } else if (!astc) {
        if (feat & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT) usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        if (feat & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) usage |= VK_IMAGE_USAGE_STORAGE_BIT;
    }
    // a target may be sampled while bound (backend_vk.c pass_begin)
    if (vkc.has_feedback_loop && (usage & (VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT)))
        usage |= VK_IMAGE_USAGE_ATTACHMENT_FEEDBACK_LOOP_BIT_EXT;
    VkImageCreateFlags flags = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT | VK_IMAGE_CREATE_EXTENDED_USAGE_BIT;   // NVN views may reinterpret the format
    int compressed = !astc && f >= VK_FORMAT_BC1_RGB_UNORM_BLOCK && f <= VK_FORMAT_BC7_SRGB_BLOCK;
    if (compressed) flags |= VK_IMAGE_CREATE_BLOCK_TEXEL_VIEW_COMPATIBLE_BIT;   // read a block as RGBA16UI / RG32UI (TOTK's GPU BC encoder)
    if (type == VK_IMAGE_TYPE_2D && !ms && layers >= 6 && w == h) flags |= VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
    if (type == VK_IMAGE_TYPE_3D) flags |= VK_IMAGE_CREATE_2D_ARRAY_COMPATIBLE_BIT;
    // [UIT 0125] BC textures as render targets: TOTK's GPU BC4 encoder (agl) draws a BC texture's blocks through an
    // uncompressed view of the same bytes per block (RGBA16UI for BC4: one texel per 8-byte block; for a 3D texture
    // one depth plane per pass). Vulkan renders such views of a BLOCK_TEXEL_VIEW_COMPATIBLE image when the image
    // carries the COLOR_ATTACHMENT usage, which EXTENDED_USAGE allows on a BC format; kept only when the device takes
    // that combination for this shape. Without it these draws were dropped ("not attachable (compressed)").
    // TOTK_VK_BC_TARGETS=0: as before. (Approach as in the MIT-licensed reference runtime/vk_texture.c block_views_ok.)
    static int bc_targets = -1;
    if (bc_targets < 0) { const char *e = getenv("TOTK_VK_BC_TARGETS"); bc_targets = !(e && e[0] == '0' && !e[1]); }
    if (compressed && bc_targets && !ms) {
        VkImageFormatProperties q;
        VkImageUsageFlags bu = usage | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;   // flags: BLOCK_TEXEL_VIEW_COMPATIBLE, EXTENDED_USAGE (3D: 2D_ARRAY_COMPATIBLE)
        if (vk.vkGetPhysicalDeviceImageFormatProperties(vkc.gpu, f, type, VK_IMAGE_TILING_OPTIMAL, bu, flags, &q) == VK_SUCCESS &&
            w <= q.maxExtent.width && h <= q.maxExtent.height && depth <= q.maxExtent.depth && levels <= q.maxMipLevels &&
            layers <= q.maxArrayLayers)
            usage = bu;
    }
    VkImageCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .flags = flags, .imageType = type, .format = f,
        .extent = {w, h, depth}, .mipLevels = levels, .arrayLayers = layers,
        .samples = ms && d->samples > 1 ? (VkSampleCountFlagBits)d->samples : VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = usage, .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
    VkResult r = vk.vkCreateImage(vkc.device, &ci, NULL, &im->img);
    if (r != VK_SUCCESS) {
        static int said;
        if (said++ < 16) VLOG("texture %llu (format %#x target %d %ux%ux%u, %u levels, %u layers): vkCreateImage %d",
                              (unsigned long long)t->serial, d->format, d->target, w, h, depth, levels, layers, (int)r);
        return 0;
    }
    // [UIT 0075] a place in a shared block (TOTK_VK_SUBALLOC), else one allocation per texture as before (vkb_mem.c);
    // [UIT 0130] render targets, depth and storage images always the latter unless TOTK_VK_SUBALLOC_TARGETS=1
    if (!vkb_mem_image(im->img, usage, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &im->mem, &im->alloc)) {   // [UIT 0130] usage
        vk.vkDestroyImage(vkc.device, im->img, NULL);
        return 0;
    }
    im->serial = t->serial; im->d = *d; im->format = f; im->aspect = aspect; im->layout = VK_IMAGE_LAYOUT_UNDEFINED;
    im->width = w; im->height = h; im->depth = depth; im->layers = layers; im->levels = levels; im->astc = astc;
    im->storage = t->mem;
    im->usage = usage;
    im->compressed = compressed;
    return 1;
}

// ---- one memory, several sizes: TOTK's dynamic resolution re-creates its render targets at another size over the same
// memory (1600x896 <-> 1432x808 on the Great Sky Island when the camera turns to the sky and back). On the console the
// new objects see what was rendered last through the old ones; here each size is an image of its own, and coming back
// to a size took back its image as it was frames earlier: depth from when the camera faced the sky, so the sea of
// clouds was drawn over the whole scene, and the lighting of the title menu read another shot's targets - the scene went
// dark for 4-6 frames (2026-10-24). As GPU emulators' texture caches do, an image that a texture object starts using
// takes the contents of the image last written at the same memory in the same format but another size, scaled
// (vkb_tex_refresh, before its next use outside a pass). Off (TOTK_VK_SIZE_REFRESH=1 to try): TOTK also reuses that memory
// for small transient targets of the same format (its environment probe renders 64x64 into the main depth's memory), so
// "the last written" was often such a transient and the full-size target got its stretched copy; the drop-outs stayed.
// The fix is to keep the size fixed (no dynamic resolution).
static _Atomic uint64_t g_write_seq;
static uint64_t g_refreshes;
static void iwb_note(vkb_image *im);
void vkb_image_written(vkb_image *im) {
    if (!im->gpu_written) atomic_fetch_add(&g_iwb_img_gen, 1);   // [UIT 0056] newly GPU-drawn: read ranges looked at again
    im->gpu_written = 1; im->write_seq = atomic_fetch_add(&g_write_seq, 1) + 1;
    if (im->wb_ok) iwb_note(im);   // [UIT 0054] (wb_ok only when TOTK_VK_IMAGE_WRITEBACK is on)
}
static void refresh_mark(entry *e) {   // g_mu held: e is starting to be used by a new texture object
    entry *best = NULL;
    for (entry *o = g_all; o; o = o->all_next)
        if (o != e && o->im.storage == e->im.storage && o->im.format == e->im.format && o->im.gpu_written && o->im.aspect == e->im.aspect &&
            (o->im.width != e->im.width || o->im.height != e->im.height) && o->im.write_seq > e->im.write_seq &&
            (!best || o->im.write_seq > best->im.write_seq)) best = o;
    e->im.refresh_from = best ? &best->im : NULL;
}
int vkb_tex_refresh(VkCommandBuffer cb, vkb_image *im) {
    vkb_image *src = im->refresh_from;
    im->refresh_from = NULL;
    if (!src || vkb_in_pass()) return 0;
    img_lock(IS_REFRESH);
    int alive = 0;   // the source may have been destroyed since it was marked
    for (entry *o = g_all; o && !alive; o = o->all_next) alive = &o->im == src;
    img_unlock();
    if (!alive || src->write_seq <= im->write_seq) return 0;
    VkFormatFeatureFlags need = VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT;
    VkFormatProperties fp;
    vk.vkGetPhysicalDeviceFormatProperties(vkc.gpu, im->format, &fp);
    if ((fp.optimalTilingFeatures & need) != need) return 0;
    int linear = im->aspect == VK_IMAGE_ASPECT_COLOR_BIT && (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT);
    vkb_image_use(cb, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
    vkb_image_use(cb, im, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    uint32_t layers = src->layers < im->layers ? src->layers : im->layers;
    VkImageBlit r = {.srcSubresource = {im->aspect, 0, 0, layers}, .srcOffsets = {{0, 0, 0}, {(int32_t)src->width, (int32_t)src->height, 1}},
                     .dstSubresource = {im->aspect, 0, 0, layers}, .dstOffsets = {{0, 0, 0}, {(int32_t)im->width, (int32_t)im->height, 1}}};
    vkb_checkpoint("refresh from another size", NULL, NULL);
    vk.vkCmdBlitImage(cb, src->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, im->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r,
                      linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST);
    im->gpu_written = 1;
    im->write_seq = src->write_seq;
    g_refreshes++;
    static int said;
    if (said++ < 8) VLOG("image %llu (%ux%u, format %#x) takes the contents of image %llu (%ux%u) at the same memory", (unsigned long long)im->serial,
                         im->width, im->height, im->d.format, (unsigned long long)src->serial, src->width, src->height);
    return 1;
}

// Recording threads look up the same serials draw after draw: a small cache per thread, valid for one submission (no
// image is freed while a submission is recorded: releases run on the translation thread between submissions).
typedef struct { uint64_t serial, sub; vkb_image *im; } icache_t;
static __thread icache_t *t_icache;
static void pre_drop(entry *pre) {   // [UIT 0231] g_mu held: an image made without the lock that another thread's beat (never used)
    vkb_destroy_image_later(pre->im.img, pre->im.mem, pre->im.alloc);
    free(pre);
    atomic_fetch_add_explicit(&g_unl_lost, 1, memory_order_relaxed);
}
vkb_image *vkb_image_of(const tex_t *t) {
    if (!t || !t->serial || !vkb_ready()) return NULL;
    icache_t *c = NULL;
    uint64_t sub = mtl_last_fence() + 1;
    if (vkb_mt_recording()) {
        if (!t_icache && !(t_icache = calloc(1024, sizeof *t_icache))) die("image cache");
        c = &t_icache[(t->serial * 0x9E3779B97F4A7C15ull) >> 54];
        if (c->serial == t->serial && c->sub == sub) { if (img_stats_on()) vkb_cnt_add(g_lu[LU_CACHE], 1); return c->im; }
        if (lockfree_on()) {   // [UIT 0233] exact or NULL (then the lookup with the lock, as before)
            entry *f = (entry *)vst_get_lockfree(&g_st, t->serial);
            if (img_stats_on()) vkb_cnt_add(g_lu[f ? LU_LOCKFREE : LU_LOCKFREE_MISS], 1);
            if (f) { *c = (icache_t){t->serial, sub, &f->im}; return &f->im; }
        }
    }
    entry *pre = NULL;   // [UIT 0231] made without the lock; used below unless another thread put one in meanwhile
    img_lock(IS_LOOKUP);
again:;
    table_reserve();
    vst_slot *s = vst_slot_of(&g_st, t->serial);
    if (s->serial == t->serial && s->e) {
        vkb_image *found = &((entry *)s->e)->im;
        if (pre) pre_drop(pre);
        img_unlock();
        if (img_stats_on()) vkb_cnt_add(g_lu[LU_FOUND], 1);
        if (c) *c = (icache_t){t->serial, sub, found};
        return found;
    }
    entry *e = NULL;
    static int no_share = -1;   // TOTK_VK_NO_IMAGE_SHARE=1 (development A/B): an image per texture object, as before
    if (no_share < 0) { const char *ns = getenv("TOTK_VK_NO_IMAGE_SHARE"); no_share = ns && *ns == '1'; }
    if (t->mem && !no_share) for (e = g_shape[shape_of(t->mem, &t->d)]; e && !same_shape(e, t); e = e->shape_next) {}
    if ((!e || !e->refs) && t->mem) {   // a new or taken-back image over memory another live image uses: what differs (the game sees one texture there)
        static int said;
        static long long from = -1;   // TOTK_SHAPE_LOG=<present>: from that present on (development)
        if (from < 0) { const char *f = getenv("TOTK_SHAPE_LOG"); from = f ? atoll(f) : 0; }
        for (entry *o = g_all; o && from && vkb_frame() >= (uint64_t)from && said < 48; o = o->all_next) {
            if (o == e || o->im.storage != t->mem || !o->refs) continue;
            const nvn_texdesc *a = &o->im.d, *b = &t->d;
            said++;
            VLOG("[shape] texture %llu (%s) over the memory of image %llu (%p): target %u/%u format %#x/%#x flags %#x/%#x %ux%ux%u/%ux%ux%u "
                 "levels %u/%u samples %u/%u stride %u/%u block_h %u/%u swizzle %u%u%u%u/%u%u%u%u layout %d/%d%s",
                 (unsigned long long)t->serial, e ? "taken back" : "new", (unsigned long long)o->im.serial, t->mem, a->target, b->target, a->format, b->format,
                 a->flags, b->flags, a->width, a->height, a->depth, b->width, b->height, b->depth, a->levels, b->levels, a->samples, b->samples,
                 a->stride, b->stride, a->pkg_block_h, b->pkg_block_h, a->swizzle_r, a->swizzle_g, a->swizzle_b, a->swizzle_a,
                 b->swizzle_r, b->swizzle_g, b->swizzle_b, b->swizzle_a, o->has_layout, t->has_layout,
                 o->has_layout && t->has_layout && memcmp(o->layout, t->layout, sizeof o->layout) ? " (layouts differ)" : "");
        }
    }
    if (e && pre) { pre_drop(pre); pre = NULL; }   // [UIT 0231] shared or taken back after all
    if (e && !e->refs) {   // taken back from the side: written by the CPU meanwhile -> its new contents upload
        int k = 0;
        while (k < g_naside && g_aside[k] != e) k++;
        if (k < g_naside) memmove(&g_aside[k], &g_aside[k + 1], (size_t)(--g_naside - k) * sizeof g_aside[0]);
        if (e->im.wb_rel) { e->im.wb_rel = e->im.wb_alias = 0; g_iwb_rel_unmarked++; atomic_fetch_add(&g_bidx_gen, 1); bact_fix(&e->im); }   // ([UIT 0234] buffer reads may mark it again)   // [UIT 0155] held again: its own image is current
        nvn_texlayout L;
        nvn_tex_layout(&e->im.d, &L);
        if (nvn_dirty_tracked(e->im.storage, L.size) && nvn_dirty_since(e->im.storage, L.size, e->released_stamp)) {
            e->im.gpu_written = 0; e->im.upload_stamp = 0;
        } else if (gpu_writes_on() && e->im.gpu_written && e->im.wb_ok && !e->im.wb_pending && !e->im.not_uploadable &&
                   e->released_gpu && nvn_gpu_written_since(e->im.storage, L.size, e->released_gpu)) {
            // [UIT 0139] Taken back after another image's drawing was copied back over its memory (a terrain tile drawn
            // again under another shape while this one was aside): before, it kept its own older drawing (gpu_written
            // stops uploads) and the game saw the old tile there. Its own drawing is in game memory too (wb_ok, nothing
            // pending), so the memory is the newest whole picture: uploaded again. TOTK_TEX_GPU_WRITES=0: as before.
            e->im.gpu_written = 0; e->im.upload_stamp = 0;
            g_tb_gpu++;
            static int said;
            if (said++ < 8) VLOG("texture %llu (format %#x %ux%u): taken back after GPU writes from another image landed in its memory: uploads again",
                                 (unsigned long long)e->im.serial, e->im.d.format, e->im.width, e->im.height);
        }
        g_images_taken_back++;
    } else if (e) g_images_shared++;
    if (!e && !pre && unlocked_on()) {   // [UIT 0231] made without the lock, then looked up again
        img_unlock();
        pre = calloc(1, sizeof *pre);
        if (!pre) die("image entry");
        uint64_t t0 = runtime_now_ns();
        int ok = create(&pre->im, t);
        vkb_step_add(PK_CREATE, t0);
        if (!ok) { free(pre); return NULL; }
        atomic_fetch_add_explicit(&g_unl_images, 1, memory_order_relaxed);
        img_lock(IS_CREATE);
        goto again;
    }
    if (!e) {
        int ok = 1;
        if (pre) { e = pre; pre = NULL; }
        else {
            e = calloc(1, sizeof *e);
            if (!e) die("image entry");
            uint64_t t0 = runtime_now_ns();
            ok = create(&e->im, t);
            vkb_step_add(PK_CREATE, t0);
        }
        if (!ok) { free(e); img_unlock(); return NULL; }
        e->im.wb_ok = iwb_eligible(t, &e->im);   // [UIT 0054]
        e->im.wb_fit = e->im.wb_ok || iwb_fit(t, &e->im);   // [UIT 0056]
        e->has_layout = t->has_layout;
        memcpy(e->layout, t->layout, sizeof e->layout);
        if (t->mem) { uint32_t h = shape_of(t->mem, &t->d); e->shape_next = g_shape[h]; g_shape[h] = e; e->shaped = 1; }
        e->all_next = g_all; e->all_pprev = &g_all;
        if (g_all) g_all->all_pprev = &e->all_next;
        g_all = e;
        e->seq = ++g_entry_seq;   // [UIT 0230]
        if (e->im.storage) {
            nvn_texlayout L;
            nvn_tex_layout(&e->im.d, &L);
            uint64_t lo = (uint64_t)(uintptr_t)e->im.storage;
            if (!vkb_ix_add(&g_ix_all, e, e->seq, lo, lo + L.size, &e->ix_all) && !g_ix_bad) {
                g_ix_bad = 1; VLOG("[UIT 0230] image index: out of memory, the old walks from now on");
            }
        }
        g_images_made++;
    }
    static int size_refresh = -1;   // [UIT 0230] read once (was a getenv per new texture object, with the lock held)
    if (size_refresh < 0) size_refresh = getenv("TOTK_VK_SIZE_REFRESH") != NULL;
    if (t->mem && size_refresh) refresh_mark(e);   // [experiment, off] picks transient aliases (see above)
    e->refs++;
    vst_put(&g_st, s, t->serial, e);
    img_unlock();
    if (img_stats_on()) vkb_cnt_add(g_lu[LU_MADE], 1);
    if (c) *c = (icache_t){t->serial, sub, &e->im};
    return &e->im;
}

// ---- layouts as a chunk records them (vkb_mt.c): an image the chunk has not used yet is taken to be in the layout it
// is first needed in (no barrier recorded); the command buffer between chunks moves it there from where the previous
// chunk left it. One thread recording (t_trk NULL): the image's own layout, a barrier at every change.
static __thread vkb_trk *t_trk;
void vkb_trk_set(vkb_trk *t) { t_trk = t; }
vkb_trk *vkb_trk_get(void) { return t_trk; }
void vkb_trk_reset(vkb_trk *t) {
    t->n = 0;
    if (t->tab) memset(t->tab, 0xff, sizeof *t->tab * (size_t)t->tcap);
}
static uint32_t trk_hash(const vkb_image *im, int cap) { return (uint32_t)(((uint64_t)(uintptr_t)im * 0x9E3779B97F4A7C15ull) >> 40) & (uint32_t)(cap - 1); }
static vkb_trk_e *trk_find(vkb_trk *t, const vkb_image *im) {
    if (!t->tcap) return NULL;
    for (uint32_t h = trk_hash(im, t->tcap);; h = (h + 1) & (uint32_t)(t->tcap - 1)) {
        int k = t->tab[h];
        if (k < 0) return NULL;
        if (t->e[k].im == im) return &t->e[k];
    }
}
static vkb_trk_e *trk_add(vkb_trk *t, vkb_image *im, VkImageLayout l) {
    if ((t->n + 1) * 2 > t->tcap) {   // the index stays at most half full
        int cap = t->tcap ? t->tcap * 2 : 256;
        int *tab = malloc(sizeof *tab * (size_t)cap);
        if (!tab) die("layout tracking");
        memset(tab, 0xff, sizeof *tab * (size_t)cap);
        for (int k = 0; k < t->n; k++) {
            uint32_t h = trk_hash(t->e[k].im, cap);
            while (tab[h] >= 0) h = (h + 1) & (uint32_t)(cap - 1);
            tab[h] = k;
        }
        free(t->tab); t->tab = tab; t->tcap = cap;
    }
    if (t->n == t->cap) {
        t->cap = t->cap ? t->cap * 2 : 128;
        if (!(t->e = realloc(t->e, sizeof *t->e * (size_t)t->cap))) die("layout tracking");
    }
    vkb_trk_e *e = &t->e[t->n];
    *e = (vkb_trk_e){im, l, l};
    uint32_t h = trk_hash(im, t->tcap);
    while (t->tab[h] >= 0) h = (h + 1) & (uint32_t)(t->tcap - 1);
    t->tab[h] = t->n++;
    return e;
}
int vkb_layout_is(vkb_image *im, VkImageLayout l) {
    if (!t_trk) return im->layout == l;
    vkb_trk_e *e = trk_find(t_trk, im);
    if (!e) { trk_add(t_trk, im, l); return 1; }
    return e->cur == l;
}
VkImageLayout vkb_layout(vkb_image *im) {
    if (!t_trk) return im->layout;
    vkb_trk_e *e = trk_find(t_trk, im);
    return e ? e->cur : im->layout;
}

void vkb_image_use(VkCommandBuffer cb, vkb_image *im, VkImageLayout layout, VkPipelineStageFlags2 stage, VkAccessFlags2 access) {
    VkImageLayout old = im->layout;
    if (t_trk) {
        vkb_trk_e *e = trk_find(t_trk, im);
        if (!e) { trk_add(t_trk, im, layout); return; }   // its first use in the chunk: the barrier between chunks
        old = e->cur;
        e->cur = layout;
    } else im->layout = layout;
    // [temporary] every use waits for all earlier work on the image: precise per-pass barriers come with the pass tracker
    VkImageMemoryBarrier2 b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2, .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
        .srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT, .dstStageMask = stage, .dstAccessMask = access, .oldLayout = old,
        .newLayout = layout, .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = im->img, .subresourceRange = {im->aspect, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS}};
    VkDependencyInfo di = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &b};
    vk.vkCmdPipelineBarrier2(cb, &di);
}

static VkComponentSwizzle swz(uint8_t s, VkComponentSwizzle identity) {
    switch (s) {
    case NVN_TEXTURE_SWIZZLE_ZERO: return VK_COMPONENT_SWIZZLE_ZERO;
    case NVN_TEXTURE_SWIZZLE_ONE: return VK_COMPONENT_SWIZZLE_ONE;
    case NVN_TEXTURE_SWIZZLE_R: return VK_COMPONENT_SWIZZLE_R;
    case NVN_TEXTURE_SWIZZLE_G: return VK_COMPONENT_SWIZZLE_G;
    case NVN_TEXTURE_SWIZZLE_B: return VK_COMPONENT_SWIZZLE_B;
    case NVN_TEXTURE_SWIZZLE_A: return VK_COMPONENT_SWIZZLE_A;
    default: return identity;
    }
}

// Formats without alpha read a = 1 (RGBX, RGB DXT1): NVN_FORMAT values, as MacNativeGames' format table.
static int alpha_one(int f) {
    return f == 0x37 || f == 0x41 || f == 0x45 || (f >= 0x6D && f <= 0x78) || f == 0x96 || f == 0x98 || f == 0x9A;
}

VkImageView vkb_view(vkb_image *im, const view_t *v, int attachment, int view_type) {
    entry *e = (entry *)im;   // im is the first member
    uint8_t key[VKB_VIEW_KEY];
    // streaming (TOTK_TEX_ASYNC): sampled views start at the largest level present; a view per step until all are
    uint32_t rb = attachment ? 0 : __atomic_load_n(&im->resident_base, __ATOMIC_ACQUIRE);
    int fixed = view_key_fixed();
    vkb_view_key(key, v, attachment, view_type, rb, fixed);   // [UIT 0200] vkb_viewkey.h (0124: targets/storage, no swizzle)
    // Recording threads look views up without a lock: entries are only appended (a full table takes no more), each
    // published by the count after it is written.
    int nv = __atomic_load_n(&e->nviews, __ATOMIC_ACQUIRE);
    for (int i = 0; i < nv; i++) if (!memcmp(view_key_at(e, i), key, sizeof key)) return *view_at(e, i);
    uint32_t base = v && v->set_levels ? v->base_level : 0, nlev = v && v->set_levels && v->nlevels ? v->nlevels : im->levels - base;
    uint32_t layer = v && v->set_layers ? v->min_layer : 0, nlay = v && v->set_layers && v->nlayers ? v->nlayers : im->layers - layer;
    if (attachment) nlev = 1;
    if (attachment == 1 && !(im->usage & (VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT))) return VK_NULL_HANDLE;
    if (attachment == 2 && !(im->usage & VK_IMAGE_USAGE_STORAGE_BIT)) return VK_NULL_HANDLE;
    VkFormat f = im->format;
    if (v && v->set_format && !im->astc) {
        VkFormat vf = vkb_format(v->format, NULL, NULL);
        if (vf != VK_FORMAT_UNDEFINED) f = vf;
    }
    int reinterpret_block = im->compressed && !(f >= VK_FORMAT_BC1_RGB_UNORM_BLOCK && f <= VK_FORMAT_BC7_SRGB_BLOCK);
    if (reinterpret_block) {   // an uncompressed view of a compressed image: one level, one layer
        // [UIT 0125] a target when the image was made with COLOR_ATTACHMENT (create: BC render targets)
        if (attachment == 2 || (attachment == 1 && !(im->usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT))) return VK_NULL_HANDLE;
        nlev = 1;
        nlay = 1;
    } else if (attachment && im->compressed) return VK_NULL_HANDLE;   // a compressed format is never a target itself
    if (attachment == 2) {   // storage views: the UNORM form of sRGB formats
        if (f == VK_FORMAT_R8G8B8A8_SRGB) f = VK_FORMAT_R8G8B8A8_UNORM;
        else if (f == VK_FORMAT_B8G8R8A8_SRGB) f = VK_FORMAT_B8G8R8A8_UNORM;
    }
    int target = v && v->set_target ? v->target : im->d.target;
    VkImageViewType type = im->depth > 1 ? VK_IMAGE_VIEW_TYPE_3D
        : target == NVN_TEXTURE_TARGET_1D ? VK_IMAGE_VIEW_TYPE_1D : target == NVN_TEXTURE_TARGET_1D_ARRAY ? VK_IMAGE_VIEW_TYPE_1D_ARRAY
        : target == NVN_TEXTURE_TARGET_CUBEMAP && !attachment ? VK_IMAGE_VIEW_TYPE_CUBE
        : target == NVN_TEXTURE_TARGET_CUBEMAP_ARRAY && !attachment ? VK_IMAGE_VIEW_TYPE_CUBE_ARRAY
        : nlay > 1 || target == NVN_TEXTURE_TARGET_2D_ARRAY || target == NVN_TEXTURE_TARGET_2D_MULTISAMPLE_ARRAY ? VK_IMAGE_VIEW_TYPE_2D_ARRAY
        : VK_IMAGE_VIEW_TYPE_2D;
    if (type == VK_IMAGE_VIEW_TYPE_3D && attachment) { type = VK_IMAGE_VIEW_TYPE_2D_ARRAY; nlay = reinterpret_block ? 1 : im->depth; }
    if (!attachment && view_type >= 0 && im->depth <= 1) {   // as the shader declares it (layer counts must fit the type)
        type = (VkImageViewType)view_type;
        if (((type == VK_IMAGE_VIEW_TYPE_CUBE || type == VK_IMAGE_VIEW_TYPE_CUBE_ARRAY) && nlay < 6) || type == VK_IMAGE_VIEW_TYPE_3D)
            vkb_audit(A_TEX_VIEW_ADJUSTED, NULL);
        if ((type == VK_IMAGE_VIEW_TYPE_CUBE && nlay < 6) || (type == VK_IMAGE_VIEW_TYPE_CUBE_ARRAY && nlay < 6))
            type = nlay > 1 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
        if (type == VK_IMAGE_VIEW_TYPE_CUBE) nlay = 6;
        if (type == VK_IMAGE_VIEW_TYPE_CUBE_ARRAY) nlay -= nlay % 6;
        if (type == VK_IMAGE_VIEW_TYPE_2D || type == VK_IMAGE_VIEW_TYPE_1D) nlay = 1;
        if (type == VK_IMAGE_VIEW_TYPE_3D) type = VK_IMAGE_VIEW_TYPE_2D_ARRAY;   // a 2D image declared 3D: closest valid view
    } else if (!attachment && view_type == VK_IMAGE_VIEW_TYPE_3D && im->depth > 1) type = VK_IMAGE_VIEW_TYPE_3D;
    if (type == VK_IMAGE_VIEW_TYPE_3D) { layer = 0; nlay = 1; }
    if (type == VK_IMAGE_VIEW_TYPE_2D_ARRAY && im->depth > 1) {   // slices of a 3D image: the depth of the view's level
        uint32_t dl = im->depth >> base ? im->depth >> base : 1;
        nlev = 1;
        if (layer + nlay > dl) { if (layer >= dl) layer = 0; nlay = dl - layer; }
    }
    if (im->depth <= 1 && layer + nlay > im->layers) { if (layer >= im->layers) layer = 0; nlay = im->layers - layer; }
    if (base + nlev > im->levels) { if (base >= im->levels) base = 0; nlev = im->levels - base; }
    if (rb && base < rb && rb < im->levels) { uint32_t end = base + nlev; base = rb; nlev = end > rb ? end - rb : 1; }
    // swizzle: the view's, else the texture's own (TextureBuilder SetSwizzle); formats without alpha read a = 1
    VkComponentMapping cm = {VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY};
    if (!attachment) {
        if (v && v->set_swizzle)
            cm = (VkComponentMapping){swz(v->swz[0], VK_COMPONENT_SWIZZLE_R), swz(v->swz[1], VK_COMPONENT_SWIZZLE_G),
                                      swz(v->swz[2], VK_COMPONENT_SWIZZLE_B), swz(v->swz[3], VK_COMPONENT_SWIZZLE_A)};
        else if (im->d.swizzle_r | im->d.swizzle_g | im->d.swizzle_b | im->d.swizzle_a)
            cm = (VkComponentMapping){swz((uint8_t)im->d.swizzle_r, VK_COMPONENT_SWIZZLE_R), swz((uint8_t)im->d.swizzle_g, VK_COMPONENT_SWIZZLE_G),
                                      swz((uint8_t)im->d.swizzle_b, VK_COMPONENT_SWIZZLE_B), swz((uint8_t)im->d.swizzle_a, VK_COMPONENT_SWIZZLE_A)};
        if (alpha_one(v && v->set_format ? v->format : im->d.format)) {
            if (cm.a == VK_COMPONENT_SWIZZLE_IDENTITY || cm.a == VK_COMPONENT_SWIZZLE_A) cm.a = VK_COMPONENT_SWIZZLE_ONE;
        }
    }
    VkImageAspectFlags aspect = im->aspect;
    if (!attachment && aspect == (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)) aspect = VK_IMAGE_ASPECT_DEPTH_BIT;   // sampled: depth
    // the view's own usage: an image made for storage too must not make every view storage (sRGB views cannot be)
    VkImageViewUsageCreateInfo vu = {VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO,
        .usage = attachment == 2 ? VK_IMAGE_USAGE_STORAGE_BIT
               : attachment == 1 ? im->usage & (VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT)
               : VK_IMAGE_USAGE_SAMPLED_BIT};
    // views used in the feedback loop layout: as attachment and sampled at once (a sampled view in another format
    // keeps plain sampling: the format may not be renderable)
    if ((im->usage & VK_IMAGE_USAGE_ATTACHMENT_FEEDBACK_LOOP_BIT_EXT) && (attachment == 1 || (!attachment && f == im->format)))
        vu.usage = im->usage & (VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
                                VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_ATTACHMENT_FEEDBACK_LOOP_BIT_EXT);
    VkImageViewCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, &vu, .image = im->img, .viewType = type, .format = f,
                                .components = cm, .subresourceRange = {aspect, base, nlev, layer, nlay}};
    VkImageView view = VK_NULL_HANDLE;
    int unl = unlocked_on();   // [UIT 0231] made before taking the lock (the image lives on: no image goes while a submission is recorded)
    if (unl) {
        if (!vk_check(vk.vkCreateImageView(vkc.device, &ci, NULL, &view), "image view")) return VK_NULL_HANDLE;
        atomic_fetch_add_explicit(&g_unl_views, 1, memory_order_relaxed);
    }
    img_lock(IS_VIEW);
    nv = e->nviews;
    for (int i = 0; i < nv; i++)
        if (!memcmp(view_key_at(e, i), key, sizeof key)) {   // made meanwhile
            VkImageView got = *view_at(e, i);
            img_unlock();
            if (view) { vk.vkDestroyImageView(vkc.device, view, NULL); atomic_fetch_add_explicit(&g_unl_lost, 1, memory_order_relaxed); }   // never used
            return got;
        }
    if (!unl && !vk_check(vk.vkCreateImageView(vkc.device, &ci, NULL, &view), "image view")) { img_unlock(); return VK_NULL_HANDLE; }
    atomic_fetch_add(&g_views_made, 1);
    int cap = fixed ? VKB_VIEW_MAX : VIEWS;
    if (nv >= VIEWS && nv < cap && !e->vblk[(nv - VIEWS) / VKB_VIEW_BLOCK])
        e->vblk[(nv - VIEWS) / VKB_VIEW_BLOCK] = calloc(1, sizeof(view_blk));   // NULL: not kept (below)
    if (nv < cap && (nv < VIEWS || e->vblk[(nv - VIEWS) / VKB_VIEW_BLOCK])) {
        memcpy(view_key_at(e, nv), key, sizeof key);
        *view_at(e, nv) = view;
        __atomic_store_n(&e->nviews, nv + 1, __ATOMIC_RELEASE);
    } else {   // many distinct views of one texture: this one is not kept (destroyed when the GPU is done with it)
        static int said; if (said++ < 4) VLOG("texture %llu: more than %d views", (unsigned long long)im->serial, cap);
        atomic_fetch_add(&g_views_dropped, 1);
        vkb_destroy_later(view, VK_NULL_HANDLE, VK_NULL_HANDLE);
    }
    img_unlock();
    return view;
}

void vkb_tex_release(const tex_t *t) {
    if (!t || !t->serial || !vkb_ready()) return;
    img_lock(IS_RELEASE);
    vst_slot *s = vst_cap(&g_st) ? vst_slot_of(&g_st, t->serial) : NULL;
    if (s && s->serial == t->serial && s->e) {
        entry *e = s->e;
        vst_remove(&g_st, s);   // [UIT 0233] the epoch first: lock-free readers in an old table answer "don't know"
        if (--e->refs == 0) {
            if (!e->shaped) entry_drop(e);   // no memory: nothing can take it back
            else {
                if (g_naside >= aside_cap()) { entry *o = g_aside[0]; memmove(g_aside, g_aside + 1, (size_t)--g_naside * sizeof g_aside[0]); entry_drop(o); }
                e->released = vkb_frame();
                e->released_stamp = nvn_dirty_now();
                e->released_gpu = nvn_gpu_write_now();   // [UIT 0139]
                // [UIT 0155] its last drawing goes into game memory (at the end of the submission that drew it, or the
                // next one if it is pending already): what the Switch's memory holds once the game lets it go
                if (coherent_on() && e->im.wb_ok && !e->im.wb_alias && vkb_iwb_mode() != IWB_OFF && e->im.gpu_written &&
                    iwb_size(&e->im) <= IWB_REL_MAX) {
                    e->im.wb_alias = e->im.wb_rel = 1;
                    atomic_fetch_add(&g_bidx_gen, 1);   // [UIT 0234] buffer reads may clear wb_rel
                    bact_fix(&e->im);                   // [UIT 0243]
                    g_iwb_rel_marked++;
                    if (e->im.wb_pending) atomic_store(&g_iwb_alias_want, 1);
                    uint64_t m = g_iwb_rel_marked;
                    if (m == 1 || m == 100 || m == 10000)
                        VLOG("[alias] %llu GPU-drawn images let go by the game: their last drawing copied back (TOTK_VK_GPU_MEM_COHERENT)", (unsigned long long)m);
                }
                g_aside[g_naside++] = e;
            }
        }
    }
    aside_purge();
    img_unlock();
}

// ---------------- contents from game memory ----------------
// Every level and layer: block-linear (or linear) storage -> rows of blocks in upload memory (nvn_deswizzle) -> image.
// ASTC: the blocks are decoded to RGBA8 on the CPU first ([temporary], phase 5: BC7 at build time). Depth / stencil and
// 3-component formats are not uploaded yet (render targets; logged once).
// The CPU cannot write game memory while a submission is replayed (the replay runs on the submitting thread), so one
// check per texture per submission is enough; a texture found changed is uploaded before its next use (the pass ends
// for it). Without this, textures the CPU updates while drawn every frame (the font atlas: new glyphs, the save slot
// thumbnails) kept their first contents.
// TOTK_TEX_GPU_WRITES (1): GPU writes into a texture's game memory (copied back at the end of a submission) make it
// stale too (nvn_gpu_written_since). 0: as before 0008 (such a texture keeps its first contents).
static int gpu_writes_on(void) {
    static int v = -1;
    if (v < 0) { const char *e = getenv("TOTK_TEX_GPU_WRITES"); v = !(e && *e == '0'); }
    return v;
}
static uint64_t g_t_gpu_stale;   // textures found changed by GPU writes (the per-frame line)
int vkb_tex_stale(vkb_image *im) {
    uint64_t sub = mtl_last_fence() + 1;
    if (!im->upload_stamp || im->gpu_written || !im->storage) return 0;
    if (vkb_cpuw_peek(&im->cpu_written)) return 1;
    if (im->vpool_gen && im->vpool_gen != totk_vpool_generation()) return 1;   // a virtual pool's mapping changed
    if (__atomic_load_n(&im->dirty_checked, __ATOMIC_ACQUIRE) != sub) {
        int mt = vkb_mt_recording();
        if (mt) {   // one thread checks; the others wait for its answer
            // [UIT 0019] claimed per texture (the submission with the top bit set) instead of under the images lock:
            // every texture's first check of a submission took that lock, and with more recording threads they queued
            // on it (TOTK_PROFILE showed ~1.5 ms a frame); checks of different textures now run side by side
            const uint64_t busy = sub | (1ull << 63);
            for (unsigned spin = 0;; spin++) {
                uint64_t dc = __atomic_load_n(&im->dirty_checked, __ATOMIC_ACQUIRE);
                if (dc == sub) return im->stale;
                if (dc == busy) { if (spin < 64) __builtin_ia32_pause(); else sched_yield(); continue; }
                if (__atomic_compare_exchange_n(&im->dirty_checked, &dc, busy, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) break;
            }
        }
        nvn_texlayout L;
        nvn_tex_layout(&im->d, &L);
        // a virtual pool: the physical pages behind it; [temporary] untracked memory (CPU-no-access pools hold GPU data)
        // counts as unchanged: uploaded once
        uint64_t gs = gpu_writes_on() ? im->gpu_stamp : 0;
        int v = im->vpool_gen ? totk_vpool_dirty_since((uintptr_t)im->storage, L.size, im->upload_stamp, gs) : -1;
        im->stale = v >= 0 ? v : nvn_dirty_tracked(im->storage, L.size) && nvn_dirty_since(im->storage, L.size, im->upload_stamp);
        if (!im->stale && v < 0 && gs && nvn_gpu_written_since(im->storage, L.size, gs)) {   // the GPU filled it (0008)
            im->stale = 1;
            __atomic_fetch_add(&g_t_gpu_stale, 1, __ATOMIC_RELAXED);
            static int said;
            if (said++ < 16) VLOG("texture %llu (format %#x %ux%ux%u, %u levels): its memory was written by the GPU since its upload: uploads again",
                                  (unsigned long long)im->serial, im->d.format, im->width, im->height, im->layers, im->levels);
        }
        static long long log_serial = -1;   // TOTK_VK_STALE_LOG=<texture serial>: the check's answers (development)
        if (log_serial == -1) { const char *e = getenv("TOTK_VK_STALE_LOG"); log_serial = e ? atoll(e) : 0; }
        if (log_serial && (long long)im->serial == log_serial)
            VLOG("[stale] texture %llu submission %llu: tracked %d stale %d (stamp %llu now %llu)", (unsigned long long)im->serial,
                 (unsigned long long)sub, nvn_dirty_tracked(im->storage, L.size), im->stale, (unsigned long long)im->upload_stamp,
                 (unsigned long long)nvn_dirty_now());
        __atomic_store_n(&im->dirty_checked, sub, __ATOMIC_RELEASE);
    }
    return im->stale;
}

// ASTC is decoded on the CPU (desktop GPUs have no ASTC); entering the world, decoding new textures on the recording
// thread held single submissions for 0.3-2.3 s (28% of that thread there, Great Sky Island, 2026-10-24). A texture's
// levels, layers and slices are gathered (astc_add) and decoded together (astc_run) by ASTC_WORKERS threads and the
// recording thread, in pieces of ASTC_CHUNK block rows: most of TOTK's textures and mip levels are small, so splitting
// texture by texture left the workers idle.
// ASTC -> BC7 when the port is built (vkb_bc7.c, tools/bc7_build.py) takes most decoding out of play.
enum { ASTC_WORKERS = 4, ASTC_CHUNK = 4 };   // the fewest workers (before UIT 0140: always 4); block rows per piece taken
typedef struct { const uint8_t *src; uint8_t *dst; uint32_t w, h, bw, bh, first; int srgb; } astc_item;   // first: its first piece
static astc_item *g_ai;
static int g_nai, g_capai;
static uint32_t g_apieces;                  // pieces of all items
static _Atomic uint32_t g_anext, g_adone;   // the next piece to take / pieces decoded
static uint64_t g_aj_serial;
static int g_aj_open, g_aj_active;          // a batch is being decoded / workers in it
static pthread_mutex_t g_aj_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_aj_cv = PTHREAD_COND_INITIALIZER, g_aj_done_cv = PTHREAD_COND_INITIALIZER;
uint64_t vkb_astc_ns;                       // decoding time on the recording thread (long submission log)
static void astc_add(const uint8_t *src, uint32_t w, uint32_t h, uint32_t bw, uint32_t bh, int srgb, uint8_t *dst) {
    if (g_nai == g_capai) {
        g_capai = g_capai ? g_capai * 2 : 64;
        if (!(g_ai = realloc(g_ai, sizeof *g_ai * (size_t)g_capai))) die("ASTC items");
    }
    g_ai[g_nai++] = (astc_item){src, dst, w, h, bw, bh, g_apieces, srgb};
    g_apieces += ((h + bh - 1) / bh + ASTC_CHUNK - 1) / ASTC_CHUNK;
}
static void astc_work(void) {   // pieces until none is left
    int k = 0;
    for (;;) {
        uint32_t p = atomic_fetch_add(&g_anext, 1);
        if (p >= g_apieces) return;
        while (k + 1 < g_nai && g_ai[k + 1].first <= p) k++;   // pieces are taken in order: the item only moves forward
        while (k > 0 && g_ai[k].first > p) k--;
        const astc_item *it = &g_ai[k];
        uint32_t r = (p - it->first) * ASTC_CHUNK;
        astc_decode_ldr_rows(it->src, it->w, it->h, it->bw, it->bh, it->srgb, it->dst, r, r + ASTC_CHUNK);
        if (atomic_fetch_add(&g_adone, 1) + 1 == g_apieces) {
            pthread_mutex_lock(&g_aj_mu);
            pthread_cond_broadcast(&g_aj_done_cv);
            pthread_mutex_unlock(&g_aj_mu);
        }
    }
}
static void *astc_worker(void *unused) {
    (void)unused;
    pthread_setname_np("totk-astc");
    uint64_t seen = 0;
    for (;;) {
        pthread_mutex_lock(&g_aj_mu);
        while (g_aj_serial == seen) pthread_cond_wait(&g_aj_cv, &g_aj_mu);
        seen = g_aj_serial;
        int open = g_aj_open;
        if (open) g_aj_active++;
        pthread_mutex_unlock(&g_aj_mu);
        if (!open) continue;
        astc_work();
        pthread_mutex_lock(&g_aj_mu);
        g_aj_active--;
        pthread_cond_broadcast(&g_aj_done_cv);
        pthread_mutex_unlock(&g_aj_mu);
    }
    return NULL;
}
static void astc_start(void) {   // [UIT 0140] as many as the texture preparation threads (TOTK_TEX_THREADS), 4 at least
    int n = vkb_pfor_threads() - 1 > ASTC_WORKERS ? vkb_pfor_threads() - 1 : ASTC_WORKERS;
    for (int i = 0; i < n; i++) {
        pthread_t t;
        if (pthread_create(&t, NULL, astc_worker, NULL) == 0) pthread_detach(t);
    }
}
static void astc_run(void) {   // the gathered items, decoded; then the batch is empty
    if (!g_nai) return;
    uint64_t t0 = runtime_now_ns();
    atomic_store(&g_anext, 0); atomic_store(&g_adone, 0);
    if (g_apieces > 2) {
        static pthread_once_t once = PTHREAD_ONCE_INIT;
        pthread_once(&once, astc_start);
        pthread_mutex_lock(&g_aj_mu);
        g_aj_open = 1;
        g_aj_serial++;
        pthread_cond_broadcast(&g_aj_cv);
        pthread_mutex_unlock(&g_aj_mu);
    }
    astc_work();
    if (g_apieces > 2) {
        pthread_mutex_lock(&g_aj_mu);
        while (atomic_load(&g_adone) < g_apieces) pthread_cond_wait(&g_aj_done_cv, &g_aj_mu);
        g_aj_open = 0;   // a worker that wakes late takes nothing
        while (g_aj_active) pthread_cond_wait(&g_aj_done_cv, &g_aj_mu);
        pthread_mutex_unlock(&g_aj_mu);
    }
    g_nai = 0; g_apieces = 0;
    vkb_astc_ns += runtime_now_ns() - t0;
}

static int tex_upload(VkCommandBuffer cb, vkb_image *im);
// Several threads recording: into the submission's prologue, which runs before every chunk (the CPU wrote the bytes
// before the submission, so earlier is as good; a GPU write to the image in a chunk still comes after it).
static pthread_mutex_t g_up_mu = PTHREAD_MUTEX_INITIALIZER;   // one upload at a time: the ASTC batch is shared

// ---- texture hitches (2026-10-29). The long submissions of the first run (302: 5.9 s recorded, 14.5 s of uploads
// summed over the recording threads; 1334: 0.8 s) were ASTC textures whose BC7 blocks all came from the cache: no
// decoding, few new images. Each upload deswizzled the ASTC rows into upload memory (write-combined, often in video
// memory), read them back there to hash the key (reads of such memory are very slow), and read the blocks with fread
// under one lock from a cache file not yet in memory; and uploads go one at a time. Now:
//  - TOTK_TEX_HOSTROWS (1): the rows are deswizzled into ordinary memory (key, decoding); upload memory is only written.
//  - vkb_bc7.c: the cache file mapped into memory (TOTK_TEX_MMAP) and prefetched from a used-textures list (TOTK_TEX_PREFETCH).
//  - TOTK_TEX_ASYNC (1): a texture's mips up to TOTK_TEX_TAIL texels (128) are uploaded at once; its larger levels are
//    made on TOTK_TEX_STREAM_THREADS background threads, smallest first, textures drawn in the last frame first, and
//    applied at the next use within TOTK_TEX_BUDGET_MB a frame (256). Until then its views start at the largest level
//    present (a blurrier texture for a few frames: pop-in, never a blank). TOTK_TEX_GRACE_MS (2) a frame waits for a
//    level that is nearly made. Copies and CPU reads of such a texture first take every level (vkb_tex_upload_full).
typedef struct { int hostrows, async, tail, threads, grace_us; uint64_t budget, max_pending, deadline_ns; } texcfg_t;
static texcfg_t g_tc;
static int env_int(const char *n, int def) { const char *e = getenv(n); return e && *e ? atoi(e) : def; }
static void tc_init(void) {
    g_tc.hostrows = env_int("TOTK_TEX_HOSTROWS", 1) != 0;
    g_tc.async = env_int("TOTK_TEX_ASYNC", 1) != 0;
    g_tc.tail = env_int("TOTK_TEX_TAIL", 128);
    if (g_tc.tail < 4) g_tc.tail = 4;
    int cpus = vkb_cpu_count(), th = cpus >= 16 ? 4 : cpus >= 8 ? 3 : 2;
    g_tc.threads = env_int("TOTK_TEX_STREAM_THREADS", th);
    if (g_tc.threads < 1) g_tc.threads = 1;
    if (g_tc.threads > 8) g_tc.threads = 8;
    double g = getenv("TOTK_TEX_GRACE_MS") && *getenv("TOTK_TEX_GRACE_MS") ? atof(getenv("TOTK_TEX_GRACE_MS")) : 2.0;
    g_tc.grace_us = g < 0 ? 0 : g > 50 ? 50000 : (int)(g * 1000);
    int b = env_int("TOTK_TEX_BUDGET_MB", 256);
    g_tc.budget = (uint64_t)(b < 8 ? 8 : b > 1536 ? 1536 : b) << 20;   // the upload memory of a submission is 2 GiB at most
    g_tc.max_pending = (uint64_t)env_int("TOTK_TEX_STREAM_MAX_MB", 1024) << 20;
    int dl = env_int("TOTK_TEX_STREAM_DEADLINE_MS", 1000);   // 0: no deadline
    g_tc.deadline_ns = dl > 0 ? (uint64_t)dl * 1000000ull : 0;
    VLOG("textures: rows in host memory %s, streaming %s (mips over %d texels on %d threads, %llu MiB a frame, grace %.1f ms, "
         "whole after %d ms), GPU writes into texture memory %s",
         g_tc.hostrows ? "on" : "off", g_tc.async ? "on" : "off", g_tc.tail, g_tc.threads, (unsigned long long)(g_tc.budget >> 20), g_tc.grace_us / 1000.0,
         dl > 0 ? dl : 0, gpu_writes_on() ? "upload again (TOTK_TEX_GPU_WRITES=1)" : "ignored (TOTK_TEX_GPU_WRITES=0)");
}
static const texcfg_t *tc(void) {
    static pthread_once_t once = PTHREAD_ONCE_INIT;
    pthread_once(&once, tc_init);
    return &g_tc;
}
// [UIT 0170] TOTK_TEX_BUDGET_MB and TOTK_TEX_GRACE_MS can change while the game runs (the in-game overlay,
// src/host/uit_live.h): read at every use with relaxed atomics, stored by the window thread
static uint64_t tc_budget(void) { return __atomic_load_n(&tc()->budget, __ATOMIC_RELAXED); }
static int tc_grace_us(void) { return __atomic_load_n(&tc()->grace_us, __ATOMIC_RELAXED); }
int vkb_tex_live(int *budget_mb, double *grace_ms) {
    if (budget_mb) *budget_mb = (int)(tc_budget() >> 20);
    if (grace_ms) *grace_ms = tc_grace_us() / 1000.0;
    return tc()->async;
}
void vkb_tex_set_budget_mb(int mb) {
    tc();
    __atomic_store_n(&g_tc.budget, (uint64_t)(mb < 8 ? 8 : mb > 1536 ? 1536 : mb) << 20, __ATOMIC_RELAXED);
}
void vkb_tex_set_grace_ms(double ms) {
    tc();
    __atomic_store_n(&g_tc.grace_us, !(ms > 0) ? 0 : ms > 50 ? 50000 : (int)(ms * 1000 + 0.5), __ATOMIC_RELAXED);
}
// counters (the per-frame and long-submission lines)
static uint64_t g_t_prep_ns, g_t_read_ns, g_t_read_bytes, g_t_copy_ns, g_t_uploads;
static uint64_t g_t_par_uploads, g_t_par_reads;   // [UIT 0140] uploads looked at / read on several threads (the log line)
static uint64_t g_s_queued, g_s_levels, g_s_bytes, g_s_done, g_s_full_ns, g_s_full_max, g_s_low_uses, g_s_grace_ns, g_s_grace_n,
                g_s_bg_ns, g_s_cancelled, g_s_pending, g_s_pending_bytes, g_s_failed, g_s_late;
static uint64_t g_t_slow;   // uploads over 20 ms (logged, the first 32)
static uint64_t g_t_blank;  // draws given an empty texture in place of a game texture (vkb_draw.c write_stage)
void vkb_tex_note_blank(const vkb_image *im, int not_uploaded) {
    uint64_t n = __atomic_fetch_add(&g_t_blank, 1, __ATOMIC_RELAXED);
    if (n < 16) VLOG("[blank texture] texture %llu (format %#x %ux%u, %u levels) drawn as an empty texture: %s", (unsigned long long)im->serial,
                     im->d.format, im->width, im->height, im->levels, not_uploaded ? "its format cannot be uploaded" : "not readable yet");
}

typedef struct sjob {
    vkb_image *im;
    nvn_texdesc d;
    const uint8_t *src;
    uint32_t width, height, layers;
    int bw, bh, srgb, fmt;
    int split, made, applied, busy, cancel;   // levels [made, split) made, [applied, levels) on the image
    int failed;                               // a level could not be made: the texture uploads whole instead (0008)
    uint64_t last_use, t0, bytes;
    uint8_t *lvl[16];
    struct sjob *next;
} sjob;
static pthread_mutex_t g_st_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_st_cv = PTHREAD_COND_INITIALIZER, g_st_done = PTHREAD_COND_INITIALIZER;
static sjob *g_sjobs;
static uint64_t g_bud_frame, g_bud_used, g_grace_used;   // this frame's streamed bytes and grace (g_up_mu)

static size_t bc_bytes(int fmt, uint32_t w, uint32_t h) { return fmt == VKB_BC_1 ? bc1_size(w, h, 1) : bc7_size(w, h, 1); }
static void level_dims(uint32_t w, uint32_t h, int level, uint32_t *lw, uint32_t *lh) {
    *lw = w >> level ? w >> level : 1;
    *lh = h >> level ? h >> level : 1;
}
static uint64_t job_level_bytes(const sjob *j, int level) {
    uint32_t lw, lh;
    level_dims(j->width, j->height, level, &lw, &lh);
    return (uint64_t)bc_bytes(j->fmt, lw, lh) * j->layers;
}
// One level of a job, every layer: the ASTC rows from game memory, the blocks from the cache (else decoded and encoded).
// NULL: cancelled, or a layer could not be read (0008: before, such a layer became zeros - transparent black blocks that
// were then applied as the texture's sharpest level; now the job fails and the texture uploads whole at its next use).
static uint8_t *make_level(sjob *j, int level, uint8_t **rows, size_t *rows_cap, uint8_t **rgba, size_t *rgba_cap) {
    uint32_t lw, lh;
    level_dims(j->width, j->height, level, &lw, &lh);
    uint32_t bx = (lw + (uint32_t)j->bw - 1) / (uint32_t)j->bw, by = (lh + (uint32_t)j->bh - 1) / (uint32_t)j->bh, pitch = bx * 16;
    size_t rb = (size_t)pitch * by, n = bc_bytes(j->fmt, lw, lh), px = (size_t)lw * lh * 4;
    uint8_t *out = malloc(n * j->layers);
    if (!out) die("texture streaming");
    if (*rows_cap < rb) { free(*rows); *rows = malloc(rb); *rows_cap = rb; if (!*rows) die("texture streaming"); }
    for (uint32_t layer = 0; layer < j->layers; layer++) {
        uint8_t *o = out + n * layer;
        if (__atomic_load_n(&j->cancel, __ATOMIC_ACQUIRE) || nvn_deswizzle(&j->d, (uint32_t)level, layer, j->src, *rows, pitch)) { free(out); return NULL; }
        vkb_bc7_key k = vkb_bc7_key_of(*rows, rb, lw, lh, 1, (uint32_t)j->bw, (uint32_t)j->bh, j->srgb);
        int f;
        size_t len;
        if (vkb_bc7_lookup(k, &f, &len) && f == j->fmt && len == n && vkb_bc7_read(k, o, n)) continue;
        if (*rgba_cap < px) { free(*rgba); *rgba = malloc(px); *rgba_cap = px; if (!*rgba) die("texture streaming"); }
        astc_decode_ldr(*rows, lw, lh, (uint32_t)j->bw, (uint32_t)j->bh, j->srgb, *rgba);
        vkb_bc7_encode_now(k, *rgba, lw, lh, 1, j->srgb, j->fmt, o);
    }
    return out;
}
static void *stream_worker(void *unused) {
    (void)unused;
    pthread_setname_np("totk-texstream");
    vkb_thread_below_normal();   // the game's and the recording threads come first
    uint8_t *rows = NULL, *rgba = NULL;
    size_t rows_cap = 0, rgba_cap = 0;
    pthread_mutex_lock(&g_st_mu);
    for (;;) {
        // next: a texture drawn in the last frame before others, then the smallest level waiting (mips small to large
        // across all textures), then the most recently drawn
        sjob *best = NULL;
        uint64_t now = vkb_frame(), bb = 0;
        int bon = 0;
        for (sjob *j = g_sjobs; j; j = j->next) {
            if (j->busy || j->cancel || j->failed || j->made <= 0) continue;
            uint64_t lu = __atomic_load_n(&j->last_use, __ATOMIC_RELAXED), b = job_level_bytes(j, j->made - 1);
            int on = lu + 1 >= now;
            if (!best || on > bon || (on == bon && (b < bb || (b == bb && lu > best->last_use)))) { best = j; bb = b; bon = on; }
        }
        if (!best) { pthread_cond_wait(&g_st_cv, &g_st_mu); continue; }
        best->busy = 1;
        int level = best->made - 1;
        pthread_mutex_unlock(&g_st_mu);
        uint64_t t0 = runtime_now_ns();
        uint8_t *out = make_level(best, level, &rows, &rows_cap, &rgba, &rgba_cap);
        if (rows_cap > (64u << 20)) { free(rows); rows = NULL; rows_cap = 0; }
        if (rgba_cap > (64u << 20)) { free(rgba); rgba = NULL; rgba_cap = 0; }
        pthread_mutex_lock(&g_st_mu);
        g_s_bg_ns += runtime_now_ns() - t0;
        if (out) {
            best->lvl[level] = out;
            __atomic_store_n(&best->made, level, __ATOMIC_RELEASE);
        } else if (!best->cancel) best->failed = 1;   // its next use uploads it whole (stream_use)
        best->busy = 0;
        pthread_cond_broadcast(&g_st_done);
    }
    return NULL;
}
static void stream_start_threads(void) {
    for (int i = 0; i < tc()->threads; i++) {
        pthread_t t;
        if (pthread_create(&t, NULL, stream_worker, NULL) == 0) pthread_detach(t);
    }
}
static void job_unlink(sjob *j) {   // g_st_mu held
    sjob **p = &g_sjobs;
    while (*p && *p != j) p = &(*p)->next;
    if (*p) *p = j->next;
    g_s_pending--;
    g_s_pending_bytes -= j->bytes;
}
// The job's levels are not wanted (the entry goes, or the texture changed and uploads whole): waits for a level being made.
static void stream_cancel(vkb_image *im) {
    sjob *j = __atomic_load_n((sjob **)&im->stream, __ATOMIC_ACQUIRE);
    if (!j) return;
    pthread_mutex_lock(&g_st_mu);
    j->cancel = 1;
    while (j->busy) pthread_cond_wait(&g_st_done, &g_st_mu);
    job_unlink(j);
    g_s_cancelled++;
    __atomic_store_n((sjob **)&im->stream, NULL, __ATOMIC_RELEASE);
    pthread_mutex_unlock(&g_st_mu);
    for (int l = 0; l < 16; l++) free(j->lvl[l]);
    free(j);
}
static void budget_roll(void) {   // g_up_mu held
    uint64_t f = vkb_frame();
    if (f != g_bud_frame) { g_bud_frame = f; g_bud_used = 0; g_grace_used = 0; }
}
// The levels made since the last time, as many as this frame's budget takes (the first always), onto the image. g_up_mu held.
static int stream_apply(VkCommandBuffer cb, vkb_image *im, sjob *j) {
    pthread_mutex_lock(&g_st_mu);
    int from = j->applied, to = j->made, low = from;
    uint64_t bytes = 0;
    while (low > to) {
        uint64_t b = job_level_bytes(j, low - 1);
        if (g_bud_used + bytes + b > tc_budget() && (bytes || g_bud_used)) break;
        bytes += b;
        low--;
    }
    uint8_t *bufs[16] = {0};
    for (int l = low; l < from; l++) { bufs[l] = j->lvl[l]; j->lvl[l] = NULL; }
    pthread_mutex_unlock(&g_st_mu);
    if (low == from) return 0;
    uint64_t t0 = runtime_now_ns();
    vkb_image_use(cb, im, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    for (int l = from - 1; l >= low; l--) {
        uint32_t lw, lh;
        level_dims(j->width, j->height, l, &lw, &lh);
        size_t n = bc_bytes(j->fmt, lw, lh);
        for (uint32_t layer = 0; layer < j->layers; layer++) {
            VkBuffer buf;
            VkDeviceSize off;
            uint8_t *dst = vkb_ring_alloc(n, 16, &buf, &off);
            memcpy(dst, bufs[l] + n * layer, n);
            VkBufferImageCopy r = {.bufferOffset = off, .bufferRowLength = (lw + 3) / 4 * 4, .bufferImageHeight = (lh + 3) / 4 * 4,
                                   .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, (uint32_t)l, layer, 1}, .imageExtent = {lw, lh, 1}};
            vkb_checkpoint("upload texture (streamed mip)", NULL, NULL);
            vk.vkCmdCopyBufferToImage(cb, buf, im->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r);
        }
        free(bufs[l]);
    }
    g_bud_used += bytes;
    uint64_t now = runtime_now_ns();
    pthread_mutex_lock(&g_st_mu);
    g_t_copy_ns += now - t0;
    g_s_levels += (uint64_t)(from - low);
    g_s_bytes += bytes;
    j->applied = low;
    __atomic_store_n(&im->resident_base, (uint32_t)low, __ATOMIC_RELEASE);   // after the copies: views made from here see them
    int done = low == 0;
    if (done) {
        job_unlink(j);
        uint64_t w = now - j->t0;
        g_s_done++;
        g_s_full_ns += w;
        if (w > g_s_full_max) g_s_full_max = w;
        __atomic_store_n((sjob **)&im->stream, NULL, __ATOMIC_RELEASE);
    }
    pthread_mutex_unlock(&g_st_mu);
    if (done) free(j);
    return 1;
}
// A use of a streaming texture: its new levels applied (into the prologue on several threads). -1: no job any more, or
// the texture changed (the job is cancelled): the caller uploads as usual.
static int stream_use(VkCommandBuffer cb, vkb_image *im) {
    int mt = vkb_mt_recording();
    pthread_mutex_lock(&g_up_mu);
    sjob *j = __atomic_load_n((sjob **)&im->stream, __ATOMIC_ACQUIRE);
    if (!j) { pthread_mutex_unlock(&g_up_mu); return -1; }
    if (!im->upload_stamp || vkb_tex_stale(im)) { stream_cancel(im); pthread_mutex_unlock(&g_up_mu); return -1; }
    budget_roll();
    __atomic_store_n(&j->last_use, vkb_frame(), __ATOMIC_RELAXED);
    pthread_mutex_lock(&g_st_mu);
    if (j->made >= j->applied && j->busy && g_grace_used < (uint64_t)tc_grace_us() * 1000) {   // nearly there: wait a little
        uint64_t t0 = runtime_now_ns(), left = (uint64_t)tc_grace_us() * 1000 - g_grace_used;
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        uint64_t ns = (uint64_t)ts.tv_nsec + left;
        ts.tv_sec += (time_t)(ns / 1000000000ull);
        ts.tv_nsec = (long)(ns % 1000000000ull);
        while (j->made >= j->applied && j->busy)
            if (pthread_cond_timedwait(&g_st_done, &g_st_mu, &ts)) break;
        uint64_t w = runtime_now_ns() - t0;
        g_grace_used += w;
        g_s_grace_ns += w;
        g_s_grace_n++;
    }
    // 0008: a job that failed, or whose texture has been drawn at a lower mip for longer than TOTK_TEX_STREAM_DEADLINE_MS,
    // is given up: the texture uploads whole now (its levels come from the cache, already in memory: a short wait)
    // rather than staying blurred or wrong for long.
    uint64_t age = runtime_now_ns() - j->t0;
    int give_up = j->failed || (tc()->deadline_ns && age > tc()->deadline_ns);
    int ready = !give_up && j->made < j->applied;
    pthread_mutex_unlock(&g_st_mu);
    if (give_up) {
        __atomic_fetch_add(j->failed ? &g_s_failed : &g_s_late, 1, __ATOMIC_RELAXED);
        stream_cancel(im);
        vkb_cpuw_mark(&im->cpu_written);   // the caller's upload takes every level (resident_base goes to 0 after it)
        pthread_mutex_unlock(&g_up_mu);
        return -1;
    }
    int r = 0;
    if (ready) {
        if (mt) vkb_pro_begin();
        r = stream_apply(mt ? vkb_cmd() : cb, im, j);
        if (mt) vkb_pro_end();
    }
    if (__atomic_load_n((sjob **)&im->stream, __ATOMIC_ACQUIRE)) __atomic_fetch_add(&g_s_low_uses, 1, __ATOMIC_RELAXED);
    pthread_mutex_unlock(&g_up_mu);
    return r;
}
int vkb_tex_stream_due(const vkb_image *im) {
    sjob *j = __atomic_load_n((sjob *const *)&im->stream, __ATOMIC_ACQUIRE);
    if (j && (__atomic_load_n(&j->failed, __ATOMIC_ACQUIRE) || (tc()->deadline_ns && runtime_now_ns() - j->t0 > tc()->deadline_ns))) return 1;
    return j && __atomic_load_n(&j->made, __ATOMIC_ACQUIRE) < j->applied && (g_bud_frame != vkb_frame() || g_bud_used < tc_budget());
}

// [UIT native] Uploads from game memory for the long-frame log (mtl_pkg_stats / mtl_upload_bytes / mtl_upload_ns_take,
// which were the null backend's stubs): first uploads, repeated ones (the game changed the texture), the bytes staged
// on this thread meanwhile (the upload ring) and the time.
static _Atomic uint64_t g_up_first, g_up_again, g_up_bytes, g_up_ns, g_up_ns_taken;
static void up_count(int r, uint64_t stamp, uint64_t bytes, uint64_t ns) {
    if (!r) return;
    atomic_fetch_add_explicit(stamp ? &g_up_again : &g_up_first, 1, memory_order_relaxed);
    atomic_fetch_add_explicit(&g_up_bytes, bytes, memory_order_relaxed);
    atomic_fetch_add_explicit(&g_up_ns, ns, memory_order_relaxed);
}
void mtl_pkg_stats(uint64_t *uploads, uint64_t *reuploads) { *uploads = atomic_load(&g_up_first); *reuploads = atomic_load(&g_up_again); }
uint64_t mtl_upload_bytes(void) { return atomic_load(&g_up_bytes); }
uint64_t mtl_upload_ns_take(void) { uint64_t n = atomic_load(&g_up_ns); return n - atomic_exchange(&g_up_ns_taken, n); }
int vkb_tex_upload(VkCommandBuffer cb, vkb_image *im) {
    if (__atomic_load_n((sjob **)&im->stream, __ATOMIC_ACQUIRE)) {   // streaming its larger mips
        uint64_t t0 = runtime_now_ns();
        int r = stream_use(cb, im);
        if (r >= 0) { vkb_step_add(PK_UPLOAD, t0); return r; }
    }
    uint64_t stamp = __atomic_load_n(&im->upload_stamp, __ATOMIC_ACQUIRE);
    if (vkb_mt_recording() && __atomic_load_n(&im->uploading, __ATOMIC_ACQUIRE)) {   // another thread is uploading it: its
        pthread_mutex_lock(&g_up_mu);                                                 // image may change (BC7), wait
        pthread_mutex_unlock(&g_up_mu);
        stamp = __atomic_load_n(&im->upload_stamp, __ATOMIC_ACQUIRE);
    }
    if (!im->storage || im->gpu_written || (stamp && !vkb_tex_stale(im))) return 0;   // the common case: nothing to do
    uint64_t t0 = runtime_now_ns(), rb0 = vkb_ring_bytes_tl;
    int r;
    if (vkb_mt_recording()) {
        pthread_mutex_lock(&g_up_mu);
        vkb_pro_begin();
        r = tex_upload(vkb_cmd(), im);   // checks again: another thread may have uploaded it meanwhile
        vkb_pro_end();
        pthread_mutex_unlock(&g_up_mu);
    } else r = tex_upload(cb, im);
    vkb_step_add(PK_UPLOAD, t0);
    up_count(r, stamp, vkb_ring_bytes_tl - rb0, runtime_now_ns() - t0);
    uint64_t ms = (runtime_now_ns() - t0) / 1000000ull;
    if (r && ms >= 20) {   // 0008: which textures make long submissions (test4: 0.2-0.6 s of uploads with no ASTC)
        uint64_t n = __atomic_fetch_add(&g_t_slow, 1, __ATOMIC_RELAXED);
        if (n < 32) VLOG("[slow upload] texture %llu: %llu ms (format %#x %ux%ux%u, %u levels, %s%s)", (unsigned long long)im->serial,
                         (unsigned long long)ms, im->d.format, im->width, im->height, im->depth > 1 ? im->depth : im->layers, im->levels,
                         im->astc ? (im->bc7 ? "ASTC as BC7" : "ASTC decoded") : "uncompressed / BCn",
                         im->vpool_gen ? ", virtual pool: gathered" : "");
    }
    return r;
}
int vkb_tex_upload_full(VkCommandBuffer cb, vkb_image *im) {   // copies read or keep the contents: every level first
    if (__atomic_load_n((sjob **)&im->stream, __ATOMIC_ACQUIRE)) {
        pthread_mutex_lock(&g_up_mu);
        if (im->stream) { stream_cancel(im); vkb_cpuw_mark(&im->cpu_written); }   // uploads whole below (from the cache, now in memory)
        pthread_mutex_unlock(&g_up_mu);
    }
    return vkb_tex_upload(cb, im);
}
void vkb_tex_counters(uint64_t v[8]) {
    v[0] = __atomic_load_n(&g_t_prep_ns, __ATOMIC_RELAXED); v[1] = __atomic_load_n(&g_t_read_ns, __ATOMIC_RELAXED);
    v[2] = __atomic_load_n(&g_t_read_bytes, __ATOMIC_RELAXED); v[3] = __atomic_load_n(&g_t_copy_ns, __ATOMIC_RELAXED);
    v[4] = __atomic_load_n(&g_t_uploads, __ATOMIC_RELAXED); v[5] = __atomic_load_n(&g_s_levels, __ATOMIC_RELAXED);
    v[6] = __atomic_load_n(&g_s_bytes, __ATOMIC_RELAXED); v[7] = __atomic_load_n(&g_s_grace_ns, __ATOMIC_RELAXED);
}
void vkb_tex_stream_log(double f) {
    static uint64_t p[8], pq, pd, pfull, plow, pgn, pbg, pc;
    uint64_t v[8];
    vkb_tex_counters(v);
    static uint64_t ppu, ppr;
    uint64_t pu = __atomic_load_n(&g_t_par_uploads, __ATOMIC_RELAXED), pr = __atomic_load_n(&g_t_par_reads, __ATOMIC_RELAXED);
    VLOG("per frame: texture uploads %.1f: first look (deswizzle + key) %.2f ms, cache reads %.2f ms (%.2f MiB), into upload memory %.2f ms; "
         "on %d threads: %.2f looks, %.2f reads",
         (double)(v[4] - p[4]) / f, (double)(v[0] - p[0]) / 1e6 / f, (double)(v[1] - p[1]) / 1e6 / f, (double)(v[2] - p[2]) / 1048576.0 / f,
         (double)(v[3] - p[3]) / 1e6 / f, vkb_pfor_threads(), (double)(pu - ppu) / f, (double)(pr - ppr) / f);
    ppu = pu; ppr = pr;
    if (tc()->async) {
        pthread_mutex_lock(&g_st_mu);
        uint64_t q = g_s_queued, d = g_s_done, full = g_s_full_ns, gn = g_s_grace_n, bg = g_s_bg_ns, c = g_s_cancelled, mx = g_s_full_max,
                 pend = g_s_pending, pendb = g_s_pending_bytes;
        pthread_mutex_unlock(&g_st_mu);
        uint64_t low = __atomic_load_n(&g_s_low_uses, __ATOMIC_RELAXED);
        VLOG("per frame: textures streamed: %.1f queued, %.1f levels applied (%.2f MiB), %.1f at full detail (after %.0f ms on average, "
             "longest %.0f ms since the last line), %.1f uses at a lower mip, grace waits %.2f ms / %.1f, background %.1f ms, %.1f cancelled; "
             "waiting now %llu (%.1f MiB)", (double)(q - pq) / f, (double)(v[5] - p[5]) / f, (double)(v[6] - p[6]) / 1048576.0 / f,
             (double)(d - pd) / f, d > pd ? (double)(full - pfull) / 1e6 / (double)(d - pd) : 0.0, (double)mx / 1e6,
             (double)(low - plow) / f, (double)(v[7] - p[7]) / 1e6 / f, (double)(gn - pgn) / f, (double)(bg - pbg) / 1e6 / f,
             (double)(c - pc) / f, (unsigned long long)pend, pendb / 1048576.0);
        pq = q; pd = d; pfull = full; plow = low; pgn = gn; pbg = bg; pc = c;
        pthread_mutex_lock(&g_st_mu);
        g_s_full_max = 0;   // the longest per line
        pthread_mutex_unlock(&g_st_mu);
    }
    static uint64_t pgs, pf, pl, psl, pbl;
    uint64_t gs = __atomic_load_n(&g_t_gpu_stale, __ATOMIC_RELAXED), fl = __atomic_load_n(&g_s_failed, __ATOMIC_RELAXED),
             lt = __atomic_load_n(&g_s_late, __ATOMIC_RELAXED), sl = __atomic_load_n(&g_t_slow, __ATOMIC_RELAXED),
             bl = __atomic_load_n(&g_t_blank, __ATOMIC_RELAXED);
    VLOG("per frame: textures uploaded again after GPU writes %.2f; streams given up %.2f (failed) / %.2f (late: uploaded whole); "
         "uploads over 20 ms %.2f; draws with an empty texture %.2f", (double)(gs - pgs) / f, (double)(fl - pf) / f, (double)(lt - pl) / f,
         (double)(sl - psl) / f, (double)(bl - pbl) / f);
    pgs = gs; pf = fl; pl = lt; psl = sl; pbl = bl;
    memcpy(p, v, sizeof p);
}
// ---- ASTC as BC7 (vkb_bc7.c). At its first upload an ASTC texture becomes a BC7 image when the cache holds every one of
// its surfaces (no view of it exists yet, nothing has drawn with it); its contents then come from the cache. Otherwise
// it stays RGBA8, decoded as before, and each surface is queued for encoding: the next time it is in the cache.
// [temporary] 1D and 3D ASTC textures stay RGBA8.
static int to_bc7(vkb_image *im, int srgb, int fmt) {
    entry *e = (entry *)im;
    if (__atomic_load_n(&e->nviews, __ATOMIC_ACQUIRE) || im->gpu_written || im->depth > 1 || im->d.target == NVN_TEXTURE_TARGET_1D ||
        im->d.target == NVN_TEXTURE_TARGET_1D_ARRAY) return 0;
    VkFormat f = fmt == VKB_BC_1 ? (srgb ? VK_FORMAT_BC1_RGB_SRGB_BLOCK : VK_FORMAT_BC1_RGB_UNORM_BLOCK)
                                 : (srgb ? VK_FORMAT_BC7_SRGB_BLOCK : VK_FORMAT_BC7_UNORM_BLOCK);
    VkFormatProperties fp;
    vk.vkGetPhysicalDeviceFormatProperties(vkc.gpu, f, &fp);
    if (!(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT)) return 0;
    VkImageUsageFlags usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    VkImageCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .flags = im->layers >= 6 && im->width == im->height ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0,
        .imageType = VK_IMAGE_TYPE_2D, .format = f, .extent = {im->width, im->height, 1}, .mipLevels = im->levels,
        .arrayLayers = im->layers, .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = usage,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
    VkImage img;
    if (vk.vkCreateImage(vkc.device, &ci, NULL, &img) != VK_SUCCESS) return 0;
    VkDeviceMemory mem;
    void *alloc;
    if (!vkb_mem_image(img, usage, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &mem, &alloc)) { vk.vkDestroyImage(vkc.device, img, NULL); return 0; }   // [UIT 0075]
    vkb_destroy_image_later(im->img, im->mem, im->alloc);   // the RGBA8 image no command has used
    im->img = img; im->mem = mem; im->alloc = alloc; im->format = f; im->usage = usage; im->compressed = 1; im->bc7 = fmt == VKB_BC_1 ? 2 : 1;
    im->layout = VK_IMAGE_LAYOUT_UNDEFINED;
    return 1;
}
typedef struct { const uint8_t *rows; size_t roff; uint8_t *rgba, *ring; vkb_bc7_key k; uint32_t level, layer, lw, lh, ld; int have, fmt; VkDeviceSize slice; } surf_t;
static uint64_t g_bc7_images;   // images made BC7 (the log line)
static _Thread_local uint8_t *t_rows;   // TOTK_TEX_HOSTROWS: the ASTC rows of an upload, in ordinary memory
static _Thread_local size_t t_rows_cap;
// [UIT 0140] the deswizzle + key + cache look of one surface, on any thread (vkb_pfor): each touches only its own
// surface and rows; vkb_bc7_lookup takes the cache's lock
typedef struct { const nvn_texdesc *d; const uint8_t *src; surf_t *sf; int bw, bh, srgb; } look_job;
static void look_one(void *c, int i) {
    look_job *x = c;
    surf_t *s = &x->sf[i];
    uint8_t *rows = (uint8_t *)s->rows;
    s->rows = NULL;
    VkDeviceSize bytes = s->slice * s->ld;
    size_t pitch = (size_t)((s->lw + x->bw - 1) / x->bw) * 16;
    if (nvn_deswizzle(x->d, s->level, s->layer, x->src, rows, pitch)) return;
    s->rows = rows;
    s->k = vkb_bc7_key_of(rows, (size_t)bytes, s->lw, s->lh, s->ld, (uint32_t)x->bw, (uint32_t)x->bh, x->srgb);
    size_t len;
    s->have = vkb_bc7_lookup(s->k, &s->fmt, &len) &&
              len == (s->fmt == VKB_BC_1 ? bc1_size(s->lw, s->lh, s->ld) : bc7_size(s->lw, s->lh, s->ld));
}
// [UIT 0140] surfaces of an upload with at least this many bytes of rows are looked at on several threads
enum { LOOK_PAR_BYTES = 256 << 10 };
// Levels [lv0, lv1) of every layer into sf[*k...]: rows, key, whether the cache has it. 0 if any surface is not there.
// [UIT 0140] in three steps: the places of the rows (here: upload memory is not shared between threads), the
// deswizzle + key + look of each surface (look_one, on vkb_pfor's threads when the rows are large), then the answer in
// surface order - the same rows, keys and answer as the one loop before.
static int astc_look(vkb_image *im, const uint8_t *src, int bw, int bh, int srgb, uint32_t lv0, uint32_t lv1, surf_t *sf, int *k, int *fmt, size_t *pat) {
    const nvn_texdesc *d = &im->d;
    int all = 1, host = tc()->hostrows, k0 = *k;
    size_t need = 0, at = *pat;   // where these rows start in t_rows (after rows an earlier call gathered)
    for (uint32_t level = lv0; level < lv1; level++) {
        uint32_t lw = im->width >> level ? im->width >> level : 1, lh = im->height >> level ? im->height >> level : 1;
        uint32_t ld = im->depth >> level ? im->depth >> level : 1;
        need += (size_t)((lw + bw - 1) / bw) * 16 * ((lh + bh - 1) / bh) * ld * im->layers;
    }
    if (host) {   // (offsets kept: the buffer may move)
        if (at + need > t_rows_cap) {
            uint8_t *nb = realloc(t_rows, at + need);
            if (!nb) die("ASTC rows");
            t_rows = nb;
            t_rows_cap = at + need;
        }
    }
    for (uint32_t level = lv0; level < lv1; level++) {
        uint32_t lw = im->width >> level ? im->width >> level : 1, lh = im->height >> level ? im->height >> level : 1;
        uint32_t ld = im->depth >> level ? im->depth >> level : 1;
        uint32_t bx = (lw + bw - 1) / bw, by = (lh + bh - 1) / bh, pitch = bx * 16;
        VkDeviceSize slice = (VkDeviceSize)pitch * by, bytes = slice * ld;
        for (uint32_t layer = 0; layer < im->layers; layer++, (*k)++) {
            uint8_t *rows;
            surf_t *s = &sf[*k];
            *s = (surf_t){NULL, 0, NULL, NULL, {0, 0}, level, layer, lw, lh, ld, 0, 0, slice};
            if (host) { rows = t_rows + at; s->roff = at; at += (size_t)bytes; }
            else { VkBuffer buf; VkDeviceSize off; rows = vkb_ring_alloc(bytes, 16, &buf, &off); }
            s->rows = rows;   // where look_one deswizzles to (NULL again if it cannot)
        }
    }
    look_job job = {d, src, sf + k0, bw, bh, srgb};
    int n = *k - k0;
    if (need >= LOOK_PAR_BYTES && n > 1 && vkb_pfor_threads() > 1) { vkb_pfor(n, look_one, &job); __atomic_fetch_add(&g_t_par_uploads, 1, __ATOMIC_RELAXED); }
    else for (int i = 0; i < n; i++) look_one(&job, i);
    for (int i = k0; i < *k; i++) {
        surf_t *s = &sf[i];
        if (!s->rows) { all = 0; continue; }
        all &= s->have && (*fmt < 0 || s->fmt == *fmt);   // one format for the whole image
        if (s->have && *fmt < 0) *fmt = s->fmt;
    }
    *pat = at;
    return all;
}
// [UIT 0140] the cache read of one surface of a BC7 upload into its upload memory, on any thread (vkb_bc7_read takes
// the cache's lock to find the entry and copies outside it)
typedef struct { const surf_t *sf; int bcf; int *got; uint8_t **at; } read_job;
static void read_one(void *c, int i) {
    read_job *x = c;
    const surf_t *s = &x->sf[i];
    if (!s->rows) return;
    size_t n = x->bcf == VKB_BC_1 ? bc1_size(s->lw, s->lh, s->ld) : bc7_size(s->lw, s->lh, s->ld);
    x->got[i] = vkb_bc7_read(s->k, x->at[i], n);
}
// may_stream: the first upload of a texture in ordinary game memory. Its levels over TOTK_TEX_TAIL texels then stream
// in (when its small levels are all in the cache in one format and it became BC7 / BC1).
static void astc_upload(VkCommandBuffer cb, vkb_image *im, const uint8_t *src, int bw, int bh, int srgb, int may_stream) {
    const nvn_texdesc *d = &im->d;
    int ns = (int)(im->levels * im->layers), k = 0, fmt = -1;
    uint64_t tp = runtime_now_ns();
    surf_t *sf = calloc((size_t)(ns ? ns : 1), sizeof *sf);
    if (!sf) die("ASTC surfaces");
    uint32_t split = 0;   // levels [0, split) streamed
    if (may_stream && tc()->async && !im->bc7 && im->depth <= 1 && im->levels >= 2 && im->levels <= 16 && !im->gpu_written &&
        d->target != NVN_TEXTURE_TARGET_1D && d->target != NVN_TEXTURE_TARGET_1D_ARRAY && d->target != NVN_TEXTURE_TARGET_3D) {
        while (split < im->levels - 1 && ((im->width >> split) > (uint32_t)tc()->tail || (im->height >> split) > (uint32_t)tc()->tail)) split++;
        if (split && __atomic_load_n(&g_s_pending_bytes, __ATOMIC_RELAXED) > tc()->max_pending) split = 0;   // enough waiting: whole
    }
    size_t at = 0;
    int all = astc_look(im, src, bw, bh, srgb, split, im->levels, sf, &k, &fmt, &at);
    int streamed = 0;
    if (!im->bc7 && all && ns && to_bc7(im, srgb, fmt)) {
        g_bc7_images++;
        streamed = split > 0;
        static int said;   // TOTK_BC7_LOG=1: every image made BC7 (development)
        if (getenv("TOTK_BC7_LOG") && said++ < 400)
            VLOG("[bc7] texture %llu format %#x %ux%u target %d levels %u layers %u%s%s", (unsigned long long)im->serial, d->format, im->width,
                 im->height, d->target, im->levels, im->layers, srgb ? " sRGB" : "", streamed ? " (larger mips streamed)" : "");
    }
    if (split && !streamed) {   // whole after all: the larger levels too (with the others: the image stays RGBA8)
        surf_t *tail = malloc(sizeof *sf * (size_t)k);
        if (!tail) die("ASTC surfaces");
        memcpy(tail, sf, sizeof *sf * (size_t)k);
        int nt = k, nk = 0, f2 = -1;
        astc_look(im, src, bw, bh, srgb, 0, split, sf, &nk, &f2, &at);
        memcpy(sf + nk, tail, sizeof *sf * (size_t)nt);
        free(tail);
        k = nk + nt;
        split = 0;
    }
    if (tc()->hostrows)   // the rows' places, now that the buffer no longer moves
        for (int i = 0; i < k; i++) if (sf[i].rows) sf[i].rows = t_rows + sf[i].roff;
    __atomic_fetch_add(&g_t_prep_ns, runtime_now_ns() - tp, __ATOMIC_RELAXED);
    __atomic_fetch_add(&g_t_uploads, 1, __ATOMIC_RELAXED);
    if (!im->bc7 && getenv("TOTK_BC7_LOG")) {   // not in the cache (development: where the rest of the ASTC comes from)
        int have = 0;
        for (int i = 0; i < k; i++) have += sf[i].have;
        static int said;
        if (said++ < 400)
            VLOG("[bc7] texture %llu format %#x %ux%u target %d levels %u layers %u: %d of %d surfaces in the cache, storage %p",
                 (unsigned long long)im->serial, d->format, im->width, im->height, d->target, im->levels, im->layers, have, ns, (const void *)im->storage);
    }
    vkb_image_use(cb, im, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    if (im->bc7) {   // blocks from the cache (a surface missing from it: encoded now)
        int bcf = im->bc7 == 2 ? VKB_BC_1 : VKB_BC_7;
        // [UIT 0140] the upload memory of every surface first (in the same order as before), then the cache reads
        // (read_one, on vkb_pfor's threads for a large texture: a copy out of the mapped pack each, under the cache's
        // lock only to find the entry), then the misses and the copy commands in surface order on this thread
        read_job rj = {sf, bcf, calloc((size_t)(k ? k : 1), sizeof *rj.got), calloc((size_t)(k ? k : 1), sizeof *rj.at)};
        VkBuffer *bufs = calloc((size_t)(k ? k : 1), sizeof *bufs);
        VkDeviceSize *offs = calloc((size_t)(k ? k : 1), sizeof *offs);
        if (!rj.got || !rj.at || !bufs || !offs) die("ASTC surfaces");
        size_t total = 0;
        for (int i = 0; i < k; i++) {
            surf_t *s = &sf[i];
            if (!s->rows) continue;
            size_t n = bcf == VKB_BC_1 ? bc1_size(s->lw, s->lh, s->ld) : bc7_size(s->lw, s->lh, s->ld);
            rj.at[i] = vkb_ring_alloc(n, 16, &bufs[i], &offs[i]);
            total += n;
        }
        uint64_t tr = runtime_now_ns();
        if (total >= LOOK_PAR_BYTES && k > 1 && vkb_pfor_threads() > 1) {
            vkb_pfor(k, read_one, &rj);
            __atomic_fetch_add(&g_t_par_reads, 1, __ATOMIC_RELAXED);
        } else for (int i = 0; i < k; i++) read_one(&rj, i);
        __atomic_fetch_add(&g_t_read_ns, runtime_now_ns() - tr, __ATOMIC_RELAXED);
        for (int i = 0; i < k; i++) {
            surf_t *s = &sf[i];
            if (!s->rows) continue;
            size_t n = bcf == VKB_BC_1 ? bc1_size(s->lw, s->lh, s->ld) : bc7_size(s->lw, s->lh, s->ld);
            VkBuffer buf = bufs[i];
            VkDeviceSize off = offs[i];
            uint8_t *blocks = rj.at[i];
            int got = rj.got[i];
            if (got) __atomic_fetch_add(&g_t_read_bytes, n, __ATOMIC_RELAXED);
            else {   // decoded in ordinary memory, encoded straight into upload memory
                size_t px = (size_t)s->lw * s->lh * 4;
                uint8_t *rgba = malloc(px * s->ld);
                if (!rgba) die("ASTC decode");
                for (uint32_t z = 0; z < s->ld; z++)
                    astc_add(s->rows + s->slice * z, s->lw, s->lh, (uint32_t)bw, (uint32_t)bh, srgb, rgba + px * z);
                astc_run();
                vkb_bc7_encode_now(s->k, rgba, s->lw, s->lh, s->ld, srgb, bcf, blocks);
                free(rgba);
            }
            VkBufferImageCopy r = {.bufferOffset = off, .bufferRowLength = (s->lw + 3) / 4 * 4, .bufferImageHeight = (s->lh + 3) / 4 * 4,
                                   .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, s->level, s->layer, 1}, .imageExtent = {s->lw, s->lh, s->ld}};
            vkb_checkpoint("upload texture (BC7)", NULL, NULL);
            vk.vkCmdCopyBufferToImage(cb, buf, im->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r);
        }
        free(rj.got); free(rj.at); free(bufs); free(offs);
    } else {   // decoded to RGBA8 (all surfaces together) in ordinary memory, copied to upload memory, queued for encoding
        for (int i = 0; i < k; i++) {
            surf_t *s = &sf[i];
            if (!s->rows) continue;
            VkBuffer buf;
            VkDeviceSize off;
            size_t px = (size_t)s->lw * s->lh * 4;
            s->ring = vkb_ring_alloc((VkDeviceSize)px * s->ld, 16, &buf, &off);
            s->rgba = tc()->hostrows ? malloc(px * s->ld) : s->ring;
            if (!s->rgba) die("ASTC decode");
            for (uint32_t z = 0; z < s->ld; z++)
                astc_add(s->rows + s->slice * z, s->lw, s->lh, (uint32_t)bw, (uint32_t)bh, srgb, s->rgba + px * z);
            VkBufferImageCopy r = {.bufferOffset = off, .bufferRowLength = s->lw, .bufferImageHeight = s->lh,
                                   .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, s->level, s->layer, 1}, .imageExtent = {s->lw, s->lh, s->ld}};
            vkb_checkpoint("upload texture", NULL, NULL);
            vk.vkCmdCopyBufferToImage(cb, buf, im->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r);
        }
        astc_run();
        uint64_t tc0 = runtime_now_ns();
        for (int i = 0; i < k; i++) {
            surf_t *s = &sf[i];
            if (!s->rgba) continue;
            if (s->rgba != s->ring) memcpy(s->ring, s->rgba, (size_t)s->lw * s->lh * 4 * s->ld);
            if (!s->have) vkb_bc7_queue(s->k, s->rgba, s->lw, s->lh, s->ld, srgb);
            if (s->rgba != s->ring) free(s->rgba);
        }
        __atomic_fetch_add(&g_t_copy_ns, runtime_now_ns() - tc0, __ATOMIC_RELAXED);
    }
    free(sf);
    if (t_rows_cap > (64u << 20)) { free(t_rows); t_rows = NULL; t_rows_cap = 0; }
    if (!streamed) { __atomic_store_n(&im->resident_base, 0, __ATOMIC_RELEASE); return; }
    // the larger levels: made in the background, applied at the next uses (stream_use)
    sjob *j = calloc(1, sizeof *j);
    if (!j) die("texture streaming");
    *j = (sjob){.im = im, .d = *d, .src = im->storage, .width = im->width, .height = im->height, .layers = im->layers, .bw = bw, .bh = bh,
                .srgb = srgb, .fmt = im->bc7 == 2 ? VKB_BC_1 : VKB_BC_7, .split = (int)split, .made = (int)split, .applied = (int)split,
                .last_use = vkb_frame(), .t0 = runtime_now_ns()};
    for (int l = 0; l < (int)split; l++) j->bytes += job_level_bytes(j, l);
    __atomic_store_n(&im->resident_base, split, __ATOMIC_RELEASE);
    static pthread_once_t once = PTHREAD_ONCE_INIT;
    pthread_once(&once, stream_start_threads);
    pthread_mutex_lock(&g_st_mu);
    j->next = g_sjobs;
    g_sjobs = j;
    g_s_queued++;
    g_s_pending++;
    g_s_pending_bytes += j->bytes;
    __atomic_store_n((sjob **)&im->stream, j, __ATOMIC_RELEASE);
    pthread_cond_broadcast(&g_st_cv);
    pthread_mutex_unlock(&g_st_mu);
}
uint64_t vkb_bc7_images(void) { return g_bc7_images; }

static int tex_upload(VkCommandBuffer cb, vkb_image *im) {
    if (!im->storage || im->gpu_written) return 0;
    const nvn_texdesc *d = &im->d;
    nvn_texlayout L;
    nvn_tex_layout(d, &L);
    if (im->upload_stamp && !vkb_tex_stale(im)) return 0;   // unchanged since the upload (checked once per submission)
    im->stale = 0;
    vkb_cpuw_take(&im->cpu_written);   // [UIT 0143] before the game memory is read (vkb_cpuw.h)
    int bw = 1, bh = 1, bpb = nvn_fmt_bpb(d->format, &bw, &bh);
    // Depth textures the game fills from the CPU (D16 / D32F, stored as D32_SFLOAT here): converted to floats on the way.
    // TOTK sets one up at every camera cut of the title menu (192x192 D16, read by the chain that lights its environment
    // probes); left empty, the probe and so the scene went dark for 4-6 frames (2026-10-24). [temporary] D24 (0x33) and
    // the depth-stencil formats: the packing of their words is not settled yet.
    int depth_up = im->aspect == VK_IMAGE_ASPECT_DEPTH_BIT && (d->format == 0x32 || d->format == 0x34) && bw == 1 && bh == 1;
    if ((im->aspect != VK_IMAGE_ASPECT_COLOR_BIT && !depth_up) || !bpb || (d->format >= 0x19 && d->format <= 0x24)) {
        im->not_uploadable = 1;
        static int said; if (said++ < 4) VLOG("[temporary] texture %llu format %#x: contents not uploaded", (unsigned long long)im->serial, d->format);
        im->upload_stamp = nvn_dirty_now();
        return 0;
    }
    // First upload: does a GPU-written texture use the same memory? Such a texture (another format, flags, level count
    // or size over the same bytes; same_shape gives exactly matching ones the same image) is uploaded from game memory,
    // which has the GPU's drawing only after a copy back. [UIT 0137] Until now the copy back came only when the CPU read
    // the memory (explicit reads, TOTK_VK_IMAGE_WRITEBACK=1), so such a texture showed whatever the game memory held
    // before the GPU drew there, or the drawing as of the CPU's last read, never later drawings. 2026-10-31 D3 log: the
    // 260x260 RG8 terrain tiles the GPU draws (flags 0x200, 9 levels, 214528 bytes each, 0x35000 apart) are read by
    // 0xd / 0x6 / 0x25 textures over the same memory (lines 735-1012), and the floating ground (decals and terrain
    // overlays as flat raised slabs) is there every time. The writer is now marked (wb_alias): every submission that
    // draws it ends with its copy back like the CPU-read targets of TOTK_VK_IMAGE_WRITEBACK=2, landing marks the pages
    // GPU-written, and the reader uploads again (TOTK_TEX_GPU_WRITES). A pending writer is copied at the next submission.
    // Live writers only (refs: the game still holds them), copy-back eligible (wb_ok), at most IWB_EXPLICIT_MAX.
    // The log (first 32) gives both sides' address, flags, levels, block height, size, and the writer's state.
    if (!im->upload_stamp) {
        static int said;
        int keep = alias_copyback_on() && vkb_iwb_mode() != IWB_OFF;
        img_lock(IS_UPLOAD);
        eiter it;
        if (said < 32 || (keep && g_iwb_live)) {
            uint64_t lo = (uint64_t)(uintptr_t)im->storage;   // [UIT 0230] only the images over this texture's memory
            for (entry *e = it_first(&it, SC_UPLOAD, &g_ix_all, lo, lo + L.size); e; e = it_next(&it)) {
                if (&e->im == im || !e->im.gpu_written || !e->im.storage) continue;
                nvn_texlayout L2;
                nvn_tex_layout(&e->im.d, &L2);
                if (!(e->im.storage < im->storage + L.size && im->storage < e->im.storage + L2.size)) continue;
                int marked = 0;
                if (keep && e->im.wb_ok && (e->refs > 0 || (released_ok() && e->im.wb_pending)) && !e->im.wb_alias &&
                    L2.size <= IWB_EXPLICIT_MAX) {
                    e->im.wb_alias = marked = 1;
                    bact_fix(&e->im);   // [UIT 0243]
                    g_iwb_alias_marked++;
                    if (e->im.wb_pending) atomic_store(&g_iwb_alias_want, 1);
                    uint64_t m = g_iwb_alias_marked;
                    if (m == 1 || m == 10 || m == 100 || m == 1000 || m == 10000)
                        VLOG("[alias] %llu GPU-drawn images read by textures of another shape are now copied back after each drawing "
                             "(TOTK_VK_ALIAS_COPYBACK); %llu such copies so far", (unsigned long long)m, (unsigned long long)g_iwb_alias_copies);
                }
                if (said >= 32) continue;
                said++;
                VLOG("[alias] texture %llu (format %#x %ux%u, %u levels, flags %#x, block height %u, %llu bytes at %p) is read from "
                     "memory that GPU-written texture %llu (format %#x %ux%u, %u levels, flags %#x, block height %u, %llu bytes at %p) "
                     "uses; offset %+lld; writer: %s, copy back %s%s, last drawn #%llu%s", (unsigned long long)im->serial, d->format,
                     im->width, im->height, im->levels, d->flags, L.tileH, (unsigned long long)L.size, (const void *)im->storage,
                     (unsigned long long)e->im.serial, e->im.d.format, e->im.width, e->im.height, e->im.levels, e->im.d.flags, L2.tileH,
                     (unsigned long long)L2.size, (const void *)e->im.storage, (long long)(im->storage - e->im.storage),
                     e->refs > 0 ? "live" : "released", e->im.wb_ok ? "possible" : e->im.wb_fit ? "possible (setting off)" : "not possible",
                     e->im.wb_pending ? ", pending (game memory lacks its last drawing)" : "", (unsigned long long)e->im.write_seq,
                     marked ? "; now copied back after each drawing (TOTK_VK_ALIAS_COPYBACK)" : e->im.wb_alias ? "; already copied back after each drawing" : "");
            }
            it_end(&it);
        }
        img_unlock();
    }
    int first = !im->upload_stamp;   // (streaming: the first upload only)
    __atomic_store_n(&im->uploading, 1, __ATOMIC_SEQ_CST);   // before the stamp: a thread that sees the stamp waits (vkb_tex_upload)
    im->gpu_stamp = nvn_gpu_write_now();   // (likewise for GPU writes copied back meanwhile)
    __atomic_store_n(&im->upload_stamp, nvn_dirty_now(), __ATOMIC_RELEASE);   // before reading: a write during the upload shows as dirty next time
    // A texture in a virtual pool (TOTK streams world textures this way): its addresses are only reserved, the bytes
    // are in physical pools' pages mapped through the page table — gathered into a copy first. It uploads again when
    // the pages behind it are written or the mapping changes ([temporary]: any MapVirtual; Vulkan sparse binding later).
    const uint8_t *src = im->storage;
    uint8_t *gathered = NULL;
    uint64_t gen = totk_vpool_generation();
    if (totk_vpool_gather((uintptr_t)im->storage, 0, NULL)) {   // (length 0: only whether it is in a virtual pool)
        gathered = malloc(L.size ? L.size : 1);
        if (!gathered) die("texture gather");
        totk_vpool_gather((uintptr_t)im->storage, L.size, gathered);
        src = gathered;
        im->vpool_gen = gen;
    }
    int srgb = d->format >= 0x87 && d->format <= 0x94;
    if (im->astc && vkb_bc7_on() && bpb == 16) {
        astc_upload(cb, im, src, bw, bh, srgb, first && !gathered);
        free(gathered);
        __atomic_store_n(&im->uploading, 0, __ATOMIC_RELEASE);
        return 1;
    }
    vkb_image_use(cb, im, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    // [UIT native] Rows that are read again on the CPU (D16 -> float, ASTC decoded) are unpacked in ordinary memory:
    // upload memory is device-local through resizable BAR, and reading it back costs ~90 ns a texel over PCIe. A
    // 960x960 x4 D16 shadow texture took 324 ms (88 ns a texel in all 22 "[slow upload] ... virtual pool: gathered" of the
    // 86-minute log, 2026-10-30), and the other recording threads waited for it (g_up_mu). Same bytes uploaded.
    // TOTK_TEX_HOSTROWS=0: as before (comparison).
    int cpu_rows = tc()->hostrows && ((depth_up && bpb == 2) || im->astc);
    uint8_t **scratch = NULL;   // the ASTC items read theirs until astc_run
    uint32_t nscratch = 0;
    if (cpu_rows && !(scratch = calloc((size_t)im->levels * im->layers, sizeof *scratch))) die("texture rows");
    for (uint32_t level = 0; level < im->levels; level++) {
        uint32_t lw = im->width >> level ? im->width >> level : 1, lh = im->height >> level ? im->height >> level : 1;
        uint32_t ld = im->depth >> level ? im->depth >> level : 1;
        uint32_t bx = (lw + bw - 1) / bw, by = (lh + bh - 1) / bh, pitch = bx * (uint32_t)bpb;
        VkDeviceSize slice = (VkDeviceSize)pitch * by, bytes = slice * ld;
        for (uint32_t layer = 0; layer < im->layers; layer++) {
            VkBuffer buf = VK_NULL_HANDLE;
            VkDeviceSize off = 0;
            uint8_t *rows;
            if (cpu_rows) {
                if (!(rows = scratch[nscratch++] = malloc(bytes ? bytes : 1))) die("texture rows");
            } else rows = vkb_ring_alloc(bytes, 16, &buf, &off);
            if (nvn_deswizzle(d, level, layer, src, rows, pitch)) continue;
            VkBufferImageCopy r = {.bufferOffset = off, .bufferRowLength = bx * (uint32_t)bw, .bufferImageHeight = by * (uint32_t)bh,
                                   .imageSubresource = {depth_up ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT, level, layer, 1},
                                   .imageExtent = {lw, lh, ld}};
            if (depth_up && bpb == 2) {   // D16 -> 32-bit float depth
                float *f = vkb_ring_alloc((VkDeviceSize)lw * lh * ld * 4, 16, &buf, &off);
                for (uint64_t k = 0; k < (uint64_t)lw * lh * ld; k++) { uint16_t v; memcpy(&v, rows + 2 * k, 2); f[k] = v / 65535.0f; }
                r.bufferOffset = off;
            }
            if (im->astc) {   // decoded texels, slice by slice
                uint8_t *rgba = vkb_ring_alloc((VkDeviceSize)lw * lh * 4 * ld, 16, &buf, &off);
                for (uint32_t z = 0; z < ld; z++)
                    astc_add(rows + slice * z, lw, lh, (uint32_t)bw, (uint32_t)bh, srgb, rgba + (VkDeviceSize)lw * lh * 4 * z);
                r.bufferOffset = off; r.bufferRowLength = lw; r.bufferImageHeight = lh;
            }
            vkb_checkpoint("upload texture", NULL, NULL);
            vk.vkCmdCopyBufferToImage(cb, buf, im->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r);
        }
    }
    astc_run();   // the copies recorded above read these texels when the submission runs
    for (uint32_t i = 0; i < nscratch; i++) free(scratch[i]);
    free(scratch);
    free(gathered);
    __atomic_store_n(&im->uploading, 0, __ATOMIC_RELEASE);
    return 1;
}

// ---------------- CPU reads of texels (NVN ReadTexels) ----------------
// TOTK reads GPU results on the CPU this way; the null backend's zeros made the title screen several times too bright.
// GPU-written images: the region is copied to upload memory and read after the GPU is done. Others: the game memory is
// the truth, de-laid-out as for an upload. Tightly packed rows unless the caller gives strides (block rows for BC).
// [UIT 0047] The GPU copies of ReadTexels / WriteTexels are recorded by the translation thread (nvn_x_call, hle_nvn.c):
// a game thread recording beside it shared g_cur, the slots, the post ops and the upload ring with it. The CPU work
// (de-layout, the game memory) stays on the calling thread. The GPU wait is inside the turn: the upload ring memory
// read back belongs to the slot, which the next submission may take as soon as its fence is reached.
void nvn_x_call(void (*fn)(void *), void *arg);
static int texels_gpu(const tex_t *t, const vkb_image *im) {
    return im && im->gpu_written && !im->astc && im->aspect == VK_IMAGE_ASPECT_COLOR_BIT && !(t->d.format >= 0x19 && t->d.format <= 0x24);
}
typedef struct {
    const tex_t *t; unsigned level, layer0; const NVNcopyRegion *r; uint8_t *dst; const uint8_t *src; size_t row, img;
    uint32_t bx, by, nz; int bw, bh, is3d; size_t wb; int done;
    int nowait;   // [UIT 0256] a queued WriteTexels: the GPU copy is not waited for (its upload memory stays the submission's until its fence)
} texels_job;
static void read_texels_gpu(void *p) {
    texels_job *a = p;
    vkb_image *im = vkb_image_of(a->t);
    if (!texels_gpu(a->t, im)) return;   // changed meanwhile: the caller reads game memory
    const NVNcopyRegion *r = a->r;
    mtl_sub_begin();
    VkCommandBuffer cb = vkb_cmd();
    VkBuffer b;
    VkDeviceSize off;
    uint8_t *px = vkb_ring_alloc((VkDeviceSize)a->wb * a->by * a->nz, 16, &b, &off);
    vkb_image_use(cb, im, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
    VkBufferImageCopy c = {.bufferOffset = off, .bufferRowLength = a->bx * (uint32_t)a->bw, .bufferImageHeight = a->by * (uint32_t)a->bh,
        .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, a->level, a->is3d ? 0 : a->layer0 + (uint32_t)r->zoffset, a->is3d ? 1 : a->nz},
        .imageOffset = {r->xoffset, r->yoffset, a->is3d ? r->zoffset : 0}, .imageExtent = {(uint32_t)r->width, (uint32_t)r->height, a->is3d ? a->nz : 1}};
    vk.vkCmdCopyImageToBuffer(cb, im->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, b, 1, &c);
    mtl_wait(mtl_sub_commit(), UINT64_MAX);
    for (uint32_t z = 0; z < a->nz; z++)
        for (uint32_t y = 0; y < a->by; y++) memcpy(a->dst + z * a->img + y * a->row, px + ((size_t)z * a->by + y) * a->wb, a->wb);
    a->done = 1;
}
static void write_texels_gpu(void *p) {
    texels_job *a = p;
    vkb_image *im = vkb_image_of(a->t);
    if (!texels_gpu(a->t, im)) return;   // the game memory has the region already
    const NVNcopyRegion *r = a->r;
    mtl_sub_begin();
    VkCommandBuffer cb = vkb_cmd();
    VkBuffer b;
    VkDeviceSize off;
    uint8_t *px = vkb_ring_alloc((VkDeviceSize)a->wb * a->by * a->nz, 16, &b, &off);
    for (uint32_t z = 0; z < a->nz; z++)
        for (uint32_t y = 0; y < a->by; y++) memcpy(px + ((size_t)z * a->by + y) * a->wb, a->src + z * a->img + y * a->row, a->wb);
    vkb_image_use(cb, im, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    VkBufferImageCopy c = {.bufferOffset = off, .bufferRowLength = a->bx * (uint32_t)a->bw, .bufferImageHeight = a->by * (uint32_t)a->bh,
        .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, a->level, a->is3d ? 0 : a->layer0 + (uint32_t)r->zoffset, a->is3d ? 1 : a->nz},
        .imageOffset = {r->xoffset, r->yoffset, a->is3d ? r->zoffset : 0}, .imageExtent = {(uint32_t)r->width, (uint32_t)r->height, a->is3d ? a->nz : 1}};
    vkb_checkpoint("write texels", NULL, NULL);
    vk.vkCmdCopyBufferToImage(cb, b, im->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
    uint64_t f = mtl_sub_commit();
    if (!a->nowait) mtl_wait(f, UINT64_MAX);
    a->done = 1;
}

void mtl_read_texels(const tex_t *t, unsigned level, unsigned layer0, const NVNcopyRegion *r, uint8_t *dst, size_t row, size_t img) {
    if (!t || !r || !dst || r->width <= 0 || r->height <= 0) return;
    int bw = 1, bh = 1, bpb = nvn_fmt_bpb(t->d.format, &bw, &bh);
    uint32_t bx = (uint32_t)(r->width + bw - 1) / bw, by = (uint32_t)(r->height + bh - 1) / bh, nz = r->depth > 0 ? (uint32_t)r->depth : 1;
    size_t wb = (size_t)bx * (size_t)bpb;
    if (!row) row = wb;
    if (!img) img = row * by;
    vkb_image *im = vkb_image_of(t);
    int is3d = t->d.target == NVN_TEXTURE_TARGET_3D;
    static int calls;
    if (calls++ < 8) VLOG("ReadTexels: texture %llu format %#x level %u region %dx%dx%d, %s", (unsigned long long)t->serial, t->d.format,
                          level, r->width, r->height, r->depth, im && im->gpu_written ? "GPU-written" : "game memory");
    if (bpb && texels_gpu(t, im)) {
        texels_job a = {t, level, layer0, r, dst, NULL, row, img, bx, by, nz, bw, bh, is3d, wb, 0, 0};
        nvn_x_call(read_texels_gpu, &a);
        if (a.done) return;
    }
    // from game memory: the whole level / layer, then the region
    uint32_t lw = t->d.width >> level ? t->d.width >> level : 1, lh = t->d.height >> level ? t->d.height >> level : 1;
    uint32_t ld = is3d ? (t->d.depth >> level ? t->d.depth >> level : 1) : 1;
    uint32_t lbx = (lw + bw - 1) / bw, lby = (lh + bh - 1) / bh, pitch = lbx * (uint32_t)bpb;
    uint8_t *lin = bpb && t->mem ? malloc((size_t)pitch * lby * ld) : NULL;
    for (uint32_t z = 0; z < nz; z++) {
        uint32_t layer = is3d ? 0 : layer0 + (uint32_t)r->zoffset + z, slice = is3d ? (uint32_t)r->zoffset + z : 0;
        int ok = lin && (is3d ? z == 0 : 1) ? !nvn_deswizzle(&t->d, level, layer, t->mem, lin, pitch) : lin != NULL;
        for (uint32_t y = 0; y < by; y++) {
            uint8_t *o = dst + z * img + y * row;
            uint32_t sy = (uint32_t)r->yoffset / bh + y, sx = (uint32_t)r->xoffset / bw;
            if (ok && sy < lby && slice < ld && sx + bx <= lbx) memcpy(o, lin + ((size_t)slice * lby + sy) * pitch + (size_t)sx * bpb, wb);
            else memset(o, 0, wb);
        }
    }
    free(lin);
    static int said;
    if (!bpb && said++ < 4) VLOG("[temporary] ReadTexels: texture %llu format %#x not read", (unsigned long long)t->serial, t->d.format);
}

// ---------------- CPU writes of texels (NVN WriteTexels) ----------------
// The region goes into the game memory in the texture's own layout (the truth for textures the CPU fills: the next
// upload sees the pages dirty); an image the GPU has written gets the region copied in directly as well. TOTK fills
// its light falloff table this way: with the null backend's no-op the lights read garbage and the title screen was
// several times too bright (2026-10-23).
// [UIT 0256] the level's bytes as mtl_write_texels de-lays it out (0: not written), for hle_nvn.c's choice to queue it
uint64_t mtl_texels_level_bytes(const tex_t *t, unsigned level) {
    int bw = 1, bh = 1, bpb = t ? nvn_fmt_bpb(t->d.format, &bw, &bh) : 0;
    if (!bpb) return 0;
    uint32_t lw = t->d.width >> level ? t->d.width >> level : 1, lh = t->d.height >> level ? t->d.height >> level : 1;
    uint32_t ld = t->d.target == NVN_TEXTURE_TARGET_3D ? (t->d.depth >> level ? t->d.depth >> level : 1) : 1;
    return (uint64_t)((lw + bw - 1) / bw) * (uint64_t)bpb * ((lh + bh - 1) / bh) * ld;
}
static void write_texels_impl(const tex_t *t, unsigned level, unsigned layer0, const NVNcopyRegion *r, const uint8_t *src, size_t row, size_t img, int queued);
void mtl_write_texels(const tex_t *t, unsigned level, unsigned layer0, const NVNcopyRegion *r, const uint8_t *src, size_t row, size_t img) {
    write_texels_impl(t, level, layer0, r, src, row, img, 0);
}
// [UIT 0256] on the translation thread, a job of the queue (TOTK_VK_WRITETEXELS_QUEUED): the same, the GPU copy not waited for
void mtl_write_texels_queued(const tex_t *t, unsigned level, unsigned layer0, const NVNcopyRegion *r, const uint8_t *src, size_t row, size_t img) {
    write_texels_impl(t, level, layer0, r, src, row, img, 1);
}
static void write_texels_impl(const tex_t *t, unsigned level, unsigned layer0, const NVNcopyRegion *r, const uint8_t *src, size_t row, size_t img, int queued) {
    if (!t || !r || !src || r->width <= 0 || r->height <= 0 || !t->mem) return;
    int bw = 1, bh = 1, bpb = nvn_fmt_bpb(t->d.format, &bw, &bh);
    if (!bpb) { static int said; if (said++ < 4) VLOG("[temporary] WriteTexels: texture %llu format %#x not written", (unsigned long long)t->serial, t->d.format); return; }
    uint32_t bx = (uint32_t)(r->width + bw - 1) / bw, by = (uint32_t)(r->height + bh - 1) / bh, nz = r->depth > 0 ? (uint32_t)r->depth : 1;
    size_t wb = (size_t)bx * (size_t)bpb;
    if (!row) row = wb;
    if (!img) img = row * by;
    int is3d = t->d.target == NVN_TEXTURE_TARGET_3D;
    uint32_t lw = t->d.width >> level ? t->d.width >> level : 1, lh = t->d.height >> level ? t->d.height >> level : 1;
    uint32_t ld = is3d ? (t->d.depth >> level ? t->d.depth >> level : 1) : 1;
    uint32_t lbx = (lw + bw - 1) / bw, lby = (lh + bh - 1) / bh, pitch = lbx * (uint32_t)bpb;
    uint32_t sx = (uint32_t)r->xoffset / bw, sy = (uint32_t)r->yoffset / bh;
    if (sx + bx > lbx || sy + by > lby) { VLOG("WriteTexels: region outside texture %llu", (unsigned long long)t->serial); return; }
    uint8_t *lin = malloc((size_t)pitch * lby * ld);
    if (!lin) die("WriteTexels");
    uint8_t *storage = (uint8_t *)t->mem;
    for (uint32_t z = 0; z < (is3d ? 1 : nz); z++) {   // 3D: one pass over the level's slices
        uint32_t layer = is3d ? 0 : layer0 + (uint32_t)r->zoffset + z;
        if (nvn_deswizzle(&t->d, level, layer, storage, lin, pitch)) { VLOG("WriteTexels: texture %llu layout not supported", (unsigned long long)t->serial); break; }
        for (uint32_t s = 0; s < (is3d ? nz : 1); s++) {
            uint32_t slice = is3d ? (uint32_t)r->zoffset + s : 0;
            if (slice >= ld) break;
            for (uint32_t y = 0; y < by; y++)
                memcpy(lin + ((size_t)slice * lby + sy + y) * pitch + (size_t)sx * bpb, src + (is3d ? s : z) * img + y * row, wb);
        }
        nvn_swizzle(&t->d, level, layer, lin, pitch, storage);
    }
    free(lin);
    vkb_image *im = vkb_image_of(t);
    if (im && !im->gpu_written) vkb_cpuw_mark(&im->cpu_written);   // the pool may not be write-watched: say so directly
    if (texels_gpu(t, im)) {
        texels_job a = {t, level, layer0, r, NULL, src, row, img, bx, by, nz, bw, bh, is3d, wb, 0, queued};
        nvn_x_call(write_texels_gpu, &a);   // (queued: on the translation thread already, so run here)
    }
    static int calls;
    if (calls++ < 8) VLOG("WriteTexels: texture %llu format %#x level %u region %dx%dx%d at %d,%d,%d%s", (unsigned long long)t->serial,
                          t->d.format, level, r->width, r->height, r->depth, r->xoffset, r->yoffset, r->zoffset,
                          im && im->gpu_written ? " (GPU-written image updated too)" : "");
}

// ---------------- development: dump the frame's render targets ----------------
// TOTK_DUMP=<present number>:<dir>: every image the GPU has written (level 0, layer 0) in a format read back here
// (RGBA8 / BGRA8 / R11G11B10F / RGBA16F) is saved as <dir>/<serial>_<format>_<w>x<h>.bmp after that present.
static float half_to_float(uint16_t h) {
    uint32_t s = (h >> 15) & 1, e = (h >> 10) & 31, m = h & 1023;
    float v = e == 0 ? m / 1024.0f / 16384.0f : e == 31 ? 65504.0f : (1.0f + m / 1024.0f) * (float)(1 << e) / 32768.0f;
    return s ? -v : v;
}
static float uf(uint32_t bits, int mbits) {   // unsigned small float: 5-bit exponent, mbits mantissa (R11G11B10F)
    uint32_t e = bits >> mbits, m = bits & ((1u << mbits) - 1);
    if (e == 0) return m / (float)(1 << mbits) / 16384.0f;
    if (e == 31) return 65504.0f;
    return (1.0f + m / (float)(1 << mbits)) * (float)(1 << e) / 32768.0f;
}
static uint8_t tone(float v) { v = v / (1.0f + v); return v <= 0 ? 0 : v >= 1 ? 255 : (uint8_t)(v * 255.0f + 0.5f); }   // HDR: Reinhard

// Bytes per texel of the formats the dump reads back (0: not dumped); depth: the float depth aspect.
static uint32_t dump_bpp(const vkb_image *im, int *depth) {
    VkFormat f = im->format;
    *depth = f == VK_FORMAT_D32_SFLOAT || f == VK_FORMAT_D32_SFLOAT_S8_UINT;   // depth: the float values, as R32F
    if (im->astc || im->depth > 1 || (im->aspect != VK_IMAGE_ASPECT_COLOR_BIT && !*depth)) return 0;
    return f == VK_FORMAT_R16G16B16A16_SFLOAT ? 8 : (f == VK_FORMAT_R8G8B8A8_UNORM || f == VK_FORMAT_R8G8B8A8_SRGB ||
           f == VK_FORMAT_B8G8R8A8_UNORM || f == VK_FORMAT_B8G8R8A8_SRGB || f == VK_FORMAT_B10G11R11_UFLOAT_PACK32 ||
           f == VK_FORMAT_R32_SFLOAT || *depth) ? 4 : f == VK_FORMAT_R16_SFLOAT ? 2 : 0;
}
// Records the copy of level 0, layer `layer` into upload memory (valid until the submission's slot is reused).
static uint8_t *dump_copy(VkCommandBuffer cb, vkb_image *im, uint32_t layer, uint32_t *bpp) {
    int depth;
    *bpp = dump_bpp(im, &depth);
    if (!*bpp) return NULL;
    VkBuffer b;
    VkDeviceSize off;
    uint8_t *px = vkb_ring_alloc((VkDeviceSize)im->width * im->height * *bpp, 16, &b, &off);
    vkb_image_use(cb, im, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
    VkBufferImageCopy r = {.bufferOffset = off, .imageSubresource = {depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT, 0, layer, 1},
                           .imageExtent = {im->width, im->height, 1}};
    vk.vkCmdCopyImageToBuffer(cb, im->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, b, 1, &r);
    return px;
}
// <dir>/<prefix><serial>_<format>_<w>x<h>.bmp (float formats Reinhard-mapped) and value statistics in the log.
static void dump_save(const char *dir, const char *prefix, const vkb_image *im, const uint8_t *px, uint32_t bpp) {
    uint32_t w = im->width, h = im->height, size = w * h * 4;
    uint8_t *out = malloc(size);
    if (!out) return;
    for (uint32_t p = 0; p < w * h; p++) {
        const uint8_t *s = px + (size_t)p * bpp;
        uint8_t *o = out + (size_t)p * 4;   // BMP: B G R A
        switch (im->format) {
        case VK_FORMAT_R16G16B16A16_SFLOAT: {
            uint16_t c[4]; memcpy(c, s, 8);
            o[2] = tone(half_to_float(c[0])); o[1] = tone(half_to_float(c[1])); o[0] = tone(half_to_float(c[2])); o[3] = 255;
            break;
        }
        case VK_FORMAT_B10G11R11_UFLOAT_PACK32: {
            uint32_t v; memcpy(&v, s, 4);
            o[2] = tone(uf(v & 0x7FF, 6)); o[1] = tone(uf((v >> 11) & 0x7FF, 6)); o[0] = tone(uf(v >> 22, 5)); o[3] = 255;
            break;
        }
        case VK_FORMAT_B8G8R8A8_UNORM: case VK_FORMAT_B8G8R8A8_SRGB: o[0] = s[0]; o[1] = s[1]; o[2] = s[2]; o[3] = 255; break;
        case VK_FORMAT_R32_SFLOAT: case VK_FORMAT_D32_SFLOAT: case VK_FORMAT_D32_SFLOAT_S8_UINT:
            { float v; memcpy(&v, s, 4); o[0] = o[1] = o[2] = tone(v < 0 ? -v : v); o[3] = 255; break; }
        case VK_FORMAT_R16_SFLOAT: { uint16_t v; memcpy(&v, s, 2); float fv = half_to_float(v); o[0] = o[1] = o[2] = tone(fv < 0 ? -fv : fv); o[3] = 255; break; }
        default: o[0] = s[2]; o[1] = s[1]; o[2] = s[0]; o[3] = 255; break;
        }
    }
    if (im->format == VK_FORMAT_R16_SFLOAT) {   // a table (light falloff): its corners and middle row
        const uint16_t *h16 = (const uint16_t *)px;
        char row[512];
        int rn = 0;
        for (uint32_t x = 0; x < w && rn < 480; x += w / 16 ? w / 16 : 1) rn += snprintf(row + rn, sizeof row - rn, " %.4g", half_to_float(h16[(size_t)(h / 2) * w + x]));
        VLOG("[dump] %stexture %llu R16F %ux%u: row %u:%s; corners %g %g %g %g", prefix, (unsigned long long)im->serial, w, h, h / 2, row,
             half_to_float(h16[0]), half_to_float(h16[w - 1]), half_to_float(h16[(size_t)(h - 1) * w]), half_to_float(h16[(size_t)h * w - 1]));
    }
    if (im->format == VK_FORMAT_R32_SFLOAT || im->format == VK_FORMAT_D32_SFLOAT || im->format == VK_FORMAT_D32_SFLOAT_S8_UINT) {   // single channel: the values
        double sum = 0; float lo = 3e38f, hi = -3e38f;
        for (uint32_t p = 0; p < w * h; p++) { float v; memcpy(&v, px + (size_t)p * 4, 4); sum += v; if (v < lo) lo = v; if (v > hi) hi = v; }
        VLOG("[dump] %stexture %llu %s %ux%u: min %g mean %g max %g, centre %g", prefix, (unsigned long long)im->serial,
             im->format == VK_FORMAT_R32_SFLOAT ? "R32F" : "depth", w, h, lo, sum / (w * h), hi, ((const float *)px)[(size_t)(h / 2) * w + w / 2]);
    }
    char path[1024];
    snprintf(path, sizeof path, "%s/%s%llu_%02x_%ux%u.bmp", dir, prefix, (unsigned long long)im->serial, im->d.format, w, h);
    FILE *f = fopen(path, "wb");
    if (f) {
        uint8_t hdr[54] = {'B', 'M'};
        uint32_t v = 54 + size; memcpy(hdr + 2, &v, 4);
        v = 54; memcpy(hdr + 10, &v, 4);
        v = 40; memcpy(hdr + 14, &v, 4);
        memcpy(hdr + 18, &w, 4);
        int32_t nh = -(int32_t)h; memcpy(hdr + 22, &nh, 4);
        hdr[26] = 1; hdr[28] = 32;
        memcpy(hdr + 34, &size, 4);
        fwrite(hdr, 1, sizeof hdr, f);
        fwrite(out, 1, size, f);
        fclose(f);
    }
    free(out);
}

// ---------------- development: snapshots in the middle of a frame ----------------
// TOTK_SNAP=<present>:<shader key prefix>:<dir>: before the first draw of that present whose vertex or fragment shader
// key starts so, every texture the draw samples (and its render targets) is copied; saved as <dir>/snap_... at the
// next present. Answers "what did this pass see", which the end-of-frame dump cannot.
typedef struct { vkb_image *im; uint8_t *px; uint32_t bpp; char prefix[48]; } snap_t;
static snap_t g_snap[64];
static int g_nsnap;
static const char *g_snap_dir;
static int g_snap_key;   // the index of the key of the draw being snapped (file names)
static long long g_snap_at_jump;   // TOTK_SNAP_ON_JUMP=1: the TOTK_SNAP frame is replaced by the present after the first upward jump (lumlog)
int vkb_tex_snap_wanted(const char *vkey, const char *fkey, uint64_t frame) {   // keys: k1,k2,...: the first draw of each
    static long long at = -1;
    static char keys[8][65];
    static int nkeys, done[8];
    if (at == -1) {
        const char *e = getenv("TOTK_SNAP");
        at = e ? atoll(e) : 0;
        const char *k = e ? strchr(e, ':') : NULL, *d = k ? strchr(k + 1, ':') : NULL;
        if (!k || !d) at = 0;
        for (const char *p = k ? k + 1 : NULL; p && p < d && nkeys < 8;) {
            size_t n = strcspn(p, ",:");
            if (n && n < 65) { memcpy(keys[nkeys], p, n); keys[nkeys++][n] = 0; }
            p += n + (p[n] == ',');
            if (*p == ':' || p >= d) break;
        }
        if (d) g_snap_dir = d + 1;
    }
    if (g_snap_at_jump) at = g_snap_at_jump;
    if (at <= 0 || frame != (uint64_t)at) return 0;
    for (int i = 0; i < nkeys; i++) {
        size_t n = strlen(keys[i]);
        if (done[i] || !((vkey && !strncmp(vkey, keys[i], n)) || (fkey && !strncmp(fkey, keys[i], n)))) continue;
        done[i] = 1;   // one draw per key
        g_snap_key = i;
        return 1;
    }
    return 0;
}
void vkb_tex_snap(VkCommandBuffer cb, vkb_image *im, const char *what) {
    if (!im) return;
    for (uint32_t layer = 0; layer < (im->layers > 1 ? 2u : 1u) && g_nsnap < 64; layer++) {
        snap_t *s = &g_snap[g_nsnap];
        s->px = dump_copy(cb, im, layer, &s->bpp);
        if (!s->px) return;
        s->im = im;
        snprintf(s->prefix, sizeof s->prefix, "snap%d_%s%s_%04llx_", g_snap_key, what, layer ? "_l1" : "",
                 (unsigned long long)((uintptr_t)im->img & 0xffff));   // the key's index; the VkImage's low bits: same image or not
        g_nsnap++;
    }
}
void vkb_tex_snap_flush(void) {
    if (!g_nsnap) return;
    mtl_finish();
    _mkdir(g_snap_dir);
    for (int k = 0; k < g_nsnap; k++) {
        dump_save(g_snap_dir, g_snap[k].prefix, g_snap[k].im, g_snap[k].px, g_snap[k].bpp);
        const vkb_image *im = g_snap[k].im;
        if (g_snap[k].bpp == 8) {   // RGBA16F: per-channel means and pixels below 1 (flags and masks)
            uint32_t np = im->width * im->height, below[4] = {0};
            double ch[4] = {0};
            for (uint32_t p = 0; p < np; p++) {
                uint16_t h[4]; memcpy(h, g_snap[k].px + (size_t)p * 8, 8);
                for (int q = 0; q < 4; q++) { float v = half_to_float(h[q]); ch[q] += v; if (v < 1.0f) below[q]++; }
            }
            VLOG("snapshot %s%ux%u: rgba %.4g %.4g %.4g %.4g, below 1: %u %u %u %u of %u", g_snap[k].prefix, im->width, im->height,
                 ch[0] / np, ch[1] / np, ch[2] / np, ch[3] / np, below[0], below[1], below[2], below[3], np);
        }
    }
    VLOG("snapshot: %d images to %s", g_nsnap, g_snap_dir);
    g_nsnap = 0;
}

// TOTK_SAMEMEM=<present>: at that present, every group of live images at exactly the same memory, with what differs
// (development: the game sees one texture where the backend has several images).
static int by_storage(const void *a, const void *b) {
    const entry *x = *(entry *const *)a, *y = *(entry *const *)b;
    return (uintptr_t)x->im.storage < (uintptr_t)y->im.storage ? -1 : (uintptr_t)x->im.storage > (uintptr_t)y->im.storage;
}
static void samemem_report(uint64_t present) {
    static long long at = -1;
    if (at == -1) { const char *e = getenv("TOTK_SAMEMEM"); at = e ? atoll(e) : 0; }
    if (at <= 0 || present != (uint64_t)at) return;
    img_lock(IS_OTHER);
    int n = 0;
    for (entry *e = g_all; e; e = e->all_next) if (e->refs && e->im.storage) n++;
    entry **v = malloc(sizeof *v * (size_t)(n ? n : 1));
    n = 0;
    for (entry *e = g_all; v && e; e = e->all_next) if (e->refs && e->im.storage) v[n++] = e;
    if (v) qsort(v, (size_t)n, sizeof *v, by_storage);
    int groups = 0;
    for (int i = 0; v && i < n;) {
        int j = i + 1;
        while (j < n && v[j]->im.storage == v[i]->im.storage) j++;
        if (j - i > 1) {
            groups++;
            for (int k = i; k < j; k++) {
                const nvn_texdesc *d = &v[k]->im.d;
                VLOG("[samemem] %p image %llu refs %d: target %u format %#x flags %#x %ux%ux%u levels %u samples %u stride %u block_h %u "
                     "swizzle %u%u%u%u layout %d %02x%02x%02x%02x%02x%02x%02x%02x gpu_written %d", v[k]->im.storage, (unsigned long long)v[k]->im.serial,
                     v[k]->refs, d->target, d->format, d->flags, d->width, d->height, d->depth, d->levels, d->samples, d->stride, d->pkg_block_h,
                     d->swizzle_r, d->swizzle_g, d->swizzle_b, d->swizzle_a, v[k]->has_layout, v[k]->layout[0], v[k]->layout[1], v[k]->layout[2],
                     v[k]->layout[3], v[k]->layout[4], v[k]->layout[5], v[k]->layout[6], v[k]->layout[7], v[k]->im.gpu_written);
            }
        }
        i = j;
    }
    VLOG("[samemem] present %llu: %d live images, %d memory addresses with more than one", (unsigned long long)present, n, groups);
    free(v);
    img_unlock();
}

// TOTK_LUMLOG=<present>:<count>: the mean of the presented image for count presents from that one (development:
// frame-to-frame flicker). Waits for the GPU at each of those presents.
// TOTK_TEXLOG=<address>,...: within the TOTK_LUMLOG presents, the mean of every layer (level 0) of the images at those
// memory addresses (development: how a GPU-built texture, such as an environment probe, evolves frame by frame).
static void texlog(uint64_t present) {
    static uint64_t addr[8], serial[8];
    static int n = -1;
    if (n < 0) {
        n = 0;
        const char *e = getenv("TOTK_TEXLOG");
        for (const char *p = e; p && *p && n < 8;) {   // s<serial>: the image with that serial (the draw trace's numbers)
            serial[n] = *p == 's' ? strtoull(p + 1, NULL, 10) : 0;
            addr[n++] = *p == 's' ? 0 : strtoull(p, NULL, 16);
            p = strchr(p, ','); if (p) p++;
        }
    }
    for (int k = 0; k < n; k++) {
        vkb_image *im = NULL;
        img_lock(IS_OTHER);
        for (entry *e = g_all; e; e = e->all_next)
            if (e->refs && (serial[k] ? e->im.serial == serial[k] : (uint64_t)(uintptr_t)e->im.storage == addr[k])) { im = &e->im; break; }
        img_unlock();
        if (!im) continue;
        char line[512];
        int ln = snprintf(line, sizeof line, "[texlog] present %llu %#llx image %llu (game memory %p, %ux%u):", (unsigned long long)present,
                          (unsigned long long)addr[k], (unsigned long long)im->serial, (const void *)im->storage, im->width, im->height);
        for (uint32_t layer = 0; layer < im->layers && layer < 6; layer++) {
            mtl_sub_begin();
            uint32_t bpp;
            uint8_t *px = dump_copy(vkb_cmd(), im, layer, &bpp);
            mtl_wait(mtl_sub_commit(), UINT64_MAX);
            if (!px) continue;
            double s = 0, ch[4] = {0};
            uint32_t np = im->width * im->height, below1[4] = {0};
            for (uint32_t p = 0; p < np; p++) {
                if (bpp == 4 && im->format == VK_FORMAT_B10G11R11_UFLOAT_PACK32) {   // 11/11/10-bit unsigned floats
                    uint32_t v; memcpy(&v, px + (size_t)p * 4, 4);
                    uint32_t c[3] = {v & 0x7ff, (v >> 11) & 0x7ff, v >> 22}, mb[3] = {6, 6, 5};
                    for (int q = 0; q < 3; q++) {
                        uint32_t e = c[q] >> mb[q], m = c[q] & ((1u << mb[q]) - 1);
                        s += (e ? ldexp(1.0 + m / (double)(1u << mb[q]), (int)e - 15) : ldexp(m / (double)(1u << mb[q]), -14)) / 3.0;
                    }
                }
                else if (bpp == 4) s += (px[(size_t)p * 4] + px[(size_t)p * 4 + 1] + px[(size_t)p * 4 + 2]) / 3.0;
                else if (bpp == 8) {   // RGBA16F: per channel below
                    uint16_t h[4]; memcpy(h, px + (size_t)p * 8, 8);
                    for (int q = 0; q < 4; q++) { float v = half_to_float(h[q]); ch[q] += v; if (v < 1.0f) below1[q]++; }
                    s += (half_to_float(h[0]) + half_to_float(h[1]) + half_to_float(h[2])) / 3.0;
                }
            }
            ln += snprintf(line + ln, sizeof line - (size_t)ln, " %.4g", np ? s / np : 0);
            if (bpp == 8 && np)
                ln += snprintf(line + ln, sizeof line - (size_t)ln, " [rgba %.3g %.3g %.3g %.3g, below 1: %u %u %u %u]", ch[0] / np, ch[1] / np, ch[2] / np,
                               ch[3] / np, below1[0], below1[1], below1[2], below1[3]);
        }
        VLOG("%s", line);
    }
}

// TOTK_MEMLOG=<address>:<bytes>[,...]: within the TOTK_LUMLOG presents (the GPU done with the frame), a summary of game
// memory there - nonzero 32-bit words, the sum of their absolute float values, an FNV hash - and with TOTK_LUMLOG_DIR the
// bytes as <dir>/mem_<address>_<present>.bin (development: GPU-built buffers such as a light list, frame by frame).
static void memlog(uint64_t present) {
    static uint64_t addr[4], len[4];
    static int n = -1;
    if (n < 0) {
        n = 0;
        for (const char *p = getenv("TOTK_MEMLOG"); p && *p && n < 4;) {
            char *e;
            addr[n] = strtoull(p, &e, 16);
            len[n] = *e == ':' ? strtoull(e + 1, &e, 0) : 0;
            if (len[n]) n++;
            p = strchr(p, ',');
            if (p) p++;
        }
    }
    for (int k = 0; k < n; k++) {
        const uint8_t *m = (const uint8_t *)(uintptr_t)addr[k];
        uint64_t nz = 0, h = 1469598103934665603ull;
        double s = 0;
        for (uint64_t i = 0; i + 4 <= len[k]; i += 4) {
            uint32_t w; float f;
            memcpy(&w, m + i, 4); memcpy(&f, m + i, 4);
            if (w) { nz++; if (f == f && f < 1e30f && f > -1e30f) s += f < 0 ? -f : f; }
            h = (h ^ w) * 1099511628211ull;
        }
        VLOG("[memlog] present %llu %#llx+%llu: nonzero words %llu, sum |f| %.6g, hash %016llx", (unsigned long long)present,
             (unsigned long long)addr[k], (unsigned long long)len[k], (unsigned long long)nz, s, (unsigned long long)h);
        const char *dir = getenv("TOTK_LUMLOG_DIR");
        if (dir && *dir) {
            _mkdir(dir);
            char path[512];
            snprintf(path, sizeof path, "%s/mem_%llx_%llu.bin", dir, (unsigned long long)addr[k], (unsigned long long)present);
            FILE *f = fopen(path, "wb");
            if (f) { fwrite(m, 1, (size_t)len[k], f); fclose(f); }
        }
    }
}

void vkb_tex_dump(const char *dir);
void vkb_tex_lumlog(const tex_t *t, uint64_t present) {
    samemem_report(present);
    {
        static long long at = -1, count;
        if (at == -1) { const char *e = getenv("TOTK_LUMLOG"); at = e ? atoll(e) : 0; count = e && strchr(e, ':') ? atoll(strchr(e, ':') + 1) : 0; }
        if (at > 0 && present >= (uint64_t)at && present < (uint64_t)(at + count)) { texlog(present); }
    }
    static long long at = -1, count;
    if (at == -1) { const char *e = getenv("TOTK_LUMLOG"); at = e ? atoll(e) : 0; count = e && strchr(e, ':') ? atoll(strchr(e, ':') + 1) : 0; }
    if (at <= 0 || present < (uint64_t)at || present >= (uint64_t)(at + count)) return;
    vkb_image *im = vkb_image_of(t);
    if (!im) return;
    mtl_sub_begin();
    uint32_t bpp;
    uint8_t *px = dump_copy(vkb_cmd(), im, 0, &bpp);
    mtl_wait(mtl_sub_commit(), UINT64_MAX);
    if (!px || bpp != 4) return;
    double sum[3] = {0}, low = 0;   // low: the lower half (the ground on the island; clouds there when it is missing)
    for (uint32_t p = 0; p < im->width * im->height; p += 7)
        for (int c = 0; c < 3; c++) { sum[c] += px[(size_t)p * 4 + c]; if (p / im->width >= im->height / 2) low += px[(size_t)p * 4 + c]; }
    double n = (double)(im->width * im->height / 7) * 255.0;
    static uint64_t last_draws;
    uint64_t draws = vkb_draw_count();
    VLOG("[lum] present %llu: mean %.3f %.3f %.3f, lower half %.3f, draws %llu", (unsigned long long)present, sum[0] / n, sum[1] / n, sum[2] / n,
         low / (n * 1.5), (unsigned long long)(draws - last_draws));
    last_draws = draws;
    memlog(present);   // the GPU has finished this frame (waited for above)
    // and each of those frames as <dir>/f<present>_<serial>...bmp; with TOTK_LUMLOG_JUMP=<delta> only frames where the lower
    // half's mean jumps by more than delta, with the frame before and the two after (catching a short drop-out)
    const char *dir = getenv("TOTK_LUMLOG_DIR"), *jump = getenv("TOTK_LUMLOG_JUMP");
    static double prev_low = -1;
    static uint8_t *prev_px;
    static uint64_t prev_present;
    static int save_left;
    double lowm = low / (n * 1.5);
    if (dir && *dir) {
        _mkdir(dir);
        char prefix[32];
        size_t bytes = (size_t)im->width * im->height * bpp;
        int save = !jump;
        if (jump && prev_low >= 0 && (lowm - prev_low > atof(jump) || prev_low - lowm > atof(jump))) {
            save = 1; save_left = 2;
            static int jumps;
            if (prev_px && jumps++ < 8) { snprintf(prefix, sizeof prefix, "f%llu_before_", (unsigned long long)prev_present); dump_save(dir, prefix, im, prev_px, bpp); }
            VLOG("[lum] jump at present %llu: lower half %.3f -> %.3f", (unsigned long long)present, prev_low, lowm);
            // 1: after the first jump up (the drop-out ends); down[<n>]: the n-th drop (default the first)
            const char *soj = getenv("TOTK_SNAP_ON_JUMP");
            int down = soj && !strncmp(soj, "down", 4);
            static int drops;
            if (down && lowm < prev_low) drops++;
            if (soj && !g_snap_at_jump && (down ? lowm < prev_low && drops >= (atoi(soj + 4) > 0 ? atoi(soj + 4) : 1) : lowm > prev_low)) {
                g_snap_at_jump = (long long)vkb_frame() + 1;   // the next frame recorded (the drop-out lasts a few)
                VLOG("[lum] snapshot set for present %lld", g_snap_at_jump);
            }
            static int dumps;   // TOTK_LUMLOG_JUMP_DUMP=1: the first jumps also dump every render target of the frame
            if (getenv("TOTK_LUMLOG_JUMP_DUMP") && dumps++ < 6) {
                char d2[512];
                snprintf(d2, sizeof d2, "%s/jump%llu", dir, (unsigned long long)present);
                _mkdir(d2);
                vkb_tex_dump(d2);
            }
        } else if (save_left > 0) { save = 1; save_left--; }
        static long every = -1;   // TOTK_LUMLOG_EVERY=<n>: also every n-th frame (where the scripted run went)
        if (every < 0) every = getenv("TOTK_LUMLOG_EVERY") ? atol(getenv("TOTK_LUMLOG_EVERY")) : 0;
        if (every > 0 && present % (uint64_t)every == 0) save = 1;
        if (save) { snprintf(prefix, sizeof prefix, "f%llu_", (unsigned long long)present); dump_save(dir, prefix, im, px, bpp); }
        if (jump) {   // the frame, kept for "before" (the ring memory is reused)
            if (!prev_px) prev_px = malloc(bytes);
            if (prev_px) memcpy(prev_px, px, bytes);
            prev_present = present;
        }
    }
    prev_low = lowm;
}

void vkb_tex_dump(const char *dir) {
    typedef struct { vkb_image *im; uint8_t *px; uint32_t bpp; } job;
    job jobs[512];
    int n = 0;
    mtl_sub_begin();
    VkCommandBuffer cb = vkb_cmd();
    img_lock(IS_OTHER);
    for (entry *e = g_all; e && n < 512; e = e->all_next) {
        // TOTK_DUMP_ALL=1: textures uploaded from game memory too (tables, LUTs)
        if (!e->refs || (!e->im.gpu_written && !(e->im.upload_stamp && getenv("TOTK_DUMP_ALL")))) continue;
        uint32_t bpp;
        uint8_t *px = dump_copy(cb, &e->im, 0, &bpp);
        if (px) jobs[n++] = (job){&e->im, px, bpp};
    }
    img_unlock();
    mtl_wait(mtl_sub_commit(), UINT64_MAX);
    _mkdir(dir);   // one level: the parent must exist
    for (int k = 0; k < n; k++) dump_save(dir, "", jobs[k].im, jobs[k].px, jobs[k].bpp);
    VLOG("dumped %d render targets to %s", n, dir);
    {   // GPU-written textures that share memory (aliasing): each is a separate image here
        int said = 0;
        img_lock(IS_OTHER);
        for (entry *a = g_all; a && said < 40; a = a->all_next) {
            if (!a->refs || !a->im.gpu_written || !a->im.storage) continue;
            nvn_texlayout La;
            nvn_tex_layout(&a->im.d, &La);
            for (entry *b = a->all_next; b && said < 40; b = b->all_next) {
                if (!b->refs || !b->im.gpu_written || !b->im.storage) continue;
                nvn_texlayout Lb;
                nvn_tex_layout(&b->im.d, &Lb);
                if (a->im.storage < b->im.storage + Lb.size && b->im.storage < a->im.storage + La.size) {
                    said++;
                    VLOG("[alias] GPU-written textures %llu (format %#x %ux%u at %p) and %llu (format %#x %ux%u at %p) share memory",
                         (unsigned long long)a->im.serial, a->im.d.format, a->im.width, a->im.height, (const void *)a->im.storage,
                         (unsigned long long)b->im.serial, b->im.d.format, b->im.width, b->im.height, (const void *)b->im.storage);
                }
            }
        }
        img_unlock();
    }
    // TOTK_DUMP_MEM=<guest address>:<bytes>[,...]: raw guest memory (GPU-written buffers) as <dir>/mem_<address>.bin
    for (const char *p = getenv("TOTK_DUMP_MEM"); p && *p;) {
        char *end;
        unsigned long long addr = strtoull(p, &end, 0), size = *end == ':' ? strtoull(end + 1, &end, 0) : 0;
        if (addr && size && size < (64u << 20)) {
            char path[1024];
            snprintf(path, sizeof path, "%s/mem_%llx.bin", dir, addr);
            FILE *f = fopen(path, "wb");
            if (f) { fwrite((const void *)(uintptr_t)addr, 1, size, f); fclose(f); }
        }
        p = strchr(end, ',');
        if (p) p++;
    }
}

// ---------------- [UIT 0054] GPU-drawn images copied back to game memory ("reactive flushing") ----------------
// On the console the GPU draws into the game's memory, so the CPU can read what was drawn. Here an image is drawn in
// device memory and its game memory keeps what was uploaded: the CPU reads old bytes, and textures over that memory
// (other formats / sizes of the same storage) upload them. yuzu copies such images back when the game asks to read
// GPU-written memory ("reactive flushing"; off there, TotK's grass vanishes the same way). TOTK_VK_IMAGE_WRITEBACK:
//  0 / off: none ([UIT 0057] only when asked: the grass needs it);
//  1 (explicit reads only, [UIT 0057] the default, also when unset): the images drawn since their last copy back over a range the game invalidates
//    (mtl_pool_invalidate, nvnMemoryPoolInvalidateMappedRange) are copied back before the call returns (a stall);
//  2 (all CPU-read targets): also, at the end of each submission, every such image of at most IWB_SMALL_MAX bytes
//    drawn in it, landed by the retire thread when the GPU is done (no stall; the frame after for the CPU).
// Only images that the CPU can read (vkb_imgwb.h: CPU-mapped pool or explicit layout, no frame / depth / compressed /
// multisample flags, formats whose bytes match): the frame's large targets never are, so nothing is read back per frame.
// Fences: the copy is in the submission's command buffer after the drawing (vkb_image_use waits for all earlier work on
// the image); the host memory is the slot's, read before the slot is reused (backend_vk.c). An image dropped meanwhile
// is out of the lists (entry_drop, images lock) and its Vulkan objects go when the GPU is done ([UIT 0045]).
// Game memory: pages the CPU wrote since the drawing was committed stay the CPU's (nvn_dirty stamps, or a snapshot of
// the storage); the pages written are GPU writes (nvn_dirty_gpu_write + nvn_gpu_write_mark: textures over them upload).
int vkb_pool_copy(const void *src, uint64_t n, void *dst);   // backend_vk.c: copies a live pool's bytes (dst NULL: is live)
static int g_iwb_mode = -1;
int vkb_iwb_mode(void) {
    int m = __atomic_load_n(&g_iwb_mode, __ATOMIC_ACQUIRE);
    if (m < 0) {
        m = iwb_mode_parse(getenv("TOTK_VK_IMAGE_WRITEBACK"));
        __atomic_store_n(&g_iwb_mode, m, __ATOMIC_RELEASE);
        const char *e = getenv("TOTK_VK_IMAGE_WRITEBACK");
        VLOG("[iwb] copy GPU-drawn images back to game memory: %s%s", m == IWB_ALL ? "all CPU-read targets" : m ? "explicit reads only" : "off",
             e && *e && *e != '-' ? "" : " (default)");
        if (m) readahead_on();   // [UIT 0255] its setting in the log at the start
    }
    return m;
}
static pthread_mutex_t g_iwb_mu = PTHREAD_MUTEX_INITIALIZER;   // after g_mu
static vkb_image **g_iwb_list;   // drawn in the submission being recorded (g_iwb_mu)
static int g_iwb_n, g_iwb_cap;
static _Atomic int g_iwb_npending;   // images with wb_pending (any thread's quick test)
static uint64_t g_iwb_superseded, g_iwb_too_big, g_iwb_short, g_iwb_gone, g_iwb_eligible;   // (g_mu)
// [UIT native-1031] g_iwb_live (declared before entry_drop): the eligible images that exist now

static int iwb_fit(const tex_t *t, const vkb_image *im);
static int iwb_eligible(const tex_t *t, const vkb_image *im) {   // g_mu held, at creation
    if (!vkb_iwb_mode() || !iwb_fit(t, im)) return 0;
    g_iwb_eligible++; g_iwb_live++;
    return 1;
}
static int iwb_fit(const tex_t *t, const vkb_image *im) {   // [UIT 0056] whatever the setting (the read log says so)
    if (!t->mem || !im->img || im->aspect != VK_IMAGE_ASPECT_COLOR_BIT || im->astc || im->compressed) return 0;
    const nvn_texdesc *d = &t->d;
    if (!iwb_flags_ok(d->flags) || !iwb_format_ok(d->format) || !iwb_target_ok(d->target) || d->samples > 1) return 0;
    if (!t->has_layout && !(t->pool && !t->pool->host && iwb_pool_ok(t->pool->flags))) return 0;
    VkImageAspectFlags a; int astc;
    if (im->format != vkb_format(d->format, &a, &astc) || im->width != (d->width ? d->width : 1)) return 0;
    if (im->depth != 1 || im->height != (d->target == 0 || d->target == 3 ? 1u : (d->height ? d->height : 1))) return 0;
    if (totk_vpool_gather((uintptr_t)t->mem, 0, NULL)) return 0;   // a virtual pool: its storage is not one range
    iwb_plan p;
    if (iwb_make_plan(d, im->levels, im->layers, &p)) return 0;
    return 1;
}
static void iwb_note(vkb_image *im) {   // any recording thread: listed once per submission
    if (vkb_iwb_mode() == IWB_OFF) return;
    uint64_t sub = mtl_last_fence() + 1, was = __atomic_load_n(&im->wb_sub, __ATOMIC_ACQUIRE);
    if (was == sub || !__atomic_compare_exchange_n(&im->wb_sub, &was, sub, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) return;
    pthread_mutex_lock(&g_iwb_mu);
    if (g_iwb_n == g_iwb_cap) {
        int c = g_iwb_cap ? g_iwb_cap * 2 : 64;
        vkb_image **l = realloc(g_iwb_list, sizeof *l * (size_t)c);
        if (!l) { pthread_mutex_unlock(&g_iwb_mu); return; }
        g_iwb_list = l; g_iwb_cap = c;
    }
    g_iwb_list[g_iwb_n++] = im;
    pthread_mutex_unlock(&g_iwb_mu);
}
static void iwb_unpend(vkb_image *im) {   // g_mu held
    if (im->wb_pending) { im->wb_pending = 0; atomic_fetch_sub(&g_iwb_npending, 1); bidx_mod(im, -1); bact_fix(im); }
    ix_pend_del(im);   // [UIT 0230]
    free(im->wb_snap); im->wb_snap = NULL;
}
static void iwb_forget(vkb_image *im) {   // g_mu held: the entry goes
    if (!im->wb_ok) return;
    if (im->wb_hot) { im->wb_hot = 0; g_iwb_hot_n--; }   // [UIT 0255]
    pthread_mutex_lock(&g_iwb_mu);
    for (int i = 0; i < g_iwb_n; i++) if (g_iwb_list[i] == im) g_iwb_list[i--] = g_iwb_list[--g_iwb_n];
    pthread_mutex_unlock(&g_iwb_mu);
    iwb_unpend(im);
}
static uint64_t iwb_size(const vkb_image *im) { nvn_texlayout L; nvn_tex_layout(&im->d, &L); return L.size; }
// g_mu held: the drawing is done as of this submission: the stamp, and the snapshot of the storage. 0: its pool is gone.
static int iwb_stamp(vkb_image *im, uint64_t size) {
    im->wb_stamp = nvn_dirty_now();
    if (size <= IWB_SNAP_MAX) {
        uint8_t *s = im->wb_snap ? im->wb_snap : malloc(size);   // (an image's size does not change)
        im->wb_snap = s;
        if (s && !vkb_pool_copy(im->storage, size, s)) { iwb_unpend(im); g_iwb_gone++; return 0; }
    } else {
        free(im->wb_snap); im->wb_snap = NULL;
        if (!vkb_pool_copy(im->storage, size, NULL)) { iwb_unpend(im); g_iwb_gone++; return 0; }
    }
    if (!im->wb_pending) { im->wb_pending = 1; atomic_fetch_add(&g_iwb_npending, 1); bidx_mod(im, 1); ix_pend_add(im); bact_fix(im); }
    return 1;
}
static int iwb_over(const vkb_image *im, uint64_t lo, uint64_t hi) {
    return iwb_overlap((uint64_t)(uintptr_t)im->storage, iwb_size(im), lo, hi - lo);
}
int vkb_iwb_wanted(uint64_t lo, uint64_t hi) {
    if (!atomic_load(&g_iwb_npending)) return 0;
    int hit = 0;
    img_lock(IS_WANTED);
    eiter it;   // [UIT 0230] the pending ones ([UIT 0254] over [lo, hi))
    uint64_t qlo, qhi; pend_q(lo, hi, &qlo, &qhi);
    for (entry *e = it_first(&it, SC_WANTED, &g_ix_pend, qlo, qhi); e && !hit; e = it_next(&it)) hit = e->im.wb_pending && iwb_over(&e->im, lo, hi);
    it_end(&it);
    img_unlock();
    return hit;
}
// [UIT 0155] a buffer read (vertex, index, uniform, storage, copy source) of [addr, +len): GPU drawings over that
// memory not in game memory yet are marked like TOTK_VK_ALIAS_COPYBACK's writers (copied back after each drawing; the
// pool's device copy gets them too). Recording threads, no lock held. Logged (first 32) whatever the setting.
static __thread uint64_t t_bufq_addr, t_bufq_len, t_bufq_gen;
// [UIT 0234] TOTK_VK_BUFREAD_MEMO (default on; =0 as before): ranges whose walk changed nothing, per thread (vkb_bufmemo.h)
#include "vkb_bufmemo.h"
static __thread bqm t_bqm;
static int bufmemo_on(void) {
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("TOTK_VK_BUFREAD_MEMO");
        on = !(e && e[0] == '0' && !e[1]);
        VLOG("[UIT 0234] buffer reads over GPU drawings: %s (TOTK_VK_BUFREAD_MEMO%s)",
             on ? "ranges already found to change nothing are not looked at again until a drawing changes" : "every one looked at, as before",
             on ? ", default; =0 as before" : "=0");
    }
    return on;
}
void vkb_iwb_buf_read(uint64_t addr, uint64_t len) {
    if (!len || !atomic_load_explicit(&g_iwb_npending, memory_order_relaxed) || vkb_iwb_mode() == IWB_OFF) return;
    uint64_t c0 = addr >> 20, c1 = (addr + len - 1) >> 20;
    if (c1 - c0 > 1024) c1 = c0 + 1024;   // (a huge range: its first GiB)
    int any = 0;
    for (uint64_t c = c0; c <= c1 && !any; c++) any = atomic_load_explicit(&g_bidx[bidx_h(c)], memory_order_relaxed) != 0;
    if (!any) return;
    // [UIT 0243] no pending drawing there that a walk could mark or clear (and the log lines are used up).
    // [UIT 0254] with TOTK_VK_BUFREAD_RANGE on, without waiting for the 32 "[alias] buffer" log lines: in every
    // 2026-11-01 log not one such line was written (no read overlapped a pending drawing), so the count never reached 32
    // and 0243 never skipped a read ("nothing to change 0" in every window). The log lines then come only from reads
    // that could change something (a drawing not yet marked, or let go): a read skipped here changes nothing either way.
    if (bufact_on() && (bufrange_on() || atomic_load_explicit(&g_bq_said, memory_order_relaxed) >= 32) && !bact_any(&g_bact, addr, len)) { vkb_cnt_add(g_bq_noact, 1); return; }
    uint64_t gen = atomic_load(&g_bidx_gen);
    if (t_bufq_addr == addr && t_bufq_len == len && t_bufq_gen == gen) return;   // the same answer as last time
    t_bufq_addr = addr; t_bufq_len = len; t_bufq_gen = gen;
    int memo = bufmemo_on();
    if (memo && bqm_hit(&t_bqm, addr, len, gen)) { vkb_cnt_add(g_bq_skip, 1); return; }   // [UIT 0234] known to change nothing
    int on = coherent_on(), eff = 0;
    img_lock(IS_BUFREAD);
    eiter it;   // [UIT 0230] the pending ones ([UIT 0254] over the read)
    uint64_t qlo, qhi; pend_q(addr, addr + len < addr ? UINT64_MAX : addr + len, &qlo, &qhi);
    for (entry *e = it_first(&it, SC_BUFREAD, &g_ix_pend, qlo, qhi); e; e = it_next(&it)) {
        vkb_image *im = &e->im;
        if (!im->wb_pending || !iwb_over(im, addr, addr + len)) continue;
        int marked = 0;
        if (on && !im->wb_alias && iwb_size(im) <= IWB_EXPLICIT_MAX) {
            im->wb_alias = marked = eff = 1;
            bact_fix(im);   // [UIT 0243]
            g_iwb_buf_marked++;
            atomic_store(&g_iwb_alias_want, 1);
        } else if (on && im->wb_rel) im->wb_rel = 0, eff = 1, bact_fix(im);   // read as a buffer: copied after each drawing from now on, even if taken back
        if (atomic_load_explicit(&g_bq_said, memory_order_relaxed) >= 32) continue;
        atomic_fetch_add_explicit(&g_bq_said, 1, memory_order_relaxed); eff = 1;
        VLOG("[alias] buffer %#llx+%llu is read from memory that GPU-drawn texture %llu (format %#x %ux%u, %u levels, flags %#x, %llu bytes at %p) "
             "uses; its last drawing (#%llu) is not in game memory; writer: %s%s", (unsigned long long)addr, (unsigned long long)len,
             (unsigned long long)im->serial, im->d.format, im->width, im->height, im->levels, im->d.flags, (unsigned long long)iwb_size(im),
             (const void *)im->storage, (unsigned long long)im->write_seq, e->refs > 0 ? "live" : "released",
             marked ? "; now copied back after each drawing (TOTK_VK_GPU_MEM_COHERENT)" : !on ? " (TOTK_VK_GPU_MEM_COHERENT=0: left as is)" : "");
    }
    it_end(&it);
    img_unlock();
    if (memo && !eff) bqm_put(&t_bqm, addr, len, gen);   // [UIT 0234] (gen as read before the walk)
    vkb_cnt_add(g_bq_walk, 1);
}
void vkb_iwb_free(vkb_iwb_rec *r, int n) {
    for (int i = 0; i < n; i++) free(r[i].snap);
    free(r);
}
vkb_iwb_rec *vkb_iwb_record(VkCommandBuffer cb, int slot, int why, uint64_t lo, uint64_t hi, int *n) {
    *n = 0;
    int mode = vkb_iwb_mode();
    if (!mode) return NULL;
    if (why == 0) {   // nothing drawn to copy back: the usual case, without the images lock
        pthread_mutex_lock(&g_iwb_mu);
        int none = !g_iwb_n;
        pthread_mutex_unlock(&g_iwb_mu);
        if (none && !atomic_load(&g_iwb_alias_want)) return NULL;
    }
    img_lock(IS_RECORD);
    vkb_image **pick = NULL;
    int np = 0;
    if (why == 0) {   // the images drawn in this submission: stamped; with all CPU-read targets the small ones are copied
        pthread_mutex_lock(&g_iwb_mu);
        vkb_image **l = g_iwb_list;
        int ln = g_iwb_n;
        g_iwb_list = NULL; g_iwb_n = g_iwb_cap = 0;
        pthread_mutex_unlock(&g_iwb_mu);
        // [UIT 0137] and those other textures read the memory of (wb_alias): copied whatever the mode; with the images
        // newly marked while already pending (drawn in an earlier submission, not copied since)
        int want = atomic_exchange(&g_iwb_alias_want, 0), extra = 0;
        eiter it;   // [UIT 0230] the pending ones (each loop below: the index's list as of its start, newest first as g_all)
        if (want) {
            for (entry *e = it_first(&it, SC_RECORD, &g_ix_pend, 0, 0); e; e = it_next(&it)) extra += e->im.wb_pending && e->im.wb_alias && (e->refs > 0 || released_ok());
            it_end(&it);
        }
        pick = malloc(sizeof *pick * (size_t)(ln + extra ? ln + extra : 1));
        for (int i = 0; i < ln && pick; i++) {
            uint64_t size = iwb_size(l[i]);
            if (iwb_stamp(l[i], size) && iwb_pick_end(mode, l[i]->wb_alias, l[i]->wb_hot, size)) {   // wb_hot: [UIT 0255] (set only with it on)
                pick[np++] = l[i];
                if (l[i]->wb_alias) g_iwb_alias_copies++;
                else if (l[i]->wb_hot && mode != IWB_ALL) g_iwb_hot_copies++;
            }
        }
        if (!pick) for (int i = 0; i < ln; i++) iwb_unpend(l[i]);
        if (pick && want) {
            for (entry *e = it_first(&it, SC_RECORD, &g_ix_pend, 0, 0); e && np < ln + extra; e = it_next(&it)) {
                vkb_image *o = &e->im;
                if (!o->wb_pending || !o->wb_alias || (e->refs <= 0 && !released_ok()) || iwb_size(o) > IWB_EXPLICIT_MAX) continue;
                int dup = 0;
                for (int k = 0; k < np && !dup; k++) dup = pick[k] == o;
                if (!dup) { pick[np++] = o; g_iwb_alias_copies++; }
            }
            it_end(&it);
        }
        free(l);
        for (int i = 1; i < np; i++)   // the last drawn lands last where images share memory
            for (int k = i; k > 0 && pick[k - 1]->write_seq > pick[k]->write_seq; k--) { vkb_image *x = pick[k]; pick[k] = pick[k - 1]; pick[k - 1] = x; }
        if (np && atomic_load(&g_iwb_npending)) {   // older drawings pending over the same memory must not land after these
            for (entry *e = it_first(&it, SC_RECORD, &g_ix_pend, 0, 0); e; e = it_next(&it)) {
                vkb_image *o = &e->im;
                if (!o->wb_pending) continue;
                for (int i = 0; i < np; i++)
                    if (o != pick[i] && o->write_seq < pick[i]->write_seq && iwb_overlap((uint64_t)(uintptr_t)o->storage, iwb_size(o),
                                                                                        (uint64_t)(uintptr_t)pick[i]->storage, iwb_size(pick[i]))) {
                        iwb_unpend(o); g_iwb_superseded++; break;
                    }
            }
            it_end(&it);
        }
    } else {   // the pending images over [lo, hi); of images over the same memory, the last drawn
        int cap = 0;
        eiter it;   // [UIT 0230] the pending ones (as of the start: those unpended meanwhile fail the test below, as before)
        uint64_t qlo, qhi; pend_q(lo, hi, &qlo, &qhi);   // [UIT 0254] over [lo, hi)
        for (entry *e = it_first(&it, SC_RECORD, &g_ix_pend, qlo, qhi); e; e = it_next(&it)) {
            vkb_image *im = &e->im;
            if (!im->wb_pending || !iwb_over(im, lo, hi)) continue;
            uint64_t a = (uint64_t)(uintptr_t)im->storage, sz = iwb_size(im);
            int keep = 1;
            for (int k = 0; k < np; k++) {
                vkb_image *o = pick[k];
                if (!iwb_overlap((uint64_t)(uintptr_t)o->storage, iwb_size(o), a, sz)) continue;
                if (o->write_seq > im->write_seq) { keep = 0; break; }
                iwb_unpend(o); g_iwb_superseded++; pick[k--] = pick[--np];
            }
            if (!keep) { iwb_unpend(im); g_iwb_superseded++; continue; }
            if (sz > IWB_EXPLICIT_MAX) {
                if (g_iwb_too_big++ < 8) VLOG("[iwb] image %llu %ux%u format %#x: %llu KiB, too large to copy back", (unsigned long long)im->serial,
                                              im->width, im->height, im->d.format, (unsigned long long)(sz >> 10));
                iwb_unpend(im);
                continue;
            }
            if (np == cap) { cap = cap ? cap * 2 : 16; vkb_image **q = realloc(pick, sizeof *q * (size_t)cap); if (!q) break; pick = q; }
            pick[np++] = im;
        }
        it_end(&it);
    }
    vkb_iwb_rec *r = np ? calloc((size_t)np, sizeof *r) : NULL;
    iwb_plan *pl = np ? malloc(sizeof *pl * (size_t)np) : NULL;
    VkDeviceSize need = 0;
    for (int i = 0; r && pl && i < np; i++) {
        iwb_make_plan(&pick[i]->d, pick[i]->levels, pick[i]->layers, &pl[i]);   // checked at creation
        need = (need + 255) & ~(VkDeviceSize)255;
        r[i].host_off = need;
        need += pl[i].total;
    }
    VkBuffer buf = VK_NULL_HANDLE;
    VkDeviceSize base = 0;
    uint8_t *host = r && pl ? vkb_iwb_stage(slot, need, &buf, &base) : NULL;
    if (np && !host) { g_iwb_short += (uint64_t)np; np = 0; }   // left pending: copied back at the next explicit read
    for (int i = 0; i < np; i++) r[i].host_off += base;
    vkb_trk *trk = vkb_trk_get();
    vkb_trk_set(NULL);   // after the chunks are glued: the images' own layouts
    for (int i = 0; i < np; i++) {
        vkb_image *im = pick[i];
        VkBufferImageCopy c[IWB_MAX_LEVELS];
        for (int l = 0; l < pl[i].levels; l++)
            c[l] = (VkBufferImageCopy){.bufferOffset = r[i].host_off + pl[i].lv[l].off, .bufferRowLength = pl[i].lv[l].w,
                .bufferImageHeight = pl[i].lv[l].h, .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, (uint32_t)l, 0, pl[i].layers},
                .imageExtent = {pl[i].lv[l].w, pl[i].lv[l].h, 1}};
        vkb_image_use(cb, im, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
        vk.vkCmdCopyImageToBuffer(cb, im->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf, (uint32_t)pl[i].levels, c);
        r[i].d = im->d; r[i].storage = (uint8_t *)im->storage; r[i].size = iwb_size(im); r[i].stamp = im->wb_stamp;
        r[i].snap = im->wb_snap; im->wb_snap = NULL;   // the record owns it now
        r[i].serial = im->serial; r[i].levels = (uint32_t)pl[i].levels; r[i].layers = pl[i].layers; r[i].why = why;
        if (why && iwb_mark_hot(readahead_on(), im->wb_hot, r[i].size, g_iwb_hot_n, READAHEAD_MAX)) {   // [UIT 0255]
            im->wb_hot = 1; g_iwb_hot_n++;
            if (g_iwb_hot_n <= 8) VLOG("[iwb] image %llu %ux%u format %#x read back by the CPU: copied back with each submission that draws it from now on "
                                       "(TOTK_VK_READBACK_AHEAD; %llu such images)", (unsigned long long)im->serial, im->width, im->height, im->d.format,
                                       (unsigned long long)g_iwb_hot_n);
        }
        iwb_unpend(im);
    }
    if (np) {   // the host reads it after the fence
        VkMemoryBarrier2 mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, .srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT,
            .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT, .dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT, .dstAccessMask = VK_ACCESS_2_HOST_READ_BIT};
        VkDependencyInfo di = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &mb};
        vk.vkCmdPipelineBarrier2(cb, &di);
    }
    vkb_trk_set(trk);
    img_unlock();
    free(pick); free(pl);
    if (!np) { free(r); r = NULL; }
    *n = np;
    return r;
}
void vkb_iwb_stats(uint64_t *eligible, uint64_t *live, uint64_t *pending, uint64_t *superseded, uint64_t *too_big, uint64_t *short_mem, uint64_t *gone) {
    *gone = g_iwb_gone; *eligible = g_iwb_eligible; *live = g_iwb_live; *pending = (uint64_t)atomic_load(&g_iwb_npending); *superseded = g_iwb_superseded;
    *too_big = g_iwb_too_big; *short_mem = g_iwb_short;
}
// [UIT 0056] Off or on: game reads (invalidates) over memory of GPU-drawn images, for the whole run. Each image's first
// read is logged (up to IWB_SEEN_MAX images), with the frame; every window, vkb_iwb_read_log says how many such reads a
// frame and which images, and what changed. (0054 logged the first 64 lines of the first 4000 reads: all on the title
// screen.) A read range's images come from a scan of every image, cached per range (iwb_rc_get).
#define IWB_SEEN_MAX 256
static pthread_mutex_t g_rd_mu = PTHREAD_MUTEX_INITIALIZER;   // the cache, the window and the seen list (after g_mu: never both)
static iwb_rc_ent g_rd_cache[IWB_RC_CAP];
static iwb_wset g_rd_win, g_rd_prev;
static uint64_t g_rd_calls, g_rd_bytes, g_rd_scans, g_rd_seen[IWB_SEEN_MAX];
static int g_rd_nseen;
int vkb_iwb_note_inval(uint64_t lo, uint64_t hi) {
    uint64_t gen = atomic_load(&g_iwb_img_gen), fr = vkb_frame();
    int fresh, n = 0;
    uint64_t ser[IWB_RC_IMGS];
    pthread_mutex_lock(&g_rd_mu);
    iwb_rc_ent *c = iwb_rc_get(g_rd_cache, lo, hi, gen, fr, 60, &fresh);
    if (!fresh) { n = c->n; memcpy(ser, c->serial, sizeof ser); }
    pthread_mutex_unlock(&g_rd_mu);
    struct { uint64_t serial; nvn_texdesc d; uint32_t w, h, levels, layers; int fit, ok, pending; uint64_t size; const void *st; } nw[IWB_RC_IMGS];
    int nnew = 0;
    if (fresh) {   // scan every image (g_mu), then fill the entry (g_rd_mu)
        img_lock(IS_INVAL);
        eiter it;   // [UIT 0230] only the images over the range
        for (entry *e = it_first(&it, SC_INVAL, &g_ix_all, lo, hi); e; e = it_next(&it)) {
            vkb_image *im = &e->im;
            if (!(im->gpu_written || im->wb_pending) || !im->storage || !iwb_over(im, lo, hi)) continue;
            if (n < IWB_RC_IMGS) {
                ser[n] = im->serial;
                nw[n] = (__typeof__(nw[0])){im->serial, im->d, im->width, im->height, im->levels, im->layers, im->wb_fit, im->wb_ok,
                                           im->wb_pending, iwb_size(im), im->storage};
            }
            n++;
        }
        it_end(&it);
        img_unlock();
        nnew = n < IWB_RC_IMGS ? n : IWB_RC_IMGS;
    }
    if (!n && !fresh) return 0;
    pthread_mutex_lock(&g_rd_mu);
    if (fresh) {
        c = iwb_rc_get(g_rd_cache, lo, hi, gen, fr, 60, &fresh);   // (the table may have moved on meanwhile)
        iwb_rc_fill(c, gen, fr, n, ser);
        g_rd_scans++;
    }
    int k = n < IWB_RC_IMGS ? n : IWB_RC_IMGS;
    if (n) { g_rd_calls++; g_rd_bytes += hi - lo; }
    for (int i = 0; i < k; i++) iwb_wset_add(&g_rd_win, ser[i]);
    for (int i = 0; i < nnew; i++) {   // each image's first read
        int seen = 0;
        for (int j = 0; j < g_rd_nseen && !seen; j++) seen = g_rd_seen[j] == nw[i].serial;
        if (seen || g_rd_nseen >= IWB_SEEN_MAX) continue;
        g_rd_seen[g_rd_nseen++] = nw[i].serial;
        VLOG("[iwb] game read %#llx+%llu bytes over GPU-drawn image %llu %ux%u x%u levels x%u layers, format %#x target %d flags %#x, "
             "%llu bytes at %p: copy back %s%s; frame %llu (first read of this image%s)", (unsigned long long)lo, (unsigned long long)(hi - lo),
             (unsigned long long)nw[i].serial, nw[i].w, nw[i].h, nw[i].levels, nw[i].layers, nw[i].d.format, nw[i].d.target, nw[i].d.flags,
             (unsigned long long)nw[i].size, nw[i].st, nw[i].ok ? "possible" : nw[i].fit ? "possible (setting off)" : "not possible",
             nw[i].pending ? ", pending" : "", (unsigned long long)fr, g_rd_nseen == IWB_SEEN_MAX ? "; no more of these lines" : "");
    }
    pthread_mutex_unlock(&g_rd_mu);
    return n;
}
void vkb_iwb_read_log(uint64_t every) {
    static uint64_t pc, pb, ps;
    char list[IWB_WSET * 24 + 64];
    pthread_mutex_lock(&g_rd_mu);
    uint64_t c = g_rd_calls, b = g_rd_bytes, sc = g_rd_scans;
    iwb_wset w = g_rd_win;
    int same = iwb_wset_same(&w, &g_rd_prev);
    g_rd_prev = w;
    memset(&g_rd_win, 0, sizeof g_rd_win);
    pthread_mutex_unlock(&g_rd_mu);
    size_t at = 0;
    list[0] = 0;
    for (int i = 0; i < w.n && at < sizeof list - 24; i++)
        at += (size_t)snprintf(list + at, sizeof list - at, "%s%llu", i ? ", " : "", (unsigned long long)w.s[i]);
    if (w.over && at < sizeof list - 8) snprintf(list + at, sizeof list - at, ", ...");
    double f = every ? (double)every : 1.0;
    VLOG("[iwb]   game reads over GPU-drawn images: %.1f calls, %.2f KiB a frame; images %s (%s); range scans %llu",
         (double)(c - pc) / f, (double)(b - pb) / f / 1024.0, w.n ? list : "none", same ? "as the last window" : "CHANGED since the last window",
         (unsigned long long)(sc - ps));
    pc = c; pb = b; ps = sc;
}
