// Internal interface of the Vulkan NVN replayer (backend_vk.c, vkb_tex.c).
#pragma once
#include <stdint.h>

#include "nvn/nvn_mtl.h"
#include "nvn/nvn_obj.h"
#include "../vk/vk.h"

#define VKB_SLOTS 8                // command buffers in flight (backend_vk.c)

// ---- backend_vk.c
const p_set_rt *vkb_rt(void);    // current render targets
uint64_t vkb_frame(void);         // presents so far
int vkb_rt_count(void);           // colour targets of the current pass
VkExtent2D vkb_rt_extent(void);   // the current pass's render area
int vkb_slot(void);               // the command slot of the submission being replayed (per-slot rings / pools)
void vkb_gprof_report(void);          // TOTK_GPU_PROF: GPU time per shader of one frame (development)
// Begins the render pass on the current targets if needed (0: not drawable); with no targets at all: a pass without
// attachments over `bare`. fb: the targets the draws also sample (bit i: colour i, VKB_FB_DEPTH: depth), which get the
// attachment feedback loop layout.
enum { VKB_FB_DEPTH = 1u << 8 };
int vkb_pass_begin(VkExtent2D bare, uint32_t fb);
uint32_t vkb_pass_fb(void);   // the current pass's feedback targets
uint64_t vkb_pass_serial(void);   // changes with every pass begun
void vkb_pass_end(void);
enum { PE_OTHER, PE_SET_RT, PE_BARRIER, PE_COPY, PE_UPDATE, PE_CLEAR_TEX, PE_SUBMIT, PE_STORES, PE_SNAP, PE_FEEDBACK, PE_IMAGES,
       PE_MIRROR, PE_DISPATCH, PE_TEXCOPY, PE_SHADOW, PE_COUNT };   // why a pass ended (counted, the per-frame log line)
void vkb_pass_end_r(int why);
int vkb_stores_take(void);
int vkb_scissor(VkRect2D *r);
int vkb_viewport0(float v[4]);   // the scissor set by the game (0: none)
uint64_t vkb_draw_count(void);   // draws and dispatches recorded so far   // vkb_draw.c: storage writes since the last pass began (and forgets them)
// Destroys a view / an image and its memory once the GPU has finished every submission that may use them: the one being
// recorded now included (its earlier commands may already refer to them).
void vkb_destroy_later(VkImageView view, VkImage img, VkDeviceMemory mem);
// [UIT 0075] an image and its memory: `alloc` a place in a shared block (vkb_mem.h; mem is then the block's and stays),
// else NULL and `mem` is the image's own
void vkb_destroy_image_later(VkImage img, VkDeviceMemory mem, void *alloc);
void vkb_destroy_sampler_later(VkSampler s);   // [UIT 0045] after the submission being recorded
void vkb_passprof_note(const char *key);   // TOTK_GPU_PASSPROF: names the open pass by its first shader
void vkb_checkpoint(const char *what, const char *k1, const char *k2);   // TOTK_VK_CHECKPOINTS: names the next GPU command
void vkb_checkpoint_info(const char *fmt, ...) __attribute__((format(printf, 1, 2)));   // [UIT 0135] details of this thread's last one
int vkb_in_pass(void);
// Host-visible upload memory of the current submission (reused after its fence): n bytes aligned to `align`.
void *vkb_ring_alloc(VkDeviceSize n, VkDeviceSize align, VkBuffer *buf, VkDeviceSize *off);
extern __thread uint64_t vkb_ring_bytes_tl;   // [UIT native] bytes this thread has taken from it (a running total)
VkCommandBuffer vkb_cmd(void);    // the command buffer of the submission being replayed
void vkb_occl_begin(VkCommandBuffer cb);   // [UIT 0050] TOTK_VK_OCCLUSION=gpu: around a draw's vkCmdDraw*, its occlusion query
void vkb_occl_end(VkCommandBuffer cb);
int vkb_render_suppressed(void);   // rendering disabled or conditional (recorded state; audit)
int vkb_ready(void);              // 1 when replaying on a Vulkan device

