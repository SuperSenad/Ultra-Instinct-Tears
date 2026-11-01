// The NVN replayer on Vulkan (milestone 3.3): the mtl_* execution layer that MacNativeGames implements on Metal
// (nvn_mtl.m), written anew for Vulkan against the same interface (nvn_mtl.h, nvn_obj.h). Step 1: memory pools,
// submissions and their completion; the recorded command stream is walked, and the commands with CPU-visible effects
// (events, counters, buffer copies and updates) run; drawing comes in the next steps.
//  - memory pools: the game's memory is imported as VkBuffers (VK_EXT_external_memory_host), no copies; a GPU address
//    is the CPU address, and mtl_addr finds the buffer and offset for it;
//  - a submission is one command buffer; its fence is the value it signals on one timeline semaphore;
//  - visible effects of completion (event values, counter records) are written by the retire thread after the GPU has
//    finished the submission, in submission order, and only then is the fence complete for mtl_wait (as on Metal).
// Without a usable Vulkan device everything completes at once (the null device's behaviour, events still written).
#include <stdarg.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#include "compat/gfx_compat.h"
#include "host/hle.h"
#include "nvn/nvn_dirty.h"
#include "nvn/nvn_obj.h"
#include "../vk/present.h"
#include "vkb.h"
#include "vkb_imgwb.h"
#include "vkb_pool_tcache.h"
#include "vkb_wb3.h"
#include "vkb_shadow_gap.h"
#include "vkb_shader.h"
#include "vkb_audit.h"
#include "vkb_capture.h"   // [UIT 0157]
#include "vkb_lock.h"
#include "nvn/totk_timeline.h"
#include "uit_prof.h"
#include "uit_crit.h"   // [UIT 0232] TOTK_CRITPATH

#define VLOG(...) LOG("[vk] " __VA_ARGS__)

static _Atomic int g_ok = -1;   // -1 not initialised, 0 no Vulkan (null behaviour), 1 ready
static VkSemaphore g_timeline;
static uint64_t g_fence;                 // last submitted (submit thread; read under g_mu by others)
static uint64_t g_done;                  // last retired (under g_mu)
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_cv = PTHREAD_COND_INITIALIZER;
// [perf-mt] TOTK_VK_ASYNC_SUBMIT=1: vkQueueSubmit2 on a thread of its own (vkb_subq.h; mtl_sub_commit, submit_thread)
#include "vkb_subq.h"
static vkb_subq g_subq = VKB_SUBQ_INIT;
static _Atomic int g_async;   // set once, in backend_init, before the first submission with Vulkan
static _Atomic uint64_t g_async_lag_ns, g_async_n;   // pushed -> vkQueueSubmit2 called (the per-frame log line)
static _Atomic uint64_t g_ret_ns, g_ret_n;   // [UIT 0250] retire thread: GPU wait returned -> the fence marked complete
static int g_ret_pc = -1;   // [UIT 0250] retire thread placement: CPUs it was given (0: where Windows puts it)

void mtl_finish(void);
// ---- deferred destruction (vkb_defer.h): what is given up while a submission may be recorded on another thread
typedef struct { VkImageView view; VkImage img; VkDeviceMemory mem; VkBuffer buf; VkSampler samp; int pool; void *alloc; } vkd_obj;   // alloc: [UIT 0075] vkb_mem.h
// pool: 1 = a memory pool's import, 2 = [UIT 0133] an image's own memory from vkb_mem_image (both counted by vkb_mem_count)
#include "vkb_defer.h"
#include "vkb_occl.h"   // [UIT 0049] samples-passed counters (TOTK_VK_OCCLUSION)
static vkd_q g_dq = VKD_Q_INIT;
// [UIT 0045] Submissions being recorded: from mtl_sub_begin until queued (mtl_sub_commit). A count: ReadTexels and
// WriteTexels record theirs on a game thread (vkb_tex.c) while the translation thread may record one.
static _Atomic int g_open;
static __thread int t_open;   // this thread's is counted
static void open_end(void) { if (t_open) { t_open = 0; atomic_fetch_sub(&g_open, 1); } }
#include "vkb_recown.h"
#include "vkb_mem.h"   // [UIT 0075]
static vkb_recown g_recown;   // [UIT 0047] the thread recording: two at once is logged (vkb_recown.h)
static uint64_t g_pool_deferred, g_pool_now, g_pool_waits;   // pool deletes: deferred / destroyed at once; imports that waited
static void pool_gone_wait(uintptr_t a, uintptr_t b);
static void pool_gone_tag(uintptr_t a, uintptr_t b);
static void dead_reap(uint64_t done);
static void pass_end(void);
static void pass_end_why(int why);
int mtl_wait(uint64_t fence, uint64_t timeout_ns);
static uint64_t gpu_ticks(void) { return runtime_now_ns() * 12 / 625; }   // the Switch's 19.2 MHz counter

// ---------------- memory pools ----------------
// sh..defer_pass: the pool's copy in device memory (below, "copies of the pools in device memory")
typedef struct { uintptr_t lo, hi, base; VkBuffer buf; VkDeviceMemory mem; const void *key; VkDeviceSize bytes;
                 VkBuffer sbuf; VkDeviceSize soff; uint32_t sh; uint8_t no_shadow, filled; uint64_t stamp, sync_sub, used_sub, defer_pass, wb_sub;
                 uint64_t pend_sub;
                 uint64_t fc; } pool_ent;   // pend_sub: the last submission whose sync found GPU writes not in game memory yet
// fc: [UIT 0258] the first chunk that used the pool and its submission (vkb_presync_chunk.h; 0: none)
static pool_ent *g_pools;
static int g_npools, g_capools;
static vkb_rw g_pool_mu;   // recording threads look pools up for every draw (read); changes write
// [UIT 0033] the pool table's generation: bumped (g_pool_mu held for writing) by every pool added, deleted or given
// up, so per-thread entries of pools current in a submission (vkb_pool_tcache.h) are never used across a change
static _Atomic uint64_t g_pool_gen = 1;
static void pool_gen_bump(void) { atomic_fetch_add_explicit(&g_pool_gen, 1, memory_order_release); }
static uint64_t g_pool_bytes;

// vkb_lock.h: Windows slim reader-writer locks (zero-initialised = unlocked), with the wait measured as vkb_lock does
void vkb_rw_read(vkb_rw *l, int kind) {
    if (TryAcquireSRWLockShared((PSRWLOCK)l)) return;
    uint64_t t0 = runtime_now_ns();
    AcquireSRWLockShared((PSRWLOCK)l);
    atomic_fetch_add_explicit(&vkb_lock_wait_ns[kind], runtime_now_ns() - t0, memory_order_relaxed);
    atomic_fetch_add_explicit(&vkb_lock_waits[kind], 1, memory_order_relaxed);
}
void vkb_rw_read_end(vkb_rw *l) { ReleaseSRWLockShared((PSRWLOCK)l); }
void vkb_rw_write(vkb_rw *l, int kind) {
    if (TryAcquireSRWLockExclusive((PSRWLOCK)l)) return;
    uint64_t t0 = runtime_now_ns();
    AcquireSRWLockExclusive((PSRWLOCK)l);
    atomic_fetch_add_explicit(&vkb_lock_wait_ns[kind], runtime_now_ns() - t0, memory_order_relaxed);
    atomic_fetch_add_explicit(&vkb_lock_waits[kind], 1, memory_order_relaxed);
}
void vkb_rw_write_end(vkb_rw *l) { ReleaseSRWLockExclusive((PSRWLOCK)l); }

// ---- per recording thread (vkb_mt.c design, implemented below in "several recording threads")
static __thread int t_worker;          // vkb_worker(): 0 the translation thread (and any other), 1.. the recording threads
static __thread int t_recording;       // recording a chunk (or the prologue) of a submission
static __thread VkCommandBuffer t_cb;  // the command buffer it records (VK_NULL_HANDLE: the submission's own)
static __thread uint64_t t_seq;        // the position of the command being replayed
int vkb_worker(void) { return t_worker; }
int vkb_mt_recording(void) { return t_recording; }
uint64_t vkb_seq(void) { return t_seq; }
void vkb_seq_set(uint64_t seq) { t_seq = seq; }
void vkb_cmd_set(VkCommandBuffer cb) { t_cb = cb; }

static void wb_forget(VkBuffer host);
static uint32_t memory_type(uint32_t bits) {
    for (uint32_t i = 0; i < vkc.memory.memoryTypeCount; i++) if (bits & (1u << i)) return i;
    return UINT32_MAX;
}

// Commands recorded per frame (the present log line every 300 presents): barriers with their image layout changes and
// render passes begun. Counted by wrapping the two functions once the device exists.
static PFN_vkCmdPipelineBarrier2 g_real_barrier;
static PFN_vkCmdBeginRendering g_real_begin;
static vkb_cnt g_n_barriers, g_n_img_barriers, g_n_passes;
static uint64_t g_parts;
_Atomic uint64_t vkb_lock_wait_ns[LK_COUNT], vkb_lock_waits[LK_COUNT], vkb_step_ns[PK_COUNT], vkb_step_n[PK_COUNT];
static void VKAPI_CALL count_barrier(VkCommandBuffer cb, const VkDependencyInfo *d) {
    vkb_cnt_add(g_n_barriers, 1); vkb_cnt_add(g_n_img_barriers, d->imageMemoryBarrierCount);
    g_real_barrier(cb, d);
}
static void VKAPI_CALL count_begin(VkCommandBuffer cb, const VkRenderingInfo *r) { vkb_cnt_add(g_n_passes, 1); g_real_begin(cb, r); }

static void backend_init(void) {
    if (g_ok >= 0) return;
    static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_lock(&mu);
    if (g_ok < 0) {
        int ok = vk_init(NULL);   // already done by the window when there is one
        if (ok) {
            VkSemaphoreTypeCreateInfo ti = {VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO, .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE};
            VkSemaphoreCreateInfo si = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, &ti};
            ok = vk_check(vk.vkCreateSemaphore(vkc.device, &si, NULL, &g_timeline), "timeline semaphore");
        }
        void retire_start(void);
        if (ok) retire_start();
        void submit_start(void);
        if (ok) submit_start();   // [perf-mt] TOTK_VK_ASYNC_SUBMIT=1 only
        if (ok) {
            g_real_barrier = vk.vkCmdPipelineBarrier2; vk.vkCmdPipelineBarrier2 = count_barrier;
            g_real_begin = vk.vkCmdBeginRendering; vk.vkCmdBeginRendering = count_begin;
        }
        vkb_state_reset();
        VLOG("replayer %s", ok ? "on Vulkan" : "without a GPU: submissions complete at once");
        atomic_store(&g_ok, ok);
    }
    pthread_mutex_unlock(&mu);
}

void mtl_pool_add(const void *mem, uint64_t size) {
    backend_init();
    if (!g_ok || !mem || !size) return;
    uintptr_t align = vkc.host_pointer_alignment ? vkc.host_pointer_alignment : 4096;
    uintptr_t lo = (uintptr_t)mem, hi = lo + size, a = lo & ~(align - 1), b = (hi + align - 1) & ~(align - 1);
    // The import needs committed memory: guest pages are committed on first touch, so the pool is committed now.
    // A virtual pool is an inaccessible address range (totk_vpool_win.c): no memory to import (sparse binding, later).
    MEMORY_BASIC_INFORMATION mi;
    if (a >= GUEST_LO && b <= GUEST_HI) plat_commit(a, b - a);
    else if (!VirtualQuery((void *)a, &mi, sizeof mi) || mi.State != MEM_COMMIT || mi.Protect == PAGE_NOACCESS) return;
    pool_gone_wait(a, b);   // [UIT 0045] a deleted pool's import of this memory is destroyed first
    VkMemoryHostPointerPropertiesEXT hp = {VK_STRUCTURE_TYPE_MEMORY_HOST_POINTER_PROPERTIES_EXT};
    if (!vk_check(vk.vkGetMemoryHostPointerPropertiesEXT(vkc.device, VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT,
                                                          (void *)a, &hp), "host pointer properties")) return;
    VkExternalMemoryBufferCreateInfo eb = {VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO,
                                           .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT};
    VkBufferCreateInfo bi = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, &eb, .size = b - a,
        .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT |
                 VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
                 VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    VkBuffer buf;
    if (!vk_check(vk.vkCreateBuffer(vkc.device, &bi, NULL, &buf), "pool buffer")) return;
    VkMemoryRequirements req;
    vk.vkGetBufferMemoryRequirements(vkc.device, buf, &req);
    uint32_t type = memory_type(req.memoryTypeBits & hp.memoryTypeBits);
    VkImportMemoryHostPointerInfoEXT imp = {VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT,
        .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT, .pHostPointer = (void *)a};
    VkMemoryAllocateFlagsInfo fl = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO, &imp, .flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT};
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &fl, .allocationSize = b - a, .memoryTypeIndex = type};
    VkDeviceMemory dm;
    if (type == UINT32_MAX || !vk_check(vk.vkAllocateMemory(vkc.device, &ai, NULL, &dm), "pool import")) {
        VLOG("memory pool %p+%#llx: cannot import (memory types %#x / %#x)", mem, (unsigned long long)size, req.memoryTypeBits, hp.memoryTypeBits);
        vk.vkDestroyBuffer(vkc.device, buf, NULL);
        return;
    }
    vk.vkBindBufferMemory(vkc.device, buf, dm, 0);
    vkb_mem_count(+1);   // [UIT 0133] the driver's allocation count
    vkb_rw_write(&g_pool_mu, LK_POOLS);
    if (g_npools == g_capools) {
        g_capools = g_capools ? g_capools * 2 : 64;
        g_pools = realloc(g_pools, (size_t)g_capools * sizeof *g_pools);
        if (!g_pools) die("pool table");
    }
    int i = g_npools;
    while (i > 0 && g_pools[i - 1].lo > lo) { g_pools[i] = g_pools[i - 1]; i--; }
    g_pools[i] = (pool_ent){lo, hi, a, buf, dm, mem, b - a};
    g_npools++;
    for (int j = i - 4; j <= i + 4; j++)   // overlapping pools: one copy each would diverge, so neither is copied
        if (j >= 0 && j < g_npools && j != i && g_pools[j].lo < hi && lo < g_pools[j].hi) {
            static int said;
            if (said++ < 8) VLOG("memory pool %p+%#llx overlaps pool %#llx..%#llx: neither is copied to device memory", mem,
                                 (unsigned long long)size, (unsigned long long)g_pools[j].lo, (unsigned long long)g_pools[j].hi);
            g_pools[i].no_shadow = g_pools[j].no_shadow = 1;
            if (g_pools[j].sh) { vkb_shadow_free(g_pools[j].sh); g_pools[j].sh = 0; }
        }
    g_pool_bytes += hi - lo;
    pool_gen_bump();
    static unsigned said;
    if (said++ < 4) VLOG("memory pool %p+%#llx imported (%d pools, %.1f MiB)", mem, (unsigned long long)size, g_npools, g_pool_bytes / 1048576.0);
    vkb_rw_write_end(&g_pool_mu);
}

void mtl_pool_del(const void *mem) {
    if (g_ok != 1) return;
    pool_ent e = {0};
    vkb_rw_write(&g_pool_mu, LK_POOLS);
    for (int i = 0; i < g_npools; i++)
        if (g_pools[i].key == mem) {
            e = g_pools[i];
            memmove(&g_pools[i], &g_pools[i + 1], (size_t)(g_npools - i - 1) * sizeof *g_pools);
            g_npools--;
            g_pool_bytes -= e.hi - e.lo;
            pool_gen_bump();
            break;
        }
    if (e.sh) vkb_shadow_free(e.sh);   // a piece is reused only after the open submission (vkb_shadow.c)
    if (e.buf) wb_forget(e.buf);
    // [UIT 0045] Read under the table's lock, after the entry is gone: a submission that found the pool (presync and
    // gather copies, draws through vkb_pool_addr / mtl_addr, the per-thread cache) began before this and is open, or
    // it is already submitted. One begun after this cannot find it.
    atomic_thread_fence(memory_order_seq_cst);
    int open = atomic_load(&g_open);
    vkb_rw_write_end(&g_pool_mu);
    if (!e.buf) return;
    // The frontend drains its queue before it deletes a pool (hle_nvn.c x_drain), but another game thread's submission
    // can be recorded meanwhile: x_drain does not wait for that one. Destroying the buffer after mtl_finish (submitted
    // work only), as before 0045, freed what that open submission's pool copies read: the device was lost at
    // "pool copies in" (2026-10-30). mtl_finish still runs: write-backs of submitted work land in game memory before
    // the game reuses it (retire thread, wb_merge).
    mtl_finish();
    if (open) {
        vkd_push(&g_dq, (vkd_obj){.buf = e.buf, .mem = e.mem, .pool = 1}, e.base, e.base + e.bytes);
        pool_gone_tag(e.base, e.base + e.bytes);   // that submission committed meanwhile: its fence
        __atomic_add_fetch(&g_pool_deferred, 1, __ATOMIC_RELAXED);
    } else {
        vk.vkDestroyBuffer(vkc.device, e.buf, NULL);
        vk.vkFreeMemory(vkc.device, e.mem, NULL);
        vkb_mem_count(-1);
        __atomic_add_fetch(&g_pool_now, 1, __ATOMIC_RELAXED);
    }
    if (vkc.has_checkpoints) VLOG("memory pool %p %#llx..%#llx deleted (present %llu)%s", mem, (unsigned long long)e.lo, (unsigned long long)e.hi,
                                  (unsigned long long)vkb_frame(), open ? ": destroyed after the submission being recorded" : "");
}
// A deleted pool queued while a submission was open, with no fence yet: once no submission is open, that one has
// committed (mtl_last_fence or before) and no later one can find the pool.
static void pool_gone_tag(uintptr_t a, uintptr_t b) {
    if (!atomic_load(&g_open)) vkd_tag_range(&g_dq, a, b, mtl_last_fence());
}
// An import of memory a deleted pool's import (not destroyed yet) also covers: that one is destroyed first. Two live
// imports of one host range are not known to work on every driver. Its submission is being recorded or is submitted:
// both end without this thread. At most 2 s, then the import goes ahead (logged).
static void pool_gone_wait(uintptr_t a, uintptr_t b) {
    uint64_t t0 = 0, f = 0;
    int r;
    while ((r = vkd_find(&g_dq, a, b, &f))) {
        if (!t0) { t0 = runtime_now_ns(); __atomic_add_fetch(&g_pool_waits, 1, __ATOMIC_RELAXED); }
        uint64_t now = runtime_now_ns();
        if (now - t0 > 2000000000ull) {
            static int said;
            if (said++ < 8) VLOG("memory pool %#llx..%#llx: a deleted pool's import of it is still in use after 2 s; importing anyway",
                                 (unsigned long long)a, (unsigned long long)b);
            return;
        }
        if (r == 2) pool_gone_tag(a, b);   // no fence yet: its submission may have committed before the delete queued it
        r = vkd_find(&g_dq, a, b, &f);
        if (r == 1) mtl_wait(f, 2000000000ull - (now - t0));   // the retire thread destroys it once f is done
        else if (r) { struct timespec ts = {0, 200000}; nanosleep(&ts, NULL); }   // committing on its thread / being destroyed
        if (r == 1 && mtl_wait(f, 0)) dead_reap(f);   // done: destroyed here if the retire thread has not yet
    }
}

// [UIT 0053] Counted for the "[draws]" line: the game asks to read what the GPU wrote (yuzu's TotK grass vanishes the
// same way when GPU-written memory is not flushed back for such reads, "reactive flushing" off).
static uint64_t g_inval_calls, g_inval_bytes;
static void iwb_explicit_read(uint64_t lo, uint64_t hi);   // [UIT 0054]
void mtl_pool_invalidate(const pool_t *p, uint64_t off, uint64_t n) {   // zero-copy: nothing cached
    __atomic_fetch_add(&g_inval_calls, 1, __ATOMIC_RELAXED);
    __atomic_fetch_add(&g_inval_bytes, n, __ATOMIC_RELAXED);
    // [UIT 0054] what the GPU drew there comes back first (TOTK_VK_IMAGE_WRITEBACK; the first such reads logged in any mode)
    if (g_ok != 1 || !p || !p->mem || !n || off >= p->size) return;
    uint64_t lo = (uint64_t)(uintptr_t)p->mem + off, hi = lo + (n < p->size - off ? n : p->size - off);
    // [UIT 0057] the cached range answer first: most reads cover no GPU-drawn image, and then the full image scan of
    // vkb_iwb_wanted (every image, under the images lock) is skipped
    if (vkb_iwb_note_inval(lo, hi) && vkb_iwb_mode() != IWB_OFF && vkb_iwb_wanted(lo, hi)) iwb_explicit_read(lo, hi);
}

static __thread int g_pool_last;   // the entry this thread found last (draws use a few pools over and over)
static const pool_ent *pool_find(uintptr_t addr, uint64_t len) {   // g_pool_mu held
    if (g_pool_last < g_npools && addr >= g_pools[g_pool_last].lo && addr + len <= g_pools[g_pool_last].hi) return &g_pools[g_pool_last];
    int lo = 0, hi = g_npools - 1, found = -1;
    while (lo <= hi) {
        int m = (lo + hi) / 2;
        if (g_pools[m].lo <= addr) { found = m; lo = m + 1; } else hi = m - 1;
    }
    for (int i = found; i >= 0 && i > found - 4; i--)   // nested / adjacent pools: the closest few that start below
        if (addr >= g_pools[i].lo && addr + len <= g_pools[i].hi) { g_pool_last = i; return &g_pools[i]; }
    return NULL;
}

// ---------------- copies of the pools in device memory (discrete GPUs; memory: vkb_shadow.c) ----------------
// Game memory is host memory imported into Vulkan, so on a discrete GPU every vertex, index and constant a draw reads
// crosses PCIe: on the Great Sky Island 28 of the frame's 34 ms of GPU time were spent inside render passes, 7.6 ms with
// the pools copied to device memory (experiment, 2026-10-24). A pool the GPU uses gets a copy in device memory; in a
// submission, once the copy is current, every GPU access to the pool goes to it (vkb_pool_addr). Keeping it current:
//  - CPU writes: pages written since the copy was last brought up to date (the write watch, nvn_dirty_runs) are copied
//    in when the pool is first used in a submission. In a render pass (no copies there) a pool with written pages stays
//    in game memory until the pass ends, unless a shader writes it (then the pass ends); a clean one switches at once.
//    At the first pass of a submission, the pools used in the two before are brought up to date together.
//  - GPU writes (shader stores, buffer copies, uniform updates, texture-to-buffer copies) go to the copy and are copied
//    back to game memory at the end of the submission, through host memory and the retire thread (wb_merge), which
//    leaves alone the pages the CPU wrote meanwhile (the CPU reads the results after the game's wait).
//    [temporary] Page granularity: GPU writes into a page the CPU also wrote are lost (counted in the log line).
// Pages the copy back writes are not CPU writes; were the write watch to count them, they would only be copied in again.
// The game writes memory for a submission before it is submitted, and does not overwrite what the GPU still reads (it
// waits for the GPU first), so bringing a copy up to date early (ahead, or for the whole pool) does not change results.
// Integrated GPUs (the Steam Deck) read host memory at full speed: no copies there (vkb_shadow_on).
typedef struct { VkBuffer src, dst; VkDeviceSize soff, doff, n; uint64_t base, stamp; } wb_t;   // base: the pool's import address; stamp: its copy's
static wb_t *g_wb;                 // GPU-written ranges of copies in this submission: back to game memory at its end
static int g_nwb, g_capwb;
static VkBufferCopy *g_runs;       // copies being gathered (sync_gather / sync_flush)
static int g_nruns, g_caruns;
typedef struct { VkBuffer src, dst; int first, n; } run_grp;
static run_grp *g_grp;
static int g_ngrp, g_capgrp;
static uint64_t g_shadow_budget, g_presync_sub;
static uint64_t g_sh_in, g_sh_in_bytes, g_sh_out_bytes, g_sh_deferred, g_sh_scan_ns;   // the per-frame log line
static uint64_t g_sh_pend_syncs, g_sh_pend_bytes;   // [UIT 0029] syncs of pools with GPU writes not in game memory yet, bytes they copied in

