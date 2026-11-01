// Uniform contents as of the submit call (after MacNativeGames' nvn_mtl.m usnap / mtl_sub_capture_ubos, MIT).
// On the console nvnQueueSubmitCommands returns at once and the GPU reads the constants soon after; the game, having
// handed the frame over, rebuilds its per-object and per-frame constant pools on other threads right away. Our replay
// runs later (the translation thread) and the GPU later still, so draws read constants of the next frame or bytes the game
// cleared for refilling: TOTK's light parameters (c13 of its lighting shaders) read as zeros every other frame, and on a
// camera cut of the title menu the scene went black for 4-6 frames (2026-10-24; MacNativeGames saw distant meshes
// drawn in front of the camera from the same race).
// So the uniform ranges a submission binds are copied in the submit call (game thread, mtl_sub_capture_ubos), uploaded
// at the start of its replay into device memory of the submission's slot (mtl_sub_snapshot_ubos_cap), and bindings of
// those ranges read the copy (vkb_ucap_range). A GPU write to a copied range during the submission (inline uniform update,
// buffer copy, texture-to-buffer copy, storage buffer store) drops that copy from then on: later readers see the GPU's
// result in memory, as MacNativeGames does. TOTK_UBO_SNAPSHOT=0 reads live memory as before (A/B).
// [UIT 0018] Vertex and index data race the same way: draws read vertex and index buffers straight from game memory
// (or the pool's device copy) when the replay runs, so a vertex/index range the game's CPU rewrites every frame (a
// streaming or ring buffer) could be read with the next frame's bytes or half-written ones: exploded, spiky polygons
// stretching across the screen (distant terrain, 2026-10-29). Vertex and index ranges of the submission (up to GEO_MAX
// each) that the CPU wrote recently (the write watch's page generations, without a new scan: no system calls on the game
// thread) are copied with the uniforms; static meshes (not written for many submissions) stay where they are.
// TOTK_GEO_SNAPSHOT: 1 (default) recently written ranges; 2 every range up to GEO_MAX (diagnostic, more copying);
// 0 off (as before 0018).
// "From then on" is the position of the command in the submission (vkb_seq): with several recording threads the walk of
// the submission (vkb_mt.c) drops in order and the threads only look, each at its own position.
// [UIT 0157] Drops are by the bytes written, not by the merged range. Bindings next to each other in memory are merged
// into one range (up to MERGE_MAX, 4 MiB), and a GPU write anywhere in it - one inline uniform update, one storage
// buffer store, or for ranges with vertex / index data any GPU write ever - sent every binding of the whole range to
// live memory, which by the replay holds the game's next frame: per frame ~53 ranges dropped while walking Hyrule
// (log 2026-10-31 11:58). Each drop is now a record of the bytes and the position, and a binding is served from the
// copy unless a record at or before its position overlaps it. With one recording thread inline uniform updates and
// buffer copies are also applied to the copy (vkb_ucap_gpu_write), so later bindings of those bytes keep the copy
// with the GPU's result in it, as the console would read. TOTK_VK_UCAP_FINE=0: the whole merged range (as 0156).
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include "compat/gfx_compat.h"
#include "host/hle.h"
#include "nvn/nvn_dirty.h"
#include "nvn/nvn_mtl.h"
#include "nvn/nvn_obj.h"
#include "vkb.h"
#include "vkb_capture.h"   // [UIT 0157]
#include "vkb_geodedup.h"   // [UIT 0244]
#include "vkb_gpuw.h"
#include "vkb_lock.h"
#include "vkb_rsort.h"
#include "vkb_umerge.h"   // [UIT 0141]
#include "vkb_urecycle.h"   // [UIT 0144]
#include "vkb_uwlog.h"   // [UIT 0335]

#define VLOG(...) LOG("[vk] " __VA_ARGS__)
enum { UBO_MAX = 65536, OTHER_MAX = 1 << 20, MERGE_MAX = 4 << 20, GEO_MAX = 1 << 20, GEO_TOTAL_MAX = 32 << 20, GEO_HIST = 32 };

typedef ucap_rng rng;   // { lo, hi, geo }; geo: holds vertex / index data (vkb_rsort.h)
typedef struct { int n; rng *r; size_t *off; uint8_t *bytes; uint64_t done; void *blk; size_t blk_cap; } ucap;   // [UIT 0144] r, off, bytes: one block of g_upool   // ranges sorted, disjoint; off: into bytes; done: the GPU had completed this submission at the capture

int vkb_ucap_on(void) {
    static int on = -1;
    if (on < 0) { const char *e = getenv("TOTK_UBO_SNAPSHOT"); on = !(e && *e == '0'); VLOG("uniform contents as of the submit call: %s", on ? "on" : "off (live memory, A/B)"); }
    return on;
}

