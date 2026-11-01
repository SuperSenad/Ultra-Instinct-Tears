// [Windows port] Dirty-page tracking of guest memory pools (the API of MacNativeGames' nvn_dirty.h) through the guest
// window's write watch (MEM_WRITE_WATCH, src/runtime/platform/windows.c plat_written_pages) instead of write-protecting
// pages and catching the faults. The OS marks every page anything writes (the game, host code, file reads by the kernel),
// so there are no protection changes, no fault handler and no host pre-write hook.
// As in MacNativeGames, each page of a tracked pool has a generation: the generation of its last observed write.
// nvn_dirty_since() collects the pages written since the last look (resetting their watch bits) and gives them a new
// generation, then compares the range's generations with the caller's stamp.
#include <sched.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include "../host/dm.h"
#include "nvn_dirty.h"

int plat_written_pages(uint64_t addr, uint64_t size, uint64_t *pages, int max, uint64_t *page);

#define MAX_RANGE 16384   // MacNativeGames: about 3,900 live pools in TOTK's world
typedef struct { uintptr_t base, end; _Atomic uint64_t *gen; _Atomic int live, watched; _Atomic uint64_t epoch; } range_t;   // watched: the OS reports its writes
static range_t g_r[MAX_RANGE];
static _Atomic int g_nr, g_live, g_live_peak;
// [UIT 0025] A scan resets the pages' watch bits and only then gives them their generation: between the two, a reader
// of the generations (another thread's texture check or pool copy) finds the pages unchanged, and as the OS no longer
// reports them, the write was lost for it for good - a pool's device copy kept old vertices (exploded geometry), a
// texture its old texels. With the texture checks no longer under the images lock (0022) and six recording threads
// (0019), it went from rare to everywhere after streaming (leaving a shrine). A scan and the reads of its generations
// are now one step per range (striped locks by range slot), and the epoch's whole-range scan is marked done after it
// is done, not before.
#define SCAN_LOCKS 64
static pthread_mutex_t g_scan_mu[SCAN_LOCKS] = {[0 ... SCAN_LOCKS - 1] = PTHREAD_MUTEX_INITIALIZER};
static _Thread_local uint64_t t_stamp;   // nvn_dirty_last_stamp: the generation at the last look, under the lock
static _Atomic uint64_t g_now = 1, g_adds_total, g_scans, g_scan_pages, g_gpu_raced;
static pthread_mutex_t g_slot_mu = PTHREAD_MUTEX_INITIALIZER;
static int g_watch = -1;   // write watch available (the window was reserved with it)

static _Thread_local int t_last;   // the range this thread found last (most lookups repeat it)

