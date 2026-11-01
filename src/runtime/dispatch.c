// Guest address -> recompiled function lookup, and the host (HLE) function registry.
// Based on LASRecomp runtime/dispatch.c (MIT); TOTK: three modules linked statically into the executable.
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include "dispatch_flat.h"
#include "hle.h"
#include "rt.h"

// Tables emitted by tools/lifter/gen.py for each module (<name>_table.c).
#define GAME_TABLE(n) extern const uint64_t n##_fn_addrs[]; extern const GuestFn n##_fn_ptrs[]; extern const uint64_t n##_fn_count;
GAME_TABLE(main)
GAME_TABLE(subsdk0)
GAME_TABLE(sdk)

typedef struct { const uint64_t *addrs; const GuestFn *fns; uint64_t count, lo, hi; } FnTable;
static FnTable tables[MAX_MODULES];
static FlatMap flat[MAX_MODULES];   // [UIT 0088] empty with TOTK_DISPATCH_FLAT=0 (flat_find then searches as before; UIT 0127: on by default)

// HLE stubs live at fake addresses HLE_BASE + 16*i
#define MAX_HLE 4096
typedef struct { const char *name; HostFn fn; void *native; char ret, blocking; } HleSlot;   // [UIT 0076] blocking: nn::fs
static HleSlot hle_slots[MAX_HLE];
static int hle_count;

// Overrides of recompiled guest functions (by address)
#define MAX_OVERRIDE 1024
static struct { uint64_t addr; HostFn fn; const char *name; } overrides[MAX_OVERRIDE];
static int override_count;
// [UIT 0019] the overrides by address: open addressing (index + 1, 0 = empty) instead of a scan of all of them on every
// dispatch cache miss; the first override of an address wins, as with the scan.
#define OV_HASH 4096
static uint16_t ov_hash[OV_HASH];
static unsigned ov_slot(uint64_t a) { return (unsigned)(((a >> 2) * 0x9E3779B97F4A7C15ull) >> 52) & (OV_HASH - 1); }
static HostFn ov_find(uint64_t pc) {
    for (unsigned h = ov_slot(pc);; h = (h + 1) & (OV_HASH - 1)) {
        unsigned i = ov_hash[h];
        if (!i) return NULL;
        if (overrides[i - 1].addr == pc) return overrides[i - 1].fn;
    }
}

// Host implementations registered by name (HLE_FUNC, hle.h)
#if defined(__linux__)
extern const HleImplEntry __start_hle_impls[], __stop_hle_impls[];
#else
// PE/COFF: the linker sorts .hle$a < .hle$m (entries) < .hle$z by the text after '$'.
__attribute__((section(".hle$a"), used)) static const HleImplEntry __start_hle_impls[1];
__attribute__((section(".hle$z"), used)) static const HleImplEntry __stop_hle_impls[1];
#endif

static const HleImplEntry *hle_lookup_entry(const char *name) {
    const HleImplEntry *start = __start_hle_impls, *stop = __stop_hle_impls;
    __asm__("" : "+r"(start), "+r"(stop));  // separate objects: keep the compiler from reasoning about them
    for (const HleImplEntry *p = start; p < stop; p++)
        if (p->name && !strcmp(p->name, name)) return p;
    return NULL;
}
const void *hle_lookup_entry_public(const char *name) { return hle_lookup_entry(name); }
HostFn hle_lookup_impl(const char *name) {
    const HleImplEntry *e = hle_lookup_entry(name);
    return e ? e->fn : NULL;
}

// HLE_NATIVE: the slot being called (set by dispatch_slow, which every host call passes through: host stubs are not
// cached in the dispatch cache).
static __thread const HleSlot *t_native;
void hle_native_thunk(Cpu *c) {
    const HleSlot *s = t_native;
    uint64_t (*f)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t) = s->native;
    uint64_t r = f(c->x[0], c->x[1], c->x[2], c->x[3], c->x[4], c->x[5], c->x[6], c->x[7]);
    switch (s->ret) {
    case 'v': break;
    case 'b': c->x[0] = (uint8_t)r; break;
    case 'w': c->x[0] = (uint32_t)r; break;
    default: c->x[0] = r; break;
    }
}