int vkb_geo_snapshot(void) {
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("TOTK_GEO_SNAPSHOT");
        v = e ? atoi(e) : 1;
        if (v < 0 || v > 2) v = 1;
        VLOG("vertex / index data as of the submit call: %s", v == 2 ? "every range up to 1 MiB (TOTK_GEO_SNAPSHOT=2)" : v ? "ranges the CPU wrote recently" : "off (live memory, TOTK_GEO_SNAPSHOT=0)");
    }
    return v;
}
typedef struct { rng *r; int n, cap; } geo_list;
static _Thread_local geo_list *t_geo;   // the vertex / index ranges of the capture being collected
static void geo_put(uint64_t addr, uint64_t sz) {
    if (!t_geo || !addr || !sz || sz > GEO_MAX) return;
    geo_list *g = t_geo;
    if (g->n && g->r[g->n - 1].lo == addr && g->r[g->n - 1].hi == addr + sz) return;   // the same binding again
    if (g->n == g->cap) { g->cap = g->cap ? g->cap * 2 : 1024; if (!(g->r = realloc(g->r, sizeof *g->r * (size_t)g->cap))) die("geometry capture ranges"); }
    g->r[g->n++] = (rng){addr, addr + sz, 1};
}
static void collect(rng **v, int *n, int *cap, const uint8_t *p, int depth) {
    for (;;) {
        op_hdr h;
        memcpy(&h, p, sizeof h);
        if (h.size < 8 || (h.size & 7) || h.op >= OP_COUNT_) die("corrupt command record at %p while copying uniforms (op %u size %u)", (const void *)p, h.op, h.size);
        if (h.op == OP_END) return;
        if (h.op == OP_JUMP) { memcpy(&p, p + 8, 8); continue; }
        if (h.op == OP_CALL) { uint64_t t; memcpy(&t, p + 8, 8); if (depth < 16) collect(v, n, cap, (const uint8_t *)(uintptr_t)t, depth + 1); }
        else if (h.op == OP_BIND_VBUF && t_geo) {
            p_bind_vbuf b;
            memset(&b, 0, sizeof b);
            memcpy(&b, p + 8, sizeof b < h.size - 8 ? sizeof b : h.size - 8);
            for (int i = 0; i < b.n && i < 16 && b.first + i < 16; i++) geo_put(b.r[i].address, b.r[i].size);
        }
        else if (h.op == OP_DRAW_IDX && t_geo) {
            p_draw_idx d;
            memcpy(&d, p + 8, sizeof d);
            uint64_t isz = d.itype == 0 ? 1 : d.itype == 1 ? 2 : 4;
            if (d.count > 0) geo_put(d.ibuf, (uint64_t)d.count * isz);
        }
        else if (h.op == OP_BIND_UBO || h.op == OP_BIND_SSBO || h.op == OP_COPY_B2B) {
            // uniform buffers; storage buffers (TOTK's light list, read by its light shaders) and the sources of buffer
            // copies (its light parameters reach their uniform buffer by a GPU copy every frame): all bytes the game
            // rewrites for the next frame as soon as it has submitted this one
            uint64_t addr, sz;
            if (h.op == OP_COPY_B2B) { memcpy(&addr, p + 8, 8); memcpy(&sz, p + 24, 8); if (sz > OTHER_MAX) sz = 0; }
            else {
                const p_bind_buf *b = (const p_bind_buf *)(p + 8);
                addr = b->addr; sz = b->size;
                if (h.op == OP_BIND_UBO && sz > UBO_MAX) sz = UBO_MAX;
                if (h.op == OP_BIND_SSBO && sz > OTHER_MAX) sz = 0;
            }
            if (addr && sz) {
                if (*n == *cap) { *cap = *cap ? *cap * 2 : 4096; if (!(*v = realloc(*v, sizeof **v * (size_t)*cap))) die("uniform capture ranges"); }
                (*v)[(*n)++] = (rng){addr, addr + sz};
            }
        }
        p += h.size;
    }
}
static int rng_cmp(const void *a, const void *b) {   // the fallback when the sort's scratch cannot be allocated: the same order but ties
    const rng *x = a, *y = b;
    if (x->lo != y->lo) return x->lo < y->lo ? -1 : 1;
    return x->hi > y->hi ? -1 : x->hi < y->hi;   // the longest first
}
// [UIT 0128] by lo, the longest first, with the radix sort of vkb_rsort.h (qsort: 2.4 ms for 20000 random ranges on the box, the radix
// sort 0.39; never slower at any size, tools/tests/test_rsort.c); of ranges starting together the longest comes first, so
// the merge below keeps one entry for them (with a shorter one first it could leave two overlapping ones)
// [UIT 0134] TOTK_UCAP_QSORT=1: qsort as before 0128 (same order: by lo, the longest first; for a test only)
static int ucap_qsort(void) { static int v = -1; if (v < 0) { const char *e = getenv("TOTK_UCAP_QSORT"); v = e && e[0] == '1'; } return v; }
static void rng_sort(rng *v, int n) { if (ucap_qsort() || ucap_rng_sort(v, n)) qsort(v, (size_t)n, sizeof *v, rng_cmp); }
// [UIT 0144] vkb_urecycle.h: the capturing thread's tables and sort scratch kept for the thread's life; the captures'
// blocks recycled through g_upool (taken by the game thread, given back by the replay)
typedef struct { rng *v; int cv; rng *tmp; int ctmp; geo_list geo; ucap_dset dset; } cap_scratch;
static _Thread_local cap_scratch t_cs;
static urc_pool g_upool = URC_POOL_INIT;
// [UIT 0244] TOTK_VK_UCAP_FASTSORT (on unless 0; launcher: Performance > CPU+): the capture's sorts by lo with one radix
// sort and a tie fix (vkb_rsort.h ucap_rng_sort1_with), and the vertex / index ranges deduplicated by a hash set and
// only the ones to copy sorted (vkb_geodedup.h); the same ranges in the same order as before (test_ucap_fastsort.c).
// t254 (Lookout Landing, 4K): sort + merge 0.34 ms and vertex / index write checks 0.57 ms a frame on the game thread
// inside the submit call; box (test_ucap_fastsort.c): 10132 geometry ranges picked in 145 -> 30 us, 24329 ranges sorted in 437 -> 311 us.
static int fastsort_on(void) {
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("TOTK_VK_UCAP_FASTSORT");
        v = !(e && e[0] == '0');
        VLOG("[UIT 0244] uniform capture sorts: %s (TOTK_VK_UCAP_FASTSORT%s)", v ? "one key + ties, geometry by a hash set" : "two keys, as before",
             v ? "" : "=0");
    }
    return v;
}
static void rng_sort_keep(rng *v, int n) {   // rng_sort with t_cs.tmp as the scratch
    if (n < 2) return;
    if (ucap_qsort()) { qsort(v, (size_t)n, sizeof *v, rng_cmp); return; }   // [UIT 0134] TOTK_UCAP_QSORT=1
    if (ucap_rng_small(v, n)) return;
    if (t_cs.ctmp < n) {
        rng *t = realloc(t_cs.tmp, sizeof *t * (size_t)n);
        if (!t) { rng_sort(v, n); return; }
        t_cs.tmp = t; t_cs.ctmp = n;
    }
    if (fastsort_on()) ucap_rng_sort1_with(v, t_cs.tmp, n);   // [UIT 0244]
    else ucap_rng_sort_with(v, t_cs.tmp, n);
}