// ---- vkb_tex.c: NVN textures as Vulkan images (one per texture serial: the game copies texture objects by value)
typedef struct vkb_image {
    uint64_t serial;
    nvn_texdesc d;
    VkImage img;
    VkDeviceMemory mem;            // its own, or the shared block it is in (alloc)
    void *alloc;                   // [UIT 0075] its place in a shared block (vkb_mem.h, TOTK_VK_SUBALLOC); NULL: mem is its own
    VkFormat format;
    VkImageAspectFlags aspect;
    VkImageLayout layout;          // the whole image's current layout (replay thread)
    uint32_t layers, levels, width, height, depth;
    int astc;                      // ASTC in the game: decoded to RGBA8 on upload, or BC7 from the cache (bc7)
    int bc7;                       // an ASTC texture held as BC7 (1) or BC1 (2) from the cache (vkb_bc7.c); 0: RGBA8
    int uploading;                 // an upload is being recorded (several threads: the others wait for it, vkb_tex_upload)
    VkImageUsageFlags usage;
    int compressed;                // block-compressed format: no attachment / storage use
    int gpu_written;               // rendered / cleared / copied to by the GPU: never overwritten from game memory ([temporary])
    uint64_t upload_stamp;         // nvn_dirty generation of the last upload from game memory (0 = never)
    uint64_t gpu_stamp;            // nvn_gpu_write_now() at that upload: GPU writes into its memory since make it stale
    uint64_t dirty_checked;        // the submission in which the game memory was last checked for CPU writes
    int stale;                     // that check found CPU writes: upload before the next use
    int cpu_written;               // WriteTexels changed its game memory: upload before the next use (any pool)
    int not_uploadable;            // contents in game memory that the upload cannot take (depth / 3-component formats)
    uint64_t vpool_gen;            // in a virtual pool: the mapping generation of the last upload (0: not in one)
    const uint8_t *storage;        // the texture's storage in game memory (block-linear / linear)
    uint64_t write_seq;            // vkb_image_written: when the GPU last wrote it (0 = never)
    struct vkb_image *refresh_from;   // newer contents at the same memory in another size: scaled in before use (vkb_tex.c)
    void *stream;                  // its larger mip levels are streaming in (vkb_tex.c, TOTK_TEX_ASYNC): the job, else NULL
    uint32_t resident_base;        // while streaming: the largest level uploaded; views start there (0: all levels)
    uint64_t rt_frame;             // [UIT 0039] TOTK_SHADER_ASYNC only: the last frame a draw rendered into it (__atomic)
    uint32_t rt_streak;            // the frames in a row (gaps of at most one) it was rendered into, up to that one
    // [UIT 0054] copying it back to game memory (TOTK_VK_IMAGE_WRITEBACK, vkb_tex.c): may be (1) / not (0); the
    // submission that listed it last (__atomic); drawn since its last copy back, with the dirty stamp and a snapshot of
    // its storage (bytes; NULL over IWB_SNAP_MAX) taken when that submission was committed (images lock)
    int wb_ok;
    int wb_fit;                    // [UIT 0056] could be copied back if the setting were on (for the read log)
    uint64_t wb_sub;
    int wb_pending;
    uint64_t wb_stamp;
    uint8_t *wb_snap;
    // [UIT 0137] a texture of another shape (format, flags, levels or size) reads this image's game memory: what the
    // GPU draws into it is copied back at the end of each submission that draws it, not only when the CPU reads it
    // (TOTK_VK_ALIAS_COPYBACK, vkb_tex.c tex_upload); images lock
    int wb_alias;
    int wb_rel;                    // [UIT 0155] wb_alias set because the game let it go (cleared when taken back)
    int wb_act;                    // [UIT 0243] counted in vkb_bufact.h's table (a buffer read could still mark or clear it)
    int wb_hot;                    // [UIT 0255] the CPU read it back (explicit read): copied ahead at the end of each submission that draws it (images lock)
} vkb_image;
void vkb_image_written(vkb_image *im);   // the GPU writes it (render target, clear, copy, storage)
// ---- [UIT 0054] GPU-drawn images copied back to game memory ("reactive flushing", TOTK_VK_IMAGE_WRITEBACK; the pure
// parts in vkb_imgwb.h). The copy is recorded by vkb_tex.c into the slot's host memory; backend_vk.c lands it.
typedef struct vkb_iwb_rec {
    nvn_texdesc d;
    uint8_t *storage;              // the texture's game memory
    uint64_t size;                 // its storage bytes
    uint64_t stamp;                // nvn_dirty_now() when the drawing was committed: pages the CPU wrote since stay the CPU's
    uint8_t *snap;                 // the storage's bytes then (NULL: over IWB_SNAP_MAX): unchanged = the CPU left it alone
    uint64_t host_off;             // where the copy starts in the slot's host memory (vkb_iwb_stage's buffer)
    uint64_t serial;
    uint32_t levels, layers;
    int why;                       // 0: the end of the submission (all CPU-read targets), 1: the game's explicit read
} vkb_iwb_rec;
void vkb_iwb_ahead_stats(uint64_t *n, uint64_t *copies);   // [UIT 0255] images read back ahead, copies so far
int vkb_iwb_mode(void);            // IWB_OFF / IWB_EXPLICIT / IWB_ALL (vkb_imgwb.h)
// Translation thread, a submission open: records copies of the images to copy back into cb and returns their records
// (malloc'd; *n of them). why 0: the images drawn in this submission (small ones copied, the others left pending);
// why 1: pending images over game memory [lo, hi). The host memory comes from vkb_iwb_stage(slot).
vkb_iwb_rec *vkb_iwb_record(VkCommandBuffer cb, int slot, int why, uint64_t lo, uint64_t hi, int *n);
int vkb_iwb_wanted(uint64_t lo, uint64_t hi);   // any thread: pending images over [lo, hi) (the explicit path's quick test)
void vkb_iwb_free(vkb_iwb_rec *r, int n);
// backend_vk.c: need bytes of the slot's cached host memory for this submission, at *off of *buf (NULL: none)
uint8_t *vkb_iwb_stage(int slot, VkDeviceSize need, VkBuffer *buf, VkDeviceSize *off);
int vkb_iwb_land(const vkb_iwb_rec *r, const uint8_t *host);   // backend_vk.c: after the GPU is done (host: the buffer's start)
int vkb_iwb_note_inval(uint64_t lo, uint64_t hi);   // game reads over GPU-drawn images: counted, each image's first logged (any mode); 0: none there (nothing to copy back)
void vkb_iwb_read_log(uint64_t every);   // [UIT 0056] with the [draws] lines: those reads a frame and which images (any mode)
void vkb_iwb_stats(uint64_t *eligible, uint64_t *live, uint64_t *pending, uint64_t *superseded, uint64_t *too_big, uint64_t *short_mem, uint64_t *gone);
int vkb_tex_refresh(VkCommandBuffer cb, vkb_image *im);   // outside a pass: the pending refresh_from copy; 1 if recorded
// Texture streaming (vkb_tex.c, 2026-10-29): an ASTC texture's small mips are uploaded at once, its larger ones are made
// on background threads and applied when ready (views start at the largest level present meanwhile).
int vkb_tex_upload_full(VkCommandBuffer cb, vkb_image *im);   // as vkb_tex_upload, every level (copies use the contents)
int vkb_tex_stream_due(const vkb_image *im);   // levels made and waiting to be applied (one thread: end the pass for it)
void vkb_tex_note_blank(const vkb_image *im, int not_uploaded);   // a game texture drawn with no contents (counted, logged)
void vkb_tex_counters(uint64_t v[8]);          // for the long submission line (backend_vk.c)
void vkb_tex_stream_log(double frames);        // the per-frame log lines
// [UIT 0141] pool lookups under one hold of the pool table's read lock (backend_vk.c; vkb_ucap.c's merge). hold: 0 when
// the backend is not running (then every range is outside, as mtl_addr says, and release is not called).
enum { VKB_POOLS_HOLD_MAX = 64 };   // lookups per hold: a writer waits for at most this many
int vkb_pools_hold(void);
void vkb_pools_release(void);
int vkb_in_pool_held(uint64_t addr, uint64_t len);
int vkb_cpu_count(void);                       // vkb_bc7.c (Windows)
void vkb_thread_below_normal(void);
uint64_t vkb_bc7_read_bytes(void);
void vkb_bc7_prefetch_start(void);             // reads the used-textures list's blocks into memory in the background