static void shadow_barrier(VkCommandBuffer cb, VkPipelineStageFlags2 src, VkAccessFlags2 sa, VkPipelineStageFlags2 dst, VkAccessFlags2 da) {
    VkMemoryBarrier2 mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, .srcStageMask = src, .srcAccessMask = sa, .dstStageMask = dst, .dstAccessMask = da};
    VkDependencyInfo dep = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &mb};
    vk.vkCmdPipelineBarrier2(cb, &dep);
}
static void run_put(VkBuffer src, VkBuffer dst, VkDeviceSize soff, VkDeviceSize doff, VkDeviceSize n) {
    if (g_nruns == g_caruns) {
        g_caruns = g_caruns ? g_caruns * 2 : 256;
        if (!(g_runs = realloc(g_runs, sizeof *g_runs * (size_t)g_caruns))) die("pool copy runs");
    }
    if (!g_ngrp || g_grp[g_ngrp - 1].src != src || g_grp[g_ngrp - 1].dst != dst) {
        if (g_ngrp == g_capgrp) {
            g_capgrp = g_capgrp ? g_capgrp * 2 : 64;
            if (!(g_grp = realloc(g_grp, sizeof *g_grp * (size_t)g_capgrp))) die("pool copy groups");
        }
        g_grp[g_ngrp++] = (run_grp){src, dst, g_nruns, 0};
    }
    g_runs[g_nruns++] = (VkBufferCopy){soff, doff, n};
    g_grp[g_ngrp - 1].n++;
    g_sh_in_bytes += n;
}
static void run_written(void *ctx, size_t off, size_t n) {   // nvn_dirty_runs: off from the pool's start
    const pool_ent *p = ctx;
    VkDeviceSize o = (p->lo - p->base) + off;
    run_put(p->buf, p->sbuf, o, p->soff + o, n);
}
// [UIT native] TOTK_IWB_POOL_COPY=1 (vkb_iwbpc.h): game memory drawn images were copied back into, waiting for their
// pool's next sync. g_iwbpc_mu after g_pool_mu (the copy back and sync_gather both hold that).
#include "vkb_iwbpc.h"
static pthread_mutex_t g_iwbpc_mu = PTHREAD_MUTEX_INITIALIZER;
static iwbpc_list g_iwbpc = {.max = 65536};
static uint64_t g_iwbpc_bytes, g_iwbpc_lost;
int vkb_gpu_coherent(void);   // [UIT 0155] vkb_tex.c: TOTK_VK_GPU_MEM_COHERENT (default on) includes this
static int iwbpc_on(void) {
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("TOTK_IWB_POOL_COPY");
        on = (e && *e == '1') || vkb_gpu_coherent();
        if (on) VLOG("%s: game memory drawn images are copied back into also goes into the pool's device copy",
                     e && *e == '1' ? "TOTK_IWB_POOL_COPY=1" : "TOTK_VK_GPU_MEM_COHERENT (default on)");
    }
    return on;
}
static void iwbpc_run(void *ctx, uint64_t off, uint64_t n) {
    const pool_ent *p = ctx;
    VkDeviceSize o = (p->lo - p->base) + off;
    run_put(p->buf, p->sbuf, o, p->soff + o, n);
    g_iwbpc_bytes += n;
}
static int iwbpc_alive(void *ctx, uintptr_t lo, uint64_t len) {   // still in a pool with a device copy
    (void)ctx;
    const pool_ent *p = pool_find(lo, len);
    return p && p->sh && !p->no_shadow;
}
static void iwbpc_note(uintptr_t lo, uint64_t len) {   // vkb_iwb_land, g_pool_mu held (read), lo's pool live
    if (!iwbpc_alive(NULL, lo, len)) return;   // no device copy (yet: its first sync copies the whole pool)
    pthread_mutex_lock(&g_iwbpc_mu);
    if (!iwbpc_add(&g_iwbpc, lo, len)) {
        iwbpc_purge(&g_iwbpc, iwbpc_alive, NULL);
        if (!iwbpc_add(&g_iwbpc, lo, len) && g_iwbpc_lost++ < 4)
            VLOG("[iwb] TOTK_IWB_POOL_COPY: %d ranges wait for their pools already; %#llx+%llu not added (its pool copy may stay stale)",
                 g_iwbpc.n, (unsigned long long)lo, (unsigned long long)len);
    }
    pthread_mutex_unlock(&g_iwbpc_mu);
}
static void sync_flush(void) {   // the gathered copies, outside a pass (threads recording: in the prologue)
    if (!g_nruns) return;
    int mt = t_recording;
    if (mt) vkb_pro_begin();
    else pass_end_why(PE_SHADOW);
    VkCommandBuffer cb = vkb_cmd();
    vkb_checkpoint("pool copies in", NULL, NULL);
    shadow_barrier(cb, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
                   VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    for (int g = 0; g < g_ngrp; g++) vk.vkCmdCopyBuffer(cb, g_grp[g].src, g_grp[g].dst, (uint32_t)g_grp[g].n, &g_runs[g_grp[g].first]);
    shadow_barrier(cb, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                   VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT);
    g_sh_in += (uint64_t)g_nruns;
    g_nruns = g_ngrp = 0;
    if (mt) vkb_pro_end();
}
// Gathers the copies that bring p's copy up to date for submission sub (g_pool_mu held). 1: current once they are
// recorded (sync_flush); 0: p stays in game memory for now (a pass is open, copies are needed and may_end is 0) or
// for good (no_shadow).
static int sync_gather(pool_ent *p, uint64_t sub, int may_end) {
    if (p->no_shadow) return 0;
    // GPU writes of an earlier submission not yet in game memory (wb_merge): the copy is the only current one
    if (p->wb_sub && p->wb_sub <= g_fence && !mtl_wait(p->wb_sub, 0)) may_end = 1;
    if (!may_end && vkb_in_pass() && p->defer_pass == vkb_pass_serial()) return 0;   // already found written in this pass
    if (!p->sh) {
        if (!g_shadow_budget) {   // half of the largest device-local heap
            for (uint32_t h = 0; h < vkc.memory.memoryHeapCount; h++)
                if ((vkc.memory.memoryHeaps[h].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) && vkc.memory.memoryHeaps[h].size / 2 > g_shadow_budget)
                    g_shadow_budget = vkc.memory.memoryHeaps[h].size / 2;
            VLOG("copies of memory pools: up to %llu MiB of device memory", (unsigned long long)(g_shadow_budget >> 20));
        }
        const char *why = !nvn_dirty_tracked((const void *)p->lo, p->hi - p->lo) ? "its writes are not tracked"
                        : vkb_shadow_bytes() + p->bytes > g_shadow_budget ? "over the device memory budget"
                        : !(p->sh = vkb_shadow_alloc(p->bytes, &p->sbuf, &p->soff)) ? "no device memory" : NULL;
        if (why) {
            p->no_shadow = 1;
            pool_gen_bump();
            static int said;
            if (said++ < 16) VLOG("[temporary] memory pool %#llx+%#llx stays in game memory: %s", (unsigned long long)p->lo, (unsigned long long)(p->hi - p->lo), why);
            return 0;
        }
        p->filled = 0;
    }
    uint64_t t0 = runtime_now_ns();
    int first = g_nruns, grp = g_ngrp;
    uint64_t bytes0 = g_sh_in_bytes;
    int pend_in = 0;
    // [UIT 0025] the stamp as of the scan (taken with the pool's range locked), not nvn_dirty_now() after it: a write
    // another thread's scan found in between got a generation not above that, and was never copied
    uint64_t stamp;
    uint64_t iwb_seq = 0;   // TOTK_IWB_POOL_COPY: ranges taken up to this sequence (0: none to drop)
    int iwb = iwbpc_on();
    if (!p->filled) {
        if (iwb) { pthread_mutex_lock(&g_iwbpc_mu); iwb_seq = g_iwbpc.seq; pthread_mutex_unlock(&g_iwbpc_mu); }   // before the copy: all in it
        nvn_dirty_since((const void *)p->lo, p->hi - p->lo, UINT64_MAX);   // writes so far are in the whole copy
        stamp = nvn_dirty_last_stamp();
        run_put(p->buf, p->sbuf, 0, p->soff, p->bytes);
    } else {
        // [UIT 0029] GPU writes of a submission not retired yet are only in the copy: only the CPU-written pages are
        // copied then, not the pages between them (vkb_shadow_gap.h; tools/tests/test_shadow_pending.c)
        static int gap_old = -1;
        if (gap_old < 0) { const char *e = getenv("TOTK_VK_POOL_GAP_OLD"); gap_old = e && *e == '1'; if (gap_old) VLOG("[development] TOTK_VK_POOL_GAP_OLD=1: pool copies merge runs 64 KiB apart even over GPU writes not in game memory yet (0028)"); }
        int pend = p->wb_sub && (p->wb_sub > g_fence || !mtl_wait(p->wb_sub, 0));
        if (nvn_dirty_runs_stamp((const void *)p->lo, p->hi - p->lo, p->stamp, vkb_shadow_gap(pend, gap_old), run_written, p, &stamp) < 0) {
            p->no_shadow = 1;   // its writes can no longer be told
            vkb_shadow_free(p->sh); p->sh = 0;
            pool_gen_bump();
            return 0;
        }
        pend_in = pend && g_sh_in_bytes > bytes0;
        // TOTK_IWB_POOL_COPY: copied-back image bytes are GPU writes to game memory, never in the runs above. Not while
        // the copy has GPU writes not in game memory yet (they could be under them): the ranges wait for a later sync.
        if (iwb && !pend) {
            pthread_mutex_lock(&g_iwbpc_mu);
            if (g_iwbpc.n) iwb_seq = iwbpc_take(&g_iwbpc, p->lo, p->hi, iwbpc_run, p);
            pthread_mutex_unlock(&g_iwbpc_mu);
        }
    }
    g_sh_scan_ns += runtime_now_ns() - t0;
    if (g_nruns > first && vkb_in_pass() && !may_end) {   // copies need the pass to end: game memory for the rest of it
        g_sh_in_bytes = bytes0;
        g_nruns = first; g_ngrp = grp;
        if (grp) g_grp[grp - 1].n = first - g_grp[grp - 1].first;
        p->defer_pass = vkb_pass_serial();
        g_sh_deferred++;
        return 0;
    }
    if (pend_in) { p->pend_sub = sub; g_sh_pend_syncs++; g_sh_pend_bytes += g_sh_in_bytes - bytes0; }
    if (iwb_seq) { pthread_mutex_lock(&g_iwbpc_mu); iwbpc_drop(&g_iwbpc, p->lo, p->hi, iwb_seq); pthread_mutex_unlock(&g_iwbpc_mu); }
    p->filled = 1;
    p->stamp = stamp;
    p->sync_sub = sub;
    return 1;
}
static void wb_add(pool_ent *p, uint64_t addr, uint64_t len) {   // a GPU write to p's copy
    p->wb_sub = g_fence + 1;
    VkDeviceSize o = addr - p->base;
    if (g_nwb && g_wb[g_nwb - 1].dst == p->buf && o <= g_wb[g_nwb - 1].doff + g_wb[g_nwb - 1].n && g_wb[g_nwb - 1].doff <= o + len) {
        wb_t *w = &g_wb[g_nwb - 1];   // overlaps or touches the last one: merged
        VkDeviceSize lo = w->doff < o ? w->doff : o, hi = w->doff + w->n > o + len ? w->doff + w->n : o + len;
        w->soff -= w->doff - lo; w->doff = lo; w->n = hi - lo;
        return;
    }
    if (g_nwb == g_capwb) {
        g_capwb = g_capwb ? g_capwb * 2 : 256;
        if (!(g_wb = realloc(g_wb, sizeof *g_wb * (size_t)g_capwb))) die("pool copies back");
    }
    g_wb[g_nwb++] = (wb_t){p->sbuf, p->buf, p->soff + o, o, len, p->base, p->stamp};
}
static void wb_forget(VkBuffer host) {   // a pool deleted (g_pool_mu held): its writes have nowhere to go
    for (int k = 0; k < g_nwb;) if (g_wb[k].dst == host) g_wb[k] = g_wb[--g_nwb]; else k++;
}
static int wb_cmp(const void *a, const void *b) {
    const wb_t *x = a, *y = b;
    if (x->dst != y->dst) return (uintptr_t)x->dst < (uintptr_t)y->dst ? -1 : 1;
    return x->doff < y->doff ? -1 : x->doff > y->doff;
}
// The copy back goes through host memory of the submission's slot (wbst_t): the GPU copies the written ranges there,
// and when the submission has completed the retire thread copies them into game memory, page by page, except pages
// the CPU wrote since the copy was brought up to date (wb_merge). Copied straight into game memory, the whole range
// overwrote what the game's CPU wrote there meanwhile: about 45% of the ranges on the Great Sky Island (2026-10-24).
// The game's own writes are newer than the GPU's there (it writes a buffer for its next use after the GPU's), so the
// CPU's pages win. A slot is reused only after its submission has retired, so its host memory is free by then.
typedef struct { VkBuffer buf; VkDeviceMemory mem; uint8_t *map; VkDeviceSize cap; } wbst_t;
static wbst_t g_wbst[VKB_SLOTS];
static uint64_t *g_wb_ranges;   // per range of the last shadow_writeback: guest address, bytes, stamp, offset in the slot's memory
static int g_wb_nranges;
static uint64_t g_wb_kept_pages, g_wb_pages;   // pages left to the CPU's writes / all pages copied back (the log line)
static uint64_t g_wb_lost_bytes;   // [UIT 0029] in those pages, bytes where the GPU's result and game memory differ

// [UIT 0029] TOTK_VK_SHADOW_VERIFY=N (diagnostic): every N presents, at the end of a submission, up to
// TOTK_VK_SHADOW_VERIFY_MIB (default 64) of the pools' device copies (in turn, all of them over a few checks) are
// copied into the slot's host memory too; the retire thread compares them with game memory after that submission's
// GPU writes are in (wb_merge). A page that differs and was not written by the CPU since the copy's stamp is stale: it
// is logged ([shadow verify]) with what touched it - GPU-written in that submission, the pool's last sync over GPU
// writes not in game memory yet - and counted in the per-frame line. Off: nothing changes.
#define VF_W 8   // per range: guest address, bytes, stamp, offset in the slot's memory, pool lo, pool hi, pend_sub, sync_sub
static int g_vf_every = -1;
static VkDeviceSize g_vf_budget;
static uint64_t g_vf_last;
static uintptr_t g_vf_at;   // where the next check starts (a guest address: pools come and go)
static uint64_t *g_vf_ranges;
static int g_vf_nranges;
static uint64_t g_vf_pages, g_vf_bad, g_vf_checks;
static uint8_t *wbst_get_in(wbst_t *s, VkDeviceSize need, VkBuffer *buf);
static uint8_t *wbst_get(int slot, VkDeviceSize need, VkBuffer *buf) { return wbst_get_in(&g_wbst[slot], need, buf); }
static uint8_t *wbst_get_in(wbst_t *s, VkDeviceSize need, VkBuffer *buf) {   // the slot's host memory, at least need bytes
    if (s->cap < need) {
        if (s->buf) { vk.vkDestroyBuffer(vkc.device, s->buf, NULL); vk.vkFreeMemory(vkc.device, s->mem, NULL); }
        *s = (wbst_t){0};
        VkDeviceSize cap = 1u << 20;
        while (cap < need) cap *= 2;
        VkBufferCreateInfo bi = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = cap, .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT};
        if (!vk_check(vk.vkCreateBuffer(vkc.device, &bi, NULL, &s->buf), "copy back buffer")) die("copy back buffer");
        VkMemoryRequirements req;
        vk.vkGetBufferMemoryRequirements(vkc.device, s->buf, &req);
        uint32_t type = UINT32_MAX, want = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        for (int pass = 0; pass < 2 && type == UINT32_MAX; pass++)   // cached first: the CPU reads it
            for (uint32_t i = 0; i < vkc.memory.memoryTypeCount && type == UINT32_MAX; i++) {
                VkMemoryPropertyFlags f = vkc.memory.memoryTypes[i].propertyFlags;
                if ((req.memoryTypeBits & (1u << i)) && (f & want) == want && (pass || (f & VK_MEMORY_PROPERTY_HOST_CACHED_BIT))) type = i;
            }
        VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = req.size, .memoryTypeIndex = type};
        if (type == UINT32_MAX || !vk_check(vk.vkAllocateMemory(vkc.device, &ai, NULL, &s->mem), "copy back memory")) die("copy back memory");
        vk.vkBindBufferMemory(vkc.device, s->buf, s->mem, 0);
        void *p;
        if (!vk_check(vk.vkMapMemory(vkc.device, s->mem, 0, VK_WHOLE_SIZE, 0, &p), "copy back map")) die("copy back map");
        s->map = p; s->cap = cap;
    }
    *buf = s->buf;
    return s->map;
}
// Retire thread, the submission complete: its copied-back ranges into game memory, but for pages the CPU wrote since.
// These writes are the GPU's, not the CPU's (nvn_dirty_gpu_write): counted as the CPU's, a later submission's copy
// back to the same range would find them newer than its copy and keep them in place of its own GPU writes.
// 0008: the pages whose bytes change are noted as GPU-written (nvn_gpu_write_mark): textures uploaded from them upload
// again. Pages written with what they held already (the same result every frame) are not: no upload for nothing.
// [UIT 0028] through nvn_dirty_gpu_write, page by page: a CPU write elsewhere in a page while it is written is no
// longer lost to the write watch (stale vertices and textures after streaming, see nvn_dirty_win.c).
static void wb_put(uint64_t a, const uint8_t *src, uint64_t n) {
    for (uint64_t o = 0; o < n;) {
        uint64_t c = 4096 - ((a + o) & 4095);
        if (c > n - o) c = n - o;
        uint8_t *d = (uint8_t *)(uintptr_t)(a + o);
        if (nvn_dirty_gpu_write(d, src + o, c)) nvn_gpu_write_mark(d, c);
        o += c;
    }
}
// [UIT 0035] TOTK_VK_WB_MERGE3 (vkb_wb3.h; on by default since UIT 0139, =0 off): at the end of a submission, game memory of the ranges the GPU writes back
// is snapshotted (the base) where the CPU has not written since the copy's stamp; the retire thread then merges kept
// pages word by word instead of leaving them to the CPU whole. Up to 32 MiB per submission; ranges past that merge
// as before.
typedef struct { uint8_t *data; uint64_t *off; uint8_t *ok; uint64_t *pg; } wb3_snap;   // per range: offset in data (UINT64_MAX: none), first page in ok
static wb3_snap *g_wb_m3;
static int g_m3_on = -1;
static uint64_t g_wb_m3_bytes, g_wb_m3_pages, g_m3_snap_ns, g_m3_snap_bytes;
static int m3_on(void) {
    if (g_m3_on < 0) {
        const char *e = getenv("TOTK_VK_WB_MERGE3");
        // [UIT 0139] on unless =0. Without it every frame of the 2026-10-31 field runs dropped 600-1,400 bytes of GPU
        // results in pages the CPU also wrote (the per-frame "GPU bytes not copied back there"), in memory the game reads
        // back each frame; a GPU result the game never sees is the one data loss the logs show while the ground floats.
        g_m3_on = !(e && *e == '0');
        if (g_m3_on) VLOG("TOTK_VK_WB_MERGE3 (on by default since UIT 0139): pages both the CPU and the GPU wrote are merged word by word");
        else VLOG("TOTK_VK_WB_MERGE3=0: pages both the CPU and the GPU wrote are left to the CPU (the GPU's bytes there are lost)");
    }
    return g_m3_on;
}
static void m3_snapshot(const uint64_t *ranges, int n) {   // the translation thread, g_pool_mu held
    g_wb_m3 = NULL;
    if (!m3_on() || !n) return;
    uint64_t t0 = runtime_now_ns(), bytes = 0, pages = 0;
    for (int k = 0; k < n; k++) pages += ((ranges[4 * k] + ranges[4 * k + 1] + 4095) >> 12) - (ranges[4 * k] >> 12);
    wb3_snap *m = calloc(1, sizeof *m);
    if (!m || !(m->off = malloc(sizeof *m->off * (size_t)n)) || !(m->pg = malloc(sizeof *m->pg * (size_t)n)) || !(m->ok = calloc(1, (size_t)pages + 1))) {
        if (m) { free(m->off); free(m->pg); free(m); }
        return;
    }
    const uint64_t cap = 32ull << 20;
    uint64_t want = 0;
    for (int k = 0; k < n; k++) want += ranges[4 * k + 1] < cap ? ranges[4 * k + 1] : 0;
    m->data = malloc((size_t)(want < cap ? want : cap) + 1);
    uint64_t pg = 0;
    for (int k = 0; k < n; k++) {
        uint64_t a = ranges[4 * k], len = ranges[4 * k + 1], stamp = ranges[4 * k + 2];
        m->pg[k] = pg;
        uint64_t np = ((a + len + 4095) >> 12) - (a >> 12);
        m->off[k] = UINT64_MAX;
        if (m->data && bytes + len <= cap && nvn_dirty_tracked((const void *)(uintptr_t)a, len)) {
            memcpy(m->data + bytes, (const void *)(uintptr_t)a, len);   // then which pages were not written since the stamp
            nvn_dirty_since((const void *)(uintptr_t)a, len, stamp);   // (a scan: writes during the copy count)
            for (uint64_t i = 0; i < np; i++) {
                uint64_t lo = i ? ((a >> 12) + i) << 12 : a, hi = ((a >> 12) + i + 1) << 12;
                if (hi > a + len) hi = a + len;
                m->ok[pg + i] = !nvn_dirty_written_after((const void *)(uintptr_t)lo, hi - lo, stamp);
            }
            m->off[k] = bytes;
            bytes += len;
        }
        pg += np;
    }
    g_m3_snap_bytes += bytes;
    g_m3_snap_ns += runtime_now_ns() - t0;
    g_wb_m3 = m;
}
static void m3_free(wb3_snap *m) { if (m) { free(m->data); free(m->off); free(m->ok); free(m->pg); free(m); } }
typedef struct { uint8_t *page; const uint8_t *gpu; } m3_ctx;
static void m3_put(void *c, size_t off, size_t len) {   // the GPU's words into game memory, as wb_put does
    m3_ctx *x = c;
    if (nvn_dirty_gpu_write(x->page + off, x->gpu + off, len)) nvn_gpu_write_mark(x->page + off, len);
}
static void wb_merge(const uint64_t *r, int n, const uint8_t *src, const wb3_snap *m3) {
    const uint64_t *r0 = r;
    for (int k = 0; k < n; k++, r += 4) {
        uint64_t a = r[0], len = r[1], stamp = r[2];
        if (!nvn_dirty_since((const void *)(uintptr_t)a, len, stamp)) { wb_put(a, src + r[3], len); g_wb_pages += (len + 4095) / 4096; continue; }
        for (uint64_t p = a; p < a + len;) {   // page by page
            uint64_t e = ((p >> 12) + 1) << 12;
            if (e > a + len) e = a + len;
            g_wb_pages++;
            if (nvn_dirty_since((const void *)(uintptr_t)p, e - p, stamp)) {
                g_wb_kept_pages++;
                const uint8_t *g = src + r[3] + (p - a), *m = (const uint8_t *)(uintptr_t)p;
                int ri = (int)((r - r0) / 4);
                uint64_t pi = (p >> 12) - (a >> 12);
                if (m3 && m3->off[ri] != UINT64_MAX && m3->ok[m3->pg[ri] + pi]) {   // [UIT 0035] word by word
                    m3_ctx c = {(uint8_t *)(uintptr_t)p, g};
                    size_t got = wb3_merge(m, g, m3->data + m3->off[ri] + (p - a), e - p, m3_put, &c);
                    g_wb_m3_bytes += got;
                    g_wb_m3_pages += got > 0;
                }
                // [UIT 0029] the GPU's bytes there that game memory does not have (lost, unless the CPU's are newer)
                for (uint64_t b = 0; b < e - p; b++) g_wb_lost_bytes += g[b] != m[b];
            } else wb_put(p, src + r[3] + (p - a), e - p);
            p = e;
        }
    }
}
static void vf_gather(VkDeviceSize *total) {   // TOTK_VK_SHADOW_VERIFY: the device copies checked with this submission
    g_vf_ranges = NULL; g_vf_nranges = 0;
    if (g_vf_every < 0) {
        const char *e = getenv("TOTK_VK_SHADOW_VERIFY"), *m = getenv("TOTK_VK_SHADOW_VERIFY_MIB");
        g_vf_every = e ? atoi(e) : 0;
        g_vf_budget = (VkDeviceSize)(m && atoi(m) > 0 ? atoi(m) : 64) << 20;
        if (g_vf_every > 0) VLOG("[diagnostic] TOTK_VK_SHADOW_VERIFY=%d: every %d presents %llu MiB of the pool copies are compared with game memory ([shadow verify] lines)",
                                 g_vf_every, g_vf_every, (unsigned long long)(g_vf_budget >> 20));
    }
    if (g_vf_every <= 0 || vkb_frame() < g_vf_last + (uint64_t)g_vf_every || !g_npools) return;
    g_vf_last = vkb_frame();
    int cap = 64;
    if (!(g_vf_ranges = malloc(sizeof *g_vf_ranges * VF_W * (size_t)cap))) return;
    VkDeviceSize left = g_vf_budget;
    int i = 0;
    while (i < g_npools && g_pools[i].hi <= g_vf_at) i++;
    for (int seen = 0; seen < g_npools && left; seen++, i++) {
        if (i >= g_npools) { i = 0; g_vf_at = 0; }
        pool_ent *p = &g_pools[i];
        if (!p->sh || !p->filled || p->no_shadow) continue;
        uintptr_t lo = g_vf_at > p->lo && g_vf_at < p->hi ? g_vf_at : p->lo, hi = p->hi;
        if (hi - lo > left) hi = lo + left;
        if (g_vf_nranges == cap && !(g_vf_ranges = realloc(g_vf_ranges, sizeof *g_vf_ranges * VF_W * (size_t)(cap *= 2)))) die("shadow verify");
        uint64_t *r = &g_vf_ranges[VF_W * g_vf_nranges++];
        r[0] = lo; r[1] = hi - lo; r[2] = p->stamp; r[3] = *total; r[4] = p->lo; r[5] = p->hi; r[6] = p->pend_sub; r[7] = p->sync_sub;
        *total += ((hi - lo) + 15) & ~(VkDeviceSize)15;
        left -= hi - lo;
        g_vf_at = hi;
        if (hi < p->hi) break;   // the rest of this pool next time
    }
    g_vf_checks++;
}
static void vf_copy(VkCommandBuffer cb, VkBuffer st) {   // the copies to check, into the slot's host memory
    for (int k = 0; k < g_vf_nranges; k++) {
        uint64_t *r = &g_vf_ranges[VF_W * k];
        const pool_ent *p = pool_find((uintptr_t)r[4], r[5] - r[4]);
        if (!p || !p->sh) { r[1] = 0; continue; }
        VkBufferCopy c = {p->soff + (r[0] - p->base), r[3], r[1]};
        vk.vkCmdCopyBuffer(cb, p->sbuf, st, 1, &c);
    }
}
// Retire thread, after wb_merge: the checked copies against game memory (TOTK_VK_SHADOW_VERIFY).
static void vf_check(uint64_t fence, const uint64_t *vf, int nvf, const uint64_t *wb, int nwb, const uint8_t *map) {
    static int said;
    uint64_t bad0 = g_vf_bad;
    for (int k = 0; k < nvf; k++) {
        const uint64_t *r = &vf[VF_W * k];
        for (uint64_t a = r[0]; a < r[0] + r[1];) {
            uint64_t e = ((a >> 12) + 1) << 12;
            if (e > r[0] + r[1]) e = r[0] + r[1];
            const uint8_t *c = map + r[3] + (a - r[0]), *m = (const uint8_t *)(uintptr_t)a;
            g_vf_pages++;
            if (memcmp(c, m, e - a) && !nvn_dirty_since((const void *)(uintptr_t)a, e - a, r[2]) && memcmp(c, m, e - a)) {
                g_vf_bad++;
                if (said < 64) {
                    said++;
                    uint64_t nb = 0, first = UINT64_MAX;
                    for (uint64_t b = 0; b < e - a; b++) if (c[b] != m[b]) { nb++; if (first == UINT64_MAX) first = b; }
                    uint64_t cv = 0, mv = 0;
                    uint64_t f8 = first & ~(uint64_t)7;
                    memcpy(&cv, c + f8, e - a - f8 < 8 ? e - a - f8 : 8); memcpy(&mv, m + f8, e - a - f8 < 8 ? e - a - f8 : 8);
                    int gw = 0;
                    for (int w = 0; w < nwb && !gw; w++) gw = a < wb[4 * w] + wb[4 * w + 1] && wb[4 * w] < e;
                    VLOG("[shadow verify] submission %llu: stale page %#llx in pool %#llx..%#llx: %llu bytes differ (first at +%#llx: copy %016llx, memory %016llx); "
                         "GPU-written in this submission: %s; pool last synced in submission %llu, last sync over GPU writes not in game memory yet: %llu",
                         (unsigned long long)fence, (unsigned long long)a, (unsigned long long)r[4], (unsigned long long)r[5], (unsigned long long)nb,
                         (unsigned long long)(a - (a & ~(uint64_t)0xfff) + first), (unsigned long long)cv, (unsigned long long)mv, gw ? "yes" : "no",
                         (unsigned long long)r[7], (unsigned long long)r[6]);
                }
            }
            a = e;
        }
    }
    if (g_vf_bad > bad0 && said >= 64) { static int s2; if (s2++ < 8) VLOG("[shadow verify] submission %llu: %llu stale pages (all: %llu)", (unsigned long long)fence, (unsigned long long)(g_vf_bad - bad0), (unsigned long long)g_vf_bad); }
}
static void shadow_writeback(VkCommandBuffer cb) {   // end of a submission: GPU writes to the slot's host memory
    vkb_rw_write(&g_pool_mu, LK_POOLS);
    g_wb_ranges = NULL; g_wb_nranges = 0;
    m3_free(g_wb_m3); g_wb_m3 = NULL;   // not taken by a job (none before): nothing to merge with
    VkDeviceSize total = 0;
    if (g_nwb) {
        qsort(g_wb, (size_t)g_nwb, sizeof *g_wb, wb_cmp);
        int m = 0;   // overlapping ranges of one pool merged
        for (int k = 1; k < g_nwb; k++) {
            wb_t *w = &g_wb[m];
            if (g_wb[k].dst == w->dst && g_wb[k].doff <= w->doff + w->n) {
                VkDeviceSize hi = g_wb[k].doff + g_wb[k].n;
                if (hi > w->doff + w->n) w->n = hi - w->doff;
                if (g_wb[k].stamp < w->stamp) w->stamp = g_wb[k].stamp;
            } else g_wb[++m] = g_wb[k];
        }
        g_nwb = m + 1;
        g_wb_ranges = malloc(sizeof *g_wb_ranges * 4 * (size_t)g_nwb);
        if (!g_wb_ranges) die("copy back ranges");
        for (int k = 0; k < g_nwb; k++) {
            uint64_t *r = &g_wb_ranges[4 * k];
            r[0] = g_wb[k].base + g_wb[k].doff; r[1] = g_wb[k].n; r[2] = g_wb[k].stamp; r[3] = total;
            total += (g_wb[k].n + 15) & ~(VkDeviceSize)15;
        }
        g_wb_nranges = g_nwb;
    }
    m3_snapshot(g_wb_ranges, g_wb_nranges);   // [UIT 0035] TOTK_VK_WB_MERGE3
    vf_gather(&total);
    if (g_nwb || g_vf_nranges) {
        VkBuffer st;
        wbst_get(vkb_slot(), total, &st);
        vkb_checkpoint("pool copies out", NULL, NULL);
        shadow_barrier(cb, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
        for (int k = 0; k < g_nwb;) {
            int e = k;
            VkBufferCopy r[64];
            int n = 0;
            while (e < g_nwb && g_wb[e].src == g_wb[k].src && n < 64) {
                r[n++] = (VkBufferCopy){g_wb[e].soff, g_wb_ranges[4 * e + 3], g_wb[e].n};
                g_sh_out_bytes += g_wb[e].n;
                e++;
            }
            vk.vkCmdCopyBuffer(cb, g_wb[k].src, st, (uint32_t)n, r);
            k = e;
        }
        vf_copy(cb, st);
        shadow_barrier(cb, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT | VK_PIPELINE_STAGE_2_HOST_BIT,
                       VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT | VK_ACCESS_2_HOST_READ_BIT);
        g_nwb = 0;
    }
    vkb_rw_write_end(&g_pool_mu);
}
// The first pass of a submission: the pools used in the two before are brought up to date together (one pair of
// barriers), so that draws in passes find them current.
// [UIT 0034] TOTK_VK_PRESYNC_SUBS=N (1-16, default 2): "the two before" is N. Larger brings more pools up to date there
// (once, all together) instead of when a draw first uses them (a draw-path sync: the exclusive lock, and with threads
// recording a prologue copy); the per-frame line "pools brought up to date" shows both counts.
static uint64_t g_sh_early, g_sh_late;
static int presync_subs(void) {
    static int n;
    if (!n) {
        const char *e = getenv("TOTK_VK_PRESYNC_SUBS");
        int v = e && *e && *e != '-' ? atoi(e) : 2;
        n = v < 1 ? 1 : v > 16 ? 16 : v;
        if (n != 2) VLOG("TOTK_VK_PRESYNC_SUBS=%d: a submission's first pass brings up to date the pools used in the %d before", n, n);
    }
    return n;
}
static void presync_prescan(uint64_t sub, uint64_t win, const uintptr_t *only, int nonly);   // [UIT 0249] (with the recording threads, below)
static __thread int t_ps_lead;   // [UIT 0249] vkb_mt_submit's prologue on the translation thread
static _Atomic uint64_t g_ps_jobs, g_ps_items, g_ps_scanned, g_ps_helped, g_ps_ns;
static void shadow_presync(void) {
    uint64_t sub = g_fence + 1;
    uint64_t win = (uint64_t)presync_subs();
    if (!vkb_shadow_on() || __atomic_load_n(&g_presync_sub, __ATOMIC_ACQUIRE) == sub) return;
    vkb_rw_write(&g_pool_mu, LK_POOLS);
    if (g_presync_sub == sub) { vkb_rw_write_end(&g_pool_mu); return; }   // another thread did it meanwhile
    __atomic_store_n(&g_presync_sub, sub, __ATOMIC_RELEASE);
    if (t_ps_lead) presync_prescan(sub, win, NULL, 0);   // [UIT 0249] the same pools' write-watch scans first, on all recording threads
    for (int i = 0; i < g_npools; i++)
        if (g_pools[i].sh && g_pools[i].sync_sub != sub && g_pools[i].used_sub + win >= sub) { sync_gather(&g_pools[i], sub, 1); g_sh_early++; }
    sync_flush();
    vkb_rw_write_end(&g_pool_mu);
}
static pool_ent *pool_find_mut(uintptr_t addr, uint64_t len);
// [UIT 0258] TOTK_VK_PRESYNC_BY_CHUNK (vkb_presync_chunk.h): vkb_mt_submit's prologue brings up to date only the pools
// its first chunk used last time (and those with no stamp); the walk the others, before the chunk that used them first.
#include "vkb_presync_chunk.h"
static int g_pbc_on = -1, g_pbc_lead = VKB_PBC_LEAD;
static __thread int t_mt_ch = -1;   // the chunk this thread records (mt_record), or -1
static int pbc_on(void) {   // first read by the translation thread before any chunk is handed out
    if (g_pbc_on < 0) {
        g_pbc_on = vkb_pbc_parse(getenv("TOTK_VK_PRESYNC_BY_CHUNK"));
        const char *l = getenv("TOTK_VK_PRESYNC_BY_CHUNK_LEAD");
        if (l && *l >= '0' && *l <= '9') g_pbc_lead = atoi(l) > 32 ? 32 : atoi(l);
        if (g_pbc_on) VLOG("[UIT 0258] TOTK_VK_PRESYNC_BY_CHUNK: a submission's prologue brings up to date only the memory pools its first "
                           "chunks used last time (and pools never used by a chunk); the walk brings the others up to date %d chunks before "
                           "the one that used them first, and the first parts may go to the GPU while the walk runs (=1; the default, off: "
                           "all in the prologue)", g_pbc_lead);
        else VLOG("[UIT 0258] TOTK_VK_PRESYNC_BY_CHUNK off (the default since UIT 0312; 1 = on): every pool is brought up to date in the prologue, before any chunk");
    }
    return g_pbc_on;
}
static inline void pbc_note(pool_ent *p, uint64_t sub) {   // g_pool_mu held (read or write)
    if (t_mt_ch >= 0 && g_pbc_on > 0) vkb_pbc_stamp(&p->fc, sub, t_mt_ch);
}
static int presync_due(const pool_ent *p, uint64_t sub, uint64_t win) {   // shadow_presync's test
    return p->sh && p->sync_sub != sub && p->used_sub + win >= sub;
}
static vkb_pbc_plan g_pbc;   // the translation thread's
static uintptr_t *g_pbc_tmp_a; static int *g_pbc_tmp_c, g_pbc_cap;
static int g_pbc_live, g_pbc_walk_open;   // this submission's plan is being worked through / the stream gate opened before the walk
static uint64_t g_pbc_sub, g_pbc_win;
static uint64_t g_pbc_cnt[3], g_pbc_ns[3], g_pbc_subs;   // pools brought up to date: prologue, walk, after the walk (the log line)
static int pbc_gather(int a, int b) {   // g_pool_mu written
    int n = 0;
    for (int k = a; k < b; k++) {
        pool_ent *p = pool_find_mut(g_pbc.addr[k], 1);
        if (p && p->lo == g_pbc.addr[k] && presync_due(p, g_pbc_sub, g_pbc_win)) { sync_gather(p, g_pbc_sub, 1); g_sh_early++; n++; }
    }
    return n;
}
// The prologue's part (t_recording and t_ps_lead set): the plan, then the pools wanted by chunk 0, as shadow_presync.
static void pbc_presync(void) {
    uint64_t sub = g_fence + 1, win = (uint64_t)presync_subs();
    g_pbc_live = 0;
    if (!vkb_shadow_on() || __atomic_load_n(&g_presync_sub, __ATOMIC_ACQUIRE) == sub) return;
    uint64_t t0 = runtime_now_ns();
    vkb_rw_write(&g_pool_mu, LK_POOLS);
    if (g_presync_sub == sub) { vkb_rw_write_end(&g_pool_mu); return; }
    __atomic_store_n(&g_presync_sub, sub, __ATOMIC_RELEASE);   // pass_begin's shadow_presync: nothing more (draws sync the rest)
    if (g_npools > g_pbc_cap) {
        int cap = g_npools * 2;
        uintptr_t *a = realloc(g_pbc_tmp_a, (size_t)cap * sizeof *a);
        if (a) g_pbc_tmp_a = a;
        uintptr_t *pa = a ? realloc(g_pbc.addr, (size_t)cap * sizeof *pa) : NULL;
        if (pa) g_pbc.addr = pa;
        int *c = pa ? realloc(g_pbc_tmp_c, (size_t)cap * sizeof *c) : NULL;
        if (c) g_pbc_tmp_c = c;
        int *pc = c ? realloc(g_pbc.ch, (size_t)cap * sizeof *pc) : NULL;
        if (pc) g_pbc.ch = pc;
        if (!pc) die("presync plan");
        g_pbc_cap = g_pbc.cap = cap;
    }
    int n = 0, n0 = 0;
    for (int i = 0; i < g_npools; i++)
        if (presync_due(&g_pools[i], sub, win)) {
            g_pbc_tmp_a[n] = g_pools[i].lo;
            g_pbc_tmp_c[n] = vkb_pbc_predict(g_pools[i].fc, sub, win, g_pbc_lead);
            n0 += !g_pbc_tmp_c[n];
            n++;
        }
    vkb_pbc_order(&g_pbc, g_pbc_tmp_a, g_pbc_tmp_c, n);
    g_pbc_sub = sub; g_pbc_win = win;
    if (t_ps_lead) presync_prescan(sub, win, g_pbc.addr, n0);   // [UIT 0249] chunk 0's pools' scans on all recording threads
    int a, b, k = 0;
    if (vkb_pbc_take(&g_pbc, 0, &a, &b)) k = pbc_gather(a, b);
    sync_flush();
    vkb_rw_write_end(&g_pool_mu);
    g_pbc_live = 1;
    g_pbc_subs++; g_pbc_cnt[0] += (uint64_t)k; g_pbc_ns[0] += runtime_now_ns() - t0;
}
// The walk's part, before chunk `upto` is handed out (ph 1), or the rest at the walk's end (ph 2): translation thread.
static void pbc_sync(int upto, int ph) {
    int a, b;
    if (!g_pbc_live || !vkb_pbc_take(&g_pbc, upto, &a, &b)) return;
    uint64_t t0 = runtime_now_ns();
    int tr = t_recording;
    t_recording = 1;   // the copies go into the open prologue segment: sent before every chunk handed out after this
    vkb_rw_write(&g_pool_mu, LK_POOLS);
    int k = pbc_gather(a, b);
    sync_flush();
    vkb_rw_write_end(&g_pool_mu);
    t_recording = tr;
    g_pbc_cnt[ph] += (uint64_t)k; g_pbc_ns[ph] += runtime_now_ns() - t0;
}
// [UIT 0033] TOTK_VK_POOL_FASTPATH: lookups of pools already current in this submission skip g_pool_mu
// (vkb_pool_tcache.h; tools/tests/test_pool_tcache.c). [UIT 0040] On by default; 0: every lookup takes the lock, as before.
static _Atomic int g_ptc_on = -1;
static __thread ptc_t t_ptc;
#define PTC_SLOTS 64
static struct { _Alignas(64) uint64_t hits, misses; } g_ptc_n[PTC_SLOTS];   // per worker (slot 0 is shared, so atomic adds): the log line
static int ptc_on(void) {
    if (g_ptc_on < 0) {
        const char *e = getenv("TOTK_VK_POOL_FASTPATH");
        g_ptc_on = !(e && *e == '0');
        if (g_ptc_on) VLOG("TOTK_VK_POOL_FASTPATH: pool lookups already current in a submission skip the pool table's lock (the default; 0 turns it off)");
        else VLOG("TOTK_VK_POOL_FASTPATH=0: every pool lookup takes the pool table's lock");
    }
    return g_ptc_on;
}
static void ptc_count(int hit) {
    int w = t_worker > 0 && t_worker < PTC_SLOTS ? t_worker : 0;   // the translation and game threads share slot 0
    __atomic_fetch_add(hit ? &g_ptc_n[w].hits : &g_ptc_n[w].misses, 1, __ATOMIC_RELAXED);
}
static void ptc_note(const pool_ent *p, uint64_t sub, uint64_t gen) {   // g_pool_mu held: p is done for sub
    int copy = p->sh && !p->no_shadow && p->sync_sub == sub;
    if (!copy && !p->no_shadow) return;
    ptc_read e = {p->lo, p->hi, sub, gen, p->base, p->soff, (void *)p->sbuf, copy, p->no_shadow};
    ptc_put(&t_ptc, &e);
}
void vkb_pool_use(uint64_t addr, uint64_t len, int write) {
    if (g_ok != 1 || !addr || !len || !vkb_shadow_on()) return;
    uint64_t sub = g_fence + 1, gen = 0;
    int fp = ptc_on();
    if (fp) {
        gen = atomic_load_explicit(&g_pool_gen, memory_order_acquire);   // before the lock: a change after it misses later
        const ptc_read *e = ptc_find(&t_ptc, addr, len, sub, gen);
        if (e && (!write || e->ns || ptc_wfind(&t_ptc, addr, len, sub, gen))) { ptc_count(1); return; }
        ptc_count(0);
    }
    if (!write) {   // the common case: a read of a pool already current in this submission
        vkb_rw_read(&g_pool_mu, LK_POOLS);
        pool_ent *p = pool_find_mut((uintptr_t)addr, len);
        int done = !p || p->no_shadow || p->sync_sub == sub;
        if (p) { __atomic_store_n(&p->used_sub, sub, __ATOMIC_RELAXED); pbc_note(p, sub); }
        if (fp && p && done) ptc_note(p, sub, gen);
        vkb_rw_read_end(&g_pool_mu);
        if (done) return;
    }
    vkb_rw_write(&g_pool_mu, LK_POOLS);
    pool_ent *p = pool_find_mut((uintptr_t)addr, len);
    if (p) {
        p->used_sub = sub;
        pbc_note(p, sub);
        // threads recording: the copies go into the prologue, before every chunk (no pass to end)
        if (p->sh && p->sync_sub != sub) g_sh_late++;   // [UIT 0034] a sync on the draw path (the per-frame line)
        int ok = p->sync_sub == sub || sync_gather(p, sub, write || t_recording);
        sync_flush();
        if (ok && write) wb_add(p, addr, len);
        if (fp && (ok || p->no_shadow)) {
            ptc_note(p, sub, gen);
            if (ok && write) ptc_wput(&t_ptc, addr, len, sub, gen);
        }
    }
    vkb_rw_write_end(&g_pool_mu);
}
int vkb_pool_addr(uint64_t addr, uint64_t len, VkBuffer *buf, VkDeviceSize *off) {
    if (g_ok != 1) return 0;
    if (ptc_on()) {
        const ptc_read *e = ptc_find(&t_ptc, addr, len, g_fence + 1, atomic_load_explicit(&g_pool_gen, memory_order_acquire));
        if (e && e->copy) { *buf = (VkBuffer)e->sbuf; *off = e->soff + (addr - e->base); ptc_count(1); return 1; }
    }
    vkb_rw_read(&g_pool_mu, LK_POOLS);
    pool_ent *p = pool_find_mut((uintptr_t)addr, len);
    if (p) {
        uint64_t sub = g_fence + 1;
        __atomic_store_n(&p->used_sub, sub, __ATOMIC_RELAXED);
        pbc_note(p, sub);
        if (p->sh && p->sync_sub == sub) { *buf = p->sbuf; *off = p->soff + (addr - p->base); }
        else { *buf = p->buf; *off = addr - p->base; }
    }
    vkb_rw_read_end(&g_pool_mu);
    return p != NULL;
}
static pool_ent *pool_find_mut(uintptr_t addr, uint64_t len) { return (pool_ent *)pool_find(addr, len); }
uint64_t vkb_pool_extent(uint64_t addr) {   // [UIT 0331] the farthest end among the pools holding addr
    if (g_ok != 1 || !addr) return 0;
    uint64_t best = 0;
    vkb_rw_read(&g_pool_mu, LK_POOLS);
    int lo = 0, hi = g_npools - 1, found = -1;
    while (lo <= hi) {
        int m = (lo + hi) / 2;
        if (g_pools[m].lo <= addr) { found = m; lo = m + 1; } else hi = m - 1;
    }
    for (int i = found; i >= 0 && i > found - 4; i--)   // nested / adjacent pools, as pool_find
        if (addr >= g_pools[i].lo && addr < g_pools[i].hi && g_pools[i].hi - addr > best) best = g_pools[i].hi - addr;
    vkb_rw_read_end(&g_pool_mu);
    return best;
}

int mtl_addr(uint64_t addr, uint64_t len, void **buf, uint64_t *off) {
    if (g_ok != 1) { *buf = NULL; *off = 0; return 0; }
    vkb_rw_read(&g_pool_mu, LK_POOLS);
    const pool_ent *p = pool_find((uintptr_t)addr, len);
    if (p) { *buf = (void *)p->buf; *off = addr - p->base; }
    vkb_rw_read_end(&g_pool_mu);
    return p != NULL;
}
// [UIT 0141] the uniform capture's merge (vkb_ucap.c, game thread): whether ranges are inside an imported pool, with the
// pool table's read lock taken once for a run of lookups instead of once for each (vkb_pools_hold / vkb_pools_release
// around at most VKB_POOLS_HOLD_MAX vkb_in_pool_held). Same answers as mtl_addr: the table may change between two
// runs as it could between two mtl_addr calls.
int vkb_pools_hold(void) {
    if (g_ok != 1) return 0;
    vkb_rw_read(&g_pool_mu, LK_POOLS);
    return 1;
}
void vkb_pools_release(void) { vkb_rw_read_end(&g_pool_mu); }
int vkb_in_pool_held(uint64_t addr, uint64_t len) { return pool_find((uintptr_t)addr, len) != NULL; }   // g_pool_mu held

// ---------------- submissions ----------------
// kind 1: an event; 2: a counter report; 3 [UIT 0050]: the end of the submission's occlusion queries (mode: the query
// count at its last samples-passed reset or OQ_NONE, v: queries used). qa..qb, oq [UIT 0050]: a samples-passed report's
// queries in this submission; oq 0: not counted (vkb_occl.h), 1: counted, 2: counted plus the earlier submissions' part.
typedef struct { int kind; volatile uint32_t *val; uint32_t mode, v; uint64_t addr, type; uint32_t qa, qb; int oq; } post_op;   // 1 event, 2 counter
typedef struct { VkCommandPool pool; VkCommandBuffer cb; uint64_t fence; } cmd_slot;
#define SLOTS VKB_SLOTS
static cmd_slot g_slots[SLOTS];
static int g_nslots, g_cur = -1;
static post_op *g_post;
static int g_npost, g_capost;
static uint64_t g_op_count[OP_COUNT_];
static int g_op_said[OP_COUNT_];

typedef struct job { uint64_t fence; post_op *ops; int n; int slot; uint64_t *wb; int nwb; struct job *next; uint64_t *vf; int nvf; wb3_snap *m3;
                    vkb_iwb_rec *iw; int niw; } job;   // iw: [UIT 0054] images copied back (the slot's g_iwbst)
static job *g_jobs, **g_jobs_tail = &g_jobs;

// [UIT 0049] TOTK_VK_OCCLUSION: what samples-passed reports write (vkb_occl.h). Unset: 1<<20 every time (as before);
// visible: a different, growing value each time (no occlusion culling, whatever the game compares).
enum { OCCL_FIXED, OCCL_VISIBLE, OCCL_GPU };
static int occl_mode(void) {
    static int m = -1;
    if (m < 0) {
        const char *e = getenv("TOTK_VK_OCCLUSION");
        m = !e || !*e || !strcmp(e, "0") ? OCCL_FIXED : !strcmp(e, "visible") ? OCCL_VISIBLE : !strcmp(e, "gpu") ? OCCL_GPU : OCCL_FIXED;
        if (e && *e && strcmp(e, "0") && m == OCCL_FIXED) VLOG("TOTK_VK_OCCLUSION=%s: unknown (visible or gpu); samples-passed reports stay 1<<20", e);
        else if (m == OCCL_VISIBLE) VLOG("TOTK_VK_OCCLUSION=visible: samples-passed reports write a different, growing value each time (nothing is occlusion-culled)");
        else if (m == OCCL_GPU) VLOG("TOTK_VK_OCCLUSION=gpu: samples-passed reports count the samples the GPU drew since the counter's reset (Vulkan occlusion queries)");
    }
    return m;
}
static oq_acc g_oq_acc;   // the retire thread's (run_posts)
// [UIT 0050] TOTK_VK_OCCLUSION=gpu: one occlusion query pool per command slot (OQ_CAP queries), reset on the host when the
// slot's submission retires; the translation thread hands out the queries (g_oqw: the walk, or EX_ONE), a chunk finds its
// draws' queries by their position (oq_find, below the bound cut with it); t_oq: the query of the draw being recorded.
static VkQueryPool g_oqp[SLOTS];
static VkQueryControlFlags g_oq_flags;
static oq_walk *g_oqw;
static int g_oq_failed;
static __thread int t_oq = -1;
static uint64_t g_oq_counted, g_oq_uncounted;   // samples-passed reports (the submission log line)
static uint64_t g_oq_res[2 * OQ_CAP];           // run_posts (the retire thread)
static int oq_on(void) { return g_oqw && !g_oq_failed; }
static void oq_slot_begin(int slot) {   // mtl_sub_begin: the slot's pool (made once), a new submission's queries
    if (occl_mode() != OCCL_GPU || g_oq_failed) return;
    if (!g_oqw && !(g_oqw = calloc(1, sizeof *g_oqw))) { g_oq_failed = 1; return; }
    if (!g_oqp[slot]) {
        static int said;
        if (!said) {
            said = 1;
            VkPhysicalDeviceFeatures2 f = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
            vk.vkGetPhysicalDeviceFeatures2(vkc.gpu, &f);
            g_oq_flags = f.features.occlusionQueryPrecise ? VK_QUERY_CONTROL_PRECISE_BIT : 0;
            VLOG("TOTK_VK_OCCLUSION=gpu: occlusion queries %s, up to %u per submission", g_oq_flags ? "precise (sample counts)" : "not precise (0 or not 0)", OQ_CAP);
        }
        VkQueryPoolCreateInfo ci = {VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, .queryType = VK_QUERY_TYPE_OCCLUSION, .queryCount = OQ_CAP};
        if (!vk_check(vk.vkCreateQueryPool(vkc.device, &ci, NULL, &g_oqp[slot]), "occlusion query pool")) {
            g_oqp[slot] = VK_NULL_HANDLE;
            g_oq_failed = 1;
            LOG("[vk] TOTK_VK_OCCLUSION=gpu: no occlusion query pool; samples-passed reports read as visible instead");
            return;
        }
        vk.vkResetQueryPool(vkc.device, g_oqp[slot], 0, OQ_CAP);
    }
    oq_sub_begin(g_oqw);
}
void vkb_occl_begin(VkCommandBuffer cb) { if (t_oq >= 0) vk.vkCmdBeginQuery(cb, g_oqp[g_cur], (uint32_t)t_oq, g_oq_flags); }
void vkb_occl_end(VkCommandBuffer cb) { if (t_oq >= 0) vk.vkCmdEndQuery(cb, g_oqp[g_cur], (uint32_t)t_oq); }

static void run_posts(const post_op *ops, int n, int slot) {
    // [UIT 0050] the submission's occlusion query results (the GPU is done with it), when it used any
    uint32_t qn = 0, last_reset = OQ_NONE;
    int qok = 1;
    if (n && ops[n - 1].kind == 3) { last_reset = ops[n - 1].mode; qn = ops[n - 1].v; }
    if (qn) {
        VkResult r = slot >= 0 && g_oqp[slot] ? vk.vkGetQueryPoolResults(vkc.device, g_oqp[slot], 0, qn, sizeof(uint64_t) * 2 * qn, g_oq_res,
                     2 * sizeof(uint64_t), VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT) : VK_ERROR_UNKNOWN;
        qok = r == VK_SUCCESS || r == VK_NOT_READY;   // NOT_READY: some were never begun (their draws returned early): 0
        if (!qok) memset(g_oq_res, 0, sizeof(uint64_t) * 2 * qn);
    }
    for (int i = 0; i < n; i++) {
        if (ops[i].kind == 3) {
            oq_job_end(&g_oq_acc, g_oq_res, last_reset, qn);
            if (qn && slot >= 0 && g_oqp[slot]) vk.vkResetQueryPool(vkc.device, g_oqp[slot], 0, qn);   // before the slot is reused (g_done)
            continue;
        }
        if (ops[i].kind == 1) {
            if (ops[i].mode == 1) *ops[i].val += ops[i].v;
            else *ops[i].val = ops[i].v;
        } else {
            // NVNcounterData {counter, timestamp}. Samples passed: [temporary] reported as visible until draws are replayed
            // (with 0 the game culls every queried object); timestamps and other types report counter 0, as on Metal.
            uint64_t rec[2] = {0, gpu_ticks()};
            if (ops[i].type == NVN_COUNTER_TYPE_SAMPLES_PASSED)
                rec[0] = occl_mode() == OCCL_FIXED ? 1u << 20
                       : ops[i].oq && qok ? oq_value(&g_oq_acc, g_oq_res, ops[i].qa, ops[i].qb, ops[i].oq == 2)   // [UIT 0050]
                       : oq_visible(&g_oq_acc);
            guest_prepare_write((void *)(uintptr_t)ops[i].addr, sizeof rec);
            memcpy((void *)(uintptr_t)ops[i].addr, rec, sizeof rec);
        }
    }
}

static void cpend_done(uint64_t fence);   // conditional rendering (below): the submission's counter reports are in memory
static void complete(uint64_t fence) {
    pthread_mutex_lock(&g_mu);
    if (fence > g_done) g_done = fence;
    pthread_cond_broadcast(&g_cv);
    pthread_mutex_unlock(&g_mu);
}

// ---- TOTK_VK_CHECKPOINTS=1 (development): a marker before each draw, dispatch and copy names it; when the device is
// lost, the markers the GPU last reached (VK_NV_device_diagnostic_checkpoints) are reported with what they name.
enum { CKPT_RING = 1 << 16 };
typedef struct { uint64_t frame; uint32_t n; const char *what; char k1[32], k2[32], info[160]; } ckpt_t;
static ckpt_t *g_ckpt;
static _Atomic uint32_t g_ckpt_n;
static __thread uint32_t g_ckpt_last;   // [UIT 0135] this thread's last checkpoint (ring index + 1)
void vkb_checkpoint(const char *what, const char *k1, const char *k2) {
    if (!vkc.has_checkpoints) return;
    if (!g_ckpt && !(g_ckpt = calloc(CKPT_RING, sizeof *g_ckpt))) return;
    uint32_t n = atomic_fetch_add(&g_ckpt_n, 1), i = n % CKPT_RING;
    ckpt_t *c = &g_ckpt[i];
    c->frame = vkb_frame(); c->n = n; c->what = what;
    snprintf(c->k1, sizeof c->k1, "%s", k1 ? k1 : "-");
    snprintf(c->k2, sizeof c->k2, "%s", k2 ? k2 : "-");
    c->info[0] = 0;
    g_ckpt_last = i + 1;
    vk.vkCmdSetCheckpointNV(vkb_cmd(), (void *)(uintptr_t)(i + 1));
}
// [UIT 0135] what the command of this thread's last checkpoint uses (a draw's index range, counts, vertex buffers),
// printed with it when the device is lost
void vkb_checkpoint_info(const char *fmt, ...) {
    if (!vkc.has_checkpoints || !g_ckpt || !g_ckpt_last) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_ckpt[g_ckpt_last - 1].info, sizeof g_ckpt->info, fmt, ap);
    va_end(ap);
}
// 0008: what a lost device leaves to go on (test4: -4 = VK_ERROR_DEVICE_LOST after 874 s, with nothing before it).
// The driver resets the GPU when a submission runs too long (Windows TDR, about 2 s) or reads or writes memory it has
// no right to; which one it was, only the checkpoints (TOTK_VK_CHECKPOINTS=1) or bounds checks (TOTK_VK_ROBUST=1) tell.
// [UIT 0133] "s in" was runtime_now_ns() (QueryPerformanceCounter: time since Windows started, 1925-2571 s in the
// 2026-10-31 reports of 31-115 s sessions); now seconds since this process started, as the [long frame] lines. The
// upload count is the ASTC / texture cache uploads only (vkb_tex.c g_t_uploads), so it is named so. Added: the device
// memory allocations alive (0133 guard), and the storage buffers bound to the empty stand-in so far (vkb_draw.c).
static uint64_t g_proc_t0;
__attribute__((constructor)) static void proc_t0(void) { g_proc_t0 = runtime_now_ns(); }
static void lost_hint(VkResult r, uint64_t fence) {
    uint64_t v[8];
    vkb_tex_counters(v);
    uint64_t vkb_sbuf_empty_count(void);
    VLOG("the GPU stopped (VkResult %d%s) at submission %llu, present %llu, %.1f s after start; ASTC / cached texture uploads so far %llu, "
         "streamed levels %llu; device memory allocations alive %llu (images' own %llu, pools %u, driver limit %u, targets put in shared "
         "blocks %llu); storage buffers bound empty so far %llu",
         (int)r, r == VK_ERROR_DEVICE_LOST ? ": device lost" : "", (unsigned long long)fence, (unsigned long long)vkb_frame(),
         (runtime_now_ns() - g_proc_t0) / 1e9, (unsigned long long)v[4], (unsigned long long)v[5],
         (unsigned long long)vkb_mem_live(), (unsigned long long)vkb_mem_dedicated_live(), (unsigned)g_npools,
         vkc.props.limits.maxMemoryAllocationCount, (unsigned long long)vkb_mem_crowded(), (unsigned long long)vkb_sbuf_empty_count());
    if (!vkc.has_checkpoints)
        VLOG("to find where: run again with TOTK_VK_CHECKPOINTS=1 (the last commands the GPU reached are logged here; a few %% slower), "
             "and TOTK_VK_ROBUST=1 (bounds-checked buffer reads: if the loss goes away, it was an out-of-range access); "
             "also make sure no overlay or overclock tool is hooking the game, and the GPU driver is current");
}
static void checkpoint_report(void) {
    if (!vkc.has_checkpoints || !g_ckpt) return;
    VkCheckpointDataNV d[16];
    uint32_t n = 16;
    for (uint32_t i = 0; i < n; i++) d[i] = (VkCheckpointDataNV){VK_STRUCTURE_TYPE_CHECKPOINT_DATA_NV};
    vk_queue_lock();
    vk.vkGetQueueCheckpointDataNV(vkc.queue, &n, d);
    vk_queue_unlock();
    VLOG("device lost: %u checkpoints reached (last %u recorded)", n, (unsigned)atomic_load(&g_ckpt_n));
    for (uint32_t i = 0; i < n; i++) {
        uintptr_t m = (uintptr_t)d[i].pCheckpointMarker;
        if (!m || m > CKPT_RING) continue;
        const ckpt_t *c = &g_ckpt[m - 1];
        VLOG("  stage %#llx: #%u present %llu %s %s %s%s%s", (unsigned long long)d[i].stage, c->n, (unsigned long long)c->frame, c->what, c->k1, c->k2,
             c->info[0] ? "; " : "", c->info);
        // [UIT 0135] only the mirror checkpoints carry an address; a draw's k1 is its vertex shader's key (31-32 hex
        // digits), which strtoull turned into 0xffffffffffffffff (ERANGE) in the 2026-10-31 08:53 D2 log: not an address
        unsigned long long a = !strncmp(c->what, "mirror", 6) ? strtoull(c->k1, NULL, 16) : 0;
        if (a) {   // an address: what the memory and the pool table hold now
            MEMORY_BASIC_INFORMATION mi = {0};
            VirtualQuery((void *)(uintptr_t)a, &mi, sizeof mi);
            vkb_rw_read(&g_pool_mu, LK_POOLS);
            const pool_ent *p = pool_find((uintptr_t)a, 1);
            VLOG("    memory %#llx: state %#lx protect %#lx region %p+%#llx; pool %s %#llx..%#llx", a, mi.State, mi.Protect, mi.BaseAddress,
                 (unsigned long long)mi.RegionSize, p ? "registered" : "none", p ? (unsigned long long)p->lo : 0ull, p ? (unsigned long long)p->hi : 0ull);
            vkb_rw_read_end(&g_pool_mu);
        }
    }
}