// [UIT 0240] TOTK_DIRTY_INDEX (Performance > CPU+ > Find memory pools through an index, default On; 0 = as before).
// A lookup that missed t_last went through every slot in turn: ~3,500 slots with 2,921 live pools at Lookout Landing,
// about 2.3 us a lookup (box bench tools/tests/test_dirty_index.c). The pool copies at a submission's start look up
// ~440 pools in address order (nearly all misses: slots are in the order the pools were made), the game thread's
// vertex / index write checks ~10,000 ranges, the recording threads' texture checks two lookups each: the translation
// thread's prologue (1.3 ms, "write watch" in the pool copies line) was mostly this, and so was much of the game
// thread's 0.57 ms of write checks. Now: the live ranges sorted by base with the largest end so far (nested or
// overlapping ranges are found too), read without a lock under a sequence count; a binary search finds the range.
// Writers (a pool added or removed, g_slot_mu) update it in place. Every range found is checked against its slot; a
// reader that raced a writer (the count moved) or would walk more than IX_WALK entries goes through the slots as
// before, so every answer is one the old search could give.
typedef struct { _Atomic uintptr_t base, end, maxend; _Atomic int slot; } ix_ent;
static ix_ent g_ix[MAX_RANGE];
static _Atomic int g_ix_n;
static _Atomic uint64_t g_ix_seq;   // odd: being changed
static _Atomic uint64_t g_ix_hits, g_ix_none, g_ix_slow;
static _Atomic int g_ix_on = -1;
#define IX_WALK 16
static int ix_on(void) {   // first asked by the first add (g_slot_mu held), before any lookup can use the index
    int v = atomic_load_explicit(&g_ix_on, memory_order_relaxed);
    if (v < 0) {
        const char *e = getenv("TOTK_DIRTY_INDEX");
        v = !(e && (*e == '0' || !strcmp(e, "off")));
        LOG("dirty tracking: TOTK_DIRTY_INDEX=%d: %s", v, v ? "memory pools are found by address through a sorted index (UIT 0240)"
                                                            : "memory pools are found by going through every slot (as before 0240)");
        atomic_store(&g_ix_on, v);
    }
    return v;
}
// [UIT 0242] a slot's base and end are read without the lock by the lookups (as before 0240) while nvn_dirty_add_range
// may be reusing that slot for a new pool; relaxed atomic loads and stores (the same plain 8-byte moves on x86-64) make
// that a defined read of the old or the new value, never a torn one. TSAN reported it in test_dirty_index's churn run.
#define RBASE(r) __atomic_load_n(&(r)->base, __ATOMIC_RELAXED)
#define REND(r) __atomic_load_n(&(r)->end, __ATOMIC_RELAXED)
static range_t *find_slow(uintptr_t a) {
    int n = atomic_load(&g_nr);
    for (int i = 0; i < n; i++)
        if (atomic_load_explicit(&g_r[i].live, memory_order_acquire) && a >= RBASE(&g_r[i]) && a < REND(&g_r[i])) { t_last = i; return &g_r[i]; }
    return NULL;
}
// 1: answered (*out the range, or NULL: no live range holds a); 0: ask the slots
static int ix_find(uintptr_t a, range_t **out) {
    uint64_t s1 = atomic_load_explicit(&g_ix_seq, memory_order_acquire);
    if (s1 & 1) { atomic_fetch_add_explicit(&g_ix_slow, 1, memory_order_relaxed); return 0; }
    int n = atomic_load_explicit(&g_ix_n, memory_order_relaxed);
    if (n < 0 || n > MAX_RANGE) n = 0;
    int lo = 0, hi = n - 1, f = -1;   // f: the last entry with base <= a
    while (lo <= hi) {
        int m = (lo + hi) / 2;
        if (atomic_load_explicit(&g_ix[m].base, memory_order_relaxed) <= a) { f = m; lo = m + 1; } else hi = m - 1;
    }
    int k = -1, steps = 0, slow = 0;
    for (int i = f; i >= 0; i--) {   // the closest first; done once nothing at or below i reaches past a
        if (atomic_load_explicit(&g_ix[i].maxend, memory_order_relaxed) <= a) break;
        if (++steps > IX_WALK) { slow = 1; break; }
        if (a < atomic_load_explicit(&g_ix[i].end, memory_order_relaxed)) { k = atomic_load_explicit(&g_ix[i].slot, memory_order_relaxed); break; }
    }
    atomic_thread_fence(memory_order_acquire);
    if (slow || atomic_load_explicit(&g_ix_seq, memory_order_relaxed) != s1) { atomic_fetch_add_explicit(&g_ix_slow, 1, memory_order_relaxed); return 0; }
    if (k < 0) { atomic_fetch_add_explicit(&g_ix_none, 1, memory_order_relaxed); *out = NULL; return 1; }
    if (k >= MAX_RANGE || !atomic_load_explicit(&g_r[k].live, memory_order_acquire) || a < RBASE(&g_r[k]) || a >= REND(&g_r[k])) {
        atomic_fetch_add_explicit(&g_ix_slow, 1, memory_order_relaxed);   // (a writer between: the slots decide)
        return 0;
    }
    atomic_fetch_add_explicit(&g_ix_hits, 1, memory_order_relaxed);
    t_last = k;
    *out = &g_r[k];
    return 1;
}
// g_slot_mu held: the largest end so far, again from entry i
static void ix_maxend_from(int i, int n) {
    uintptr_t m = i ? atomic_load_explicit(&g_ix[i - 1].maxend, memory_order_relaxed) : 0;
    for (; i < n; i++) {
        uintptr_t e = atomic_load_explicit(&g_ix[i].end, memory_order_relaxed);
        if (e > m) m = e;
        atomic_store_explicit(&g_ix[i].maxend, m, memory_order_relaxed);
    }
}
static void ix_copy(int to, int from) {
    atomic_store_explicit(&g_ix[to].base, atomic_load_explicit(&g_ix[from].base, memory_order_relaxed), memory_order_relaxed);
    atomic_store_explicit(&g_ix[to].end, atomic_load_explicit(&g_ix[from].end, memory_order_relaxed), memory_order_relaxed);
    atomic_store_explicit(&g_ix[to].slot, atomic_load_explicit(&g_ix[from].slot, memory_order_relaxed), memory_order_relaxed);
}
static void ix_write_begin(void) {   // seqlock writer: odd, then the stores
    atomic_store_explicit(&g_ix_seq, atomic_load_explicit(&g_ix_seq, memory_order_relaxed) + 1, memory_order_relaxed);
    atomic_thread_fence(memory_order_release);
}
static void ix_write_end(void) { atomic_store_explicit(&g_ix_seq, atomic_load_explicit(&g_ix_seq, memory_order_relaxed) + 1, memory_order_release); }
static void ix_insert(int slot) {   // g_slot_mu held, the slot's range set and live
    if (!ix_on()) return;
    uintptr_t b = g_r[slot].base;
    int n = atomic_load_explicit(&g_ix_n, memory_order_relaxed), pos = n;
    if (n >= MAX_RANGE) return;   // (one entry per live slot: cannot happen)
    int lo = 0, hi = n - 1;
    while (lo <= hi) { int m = (lo + hi) / 2; if (atomic_load_explicit(&g_ix[m].base, memory_order_relaxed) > b) { pos = m; hi = m - 1; } else lo = m + 1; }
    ix_write_begin();
    for (int i = n; i > pos; i--) ix_copy(i, i - 1);
    atomic_store_explicit(&g_ix[pos].base, b, memory_order_relaxed);
    atomic_store_explicit(&g_ix[pos].end, g_r[slot].end, memory_order_relaxed);
    atomic_store_explicit(&g_ix[pos].slot, slot, memory_order_relaxed);
    atomic_store_explicit(&g_ix_n, n + 1, memory_order_relaxed);
    ix_maxend_from(pos, n + 1);
    ix_write_end();
}
static void ix_remove(int slot) {   // g_slot_mu held
    if (atomic_load_explicit(&g_ix_on, memory_order_relaxed) <= 0) return;
    int n = atomic_load_explicit(&g_ix_n, memory_order_relaxed), pos = -1;
    for (int i = 0; i < n; i++) if (atomic_load_explicit(&g_ix[i].slot, memory_order_relaxed) == slot) { pos = i; break; }
    if (pos < 0) return;
    ix_write_begin();
    for (int i = pos; i < n - 1; i++) ix_copy(i, i + 1);
    atomic_store_explicit(&g_ix_n, n - 1, memory_order_relaxed);
    ix_maxend_from(pos, n - 1);
    ix_write_end();
}
void nvn_dirty_index_stats(uint64_t *hits, uint64_t *none, uint64_t *slow) {
    *hits = atomic_load(&g_ix_hits); *none = atomic_load(&g_ix_none); *slow = atomic_load(&g_ix_slow);
}