vkb_image *vkb_image_of(const tex_t *t);   // creates it on first use; NULL for textures that cannot be images yet
// Moves the image to `layout` before work of `stage`/`access` (records a barrier on the current command buffer).
void vkb_image_use(VkCommandBuffer cb, vkb_image *im, VkImageLayout layout, VkPipelineStageFlags2 stage, VkAccessFlags2 access);
// A view of the image as NVN's view (NULL = the whole texture); cached per image.
// attachment: 0 sampled, 1 render target, 2 storage image. view_type: a VkImageViewType the shader samples it as, or -1
// (from the texture / view target). VK_NULL_HANDLE when the image cannot be used so.
VkImageView vkb_view(vkb_image *im, const view_t *v, int attachment, int view_type);
// [UIT 0124] the view texture object t samples im through (v, or *tmp with t's own swizzle when im is shared with a
// descriptor of another swizzle); vkb_tex_share_swizzle: TOTK_TEX_SHARE_SWIZZLE (default on)
const view_t *vkb_sample_swizzle(const vkb_image *im, const tex_t *t, const view_t *v, view_t *tmp);
int vkb_tex_share_swizzle(void);
// Brings the image's contents up to date from game memory when the CPU wrote them (records copies on cb; outside a pass).
// 1 when it recorded an upload.
int vkb_tex_upload(VkCommandBuffer cb, vkb_image *im);
uint64_t vkb_sampler_one_use(void);   // [UIT 0207] samplers made while their registration changed (vkb_draw.c)
void vkb_view_stats(uint64_t *made, uint64_t *dropped);   // [UIT 0200] image views created / not kept since the last call
void vkb_tex_image_stats(uint64_t *made, uint64_t *shared, uint64_t *taken_back);   // images by memory and shape
void vkb_tex_scan_stats(uint64_t n[5], uint64_t vis[5], uint64_t ns[5]);   // [UIT 0230] image scans with the lock held, totals so far
void vkb_tex_quiesce(void);   // [UIT 0233] vkb_mt_submit before recording: frees old image tables
void vkb_tex_unlocked_stats(uint64_t *images, uint64_t *views, uint64_t *lost);   // [UIT 0231] made without the images lock / beaten by another thread, totals