// ---- deferred destruction: objects given up while a submission is recorded may be used by its earlier commands (a
// texture view replaced in the middle of a frame, a texture the game finalizes). Destroying them after waiting for the
// last submitted work (as before) freed what the open submission still referred to: the GPU read a destroyed view and
// the device was lost (TOTK's item-get screen, 2026-10-24). Now each waits for the first submission after it was given up.
// [UIT 0045] The queue is vkb_defer.h's (box test tools/tests/test_pool_defer.c); deleted pools' buffers and
// replaced samplers wait the same way.
void vkb_destroy_later(VkImageView view, VkImage img, VkDeviceMemory mem) {
    if (!vkd_push(&g_dq, (vkd_obj){.view = view, .img = img, .mem = mem}, 0, 0)) die("deferred destruction");
}
void vkb_destroy_image_later(VkImage img, VkDeviceMemory mem, void *alloc) {   // [UIT 0075]
    if (!vkd_push(&g_dq, (vkd_obj){.img = img, .mem = alloc ? VK_NULL_HANDLE : mem, .alloc = alloc, .pool = alloc || !mem ? 0 : 2}, 0, 0)) die("deferred destruction");
}
void vkb_destroy_sampler_later(VkSampler s) {
    if (s && !vkd_push(&g_dq, (vkd_obj){.samp = s}, 0, 0)) die("deferred destruction");
}
static void dead_tag(uint64_t fence) { vkd_tag(&g_dq, fence); }   // a submission: what was given up so far waits for it
static void dead_reap(uint64_t done) {   // the GPU finished up to `done`
    vkd_ent *list = vkd_take(&g_dq, done);
    for (vkd_ent *d = list; d; d = d->out) {
        if (d->o.view) vk.vkDestroyImageView(vkc.device, d->o.view, NULL);
        if (d->o.img) vk.vkDestroyImage(vkc.device, d->o.img, NULL);
        if (d->o.buf) vk.vkDestroyBuffer(vkc.device, d->o.buf, NULL);
        if (d->o.mem) {
            vk.vkFreeMemory(vkc.device, d->o.mem, NULL);
            if (d->o.pool == 1) vkb_mem_count(-1); else if (d->o.pool == 2) vkb_mem_own_freed();   // [UIT 0133]
        }
        if (d->o.alloc) vkb_mem_free(d->o.alloc);   // [UIT 0075] after its image
        if (d->o.samp) vk.vkDestroySampler(vkc.device, d->o.samp, NULL);
    }
    vkd_release(&g_dq, list);
}