static _Atomic uint64_t g_cap_bytes, g_cap_ns, g_caps, g_geo_ranges, g_geo_bytes, g_geo_seen;
// [UIT 0145] where the capture's time goes (the per-frame line): walking the commands, the vertex / index write checks,
// sorting + merging, copying the bytes; and the ranges collected
static _Atomic uint64_t g_ph_ns[4], g_ph_ranges;
static uint64_t g_geo_hist[GEO_HIST];   // write-watch generations of the last captures (game thread only)
static unsigned g_geo_at;
void *mtl_sub_capture_ubos(const uint64_t *h, int n) {   // game thread, in the submit call
    if (!vkb_ucap_on()) return NULL;
    uint64_t t0 = runtime_now_ns();
    rng *v = t_cs.v;
    int nv = 0, cv = t_cs.cv;
    int gm = vkb_geo_snapshot();
    geo_list geo = t_cs.geo;
    geo.n = 0;
    t_geo = gm ? &geo : NULL;
    for (int i = 0; i < n; i++) collect(&v, &nv, &cv, (const uint8_t *)(uintptr_t)h[i], 0);
    t_geo = NULL;
    uint64_t t1 = runtime_now_ns();
    atomic_fetch_add(&g_ph_ns[0], t1 - t0);
    atomic_fetch_add(&g_ph_ranges, (uint64_t)(nv + geo.n));
    if (geo.n) {   // the vertex / index ranges worth a copy (written by the CPU within the last GEO_HIST submissions)
        uint64_t since = g_geo_hist[(g_geo_at + 1) % GEO_HIST], total = 0;
        atomic_fetch_add(&g_geo_seen, (uint64_t)geo.n);
        int u = fastsort_on() && gm == 1 ? ucap_dedup(geo.r, geo.n, &t_cs.dset) : -1;   // [UIT 0244]
        int ng = geo.n, picked = 0;
        if (u >= 0) {   // the distinct ranges, checked; then only those to copy sorted (the same list as below, in the same order)
            int m = 0;
            for (int i = 0; i < u; i++)
                if (nvn_dirty_written_after((const void *)(uintptr_t)geo.r[i].lo, (size_t)(geo.r[i].hi - geo.r[i].lo), since)) geo.r[m++] = geo.r[i];
            rng_sort_keep(geo.r, m);
            ng = m; picked = 1;
        } else rng_sort_keep(geo.r, geo.n);
        for (int i = 0; i < ng; i++) {
            uint64_t len = geo.r[i].hi - geo.r[i].lo;
            if (!picked) {
                if (i && geo.r[i].lo == geo.r[i - 1].lo && geo.r[i].hi == geo.r[i - 1].hi) continue;
                if (gm == 1 && !nvn_dirty_written_after((const void *)(uintptr_t)geo.r[i].lo, (size_t)len, since)) continue;
            }
            if (total + len > GEO_TOTAL_MAX) {
                static int said;
                if (said++ < 4) VLOG("[temporary] vertex / index data of one submission over %u MiB: the rest is read live", (unsigned)(GEO_TOTAL_MAX >> 20));
                break;
            }
            total += len;
            if (nv == cv) { cv = cv ? cv * 2 : 4096; if (!(v = realloc(v, sizeof *v * (size_t)cv))) die("uniform capture ranges"); }
            v[nv++] = geo.r[i];
            atomic_fetch_add(&g_geo_ranges, 1);
        }
        atomic_fetch_add(&g_geo_bytes, total);
    }
    t_cs.geo = geo;   // kept for the next capture (geo.r grown as needed)
    if (gm) { g_geo_hist[g_geo_at % GEO_HIST] = nvn_dirty_now(); g_geo_at++; }
    uint64_t t2 = runtime_now_ns();
    atomic_fetch_add(&g_ph_ns[1], t2 - t1);
    ucap *c = calloc(1, sizeof *c);
    if (!c) die("uniform capture");
    c->done = mtl_done();   // before the bytes: a submission completing meanwhile only makes this conservative
    if (nv) {
        rng_sort_keep(v, nv);
        // overlapping / touching bindings merged (up to MERGE_MAX); ranges outside every imported pool dropped
        // ([UIT 0141] vkb_umerge.h: the pool lookups under one hold of the pool lock per VKB_POOLS_HOLD_MAX)
        int k = ucap_merge(v, nv, MERGE_MAX, VKB_POOLS_HOLD_MAX, vkb_pools_hold, vkb_pools_release, vkb_in_pool_held) - 1;
        c->n = k + 1;
        uint64_t t3 = runtime_now_ns();
        atomic_fetch_add(&g_ph_ns[2], t3 - t2);
        size_t total = 0;
        for (int i = 0; i < c->n; i++) total += (size_t)(v[i].hi - v[i].lo);
        size_t tab = (sizeof *c->r + sizeof *c->off) * (size_t)c->n;   // r, then off, then the bytes (8-byte aligned)
        uint8_t *blk = urc_get(&g_upool, tab + total, &c->blk_cap);
        if (!blk) die("uniform capture bytes");
        c->blk = blk;
        c->r = (rng *)blk;
        c->off = (size_t *)(blk + sizeof *c->r * (size_t)c->n);
        c->bytes = blk + tab;
        memcpy(c->r, v, sizeof *c->r * (size_t)c->n);
        total = 0;
        for (int i = 0; i < c->n; i++) { c->off[i] = total; total += (size_t)(v[i].hi - v[i].lo); }
        for (int i = 0; i < c->n; i++) memcpy(c->bytes + c->off[i], (const void *)(uintptr_t)v[i].lo, (size_t)(v[i].hi - v[i].lo));
        atomic_fetch_add(&g_cap_bytes, total);
        atomic_fetch_add(&g_ph_ns[3], runtime_now_ns() - t3);
    }
    t_cs.v = v; t_cs.cv = cv;   // kept for the next capture
    atomic_fetch_add(&g_cap_ns, runtime_now_ns() - t0);
    atomic_fetch_add(&g_caps, 1);
    return c;
}