static range_t *find(uintptr_t a) {
    int n = atomic_load(&g_nr), k = t_last;
    if (k < n && atomic_load_explicit(&g_r[k].live, memory_order_acquire) && a >= RBASE(&g_r[k]) && a < REND(&g_r[k])) return &g_r[k];
    if (atomic_load_explicit(&g_ix_on, memory_order_relaxed) > 0) {   // [UIT 0240]
        range_t *r;
        if (ix_find(a, &r)) return r;
    }
    return find_slow(a);
}

// Scans once per epoch (nvn_dirty_epoch_begin, a GPU submission being recorded) on the thread that began it: the first
// look at a range scans all of it, later looks in the epoch use the generations gathered then. The game writes what a
// submission uses before submitting it, so one look per submission sees those writes; per-texture and per-pool scans
// were hundreds of GetWriteWatch calls a submission (~9% of the recording thread, Great Sky Island, 2026-10-24).
// Other threads (the retire thread's copy back of GPU writes) scan every time.
static _Atomic uint64_t g_epoch;
static _Thread_local int t_epoch_thread;
void nvn_dirty_epoch_begin(void) { t_epoch_thread = 1; atomic_fetch_add(&g_epoch, 1); }

// Pages of [a, a+len) written since the last scan get a new generation; their watch bits are reset.
static void scan_pages(range_t *r, uintptr_t a, size_t len, int count);
static void scan(range_t *r, uintptr_t a, size_t len, int count) {   // the caller holds r_lock(r)
    uint64_t e = 0;
    if (count && t_epoch_thread) {
        e = atomic_load(&g_epoch);
        if (atomic_load(&r->epoch) == e) return;
        a = r->base; len = r->end - r->base;
    }
    scan_pages(r, a, len, count);
    if (e) atomic_store(&r->epoch, e);   // [UIT 0025] after the scan, not before
}
static void r_lock(const range_t *r) { pthread_mutex_lock(&g_scan_mu[(size_t)(r - g_r) % SCAN_LOCKS]); }
static void r_unlock(const range_t *r) { pthread_mutex_unlock(&g_scan_mu[(size_t)(r - g_r) % SCAN_LOCKS]); }
static void scan_locked(range_t *r, uintptr_t a, size_t len, int count) { r_lock(r); scan(r, a, len, count); r_unlock(r); }
// [UIT 0249] TOTK_VK_PRESYNC_PARALLEL (backend_vk.c shadow_presync): the epoch's whole-range scan of the range holding
// addr, done now on any thread, exactly as scan() does it for the epoch thread's first look (under the range's lock,
// the epoch marked after the scan): that thread's own look in epoch e then finds the range scanned and uses the
// generations gathered here, as for its second look before. 1: scanned here; 0: already scanned in e, or not tracked.
uint64_t nvn_dirty_epoch_now(void) { return atomic_load(&g_epoch); }
int nvn_dirty_epoch_prescan(const void *addr, uint64_t e) {
    range_t *r = find((uintptr_t)addr);
    if (!r || !e || g_watch <= 0 || !atomic_load(&r->watched) || atomic_load(&r->epoch) == e) return 0;
    int did = 0;
    r_lock(r);
    if (atomic_load(&r->epoch) != e) { scan_pages(r, r->base, r->end - r->base, 1); atomic_store(&r->epoch, e); did = 1; }
    r_unlock(r);
    return did;
}
// [UIT 0030] One GetWriteWatch call with room for every page of the span. It was 512 pages a call, called again when
// full: but a full call resets the whole span (GetWriteWatch's reset and plat_written_pages' ResetWriteWatch both do),
// so the pages written beyond the first 512 were never reported - more than 2 MiB written into one pool between two
// scans (streaming) left those pages' device copy and textures old. TOTK_VK_SHADOW_VERIFY found 56,420 such pages in
// one pool (2026-10-29); tools/tests/test_dirty_bigscan.c. NVN_DIRTY_OLD_SCAN (tests): the 512-page calls.
static _Thread_local uint64_t *t_pages;
static _Thread_local size_t t_cap;
static void scan_pages(range_t *r, uintptr_t a, size_t len, int count) {
    uint64_t pg = 4096;
    uintptr_t lo = a & ~(uintptr_t)0xfff, hi = (a + len + 0xfff) & ~(uintptr_t)0xfff;
    if (lo < r->base) lo = r->base;
    if (hi > r->end) hi = r->end;
#ifdef NVN_DIRTY_OLD_SCAN
    size_t need = 512;
#else
    size_t need = (hi - lo) / 4096 + 1;   // +1: a full buffer never happens
#endif
    if (need > t_cap) {
        uint64_t *p = realloc(t_pages, need * sizeof *p);
        if (!p) die("dirty tracking: page list");
        t_pages = p; t_cap = need;
    }
    uint64_t *pages = t_pages;
    while (lo < hi) {
        int n = plat_written_pages(lo, hi - lo, pages, (int)need, &pg);
        if (n < 0) {   // this range only (it used to switch tracking off for every pool, silently)
            if (atomic_exchange(&r->watched, 0)) {
                static _Atomic int said;
                // [UIT 0051] outside the guest window = our own storage for a virtual or CPU-no-access pool
                // (totk_vpool_win.c): the game cannot write it, so nothing is missed (wording only, behaviour unchanged)
                int host = r->base < 0x0800000000ull || r->end > 0x8000000000ull;
                if (atomic_fetch_add(&said, 1) < 8)
                    LOG("dirty tracking: no write watch for pool %p..%p (%llu MiB)%s", (void *)r->base, (void *)r->end,
                        (unsigned long long)((r->end - r->base) >> 20),
                        host ? ": port-owned storage of a virtual / CPU-no-access pool (the game cannot write it): not tracked, harmless"
                             : ": not tracked (textures there upload once; buffers stay in game memory)");
            }
            return;
        }
        atomic_fetch_add(&g_scans, 1);
        uint64_t g = n && count ? atomic_fetch_add(&g_now, 1) + 1 : 0;
        for (int i = 0; i < n; i++) {
            uintptr_t p = (pages[i] - r->base) / 4096;
            if (count) atomic_store(&r->gen[p], g);
        }
        atomic_fetch_add(&g_scan_pages, (uint64_t)n);
        if ((size_t)n < need) break;   // all of the span reported
    }
}

