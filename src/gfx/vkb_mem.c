// [UIT 0075] Device memory without a vkAllocateMemory on the render thread (see vkb_mem.h).
//
// Based on valentderah/totk-pc-recomp runtime/vk_headroom.c and runtime/vk_memory.c (vk_staging's pool of its own),
// reference commit c3b9a4e. MIT License, Copyright (c) 2026 Valiantsin Dzerakh (third_party/licenses/
// valentderah-totk-recomp.txt). Changes: pthreads instead of Win32 events; the upload chunks of vkb_draw.c's ring
// instead of VMA staging buffers (the ring keeps its chunks, so the pool only makes spares ahead of need); our settings.
//
// Images (TOTK_VK_SUBALLOC=1). Each texture used to get a vkAllocateMemory of its own (8-19 ms for a new driver block
// on the render thread, thousands of textures in the open world). With the setting on they are placed in shared blocks
// by the suballocator below; a large image (over half a block) or one the driver wants alone (VkMemoryDedicated-
// Requirements) still gets memory of its own.
// [UIT 0104] The suballocator is our own C (it was AMD's Vulkan Memory Allocator, C++, whose standard library headers
// the Windows toolchain does not have), with VMA's rules where they matter: blocks of 256 MiB (a heap of 1 GiB or less:
// an eighth of it), a smaller block when the driver refuses a full one (half, a quarter, an eighth), the first memory
// type with the wanted flags (the next when that one is full), every new VkDeviceMemory reported to the headroom
// thread, an empty block kept as the type's one spare and freed when another empty one exists. Offsets and sizes are
// multiples of bufferImageGranularity; each block's free ranges are a best-fit list (vkb_suballoc.h).
//
// Headroom (TOTK_VRAM_HEADROOM, on with TOTK_VK_SUBALLOC). A new block is allocated only when no block of the memory
// type has room. A new VkDeviceMemory made by any other thread (on_allocate) wakes this thread for its
// memory type, which it then checks every 500 ms: it allocates half a default block of that type. When a block has
// that much room the allocation lands there and is freed at once. When none has, a new block is allocated here, and
// the allocation stays as that block's anchor (an empty block is freed when the type has another empty one). The previous
// anchor is freed then, which gives its half block to the render thread: at most half a block per type is held back.
// While usage plus two blocks passes 85% of the heap's budget the anchor goes and nothing new is allocated.
//
// Upload chunks (TOTK_VK_STAGING_POOL, on). vkb_draw.c's upload ring keeps 64 MiB chunks per slot and recording thread,
// made the first time each one runs out: a 64 MiB vkAllocateMemory on the render thread, many times in the first
// minutes. The pool makes two spare chunks from boot on the same thread and keeps two ready (unless usage plus two
// chunks passes 85% of the budget); the ring takes a spare instead of allocating.
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <pthread.h>

#include "compat/gfx_compat.h"
#include "host/hle.h"
#include "vkb.h"
#include "vkb_mem.h"
#include "vkb_suballoc.h"

#define VLOG(...) LOG("[vk] " __VA_ARGS__)
#define HR_PERIOD_MS 500
#define HR_SMALL_HEAP (1ull << 30)   // as VMA_SMALL_HEAP_MAX_SIZE: smaller heaps use blocks of a heap eighth
#define SPARES 2

static int env_on(const char *k, int dflt) {   // default-on settings are written as "0" when off, default-off as "1"
    const char *v = getenv(k);
    if (!v || !*v) return dflt;
    return *v != '0';
}