// The replay's table: the captured ranges in device memory of the slot (per-slot buffer, grown as needed).
// drop_at: the first position with a drop record (UINT64_MAX: none); whole_at: the first position the whole range is
// dropped (TOTK_VK_UCAP_FINE=0, or the records ran out); head: its newest drop record (-1: none)
typedef struct { uint64_t lo, hi; VkDeviceSize dev; _Atomic uint64_t drop_at, whole_at; _Atomic int head; } urng;
// [UIT 0157] a drop record: the bytes [lo, hi) the GPU writes from position at; gpu_ok: the write was applied to the
// copy in device memory as well (only CPU reads of the captured bytes must not use it). Written by the one thread that
// drops (the replay, or the walk of vkb_mt.c), published by the release store of head; never changed after.
typedef struct { uint64_t lo, hi, at; int next; uint8_t gpu_ok; } udrop;
enum { UDROP_MAX = 1 << 16 };
static udrop *g_ud;
static int g_nud;
static uint64_t g_ud_records, g_ud_overflow, g_applied;
static vkb_cnt g_fine_saved;
static int fine_on(void) {
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("TOTK_VK_UCAP_FINE");
        on = !(e && *e == '0');
        VLOG("[UIT 0157] GPU writes drop only the bytes they write from the submit-time copy: %s (TOTK_VK_UCAP_FINE%s)",
             on ? "on; with one recording thread updates and copies also go into the copy" : "off, the whole merged range as 0156", on ? "" : "=0");
    }
    return on;
}
static urng *g_u;
static int g_nu, g_capu, g_uslot = -1;
static const uint8_t *g_ubytes;   // the captured bytes (CPU reads of pointers kept in uniform buffers)
static size_t *g_uoff;
static ucap *g_cur;
typedef struct { VkBuffer buf; VkDeviceMemory mem; VkDeviceSize cap; } arena_t;
static arena_t g_arena[VKB_SLOTS];
static vkb_cnt g_hits;
// [UIT 0335] the bytes of GPU writes (inline updates, buffer copies) in this submission, for CPU reads (vkb_uwlog.h)
static uwlog g_wl;
static _Thread_local uwl_view t_wv;
static uint64_t g_wl_reads, g_wl_said, g_wl_over_said;
static int gpuview_on(void) {
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("TOTK_VK_UCAP_GPUVIEW");
        on = !(e && *e == '0');
        VLOG("[UIT 0335] pointers read from uniform memory see the GPU's earlier writes in the submission: %s (TOTK_VK_UCAP_GPUVIEW%s)",
             on ? "on" : "off, the bytes as of the submit call", on ? "" : "=0");
    }
    return on;
}
static void udrop_whole(urng *u, uint64_t at) {
    if (atomic_load_explicit(&u->whole_at, memory_order_relaxed) > at) atomic_store_explicit(&u->whole_at, at, memory_order_release);
}
// a GPU write of [lo, hi) at position at in range u (the dropping thread only)
static int udrop_add(urng *u, uint64_t lo, uint64_t hi, uint64_t at, int gpu_ok) {
    if (lo < u->lo) lo = u->lo;
    if (hi > u->hi) hi = u->hi;
    if (hi <= lo) return 0;
    uint64_t was = atomic_load_explicit(&u->drop_at, memory_order_relaxed);
    if (!fine_on()) { udrop_whole(u, at); gpu_ok = 0; }
    else {
        int h = atomic_load_explicit(&u->head, memory_order_relaxed);
        if (h >= 0 && g_ud[h].lo <= lo && g_ud[h].hi >= hi && g_ud[h].at <= at && g_ud[h].gpu_ok <= gpu_ok) return 0;   // covered
        if (!g_ud && !(g_ud = malloc(sizeof *g_ud * UDROP_MAX))) die("uniform copy drop records");
        if (g_nud >= UDROP_MAX) { udrop_whole(u, at); g_ud_overflow++; gpu_ok = 0; }
        else {
            int k = g_nud++;
            g_ud[k] = (udrop){lo, hi, at, h, (uint8_t)gpu_ok};
            atomic_store_explicit(&u->head, k, memory_order_release);
            g_ud_records++;
        }
    }
    if (was > at) atomic_store_explicit(&u->drop_at, at, memory_order_release);
    if (was == UINT64_MAX && !gpu_ok) return 1;
    return 0;
}
// whether [lo, hi) of range u is served from the copy at this thread's position; cpu: for a CPU read of the captured
// bytes (writes applied only to the device copy count)
static int served(const urng *u, uint64_t lo, uint64_t hi, int cpu, int *fine) {
    uint64_t seq = vkb_seq();
    if (atomic_load_explicit(&u->drop_at, memory_order_acquire) > seq) return 1;
    if (atomic_load_explicit(&u->whole_at, memory_order_acquire) <= seq) return 0;
    for (int j = atomic_load_explicit(&u->head, memory_order_acquire); j >= 0; j = g_ud[j].next) {
        const udrop *d = &g_ud[j];
        if (d->at <= seq && d->lo < hi && lo < d->hi && (cpu || !d->gpu_ok)) return 0;
    }
    if (fine) *fine = 1;
    return 1;
}
// whether a byte of range u is under a drop record (or a whole drop) at this thread's position
static int dropped_byte(const urng *u, uint64_t a) {
    uint64_t seq = vkb_seq();
    if (atomic_load_explicit(&u->drop_at, memory_order_acquire) > seq) return 0;
    if (atomic_load_explicit(&u->whole_at, memory_order_acquire) <= seq) return 1;
    for (int j = atomic_load_explicit(&u->head, memory_order_acquire); j >= 0; j = g_ud[j].next)
        if (g_ud[j].at <= seq && g_ud[j].lo <= a && a < g_ud[j].hi) return 1;
    return 0;
}
static uint64_t g_drops, g_geo_gpu;
// GPU writes of submissions recorded so far (inline updates, copies, stores). A capture taken before such a write
// completed holds the bytes from before it, so ranges it overlaps read memory (the GPU timeline) instead of the copy.
// [UIT 0027] Kept by the capture's own completion point, not the replay's: the game runs submissions ahead of the
// translation thread, so a write completed between the capture and its replay used to be forgotten and the stale copy
// served. Vertex / index ranges also read memory after a GPU write (0027-0037: within 600 submissions): with the pool's
// device copy its bytes may never reach game memory (pages the CPU wrote meanwhile keep the CPU's), so a copy of game
// memory there is the CPU's old data - torn meshes. Their copy is only worth it for data only the CPU writes.
// [UIT 0038] ... and that holds after 600 submissions too: the 600-submission window (0027-0037) assumed a write that old
// is in game memory "or the CPU's since", but a page the CPU also wrote keeps the CPU's bytes (wb_merge), and the CPU
// writing one page of a vertex range does not mean it rewrote the GPU's part. Once 0030's scans saw every written page,
// such ranges (GPU-filled grass data next to CPU-written pages: grass gone shortly after loading, 2026-10-30) were
// copied from game memory from 600 submissions after the GPU filled them on, every submission.
// TOTK_GEO_GPU_KEEP: submissions a GPU write keeps its vertex / index range read from memory; unset or 0: always.
static gpuw_set g_gw;
static uint64_t geo_gpu_keep(void) {
    static uint64_t v;
    static int got;
    if (!got) {
        const char *e = getenv("TOTK_GEO_GPU_KEEP");
        long long n = e ? atoll(e) : 0;
        v = n > 0 ? (uint64_t)n : GPUW_FOREVER;
        got = 1;
        if (n > 0) VLOG("vertex / index ranges the GPU wrote: read from memory for %lld submissions after the write (TOTK_GEO_GPU_KEEP)", n);
        else VLOG("vertex / index ranges the GPU wrote: always read from memory, never copied at submit");
    }
    return v;
}