// ---- vkb_draw.c: device-local mirrors of written game memory for the current submission
int vkb_gpu_addr(uint64_t addr, uint64_t len, VkBuffer *buf, VkDeviceSize *off);   // the mirror, else the imported pool
void vkb_mirrors_evict(uint64_t addr, uint64_t len);   // mirrors straddling the range go back (outside a pass)
void vkb_mirrors_submit(VkCommandBuffer cb);           // end of a submission: all back to game memory

// ---- backend_vk.c / vkb_shadow.c: copies of the memory pools in device memory (discrete GPUs)
void vkb_pool_use(uint64_t addr, uint64_t len, int write);   // before a GPU access is recorded (write: may end the pass)
int vkb_pool_addr(uint64_t addr, uint64_t len, VkBuffer *buf, VkDeviceSize *off);   // the copy when current, else game memory
uint64_t vkb_pool_extent(uint64_t addr);   // [UIT 0331] bytes from addr to the end of its pool (0: in none)
int vkb_shadow_on(void);
uint32_t vkb_shadow_alloc(VkDeviceSize size, VkBuffer *buf, VkDeviceSize *off);   // 0: none
void vkb_shadow_free(uint32_t h);   // reused after the submission being recorded
uint64_t vkb_shadow_bytes(void);

// ---- vkb_ucap.c: uniform contents as of the submit call
int vkb_ucap_on(void);
uint64_t mtl_done(void);   // backend_vk.c: the last submission the GPU completed
int vkb_ucap_range(uint64_t addr, uint64_t size, VkBuffer *buf, VkDeviceSize *off);   // the copy of a bound uniform range
void vkb_ucap_read(uint64_t addr, void *out, size_t n);   // CPU read of uniform memory, from the copy where there is one
void vkb_ucap_drop(uint64_t addr, uint64_t len);
// [UIT 0335] GPU writes to memory in the submission (inline updates, buffer copies), at this position: later CPU reads
// (vkb_ucap_read) see their bytes. Several recording threads: the walk notes them (recording threads are ignored)
void vkb_ucap_note_write(uint64_t dst, uint64_t n, const void *data);
void vkb_ucap_note_copy(uint64_t src, uint64_t dst, uint64_t n);
void vkb_ucap_gpuview_stats(uint64_t *logged, uint64_t *over, uint64_t *reads);
int vkb_ucap_why(uint64_t addr, uint64_t size);          // a GPU write: copies it overlaps are no longer used
void vkb_ucap_geo_stats(uint64_t *seen, uint64_t *ranges, uint64_t *bytes, uint64_t *gpu);   // [UIT 0018] vertex / index ranges seen / copied at submit; [0027] gpu: read memory, GPU-written
int vkb_geo_snapshot(void);
void vkb_ucap_stats(uint64_t *caps, uint64_t *bytes, uint64_t *ns, uint64_t *hits, uint64_t *drops);
void vkb_ucap_phase_stats(uint64_t ns[4], uint64_t *ranges);   // [UIT 0145] walk, vertex / index checks, sort + merge, copy
// [UIT 0157] an inline update (data) or buffer copy (source sb at so) of [dst, dst+n) recorded in cb: dropped from the copy
// only where it writes, and with one recording thread written into the copy too (instead of vkb_ucap_drop)
void vkb_ucap_gpu_write(VkCommandBuffer cb, uint64_t dst, uint64_t n, const uint8_t *data, VkBuffer sb, VkDeviceSize so);
void vkb_ucap_fine_stats(uint64_t *records, uint64_t *overflow, uint64_t *applied, uint64_t *saved);
// [UIT 0157] a binding against the copy (diagnostics): why as vkb_ucap_why; cap: the captured bytes (NULL: not captured);
// ndiff: bytes where the copy and memory differ now; nout: those not under a GPU write recorded so far (there memory holds
// what the game wrote after the submit call: what a binding reading memory gets wrong); first: the first such offset
typedef struct { int why; const uint8_t *cap; uint32_t ndiff, nout, first; } vkb_ucap_cmp;
void vkb_ucap_compare(uint64_t addr, uint64_t size, vkb_ucap_cmp *r);