static void gpu_span(int slot, uint64_t fence);
static void gpu_frame_add(int slot);
// Retires submissions in order: waits for the GPU, writes their events and counters, completes their fence.
// ---- [UIT 0054] GPU-drawn images copied back to game memory (vkb_tex.c records the copies; TOTK_VK_IMAGE_WRITEBACK) ----
// The copies go to host memory of their own per slot (cached: the CPU reads it), apart from the buffer copy back's:
// that one grows at most once per submission and would lose copies already recorded. Several asks in one submission
// (the end of it and an explicit read) share it; one that does not fit while it is in use gets none (the image stays
// pending and comes back at the next explicit read).
static wbst_t g_iwbst[VKB_SLOTS];
static uint64_t g_iwbst_tag[VKB_SLOTS];   // g_slots[slot].fence when the slot's memory was last handed out (the submission before)
static VkDeviceSize g_iwbst_used[VKB_SLOTS];
uint8_t *vkb_iwb_stage(int slot, VkDeviceSize need, VkBuffer *buf, VkDeviceSize *off) {   // translation thread
    if (slot < 0 || slot >= VKB_SLOTS || !need) return NULL;
    if (g_iwbst_tag[slot] != g_slots[slot].fence || !g_iwbst[slot].buf) { g_iwbst_tag[slot] = g_slots[slot].fence; g_iwbst_used[slot] = 0; }
    VkDeviceSize at = (g_iwbst_used[slot] + 255) & ~(VkDeviceSize)255;
    if (at + need > g_iwbst[slot].cap) {
        if (g_iwbst_used[slot]) return NULL;
        at = 0;
        VkBuffer b;
        wbst_get_in(&g_iwbst[slot], need, &b);   // (the slot's last submission is done: its memory may go)
    }
    g_iwbst_used[slot] = at + need;
    *buf = g_iwbst[slot].buf; *off = at;
    return g_iwbst[slot].map;
}
int vkb_pool_copy(const void *src, uint64_t n, void *dst) {   // any thread: is [src, +n) in a live pool (and its bytes)
    vkb_rw_read(&g_pool_mu, LK_POOLS);
    int live = pool_find((uintptr_t)src, n) != NULL;
    if (live && dst) memcpy(dst, src, n);
    vkb_rw_read_end(&g_pool_mu);
    return live;
}
static uint64_t g_iwb_flushes[2], g_iwb_bytes, g_iwb_written_kib, g_iwb_kept, g_iwb_unchanged, g_iwb_lgone, g_iwb_land_ns;
static uint64_t g_iwb_reads, g_iwb_stall_ns, g_iwb_stall_max;
// Retire thread (the submission's fence reached) or the translation thread (an explicit read, after its own fence).
// The copy is laid out in a scratch copy of the storage; pages the CPU wrote since the drawing was committed keep the
// CPU's bytes (the snapshot differs there, else the dirty stamp says so); the others are written as GPU writes.
int vkb_iwb_land(const vkb_iwb_rec *r, const uint8_t *host) {
    uint64_t t0 = runtime_now_ns();
    iwb_plan p;
    if (!r->size || !host || iwb_make_plan(&r->d, r->levels, r->layers, &p)) return 0;
    uint8_t *scr = malloc(r->size);
    if (!scr) return 0;
    uintptr_t a = (uintptr_t)r->storage;
    if (!vkb_pool_copy(r->storage, r->size, scr)) { free(scr); __atomic_fetch_add(&g_iwb_lgone, 1, __ATOMIC_RELAXED); return 0; }
    uint8_t *cur = r->snap ? malloc(r->size) : NULL;   // the bytes before ours, against the snapshot
    if (cur) memcpy(cur, scr, r->size);
    iwb_apply(&r->d, &p, host + r->host_off, scr);
    int tracked = !r->snap && nvn_dirty_tracked(r->storage, r->size);
    uint64_t wrote = 0, kept = 0;
    int changed = 0;
    vkb_rw_read(&g_pool_mu, LK_POOLS);   // the pool stays while its memory is written (mtl_pool_del takes it to write)
    if (pool_find(a, r->size)) {
        for (uint64_t o = 0; o < r->size;) {
            uint64_t c = 4096 - ((a + o) & 4095);
            if (c > r->size - o) c = r->size - o;
            int cpu = r->snap ? (cur ? memcmp(cur + o, r->snap + o, c) != 0 : 1) : tracked && nvn_dirty_since(r->storage + o, c, r->stamp);
            if (cpu) kept++;
            else if (nvn_dirty_gpu_write(r->storage + o, scr + o, c)) { nvn_gpu_write_mark(r->storage + o, c); wrote += c; changed = 1; }
            o += c;
        }
        if (changed && iwbpc_on()) iwbpc_note(a, r->size);   // the pool's device copy gets these bytes at its next sync
    } else __atomic_fetch_add(&g_iwb_lgone, 1, __ATOMIC_RELAXED);
    vkb_rw_read_end(&g_pool_mu);
    free(cur); free(scr);
    __atomic_fetch_add(&g_iwb_flushes[r->why ? 1 : 0], 1, __ATOMIC_RELAXED);
    __atomic_fetch_add(&g_iwb_bytes, r->size, __ATOMIC_RELAXED);
    __atomic_fetch_add(&g_iwb_written_kib, wrote >> 10, __ATOMIC_RELAXED);
    __atomic_fetch_add(&g_iwb_kept, kept, __ATOMIC_RELAXED);
    if (!changed) __atomic_fetch_add(&g_iwb_unchanged, 1, __ATOMIC_RELAXED);
    __atomic_fetch_add(&g_iwb_land_ns, runtime_now_ns() - t0, __ATOMIC_RELAXED);
    static _Atomic int said;
    if (atomic_fetch_add(&said, 1) < 32)
        VLOG("[iwb] image %llu %ux%u format %#x, %u levels x %u layers, %llu KiB at %p copied back (%s): %llu KiB changed, %llu pages kept the CPU's",
             (unsigned long long)r->serial, r->d.width, r->d.height, r->d.format, r->levels, r->layers, (unsigned long long)(r->size >> 10),
             (void *)r->storage, r->why ? "explicit read" : "end of submission", (unsigned long long)(wrote >> 10), (unsigned long long)kept);
    return changed;
}
void nvn_x_call(void (*fn)(void *), void *arg);
typedef struct { uint64_t lo, hi; int n; } iwb_x;
static void iwb_explicit_x(void *arg) {   // translation thread: its own submission, waited for (the slot's memory)
    iwb_x *x = arg;
    if (!vkb_iwb_wanted(x->lo, x->hi)) return;
    mtl_sub_begin();
    int slot = g_cur, n = 0;
    vkb_iwb_rec *r = vkb_iwb_record(vkb_cmd(), slot, 1, x->lo, x->hi, &n);
    uint64_t f = mtl_sub_commit();
    if (!n) return;
    mtl_wait(f, UINT64_MAX);
    for (int i = 0; i < n; i++) vkb_iwb_land(&r[i], g_iwbst[slot].map);
    vkb_iwb_free(r, n);
    x->n = n;
}
static void iwb_explicit_read(uint64_t lo, uint64_t hi) {   // the game's thread
    uint64_t t0 = runtime_now_ns();
    iwb_x x = {lo, hi, 0};
    nvn_x_call(iwb_explicit_x, &x);
    uint64_t dt = runtime_now_ns() - t0;
    __atomic_fetch_add(&g_iwb_reads, 1, __ATOMIC_RELAXED);
    __atomic_fetch_add(&g_iwb_stall_ns, dt, __ATOMIC_RELAXED);
    uint64_t m = __atomic_load_n(&g_iwb_stall_max, __ATOMIC_RELAXED);
    while (dt > m && !__atomic_compare_exchange_n(&g_iwb_stall_max, &m, dt, 0, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {}
}
static void iwb_log(uint64_t every) {   // with the [occl] / [draws] lines
    if (vkb_iwb_mode() == IWB_OFF) return;
    static uint64_t pf0, pf1, pb, pw, pk, pu, pg, pl, pr, ps;
    uint64_t f0 = g_iwb_flushes[0], f1 = g_iwb_flushes[1], b = g_iwb_bytes, w = g_iwb_written_kib, k = g_iwb_kept, u = g_iwb_unchanged;
    uint64_t g = g_iwb_lgone, l = g_iwb_land_ns, rd = g_iwb_reads, st = g_iwb_stall_ns, mx = __atomic_exchange_n(&g_iwb_stall_max, 0, __ATOMIC_RELAXED);
    uint64_t el, live, pend, sup, big, sh, gone;
    vkb_iwb_stats(&el, &live, &pend, &sup, &big, &sh, &gone);
    double fr = (double)every;
    LOG("[iwb] per frame (last %llu): flushes %.2f (end of submission %.2f, explicit %.2f), %.1f KiB copied, %.1f KiB changed; "
        "explicit reads %.2f, stall %.2f ms a frame, longest %.2f ms; landing %.2f ms a frame; pages kept the CPU's %.1f, unchanged %.2f; "
        "pool gone %llu; images eligible %llu now (%llu made since the start), pending %llu, superseded %llu, too large %llu, no host memory %llu, gone at commit %llu",
        (unsigned long long)every, (double)(f0 + f1 - pf0 - pf1) / fr, (double)(f0 - pf0) / fr, (double)(f1 - pf1) / fr,
        (double)(b - pb) / fr / 1024.0, (double)(w - pw) / fr, (double)(rd - pr) / fr, (double)(st - ps) / fr / 1e6, (double)mx / 1e6,
        (double)(l - pl) / fr / 1e6, (double)(k - pk) / fr, (double)(u - pu) / fr, (unsigned long long)(g - pg), (unsigned long long)live,
        (unsigned long long)el, (unsigned long long)pend, (unsigned long long)sup, (unsigned long long)big, (unsigned long long)sh, (unsigned long long)gone);
    pf0 = f0; pf1 = f1; pb = b; pw = w; pk = k; pu = u; pg = g; pl = l; pr = rd; ps = st;
    static uint64_t pha; uint64_t hn, ha; vkb_iwb_ahead_stats(&hn, &ha);   // [UIT 0255] a line of its own (the one above unchanged)
    if (hn || ha != pha) LOG("[iwb] read back ahead (TOTK_VK_READBACK_AHEAD): %llu images the CPU reads back, %.2f copies a frame with the submissions that draw them",
                            (unsigned long long)hn, (double)(ha - pha) / fr);
    pha = ha;
}

static void *retire_thread(void *unused) {
    (void)unused;
    pthread_setname_np("totk-vk-retire");
    // [UIT 0250] TOTK_VK_RETIRE_PCORES (on; =0 as before): the retire thread is on every frame's critical path (the GPU
    // finishes -> this thread wakes, copies GPU writes back into game memory, marks the fence -> the game's fence wait
    // returns) but was the only graphics thread left where Windows puts it, often an efficiency core of a hybrid CPU
    // (slower wake-up and copy-back). It now takes the shared performance cores, as the submit and recording threads
    // (plat_thread_pcores: affinity only, with TOTK_VK_PCORES / TOTK_HYBRID_PLACEMENT; nothing on other CPUs / Linux).
    {
        const char *e = getenv("TOTK_VK_RETIRE_PCORES");
        int on = !(e && e[0] == '0' && !e[1]);
        int plat_thread_pcores(void);
        __atomic_store_n(&g_ret_pc, on ? plat_thread_pcores() : 0, __ATOMIC_RELAXED);
        VLOG("[UIT 0250] retire thread: %s", !on ? "where Windows puts it (TOTK_VK_RETIRE_PCORES=0)"
             : g_ret_pc > 0 ? "on the shared performance cores (TOTK_VK_RETIRE_PCORES)"
             : "where Windows puts it (TOTK_VK_RETIRE_PCORES on, but no performance-core placement here)");
    }
    for (;;) {
        pthread_mutex_lock(&g_mu);
        while (!g_jobs) pthread_cond_wait(&g_cv, &g_mu);
        job *j = g_jobs;
        g_jobs = j->next;
        if (!g_jobs) g_jobs_tail = &g_jobs;
        pthread_mutex_unlock(&g_mu);
        VkSemaphoreWaitInfo wi = {VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO, .semaphoreCount = 1, .pSemaphores = &g_timeline,
                                  .pValues = &j->fence};
        VkResult r = vk.vkWaitSemaphores(vkc.device, &wi, UINT64_MAX);
        if (r != VK_SUCCESS) { vk_check(r, "vkWaitSemaphores"); checkpoint_report(); lost_hint(r, j->fence); die("GPU submission %llu lost", (unsigned long long)j->fence); }
        uint64_t rt0 = runtime_now_ns();   // [UIT 0250]
        if (totk_tl_on()) gpu_span(j->slot, j->fence);   // TOTK_TIMELINE: when the GPU ran this command buffer
        gpu_frame_add(j->slot);   // [UIT native] GPU time per game frame (mtl_gpu_ms)
        if (j->wb) wb_merge(j->wb, j->nwb, g_wbst[j->slot].map, j->m3);   // GPU writes into game memory
        m3_free(j->m3);
        if (j->vf) { vf_check(j->fence, j->vf, j->nvf, j->wb, j->nwb, g_wbst[j->slot].map); free(j->vf); }   // TOTK_VK_SHADOW_VERIFY
        free(j->wb);
        if (j->iw) { for (int i = 0; i < j->niw; i++) vkb_iwb_land(&j->iw[i], g_iwbst[j->slot].map); vkb_iwb_free(j->iw, j->niw); }   // [UIT 0054]
        run_posts(j->ops, j->n, j->slot);
        cpend_done(j->fence);
        complete(j->fence);
        atomic_fetch_add_explicit(&g_ret_ns, runtime_now_ns() - rt0, memory_order_relaxed);   // [UIT 0250]
        atomic_fetch_add_explicit(&g_ret_n, 1, memory_order_relaxed);
        dead_reap(j->fence);
        free(j->ops);
        free(j);
    }
    return NULL;
}
void retire_start(void) {
    pthread_t t;
    if (pthread_create(&t, NULL, retire_thread, NULL)) die("retire thread");
}
static void jobs_push(job *j) {   // to the retire thread, after the submission's vkQueueSubmit2 (in fence order)
    pthread_mutex_lock(&g_mu);
    *g_jobs_tail = j;
    g_jobs_tail = &j->next;
    pthread_cond_broadcast(&g_cv);
    pthread_mutex_unlock(&g_mu);
}

// ---- [perf-mt] TOTK_VK_ASYNC_SUBMIT=1: the queue submit on a thread of its own
// vkQueueSubmit2 took up to ~9 ms a frame of the translation thread (the per-frame "queue submits", 2026-10 logs; spikes
// to 13 ms), most of "moves + submit" in the recording threads' log line. With this the thread that recorded a
// submission ends its command buffers and builds its retire job as before, then hands the command buffer list to the
// submit thread (vkb_subq.h) and goes on with the next one. Why it is safe:
//   - submitted strictly in fence order (vkb_subq_pop), so the timeline semaphore's values increase as before, and the
//     retire job is queued after its submit, as before
//   - a command slot, its command pools and copy-back memory are reused only once their fence is complete (g_done),
//     which needs the submit first; so are destroyed objects ([UIT 0045] dead_tag / vkd queue: completion fences)
//   - mtl_wait / mtl_finish wait for completion, which implies the submit
//   - a frame's "ready" for the present thread (vkp_out_ready: a GPU wait on its timeline value) is given only after the
//     submit that signals that value: no wait queued before its signal
// The cost: one malloc + a copy of the command buffer list a submission (~1-3 us), and the GPU gets the work after the
// hand-off (tens of us) instead of at once.
typedef struct sub_req {
    vkb_subq_item q;   // first: fence, link
    int skip;          // no GPU work (no Vulkan yet): only the fence's turn
    int partial;       // [UIT 0236] an early part (vkb_early.h): submitted without a signal; the fence's turn goes on
    job *j;
    int out_slot, has_crop, crop[4];   // >= 0: the frame mtl_present copied (vkp_out_ready after the submit)
    int ncbi;
    VkCommandBufferSubmitInfo cbi[];
} sub_req;
static __thread int t_present_slot = -1, t_present_has_crop, t_present_crop[4];   // mtl_present -> mtl_sub_commit
static void *submit_thread(void *unused) {
    (void)unused;
    pthread_setname_np("totk-vk-submit");
    { int plat_thread_pcores(void); plat_thread_pcores(); }   // [UIT 0033] TOTK_VK_PCORES=1: performance cores, as the recording threads
    for (;;) {
        sub_req *r = (sub_req *)vkb_subq_pop(&g_subq);
        uint64_t fence = r->q.fence;
        if (r->partial) {   // [UIT 0236] an early part of `fence`: no signal, no retire job; its last part comes later
            uint64_t ts0 = runtime_now_ns();
            VkSubmitInfo2 si = {VK_STRUCTURE_TYPE_SUBMIT_INFO_2, .commandBufferInfoCount = (uint32_t)r->ncbi, .pCommandBufferInfos = r->cbi};
            vk_queue_lock();
            VkResult res = vk.vkQueueSubmit2(vkc.queue, 1, &si, VK_NULL_HANDLE);
            vk_queue_unlock();
            vkb_step_add(PK_SUBMIT, ts0);
            if (res != VK_SUCCESS) { vk_check(res, "vkQueueSubmit2 (early part)"); die("GPU submission %llu (early part) failed", (unsigned long long)fence); }
            free(r);
            continue;
        }
        if (!r->skip) {
            uint64_t ts0 = runtime_now_ns();
            atomic_fetch_add_explicit(&g_async_lag_ns, ts0 - r->q.t_push, memory_order_relaxed);
            atomic_fetch_add_explicit(&g_async_n, 1, memory_order_relaxed);
            VkSemaphoreSubmitInfo sig = {VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, .semaphore = g_timeline, .value = fence,
                                         .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT};
            VkSubmitInfo2 si = {VK_STRUCTURE_TYPE_SUBMIT_INFO_2, .commandBufferInfoCount = (uint32_t)r->ncbi, .pCommandBufferInfos = r->cbi,
                                .signalSemaphoreInfoCount = 1, .pSignalSemaphoreInfos = &sig};
            vk_queue_lock();
            VkResult res = vk.vkQueueSubmit2(vkc.queue, 1, &si, VK_NULL_HANDLE);
            vk_queue_unlock();
            vkb_step_add(PK_SUBMIT, ts0);
            if (res != VK_SUCCESS) { vk_check(res, "vkQueueSubmit2"); die("GPU submission %llu failed", (unsigned long long)fence); }
            jobs_push(r->j);
        }
        vkb_subq_done(&g_subq, fence);
        if (r->out_slot >= 0) vkp_out_ready(r->out_slot, g_timeline, fence, r->has_crop ? r->crop : NULL);
        free(r);
    }
    return NULL;
}
void submit_start(void) {
    // [UIT 0151] on by default (TOTK_VK_ASYNC_SUBMIT=0: on the recording thread, as before): the 2026-10-30 14:25 long play
    // (c2736a1, ~290,000 frames, closed normally) ran with it; "moves + submit" on the translation thread 0.11 ms a frame
    // there vs 3.19 ms without it in the 21:08 log
    const char *e = getenv("TOTK_VK_ASYNC_SUBMIT");
    if (e && *e && atoi(e) != 1) { VLOG("queue submits: on the recording thread (TOTK_VK_ASYNC_SUBMIT=0; the default is a thread of their own)"); return; }
    pthread_mutex_lock(&g_subq.mu);   // fences taken before Vulkan was ready completed at once: the queue starts after them
    g_subq.next = __atomic_load_n(&g_fence, __ATOMIC_SEQ_CST) + 1;
    g_subq.submitted = g_subq.next - 1;
    pthread_mutex_unlock(&g_subq.mu);
    pthread_t t;
    if (pthread_create(&t, NULL, submit_thread, NULL)) die("submit thread");
    atomic_store(&g_async, 1);
    VLOG("queue submits: on a thread of their own (TOTK_VK_ASYNC_SUBMIT=1)");
}

// GPU time of every submission (a timestamp at its start and end), summed per frame for the per-frame log line, and the
// time the recording thread waits for a free command slot (the GPU is behind).
static VkQueryPool g_tsq;
static int g_ts_used[SLOTS];
static uint64_t g_gpu_ns, g_slot_wait_ns;
static void gpu_time_begin(int slot) {   // into the slot's own command buffer (the submission's first)
    if (!g_tsq) {
        VkQueryPoolCreateInfo ci = {VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, .queryType = VK_QUERY_TYPE_TIMESTAMP, .queryCount = 2 * SLOTS};
        if (!vk_check(vk.vkCreateQueryPool(vkc.device, &ci, NULL, &g_tsq), "timestamp pool")) return;
        vk.vkResetQueryPool(vkc.device, g_tsq, 0, 2 * SLOTS);
    }
    if (g_ts_used[slot]) {   // the slot's previous submission is complete
        uint64_t t[2];
        if (vk.vkGetQueryPoolResults(vkc.device, g_tsq, (uint32_t)(2 * slot), 2, sizeof t, t, sizeof t[0], VK_QUERY_RESULT_64_BIT) == VK_SUCCESS && t[1] > t[0])
            g_gpu_ns += (uint64_t)((double)(t[1] - t[0]) * vkc.props.limits.timestampPeriod);
        g_ts_used[slot] = 0;
    }
    vk.vkCmdResetQueryPool(g_slots[slot].cb, g_tsq, (uint32_t)(2 * slot), 2);
    vk.vkCmdWriteTimestamp2(g_slots[slot].cb, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, g_tsq, (uint32_t)(2 * slot));
}
// The GPU span of a completed command buffer on the host clock: it ended about when the retire thread saw its fence
// (the thread waits on fences in order), and lasted what its two timestamps say.
static void gpu_span(int slot, uint64_t fence) {
    uint64_t end = runtime_now_ns(), t[2];
    if (!g_tsq || slot < 0) return;
    if (vk.vkGetQueryPoolResults(vkc.device, g_tsq, (uint32_t)(2 * slot), 2, sizeof t, t, sizeof t[0], VK_QUERY_RESULT_64_BIT) != VK_SUCCESS || t[1] <= t[0]) return;
    uint64_t dur = (uint64_t)((double)(t[1] - t[0]) * vkc.props.limits.timestampPeriod);
    totk_tl_at(TL_GPU, end > dur ? end - dur : 0, fence, end);
}
// [UIT native] GPU time per game frame, for the long-frame log and the per-frame line (mtl_gpu_ms / mtl_gpu_span,
// which were the null backend's stubs: every "[long frame]" line said GPU 0.0 ms). A submission belongs to the frame
// mtl_set_frame last named when it was committed; the retire thread adds its two timestamps into that frame's entry
// once its fence is done. A frame's entry is read 8 frames later (hle_nvn.c), by when its submissions are complete.
// The slot's queries can be reset by its next use before the retire thread reads them: then that submission is not
// counted (vkGetQueryPoolResults says not ready) - a diagnostic, it can only under-count.
static _Atomic uint64_t g_vk_frame;            // the game frame the submissions being committed belong to
static _Atomic uint64_t g_slot_frame[SLOTS];   // the frame of each slot's last submission
#include "vkb_gpuframe.h"
static gpu_frame g_gf[GFN];
void mtl_set_frame(uint64_t f) { atomic_store_explicit(&g_vk_frame, f, memory_order_relaxed); }
static void gpu_frame_add(int slot) {   // the retire thread, after the slot's fence
    uint64_t t[2], end = runtime_now_ns();
    if (!g_tsq || slot < 0 || slot >= SLOTS) return;
    if (vk.vkGetQueryPoolResults(vkc.device, g_tsq, (uint32_t)(2 * slot), 2, sizeof t, t, sizeof t[0], VK_QUERY_RESULT_64_BIT) != VK_SUCCESS || t[1] <= t[0]) return;
    uint64_t dur = (uint64_t)((double)(t[1] - t[0]) * vkc.props.limits.timestampPeriod), beg = end > dur ? end - dur : 0;
    gpu_frame_put(g_gf, atomic_load_explicit(&g_slot_frame[slot], memory_order_relaxed), beg, end, dur);
}
double mtl_gpu_ms(uint64_t f) { return gpu_frame_ms(g_gf, f); }   // -1: not measured (no submission of that frame seen complete)
int mtl_gpu_span(uint64_t f, uint64_t *t0, uint64_t *t1) { return gpu_frame_span(g_gf, f, t0, t1); }   // host clock
// The average and the worst frame of the last `n` complete frames (ending 8 frames back): the per-frame log line.
static void gpu_frames_recent(uint64_t n, double *avg, double *worst, uint64_t *got) {
    uint64_t cur = atomic_load_explicit(&g_vk_frame, memory_order_relaxed), c = 0;
    double s = 0, w = 0;
    if (n > GFN - 16) n = GFN - 16;
    for (uint64_t f = cur > 8 + n ? cur - 8 - n : 1; f + 8 <= cur; f++) {
        double g = mtl_gpu_ms(f);
        if (g < 0) continue;
        s += g; c++; if (g > w) w = g;
    }
    *avg = c ? s / c : 0; *worst = w; *got = c;
}
// [UIT 0232] the [critical path] line's GPU time: the average of the last 300 measured frames, -1 when none
double vkb_gpu_ms_recent(void) { double a, w; uint64_t g; gpu_frames_recent(300, &a, &w, &g); return g ? a : -1; }
static void gpu_time_end(int slot, VkCommandBuffer cb) {   // into the submission's last command buffer
    if (!g_tsq) return;
    vk.vkCmdWriteTimestamp2(cb, VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT, g_tsq, (uint32_t)(2 * slot + 1));
    g_ts_used[slot] = 1;
    atomic_store_explicit(&g_slot_frame[slot], atomic_load_explicit(&g_vk_frame, memory_order_relaxed), memory_order_relaxed);
}
// [UIT native] cumulative counters of the replay's own work, read once a frame by the long-frame log (hle_nvn.c,
// vkb_lf_counters): what the recording / translation side was doing during a long frame.
uint64_t vkb_slot_wait_ns(void) { return __atomic_load_n(&g_slot_wait_ns, __ATOMIC_RELAXED); }
uint64_t vkb_shader_bg_ns(void);
void plat_io_stats(uint64_t *ns, uint64_t *bytes);
// v[0] recording waits for a free command slot, [1] new images, [2] queue submits, [3] lock waits (all tables),
// [4] background shader compiles, [5] file reads (ns), [6] file bytes; with TOTK_PROFILE (else 0): [7] translation
// thread replaying, [8] of it waiting for recording threads, [9] recording threads, [10] game fence waits, [11] game
// threads waiting for the translation queue to drain. All ns, running totals.
void vkb_lf_counters(uint64_t v[12]) {
    v[0] = vkb_slot_wait_ns();
    v[1] = atomic_load_explicit(&vkb_step_ns[PK_CREATE], memory_order_relaxed);
    v[2] = atomic_load_explicit(&vkb_step_ns[PK_SUBMIT], memory_order_relaxed);
    v[3] = 0; for (int k = 0; k < LK_COUNT; k++) v[3] += atomic_load_explicit(&vkb_lock_wait_ns[k], memory_order_relaxed);
    v[4] = vkb_shader_bg_ns();
    plat_io_stats(&v[5], &v[6]);
    int p = uit_prof_on() > 0;
    v[7] = p ? atomic_load_explicit(&uit_prof_ns[UP_X_SUBMIT], memory_order_relaxed) : 0;
    v[8] = p ? atomic_load_explicit(&uit_prof_ns[UP_X_RECWAIT], memory_order_relaxed) : 0;
    v[9] = p ? atomic_load_explicit(&uit_prof_ns[UP_R_REC], memory_order_relaxed) : 0;
    v[10] = p ? atomic_load_explicit(&uit_prof_ns[UP_G_SYNC], memory_order_relaxed) : 0;
    v[11] = p ? atomic_load_explicit(&uit_prof_ns[UP_G_DRAIN], memory_order_relaxed) : 0;
}

// [UIT native] TOTK_VK_MEMLOG=1 (launcher: Advanced > Developer: profiling+ > Memory use in the log): with the per-frame lines
// (every 300 frames), the video memory the driver gives this process against its budget (VK_EXT_memory_budget:
// device-local heaps, and the system memory the GPU uses), and the process's RAM (working set, commit, their peaks)
// and the system's free memory. Read-only queries; nothing changes. Off: no line.
// [UIT 0104] No <psapi.h> (the builder package's trimmed MinGW headers have none, nor libpsapi.a): the process
// counters come from kernel32's K32GetProcessMemoryInfo (Windows 7+), looked up once by name, with the documented
// PROCESS_MEMORY_COUNTERS layout of its own; when the lookup fails the line says 0 for the process figures.
typedef struct {
    DWORD cb, PageFaultCount;
    SIZE_T PeakWorkingSetSize, WorkingSetSize, QuotaPeakPagedPoolUsage, QuotaPagedPoolUsage,
        QuotaPeakNonPagedPoolUsage, QuotaNonPagedPoolUsage, PagefileUsage, PeakPagefileUsage;
} uit_process_memory;
typedef BOOL (WINAPI *uit_get_process_memory_fn)(HANDLE, uit_process_memory *, DWORD);
static int uit_process_memory_info(uit_process_memory *pm) {
    static uit_get_process_memory_fn fn;
    static int looked;
    if (!looked) {
        HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
        fn = k32 ? (uit_get_process_memory_fn)(void (*)(void))GetProcAddress(k32, "K32GetProcessMemoryInfo") : NULL;
        looked = 1;
    }
    return fn && fn(GetCurrentProcess(), pm, sizeof *pm);
}
static int memlog_on(void) {
    static int on = -1;
    if (on < 0) { const char *e = getenv("TOTK_VK_MEMLOG"); on = e && atoi(e) > 0; }
    return on;
}
static void vkb_mem_log(void) {
    if (!memlog_on()) return;
    char gpu[256] = "video memory budget: not reported by the driver (no VK_EXT_memory_budget)";
    if (vkc.has_memory_budget && vk.vkGetPhysicalDeviceMemoryProperties2) {
        VkPhysicalDeviceMemoryBudgetPropertiesEXT b = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT};
        VkPhysicalDeviceMemoryProperties2 mp = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2, &b};
        vk.vkGetPhysicalDeviceMemoryProperties2(vkc.gpu, &mp);
        uint64_t du = 0, db = 0, su = 0, sb = 0;
        for (uint32_t i = 0; i < mp.memoryProperties.memoryHeapCount && i < VK_MAX_MEMORY_HEAPS; i++) {
            if (mp.memoryProperties.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) { du += b.heapUsage[i]; db += b.heapBudget[i]; }
            else { su += b.heapUsage[i]; sb += b.heapBudget[i]; }
        }
        snprintf(gpu, sizeof gpu, "video memory %.0f / %.0f MiB of the budget (%.0f%%), system memory used by the GPU %.0f / %.0f MiB",
                 du / 1048576.0, db / 1048576.0, db ? 100.0 * du / db : 0.0, su / 1048576.0, sb / 1048576.0);
    }
    uit_process_memory pm = {.cb = sizeof pm};
    MEMORYSTATUSEX ms = {.dwLength = sizeof ms};
    int okp = uit_process_memory_info(&pm), oks = GlobalMemoryStatusEx(&ms) != 0;
    VLOG("[memory] %s; process RAM %.0f MiB (peak %.0f), commit %.0f MiB (peak %.0f); system RAM free %.0f / %.0f MiB",
         gpu, okp ? pm.WorkingSetSize / 1048576.0 : 0.0, okp ? pm.PeakWorkingSetSize / 1048576.0 : 0.0,
         okp ? pm.PagefileUsage / 1048576.0 : 0.0, okp ? pm.PeakPagefileUsage / 1048576.0 : 0.0,
         oks ? ms.ullAvailPhys / 1048576.0 : 0.0, oks ? ms.ullTotalPhys / 1048576.0 : 0.0);
}

// A submission whose recording took over 100 ms: where the time went (log line "long submission"). The game waits for
// its recording at the present: these are the hitches when new parts of the world arrive.
extern uint64_t vkb_astc_ns, vkb_shader_load_ns;
static uint64_t g_sub_t0, g_sub_snap[6], g_sub_tex[8];
static void sub_snap(uint64_t v[6]) {
    v[0] = vkb_step_ns[PK_CREATE]; v[1] = vkb_step_ns[PK_UPLOAD]; v[2] = vkb_astc_ns; v[3] = vkb_shader_load_ns;
    v[4] = g_sh_scan_ns; v[5] = g_sh_in_bytes;
}
static void sub_long_check(uint64_t fence) {
    uint64_t now = runtime_now_ns(), v[6];
    if (!g_sub_t0 || now - g_sub_t0 < 100000000ull) return;
    sub_snap(v);
    static int said;
    if (said++ < 64)
        VLOG("long submission %llu: recorded in %.0f ms: new images %.0f ms, uploads %.0f ms (ASTC %.0f ms), shaders made %.0f ms, "
             "pool copy checks %.0f ms, pool copies in %.1f MiB; draws %llu", (unsigned long long)fence, (now - g_sub_t0) / 1e6,
             (v[0] - g_sub_snap[0]) / 1e6, (v[1] - g_sub_snap[1]) / 1e6, (v[2] - g_sub_snap[2]) / 1e6, (v[3] - g_sub_snap[3]) / 1e6,
             (v[4] - g_sub_snap[4]) / 1e6, (v[5] - g_sub_snap[5]) / 1048576.0, (unsigned long long)vkb_draw_count());
    uint64_t t[8];   // the uploads in detail (vkb_tex.c; times summed over the recording threads)
    vkb_tex_counters(t);
    if (said <= 64)
        VLOG("long submission %llu: textures: %llu ASTC uploads, first look (deswizzle + key) %.0f ms, cache reads %.0f ms (%.1f MiB), "
             "into upload memory %.0f ms; streamed mips applied %llu (%.1f MiB), grace waits %.1f ms", (unsigned long long)fence,
             (unsigned long long)(t[4] - g_sub_tex[4]), (t[0] - g_sub_tex[0]) / 1e6, (t[1] - g_sub_tex[1]) / 1e6, (t[2] - g_sub_tex[2]) / 1048576.0,
             (t[3] - g_sub_tex[3]) / 1e6, (unsigned long long)(t[5] - g_sub_tex[5]), (t[6] - g_sub_tex[6]) / 1048576.0, (t[7] - g_sub_tex[7]) / 1e6);
}

void mtl_sub_begin(void) {
    backend_init();
    g_sub_t0 = runtime_now_ns();
    sub_snap(g_sub_snap);
    vkb_tex_counters(g_sub_tex);
    g_npost = 0;
    if (g_ok != 1) return;
    if (!t_open) { t_open = 1; atomic_fetch_add(&g_open, 1); }   // [UIT 0045] before anything is recorded (mtl_pool_del)
    { int k = vkb_recown_enter(&g_recown);
      if (k && k <= 4) LOG("[vk] two threads record GPU work at once (clash %d): the GPU work of a game thread should go through the "
                           "translation thread (nvn_x_call); please send this log", k); }
    pthread_mutex_lock(&g_mu);
    uint64_t done = g_done;
    pthread_mutex_unlock(&g_mu);
    int pick = -1;
    for (int i = 0; i < g_nslots; i++) if (g_slots[i].fence <= done) { pick = i; break; }
    if (pick < 0 && g_nslots < SLOTS) {
        VkCommandPoolCreateInfo pi = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT,
                                      .queueFamilyIndex = vkc.family};
        cmd_slot *s = &g_slots[g_nslots];
        if (!vk_check(vk.vkCreateCommandPool(vkc.device, &pi, NULL, &s->pool), "command pool")) die("command pool");
        VkCommandBufferAllocateInfo ai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = s->pool,
                                          .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
        if (!vk_check(vk.vkAllocateCommandBuffers(vkc.device, &ai, &s->cb), "command buffer")) die("command buffer");
        pick = g_nslots++;
    }
    if (pick < 0) {   // all in flight: the oldest
        pick = 0;
        for (int i = 1; i < g_nslots; i++) if (g_slots[i].fence < g_slots[pick].fence) pick = i;
        uint64_t t0 = runtime_now_ns();
        mtl_wait(g_slots[pick].fence, UINT64_MAX);
        g_slot_wait_ns += runtime_now_ns() - t0;
    }
    g_cur = pick;
    nvn_dirty_epoch_begin();   // write-watch scans: once per range in this submission (nvn_dirty_win.c)
    vkb_slot_reset(pick);   // its previous submission is complete: upload memory and descriptors are free
    vkb_mt_slot_reset(pick);
    vk.vkResetCommandPool(vkc.device, g_slots[pick].pool, 0);
    VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    vk.vkBeginCommandBuffer(g_slots[pick].cb, &bi);
    gpu_time_begin(pick);
    oq_slot_begin(pick);   // [UIT 0050]
}