static int arena_get(int slot, VkDeviceSize need) {
    arena_t *a = &g_arena[slot];
    if (a->cap >= need) return 1;
    if (a->buf) { vk.vkDestroyBuffer(vkc.device, a->buf, NULL); vk.vkFreeMemory(vkc.device, a->mem, NULL); }   // the slot's last submission is done
    *a = (arena_t){0};
    VkDeviceSize cap = 1u << 20;
    while (cap < need) cap *= 2;
    VkBufferCreateInfo bi = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = cap,
        .usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                 VK_BUFFER_USAGE_TRANSFER_SRC_BIT |   // [UIT 0157] buffer copies read the copy (mtl_sub_copy's source as of the submit call)
                 VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                 VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT};
    if (!vk_check(vk.vkCreateBuffer(vkc.device, &bi, NULL, &a->buf), "uniform copy buffer")) return 0;
    VkMemoryRequirements req;
    vk.vkGetBufferMemoryRequirements(vkc.device, a->buf, &req);
    uint32_t type = UINT32_MAX;
    for (uint32_t i = 0; i < vkc.memory.memoryTypeCount && type == UINT32_MAX; i++)
        if ((req.memoryTypeBits & (1u << i)) && (vkc.memory.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) type = i;
    VkMemoryAllocateFlagsInfo fl = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO, .flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT};
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &fl, .allocationSize = req.size, .memoryTypeIndex = type};
    if (type == UINT32_MAX || !vk_check(vk.vkAllocateMemory(vkc.device, &ai, NULL, &a->mem), "uniform copy memory")) {
        vk.vkDestroyBuffer(vkc.device, a->buf, NULL); *a = (arena_t){0}; return 0;
    }
    vk.vkBindBufferMemory(vkc.device, a->buf, a->mem, 0);
    a->cap = cap;
    return 1;
}