static struct {
    pthread_once_t once;
    int on;                    // the allocator is up (TOTK_VK_SUBALLOC)
    int guard;                 // [UIT 0133] TOTK_VK_ALLOC_GUARD (default on)
    uint64_t crowded;          // [UIT 0133] GPU-written images placed in shared blocks by the guard
    int targets;               // [UIT 0130] TOTK_VK_SUBALLOC_TARGETS=1: GPU-written images share blocks too (as 0104)
    // the background thread
    pthread_mutex_t mu;
    pthread_cond_t cv;
    int started, headroom;
    atomic_ullong types;       // memory types a thread other than this one allocated device memory of
    atomic_ullong made, other; // blocks this thread brought in; 32 MiB+ device memory other threads allocated
    void *anchor[64];          // per memory type: the half block that keeps the newest spare block from being deleted
    // spare upload chunks (mu)
    vkb_chunk_make_fn make;
    VkDeviceSize chunk;
    uint32_t chunk_type;
    int nspare, want_spares;
    struct { VkBuffer buf; VkDeviceMemory mem; uint8_t *map; } spare[SPARES];
    uint64_t spares_made, spares_taken;
} M = {.once = PTHREAD_ONCE_INIT, .mu = PTHREAD_MUTEX_INITIALIZER, .cv = PTHREAD_COND_INITIALIZER};
static __thread int t_bg;   // this is the background thread

// ---------------------------------------------------------------- budget
static void heap_budget(uint32_t heap, uint64_t *usage, uint64_t *budget) {
    *usage = 0; *budget = vkc.memory.memoryHeaps[heap].size / 10 * 8;   // VMA's estimate without the extension
    static PFN_vkGetPhysicalDeviceMemoryProperties2 get;
    if (!vkc.has_memory_budget || !vkc.gipa) return;
    if (!get) get = (PFN_vkGetPhysicalDeviceMemoryProperties2)vkc.gipa(vkc.instance, "vkGetPhysicalDeviceMemoryProperties2");
    if (!get) return;
    VkPhysicalDeviceMemoryBudgetPropertiesEXT bp = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT};
    VkPhysicalDeviceMemoryProperties2 p2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2, &bp};
    get(vkc.gpu, &p2);
    *usage = bp.heapUsage[heap]; *budget = bp.heapBudget[heap];
}
static int room(uint32_t type, VkDeviceSize reserve) {
    uint64_t u, b;
    heap_budget(vkc.memory.memoryTypes[type].heapIndex, &u, &b);
    return vkb_mem_room(u, b, reserve);
}

// ---------------------------------------------------------------- the suballocator
typedef struct sa_block {
    struct sa_block *next;
    VkDeviceMemory mem;
    uint32_t type;
    vkb_freelist fl;
} sa_block;
typedef struct {       // one allocation: a place in a block, or (blk NULL) a VkDeviceMemory of its own
    sa_block *blk;
    VkDeviceMemory mem;
    VkDeviceSize off, size;
    uint32_t type;
} sa_alloc;
static struct {
    pthread_mutex_t mu;        // the block lists and free lists (vkAllocateMemory / vkFreeMemory outside it)
    sa_block *blocks[64];      // per memory type, the newest first
    VkDeviceSize gran;         // bufferImageGranularity
    PFN_vkGetImageMemoryRequirements2 req2;
    uint64_t nblocks, ndedicated;
} SA = {.mu = PTHREAD_MUTEX_INITIALIZER};

static _Atomic int64_t g_live, g_own_live;   // [UIT 0133] vkb_mem_count
void vkb_mem_count(int delta) { atomic_fetch_add_explicit(&g_live, delta, memory_order_relaxed); }
uint64_t vkb_mem_live(void) { int64_t v = atomic_load_explicit(&g_live, memory_order_relaxed); return v > 0 ? (uint64_t)v : 0; }
uint64_t vkb_mem_dedicated_live(void) { int64_t v = atomic_load_explicit(&g_own_live, memory_order_relaxed); return v > 0 ? (uint64_t)v : 0; }
static void own_count(int delta) { vkb_mem_count(delta); atomic_fetch_add_explicit(&g_own_live, delta, memory_order_relaxed); }
static VkDeviceSize block_size(uint32_t type);
static void on_allocate(uint32_t type, VkDeviceSize size);
static VkDeviceSize align_up(VkDeviceSize v, VkDeviceSize a) { return (v + a - 1) & ~(a - 1); }