// [UIT 0201] A new pool's pages count as written when it is added (TOTK_DIRTY_NEW_POOL, Advanced > Compatibility
// switches > New pools count as written, default On; 0 = as before). Before: the pages started at generation 0 and
// the add's scan threw away the writes the OS had seen there ("forget writes from before the pool existed"). An image
// over the address kept from an earlier pool (set aside for the next frame's object, or found by memory and shape)
// was checked against its upload stamp, found "unchanged", and drawn with the earlier pool's texels although the game
// had put new data there (its file reads and copies before MemoryPoolInitialize, writes while no pool tracked the
// address). Now such an image uploads again once; a texture first uploaded over the new pool takes its stamp after
// this and is not affected, nor are pool copies (their first fill takes the stamp after its scan). ~230 pools are
// deleted in a 30-minute session (t22x logs), so the cost is a few extra uploads.
static int new_pool_written(void) {
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("TOTK_DIRTY_NEW_POOL");
        v = !(e && *e == '0');
        LOG("dirty tracking: TOTK_DIRTY_NEW_POOL=%d: %s", v, v ? "a new pool's pages count as written (images kept from an earlier pool there upload again)"
                                                                : "a new pool's earlier writes are forgotten (as before 0201)");
    }
    return v;
}

void nvn_dirty_add_range(const void *mem, size_t size) {
    if (g_watch < 0) {   // probe once on a page inside the guest window (the main module's base)
        uint64_t p, pg;
        g_watch = plat_written_pages(0x0800000000ull, 4096, &p, 1, &pg) >= 0;
        if (!g_watch) LOG("dirty tracking: the guest window has no write watch; every use re-uploads");
    }
    uintptr_t base = (uintptr_t)mem & ~(uintptr_t)0xfff, end = ((uintptr_t)mem + size + 0xfff) & ~(uintptr_t)0xfff;
    size_t npages = (end - base) / 4096;
    _Atomic uint64_t *gen = calloc(npages ? npages : 1, sizeof *gen);
    if (!gen) die("dirty tracking: allocation");
    pthread_mutex_lock(&g_slot_mu);
    int slot = -1, n = atomic_load(&g_nr);
    for (int i = 0; i < n; i++) if (!atomic_load(&g_r[i].live)) { slot = i; break; }
    if (slot < 0) {
        if (n == MAX_RANGE) { pthread_mutex_unlock(&g_slot_mu); free(gen); LOG("dirty tracking: more than %d pools", MAX_RANGE); return; }
        slot = n; atomic_store(&g_nr, n + 1);
    }
    range_t *r = &g_r[slot];
    free(r->gen);
    if (new_pool_written()) {   // [UIT 0201] (gen is not published yet: plain stores)
        uint64_t g = atomic_fetch_add(&g_now, 1) + 1;
        for (size_t i = 0; i < npages; i++) atomic_store_explicit(&gen[i], g, memory_order_relaxed);
    }
    __atomic_store_n(&r->base, base, __ATOMIC_RELAXED); __atomic_store_n(&r->end, end, __ATOMIC_RELAXED); r->gen = gen;   // [UIT 0242] see RBASE
    atomic_store(&r->watched, 1);
    atomic_store(&r->epoch, 0);
    atomic_store_explicit(&r->live, 1, memory_order_release);
    ix_insert(slot);   // [UIT 0240]
    pthread_mutex_unlock(&g_slot_mu);
    scan_locked(r, base, end - base, 0);   // forget writes from before the pool existed
    atomic_fetch_add(&g_adds_total, 1);
    int live = atomic_fetch_add(&g_live, 1) + 1, peak = atomic_load(&g_live_peak);
    while (live > peak && !atomic_compare_exchange_weak(&g_live_peak, &peak, live)) {}
}