// ---- [UIT 0076] host depth and switch points (src/host/threads.c, TOTK_PREEMPT_POINTS / TOTK_ASYNC_PREEMPT)
// Adapted from valentderah/totk-pc-recomp runtime/hostfn.c and runtime/dispatch.c @ c3b9a4e (MIT, Copyright (c) 2026
// Valiantsin Dzerakh). With rt_host_track set (once, before guest threads start), every host call made through the
// dispatcher counts in the calling thread's host depth (the second int of its thread object, Cpu.thread), so the
// thread scheduler knows when a thread runs game code at the top level, with no host function (and so no host lock)
// on its stack. Not a tail call: this frame stays on the host stack while the host function runs.
int rt_host_track;
static __thread GuestFn t_track_fn;
__attribute__((noinline)) static void host_tracked(Cpu *c) {
    GuestFn f = t_track_fn;   // read first: f may make host calls of its own
    void *t = c->thread;
    if (!t) { f(c); return; }
    int *d = (int *)t + 1;
    __atomic_store_n(d, *d + 1, __ATOMIC_RELAXED);
    f(c);
    __atomic_store_n(d, *d - 1, __ATOMIC_RELAXED);
}
static GuestFn host_track(GuestFn f) { t_track_fn = f; return host_tracked; }
static HostFn ov_find(uint64_t pc);
// An override in the dispatch cache: found again by its address (c->pc is still the call target here)
__attribute__((noinline)) static void host_tracked_override(Cpu *c) {
    t_track_fn = ov_find(c->pc);
    host_tracked(c);
}
// nn::fs calls give the virtual core up while they wait for the disk (TOTK_FS_YIELD_CORE, on unless 0): on the
// Switch they are IPC requests that block the thread, and loader threads otherwise held a core through CloseFile and
// CreateCacheStorage while better threads waited for it (valentderah's TOTK_CORE_DIAG).
int runtime_core_away(void);
void runtime_core_back(int *away);
static int fs_yield_on(void) {
    static int on = -1;
    if (on < 0) { const char *e = getenv("TOTK_FS_YIELD_CORE"); on = !(e && !strcmp(e, "0")); }
    return on;
}
static void hle_native_blocking(Cpu *c) {
    const HleSlot *s = t_native;   // read before giving the core up: nothing below calls guest code
    int away = runtime_core_away();
    t_native = s;
    hle_native_thunk(c);
    runtime_core_back(&away);
}

static __thread uint64_t unimpl_pc;
static void hle_unimplemented(Cpu *c) {
    int i = (int)((unimpl_pc - HLE_BASE) / 16);
    FATAL("unimplemented host function %s (x0=%llx x1=%llx lr=%llx)\n", hle_slots[i].name,
          (unsigned long long)c->x[0], (unsigned long long)c->x[1], (unsigned long long)c->x[30]);
}

uint64_t hle_register(const char *name, HostFn fn) {
    for (int i = 0; i < hle_count; i++)
        if (!strcmp(hle_slots[i].name, name)) return HLE_BASE + 16ull * i;
    if (hle_count == MAX_HLE) FATAL("too many HLE stubs\n");
    hle_slots[hle_count].name = strdup(name);
    hle_slots[hle_count].fn = fn;
    return HLE_BASE + 16ull * hle_count++;
}

uint64_t hle_register_entry(const char *name, const void *entry) {
    const HleImplEntry *e = entry;
    uint64_t a = hle_register(name, e->fn);
    HleSlot *s = &hle_slots[(a - HLE_BASE) / 16];
    s->native = e->native;
    s->ret = e->ret;
    s->blocking = !strncmp(name, "_ZN2nn2fs", 9);   // [UIT 0076] file calls wait for the disk (TOTK_FS_YIELD_CORE)
    return a;
}