static void post(post_op o) {
    if (g_npost == g_capost) {
        g_capost = g_capost ? g_capost * 2 : 64;
        g_post = realloc(g_post, (size_t)g_capost * sizeof *g_post);
        if (!g_post) die("post ops");
    }
    g_post[g_npost++] = o;
}
void mtl_sub_event(volatile uint32_t *val, uint32_t mode, uint32_t v) { post((post_op){1, val, mode, v, 0, 0}); }
static uint64_t g_counter_types[32];   // reports by NVN counter type (the per-frame log line)
// ---------- Conditional rendering (NVN SetRenderEnableConditional), evaluated in GPU order ----------
// Ported from MacNativeGames nvn_mtl.m (cpend_*, cond_eval; MIT). TOTK gates its ClearColor commands with it: the CPU
// writes counter=1 to request a clear and the TIMESTAMP_TOP report replayed right after the clear resets it to 0, so the
// clear runs once. Counter reports replayed earlier whose memory write has not happened yet (run_posts writes it when
// the submission retires) are kept here with the value they will write: 0 for timestamps and other types, unknown for
// samples passed. A conditional looks there first, then at game memory - the counter values the GPU would see.
// Replaying every gated clear (as before 2026-10-24) cleared light and history targets on frames the game had not
// asked for: menu and world lighting went black for a few frames after camera cuts.
typedef struct { uint64_t addr, fence, val; int known; } cpend_ent;
#define CPEND_N 4096
static cpend_ent g_cpend[CPEND_N];
static unsigned g_cpend_head, g_cpend_tail;
static pthread_mutex_t g_cpend_mu = PTHREAD_MUTEX_INITIALIZER;   // replay thread pushes, retire thread clears
static void cpend_push(uint64_t addr, uint64_t fence, uint64_t val, int known) {
    pthread_mutex_lock(&g_cpend_mu);
    g_cpend[g_cpend_head % CPEND_N] = (cpend_ent){addr, fence, val, known};
    g_cpend_head++;
    pthread_mutex_unlock(&g_cpend_mu);
}
static void cpend_done(uint64_t fence) {   // the reports of this submission are in memory now
    pthread_mutex_lock(&g_cpend_mu);
    if (g_cpend_head - g_cpend_tail > CPEND_N) g_cpend_tail = g_cpend_head - CPEND_N;
    for (unsigned i = g_cpend_tail; i != g_cpend_head; i++)
        if (g_cpend[i % CPEND_N].addr && g_cpend[i % CPEND_N].fence <= fence) g_cpend[i % CPEND_N].addr = 0;
    while (g_cpend_tail != g_cpend_head && !g_cpend[g_cpend_tail % CPEND_N].addr) g_cpend_tail++;
    pthread_mutex_unlock(&g_cpend_mu);
}
static int cpend_lookup(uint64_t addr, uint64_t *val) {   // 1 known (in *val), 0 pending but unknown, -1 none (read memory)
    int r = -1;
    pthread_mutex_lock(&g_cpend_mu);
    unsigned n = g_cpend_head - g_cpend_tail;
    if (n > CPEND_N) n = CPEND_N;
    for (unsigned k = 0; k < n; k++) {   // the latest report to the address
        const cpend_ent *e = &g_cpend[(g_cpend_head - 1 - k) % CPEND_N];
        if (e->addr == addr) { r = e->known; *val = e->val; break; }
    }
    pthread_mutex_unlock(&g_cpend_mu);
    return r;
}
// [UIT 0049] Addresses that received a samples-passed report (a small direct-mapped set; the walk / one thread): the
// "off from samples-passed words" count no longer depends on the value written (TOTK_VK_OCCLUSION changes it).
static uint64_t g_sp_addr[1024];
static void sp_note(uint64_t addr) { g_sp_addr[(addr >> 4) & 1023] = addr; }
static int sp_has(uint64_t addr) { return addr && g_sp_addr[(addr >> 4) & 1023] == addr; }
static uint64_t g_reset_types[32];   // [UIT 0049] ResetCounter by NVN counter type (the per-frame log line)
static void counter_reset(uint32_t type) {
    g_reset_types[type & 31]++;
    if (type == NVN_COUNTER_TYPE_SAMPLES_PASSED && oq_on()) oq_reset(g_oqw);   // [UIT 0050] draws count from here
}
static vkb_cnt g_cond_skipped;     // draws and clears not replayed (log line)
static uint64_t g_cond_unknown;    // conditionals taken as enabled (log line; the walk / one thread)
static uint64_t g_cond_off_pending, g_cond_off_samples, g_cond_off_mem;   // [UIT 0048] conditionals evaluated off, by source
static int cond_eval(const p_render_cond *c) {   // 1: rendering enabled
    uint64_t x = 0, y = 0;
    int kx = cpend_lookup(c->addr, &x), ky = cpend_lookup(c->addr + 16, &y);
    void *bf; uint64_t off;
    if (kx == 0 || ky == 0 || !mtl_addr(c->addr, 32, &bf, &off)) { g_cond_unknown++; return 1; }
    if (kx < 0) memcpy(&x, (const void *)(uintptr_t)c->addr, 8);
    if (ky < 0) memcpy(&y, (const void *)(uintptr_t)(c->addr + 16), 8);
    int on = c->mode == 0 ? x == y : x != y;
    if (!on) {   // [UIT 0048] where an "off" came from (log line): a pending report, the 1<<20 samples-passed word, other memory
        if (kx > 0 && ky > 0) g_cond_off_pending++;
        else if (x == 1u << 20 || y == 1u << 20 || sp_has(c->addr) || sp_has(c->addr + 16)) g_cond_off_samples++;
        else g_cond_off_mem++;
    }
    return on;
}
void mtl_sub_counter(uint64_t addr, uint32_t type) {
    g_counter_types[type & 31]++;
    if (type == NVN_COUNTER_TYPE_SAMPLES_PASSED) { sp_note(addr); (void)occl_mode(); }   // (the setting is read and logged here first)
    cpend_push(addr, g_fence + 1, 0, type != NVN_COUNTER_TYPE_SAMPLES_PASSED);   // the counter word run_posts will write
    post_op o = {2, NULL, 0, 0, addr, type};
    if (type == NVN_COUNTER_TYPE_SAMPLES_PASSED && oq_on()) {   // [UIT 0050] which queries it sums
        int carry = 0;
        if (oq_report(g_oqw, &o.qa, &o.qb, &carry)) { o.oq = carry ? 2 : 1; g_oq_counted++; }
        else g_oq_uncounted++;
    }
    post(o);
}

// Everything before is visible to everything after (step 1: transfers only; passes get precise barriers later).
static void full_barrier(VkCommandBuffer cb) {
    VkMemoryBarrier2 mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
        .srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT, .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
        .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT};
    VkDependencyInfo di = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &mb};
    vk.vkCmdPipelineBarrier2(cb, &di);
}

// TOTK_WATCH=<address>:<bytes>: GPU-side writes into that range (buffer copies, inline uniform updates) are logged too.
static int watched(uint64_t dst, uint64_t n) {
    static uint64_t lo, hi;
    static int init;
    if (!init) {
        init = 1;
        const char *e = getenv("TOTK_WATCH");
        if (e) { char *end; lo = strtoull(e, &end, 0); hi = *end == ':' ? lo + strtoull(end + 1, NULL, 0) : lo; }
    }
    return hi > lo && dst < hi && lo < dst + n;
}

void mtl_sub_copy(uint64_t src, uint64_t dst, uint64_t n) {
    vkb_ucap_note_copy(src, dst, n);   // [UIT 0335] (one recording thread; with several the walk did)
    if (watched(dst, n)) {
        static int said;
        if (said++ < 32) VLOG("[watch] present %llu: GPU copy %#llx -> %#llx (%llu bytes)", (unsigned long long)vkb_frame(),
                              (unsigned long long)src, (unsigned long long)dst, (unsigned long long)n);
    }
    if (g_ok != 1 || !n) { if (g_ok != 1 && n) memmove((void *)(uintptr_t)dst, (const void *)(uintptr_t)src, n); return; }
    pass_end_why(PE_COPY);
    vkb_mirrors_evict(src, n);   // ranges that straddle a mirror: game memory first
    vkb_mirrors_evict(dst, n);
    vkb_pool_use(src, n, 0);
    vkb_pool_use(dst, n, 1);
    VkBuffer sb, db; VkDeviceSize so, dof;
    // the source as of the submit call ([UIT 0157] looked up before the write is recorded: a copy within one range reads
    // the bytes from before it, as the GPU does; 0156 dropped first, so such a source read memory)
    int from_cap = vkb_ucap_range(src, n, &sb, &so);
    if (watched(dst, n)) {
        float cv[4] = {0}, mv[4] = {0};
        vkb_ucap_read(src + 0x30, cv, 16);
        memcpy(mv, (const void *)(uintptr_t)(src + 0x30), 16);
        static int said;
        if (vkb_frame() >= 330 && said++ < 64) VLOG("[watch] present %llu: copy %#llx -> %#llx from the capture %d (why %d); source +0x30 captured %g %g %g %g, memory now %g %g %g %g",
                              (unsigned long long)vkb_frame(), (unsigned long long)src, (unsigned long long)dst, from_cap, vkb_ucap_why(src, n), cv[0], cv[1], cv[2], cv[3], mv[0], mv[1], mv[2], mv[3]);
    }
    if ((!from_cap && !vkb_gpu_addr(src, n, &sb, &so)) || !vkb_gpu_addr(dst, n, &db, &dof)) {
        vkb_audit(A_COPY_SKIPPED, NULL);
        static int said; if (said++ < 8) VLOG("copy %#llx -> %#llx (%llu bytes) outside the memory pools: skipped",
                                              (unsigned long long)src, (unsigned long long)dst, (unsigned long long)n);
        vkb_ucap_drop(dst, n);
        return;
    }
    pass_end_why(PE_COPY);
    VkCommandBuffer cb = vkb_cmd();
    full_barrier(cb);
    VkBufferCopy r = {so, dof, n};
    vkb_checkpoint("copy buffer", NULL, NULL);
    vk.vkCmdCopyBuffer(cb, sb, db, 1, &r);
    vkb_ucap_gpu_write(cb, dst, n, NULL, sb, so);   // [UIT 0157] into the copy too (one recording thread), else dropped there
    full_barrier(cb);
}

static void update_buffer(uint64_t dst, uint64_t n, const uint8_t *data) {
    vkb_ucap_note_write(dst, n, data);   // [UIT 0335] (one recording thread; with several the walk did)
    if (watched(dst, n)) {
        static int said;
        float f[4] = {0};
        memcpy(f, data, n < 16 ? n : 16);
        if (said++ < 32) VLOG("[watch] present %llu: uniform update %#llx+%llu from command memory %p: %g %g %g %g",
                              (unsigned long long)vkb_frame(), (unsigned long long)dst, (unsigned long long)n,
                              (const void *)data, f[0], f[1], f[2], f[3]);
    }
    if (g_ok != 1) { memcpy((void *)(uintptr_t)dst, data, n); return; }
    if (!n) return;
    pass_end_why(PE_UPDATE);
    vkb_mirrors_evict(dst, n);
    vkb_pool_use(dst, n, 1);
    VkBuffer db; VkDeviceSize off;
    if (!vkb_gpu_addr(dst, n, &db, &off)) { vkb_ucap_drop(dst, n); return; }   // an inline uniform update: later draws read the updated memory
    VkCommandBuffer cb = vkb_cmd();
    full_barrier(cb);
    vkb_ucap_gpu_write(cb, dst, n, data, VK_NULL_HANDLE, 0);   // [UIT 0157] the update goes into the copy too (one recording thread), else dropped there
    // vkCmdUpdateBuffer: 4-byte aligned, at most 65536 bytes per call
    if (!(off & 3) && !(n & 3)) {
        for (uint64_t k = 0; k < n; k += 65536) vk.vkCmdUpdateBuffer(cb, db, off + k, n - k < 65536 ? n - k : 65536, data + k);
    } else {
        vkb_audit(A_UPDATE_SKIPPED, NULL);
        static int said; if (said++ < 4) VLOG("[temporary] unaligned uniform update %#llx+%llu: skipped", (unsigned long long)dst, (unsigned long long)n);
    }
    full_barrier(cb);
}

// ---------------- render passes (milestone 3.3 step 3) ----------------
// SetRenderTargets only records the targets; the pass (dynamic rendering) begins at the first command that renders and
// ends before anything that is not a draw or a clear (copies, other passes, the end of the submission).
static __thread p_set_rt g_rt;       // current render targets (per recording thread)
static _Atomic uint64_t g_ticket;   // presents so far
static __thread int g_in_pass;
static __thread uint32_t g_pass_fb;      // the current pass's feedback targets (vkb.h)
static __thread VkExtent2D g_pass_extent;

VkCommandBuffer vkb_cmd(void) { return t_cb ? t_cb : g_cur >= 0 ? g_slots[g_cur].cb : VK_NULL_HANDLE; }
int vkb_slot(void) { return g_cur; }
int vkb_in_pass(void) { return g_in_pass; }
const p_set_rt *vkb_rt(void) { return &g_rt; }
uint64_t vkb_frame(void) { return atomic_load(&g_ticket); }
int vkb_rt_count(void) { return g_rt.n < 0 ? 0 : g_rt.n > 8 ? 8 : g_rt.n; }
VkExtent2D vkb_rt_extent(void) { return g_pass_extent; }
int vkb_ready(void) { return g_ok == 1; }

// ---- TOTK_GPU_PASSPROF=<present>: GPU time of every render pass of that frame (a timestamp at its start and end; work
// between passes - copies, dispatches, barriers - as gaps). One timestamp pair per pass, not per draw: per-draw
// timestamps serialise the GPU and inflate small draws. The report names each pass by its targets, draws and first
// fragment shader, sorted by time (development).
enum { PP_MAX = 4096 };
typedef struct { uint32_t w, h, ncolor, fmt0, depth, draws; uint64_t draws0; char key[16]; } pp_info;
static VkQueryPool g_ppq;
static pp_info g_pp[PP_MAX];
static int g_ppn, g_pp_done, g_pp_open = -1;
static long long pp_frame(void) {
    static long long at = -1;
    if (at < 0) { const char *e = getenv("TOTK_GPU_PASSPROF"); at = e ? atoll(e) : 0; }
    return at;
}
static int pp_on(void) { return pp_frame() > 0 && !g_pp_done && vkb_frame() == (uint64_t)pp_frame(); }
void vkb_passprof_note(const char *key) {   // vkb_draw.c: the first fragment shader of the open pass
    if (g_pp_open >= 0 && !g_pp[g_pp_open].key[0] && key) snprintf(g_pp[g_pp_open].key, sizeof g_pp[0].key, "%.12s", key);
}
static void pp_begin(VkCommandBuffer cb, VkExtent2D ext) {
    if (!pp_on() || g_ppn >= PP_MAX / 2) return;
    if (!g_ppq) {
        VkQueryPoolCreateInfo ci = {VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, .queryType = VK_QUERY_TYPE_TIMESTAMP, .queryCount = PP_MAX};
        if (!vk_check(vk.vkCreateQueryPool(vkc.device, &ci, NULL, &g_ppq), "pass timestamp pool")) { g_pp_done = 1; return; }
        vk.vkResetQueryPool(vkc.device, g_ppq, 0, PP_MAX);
    }
    pp_info *p = &g_pp[g_ppn];
    memset(p, 0, sizeof *p);
    p->w = ext.width; p->h = ext.height;
    p->ncolor = (uint32_t)(g_rt.n < 0 ? 0 : g_rt.n);
    p->fmt0 = g_rt.n > 0 && g_rt.color[0] ? (uint32_t)g_rt.color[0]->d.format : 0;
    p->depth = g_rt.depth ? (uint32_t)g_rt.depth->d.format : 0;
    p->draws0 = vkb_draw_count();
    vk.vkCmdWriteTimestamp2(cb, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, g_ppq, (uint32_t)(2 * g_ppn));
    g_pp_open = g_ppn++;
}
static void pp_end(VkCommandBuffer cb) {
    if (g_pp_open < 0) return;
    g_pp[g_pp_open].draws = (uint32_t)(vkb_draw_count() - g_pp[g_pp_open].draws0);
    vk.vkCmdWriteTimestamp2(cb, VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT, g_ppq, (uint32_t)(2 * g_pp_open + 1));
    g_pp_open = -1;
}
static int pp_cmp(const void *a, const void *b) {
    double x = ((const double *)a)[0], y = ((const double *)b)[0];
    return x < y ? 1 : x > y ? -1 : 0;
}
static void pp_report(void) {   // at a present after the frame: wait for the GPU, read, print
    if (!g_ppn || g_pp_done || pp_frame() <= 0 || vkb_frame() <= (uint64_t)pp_frame()) return;
    g_pp_done = 1;
    mtl_finish();
    uint64_t *ts = malloc(sizeof *ts * 2 * (size_t)g_ppn);
    double (*row)[2] = malloc(sizeof *row * (size_t)g_ppn);
    if (!ts || !row) return;
    VkResult r = vk.vkGetQueryPoolResults(vkc.device, g_ppq, 0, (uint32_t)(2 * g_ppn), sizeof *ts * 2 * (size_t)g_ppn, ts, sizeof *ts,
                                          VK_QUERY_RESULT_64_BIT);
    if (r != VK_SUCCESS) { VLOG("pass profile: results %d", (int)r); return; }
    double per = vkc.props.limits.timestampPeriod / 1e6, inpass = 0, gaps = 0;
    for (int i = 0; i < g_ppn; i++) {
        row[i][0] = ts[2 * i + 1] > ts[2 * i] ? (double)(ts[2 * i + 1] - ts[2 * i]) * per : 0;
        row[i][1] = i;
        inpass += row[i][0];
        if (i && ts[2 * i] > ts[2 * i - 1]) gaps += (double)(ts[2 * i] - ts[2 * i - 1]) * per;
    }
    double span = (double)(ts[2 * g_ppn - 1] - ts[0]) * per;
    VLOG("pass profile of present %lld: %d passes, %.2f ms in passes, %.2f ms between them, %.2f ms first to last",
         pp_frame(), g_ppn, inpass, gaps, span);
    qsort(row, (size_t)g_ppn, sizeof *row, pp_cmp);
    for (int k = 0; k < g_ppn && k < 30; k++) {
        const pp_info *p = &g_pp[(int)row[k][1]];
        VLOG("  %7.3f ms  pass %4d  %ux%u  %u colour (fmt %#x) depth %#x  %u draws  fs %s", row[k][0], (int)row[k][1], p->w, p->h,
             p->ncolor, p->fmt0, p->depth, p->draws, p->key[0] ? p->key : "-");
    }
    free(ts); free(row);
}

static void pass_end(void) {
    if (!g_in_pass) return;
    vk.vkCmdEndRendering(vkb_cmd());
    pp_end(vkb_cmd());
    g_in_pass = 0;
}

static uint32_t level_size(uint32_t v, const view_t *view) {
    uint32_t base = view && view->set_levels ? view->base_level : 0, s = v >> base;
    return s ? s : 1;
}

// 0 when the targets cannot be rendered to (no image yet for a format). Without any target the pass has no attachments
// and covers `bare` (the draw's viewport): TOTK draws with no targets whose fragment shaders write storage buffers (the
// ambient light from the environment cube map goes to the scene buffer that the lighting reads).
// fb: targets that the pass's draws also sample (vkb.h): TOTK's G-buffer decals read the G-buffer they draw into and
// effects read the depth they test against, as on NVN where a bound target may be sampled.
static __thread uint64_t g_pass_serial;   // passes begun on this thread (vkb_pass_serial: per-pass state of the draws)
uint64_t vkb_pass_serial(void) { return g_pass_serial; }
static void shadow_presync(void);
static int pass_begin(VkExtent2D bare, uint32_t fb) {
    if (g_in_pass) return 1;
    shadow_presync();
    g_pass_serial++;
    if (!vkc.has_feedback_loop) fb = 0;
    g_pass_fb = fb;
    VkCommandBuffer cb = vkb_cmd();
    if (vkb_stores_take()) {   // storage writes of earlier draws are visible to this pass (vkb_draw.c g_stores_pending)
        VkMemoryBarrier2 mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
            .srcStageMask = VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
            .srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
            .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
            .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT};
        VkDependencyInfo dep = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &mb};
        vk.vkCmdPipelineBarrier2(cb, &dep);
    }
    VkRenderingAttachmentInfo color[8] = {0}, depth = {0}, stencil = {0};
    uint32_t w = UINT32_MAX, h = UINT32_MAX, layers = 1;
    int ncolor = g_rt.n < 0 ? 0 : g_rt.n > 8 ? 8 : g_rt.n, any = 0;
    for (int i = 0; i < ncolor; i++) {
        color[i].sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        vkb_image *im = g_rt.color[i] ? vkb_image_of(g_rt.color[i]) : NULL;
        const view_t *v = g_rt.has_cview[i] ? &g_rt.cview[i] : NULL;
        if (im && im->refresh_from) vkb_tex_refresh(cb, im);   // newer contents at this memory in another size (not in a pass yet)
        // [UIT 0125] a compressed image is attached through an uncompressed view of its blocks (vkb_view); it has no
        // feedback-loop usage, so it cannot also be sampled by the pass
        int blocks = im && im->compressed;
        if (!im || im->aspect != VK_IMAGE_ASPECT_COLOR_BIT || !vkb_view(im, v, 1, -1) || (blocks && ((fb >> i) & 1))) {
            if (im) vkb_audit(A_RT_NOT_ATTACHABLE, NULL);
            if (im) { static int said; if (said++ < 8) VLOG("[temporary] texture %llu (format %#x, view format %#x) as a render target: not attachable%s",
                                                            (unsigned long long)im->serial, im->d.format, v && v->set_format ? v->format : 0,
                                                            blocks ? " (compressed)" : ""); }
            continue;
        }
        int loop = (fb >> i) & 1;
        VkImageLayout layout = loop ? VK_IMAGE_LAYOUT_ATTACHMENT_FEEDBACK_LOOP_OPTIMAL_EXT : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        vkb_image_use(cb, im, layout, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT | (loop ? VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT : 0),
                      VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | (loop ? VK_ACCESS_2_SHADER_SAMPLED_READ_BIT : 0));
        color[i].imageView = vkb_view(im, v, 1, -1);
        vkb_image_written_by(im, VKB_W_COLOR, (uint64_t)i);
        color[i].imageLayout = layout;
        color[i].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        color[i].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        uint32_t lw = level_size(im->width, v), lh = level_size(im->height, v);
        if (blocks) { lw = (lw + 3) / 4; lh = (lh + 3) / 4; }   // [UIT 0125] the view's texels are 4x4 blocks
        if (lw < w) w = lw;
        if (lh < h) h = lh;
        any = 1;
    }
    vkb_image *dim = g_rt.depth ? vkb_image_of(g_rt.depth) : NULL;
    if (dim && dim->refresh_from) vkb_tex_refresh(cb, dim);
    if (dim && (dim->aspect & (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT))) {
        const view_t *v = g_rt.has_dview ? &g_rt.dview : NULL;
        int loop = (fb & VKB_FB_DEPTH) != 0;
        VkImageLayout layout = loop ? VK_IMAGE_LAYOUT_ATTACHMENT_FEEDBACK_LOOP_OPTIMAL_EXT : VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        vkb_image_use(cb, dim, layout,
                      VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT | (loop ? VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT : 0),
                      VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | (loop ? VK_ACCESS_2_SHADER_SAMPLED_READ_BIT : 0));
        VkRenderingAttachmentInfo a = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = vkb_view(dim, v, 1, -1),
            .imageLayout = layout, .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD,
            .storeOp = VK_ATTACHMENT_STORE_OP_STORE};
        vkb_image_written_by(dim, VKB_W_DEPTH, 0);
        if (dim->aspect & VK_IMAGE_ASPECT_DEPTH_BIT) depth = a;
        if (dim->aspect & VK_IMAGE_ASPECT_STENCIL_BIT) stencil = a;
        uint32_t lw = level_size(dim->width, v), lh = level_size(dim->height, v);
        if (lw < w) w = lw;
        if (lh < h) h = lh;
        any = 1;
    }
    if (!any) {
        if (ncolor || !bare.width || !bare.height) return 0;   // targets that cannot be attached: not drawn
        w = bare.width < vkc.props.limits.maxFramebufferWidth ? bare.width : vkc.props.limits.maxFramebufferWidth;
        h = bare.height < vkc.props.limits.maxFramebufferHeight ? bare.height : vkc.props.limits.maxFramebufferHeight;
    }
    g_pass_extent = (VkExtent2D){w, h};
    VkRenderingInfo ri = {VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = {{0, 0}, {w, h}}, .layerCount = layers,
        .colorAttachmentCount = (uint32_t)ncolor, .pColorAttachments = color,
        .pDepthAttachment = depth.imageView ? &depth : NULL, .pStencilAttachment = stencil.imageView ? &stencil : NULL};
    vkb_checkpoint("begin pass", NULL, NULL);
    pp_begin(cb, g_pass_extent);
    vk.vkCmdBeginRendering(cb, &ri);
    g_in_pass = 1;
    return 1;
}

// TOTK_VK_TRACE=<n>[@<present>]: the first n render-target and clear commands (from that present on), with their
// textures (development).
static long trace_left(void) {
    static long left = -1;
    static unsigned long long from;
    if (left < 0) {
        const char *e = getenv("TOTK_VK_TRACE");
        left = e ? atol(e) : 0;
        from = e && strchr(e, '@') ? strtoull(strchr(e, '@') + 1, NULL, 10) : 0;
    }
    if (atomic_load(&g_ticket) < from) return 0;
    return left > 0 ? left-- : 0;
}
static void trace_tex(const char *what, int i, const tex_t *t, const view_t *v) {
    if (t) VLOG("  %s %d: texture %llu format %#x target %d %ux%ux%u levels %u%s layers %u+%u level %u format %#x @%p", what, i,
                (unsigned long long)t->serial, t->d.format, t->d.target, t->d.width, t->d.height, t->d.depth, t->d.levels,
                v ? " | view" : "", v && v->set_layers ? v->min_layer : 0, v && v->set_layers ? v->nlayers : 0,
                v && v->set_levels ? v->base_level : 0, v && v->set_format ? v->format : 0u, (const void *)t->mem);
}
static void trace_rt(void) {
    if (!trace_left()) return;
    VLOG("[trace] SetRenderTargets %d colour + %s", g_rt.n, g_rt.depth ? "depth" : "no depth");
    for (int i = 0; i < g_rt.n && i < 8; i++) trace_tex("colour", i, g_rt.color[i], g_rt.has_cview[i] ? &g_rt.cview[i] : NULL);
    trace_tex("depth", 0, g_rt.depth, g_rt.has_dview ? &g_rt.dview : NULL);
}

int vkb_pass_begin(VkExtent2D bare, uint32_t fb) { return pass_begin(bare, fb); }
uint32_t vkb_pass_fb(void) { return g_in_pass ? g_pass_fb : 0; }
vkb_cnt vkb_pass_end_n[PE_COUNT];   // passes ended, by reason (the per-frame log line)
static void pass_end_why(int why) { if (g_in_pass) vkb_cnt_add(vkb_pass_end_n[why], 1); pass_end(); }
void vkb_pass_end_r(int why) { pass_end_why(why); }
void vkb_pass_end(void) { pass_end_why(PE_OTHER); }

static void clear_color(const p_clear_color *c) {
    if (trace_left()) {
        VkRect2D s = {0};
        int has = vkb_scissor(&s);
        float v[4] = {0};
        int hv = vkb_viewport0(v);
        VLOG("[trace] ClearColor index %d mask %#x rgba %g %g %g %g scissor %s %d,%d %ux%u viewport %s %g,%g %gx%g", c->index, c->mask, c->rgba[0],
             c->rgba[1], c->rgba[2], c->rgba[3], has ? "set" : "none", s.offset.x, s.offset.y, s.extent.width, s.extent.height, hv ? "set" : "none",
             v[0], v[1], v[2], v[3]);
    }
    if (c->index < 0 || c->index >= 8 || c->index >= g_rt.n || !g_rt.color[c->index]) return;
    static int skip = -2;   // TOTK_VK_SKIP_CLEAR=<index>: colour clears of that target index are not replayed (development A/B)
    if (skip == -2) { const char *e = getenv("TOTK_VK_SKIP_CLEAR"); skip = e ? atoi(e) : -1; }
    if (c->index == skip) return;
    if ((c->mask & 0xF) != 0xF) {   // [temporary] per-channel masked clears need a draw (step 5)
        vkb_audit(A_CLEAR_MASKED, NULL);
        static int said; if (!said++) VLOG("[temporary] clear with channel mask %#x: not replayed yet", c->mask);
        if (!(c->mask & 0xF)) return;
    }
    if (!pass_begin((VkExtent2D){0, 0}, 0)) return;
    VkClearAttachment a = {VK_IMAGE_ASPECT_COLOR_BIT, (uint32_t)c->index, {.color = {{c->rgba[0], c->rgba[1], c->rgba[2], c->rgba[3]}}}};
    vkb_image *im = vkb_image_of(g_rt.color[c->index]);
    if (vkb_watch_on && im && vkb_watch_match(im))   // [UIT 0342]
        vkb_watch_note(im, "clear as render target %d: rgba %g %g %g %g, channel mask %#x (replayed whole: %ux%u)", c->index, c->rgba[0], c->rgba[1],
                       c->rgba[2], c->rgba[3], c->mask, g_pass_extent.width, g_pass_extent.height);
    const nvn_texdesc *d = im ? &im->d : NULL;
    int vf = !d ? 0 : g_rt.has_cview[c->index] && g_rt.cview[c->index].set_format ? g_rt.cview[c->index].format : (int)d->format;   // [UIT 0125] the attached view's
    if (vf == 0x27 || vf == 0x28 || vf == 0x2C || vf == 0x2D || vf == 0x2F || vf == 0x30)
        for (int k = 0; k < 4; k++) a.clearValue.color.uint32[k] = (uint32_t)c->rgba[k];   // integer targets (NVN takes float values)
    VkClearRect r = {{{0, 0}, g_pass_extent}, 0, 1};
    vkb_checkpoint("clear attachment", NULL, NULL);
    vk.vkCmdClearAttachments(vkb_cmd(), 1, &a, 1, &r);
}

static void clear_ds(const p_clear_ds *c) {
    if (!g_rt.depth || !pass_begin((VkExtent2D){0, 0}, 0)) return;
    vkb_image *im = vkb_image_of(g_rt.depth);
    if (!im) return;
    VkImageAspectFlags asp = 0;
    if ((im->aspect & VK_IMAGE_ASPECT_DEPTH_BIT) && c->depth_mask) asp |= VK_IMAGE_ASPECT_DEPTH_BIT;
    if ((im->aspect & VK_IMAGE_ASPECT_STENCIL_BIT) && (c->stencil_mask & 0xFF)) asp |= VK_IMAGE_ASPECT_STENCIL_BIT;
    if (!asp) return;
    if ((asp & VK_IMAGE_ASPECT_STENCIL_BIT) && (c->stencil_mask & 0xFF) != 0xFF) {
        static int said; if (!said++) VLOG("[temporary] stencil clear with write mask %#x: cleared fully", c->stencil_mask);
    }
    VkClearAttachment a = {asp, 0, {.depthStencil = {c->depth, (uint32_t)c->stencil}}};
    VkClearRect r = {{{0, 0}, g_pass_extent}, 0, 1};
    vkb_checkpoint("clear attachment", NULL, NULL);
    vk.vkCmdClearAttachments(vkb_cmd(), 1, &a, 1, &r);
}