static VkDeviceMemory sa_device_memory(uint32_t type, VkDeviceSize size, VkImage dedicated) {
    VkMemoryDedicatedAllocateInfo di = {VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO, .image = dedicated};
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, dedicated ? &di : NULL, size, type};
    VkDeviceMemory m = VK_NULL_HANDLE;
    if (vk.vkAllocateMemory(vkc.device, &ai, NULL, &m) != VK_SUCCESS) return VK_NULL_HANDLE;
    vkb_mem_count(+1);
    on_allocate(type, size);
    return m;
}
// A place for `size` bytes (aligned to `align`) in a block of memory type `type`; a new block when none has room
static sa_alloc *sa_place(uint32_t type, VkDeviceSize size, VkDeviceSize align) {
    sa_alloc *a = calloc(1, sizeof *a);
    if (!a) return NULL;
    align = align > SA.gran ? align : SA.gran;
    size = align_up(size, SA.gran);
    a->type = type; a->size = size;
    pthread_mutex_lock(&SA.mu);
    for (sa_block *b = SA.blocks[type]; b; b = b->next)
        if (vkb_fl_alloc(&b->fl, size, align, &a->off)) {
            a->blk = b; a->mem = b->mem;
            pthread_mutex_unlock(&SA.mu);
            return a;
        }
    pthread_mutex_unlock(&SA.mu);
    sa_block *b = calloc(1, sizeof *b);
    VkDeviceSize bs = block_size(type);
    for (int i = 0; b && i < 4 && bs >= size; i++, bs /= 2)   // as VMA: a full block, else a half, quarter, eighth
        if ((b->mem = sa_device_memory(type, bs, VK_NULL_HANDLE))) break;
    if (!b || !b->mem || !vkb_fl_init(&b->fl, bs) || !vkb_fl_alloc(&b->fl, size, align, &a->off)) {
        if (b && b->mem) { vk.vkFreeMemory(vkc.device, b->mem, NULL); vkb_mem_count(-1); }
        if (b) vkb_fl_destroy(&b->fl);
        free(b); free(a);
        return NULL;
    }
    b->type = type;
    a->blk = b; a->mem = b->mem;
    pthread_mutex_lock(&SA.mu);
    b->next = SA.blocks[type]; SA.blocks[type] = b;
    SA.nblocks++;
    pthread_mutex_unlock(&SA.mu);
    return a;
}
static void sa_free(sa_alloc *a) {
    if (!a) return;
    if (!a->blk) { vk.vkFreeMemory(vkc.device, a->mem, NULL); own_count(-1); free(a); return; }
    VkDeviceMemory drop = VK_NULL_HANDLE;
    sa_block *dead = NULL;
    pthread_mutex_lock(&SA.mu);
    sa_block *b = a->blk;
    vkb_fl_free(&b->fl, a->off, a->size);
    if (vkb_fl_empty(&b->fl)) {   // keep one empty block per type; a second empty one is freed
        int other = 0;
        for (sa_block *o = SA.blocks[b->type]; o && !other; o = o->next) other = o != b && vkb_fl_empty(&o->fl);
        if (other) {
            for (sa_block **pp = &SA.blocks[b->type]; *pp; pp = &(*pp)->next)
                if (*pp == b) { *pp = b->next; break; }
            SA.nblocks--;
            dead = b; drop = b->mem;
        }
    }
    pthread_mutex_unlock(&SA.mu);
    if (dead) { vk.vkFreeMemory(vkc.device, drop, NULL); vkb_mem_count(-1); vkb_fl_destroy(&dead->fl); free(dead); }
    free(a);
}
// Memory for `req` in the first type of req.memoryTypeBits with the `want` flags that has room (or takes a new block)
static sa_alloc *sa_alloc_req(const VkMemoryRequirements *req, VkMemoryPropertyFlags want, VkImage dedicated_for, int dedicated) {
    for (uint32_t t = 0; t < vkc.memory.memoryTypeCount && t < 64; t++) {
        if (!(req->memoryTypeBits >> t & 1) || (vkc.memory.memoryTypes[t].propertyFlags & want) != want) continue;
        if (!dedicated && req->size <= block_size(t) / 2) {
            sa_alloc *a = sa_place(t, req->size, req->alignment ? req->alignment : 1);
            if (a) return a;
        }
        sa_alloc *a = calloc(1, sizeof *a);   // its own memory (VMA's dedicated allocation)
        if (!a) return NULL;
        if ((a->mem = sa_device_memory(t, req->size, dedicated_for))) {
            a->type = t; a->size = req->size;
            pthread_mutex_lock(&SA.mu); SA.ndedicated++; pthread_mutex_unlock(&SA.mu);
            atomic_fetch_add_explicit(&g_own_live, 1, memory_order_relaxed);   // (counted in g_live by sa_device_memory)
            return a;
        }
        free(a);
    }
    return NULL;
}