void hle_override(uint64_t addr, HostFn fn, const char *name) {
    if (override_count == MAX_OVERRIDE) FATAL("too many overrides\n");
    overrides[override_count].addr = addr;
    overrides[override_count].fn = fn;
    overrides[override_count].name = name;
    override_count++;
    for (unsigned h = ov_slot(addr);; h = (h + 1) & (OV_HASH - 1)) {
        unsigned i = ov_hash[h];
        if (i && overrides[i - 1].addr == addr) break;   // an earlier override of this address stays in force
        if (!i) { ov_hash[h] = (uint16_t)override_count; break; }
    }
}

static uint64_t hle_next = HLE_HEAP;
void *hle_alloc(uint64_t size) {
    static pthread_mutex_t m = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_lock(&m);
    uint64_t a = hle_next;
    hle_next += (size + 63) & ~63ull;
    if (hle_next > HLE_HEAP + HLE_HEAP_SIZE) FATAL("HLE heap exhausted\n");
    pthread_mutex_unlock(&m);
    plat_commit(a, size);
    memset((void *)a, 0, size);
    return (void *)a;
}

void dispatch_init(void) {
    const uint64_t *addrs[] = {main_fn_addrs, subsdk0_fn_addrs, sdk_fn_addrs};
    const GuestFn *fns[] = {main_fn_ptrs, subsdk0_fn_ptrs, sdk_fn_ptrs};
    uint64_t counts[] = {main_fn_count, subsdk0_fn_count, sdk_fn_count};
    for (int t = 0; t < MAX_MODULES; t++)
        tables[t] = (FnTable){addrs[t], fns[t], counts[t], addrs[t][0], addrs[t][counts[t] - 1]};
    // [UIT 0088] dispatch_flat.h. [UIT 0127] On by default (TOTK_DISPATCH_FLAT=0 turns it off): it returns what the
    // binary search returns (tools/tests/test_dispatch_flat.c, 8 threads) and only touched pages become memory.
    const char *e = getenv("TOTK_DISPATCH_FLAT");
    if (!(e && !strcmp(e, "0"))) {
        uint64_t slots = 0;
        for (int t = 0; t < MAX_MODULES; t++)
            if (flat_init(&flat[t], tables[t].lo, tables[t].hi, tables[t].count)) slots += flat[t].n;
        fprintf(stderr, "[totk] dispatch: flat function table on (TOTK_DISPATCH_FLAT): %llu slots, %llu MiB reserved, "
                "filled as functions are first called\n", (unsigned long long)slots, (unsigned long long)(slots * 4 >> 20));
    }
}

GuestFn dispatch_lookup(uint64_t addr) {
    for (int t = 0; t < MAX_MODULES; t++) {
        FnTable *ft = &tables[t];
        if (addr < ft->lo || addr > ft->hi) continue;
        int64_t i = flat_find(&flat[t], ft->addrs, ft->count, addr);   // the binary search, or its remembered result
        if (i >= 0) return ft->fns[i];
    }
    return NULL;
}

// [TOTK] start of the lifted function containing addr (0 = in none): diagnostics only
static uint64_t dispatch_owner(uint64_t addr) {
    for (int t = 0; t < MAX_MODULES; t++) {
        FnTable *ft = &tables[t];
        if (!ft->count || addr < ft->lo || addr > ft->hi + 0x100000) continue;
        uint64_t lo = 0, hi = ft->count;
        while (lo < hi) {
            uint64_t mid = (lo + hi) / 2;
            if (ft->addrs[mid] <= addr) lo = mid + 1;
            else hi = mid;
        }
        if (lo) return ft->addrs[lo - 1];
    }
    return 0;
}