// ClearTexture: a region of one texture outside a pass
static void clear_tex(const p_clear_tex *c) {
    pass_end_why(PE_CLEAR_TEX);
    vkb_image *im = vkb_image_of(c->tex);
    if (!im) return;
    VkCommandBuffer cb = vkb_cmd();
    const view_t *v = c->has_view ? &c->view : NULL;
    uint32_t level = v && v->set_levels ? v->base_level : 0, layer = v && v->set_layers ? v->min_layer : 0;
    uint32_t lw = im->width >> level, lh = im->height >> level;
    if (!lw) lw = 1;
    if (!lh) lh = 1;
    int full = c->r.xoffset == 0 && c->r.yoffset == 0 && (uint32_t)c->r.width >= lw && (uint32_t)c->r.height >= lh;
    if (!full || (c->mask & 0xF) != 0xF) {   // [temporary] partial clears need a pass with a scissor (step 5)
        static int said; if (said++ < 4) VLOG("[temporary] partial ClearTexture %dx%d of %ux%u mask %#x: not replayed yet",
                                              c->r.width, c->r.height, lw, lh, c->mask);
        char what[96];   // [UIT 0342]
        snprintf(what, sizeof what, "%dx%d at %d,%d of %ux%u, mask %#x", c->r.width, c->r.height, c->r.xoffset, c->r.yoffset, lw, lh, c->mask);
        vkb_image_skipped(im, full ? VKB_SKIP_MASKED_CLEAR : VKB_SKIP_PARTIAL_CLEAR, what);
        return;
    }
    vkb_image_use(cb, im, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    { uint64_t cbits; memcpy(&cbits, &c->c, sizeof cbits); vkb_image_written_by(im, VKB_W_CLEAR, cbits); }   // [UIT 0342] detail: the first two words
    uint32_t nlayers = c->r.depth > 0 ? (uint32_t)c->r.depth : 1;
    VkImageSubresourceRange range = {im->aspect, level, 1, layer + (uint32_t)c->r.zoffset, nlayers};
    if (im->aspect == VK_IMAGE_ASPECT_COLOR_BIT) {
        VkClearColorValue cv;
        memcpy(&cv, &c->c, sizeof cv);
        vkb_checkpoint("clear texture", NULL, NULL);
        vk.vkCmdClearColorImage(cb, im->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &cv, 1, &range);
    } else {
        VkClearDepthStencilValue dv = {c->c.f[0], c->c.u[1]};
        vkb_checkpoint("clear depth texture", NULL, NULL);
        vk.vkCmdClearDepthStencilImage(cb, im->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &dv, 1, &range);
    }
}

void mtl_tex_release(tex_t *t) { vkb_tex_release(t); }

// ---------------- presentation ----------------

// TOTK_SHOT=<present number>:<file.bmp>: saves that presented frame (development check of the picture).
static VkBuffer g_shot_buf;
static VkDeviceMemory g_shot_mem;
static VkBuffer shot_record(VkCommandBuffer cb, uint64_t n, VkImage out, uint32_t w, uint32_t h) {
    static long long at = -1;
    if (at == -1) { const char *e = getenv("TOTK_SHOT"); at = e ? atoll(e) : 0; }
    if (at <= 0 || (uint64_t)at != n) return VK_NULL_HANDLE;
    VkBufferCreateInfo bi = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = (VkDeviceSize)w * h * 4,
                             .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    if (!vk_check(vk.vkCreateBuffer(vkc.device, &bi, NULL, &g_shot_buf), "shot buffer")) return VK_NULL_HANDLE;
    VkMemoryRequirements req;
    vk.vkGetBufferMemoryRequirements(vkc.device, g_shot_buf, &req);
    uint32_t type = UINT32_MAX;
    for (uint32_t i = 0; i < vkc.memory.memoryTypeCount && type == UINT32_MAX; i++)
        if ((req.memoryTypeBits & (1u << i)) && (vkc.memory.memoryTypes[i].propertyFlags &
            (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) == (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))
            type = i;
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = req.size, .memoryTypeIndex = type};
    if (type == UINT32_MAX || !vk_check(vk.vkAllocateMemory(vkc.device, &ai, NULL, &g_shot_mem), "shot memory")) return VK_NULL_HANDLE;
    vk.vkBindBufferMemory(vkc.device, g_shot_buf, g_shot_mem, 0);
    VkBufferImageCopy r = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageExtent = {w, h, 1}};
    vk.vkCmdCopyImageToBuffer(cb, out, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, g_shot_buf, 1, &r);
    return g_shot_buf;
}
static void shot_write(uint64_t fence, uint32_t w, uint32_t h) {
    const char *e = getenv("TOTK_SHOT"), *path = e ? strchr(e, ':') : NULL;
    mtl_wait(fence, UINT64_MAX);
    void *px = NULL;
    if (path && vk.vkMapMemory(vkc.device, g_shot_mem, 0, VK_WHOLE_SIZE, 0, &px) == VK_SUCCESS) {
        FILE *f = fopen(path + 1, "wb");
        if (f) {   // 32-bit BGRA, top-down
            uint32_t size = w * h * 4;
            uint8_t hdr[54] = {'B', 'M'};
            uint32_t v;
            v = 54 + size; memcpy(hdr + 2, &v, 4);
            v = 54; memcpy(hdr + 10, &v, 4);
            v = 40; memcpy(hdr + 14, &v, 4);
            memcpy(hdr + 18, &w, 4);
            int32_t nh = -(int32_t)h; memcpy(hdr + 22, &nh, 4);
            hdr[26] = 1; hdr[28] = 32;
            memcpy(hdr + 34, &size, 4);
            fwrite(hdr, 1, sizeof hdr, f);
            fwrite(px, 1, size, f);
            fclose(f);
            VLOG("frame saved to %s (%ux%u)", path + 1, w, h);
        }
        vk.vkUnmapMemory(vkc.device, g_shot_mem);
    }
    vk.vkDestroyBuffer(vkc.device, g_shot_buf, NULL);
    vk.vkFreeMemory(vkc.device, g_shot_mem, NULL);
    g_shot_buf = VK_NULL_HANDLE;
}
// TOTK_DUMP=<present number>:<dir>: the frame's render targets (development); TOTK_DUMP_EXIT=1 then exits
static void dump_check(uint64_t n) {
    static long long dump_at = -1;
    static const char *dump_dir;
    if (dump_at == -1) { const char *e = getenv("TOTK_DUMP"); dump_at = e ? atoll(e) : 0; dump_dir = e && strchr(e, ':') ? strchr(e, ':') + 1 : NULL; }
    if (dump_at > 0 && n >= (uint64_t)dump_at && dump_dir) {   // present numbers can skip: the first at or after it
        dump_at = -2;
        vkb_tex_dump(dump_dir);
        if (getenv("TOTK_DUMP_EXIT")) { fflush(NULL); _Exit(0); }   // short development runs: done after the dump
    }
}