// ---- vkb_bc7.c: ASTC textures as BC7, from a cache by the contents of the ASTC blocks
typedef struct { uint64_t a, b; } vkb_bc7_key;
int vkb_bc7_on(void);   // TOTK_BC7=0: ASTC decoded to RGBA8 (A/B)
// The key of one surface: its ASTC blocks in rows (n bytes), its size in texels (depth: slices of a 3D level), block size, sRGB.
vkb_bc7_key vkb_bc7_key_of(const uint8_t *blocks, size_t n, uint32_t w, uint32_t h, uint32_t depth, uint32_t bw, uint32_t bh, int srgb);
enum { VKB_BC_7 = 0, VKB_BC_1 = 1 };                         // the format of a cache entry
int vkb_bc7_lookup(vkb_bc7_key k, int *fmt, size_t *len);    // in the cache: its format and length
int vkb_bc7_has(vkb_bc7_key k, size_t n);                    // n bytes of BC7 blocks for it in the cache
int vkb_bc7_read(vkb_bc7_key k, uint8_t *out, size_t n);
void vkb_bc7_store(vkb_bc7_key k, const uint8_t *data, size_t n);
void vkb_bc_store(vkb_bc7_key k, const uint8_t *data, size_t n, int fmt);
void vkb_bc7_queue(vkb_bc7_key k, const uint8_t *rgba, uint32_t w, uint32_t h, uint32_t d, int srgb);   // encoded in the background
void vkb_bc7_encode_now(vkb_bc7_key k, const uint8_t *rgba, uint32_t w, uint32_t h, uint32_t d, int srgb, int fmt, uint8_t *out);
void vkb_bc7_stats(uint64_t *hits, uint64_t *misses, uint64_t *encoded, uint64_t *dropped, uint64_t *encode_ns, size_t *queued);