// ---------------------------------------------------------------- headroom
static void on_allocate(uint32_t type, VkDeviceSize size) {   // every new VkDeviceMemory of the suballocator
    if (!M.headroom || type >= 64) return;
    if (t_bg) { atomic_fetch_add(&M.made, 1); return; }
    if (size >= 32ull << 20) atomic_fetch_add(&M.other, 1);
    atomic_fetch_or(&M.types, 1ull << type);
    pthread_mutex_lock(&M.mu);
    pthread_cond_signal(&M.cv);
    pthread_mutex_unlock(&M.mu);
}
static VkDeviceSize block_size(uint32_t type) {
    VkDeviceSize heap = vkc.memory.memoryHeaps[vkc.memory.memoryTypes[type].heapIndex].size;
    return heap <= HR_SMALL_HEAP ? heap / 8 : 256ull << 20;   // as VMA's default preferredLargeHeapBlockSize
}
static void drop_anchor(uint32_t type) {
    if (M.anchor[type]) sa_free(M.anchor[type]);
    M.anchor[type] = NULL;
}
static void top_up(uint32_t type) {
    VkDeviceSize block = block_size(type);
    if (!room(type, 2 * block)) { drop_anchor(type); return; }
    VkMemoryRequirements req = {.size = block / 2, .alignment = 256, .memoryTypeBits = 1u << type};
    unsigned long long before = atomic_load(&M.made);
    sa_alloc *a = sa_alloc_req(&req, 0, VK_NULL_HANDLE, 0);
    if (!a) return;
    if (atomic_load(&M.made) == before) { sa_free(a); return; }   // there was room
    drop_anchor(type);
    M.anchor[type] = a;
    static int logged;
    if (logged++ < 32)
        VLOG("headroom: a %.0f MiB block of memory type %u allocated ahead (%llu so far; %llu allocations of 32 MiB or more "
             "on other threads)", (double)block / 1048576.0, type, (unsigned long long)atomic_load(&M.made),
             (unsigned long long)atomic_load(&M.other));
}

// ---------------------------------------------------------------- spare upload chunks
static void spares_fill(void) {   // M.mu held; the budget query and the chunk made outside it
    while (M.make && M.nspare < M.want_spares) {
        pthread_mutex_unlock(&M.mu);
        if (!room(M.chunk_type, (VkDeviceSize)SPARES * M.chunk)) { pthread_mutex_lock(&M.mu); break; }
        VkBuffer b = VK_NULL_HANDLE; VkDeviceMemory m = VK_NULL_HANDLE; uint8_t *p = NULL;
        int ok = M.make(M.chunk, &b, &m, &p);
        pthread_mutex_lock(&M.mu);
        if (!ok) { M.want_spares = 0; VLOG("staging pool: a spare upload chunk failed; the ring allocates its own"); break; }
        M.spare[M.nspare].buf = b; M.spare[M.nspare].mem = m; M.spare[M.nspare].map = p;
        M.nspare++; M.spares_made++;
    }
}