static void occl_log(uint64_t n, uint64_t every);   // [UIT 0052]
uint64_t mtl_present(const tex_t *t, double period_s, const int crop[4]) {
    (void)period_s;
    uint64_t n = atomic_fetch_add(&g_ticket, 1) + 1;
    vkb_capture_present(n);   // [UIT 0157] Shift+F7 / the overlay: the frame starting now is captured (vkb_draw.c)
    {   // [UIT 0052] TOTK_VK_OCCL_LOG_EVERY=<presents> (default 60, 0 = off): the short [occl] line, to find the moment grass vanishes
        static long every = -1;
        if (every < 0) { const char *e = getenv("TOTK_VK_OCCL_LOG_EVERY"); every = e ? atol(e) : 60; if (every < 0) every = 0; }
        if (occl_log_due(n, (uint64_t)every)) occl_log(n, (uint64_t)every);
    }
    if (n == 1 || n % 300 == 0) {
        uint64_t sl, sm;
        vkb_shader_stats(&sl, &sm);
        uint64_t dn, dsk;
        vkb_draw_stats(&dn, &dsk);
        VLOG("present %llu: shaders loaded %llu, without translation %llu; draws %llu, skipped %llu", (unsigned long long)n,
             (unsigned long long)sl, (unsigned long long)sm, (unsigned long long)dn, (unsigned long long)dsk);
        static uint64_t pb, pi, pp, pd, pn;
        if (n > pn) {
            double f = (double)(n - pn);
            static uint64_t pparts, pgpu, pwait;
            VLOG("memory pools: %d imported, %.1f MiB; deleted %llu (%llu after the submission being recorded, %llu imports waited for one), "
                 "objects waiting to be destroyed %llu", g_npools, (double)g_pool_bytes / 1048576.0,
                 (unsigned long long)(__atomic_load_n(&g_pool_now, __ATOMIC_RELAXED) + __atomic_load_n(&g_pool_deferred, __ATOMIC_RELAXED)),
                 (unsigned long long)__atomic_load_n(&g_pool_deferred, __ATOMIC_RELAXED), (unsigned long long)__atomic_load_n(&g_pool_waits, __ATOMIC_RELAXED),
                 (unsigned long long)vkd_waiting(&g_dq));
            {
                static uint64_t pct[32];
                char cl[256]; int cn = 0;
                for (int k = 0; k < 32; k++) if (g_counter_types[k] > pct[k]) { cn += snprintf(cl + cn, sizeof cl - (size_t)cn, " type %d: %.1f", k, (double)(g_counter_types[k] - pct[k]) / f); pct[k] = g_counter_types[k]; }
                if (cn) VLOG("per frame: counter reports by type:%s", cl);
                static uint64_t prt[32];   // [UIT 0049]
                cn = 0;
                for (int k = 0; k < 32; k++) if (g_reset_types[k] > prt[k]) { cn += snprintf(cl + cn, sizeof cl - (size_t)cn, " type %d: %.1f", k, (double)(g_reset_types[k] - prt[k]) / f); prt[k] = g_reset_types[k]; }
                if (cn) VLOG("per frame: counter resets by type:%s", cl);
            }
            {
                static uint64_t pc, pb, pn, ph, pd;
                uint64_t c, b, ns, h, d;
                vkb_ucap_stats(&c, &b, &ns, &h, &d);
                VLOG("per frame: uniforms copied at submit %.2f MiB in %.0f submissions (%.2f ms on the game thread), bindings from the copy %.0f, dropped by GPU writes %.1f",
                     (double)(b - pb) / 1048576.0 / f, (double)(c - pc) / f, (double)(ns - pn) / 1e6 / f, (double)(h - ph) / f, (double)(d - pd) / f);
                pc = c; pb = b; pn = ns; ph = h; pd = d;
                static uint64_t pph[4], ppr;   // [UIT 0145]
                uint64_t ph4[4], pr;
                vkb_ucap_phase_stats(ph4, &pr);
                if (pr > ppr)
                    VLOG("per frame: the uniform capture's parts: walking the commands %.2f ms (%.0f ranges), vertex / index write checks %.2f ms, sort + merge %.2f ms, copying %.2f ms",
                         (double)(ph4[0] - pph[0]) / 1e6 / f, (double)(pr - ppr) / f, (double)(ph4[1] - pph[1]) / 1e6 / f,
                         (double)(ph4[2] - pph[2]) / 1e6 / f, (double)(ph4[3] - pph[3]) / 1e6 / f);
                for (int k = 0; k < 4; k++) pph[k] = ph4[k];
                ppr = pr;
                static uint64_t pgs, pgr, pgb, pgg;
                uint64_t gs, gr, gb, gg;
                vkb_ucap_geo_stats(&gs, &gr, &gb, &gg);
                if (gs > pgs) VLOG("per frame: vertex / index ranges copied at submit %.1f of %.1f (%.2f MiB), read from memory as GPU-written %.1f",
                                   (double)(gr - pgr) / f, (double)(gs - pgs) / f, (double)(gb - pgb) / 1048576.0 / f, (double)(gg - pgg) / f);
                pgs = gs; pgr = gr; pgb = gb; pgg = gg;
                static uint64_t pfr, pfo, pfa, pfs;   // [UIT 0157]
                uint64_t fr, fo, fa, fs;
                vkb_ucap_fine_stats(&fr, &fo, &fa, &fs);
                VLOG("per frame: [UIT 0157] GPU writes into the copy %.1f, drop records %.1f (ran out %.1f), bindings kept on the copy by "
                     "dropping only the written bytes %.1f", (double)(fa - pfa) / f, (double)(fr - pfr) / f, (double)(fo - pfo) / f, (double)(fs - pfs) / f);
                pfr = fr; pfo = fo; pfa = fa; pfs = fs;
                static uint64_t pgv[VKB_GV_N];   // [UIT 0336] the 0335 GPU-write log
                uint64_t gv[VKB_GV_N];
                vkb_ucap_gpuview_stats(gv);
                VLOG("per frame: [UIT 0336] GPU writes kept for CPU reads %.1f (%.1f KiB), not kept %.1f (%.1f KiB); CPU reads of uniform "
                     "memory %.1f, changed by a kept GPU write %.1f, under a write not kept %.1f",
                     (double)(gv[0] - pgv[0]) / f, (double)(gv[1] - pgv[1]) / 1024.0 / f, (double)(gv[2] - pgv[2]) / f,
                     (double)(gv[3] - pgv[3]) / 1024.0 / f, (double)(gv[4] - pgv[4]) / f, (double)(gv[5] - pgv[5]) / f,
                     (double)(gv[6] - pgv[6]) / f);
                for (int k = 0; k < VKB_GV_N; k++) pgv[k] = gv[k];
                static uint64_t pvp;   // [UIT 0337]
                uint64_t vp = vkb_vfetch_past();
                if (vp > pvp) VLOG("per frame: [UIT 0337] draws reading a vertex stream past its bound size %.1f", (double)(vp - pvp) / f);
                pvp = vp;
                static uint64_t puv[VKB_UV_N];
                uint64_t uv[VKB_UV_N];
                if (vkb_ubo_verify_stats(uv)) VLOG("per frame: [UIT 0157] uniform bindings %.1f: from the copy %.1f; memory %.1f (dropped by a GPU write %.1f, of them "
                     "differing from the copy outside the GPU's bytes %.1f; not in the copy %.1f, of them over 64 KiB %.1f; another slot %.1f; "
                     "misaligned %.1f)",
                     (double)(uv[0] - puv[0]) / f, (double)(uv[1] - puv[1]) / f, (double)(uv[0] - puv[0] - uv[1] + puv[1]) / f,
                     (double)(uv[2] - puv[2]) / f, (double)(uv[3] - puv[3]) / f, (double)(uv[4] - puv[4]) / f, (double)(uv[5] - puv[5]) / f,
                     (double)(uv[6] - puv[6]) / f, (double)(uv[7] - puv[7]) / f);
                for (int k = 0; k < VKB_UV_N; k++) puv[k] = uv[k];
            }
            {   // [UIT 0200] (vkb_view_stats resets its counts)
                uint64_t vmade, vdrop;
                vkb_view_stats(&vmade, &vdrop);
                if (vmade) VLOG("per frame: image views created %.1f, not kept (a texture's view cache full) %.1f (TOTK_VK_VIEW_KEY)",
                                (double)vmade / f, (double)vdrop / f);
            }
            {   // [UIT 0207]
                static uint64_t prev_one;
                uint64_t one = vkb_sampler_one_use();
                if (one != prev_one) VLOG("samplers made while the game re-registered them: %llu so far (TOTK_VK_SAMPLER_FREE)", (unsigned long long)one);
                prev_one = one;
            }
            if (vkb_shadow_on()) {
                static uint64_t pin, pib, pob, pdf, psc, pwp, pwk, pwl, pps, ppb;
                char vfl[160] = "";
                if (g_vf_every > 0) snprintf(vfl, sizeof vfl, "; verify: %llu checks, %llu pages compared, %llu stale (all)",
                                             (unsigned long long)g_vf_checks, (unsigned long long)g_vf_pages, (unsigned long long)g_vf_bad);
                VLOG("per frame: pool copies %.1f MiB of device memory; in %.0f copies %.2f MiB, back %.2f MiB, "
                     "left in game memory in a pass %.0f, write watch %.2f ms; pages copied back %.0f, left to CPU writes %.1f (GPU bytes not copied back there %.0f), CPU writes caught while copying back %.0f (all); "
                     "syncs over GPU writes not in game memory yet %.1f (%.3f MiB)%s", (double)vkb_shadow_bytes() / 1048576.0,
                     (double)(g_sh_in - pin) / f, (double)(g_sh_in_bytes - pib) / 1048576.0 / f, (double)(g_sh_out_bytes - pob) / 1048576.0 / f,
                     (double)(g_sh_deferred - pdf) / f, (double)(g_sh_scan_ns - psc) / 1e6 / f,
                     (double)(g_wb_pages - pwp) / f, (double)(g_wb_kept_pages - pwk) / f, (double)(g_wb_lost_bytes - pwl) / f, (double)nvn_dirty_gpu_raced(),
                     (double)(g_sh_pend_syncs - pps) / f, (double)(g_sh_pend_bytes - ppb) / 1048576.0 / f, vfl);
                pwp = g_wb_pages; pwk = g_wb_kept_pages; pwl = g_wb_lost_bytes; pps = g_sh_pend_syncs; ppb = g_sh_pend_bytes;
                pin = g_sh_in; pib = g_sh_in_bytes; pob = g_sh_out_bytes; pdf = g_sh_deferred; psc = g_sh_scan_ns;
            }
            if (g_m3_on > 0) {   // [UIT 0035] TOTK_VK_WB_MERGE3
                static uint64_t pb, pp, pns, psb;
                VLOG("per frame: three-way merge: GPU bytes recovered %.0f in %.1f pages; snapshots %.2f MiB, %.2f ms (TOTK_VK_WB_MERGE3)",
                     (double)(g_wb_m3_bytes - pb) / f, (double)(g_wb_m3_pages - pp) / f, (double)(g_m3_snap_bytes - psb) / 1048576.0 / f,
                     (double)(g_m3_snap_ns - pns) / 1e6 / f);
                pb = g_wb_m3_bytes; pp = g_wb_m3_pages; pns = g_m3_snap_ns; psb = g_m3_snap_bytes;
            }
            {   // [UIT 0034] TOTK_VK_PRESYNC_SUBS
                static uint64_t pe, pla;
                if (vkb_shadow_on())
                    VLOG("per frame: pools brought up to date at a submission's start %.1f, later from draws %.1f (TOTK_VK_PRESYNC_SUBS=%d)",
                         (double)(g_sh_early - pe) / f, (double)(g_sh_late - pla) / f, presync_subs());
                pe = g_sh_early; pla = g_sh_late;
                {   // [UIT 0249]
                    static uint64_t pj, pi, ps, ph, pn;
                    uint64_t j = atomic_load(&g_ps_jobs), it = atomic_load(&g_ps_items), sc = atomic_load(&g_ps_scanned), hp = atomic_load(&g_ps_helped), ns = atomic_load(&g_ps_ns);
                    if (j != pj) VLOG("per frame: [UIT 0249] write-watch scans before the pool copies, on all recording threads: %.1f pools (%.1f scanned there), %.3f ms, recording threads joined %.1f times (TOTK_VK_PRESYNC_PARALLEL)",
                                      (double)(it - pi) / f, (double)(sc - ps) / f, (ns - pn) / 1e6 / f, (double)(hp - ph) / f);
                    pj = j; pi = it; ps = sc; ph = hp; pn = ns;
                }
            }
            double ga, gw; uint64_t gn; gpu_frames_recent(300, &ga, &gw, &gn);
            VLOG("per frame: GPU %.1f ms (whole submissions), recording waited %.1f ms for a free command slot; "
                 "GPU per game frame: %.1f ms average, %.1f ms worst (%llu frames measured)",
                 (double)(g_gpu_ns - pgpu) / 1e6 / f, (double)(g_slot_wait_ns - pwait) / 1e6 / f, ga, gw, (unsigned long long)gn);
            vkb_mem_log();   // [UIT native] TOTK_VK_MEMLOG=1: video memory budget and process memory
            pgpu = g_gpu_ns; pwait = g_slot_wait_ns;
            static uint64_t pl[LK_COUNT], pn[LK_COUNT];
            uint64_t l0 = atomic_load(&vkb_lock_wait_ns[0]), l1 = atomic_load(&vkb_lock_wait_ns[1]), l2 = atomic_load(&vkb_lock_wait_ns[2]);
            VLOG("per frame: lock waits (all threads) texture registry %.2f ms / %.0f, images %.2f ms / %.0f, pools %.2f ms / %.0f",
                 (double)(l0 - pl[0]) / 1e6 / f, (double)(atomic_load(&vkb_lock_waits[0]) - pn[0]) / f,
                 (double)(l1 - pl[1]) / 1e6 / f, (double)(atomic_load(&vkb_lock_waits[1]) - pn[1]) / f,
                 (double)(l2 - pl[2]) / 1e6 / f, (double)(atomic_load(&vkb_lock_waits[2]) - pn[2]) / f);
            for (int k = 0; k < LK_COUNT; k++) { pl[k] = atomic_load(&vkb_lock_wait_ns[k]); pn[k] = atomic_load(&vkb_lock_waits[k]); }
            {   // [UIT 0230] scans for images with the images lock held (TOTK_VK_IMAGE_INDEX): count, images looked at, time
                static uint64_t pn5[5], pv5[5], pt5[5];
                uint64_t n5[5], v5[5], t5[5];
                vkb_tex_scan_stats(n5, v5, t5);
                static const char *const nm[5] = {"first uploads", "buffer reads", "copy-back checks", "copy-back picks", "invalidations"};
                char b[512]; int o = 0;
                for (int k = 0; k < 5; k++) {
                    o += snprintf(b + o, sizeof b - (size_t)o, "%s%s %.1f / %.0f images / %.3f ms", k ? ", " : "", nm[k], (double)(n5[k] - pn5[k]) / f,
                                  (double)(v5[k] - pv5[k]) / f, (double)(t5[k] - pt5[k]) / 1e6 / f);
                    if (o >= (int)sizeof b) o = (int)sizeof b - 1;
                    pn5[k] = n5[k]; pv5[k] = v5[k]; pt5[k] = t5[k];
                }
                VLOG("per frame: image scans with the images lock held (TOTK_VK_IMAGE_INDEX): %s", b);
            }
            {   // [UIT 0232] the images lock by site (TOTK_CRITPATH)
                int vkb_tex_lock_line(char *b, int cap, double f);
                char b[1536];
                if (vkb_tex_lock_line(b, (int)sizeof b, f) > 0) VLOG("per frame: images lock by site (taken / waited / held, all threads): %s", b);
            }
            {   // [UIT 0231]
                static uint64_t pi, pv, pl0;
                uint64_t ui, uv, ul;
                vkb_tex_unlocked_stats(&ui, &uv, &ul);
                if (ui != pi || uv != pv)
                    VLOG("per frame: made without the images lock (TOTK_VK_IMAGE_CREATE_UNLOCKED): images %.2f, views %.2f, thrown away (another thread was first) %.2f",
                         (double)(ui - pi) / f, (double)(uv - pv) / f, (double)(ul - pl0) / f);
                pi = ui; pv = uv; pl0 = ul;
            }
            if (g_ptc_on > 0) {   // [UIT 0033] TOTK_VK_POOL_FASTPATH
                static uint64_t ph, pm;
                uint64_t h = 0, m = 0;
                for (int k = 0; k < PTC_SLOTS; k++) { h += __atomic_load_n(&g_ptc_n[k].hits, __ATOMIC_RELAXED); m += __atomic_load_n(&g_ptc_n[k].misses, __ATOMIC_RELAXED); }
                VLOG("per frame: pool lookups without the lock %.0f, with it %.0f (TOTK_VK_POOL_FASTPATH)", (double)(h - ph) / f, (double)(m - pm) / f);
                ph = h; pm = m;
            }
            {   // [UIT 0240] TOTK_DIRTY_INDEX
                static uint64_t ph, pn, pw;
                uint64_t h, nn, w;
                nvn_dirty_index_stats(&h, &nn, &w);
                if (h != ph || nn != pn || w != pw)
                    VLOG("per frame: memory pool lookups through the index %.0f (no pool there %.0f), left to the slot search %.0f (TOTK_DIRTY_INDEX)",
                         (double)(h - ph) / f, (double)(nn - pn) / f, (double)(w - pw) / f);
                ph = h; pn = nn; pw = w;
            }
            static uint64_t ps[PK_COUNT], psn[PK_COUNT];
            VLOG("per frame: new images %.2f ms / %.1f, uploads %.2f ms / %.1f, queue submits %.2f ms / %.1f",
                 (double)(vkb_step_ns[0] - ps[0]) / 1e6 / f, (double)(vkb_step_n[0] - psn[0]) / f, (double)(vkb_step_ns[1] - ps[1]) / 1e6 / f,
                 (double)(vkb_step_n[1] - psn[1]) / f, (double)(vkb_step_ns[2] - ps[2]) / 1e6 / f, (double)(vkb_step_n[2] - psn[2]) / f);
            for (int k = 0; k < PK_COUNT; k++) { ps[k] = vkb_step_ns[k]; psn[k] = vkb_step_n[k]; }
            if (atomic_load(&g_async)) {   // [perf-mt] the queue submits above are the submit thread's now
                static uint64_t pl, pn;
                uint64_t l = atomic_load(&g_async_lag_ns), an = atomic_load(&g_async_n);
                VLOG("per frame: submit thread: a submission waited %.3f ms on average from recorded to vkQueueSubmit2, "
                     "%llu queued at most (TOTK_VK_ASYNC_SUBMIT=1)", an > pn ? (double)(l - pl) / 1e6 / (double)(an - pn) : 0.0,
                     (unsigned long long)vkb_subq_take_max_depth(&g_subq));
                pl = l; pn = an;
                static uint64_t pqw, pqn;   // [UIT 0248] waits for the queue lock (submit thread, present thread)
                uint64_t qw, qn;
                vk_queue_lock_stats(&qw, &qn);
                VLOG("per frame: queue lock waits %.3f ms (%.2f times), presents %s", (double)(qw - pqw) / 1e6 / f, (double)(qn - pqn) / f,
                     vkc.pqueue_own ? "on a queue of their own (TOTK_VK_PRESENT_QUEUE)" : "on the same queue (TOTK_VK_PRESENT_QUEUE=0 or one queue)");
                pqw = qw; pqn = qn;
            }
            {   // [UIT 0250] the retire thread's part of the game's fence wait, after the GPU is done
                static uint64_t prn, prc;
                uint64_t rn = atomic_load(&g_ret_ns), rc = atomic_load(&g_ret_n);
                VLOG("per frame: [UIT 0250] after the GPU finishes: %.3f ms copying GPU writes back before the game's fence wait returns "
                     "(%.2f submissions), retire thread %s", (double)(rn - prn) / 1e6 / f, (double)(rc - prc) / f,
                     __atomic_load_n(&g_ret_pc, __ATOMIC_RELAXED) > 0 ? "on the performance cores (TOTK_VK_RETIRE_PCORES)" : "where Windows puts it");
                prn = rn; prc = rc;
            }
            {   // ASTC as BC7 (vkb_bc7.c)
                static uint64_t ph, pmi, pe, pi, pens;
                uint64_t h, mi, en, dr, ens; size_t q;
                vkb_bc7_stats(&h, &mi, &en, &dr, &ens, &q);
                uint64_t vkb_bc7_images(void), bi = vkb_bc7_images();
                VLOG("per frame: ASTC as BC7: %.1f images, %.1f surfaces from the cache, %.1f not there, %.1f encoded in the background "
                     "(%.1f ms each), %.1f MiB queued, %llu dropped", (double)(bi - pi) / f, (double)(h - ph) / f, (double)(mi - pmi) / f,
                     (double)(en - pe) / f, en > pe ? (double)(ens - pens) / 1e6 / (double)(en - pe) : 0.0, q / 1048576.0, (unsigned long long)dr);
                ph = h; pmi = mi; pe = en; pi = bi; pens = ens;
            }
            vkb_tex_stream_log(f);   // texture uploads in detail, streamed mips (vkb_tex.c)
            static uint64_t pm, psh, ptb;
            uint64_t im_made, im_shared, im_back;
            vkb_tex_image_stats(&im_made, &im_shared, &im_back);
            VLOG("per frame: images made %.1f, shared with a live texture %.1f, taken back from the side %.1f; device memory allocations "
                 "alive %llu (images' own %llu; driver limit %u)",   // [UIT 0133]
                 (double)(im_made - pm) / f, (double)(im_shared - psh) / f, (double)(im_back - ptb) / f,
                 (unsigned long long)vkb_mem_live(), (unsigned long long)vkb_mem_dedicated_live(), vkc.props.limits.maxMemoryAllocationCount);
            pm = im_made; psh = im_shared; ptb = im_back;
            VLOG("per frame: %.0f draws, %.0f render passes, %.0f barriers (%.0f image layout changes), %.1f extra GPU submissions", (double)(dn - pd) / f,
                 (double)(vkb_cnt_sum(g_n_passes) - pp) / f, (double)(vkb_cnt_sum(g_n_barriers) - pb) / f,
                 (double)(vkb_cnt_sum(g_n_img_barriers) - pi) / f, (double)(g_parts - pparts) / f);
            pparts = g_parts;
            static const char *const why[PE_COUNT] = {"other", "set targets", "game barrier", "buffer copy", "uniform update", "texture clear",
                "end of submission", "after a storing draw", "snapshot", "sampled targets changed", "texture to prepare", "mirror", "dispatch", "texture copy", "pool copies"};
            static uint64_t prev[PE_COUNT];
            char line[512]; int at = 0;
            for (int k = 0; k < PE_COUNT; k++) {
                uint64_t ended = vkb_cnt_sum(vkb_pass_end_n[k]);
                if (ended > prev[k] && at < (int)sizeof line - 48)
                    at += snprintf(line + at, sizeof line - (size_t)at, " %s %.0f,", why[k], (double)(ended - prev[k]) / f);
                prev[k] = ended;
            }
            if (at) VLOG("passes ended per frame by:%s", line);
        }
        pb = vkb_cnt_sum(g_n_barriers); pi = vkb_cnt_sum(g_n_img_barriers); pp = vkb_cnt_sum(g_n_passes); pd = dn; pn = n;
    }
    vkb_tex_snap_flush();
    vkb_tex_lumlog(t, n);
    vkb_audit_present(n);
    vkb_capture_finish(n);   // [UIT 0342] the finished capture's image contents; the image-write watch's
    dump_check(n);
    vkb_gprof_report();
    pp_report();
    static int shot_wanted = -1;
    if (shot_wanted < 0) shot_wanted = getenv("TOTK_SHOT") || getenv("TOTK_DUMP");
    vkb_image *im = g_ok == 1 && (vkp_active() || shot_wanted) ? vkb_image_of(t) : NULL;   // headless: only for TOTK_SHOT
    if (!im) { vkp_frame(); return n; }
    VkImage out;
    int slot = vkp_out_acquire(im->width, im->height, &out);
    if (slot < 0) return n;   // the window is behind: this frame is dropped
    mtl_sub_begin();
    VkCommandBuffer cb = vkb_cmd();
    vkb_image_use(cb, im, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
    VkImageMemoryBarrier2 b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2, .srcStageMask = VK_PIPELINE_STAGE_2_NONE,
        .dstStageMask = VK_PIPELINE_STAGE_2_BLIT_BIT, .dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = out,
        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    VkDependencyInfo di = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &b};
    vk.vkCmdPipelineBarrier2(cb, &di);
    VkImageBlit r = {{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {{0, 0, 0}, {(int32_t)im->width, (int32_t)im->height, 1}},
                     {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {{0, 0, 0}, {(int32_t)im->width, (int32_t)im->height, 1}}};
    vk.vkCmdBlitImage(cb, im->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, out, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r, VK_FILTER_NEAREST);
    b.srcStageMask = VK_PIPELINE_STAGE_2_BLIT_BIT; b.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    b.dstStageMask = VK_PIPELINE_STAGE_2_BLIT_BIT; b.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    vk.vkCmdPipelineBarrier2(cb, &di);
    VkBuffer shot = shot_record(cb, n, out, im->width, im->height);
    // [perf-mt] TOTK_VK_ASYNC_SUBMIT=1: the submit thread says the frame is ready, after its submit (t_present_slot
    // cleared by mtl_sub_commit when it took it); else here, as before
    t_present_slot = slot; t_present_has_crop = crop != NULL;
    if (crop) memcpy(t_present_crop, crop, sizeof t_present_crop);
    uint64_t fence = mtl_sub_commit();
    int took = t_present_slot < 0;
    t_present_slot = -1;
    if (shot) shot_write(fence, im->width, im->height);   // waits for completion (so for the submit too)
    if (!took) vkp_out_ready(slot, g_timeline, fence, crop);
    return n;
}

// A long submission goes to the GPU in parts, split where the render targets change (the pass ends there anyway): the
// GPU starts on the first part while the rest is recorded. One part per NVN submission made the GPU wait for the whole
// recording: on the Great Sky Island ~20 ms of recording then ~26 ms of GPU time, one after the other (2026-10-24; as
// SavageWind found for BotW). Events and counters recorded in a part are signalled when that part completes.
// TOTK_VK_SPLIT=<draws> (default 0: one part per submission). Measured on the Great Sky Island without frame pacing: 45.6 ms
// a frame with one part, 49.4 ms with parts of 1000 draws (2026-10-24); with pool copies (GPU 12 ms, recording ~30 ms) 36.9 ms
// with parts of 1500 draws against 36-41 ms with one: no gain - off.
static uint64_t g_part_draws;   // vkb_draw_count() when the current part began
static void split_if_due(void) {
    static long long every = -1;
    if (every < 0) { const char *e = getenv("TOTK_VK_SPLIT"); every = e ? atoll(e) : 0; }
    if (g_ok != 1 || every <= 0 || g_cur < 0) return;
    uint64_t n = vkb_draw_count();
    if (n < g_part_draws) g_part_draws = n;
    if (n - g_part_draws < (uint64_t)every) return;
    mtl_sub_commit();
    mtl_sub_begin();
    g_part_draws = n;
    g_parts++;
}

static __thread int g_render_off;   // 1: SetRenderEnable(false); 2: a conditional that evaluated to disabled
// [UIT 0048] TOTK_VK_COND_DRAWS=1 (diagnostic): a conditional that evaluated to disabled still skips clears (TOTK gates
// its clears with them) but no longer draws: an A/B for draws (grass) wrongly skipped by a condition read from game
// memory at recording time (GPU writes of this or an unretired submission are not there yet). SetRenderEnable(false)
// still skips both. g_cond_forced counts the draws replayed only because of it (the "submission N" log line).
// [UIT 0052] TOTK_VK_COND_DRAWS=2: clears under such a conditional are replayed too (g_cond_forced_clears).
static vkb_cnt g_cond_forced, g_cond_forced_clears;
static int cond_draws(void) {
    static int on = -1;
    if (on < 0) {
        on = cond_mode_parse(getenv("TOTK_VK_COND_DRAWS"));
        if (on == 1) VLOG("[diagnostic] TOTK_VK_COND_DRAWS=1: draws are replayed whatever conditional rendering says (clears still follow it)");
        if (on == 2) VLOG("[diagnostic] TOTK_VK_COND_DRAWS=2: draws and clears are replayed whatever conditional rendering says");
    }
    return on;
}
static inline int draw_off(void) { return g_render_off && cond_skip(g_render_off, cond_draws(), 0); }
static inline int clear_off(void) {   // [UIT 0052] counts a skipped or forced clear
    if (!g_render_off) return 0;
    if (cond_skip(g_render_off, cond_draws(), 1)) { vkb_cnt_add(g_cond_skipped, 1); return 1; }
    vkb_cnt_add(g_cond_forced_clears, 1);
    return 0;
}
int vkb_render_suppressed(void) { return g_render_off; }

static const uint8_t *const *g_rest;
static int g_nrest;
void mtl_exec_rest(const uint64_t *h, int n) { g_rest = (const uint8_t *const *)h; g_nrest = n; }
void mtl_sub_index(const uint64_t *h, int n) { (void)h; (void)n; }

// The commands of a handle, in one of three ways: one thread replaying everything (EX_ONE), the walk of a submission
// before threads record it (EX_WALK: state, events and counters, conditionals, captured uniforms voided; cuts chunks),
// and a chunk on a recording thread (EX_CHUNK: up to the chunk's end, without events and counters). The position
// (t_seq) advances by one per command read, the same in all three.
enum { EX_ONE, EX_WALK, EX_CHUNK };
typedef struct mt_chunk mt_chunk;
static int walk_op(const uint8_t *p, int hidx);   // EX_WALK: a command at depth 0 (may cut a chunk before it)
static int exec_ops(const uint8_t *p, int depth, int mode, int hidx, const mt_chunk *c);
void mtl_exec(const uint8_t *p) {
    if (trace_left()) VLOG("[trace] command handle %p (submission %llu, present %llu)", (const void *)p, (unsigned long long)(g_fence + 1),
                           (unsigned long long)vkb_frame());
    exec_ops(p, 0, EX_ONE, 0, NULL);
}
static int chunk_end_at(const mt_chunk *c, int hidx, const uint8_t *p);
static uint32_t chunk_oq_n(const mt_chunk *c);   // [UIT 0050]
static void walk_command(uint32_t op, const uint8_t *a);
static int exec_ops(const uint8_t *p, int depth, int mode, int hidx, const mt_chunk *c) {   // 1: stopped at the chunk's end
    for (;;) {
        op_hdr h;
        memcpy(&h, p, sizeof h);
        if (h.size < 8 || (h.size & 7) || h.op >= OP_COUNT_) die("corrupt command record at %p: op %u size %u", (const void *)p, h.op, h.size);
        if (mode == EX_CHUNK && !depth && chunk_end_at(c, hidx, p)) return 1;
        if (mode == EX_WALK && !depth) walk_op(p, hidx);
        t_seq++;
        const uint8_t *a = p + 8;
        if (mode != EX_CHUNK) g_op_count[h.op]++;
        if (mode == EX_WALK) {
            if (h.op == OP_END) return 0;
            if (h.op == OP_JUMP) { const uint8_t *t; memcpy(&t, a, 8); p = t; continue; }
            if (h.op == OP_CALL) { const uint8_t *t; memcpy(&t, a, 8); exec_ops(t, depth + 1, mode, hidx, c); }
            else walk_command(h.op, a);
            p += h.size;
            continue;
        }
        switch (h.op) {
        case OP_END: return 0;
        case OP_BEGIN: break;
        case OP_JUMP: { const uint8_t *t; memcpy(&t, a, 8); p = t; continue; }
        case OP_CALL: { const uint8_t *t; memcpy(&t, a, 8); exec_ops(t, depth + 1, mode, hidx, c); break; }
        case OP_SIGNAL_EVENT: {   // a chunk: the walk posted it
            if (mode == EX_CHUNK) break;
            volatile uint32_t *val; uint32_t m, v;
            memcpy(&val, a, 8); memcpy(&m, a + 8, 4); memcpy(&v, a + 12, 4);
            mtl_sub_event(val, m, v);
            break;
        }
        case OP_REPORT_COUNTER: { if (mode == EX_CHUNK) break; uint64_t t, ad; memcpy(&t, a, 8); memcpy(&ad, a + 8, 8); mtl_sub_counter(ad, (uint32_t)t); break; }
        case OP_RESET_COUNTER: { if (mode == EX_CHUNK) break; uint64_t t; memcpy(&t, a, 8); counter_reset((uint32_t)t); break; }
        case OP_COPY_B2B: {
            uint64_t s, d, n;
            memcpy(&s, a, 8); memcpy(&d, a + 8, 8); memcpy(&n, a + 16, 8);
            mtl_sub_copy(s, d, n);
            break;
        }
        case OP_SET_RT: pass_end_why(PE_SET_RT); if (mode == EX_ONE) split_if_due(); memcpy(&g_rt, a, sizeof g_rt); trace_rt(); break;
        case OP_CLEAR_COLOR: if (!clear_off() && g_ok == 1) clear_color((const p_clear_color *)a); break;
        case OP_CLEAR_DS: if (!clear_off() && g_ok == 1) clear_ds((const p_clear_ds *)a); break;
        case OP_CLEAR_TEX: if (g_ok == 1) clear_tex((const p_clear_tex *)a); break;
        case OP_COPY_STRIDE: vkb_copy_stride(((const p_copy_stride *)a)->stride); break;
        case OP_COPY_B2T: if (g_ok == 1) vkb_copy_buf_tex((const p_copy_b2t *)a, 1); break;
        case OP_COPY_T2B: if (g_ok == 1) vkb_copy_buf_tex((const p_copy_b2t *)a, 0); break;
        case OP_COPY_T2T: if (g_ok == 1) vkb_copy_tex_tex((const p_copy_t2t *)a); break;
        case OP_BIND_PROGRAM: case OP_BIND_STATE: case OP_VIEWPORTS: case OP_SCISSOR: case OP_DEPTH_RANGE: case OP_STENCIL:
        case OP_POLY_OFFSET: case OP_BLEND_COLOR: case OP_BIND_VATTR: case OP_BIND_VSTREAM: case OP_BIND_VBUF: case OP_BIND_UBO:
        case OP_BIND_SSBO: case OP_BIND_TEX: case OP_BIND_IMG: case OP_DEPTH_CLAMP: case OP_RASTER_DISCARD: case OP_PRIM_RESTART:
        case OP_LINE_WIDTH:
            vkb_state(h.op, a);
            break;
        case OP_DRAW: case OP_DRAW_IDX:
            if (draw_off()) { vkb_cnt_add(g_cond_skipped, 1); break; }
            if (g_render_off) vkb_cnt_add(g_cond_forced, 1);   // [UIT 0048] off by a conditional, drawn anyway
            if (g_ok == 1) {
                t_oq = !oq_on() ? -1 : mode == EX_CHUNK ? oq_find(g_oqw, chunk_oq_n(c), t_seq) : oq_draw(g_oqw, t_seq);   // [UIT 0050]
                vkb_draw(a, h.op == OP_DRAW_IDX);
                t_oq = -1;
            }
            break;
        case OP_DISPATCH: if (g_ok == 1) vkb_dispatch(a); break;
        case OP_BARRIER:   // [temporary] everything before is visible to everything after
            if (g_ok == 1) { pass_end_why(PE_BARRIER); full_barrier(vkb_cmd()); }
            break;
        case OP_SET_TEX_POOL: case OP_SET_SAMP_POOL: case OP_BIND_SAMP: break;
        // rendering enable / conditional rendering: draws and clears are skipped while off (dispatches are not)
        case OP_RENDER_ENABLE: g_render_off = !((const p_flag *)a)->value; break;
        case OP_RENDER_COND:   // a chunk: as the walk evaluated it, in order (in the command's pad: 1 enabled, 2 disabled)
            g_render_off = (mode == EX_CHUNK ? ((const p_render_cond *)a)->pad == 2 : !cond_eval((const p_render_cond *)a)) ? 2 : 0;
            break;
        case OP_UPDATE_UB: { uint64_t d, n; memcpy(&d, a, 8); memcpy(&n, a + 8, 8); update_buffer(d, n, a + 16); break; }
        default:
            vkb_audit(A_OP_NOT_REPLAYED, NULL);
            if (!g_op_said[h.op]++) VLOG("command %u not replayed yet (steps 2-5 of milestone 3.3)", h.op);
            break;
        }
        p += h.size;
    }
}

// Threads recording (vkb_mt_submit): the submission's command buffers after the slot's own, the last one for the work at
// its end (copies back, the GPU end time).
enum { MT_MAXCH = 96 };
static VkCommandBuffer g_mt_list[2 * MT_MAXCH + 3];
static int g_mt_nlist;
static uint64_t g_early_fence;   // [UIT 0236] the fence early parts were queued under (mtl_sub_commit checks it)
static int g_slot_cb_sent;   // [native-1031] TOTK_VK_EARLY_PARTS: the slot's command buffer went with an early part (vkb_early.h)
uint64_t mtl_sub_commit(void) {
    if (g_ok == 1 && oq_on()) post((post_op){3, NULL, g_oqw->last_reset, g_oqw->n});   // [UIT 0050] last: after the reports
    uint64_t fence = __atomic_add_fetch(&g_fence, 1, __ATOMIC_SEQ_CST);
    post_op *ops = NULL;
    if (g_npost) {
        ops = malloc((size_t)g_npost * sizeof *ops);
        if (!ops) die("post ops");
        memcpy(ops, g_post, (size_t)g_npost * sizeof *ops);
    }
    int n = g_npost;
    g_npost = 0;
    if (g_ok != 1) {   // (no recording: g_recown untouched)
        run_posts(ops, n, -1); free(ops); complete(fence); open_end();
        if (atomic_load(&g_async)) {   // [perf-mt] (not after init: g_ok stays 1) the fence's turn, so the queue goes on
            sub_req *r = calloc(1, sizeof *r);
            if (!r) die("submit request");
            r->q.fence = fence; r->skip = 1; r->out_slot = -1;
            vkb_subq_push(&g_subq, &r->q);
        }
        return fence;
    }
    pass_end_why(PE_SUBMIT);
    sub_long_check(fence);
    VkCommandBuffer last = g_mt_nlist ? g_mt_list[g_mt_nlist - 1] : g_slots[g_cur].cb;
    int niw = 0;   // [UIT 0054] the images drawn in it (copied back if small, all CPU-read targets; else pending)
    vkb_iwb_rec *iw = vkb_iwb_mode() != IWB_OFF ? vkb_iwb_record(last, g_cur, 0, 0, 0, &niw) : NULL;
    vkb_mirrors_submit(last);   // written ranges back to game memory before the fence
    shadow_writeback(last);
    dead_tag(fence);
    gpu_time_end(g_cur, last);
    cmd_slot *s = &g_slots[g_cur];
    s->fence = fence;
    if (g_slot_cb_sent && fence != g_early_fence) {   // [UIT 0236] cannot happen: only the translation thread commits
        static int said;
        if (!said++) LOG("[UIT 0236] early parts were queued under submission %llu but it is %llu (please send this log)",
                         (unsigned long long)g_early_fence, (unsigned long long)fence);
    }
    if (!g_slot_cb_sent) vk.vkEndCommandBuffer(s->cb);
    if (g_mt_nlist) vk.vkEndCommandBuffer(last);
    VkCommandBufferSubmitInfo cbi[2 * MT_MAXCH + 4];
    int ncbi = 0;
    if (!g_slot_cb_sent)   // [native-1031] else already on the GPU with the first early part (before this one in queue order)
        cbi[ncbi++] = (VkCommandBufferSubmitInfo){VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO, .commandBuffer = s->cb};
    g_slot_cb_sent = 0;
    for (int i = 0; i < g_mt_nlist; i++)
        cbi[ncbi++] = (VkCommandBufferSubmitInfo){VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO, .commandBuffer = g_mt_list[i]};
    job *j = malloc(sizeof *j);
    if (!j) die("retire job");
    *j = (job){fence, ops, n, g_cur, g_wb_ranges, g_wb_nranges, NULL, g_vf_ranges, g_vf_nranges, g_wb_m3, iw, niw};   // g_wb_ranges: in the slot's copy back memory
    g_wb_m3 = NULL;
    g_wb_ranges = NULL; g_vf_ranges = NULL; g_vf_nranges = 0;
    if (atomic_load(&g_async)) {   // [perf-mt] TOTK_VK_ASYNC_SUBMIT=1: submit_thread submits it, then queues the job
        sub_req *rq = malloc(sizeof *rq + (size_t)ncbi * sizeof *cbi);
        if (!rq) die("submit request");
        *rq = (sub_req){.q = {.fence = fence, .t_push = runtime_now_ns()}, .j = j, .out_slot = t_present_slot, .ncbi = ncbi};
        if (t_present_slot >= 0) {   // mtl_present's frame: ready for the present thread once submitted
            rq->has_crop = t_present_has_crop;
            memcpy(rq->crop, t_present_crop, sizeof rq->crop);
            t_present_slot = -1;
        }
        memcpy(rq->cbi, cbi, (size_t)ncbi * sizeof *cbi);
        totk_tl(TL_COMMIT, fence, 1);
        vkb_subq_push(&g_subq, &rq->q);
    } else {
        VkSemaphoreSubmitInfo sig = {VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, .semaphore = g_timeline, .value = fence,
                                     .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT};
        VkSubmitInfo2 si = {VK_STRUCTURE_TYPE_SUBMIT_INFO_2, .commandBufferInfoCount = (uint32_t)ncbi, .pCommandBufferInfos = cbi,
                            .signalSemaphoreInfoCount = 1, .pSignalSemaphoreInfos = &sig};
        uint64_t ts0 = runtime_now_ns();
        vk_queue_lock();
        VkResult r = vk.vkQueueSubmit2(vkc.queue, 1, &si, VK_NULL_HANDLE);
        vk_queue_unlock();
        vkb_step_add(PK_SUBMIT, ts0);
        if (r != VK_SUCCESS) { vk_check(r, "vkQueueSubmit2"); die("GPU submission failed"); }
        totk_tl(TL_COMMIT, fence, 1);
        jobs_push(j);
    }
    g_cur = -1;
    vkb_recown_leave(&g_recown);   // [UIT 0047]
    open_end();   // [UIT 0045] queued: mtl_finish covers it now
    static uint64_t said_at;
    if (fence == 1 || fence >= said_at + 3000) {
        said_at = fence;
        VLOG("submission %llu: commands so far draw %llu, indexed %llu, dispatch %llu, clear %llu, set targets %llu, "
             "render enable %llu, conditional render %llu (draws and clears skipped %llu, conditionals unknown %llu, "
             "draws forced by TOTK_VK_COND_DRAWS %llu, clears forced %llu; off from pending reports %llu, from samples-passed words %llu, "
             "from other memory %llu)",
             (unsigned long long)fence, (unsigned long long)g_op_count[OP_DRAW], (unsigned long long)g_op_count[OP_DRAW_IDX],
             (unsigned long long)g_op_count[OP_DISPATCH], (unsigned long long)g_op_count[OP_CLEAR_COLOR],
             (unsigned long long)g_op_count[OP_SET_RT], (unsigned long long)g_op_count[OP_RENDER_ENABLE],
             (unsigned long long)g_op_count[OP_RENDER_COND], (unsigned long long)vkb_cnt_sum(g_cond_skipped), (unsigned long long)g_cond_unknown,
             (unsigned long long)vkb_cnt_sum(g_cond_forced), (unsigned long long)vkb_cnt_sum(g_cond_forced_clears), (unsigned long long)g_cond_off_pending,
             (unsigned long long)g_cond_off_samples, (unsigned long long)g_cond_off_mem);
        if (oq_on()) VLOG("TOTK_VK_OCCLUSION=gpu: samples-passed reports counted on the GPU %llu, read as visible %llu (no reset before, or over %u draws)",
                          (unsigned long long)g_oq_counted, (unsigned long long)g_oq_uncounted, OQ_CAP);
    }
    return fence;
}

// [UIT 0052] per frame over the last `every` presents: samples-passed reports and resets, conditionals evaluated off,
// draws / clears they skipped or TOTK_VK_COND_DRAWS forced, all draws and dispatches. Counters only read (translation
// thread, like the other log lines); one line a second at 60 fps.
static void occl_log(uint64_t n, uint64_t every) {
    static uint64_t pr, prs, poff, psk, pfd, pfc, pdr, pdi, pqc, pqu;
    uint64_t r = g_counter_types[NVN_COUNTER_TYPE_SAMPLES_PASSED], rs = g_reset_types[NVN_COUNTER_TYPE_SAMPLES_PASSED];
    uint64_t off = g_cond_off_pending + g_cond_off_samples + g_cond_off_mem, sk = vkb_cnt_sum(g_cond_skipped);
    uint64_t fd = vkb_cnt_sum(g_cond_forced), fc = vkb_cnt_sum(g_cond_forced_clears);
    uint64_t dr = g_op_count[OP_DRAW] + g_op_count[OP_DRAW_IDX], di = g_op_count[OP_DISPATCH];
    double f = (double)every;
    LOG("[occl] present %llu: per frame (last %llu) samples-passed reports %.1f, resets %.1f; conditionals off %.1f, "
        "skipped %.1f, forced draws %.1f, forced clears %.1f; draws %.0f, dispatches %.1f; occlusion %s, conditional draws mode %d",
        (unsigned long long)n, (unsigned long long)every, (double)(r - pr) / f, (double)(rs - prs) / f, (double)(off - poff) / f,
        (double)(sk - psk) / f, (double)(fd - pfd) / f, (double)(fc - pfc) / f, (double)(dr - pdr) / f, (double)(di - pdi) / f,
        occl_mode() == OCCL_GPU ? "gpu" : occl_mode() == OCCL_VISIBLE ? "visible" : "fixed", cond_draws());
    if (oq_on()) LOG("[occl]   gpu counts %.1f, read as visible %.1f", (double)(g_oq_counted - pqc) / f, (double)(g_oq_uncounted - pqu) / f);
    pr = r; prs = rs; poff = off; psk = sk; pfd = fd; pfc = fc; pdr = dr; pdi = di; pqc = g_oq_counted; pqu = g_oq_uncounted;
    vkb_draw_why_log(n, every);   // [UIT 0053]
    iwb_log(every);   // [UIT 0054]
    static uint64_t pic, pib;
    uint64_t ic = __atomic_load_n(&g_inval_calls, __ATOMIC_RELAXED), ib = __atomic_load_n(&g_inval_bytes, __ATOMIC_RELAXED);
    LOG("[draws]   game reads of GPU-written memory (invalidate): %.1f calls, %.1f KiB a frame", (double)(ic - pic) / f,
        (double)(ib - pib) / f / 1024.0);
    pic = ic; pib = ib;
    vkb_iwb_read_log(every);   // [UIT 0056]
}

// ---------------- several recording threads (vkb.h, "vkb_mt.c") ----------------
// One submission of the Great Sky Island is one handle of 5000-7000 draws; recording it took ~25 ms of one thread
// (2026-10-24), the most of the 33 ms frame. Here the translation thread walks it once in order (exec_ops EX_WALK:
// render state, events and counters, conditional rendering, the GPU writes that void captured uniforms) and cuts it into
// chunks of about MT_DRAWS draws at render target changes, each with the state at its start. Recording threads take the
// chunks as they are cut (the translation thread too, after the walk) and record each into a command buffer of their
// own, with their own caches, upload ring and descriptor memory. Work that has to come before every chunk goes into the
// prologue (texture uploads and pool copies: the CPU wrote those bytes before the submission). Images: each chunk keeps
// the layout every image is first needed in and the one it is left in (vkb_trk); a small command buffer before each
// chunk waits for all earlier work and moves its images there. The submission is then one vkQueueSubmit2 of: the slot's
// command buffer (GPU start time, captured uniforms, the move into the prologue's layouts), the prologue, and for each
// chunk its move and itself, then the end (copies back).
// [temporary] Mirrors of written storage buffers are not made (one thread only); TOTK_VK_RECORD_THREADS=1 records as before.
// [UIT 0019] A chunk is also cut before a draw inside a long pass once it has mt_split_draws() draws (TOTK_VK_CHUNK_SPLIT,
// default 600; 0 = only at render target changes, as before). The G-buffer pass alone is 2000+ draws: cut only at target
// changes it was one chunk that one thread recorded while the others idled ("largest chunk" in the log line). The next
// chunk begins its own pass on the same targets (load + store, as after a uniform update), with the state saved here.
enum { MT_DRAWS = 400 };
static int mt_split_draws(void) {
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("TOTK_VK_CHUNK_SPLIT");
        v = e && *e ? atoi(e) : 600;
        if (v < 0) v = 0;
        if (v && v < 100) v = 100;
        VLOG("recording threads: %s", v ? "chunks are also cut inside long passes (TOTK_VK_CHUNK_SPLIT)" : "chunks cut at render target changes only (TOTK_VK_CHUNK_SPLIT=0)");
    }
    return v;
}
struct mt_chunk {
    int h0, h1; const uint8_t *p0, *p1;   // its first command, and the one after its last (h1 == the handle count: the end)
    uint64_t draws, rec_ns;               // draws the walk counted in it; its recording time
    uint64_t seq;                         // the position before its first command
    uint32_t oq_n;                        // [UIT 0050] occlusion queries handed out by the walk up to its end
    void *state; p_set_rt rt; int render_off; int64_t stride;   // the state it starts in
    VkCommandBuffer cb;
    vkb_trk trk;
    unsigned char done;                   // [native-1031] recorded (g_mt_mu): TOTK_VK_EARLY_PARTS sends finished chunks early
};
static uint32_t chunk_oq_n(const mt_chunk *c) { return c->oq_n; }
static mt_chunk g_ch[MT_MAXCH];
static int g_nch;
static const uint64_t *g_mt_h;
static int g_mt_n, g_mt_threads;
static pthread_mutex_t g_mt_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_mt_cv = PTHREAD_COND_INITIALIZER, g_mt_done_cv = PTHREAD_COND_INITIALIZER;
static int g_mt_avail, g_mt_next, g_mt_done;   // chunks cut / taken / recorded
static uint64_t g_mt_draws;                    // draws in the chunk being cut
static void *g_walk_state;                     // where the walk ended: the next one starts there
static p_set_rt g_walk_rt;
static int g_walk_off, g_walk_have;
static int64_t g_walk_stride;
// the prologue: any recording thread, one at a time
static pthread_mutex_t g_pro_mu = PTHREAD_MUTEX_INITIALIZER;
static VkCommandBuffer g_pro_cb;
static vkb_trk g_pro_trk;
static __thread VkCommandBuffer t_pro_cb;
static __thread vkb_trk *t_pro_trk;
static __thread int t_pro_depth;
// command buffers per slot and recording thread; [VKB_MAXREC]: the prologue's, the moves' and the end's
typedef struct { VkCommandPool pool; VkCommandBuffer *cb; int n, used; } wpool_t;
static wpool_t g_wp[SLOTS][VKB_MAXREC + 1];

int vkb_mt_threads(void) { return g_mt_threads ? g_mt_threads : 1; }   // [UIT 0019] for TOTK_PROFILE
int vkb_mt_on(void) {
    static int n = -1;
    if (n < 0) {
        SYSTEM_INFO si;
        GetSystemInfo(&si);
        int cpus = (int)si.dwNumberOfProcessors;
        const char *e = getenv("TOTK_VK_RECORD_THREADS");
        // [UIT 0019] more threads on big CPUs: the translation thread waits for the slowest chunk (6 at 16+ logical CPUs)
        n = e && *e ? atoi(e) : cpus >= 16 ? 6 : cpus >= 12 ? 5 : cpus >= 8 ? 4 : cpus >= 4 ? 2 : 1;
        if (n > VKB_MAXREC) n = VKB_MAXREC;
        if (n < 1) n = 1;
        static const char *const diag[] = {"TOTK_VK_DRAWTRACE", "TOTK_VK_TRACE", "TOTK_SNAP", "TOTK_GPU_PROF", "TOTK_GPU_PASSPROF",
                                           "TOTK_VK_CHECKPOINTS", "TOTK_AUDIT", "TOTK_VK_SPLIT", "TOTK_VK_STORES_LOG", "TOTK_WATCH"};
        const char *why = NULL;   // development tools that follow the recording in order: one thread
        for (unsigned i = 0; i < sizeof diag / sizeof *diag; i++) if (getenv(diag[i])) why = diag[i];
        if (n > 1 && why) n = 1;
        if (n > 1 && g_ok != 1) n = 1;
        g_mt_threads = n;
        VLOG("recording threads: %d%s%s", n, why ? " (one: " : "", why ? why : "");
        if (why) VLOG("  (one thread while %s is set)", why);
    }
    return n > 1;
}

static VkCommandBuffer wp_get(int slot, int w) {   // a command buffer of that thread's pool for the slot, begun
    wpool_t *p = &g_wp[slot][w];
    if (!p->pool) {
        VkCommandPoolCreateInfo pi = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT,
                                      .queueFamilyIndex = vkc.family};
        if (!vk_check(vk.vkCreateCommandPool(vkc.device, &pi, NULL, &p->pool), "recording command pool")) die("command pool");
    }
    if (p->used == p->n) {
        int add = 16;
        if (!(p->cb = realloc(p->cb, sizeof *p->cb * (size_t)(p->n + add)))) die("command buffers");
        VkCommandBufferAllocateInfo ai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = p->pool,
                                          .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = (uint32_t)add};
        if (!vk_check(vk.vkAllocateCommandBuffers(vkc.device, &ai, p->cb + p->n), "command buffers")) die("command buffers");
        p->n += add;
    }
    VkCommandBuffer cb = p->cb[p->used++];
    VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    vk.vkBeginCommandBuffer(cb, &bi);
    return cb;
}
void vkb_mt_slot_reset(int slot) {   // the slot's previous submission has completed (no thread is recording)
    for (int w = 0; w <= VKB_MAXREC; w++)
        if (g_wp[slot][w].used) { vk.vkResetCommandPool(vkc.device, g_wp[slot][w].pool, 0); g_wp[slot][w].used = 0; }
}

void vkb_pro_begin(void) {
    if (!t_recording || t_pro_depth++) return;
    pthread_mutex_lock(&g_pro_mu);
    t_pro_cb = t_cb; t_pro_trk = vkb_trk_get();
    t_cb = g_pro_cb;
    vkb_trk_set(&g_pro_trk);
}
void vkb_pro_end(void) {
    if (!t_recording || --t_pro_depth) return;
    t_cb = t_pro_cb;
    vkb_trk_set(t_pro_trk);
    pthread_mutex_unlock(&g_pro_mu);
}