// ---- vkb_desc.c: descriptors in buffers (VK_EXT_descriptor_buffer)
int vkb_db_on(void);
void vkb_db_layouts(const VkDescriptorSetLayout layout[2]);
void vkb_db_slot_reset(int slot);
uint64_t vkb_db_gen(void);   // changes when offsets made before are void (descriptor memory replaced)
VkDeviceSize vkb_db_set(VkCommandBuffer cb, int slot, uint64_t rec, const VkWriteDescriptorSet *w, int n, int compute);
void vkb_db_bind(VkCommandBuffer cb, int slot, uint64_t rec);   // the memory offsets refer to, bound to this command buffer
void vkb_db_stats(uint64_t *get_calls, uint64_t *cache_hits);

// ---- vkb_copy.c: NVN copies between game memory and textures (outside a pass; they end one)
void vkb_copy_stride(int64_t stride);                       // SetCopyRowStride
int64_t vkb_copy_stride_get(void);
int vkb_copy_t2b_range(const p_copy_b2t *c, uint64_t *addr, uint64_t *len);   // the buffer range CopyTextureToBuffer writes
void vkb_copy_buf_tex(const p_copy_b2t *c, int to_tex);     // CopyBufferToTexture (1) / CopyTextureToBuffer (0)
void vkb_copy_tex_tex(const p_copy_t2t *c);                 // CopyTextureToTexture
int vkb_tex_stale(vkb_image *im);          // the CPU changed its game memory since the upload (checked once per submission)
VkFormat vkb_format(int nvn_format, VkImageAspectFlags *aspect, int *astc);
void vkb_tex_release(const tex_t *t);
int vkb_tex_snap_wanted(const char *vkey, const char *fkey, uint64_t frame);   // TOTK_SNAP (development)
void vkb_tex_snap(VkCommandBuffer cb, vkb_image *im, const char *what);
void vkb_tex_snap_flush(void);
void vkb_tex_lumlog(const tex_t *t, uint64_t present);   // TOTK_LUMLOG (development)
void vkb_tex_dump(const char *dir);         // TOTK_DUMP: the GPU-written images as BMP (development)       // the game finalized the texture: image freed after the GPU is done

// ---- vkb_draw.c: render state, texture / sampler pools, draws
void vkb_slot_reset(int slot);                  // the slot's previous submission has completed: reuse its memory
void vkb_state_reset(void);
void vkb_state(uint32_t op, const uint8_t *payload);   // state / binding commands
void vkb_draw(const uint8_t *payload, int indexed);    // OP_DRAW / OP_DRAW_IDX
void vkb_dispatch(const uint8_t *payload);             // OP_DISPATCH
void vkb_draw_stats(uint64_t *draws, uint64_t *skipped);
// [UIT 0157] uniform bindings (c3 on) by where they read: [0] all, [1] the copy, [2] memory as dropped by a GPU write,
// [3] of those differing from the copy outside the GPU-written bytes, [4] not in the copy, [5] of those over 64 KiB,
// [6] another slot's copy, [7] the copy misaligned
enum { VKB_UV_N = 8 };
int vkb_ubo_verify_stats(uint64_t v[VKB_UV_N]);   // 0: TOTK_VK_UCAP_VERIFY is off (nothing counted)
void vkb_draw_why_log(uint64_t present, uint64_t every);   // [UIT 0053] the "[draws]" lines (vkb_draw.c)
size_t vkb_state_size(void);                    // the render state as bytes (a chunk's starting state, vkb_mt.c)
void vkb_state_save(void *dst);
void vkb_state_load(const void *src);
void vkb_rec_begin(void);                       // this thread starts a command buffer: its recording caches start empty
void vkb_draw_mt_init(void);                    // objects made on first use, made before threads record (prologue)
void vkb_draw_prescan(const uint8_t *payload, int indexed, int dispatch);   // the GPU writes a draw / dispatch makes (ucap)