// [TOTK] tools/lifter/seeds.py: an alias entry reached a function that has no label for it (cannot happen when the
// lift and the build match)
void rt_bad_entry(Cpu *c, uint64_t at, uint64_t fn) {
    FATAL("alias entry %llx has no label in f_%llx (lr=%llx)\n", (unsigned long long)at, (unsigned long long)fn,
          (unsigned long long)c->x[30]);
}

// Small direct-mapped cache in front of the binary search
#define DCACHE 4096
static __thread struct { uint64_t addr; GuestFn fn; } dcache[DCACHE];
// [UIT 0019] the slot: bits 2..13 of the address mixed with bits 14..25, so functions 16 KiB apart (vtables of
// related classes, the same offset in neighbouring code pages) no longer always evict each other
static inline unsigned dc_slot(uint64_t pc) { return (unsigned)((pc >> 2) ^ (pc >> 14)) & (DCACHE - 1); }
int rt_dispatch_count;                       // TOTK_PROFILE (gfx/uit_prof.c): count the misses
_Atomic unsigned long long rt_dispatch_misses;

// [UIT 0019] never inlined: inlined, it made rt_dispatch save and restore seven registers and set up a stack frame
// on every indirect call, cache hit or not; out of line the hit is a few instructions and a tail jump
__attribute__((noinline, cold)) static GuestFn dispatch_slow(Cpu *c) {
    uint64_t pc = c->pc;
    if (pc >= HLE_BASE && pc < HLE_BASE + HLE_SIZE) {
        int i = (int)((pc - HLE_BASE) / 16);
        if (i < hle_count) {
            if (hle_slots[i].native) {
                t_native = &hle_slots[i];
                GuestFn f = hle_slots[i].blocking && fs_yield_on() ? hle_native_blocking : hle_native_thunk;
                return rt_host_track ? host_track(f) : f;
            }
            if (hle_slots[i].fn) return rt_host_track ? host_track(hle_slots[i].fn) : hle_slots[i].fn;
            unimpl_pc = pc;
            return hle_unimplemented;
        }
    }
    if (rt_dispatch_count) __c11_atomic_fetch_add(&rt_dispatch_misses, 1, __ATOMIC_RELAXED);
    GuestFn ov = override_count ? ov_find(pc) : NULL;   // overrides are never removed: cached like the rest
    GuestFn f = ov ? (rt_host_track ? host_tracked_override : ov) : dispatch_lookup(pc);
    if (!f) {
        uint64_t off = 0;
        const char *s = loader_symbol_at(c->x[30], &off);
        // [TOTK] which lifted functions hold the target and the last br: a target inside a function (not at its start)
        // that a br of the same function jumped to is a jump table the lifter resolved incompletely
        uint64_t in = dispatch_owner(pc), br = c->last_br, br_in = br ? dispatch_owner(br) : 0;
        fprintf(stderr, "[dispatch] target %llx is inside f_%llx (+%llx); last br at %llx in f_%llx%s\n",
                (unsigned long long)pc, (unsigned long long)in, (unsigned long long)(in ? pc - in : 0),
                (unsigned long long)br, (unsigned long long)br_in,
                !br ? " (no br yet)" : in && in == br_in ? " (same function: an incomplete jump table)"
                                     : " (another function: a call or tail call through a register)");
        fprintf(stderr, "[dispatch] add 0x%llx to tools/lifter/seeds.py and re-run setup (see BUILD-WINDOWS-FSR.md)\n",
                (unsigned long long)pc);
        FATAL("no function at %llx (lr=%llx %s+%llx)\n", (unsigned long long)pc, (unsigned long long)c->x[30],
              s ? s : "?", (unsigned long long)off);
    }
    unsigned h = dc_slot(pc);
    dcache[h].addr = pc;
    dcache[h].fn = f;
    return f;
}