static void *bg_main(void *arg) {
    (void)arg;
    { int plat_thread_ecores(void); plat_thread_ecores(); }   // [UIT 0089] TOTK_HYBRID_PLACEMENT=1: efficiency cores (hybrid CPUs)
    t_bg = 1;
    uint64_t watch = 0;   // memory types seen so far
    pthread_mutex_lock(&M.mu);
    for (;;) {
        spares_fill();
        if (M.headroom) {
            watch |= atomic_exchange(&M.types, 0);
            pthread_mutex_unlock(&M.mu);
            for (uint32_t t = 0; t < vkc.memory.memoryTypeCount && t < 64; t++)
                if (watch >> t & 1) top_up(t);
            pthread_mutex_lock(&M.mu);
        }
        if (watch) {
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_nsec += HR_PERIOD_MS * 1000000L;
            if (ts.tv_nsec >= 1000000000L) ts.tv_sec++, ts.tv_nsec -= 1000000000L;
            pthread_cond_timedwait(&M.cv, &M.mu, &ts);
        } else pthread_cond_wait(&M.cv, &M.mu);
    }
    return NULL;
}
static void bg_start(void) {   // M.mu held
    if (M.started) { pthread_cond_signal(&M.cv); return; }
    pthread_t th;
    if (pthread_create(&th, NULL, bg_main, NULL)) {
        M.headroom = 0; M.want_spares = 0;
        VLOG("memory: no background thread; new blocks and upload chunks stay on the thread that needs them");
        return;
    }
    pthread_detach(th);
    M.started = 1;
}

// ---------------------------------------------------------------- images
static void init(void) {
    if (!env_on("TOTK_VK_SUBALLOC", 0)) return;
    if (!vkc.device || !vk.vkGetDeviceProcAddr) { VLOG("suballocation: no device functions; one allocation per texture as before"); return; }
    SA.gran = vkc.props.limits.bufferImageGranularity;
    if (!SA.gran || (SA.gran & (SA.gran - 1))) SA.gran = 1;
    SA.req2 = (PFN_vkGetImageMemoryRequirements2)vk.vkGetDeviceProcAddr(vkc.device, "vkGetImageMemoryRequirements2");
    M.headroom = env_on("TOTK_VRAM_HEADROOM", 1);
    M.targets = env_on("TOTK_VK_SUBALLOC_TARGETS", 0);
    M.guard = env_on("TOTK_VK_ALLOC_GUARD", 1);
    M.on = 1;
    VLOG("suballocation: textures share memory blocks (TOTK_VK_SUBALLOC, native suballocator, granularity %llu); budget %s; "
         "headroom thread %s; render targets %s", (unsigned long long)SA.gran,
         vkc.has_memory_budget ? "from VK_EXT_memory_budget" : "estimated (80% of each heap)", M.headroom ? "on" : "off",
         M.targets ? "shared too (TOTK_VK_SUBALLOC_TARGETS)" : "in memory of their own");
    if (M.headroom) { pthread_mutex_lock(&M.mu); bg_start(); pthread_mutex_unlock(&M.mu); }
}
int vkb_mem_suballoc(void) { pthread_once(&M.once, init); return M.on; }