static int chunk_end_at(const mt_chunk *c, int hidx, const uint8_t *p) { return hidx == c->h1 && p == c->p1; }
static vkb_cnt g_mt_rec_ns;   // chunk recording time, all threads (the log line)
static void mt_record(mt_chunk *c) {
    uint64_t t0 = runtime_now_ns();
    c->cb = wp_get(g_cur, t_worker);
    t_cb = c->cb;
    t_recording = 1;
    t_mt_ch = (int)(c - g_ch);   // [UIT 0258] the pools' first-chunk stamps
    vkb_trk_reset(&c->trk);
    vkb_trk_set(&c->trk);
    vkb_rec_begin();
    vkb_state_load(c->state);
    g_rt = c->rt;
    g_render_off = c->render_off;
    vkb_copy_stride(c->stride);
    g_in_pass = 0;
    vkb_stores_take();
    t_seq = c->seq;
    for (int i = c->h0; i < g_mt_n; i++) {
        const uint8_t *p = i == c->h0 ? c->p0 : (const uint8_t *)(uintptr_t)g_mt_h[i];
        if (chunk_end_at(c, i, p) || exec_ops(p, 0, EX_CHUNK, i, c)) break;
    }
    pass_end_why(PE_SUBMIT);
    vk.vkEndCommandBuffer(c->cb);
    vkb_trk_set(NULL);
    t_recording = 0;
    t_mt_ch = -1;
    t_cb = VK_NULL_HANDLE;
    c->rec_ns = runtime_now_ns() - t0;
    vkb_cnt_add(g_mt_rec_ns, c->rec_ns);
    uit_prof_add(UP_R_REC, c->rec_ns);
}
static int mt_take(void) {   // the next chunk cut and not taken, or -1
    pthread_mutex_lock(&g_mt_mu);
    int k = g_mt_next < g_mt_avail ? g_mt_next++ : -1;
    pthread_mutex_unlock(&g_mt_mu);
    return k;
}
static void mt_recorded(int k) {
    pthread_mutex_lock(&g_mt_mu);
    g_ch[k].done = 1;
    g_mt_done++;
    pthread_cond_broadcast(&g_mt_done_cv);
    pthread_mutex_unlock(&g_mt_mu);
}
static int g_early_stream;            // [UIT 0236] (vkb_early.h, below)
// [UIT 0258] when a submission's first work reaches the GPU (an early part, else the commit), from vkb_mt_submit's start
static uint64_t g_mt_t0, g_first_ns, g_first_early;
// [UIT 0249] TOTK_VK_PRESYNC_PARALLEL (Performance > CPU+ > Check the game's memory writes on all helper threads at a
// frame's start; default on, 0 = as before). The prologue brings the pools used in the last submissions up to date
// (shadow_presync): for each, a write-watch scan (GetWriteWatch over the whole pool, a system call) and the copies of
// the pages found. One thread did ~480 of them one after another at Lookout Landing ("write watch 1.29 ms" a frame,
// prologue 1.3 ms) while the six recording threads waited for the first chunk, and nothing reached the GPU before it
// was over. Now the same pools' scans are first handed out to the recording threads and the translation thread
// together (nvn_dirty_epoch_prescan: exactly the epoch's whole-range scan the translation thread's own first look
// would make, under the range's lock), then the loop runs as before and finds each range scanned in this epoch, as
// its second look did before; the copies and their order are unchanged. Fewer than PS_MIN pools: as before.
enum { PS_MIN = 24 };
static int presync_par_on(void) {
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("TOTK_VK_PRESYNC_PARALLEL");
        v = !(e && e[0] == '0' && !e[1]);
        VLOG("[UIT 0249] TOTK_VK_PRESYNC_PARALLEL=%d: %s", v, v ? "the write-watch scans of the pools brought up to date at a submission's start run on all recording threads"
                                                                 : "the write-watch scans at a submission's start run on the translation thread alone (as before)");
    }
    return v;
}
static uint32_t g_ps_wake;                 // g_mt_mu: raised for each job (the recording threads join it)
static _Atomic uint64_t g_ps_word;         // the job's number << 32 | the next item; items taken by compare-and-swap
static _Atomic int g_ps_n, g_ps_done;
static _Atomic uint64_t g_ps_epoch;
static const void **g_ps_addr;             // the items: a pool address each (written before the job is published)
static int g_ps_cap;
static uint32_t g_ps_job;
#define PS_CLOSED 0xffffffffu
static int ps_work(uint32_t job) {   // items of the job until none is left; how many this thread did
    int did = 0;
    for (;;) {
        uint64_t w = atomic_load(&g_ps_word);
        uint32_t i = (uint32_t)w;
        if ((uint32_t)(w >> 32) != job || i == PS_CLOSED || (int)i >= atomic_load(&g_ps_n)) return did;
        if (!atomic_compare_exchange_weak(&g_ps_word, &w, w + 1)) continue;
        if (nvn_dirty_epoch_prescan(g_ps_addr[i], atomic_load(&g_ps_epoch))) atomic_fetch_add_explicit(&g_ps_scanned, 1, memory_order_relaxed);
        atomic_fetch_add(&g_ps_done, 1);
        did++;
    }
}
// g_pool_mu held (writer), by shadow_presync on the translation thread in vkb_mt_submit: no chunk is out yet
static void presync_prescan(uint64_t sub, uint64_t win, const uintptr_t *only, int nonly) {   // only: [UIT 0258] these pools
    if (g_mt_threads < 2 || !presync_par_on()) return;
    int n = 0;
    if (only) n = nonly;
    else
        for (int i = 0; i < g_npools; i++)
            if (g_pools[i].sh && g_pools[i].sync_sub != sub && g_pools[i].used_sub + win >= sub) n++;
    if (n < PS_MIN) return;
    uint64_t t0 = runtime_now_ns();
    uint32_t job = ++g_ps_job;
    if (!job) job = g_ps_job = 1;
    atomic_store(&g_ps_word, (uint64_t)job << 32 | PS_CLOSED);   // a thread still in the last job stops taking items
    if (n > g_ps_cap) {
        const void **a = realloc(g_ps_addr, (size_t)n * 2 * sizeof *a);
        if (!a) return;
        g_ps_addr = a; g_ps_cap = n * 2;
    }
    int k = 0;
    if (only) for (; k < n; k++) g_ps_addr[k] = (const void *)only[k];
    else
        for (int i = 0; i < g_npools && k < n; i++)
            if (g_pools[i].sh && g_pools[i].sync_sub != sub && g_pools[i].used_sub + win >= sub) g_ps_addr[k++] = (const void *)g_pools[i].lo;
    atomic_store(&g_ps_n, k);
    atomic_store(&g_ps_done, 0);
    atomic_store(&g_ps_epoch, nvn_dirty_epoch_now());
    atomic_store(&g_ps_word, (uint64_t)job << 32);   // published
    pthread_mutex_lock(&g_mt_mu);
    g_ps_wake = job;
    pthread_cond_broadcast(&g_mt_cv);
    pthread_mutex_unlock(&g_mt_mu);
    ps_work(job);
    while (atomic_load(&g_ps_done) < k) sched_yield();   // items another thread took: a scan each, then the loop
    atomic_fetch_add_explicit(&g_ps_jobs, 1, memory_order_relaxed);
    atomic_fetch_add_explicit(&g_ps_items, (uint64_t)k, memory_order_relaxed);
    atomic_fetch_add_explicit(&g_ps_ns, runtime_now_ns() - t0, memory_order_relaxed);
}
static void early_try(int wait);
static void *mt_thread(void *arg) {
    t_worker = (int)(intptr_t)arg;
    char name[32];
    snprintf(name, sizeof name, "totk-record-%d", t_worker);
    pthread_setname_np(name);
    { void plat_thread_boost(void); plat_thread_boost(); }   // the frame waits for these, as for the translation thread
    { int plat_thread_pcores(void); plat_thread_pcores(); }   // [UIT 0033] TOTK_VK_PCORES=1: performance cores (hybrid CPUs)
    uint32_t ps_seen = 0;   // [UIT 0249] the last prescan job this thread joined
    for (;;) {
        pthread_mutex_lock(&g_mt_mu);
        while (g_mt_next >= g_mt_avail && g_ps_wake == ps_seen) pthread_cond_wait(&g_mt_cv, &g_mt_mu);
        if (g_mt_next >= g_mt_avail) {   // [UIT 0249] a prescan job (never with TOTK_VK_PRESYNC_PARALLEL=0)
            ps_seen = g_ps_wake;
            pthread_mutex_unlock(&g_mt_mu);
            if (ps_work(ps_seen)) atomic_fetch_add_explicit(&g_ps_helped, 1, memory_order_relaxed);
            continue;
        }
        int k = g_mt_next++;
        pthread_mutex_unlock(&g_mt_mu);
        mt_record(&g_ch[k]);
        mt_recorded(k);
        if (g_early_stream) early_try(0);   // [UIT 0236] send what is finished in order, unless another thread is sending
    }
    return NULL;
}

// ---- the walk
static void chunk_open(int hidx, const uint8_t *p) {   // a chunk starts at p, in the state the walk is in now
    mt_chunk *c = &g_ch[g_nch];
    c->h0 = hidx; c->p0 = p; c->seq = t_seq;
    c->done = 0;   // (not yet handed out: g_mt_avail is raised by chunk_close)
    if (!c->state && !(c->state = malloc(vkb_state_size()))) die("chunk state");
    vkb_state_save(c->state);
    c->rt = g_rt;
    c->render_off = g_render_off;
    c->stride = vkb_copy_stride_get();
    g_mt_draws = 0;
}
static void chunk_close(int hidx, const uint8_t *p) {   // the open chunk ends before p: recording threads may take it
    mt_chunk *c = &g_ch[g_nch];
    c->h1 = hidx; c->p1 = p;
    c->draws = g_mt_draws;
    c->oq_n = oq_on() ? g_oqw->n : 0;
    pbc_sync(g_nch, 1);   // [UIT 0258] the pools this chunk used first last time, before it is handed out
    pthread_mutex_lock(&g_mt_mu);
    g_mt_avail = ++g_nch;
    pthread_cond_broadcast(&g_mt_cv);
    pthread_mutex_unlock(&g_mt_mu);
    if (g_pbc_walk_open) early_try(0);   // [UIT 0258] done chunks may go to the GPU while the walk runs (never the last cut one)
}
#include "vkb_chunk_ramp.h"
static int early_parts(void);
// [UIT 0241] TOTK_VK_CHUNK_RAMP (vkb_chunk_ramp.h): with the stream on, the first 6 chunks are cut at 200 draws, the next at
// 400, then at the usual split
static int chunk_ramp(void) {
    static int v = -1;
    if (v < 0) {
        int r = vkb_ramp_parse(getenv("TOTK_VK_CHUNK_RAMP"));
        early_parts();   // (sets g_early_stream)
        v = g_early_stream && mt_split_draws() ? r : 0;
        if (v) VLOG("[UIT 0241] TOTK_VK_CHUNK_RAMP=%d: a submission's first %d chunks are cut at %d draws, then doubling up to %d as before, "
                    "with %d more stream parts, so the GPU gets its first part sooner (0: off)", v, VKB_RAMP_FLAT, v, mt_split_draws(), VKB_RAMP_FLAT);
        else VLOG("[UIT 0241] TOTK_VK_CHUNK_RAMP off: every chunk cut at %d draws inside long passes as before%s", mt_split_draws(),
                  r && !g_early_stream ? " (it needs TOTK_VK_EARLY_STREAM)" : "");
    }
    return v;
}
static int walk_op(const uint8_t *p, int hidx) {   // before a command at depth 0: a new chunk at a render target change
    op_hdr h;
    memcpy(&h, p, sizeof h);
    int split = mt_split_draws();
    uint64_t lim = vkb_chunk_limit(g_nch, split, chunk_ramp());   // [UIT 0241] split, or less for the first chunks
    if (g_nch < MT_MAXCH - 1 && ((h.op == OP_SET_RT && g_mt_draws >= MT_DRAWS) ||
                                 (split && (h.op == OP_DRAW || h.op == OP_DRAW_IDX) && g_mt_draws >= lim))) {
        chunk_close(hidx, p);
        chunk_open(hidx, p);
    }
    return 0;
}
static void walk_command(uint32_t op, const uint8_t *a) {
    switch (op) {
    case OP_SIGNAL_EVENT: {
        volatile uint32_t *val; uint32_t m, v;
        memcpy(&val, a, 8); memcpy(&m, a + 8, 4); memcpy(&v, a + 12, 4);
        mtl_sub_event(val, m, v);
        break;
    }
    case OP_REPORT_COUNTER: { uint64_t t, ad; memcpy(&t, a, 8); memcpy(&ad, a + 8, 8); mtl_sub_counter(ad, (uint32_t)t); break; }
    case OP_RESET_COUNTER: { uint64_t t; memcpy(&t, a, 8); counter_reset((uint32_t)t); break; }
    case OP_COPY_B2B: {   // GPU writes ([UIT 0335] noted first: the copy's source as the GPU reads it here)
        uint64_t sr, d, n; memcpy(&sr, a, 8); memcpy(&d, a + 8, 8); memcpy(&n, a + 16, 8);
        if (n) { vkb_ucap_note_copy(sr, d, n); vkb_ucap_drop(d, n); }
        break;
    }
    case OP_UPDATE_UB: { uint64_t d, n; memcpy(&d, a, 8); memcpy(&n, a + 8, 8); if (n) { vkb_ucap_note_write(d, n, a + 16); vkb_ucap_drop(d, n); } break; }
    case OP_COPY_T2B: { uint64_t ad, len; if (vkb_copy_t2b_range((const p_copy_b2t *)a, &ad, &len)) vkb_ucap_drop(ad, len); break; }
    case OP_COPY_STRIDE: vkb_copy_stride(((const p_copy_stride *)a)->stride); break;
    case OP_SET_RT: memcpy(&g_rt, a, sizeof g_rt); break;
    case OP_BIND_PROGRAM: case OP_BIND_STATE: case OP_VIEWPORTS: case OP_SCISSOR: case OP_DEPTH_RANGE: case OP_STENCIL:
    case OP_POLY_OFFSET: case OP_BLEND_COLOR: case OP_BIND_VATTR: case OP_BIND_VSTREAM: case OP_BIND_VBUF: case OP_BIND_UBO:
    case OP_BIND_SSBO: case OP_BIND_TEX: case OP_BIND_IMG: case OP_DEPTH_CLAMP: case OP_RASTER_DISCARD: case OP_PRIM_RESTART:
    case OP_LINE_WIDTH:
        vkb_state(op, a);
        break;
    case OP_DRAW: case OP_DRAW_IDX:
        if (!draw_off()) { g_mt_draws++; vkb_draw_prescan(a, op == OP_DRAW_IDX, 0); if (oq_on()) oq_draw(g_oqw, t_seq); }
        break;
    case OP_DISPATCH: g_mt_draws++; vkb_draw_prescan(a, 0, 1); break;
    case OP_RENDER_ENABLE: g_render_off = !((const p_flag *)a)->value; break;
    case OP_RENDER_COND: {   // evaluated here, in order; the chunk reads the answer from the command
        p_render_cond *r = (p_render_cond *)(uintptr_t)a;
        g_render_off = cond_eval(r) ? 0 : 2;
        r->pad = g_render_off ? 2 : 1;
        break;
    }
    default: break;
    }
}

// Before a chunk: everything earlier is done, and its images move into the layouts it needs first.
static void glue(VkCommandBuffer cb, vkb_trk *t) {
    VkMemoryBarrier2 mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
        .srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT, .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
        .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT};
    VkImageMemoryBarrier2 ib[64];
    int ni = 0, first = 1;
    for (int k = 0; k <= t->n; k++) {
        if (k < t->n) {
            vkb_trk_e *e = &t->e[k];
            if (e->im->layout == e->first) continue;
            ib[ni++] = (VkImageMemoryBarrier2){VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2, .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                .srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT, .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT, .oldLayout = e->im->layout, .newLayout = e->first,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = e->im->img,
                .subresourceRange = {e->im->aspect, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS}};
            if (ni < 64) continue;
        }
        if (ni || first) {
            VkDependencyInfo dep = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = first ? 1u : 0u, .pMemoryBarriers = &mb,
                                    .imageMemoryBarrierCount = (uint32_t)ni, .pImageMemoryBarriers = ib};
            vk.vkCmdPipelineBarrier2(cb, &dep);
            ni = 0; first = 0;
        }
    }
    for (int k = 0; k < t->n; k++) t->e[k].im->layout = t->e[k].cur;
}

// ---- [native-1031] TOTK_VK_EARLY_PARTS (vkb_early.h): finished chunks go to the GPU while the others record
#include "vkb_early.h"
// g_early_stream: [UIT 0236] TOTK_VK_EARLY_STREAM (vkb_early.h), set by early_parts() before the recording threads start
static int early_parts(void) {
    static int v = -1;
    if (v < 0) {
        v = vkb_early_mode(getenv("TOTK_VK_EARLY_PARTS"), getenv("TOTK_VK_EARLY_STREAM"), &g_early_stream);
        if (g_early_stream)
            VLOG("[UIT 0236] TOTK_VK_EARLY_STREAM on (default): finished chunks go to the GPU while the others record, in up to %d parts "
                 "a submission, sent by whichever recording thread finishes one (the last part signals the fence, as before; "
                 "TOTK_VK_EARLY_STREAM=0: one part)", v);
        else if (v > 1) VLOG("TOTK_VK_EARLY_PARTS=%d: the GPU starts on a submission once its first chunks are recorded, in up to %d parts "
                             "(the last one signals the fence, as before)", v, v);
        else VLOG("[UIT 0236] early parts off (TOTK_VK_EARLY_STREAM=0): a submission goes to the GPU in one part once all of it is recorded");
    }
    return v;
}
uint64_t mtl_last_fence(void);
#include "vkb_early_gate.h"
static vkb_early_gate g_early_gate = VKB_EARLY_GATE_INIT;   // [UIT 0236] open from the walk's end to the last part
static int g_early_next, g_early_sent;   // this submission: chunks already sent, early parts sent (g_early_gate)
static uint64_t g_early_total;           // its draws (the walk's count)
static uint64_t g_early_parts_n, g_early_chunks_n, g_early_ns, g_early_rec_n;   // the log line (g_early_gate)
// After the walk: send the chunks recorded so far if the plan says so. The open prologue segment goes in front of them
// (the first time behind the slot's command buffer, which is then ended) and a new segment opens for the chunks still
// to come. g_pro_mu held while building it: recording threads write the prologue, and its command buffers and the
// moves' are from one pool (g_wp[slot][VKB_MAXREC]). [UIT 0236] Called by the translation thread (wait = 1: it has
// nothing else to do) and, with TOTK_VK_EARLY_STREAM, by a recording thread after each chunk (wait = 0: never waits for
// another thread's send). With TOTK_VK_ASYNC_SUBMIT the part is queued for the submit thread under the fence the
// submission is about to take (vkb_subq.h keeps it before that fence's last part); before, it went to the driver here
// and was skipped while the submit thread still held an earlier submission.
static void early_try(int wait) {
    int parts = vkb_ramp_parts(early_parts(), g_early_stream, chunk_ramp());   // [UIT 0241] 6 more parts with the ramp
    if (parts <= 1) return;
    if (!vkb_early_enter(&g_early_gate, wait)) return;
    if (g_early_sent >= parts - 1) { vkb_early_leave(&g_early_gate); return; }
    unsigned char done[MT_MAXCH];
    uint64_t draws[MT_MAXCH];
    pthread_mutex_lock(&g_mt_mu);
    int nch = g_nch;
    for (int k = 0; k < nch; k++) { done[k] = g_ch[k].done; draws[k] = g_ch[k].draws; }
    pthread_mutex_unlock(&g_mt_mu);
    int to = g_early_stream ? vkb_early_stream_plan(g_early_next, nch, done, draws, parts, g_early_sent, VKB_EARLY_MIN_DRAWS)
                            : vkb_early_plan(g_early_next, nch, done, draws, g_early_total, parts, g_early_sent, VKB_EARLY_MIN_DRAWS);
    if (to <= g_early_next) { vkb_early_leave(&g_early_gate); return; }
    uint64_t t0 = runtime_now_ns();
    VkCommandBufferSubmitInfo cbi[2 * MT_MAXCH + 4];
    int n = 0;
#define EARLY_CB(c) (cbi[n++] = (VkCommandBufferSubmitInfo){VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO, .commandBuffer = (c)})
    pthread_mutex_lock(&g_pro_mu);
    vk.vkEndCommandBuffer(g_pro_cb);
    if (!g_slot_cb_sent) {
        VkCommandBuffer scb = g_slots[g_cur].cb;
        glue(scb, &g_pro_trk);
        vk.vkEndCommandBuffer(scb);
        EARLY_CB(scb);
        g_slot_cb_sent = 1;
    } else {
        VkCommandBuffer m = wp_get(g_cur, VKB_MAXREC);
        glue(m, &g_pro_trk);
        vk.vkEndCommandBuffer(m);
        EARLY_CB(m);
    }
    EARLY_CB(g_pro_cb);
    g_pro_cb = wp_get(g_cur, VKB_MAXREC);   // the next segment
    vkb_trk_reset(&g_pro_trk);
    for (int k = g_early_next; k < to; k++) {
        VkCommandBuffer m = wp_get(g_cur, VKB_MAXREC);
        glue(m, &g_ch[k].trk);
        vk.vkEndCommandBuffer(m);
        EARLY_CB(m);
        EARLY_CB(g_ch[k].cb);
    }
    pthread_mutex_unlock(&g_pro_mu);
#undef EARLY_CB
    g_early_fence = mtl_last_fence() + 1;   // the translation thread takes it in mtl_sub_commit (it alone commits)
    if (atomic_load(&g_async)) {   // [UIT 0236] the submit thread, in queue order: after every earlier submission
        sub_req *rq = malloc(sizeof *rq + (size_t)n * sizeof *cbi);
        if (!rq) die("submit request");
        *rq = (sub_req){.q = {.fence = g_early_fence, .t_push = runtime_now_ns()}, .partial = 1, .out_slot = -1, .ncbi = n};
        memcpy(rq->cbi, cbi, (size_t)n * sizeof *cbi);
        vkb_subq_push(&g_subq, &rq->q);
    } else {   // submitted on the committing thread: every earlier submission is on the queue already
        VkSubmitInfo2 si = {VK_STRUCTURE_TYPE_SUBMIT_INFO_2, .commandBufferInfoCount = (uint32_t)n, .pCommandBufferInfos = cbi};
        vk_queue_lock();
        VkResult r = vk.vkQueueSubmit2(vkc.queue, 1, &si, VK_NULL_HANDLE);
        vk_queue_unlock();
        if (r != VK_SUCCESS) { vk_check(r, "vkQueueSubmit2 (early part)"); die("GPU submission failed"); }
    }
    if (!g_early_sent) { g_first_ns += runtime_now_ns() - g_mt_t0; g_first_early++; }   // [UIT 0258]
    g_early_parts_n++; g_early_chunks_n += (uint64_t)(to - g_early_next);
    if (t_worker > 0) g_early_rec_n++;
    g_early_next = to; g_early_sent++;
    g_early_ns += runtime_now_ns() - t0;
    vkb_early_leave(&g_early_gate);
}

void vkb_mt_submit(const uint64_t *h, int n) {   // after mtl_sub_begin and the uniform capture; commits
    uint64_t t0 = runtime_now_ns();
    g_mt_t0 = t0;
    vkb_tex_quiesce();   // [UIT 0233] no recording thread is reading the images table: old tables and snapshots go
    static uint64_t walk_ns, wall_ns, subs, chunks, said_at, glue_images, big_ns, big_draws, pro_ns, wait_ns, glue_ns;
    static int started;
    if (!started) {
        started = 1;
        early_parts();   // [UIT 0236] g_early_stream is read by the recording threads: set before they start
        for (int w = 1; w < g_mt_threads; w++) {
            pthread_t t;
            if (pthread_create(&t, NULL, mt_thread, (void *)(intptr_t)w) == 0) pthread_detach(t);
            else die("recording thread");
        }
    }
    g_mt_h = h; g_mt_n = n;
    pthread_mutex_lock(&g_mt_mu);
    g_mt_avail = g_mt_next = g_mt_done = 0;
    g_nch = 0;
    pthread_mutex_unlock(&g_mt_mu);
    // the prologue: objects made on first use, the pool copies of the last submissions
    g_pro_cb = wp_get(g_cur, VKB_MAXREC);
    vkb_trk_reset(&g_pro_trk);
    t_recording = 1;
    vkb_pro_begin();
    vkb_draw_mt_init();
    vkb_pro_end();
    t_ps_lead = 1;   // [UIT 0249]
    if (pbc_on()) pbc_presync();   // [UIT 0258] chunk 0's pools; the walk the others (g_pbc_live)
    else shadow_presync();
    t_ps_lead = 0;
    t_recording = 0;
    uint64_t tw = runtime_now_ns();
    // [UIT 0258] with the stream, parts may go while the walk runs: vkb_early_stream_plan sends only done chunks followed
    // by one not done, and the walk's open chunk is not cut yet, so the last part stays this thread's (the commit)
    g_pbc_walk_open = g_pbc_live && g_early_stream && early_parts() > 1;
    if (g_pbc_walk_open) { g_early_next = g_early_sent = 0; g_early_total = 0; vkb_early_set_open(&g_early_gate, 1); }
    // the walk, from where the last one ended
    if (g_walk_have) { vkb_state_load(g_walk_state); g_rt = g_walk_rt; g_render_off = g_walk_off; vkb_copy_stride(g_walk_stride); }
    g_in_pass = 0;
    chunk_open(0, n ? (const uint8_t *)(uintptr_t)h[0] : NULL);
    for (int i = 0; i < n; i++) exec_ops((const uint8_t *)(uintptr_t)h[i], 0, EX_WALK, i, NULL);
    pbc_sync(1 << 30, 2);   // [UIT 0258] pools wanted by chunks past the last one: before it is handed out
    g_pbc_live = 0;
    chunk_close(n, NULL);
    if (!g_walk_state && !(g_walk_state = malloc(vkb_state_size()))) die("walk state");
    vkb_state_save(g_walk_state);
    g_walk_rt = g_rt; g_walk_off = g_render_off; g_walk_stride = vkb_copy_stride_get(); g_walk_have = 1;
    if (!g_pbc_walk_open) { g_early_next = g_early_sent = 0; g_early_total = 0; }   // [native-1031] TOTK_VK_EARLY_PARTS: parts from here (all chunks cut)
    for (int k = 0; k < g_nch; k++) g_early_total += g_ch[k].draws;
    if (early_parts() > 1 && !g_pbc_walk_open) vkb_early_set_open(&g_early_gate, 1);   // [UIT 0236] recording threads may send from here
    g_pbc_walk_open = 0;
    early_try(1);
    walk_ns += runtime_now_ns() - tw;
    pro_ns += tw - t0;
    // this thread records chunks too, then waits for the others
    for (int k; (k = mt_take()) >= 0;) { mt_record(&g_ch[k]); mt_recorded(k); early_try(0); }
    uint64_t tq = runtime_now_ns();
    pthread_mutex_lock(&g_mt_mu);
    while (g_mt_done < g_nch) {
        pthread_cond_wait(&g_mt_done_cv, &g_mt_mu);
        if (g_mt_done < g_nch && early_parts() > 1) { pthread_mutex_unlock(&g_mt_mu); early_try(1); pthread_mutex_lock(&g_mt_mu); }
    }
    pthread_mutex_unlock(&g_mt_mu);
    if (early_parts() > 1) vkb_early_set_open(&g_early_gate, 0);   // [UIT 0236] waits for a recording thread's send; none after
    uint64_t tg = runtime_now_ns();
    wait_ns += tg - tq;
    { uint64_t mx = 0, md = 0; for (int k = 0; k < g_nch; k++) if (g_ch[k].rec_ns > mx) { mx = g_ch[k].rec_ns; md = g_ch[k].draws; }
      big_ns += mx; big_draws += md; uit_cx_add(CX_REC_BIG, mx); uit_cx_add(CX_REC_BIGDRAWS, md); }
    // in order: the slot's command buffer moves images into the prologue's layouts, then each chunk's move and the chunk
    vk.vkEndCommandBuffer(g_pro_cb);
    g_mt_nlist = 0;
    if (!g_slot_cb_sent) glue(g_slots[g_cur].cb, &g_pro_trk);
    else {   // [native-1031] early parts went first: the last prologue segment's moves get their own command buffer
        VkCommandBuffer m = wp_get(g_cur, VKB_MAXREC);
        glue(m, &g_pro_trk);
        vk.vkEndCommandBuffer(m);
        g_mt_list[g_mt_nlist++] = m;
    }
    g_mt_list[g_mt_nlist++] = g_pro_cb;
    for (int k = g_early_next; k < g_nch; k++) {
        VkCommandBuffer m = wp_get(g_cur, VKB_MAXREC);
        glue_images += (uint64_t)g_ch[k].trk.n;
        glue(m, &g_ch[k].trk);
        vk.vkEndCommandBuffer(m);
        g_mt_list[g_mt_nlist++] = m;
        g_mt_list[g_mt_nlist++] = g_ch[k].cb;
    }
    g_mt_list[g_mt_nlist++] = wp_get(g_cur, VKB_MAXREC);   // the end
    if (!g_early_sent) g_first_ns += runtime_now_ns() - t0;   // [UIT 0258] no early part: the GPU starts with the commit
    mtl_sub_commit();
    g_mt_nlist = 0;
    uint64_t te = runtime_now_ns();
    glue_ns += te - tg;
    wall_ns += te - t0;
    uit_prof_add(UP_X_PROLOGUE, tw - t0); uit_prof_add(UP_X_WALK, tq - tw); uit_prof_add(UP_X_RECWAIT, tg - tq); uit_prof_add(UP_X_COMMIT, te - tg);
    uit_cx_add(CX_REC_PRO, tw - t0); uit_cx_add(CX_REC_WALK, tq - tw); uit_cx_add(CX_REC_WAIT, tg - tq); uit_cx_add(CX_REC_COMMIT, te - tg);   // [UIT 0232]
    uit_cx_add(CX_SUBS, 1);
    subs++; chunks += (uint64_t)g_nch;
    if (subs >= said_at + 600) {   // the log line: where a submission's recording time goes
        said_at = subs;
        static uint64_t prev_rec;
        uint64_t rec = vkb_cnt_sum(g_mt_rec_ns);
        VLOG("recording threads: per submission %.1f ms until submitted (walk %.1f ms), chunks %.1f, chunk recording %.1f ms in all "
             "threads, images moved between chunks %.0f", wall_ns / 1e6 / 600, walk_ns / 1e6 / 600, chunks / 600.0,
             (rec - prev_rec) / 1e6 / 600, glue_images / 600.0);
        VLOG("recording threads: per submission largest chunk %.1f ms (%.0f draws), prologue %.1f ms, waiting for other threads %.1f ms, "
             "moves + submit %.1f ms; %d threads", big_ns / 1e6 / 600, big_draws / 600.0, pro_ns / 1e6 / 600, wait_ns / 1e6 / 600,
             glue_ns / 1e6 / 600, g_mt_threads);
        if (early_parts() > 1) {
            VLOG("recording threads: %s=%d: per submission %.2f early parts (%.2f sent by recording threads), %.1f of %.1f chunks "
                 "sent early, %.2f ms building and handing them over", g_early_stream ? "TOTK_VK_EARLY_STREAM, parts up to" : "TOTK_VK_EARLY_PARTS",
                 early_parts(), g_early_parts_n / 600.0, g_early_rec_n / 600.0, g_early_chunks_n / 600.0, chunks / 600.0, g_early_ns / 1e6 / 600);
            g_early_parts_n = g_early_chunks_n = g_early_rec_n = g_early_ns = 0;
        }
        // [UIT 0258] both settings: the A/B number for TOTK_VK_PRESYNC_BY_CHUNK
        VLOG("[UIT 0258] recording threads: per submission the first work reaches the GPU %.2f ms after the submission's start "
             "(%.0f%% with an early part, the rest with the commit)", g_first_ns / 1e6 / 600, g_first_early * 100.0 / 600);
        if (g_pbc_subs)
            VLOG("[UIT 0258] TOTK_VK_PRESYNC_BY_CHUNK: per submission %.1f pools brought up to date in the prologue (%.2f ms with the plan), "
                 "%.1f by the walk before their chunks (%.2f ms), %.1f at the walk's end (%.2f ms); lead %d chunks; draws that had to "
                 "sync a pool themselves: \"later from draws\" in the frame line", g_pbc_cnt[0] / (double)g_pbc_subs, g_pbc_ns[0] / 1e6 / g_pbc_subs,
                 g_pbc_cnt[1] / (double)g_pbc_subs, g_pbc_ns[1] / 1e6 / g_pbc_subs, g_pbc_cnt[2] / (double)g_pbc_subs,
                 g_pbc_ns[2] / 1e6 / g_pbc_subs, g_pbc_lead);
        g_first_ns = g_first_early = g_pbc_subs = 0;
        memset(g_pbc_cnt, 0, sizeof g_pbc_cnt); memset(g_pbc_ns, 0, sizeof g_pbc_ns);
        prev_rec = rec; wall_ns = walk_ns = chunks = glue_images = 0;
        big_ns = big_draws = pro_ns = wait_ns = glue_ns = 0;
    }
}

uint64_t mtl_last_fence(void) { return __atomic_load_n(&g_fence, __ATOMIC_SEQ_CST); }
uint64_t mtl_done(void) { pthread_mutex_lock(&g_mu); uint64_t d = g_done; pthread_mutex_unlock(&g_mu); return d; }   // completed by the GPU

int mtl_wait(uint64_t fence, uint64_t timeout_ns) {
    pthread_mutex_lock(&g_mu);
    if (g_done >= fence || !timeout_ns) { int ok = g_done >= fence; pthread_mutex_unlock(&g_mu); return ok; }
    uint64_t end = timeout_ns == UINT64_MAX ? UINT64_MAX : runtime_now_ns() + timeout_ns;
    while (g_done < fence) {
        uint64_t now = runtime_now_ns();
        if (now >= end) break;
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        uint64_t wait = end - now > 100000000 ? 100000000 : end - now;   // re-check at least every 100 ms
        ts.tv_nsec += (long)(wait % 1000000000);
        ts.tv_sec += (time_t)(wait / 1000000000) + ts.tv_nsec / 1000000000;
        ts.tv_nsec %= 1000000000;
        pthread_cond_timedwait(&g_cv, &g_mu, &ts);
    }
    int ok = g_done >= fence;
    pthread_mutex_unlock(&g_mu);
    return ok;
}
void mtl_finish(void) {   // [UIT 0045] and what waited for that work is destroyed (also at suspend and exit)
    uint64_t f = mtl_last_fence();
    mtl_wait(f, UINT64_MAX);
    if (g_ok == 1) dead_reap(f);
}
void mtl_drain(void) { mtl_finish(); }