// ---- vkb_mt.c: submissions recorded by several threads (2026-10-24)
// The translation thread walks a submission once in order (vkb_mt_submit): render state, events and counters,
// conditional rendering, the GPU writes that void captured uniforms, and cuts it into chunks at render target changes,
// each with its starting state. Recording threads (the translation thread too, once the walk is done) record the chunks
// into command buffers of their own. Work that must come before every chunk (texture uploads, pool copies) goes into the
// submission's prologue (vkb_pro_begin / vkb_pro_end, under a lock). Each chunk tracks the layouts its images need first
// and leave last (vkb_trk); between chunks a small command buffer moves them (and waits for all earlier work).
int vkb_mt_on(void);             // TOTK_VK_RECORD_THREADS (default: by the number of processors; 1: single thread)
int vkb_mt_recording(void);      // this thread is recording a chunk or the prologue
uint64_t vkb_seq(void);          // the position of the command being replayed within its submission
void vkb_seq_set(uint64_t seq);
void vkb_pro_begin(void);        // records into the prologue from here (a no-op on one thread)
void vkb_pro_end(void);
void vkb_cmd_set(VkCommandBuffer cb);   // this thread's command buffer (VK_NULL_HANDLE: the submission's own)
void vkb_mt_slot_reset(int slot);       // the slot's previous submission has completed (backend_vk.c mtl_sub_begin)
void vkb_mt_submit(const uint64_t *h, int n);   // records the submission's command handles (after mtl_sub_begin)
// Image layouts per chunk (vkb_tex.c): the first layout each image needs in it and the one it leaves it in.
typedef struct { vkb_image *im; VkImageLayout first, cur; } vkb_trk_e;
typedef struct { vkb_trk_e *e; int n, cap; int *tab; int tcap; } vkb_trk;
void vkb_trk_set(vkb_trk *t);    // this thread records with this tracking (NULL: images' own layouts, one thread)
vkb_trk *vkb_trk_get(void);
void vkb_trk_reset(vkb_trk *t);
int vkb_layout_is(vkb_image *im, VkImageLayout l);   // the image is in l as recorded so far (a first use: it will be)
VkImageLayout vkb_layout(vkb_image *im);              // its layout as recorded so far

// ---- vkb_dlss.c: DLSS in place of the game's upscale draw (src/vk/dlss.h); the draw state's part of it
typedef struct { int gb_seen, has_gdepth, has_comp, jit_on; float jx, jy; tex_t gdepth, comp; } vkb_dl_state;
typedef struct {
    int role;                    // 1: the walk (vkb_draw_prescan), 0: a chunk's recording, 2: both (one thread)
    const uint8_t *a;            // the draw's payload (identifies the draw between the walk and the recording)
    const p_set_rt *rt;
    int count;                   // vertices / indices
    const char *fs_key;
    int easu;                    // vkb_dl_easu(fs_key)
    int blend, a2c, ds_test, nvp, has_sc;
    float vp[4];
    p_scissor sc;
    struct vkb_image *src;       // the upscale draw's texture (recording only)
    uint32_t src_w, src_h;       // [UIT 0160] its size from the texture registry (the walk too: it has no image)
    p_bind_buf con, cam;         // fragment c0, vertex c1 (the camera)
} vkb_dl_in;
int vkb_dl_on(void);                         // DLSS can run (Streamline loaded, GPU supported)
int vkb_dl_easu(const char *key);            // the game's upscale shader: 2 HDR, 1 display colour, 0 not
int vkb_dl_draw(vkb_dl_state *st, const vkb_dl_in *d);   // 1: DLSS replaced this draw (skip it)
int vkb_dl_jittered(const vkb_dl_state *st, const p_set_rt *rt, int count);   // jitter this draw's viewport
void vkb_dl_failed(void);
void vkb_rec_invalidate(void);               // vkb_draw.c: forget the bound pipeline / sets (foreign commands were recorded)