void nvn_dirty_remove_range(const void *mem) {
    pthread_mutex_lock(&g_slot_mu);
    range_t *r = find((uintptr_t)mem & ~(uintptr_t)0xfff);
    if (r) { atomic_store(&r->live, 0); atomic_fetch_sub(&g_live, 1); ix_remove((int)(r - g_r)); }   // [UIT 0240]
    pthread_mutex_unlock(&g_slot_mu);
}

int nvn_dirty_tracked(const void *addr, size_t len) {
    range_t *r = find((uintptr_t)addr);
    return r && g_watch > 0 && atomic_load(&r->watched) && (uintptr_t)addr + len <= r->end;
}

void nvn_dirty_protect(const void *addr, size_t len) {
    range_t *r = find((uintptr_t)addr);
    if (r) scan_locked(r, (uintptr_t)addr, len, 1);   // writes up to now belong to the content being uploaded
}

uint64_t nvn_dirty_now(void) { return atomic_load(&g_now); }

int nvn_dirty_since(const void *addr, size_t len, uint64_t stamp) {
    range_t *r = find((uintptr_t)addr);
    if (!r || g_watch <= 0 || !atomic_load(&r->watched)) return 1;   // untracked: always treat as changed
    r_lock(r);
    scan(r, (uintptr_t)addr, len, 1);
    t_stamp = atomic_load(&g_now);
    int w = 0;
    uintptr_t lo = ((uintptr_t)addr - r->base) / 4096, hi = ((uintptr_t)addr + len - r->base + 4095) / 4096;
    for (uintptr_t p = lo; p < hi && !w; p++) w = atomic_load(&r->gen[p]) > stamp;
    r_unlock(r);
    return w;
}
// [UIT 0025] The generation as of the calling thread's last nvn_dirty_since / nvn_dirty_runs, taken with the range
// locked after its scan: a later write of those pages gets a newer one. nvn_dirty_now() taken afterwards could be
// newer than a write another thread's scan found in between (never copied, never found again).
uint64_t nvn_dirty_last_stamp(void) { return t_stamp; }