static void snap_cb(void *ctx, uint64_t lo, uint64_t hi) { udrop_add(ctx, lo, hi, 0, 0); }
static void snap_drop(urng *u, uint64_t done, int recent) {
    if (!fine_on()) { udrop_add(u, u->lo, u->hi, 0, 0); return; }
    gpuw_each(&g_gw, u->lo, u->hi, done, recent, geo_gpu_keep(), snap_cb, u);
    if (vkb_capture_on()) vkb_capture_note("submission start: copy %#llx..%#llx dropped where GPU-written%s (%d drop records so far)",
                                           (unsigned long long)u->lo, (unsigned long long)u->hi, recent ? " ever (vertex / index data)" : " after the capture", g_nud);
}
void mtl_sub_snapshot_ubos_cap(const uint64_t *h, int n, void *capv) {   // replay thread, the submission just begun
    ucap *c = capv ? capv : mtl_sub_capture_ubos(h, n);   // without the translation thread: the replay is in the submit call
    if (g_cur) { urc_put(&g_upool, g_cur->blk, g_cur->blk_cap); free(g_cur); g_cur = NULL; }   // [UIT 0144] back to the pool
    g_nu = 0; g_uslot = -1; g_ubytes = NULL; g_uoff = NULL;
    g_nud = 0;   // [UIT 0157] the previous submission's records (its recording has finished)
    uwl_reset(&g_wl);   // [UIT 0335] and its GPU writes
    if (!c) return;
    g_cur = c;
    if (!c->n || vkb_slot() < 0) return;
    if (g_capu < c->n) { g_capu = c->n; if (!(g_u = realloc(g_u, sizeof *g_u * (size_t)g_capu))) die("uniform copy table"); }
    gpuw_retire(&g_gw, c->done, geo_gpu_keep());   // captures are in submission order: c->done never goes back
    VkDeviceSize at = 0;   // each range keeps its address's position within 256 bytes: bindings stay aligned as in the pool
    for (int i = 0; i < c->n; i++) {
        at = ((at + 255) & ~(VkDeviceSize)255) + (c->r[i].lo & 255);
        g_u[i] = (urng){c->r[i].lo, c->r[i].hi, at, UINT64_MAX, UINT64_MAX, -1};
        // a GPU write the capture predates; [UIT 0157] only the bytes it wrote (gpuw_each), not the whole merged range
        if (gpuw_inflight(&g_gw, c->r[i].lo, c->r[i].hi, c->done)) { snap_drop(&g_u[i], c->done, 0); g_drops++; }
        else if (c->r[i].geo && gpuw_recent(&g_gw, c->r[i].lo, c->r[i].hi, c->done, geo_gpu_keep())) { snap_drop(&g_u[i], c->done, 1); g_geo_gpu++; }
        at += c->r[i].hi - c->r[i].lo;
    }
    int slot = vkb_slot();
    if (!arena_get(slot, at)) return;
    VkBuffer rb;
    VkDeviceSize ro;
    uint8_t *p = vkb_ring_alloc(at, 256, &rb, &ro);
    for (int i = 0; i < c->n; i++) memcpy(p + g_u[i].dev, c->bytes + c->off[i], (size_t)(c->r[i].hi - c->r[i].lo));
    VkCommandBuffer cb = vkb_cmd();
    VkBufferCopy r = {ro, 0, at};
    vk.vkCmdCopyBuffer(cb, rb, g_arena[slot].buf, 1, &r);
    VkMemoryBarrier2 mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, .srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT, .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
                           .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT};
    VkDependencyInfo dep = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &mb};
    vk.vkCmdPipelineBarrier2(cb, &dep);
    g_nu = c->n; g_uslot = slot; g_ubytes = c->bytes; g_uoff = c->off;
}