// [UIT 0019] the miss: a separate function, so the hit path in rt_dispatch makes no call and needs no stack frame
__attribute__((noinline)) static void dispatch_miss(Cpu *c) {
    GuestFn f = dispatch_slow(c);
    __attribute__((musttail)) return f(c);
}

// [UIT 0076] a pending preempt request at an indirect call (a switch point: all guest state is in Cpu). Out of line,
// so rt_dispatch keeps no stack frame; it goes on without checking again (a request kept because the thread is in a
// host call would otherwise loop). The flag is only ever set with TOTK_PREEMPT_POINTS or TOTK_ASYNC_PREEMPT on.
void runtime_core_checkpoint(void);
__attribute__((noinline)) static void dispatch_go(Cpu *c);
__attribute__((noinline)) static void dispatch_preempt(Cpu *c) {
    runtime_core_checkpoint();
    __attribute__((musttail)) return dispatch_go(c);
}

// Always tail-calls, so chains of indirect jumps don't grow the host stack. The preempt check: the first int of the
// thread object (threads.c thr_t.preempt), two loads and a branch.
void rt_dispatch(Cpu *c) {
    if (__builtin_expect(c->thread && __atomic_load_n((const int *)c->thread, __ATOMIC_RELAXED), 0))
        __attribute__((musttail)) return dispatch_preempt(c);
    unsigned h = dc_slot(c->pc);
    if (__builtin_expect(dcache[h].addr == c->pc, 1)) {
        GuestFn f = dcache[h].fn;
        __attribute__((musttail)) return f(c);
    }
    __attribute__((musttail)) return dispatch_miss(c);
}

__attribute__((noinline)) static void dispatch_go(Cpu *c) {
    unsigned h = dc_slot(c->pc);
    if (__builtin_expect(dcache[h].addr == c->pc, 1)) {
        GuestFn f = dcache[h].fn;
        __attribute__((musttail)) return f(c);
    }
    __attribute__((musttail)) return dispatch_miss(c);
}

void guest_call(Cpu *c, uint64_t addr) {
    uint64_t lr = c->x[30];
    c->x[30] = 0;
    c->pc = addr;
    rt_dispatch(c);
    c->x[30] = lr;
}

void rt_udf(Cpu *c, uint64_t pc) {
    (void)c;
    FATAL("undefined instruction / trap at %llx\n", (unsigned long long)pc);
}

void rt_brk(Cpu *c, uint32_t imm, uint64_t pc) {
    (void)c;
    FATAL("brk #%u at %llx\n", imm, (unsigned long long)pc);
}

void rt_fatal_trace(void) {
    plat_print_backtrace();
    fflush(stderr);
    plat_fatal_notice();
    abort();
}

// ---- crash reporting (platform/*.c calls these)

// Host pc -> recompiled guest function containing it ("f_<addr>").
static int cmp_host(const void *a, const void *b) {
    uint64_t x = ((const uint64_t *)a)[0], y = ((const uint64_t *)b)[0];
    return x < y ? -1 : x > y;
}
const char *rt_host_fn_name(uint64_t pc, uint64_t *offset) {
    static uint64_t (*byhost)[2];  // {host address, guest address}, sorted by host address
    static uint64_t n;
    static __thread char name[32];
    if (!byhost) {
        uint64_t total = 0, k = 0;
        for (int t = 0; t < MAX_MODULES; t++) total += tables[t].count;
        uint64_t (*v)[2] = malloc(total * sizeof *v);
        if (!v) return NULL;
        for (int t = 0; t < MAX_MODULES; t++)
            for (uint64_t i = 0; i < tables[t].count; i++) {
                v[k][0] = (uint64_t)(uintptr_t)tables[t].fns[i];
                v[k++][1] = tables[t].addrs[i];
            }
        qsort(v, k, sizeof *v, cmp_host);
        n = k;
        byhost = v;
    }
    uint64_t lo = 0, hi = n;  // last entry with host address <= pc
    while (lo < hi) {
        uint64_t mid = (lo + hi) / 2;
        if (byhost[mid][0] <= pc) lo = mid + 1;
        else hi = mid;
    }
    if (!lo || pc - byhost[lo - 1][0] > 0x100000) return NULL;
    snprintf(name, sizeof name, "f_%llx", (unsigned long long)byhost[lo - 1][1]);
    if (offset) *offset = pc - byhost[lo - 1][0];
    return name;
}