// [UIT 0018] Whether a page of [addr, addr+len) got a generation above `stamp` at a scan so far, without scanning (no
// write-watch call: cheap enough for the game thread, nvn_dirty_since's bookkeeping left to the recording thread).
// Untracked memory: 0 (cannot be told).
int nvn_dirty_written_after(const void *addr, size_t len, uint64_t stamp) {
    range_t *r = find((uintptr_t)addr);
    if (!r || g_watch <= 0 || !atomic_load(&r->watched) || (uintptr_t)addr + len > r->end) return 0;
    uintptr_t lo = ((uintptr_t)addr - r->base) / 4096, hi = ((uintptr_t)addr + len - r->base + 4095) / 4096;
    for (uintptr_t p = lo; p < hi; p++) if (atomic_load_explicit(&r->gen[p], memory_order_relaxed) > stamp) return 1;
    return 0;
}

// Runs of pages of [addr, addr+len) written since `stamp`: fn(ctx, offset from addr, bytes), in order; runs less than
// `gap` bytes apart are merged (fewer, larger copies). -1: not tracked (its writes cannot be told).
static int runs(const void *addr, size_t len, uint64_t stamp, size_t gap, void (*fn)(void *ctx, size_t off, size_t n), void *ctx, int fresh) {
    range_t *r = find((uintptr_t)addr);
    if (!r || g_watch <= 0 || !atomic_load(&r->watched) || (uintptr_t)addr + len > r->end) return -1;
    r_lock(r);
    if (fresh) scan_pages(r, (uintptr_t)addr, len, 1);   // [UIT 0310] this span now, even if the epoch's scan was earlier
    else scan(r, (uintptr_t)addr, len, 1);
    t_stamp = atomic_load(&g_now);
    uintptr_t a = (uintptr_t)addr, first = (a - r->base) / 4096, last = (a + len - r->base + 4095) / 4096;
    uintptr_t run_lo = 0, run_hi = 0;   // pages, relative to the range base; run_hi 0: none open
    for (uintptr_t p = first; p < last; p++) {
        if (atomic_load(&r->gen[p]) <= stamp) continue;
        if (run_hi && (p - run_hi) * 4096 <= gap) { run_hi = p + 1; continue; }
        if (run_hi) {
            uintptr_t lo = r->base + run_lo * 4096 < a ? a : r->base + run_lo * 4096, hi = r->base + run_hi * 4096 > a + len ? a + len : r->base + run_hi * 4096;
            fn(ctx, lo - a, hi - lo);
        }
        run_lo = p; run_hi = p + 1;
    }
    if (run_hi) {
        uintptr_t lo = r->base + run_lo * 4096 < a ? a : r->base + run_lo * 4096, hi = r->base + run_hi * 4096 > a + len ? a + len : r->base + run_hi * 4096;
        fn(ctx, lo - a, hi - lo);
    }
    r_unlock(r);
    return 0;
}
int nvn_dirty_runs(const void *addr, size_t len, uint64_t stamp, size_t gap, void (*fn)(void *ctx, size_t off, size_t n), void *ctx) {
    return runs(addr, len, stamp, gap, fn, ctx, 0);
}
int nvn_dirty_runs_stamp(const void *addr, size_t len, uint64_t stamp, size_t gap, void (*fn)(void *ctx, size_t off, size_t n), void *ctx, uint64_t *now) {
    int k = nvn_dirty_runs(addr, len, stamp, gap, fn, ctx);
    if (k >= 0) *now = t_stamp;
    return k;
}
// [UIT 0310] The same with the span scanned now (not the epoch's earlier whole-range look): a write made after that
// look (a WriteTexels between two uploads of one submission) is in the runs. The generations it gives are the same
// as any scan's, so other readers of these pages (buffer copies, other textures) still see them.
int nvn_dirty_runs_fresh(const void *addr, size_t len, uint64_t stamp, size_t gap, void (*fn)(void *ctx, size_t off, size_t n), void *ctx, uint64_t *now) {
    int k = runs(addr, len, stamp, gap, fn, ctx, 1);
    if (k >= 0) *now = t_stamp;
    return k;
}