static int own_memory(VkImage img, VkMemoryPropertyFlags want, VkDeviceMemory *mem) {   // as before UIT 0075
    VkMemoryRequirements req;
    vk.vkGetImageMemoryRequirements(vkc.device, img, &req);
    uint32_t type = UINT32_MAX;
    for (uint32_t i = 0; i < vkc.memory.memoryTypeCount && type == UINT32_MAX; i++)
        if ((req.memoryTypeBits & (1u << i)) && (vkc.memory.memoryTypes[i].propertyFlags & want) == want) type = i;
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = req.size, .memoryTypeIndex = type};
    if (type == UINT32_MAX || !vk_check(vk.vkAllocateMemory(vkc.device, &ai, NULL, mem), "image memory")) return 0;
    if (vk.vkBindImageMemory(vkc.device, img, *mem, 0) != VK_SUCCESS) { vk.vkFreeMemory(vkc.device, *mem, NULL); return 0; }
    own_count(+1);   // [UIT 0133] freed by backend_vk.c dead_reap (vkb_mem_own_freed)
    return 1;
}
int vkb_mem_image(VkImage img, VkImageUsageFlags usage, VkMemoryPropertyFlags want, VkDeviceMemory *mem, void **alloc) {
    *alloc = NULL;
    // [UIT 0133] near the driver's allocation count (maxMemoryAllocationCount, 4096 on NVIDIA under Windows): TOTK's
    // 1500-3100 imported memory pools are allocations too, so 0130's render targets / depth / storage images in memory
    // of their own could take the process past it at a burst of new targets (shrine exit). Past the limit a driver may
    // fail an allocation or, under WDDM residency, lose the device; there they take a place in a shared block (0104).
    int crowded = 0;
    if (vkb_mem_suballoc() && !vkb_mem_shared_usage(usage, M.targets) && M.guard &&
        vkb_mem_near_limit(vkb_mem_live(), vkc.props.limits.maxMemoryAllocationCount, 512)) {
        crowded = 1;
        static int said;
        if (!said++) VLOG("[UIT 0133] %llu device memory allocations of the driver's %u: render targets, depth and storage images "
                          "take places in shared blocks from now (TOTK_VK_ALLOC_GUARD=0: memory of their own regardless)",
                          (unsigned long long)vkb_mem_live(), vkc.props.limits.maxMemoryAllocationCount);
        __atomic_add_fetch(&M.crowded, 1, __ATOMIC_RELAXED);
    }
    if (vkb_mem_suballoc() && (crowded || vkb_mem_shared_usage(usage, M.targets))) {   // [UIT 0130] GPU-written: as before 0075
        VkMemoryRequirements req;
        int dedicated = 0;
        if (SA.req2) {
            VkMemoryDedicatedRequirements dr = {VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS};
            VkMemoryRequirements2 r2 = {VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2, &dr};
            VkImageMemoryRequirementsInfo2 ri = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2, NULL, img};
            SA.req2(vkc.device, &ri, &r2);
            req = r2.memoryRequirements;
            dedicated = dr.requiresDedicatedAllocation || dr.prefersDedicatedAllocation;
        } else vk.vkGetImageMemoryRequirements(vkc.device, img, &req);
        sa_alloc *a = sa_alloc_req(&req, want, img, dedicated);
        VkResult r = VK_ERROR_OUT_OF_DEVICE_MEMORY;
        if (a) {
            if ((r = vk.vkBindImageMemory(vkc.device, img, a->mem, a->off)) == VK_SUCCESS) { *mem = a->mem; *alloc = a; return 1; }
            sa_free(a);
        }
        static int said;
        if (said++ < 8) VLOG("suballocation: image memory %d; an allocation of its own", (int)r);
    }
    return own_memory(img, want, mem);
}
void vkb_mem_free(void *alloc) { if (alloc) sa_free((sa_alloc *)alloc); }
void vkb_mem_own_freed(void) { own_count(-1); }
uint64_t vkb_mem_crowded(void) { return __atomic_load_n(&M.crowded, __ATOMIC_RELAXED); }

// ---------------------------------------------------------------- upload chunks
void vkb_mem_chunk_init(vkb_chunk_make_fn make, VkDeviceSize size, uint32_t type) {
    if (!env_on("TOTK_VK_STAGING_POOL", 1) || type == UINT32_MAX) return;
    pthread_mutex_lock(&M.mu);
    if (!M.make) {
        M.make = make; M.chunk = size; M.chunk_type = type; M.want_spares = SPARES;
        VLOG("staging pool: %d spare upload chunks of %llu MiB kept ready on a background thread (TOTK_VK_STAGING_POOL)",
             SPARES, (unsigned long long)(size >> 20));
        bg_start();
    }
    pthread_mutex_unlock(&M.mu);
}
int vkb_mem_chunk(VkDeviceSize size, VkBuffer *buf, VkDeviceMemory *mem, uint8_t **map) {
    pthread_mutex_lock(&M.mu);
    int ok = M.make && size == M.chunk && M.nspare > 0;
    if (ok) {
        M.nspare--;
        *buf = M.spare[M.nspare].buf; *mem = M.spare[M.nspare].mem; *map = M.spare[M.nspare].map;
        M.spares_taken++;
        if (M.started) pthread_cond_signal(&M.cv);   // make the next one
    }
    pthread_mutex_unlock(&M.mu);
    return ok;
}