bool rt_fault_watch(uint64_t fa, uint64_t pc) { (void)fa; (void)pc; return false; }

// [UIT 0120] crash report: the host function a guest address goes to (an HLE stub's or an override's name), else NULL
const char *rt_guest_host_name(uint64_t addr) {
    if (addr >= HLE_BASE && addr < HLE_BASE + 16ull * (uint64_t)hle_count && !(addr & 15))
        return hle_slots[(addr - HLE_BASE) / 16].name;
    for (unsigned h = ov_slot(addr), k = 0; k < OV_HASH; h = (h + 1) & (OV_HASH - 1), k++) {
        unsigned i = ov_hash[h];
        if (!i) break;
        if (overrides[i - 1].addr == addr) return overrides[i - 1].name ? overrides[i - 1].name : "(host override)";
    }
    return NULL;
}

void (*rt_fault_extra)(void);   // the host's guest-state report (registers, guest call chain), set at startup
void (*rt_fault_note)(void);    // first, before the report: the shader warm-up's crash journal (gfx/vkb_shader.c)

void rt_fault_report(const char *what, uint64_t fa, uint64_t pc, uint64_t lr) {
    if (rt_fault_note) rt_fault_note();
    uint64_t po = 0, lo = 0, bo = 0;
    const char *ps = rt_host_fn_name(pc, &po), *ls = rt_host_fn_name(lr, &lo), *line = NULL;
    if (!ps) ps = plat_symbol(pc, &po);
    // [UIT 0120] the f_ name is the nearest table function only (a guess: runtime, library and the lifted bodies behind
    // native_libc fronts are not in the table); the PDB's name, when there is one, is the real function
    const char *bs = plat_symbol_pdb(pc, &bo, &line);
    char real[700] = "";
    if (bs) snprintf(real, sizeof real, "; PDB: %s+%llx%s%s", bs, (unsigned long long)bo, line ? " " : "", line ? line : "");
    fprintf(stderr, "\n*** %s accessing %#llx\n    host pc %llx (%s+%llx%s%s)\n    host lr %llx (%s)\n", what,
            (unsigned long long)fa, (unsigned long long)pc, ps ? ps : "?", (unsigned long long)po,
            ps && ps[0] == 'f' && ps[1] == '_' ? ", nearest table function" : "", real,
            (unsigned long long)lr, ls ? ls : "?");
    plat_fault_detail();
    if (rt_fault_extra) rt_fault_extra();
    rt_fatal_trace();
}

// ---- function-entry profiler hook (cpu.h FN_ENTER): off unless a profiler sets g_fnprof
volatile int g_fnprof;
void fnprof_hit(uint64_t addr, uint64_t lr, uint64_t x0) { (void)addr; (void)lr; (void)x0; }

// ---- CNTPCT_EL0: the Switch's 19.2 MHz system counter
// [UIT 0074] straight from the host counter with a fixed-point multiplier set before main (platform.h plat_scale):
// no 128-bit division per read. TOTK_TIMER_EXACT=1 keeps the old ns * 12 / 625.
static PlatScale tick_scale;
__attribute__((constructor)) static void tick_scale_init(void) { tick_scale = plat_scale(19200000u, plat_counter_freq()); }
uint64_t rt_ticks(void) {
    if (__builtin_expect(plat_timer_exact() || (!tick_scale.q && !tick_scale.f), 0))
        return (uint64_t)((unsigned __int128)plat_now_ns() * 12 / 625);
    return plat_scaled(tick_scale, plat_counter());
}