// The GPU's writes of guest memory (the retire thread's copy back of a submission's GPU writes, wb_put in
// backend_vk.c): those bytes are not the CPU's, so the pages they land on are scanned afterwards without a new
// generation. [UIT 0028] That scan reset the watch bits of the whole page, and so also took a CPU write anywhere else
// in the page between the copy's check and its end: never given a generation, never reported again - a pool's device
// copy kept the page's old bytes for good (until the game wrote that page again) and its textures were not uploaded
// again. The GPU writes back next to what the game writes (pages copied back with CPU writes in them: 15-37 a frame,
// most when a new area streams in), so after enough of them a mesh or a texture stayed stale: the shrine exit's
// screen of stretched triangles and the missing floor textures, with geometry snapshots on or off (2026-10-29).
// Now per page, under the range's lock: the page is read, the CPU's writes so far get their generation, our bytes are
// applied to that copy, our bytes written, the page scanned without generation, and if it then differs from
// the copy something else wrote it meanwhile: it gets a new generation (a write that left the same bytes changed
// nothing the device copy lacks). Returns whether any byte changed.
int nvn_dirty_gpu_write(void *dst, const void *src, size_t n) {
    uintptr_t d = (uintptr_t)dst;
    const uint8_t *s = src;
    int changed = 0;
    _Alignas(64) static _Thread_local uint8_t pre[4096];
    while (n) {
        uintptr_t pg = d & ~(uintptr_t)0xfff;
        size_t off = d - pg, c = 4096 - off < n ? 4096 - off : n;
        if (memcmp((void *)d, s, c)) {
            changed = 1;
            range_t *r = find(d);
            if (!r || g_watch <= 0 || !atomic_load(&r->watched) || pg < r->base || pg + 4096 > r->end) memcpy((void *)d, s, c);
            else {
#ifdef NVN_DIRTY_OLD_GPU_WRITE   // the 0027 way, for tools/tests/test_dirty_gpuwrite.c only
                memcpy((void *)d, s, c); scan_locked(r, pg, 4096, 0); d += c; s += c; n -= c; continue;
#endif
                r_lock(r);
                memcpy(pre, (void *)pg, 4096);   // before the scan: a write after the copy is in neither
                scan_pages(r, pg, 4096, 1);
                memcpy(pre + off, s, c);
                memcpy((void *)d, s, c);
                scan_pages(r, pg, 4096, 0);
                if (memcmp((void *)pg, pre, 4096)) atomic_store(&r->gen[(pg - r->base) / 4096], atomic_fetch_add(&g_now, 1) + 1), atomic_fetch_add(&g_gpu_raced, 1);
                r_unlock(r);
            }
        }
        d += c; s += c; n -= c;
    }
    return changed;
}
uint64_t nvn_dirty_gpu_raced(void) { return atomic_load(&g_gpu_raced); }
// GPU writes per 4 KiB page and per 1 MiB: the sequence number of the last. Written by the retire thread (wb_merge) and,
// [UIT 0054], by the image copy back (one writer at a time: g_gw_mu below), after the bytes are in place, so a reader that took its stamp before reading memory either saw the
// bytes or finds a newer number. Before (2026-10-29): a texture whose game memory the GPU fills (the game's buffer
// copies or shader stores into it, copied back at the end of a submission) was never found changed, so it kept what
// it held at its first upload - often nothing yet: zeros, drawn as holes (see-through ground) until something else
// happened to write the memory from the CPU.
// Exact, no hashing of pages (a false hit would upload a texture again at every GPU write nearby): per GiB of address
// space touched, a table of the 4 KiB pages (2 MiB) and of the MiBs (8 KiB, so most textures cost one load), made on
// first use and kept. GiBs are found through a small open-addressed table.
enum { GW_SLOTS = 4096, GW_PAGES = 1 << 18, GW_MIBS = 1 << 10 };
typedef struct { _Atomic uint64_t page[GW_PAGES], mib[GW_MIBS]; } gw_gib;
static struct { _Atomic uintptr_t key; gw_gib *_Atomic t; } g_gw[GW_SLOTS];
static _Atomic uint64_t g_gw_seq = 1;
static int g_gw_full;   // more GiBs than slots (not seen): GPU writes then count everywhere, as a hit
static gw_gib *gw_find(uintptr_t gib, int make) {
    uintptr_t key = gib + 1;
    for (uint32_t i = 0, h = (uint32_t)((key * 0x9E3779B97F4A7C15ull) >> 52); i < GW_SLOTS; i++, h = (h + 1) & (GW_SLOTS - 1)) {
        uintptr_t k = atomic_load_explicit(&g_gw[h].key, memory_order_acquire);
        if (k == key) return atomic_load_explicit(&g_gw[h].t, memory_order_acquire);
        if (k) continue;
        if (!make) return NULL;
        gw_gib *t = calloc(1, sizeof *t);   // (one writer at a time makes them: g_gw_mu)
        if (!t) break;
        atomic_store_explicit(&g_gw[h].t, t, memory_order_release);
        atomic_store_explicit(&g_gw[h].key, key, memory_order_release);
        return t;
    }
    if (make) g_gw_full = 1;
    return NULL;
}
uint64_t nvn_gpu_write_now(void) { return atomic_load(&g_gw_seq); }
// [UIT 0054] Writers take turns: the retire thread, and the translation thread copying GPU-drawn images back at the
// game's explicit reads (TOTK_VK_IMAGE_WRITEBACK). Each mark's pages carry its number before the number is published,
// and one GiB table is made at a time.
static atomic_flag g_gw_mu = ATOMIC_FLAG_INIT;
void nvn_gpu_write_mark(const void *addr, size_t len) {   // after the bytes are written
    uintptr_t a = (uintptr_t)addr;
    if (!len) return;
    while (atomic_flag_test_and_set_explicit(&g_gw_mu, memory_order_acquire)) sched_yield();
    uint64_t s = atomic_load(&g_gw_seq) + 1;
    uintptr_t e = a + len - 1;
    for (uintptr_t p = a >> 12; p <= e >> 12; p++) {
        gw_gib *t = gw_find(p >> 18, 1);
        if (!t) continue;
        atomic_store_explicit(&t->page[p & (GW_PAGES - 1)], s, memory_order_release);
        atomic_store_explicit(&t->mib[(p >> 8) & (GW_MIBS - 1)], s, memory_order_release);
    }
    atomic_store(&g_gw_seq, s);   // after the pages: a reader that sees the new number sees them
    atomic_flag_clear_explicit(&g_gw_mu, memory_order_release);
}
int nvn_gpu_written_since(const void *addr, size_t len, uint64_t stamp) {
    if (!len || atomic_load(&g_gw_seq) <= stamp) return 0;
    if (g_gw_full) return 1;
    uintptr_t a = (uintptr_t)addr, e = a + len - 1;
    for (uintptr_t m = a >> 20; m <= e >> 20; m++) {
        gw_gib *t = gw_find(m >> 10, 0);
        if (!t) { m |= GW_MIBS - 1; continue; }   // nothing written in this GiB
        if (atomic_load_explicit(&t->mib[m & (GW_MIBS - 1)], memory_order_acquire) <= stamp) continue;
        uintptr_t lo = (m << 20) > a ? (m << 20) : a, hi = ((m + 1) << 20) - 1 < e ? ((m + 1) << 20) - 1 : e;
        for (uintptr_t p = lo >> 12; p <= hi >> 12; p++)
            if (atomic_load_explicit(&t->page[p & (GW_PAGES - 1)], memory_order_acquire) > stamp) return 1;
    }
    return 0;
}

int nvn_dirty_recent(uintptr_t lo, uintptr_t hi, nvn_dirty_rec *out, int max) { (void)lo; (void)hi; (void)out; (void)max; return 0; }
void nvn_dirty_stat(int *live, int *peak, int *cap) { *live = atomic_load(&g_live); *peak = atomic_load(&g_live_peak); *cap = MAX_RANGE; }
void nvn_dirty_stats(uint64_t *faults, uint64_t *preps) { *faults = atomic_load(&g_scan_pages); *preps = 0; }
void nvn_dirty_mp_stats(uint64_t *calls, uint64_t *pages, uint64_t *ns) { *calls = atomic_load(&g_scans); *pages = atomic_load(&g_scan_pages); *ns = 0; }
void nvn_dirty_slot_stats(int *live, int *used_slots, uint64_t *adds_total) {
    *live = atomic_load(&g_live); *used_slots = atomic_load(&g_nr); *adds_total = atomic_load(&g_adds_total);
}
void dm_guest_write_prepare(void *addr, size_t n) { (void)addr; (void)n; }   // the OS tracks host writes as well