static int find(uint64_t a) {   // the first range ending above a
    int lo = 0, hi = g_nu;
    while (lo < hi) { int m = (lo + hi) / 2; if (g_u[m].hi <= a) lo = m + 1; else hi = m; }
    return lo;
}
int vkb_ucap_range(uint64_t addr, uint64_t size, VkBuffer *buf, VkDeviceSize *off) {
    if (!g_nu || g_uslot != vkb_slot()) return 0;   // another slot (a split submission): its copy is not this one's
    int i = find(addr);
    int fine = 0;
    if (i >= g_nu || addr < g_u[i].lo || addr + size > g_u[i].hi || !served(&g_u[i], addr, addr + size, 0, &fine)) return 0;
    *buf = g_arena[g_uslot].buf;
    *off = g_u[i].dev + (addr - g_u[i].lo);
    vkb_cnt_add(g_hits, 1);
    if (fine) vkb_cnt_add(g_fine_saved, 1);
    return 1;
}
int vkb_ucap_why(uint64_t addr, uint64_t size) {   // development: 0 in the copy, 1 not captured, 2 dropped by a GPU write, 3 other slot
    if (!g_nu) return 1;
    if (g_uslot != vkb_slot()) return 3;
    int i = find(addr);
    if (i >= g_nu || addr < g_u[i].lo || addr + size > g_u[i].hi) return 1;
    return served(&g_u[i], addr, addr + size, 0, NULL) ? 0 : 2;
}
// [UIT 0157] a binding against the copy (diagnostics, vkb_draw.c): see vkb.h
void vkb_ucap_compare(uint64_t addr, uint64_t size, vkb_ucap_cmp *r) {
    memset(r, 0, sizeof *r);
    r->first = UINT32_MAX;
    r->why = vkb_ucap_why(addr, size);
    if (r->why == 3 || !g_nu || !addr || !size) return;
    int i = find(addr);
    if (i >= g_nu || addr < g_u[i].lo || addr + size > g_u[i].hi) return;
    const urng *u = &g_u[i];
    r->cap = g_ubytes + g_uoff[i] + (addr - u->lo);
    const uint8_t *live = (const uint8_t *)(uintptr_t)addr;
    for (uint64_t k = 0; k < size; k++) {
        if (r->cap[k] == live[k]) continue;
        r->ndiff++;
        if (!dropped_byte(u, addr + k)) { r->nout++; if (r->first == UINT32_MAX) r->first = (uint32_t)k; }
    }
    if (r->first == UINT32_MAX && r->ndiff) for (uint64_t k = 0; k < size; k++) if (r->cap[k] != live[k]) { r->first = (uint32_t)k; break; }
}
static void ucap_base(uint64_t addr, void *out, size_t n) {
    if (g_nu) {
        int i = find(addr);
        if (i < g_nu && addr >= g_u[i].lo && addr + n <= g_u[i].hi && served(&g_u[i], addr, addr + n, 1, NULL)) { memcpy(out, g_ubytes + g_uoff[i] + (addr - g_u[i].lo), n); return; }
    }
    memcpy(out, (const void *)(uintptr_t)addr, n);
}
void vkb_ucap_read(uint64_t addr, void *out, size_t n) {   // CPU reads of uniform memory (pointers in uniform buffers)
    ucap_base(addr, out, n);
    // [UIT 0335] with the GPU's writes before this position laid over (the copy and game memory hold neither)
    if (gpuview_on() && uwl_read(&g_wl, &t_wv, vkb_seq(), addr, out, n)) {
        g_wl_reads++;
        if (g_wl_said < 4 && __atomic_fetch_add(&g_wl_said, 1, __ATOMIC_RELAXED) < 4)
            VLOG("[UIT 0335] uniform memory %#llx+%llu read by the CPU at position %llu: bytes the GPU wrote there earlier in the "
                 "submission used (before: the bytes as of the submit call)", (unsigned long long)addr, (unsigned long long)n,
                 (unsigned long long)vkb_seq());
    }
}
// [UIT 0335] a GPU write to uniform memory at this position: an inline update (data) or a copy from src (CPU view)
static void wl_put(uint64_t dst, uint64_t n, const uint8_t *data) {
    if (!uwl_add(&g_wl, vkb_seq(), dst, n, data) && !g_wl_over_said++)
        VLOG("[UIT 0335] a GPU write of %llu bytes at %#llx not kept for CPU reads (writes over %d bytes, or more than %d writes / "
             "%d MiB in one submission, are not)", (unsigned long long)n, (unsigned long long)dst, UWL_WMAX, UWL_ENTS, UWL_BYTES >> 20);
}
void vkb_ucap_note_write(uint64_t dst, uint64_t n, const void *data) {
    if (vkb_mt_recording() || !n || !gpuview_on()) return;   // with several recording threads: the walk logged it
    wl_put(dst, n, data);
}
void vkb_ucap_note_copy(uint64_t src, uint64_t dst, uint64_t n) {
    if (vkb_mt_recording() || !n || !gpuview_on()) return;
    if (n > UWL_WMAX) { wl_put(dst, n, NULL); return; }
    uint8_t buf[UWL_WMAX];
    vkb_ucap_read(src, buf, (size_t)n);   // the source as the GPU reads it here
    wl_put(dst, n, buf);
}
void vkb_ucap_gpuview_stats(uint64_t *logged, uint64_t *over, uint64_t *reads) { *logged = g_wl.logged; *over = g_wl.over; *reads = g_wl_reads; }
void vkb_ucap_drop(uint64_t addr, uint64_t len) {   // a GPU write to [addr, addr+len): copies it overlaps are dropped
    if (vkb_mt_recording()) return;   // the walk of the submission dropped it already (vkb_mt.c)
    uint64_t at = vkb_seq();
    for (int i = find(addr); i < g_nu && g_u[i].lo < addr + len; i++) g_drops += (uint64_t)udrop_add(&g_u[i], addr, addr + len, at, 0);
    if (vkb_capture_on()) vkb_capture_note("GPU write %#llx+%llu at position %llu: copied bytes there dropped", (unsigned long long)addr,
                                           (unsigned long long)len, (unsigned long long)at);
    if (!vkb_ucap_on()) return;
    gpuw_add(&g_gw, addr, addr + len, mtl_last_fence() + 1);
}
// [UIT 0157] an inline update (data) or a buffer copy (from sb at so) to [dst, dst+n), recorded in cb between full
// barriers by the caller: with one recording thread also applied to the copy, so later bindings keep it (see the top)
void vkb_ucap_gpu_write(VkCommandBuffer cb, uint64_t dst, uint64_t n, const uint8_t *data, VkBuffer sb, VkDeviceSize so) {
    if (vkb_mt_recording()) return;   // the walk of the submission dropped it already (vkb_mt.c)
    if (!fine_on() || vkb_mt_on() || !cb || !g_nu || g_uslot != vkb_slot() || (!data && !sb)) { vkb_ucap_drop(dst, n); return; }
    uint64_t at = vkb_seq();
    VkBuffer ab = g_arena[g_uslot].buf;
    int applied = 0, not = 0;
    for (int i = find(dst); i < g_nu && g_u[i].lo < dst + n; i++) {
        urng *u = &g_u[i];
        uint64_t lo = dst > u->lo ? dst : u->lo, hi = dst + n < u->hi ? dst + n : u->hi;
        if (hi <= lo) continue;
        VkDeviceSize d = u->dev + (lo - u->lo), len = hi - lo;
        int ok = 0;
        if (data) {
            if (!(d & 3) && !(len & 3)) {   // vkCmdUpdateBuffer: 4-byte aligned, at most 65536 bytes per call
                for (VkDeviceSize k = 0; k < len; k += 65536) vk.vkCmdUpdateBuffer(cb, ab, d + k, len - k < 65536 ? len - k : 65536, data + (lo - dst) + k);
                ok = 1;
            }
        } else {
            VkDeviceSize s0 = so + (lo - dst);
            if (!(sb == ab && s0 < d + len && d < s0 + len)) {   // not within the copy onto itself
                VkBufferCopy r = {s0, d, len};
                vk.vkCmdCopyBuffer(cb, sb, ab, 1, &r);
                ok = 1;
            }
        }
        g_drops += (uint64_t)udrop_add(u, lo, hi, at, ok);
        if (ok) { g_applied++; applied++; } else not++;
    }
    if (vkb_capture_on()) vkb_capture_note("GPU %s %#llx+%llu at position %llu: applied to %d copied ranges, dropped in %d",
                                           data ? "uniform update" : "buffer copy to", (unsigned long long)dst, (unsigned long long)n,
                                           (unsigned long long)at, applied, not);
    if (vkb_ucap_on()) gpuw_add(&g_gw, dst, dst + n, mtl_last_fence() + 1);
}
void vkb_ucap_fine_stats(uint64_t *records, uint64_t *overflow, uint64_t *applied, uint64_t *saved) {   // [UIT 0157]
    *records = g_ud_records; *overflow = g_ud_overflow; *applied = g_applied; *saved = vkb_cnt_sum(g_fine_saved);
}
void vkb_ucap_phase_stats(uint64_t ns[4], uint64_t *ranges) {   // [UIT 0145]
    for (int i = 0; i < 4; i++) ns[i] = atomic_load(&g_ph_ns[i]);
    *ranges = atomic_load(&g_ph_ranges);
}
void vkb_ucap_stats(uint64_t *caps, uint64_t *bytes, uint64_t *ns, uint64_t *hits, uint64_t *drops) {
    *caps = g_caps; *bytes = g_cap_bytes; *ns = g_cap_ns; *hits = vkb_cnt_sum(g_hits); *drops = g_drops;
}
void vkb_ucap_geo_stats(uint64_t *seen, uint64_t *ranges, uint64_t *bytes, uint64_t *gpu) {
    *seen = g_geo_seen; *ranges = g_geo_ranges; *bytes = g_geo_bytes; *gpu = g_geo_gpu;
}
