/* Horizon kernel threads and synchronization for the recompiled game.
 * Ported from MacNativeGames games/totk/runtime/threads.c @b75be8c (MIT, "Adapted from pinned D2R Mobile
 * host/hle_thread.c, MIT"). Changes: each guest thread runs the recompiled code on its own host thread with a Cpu
 * (guest_call) instead of entering ARM code on the guest stack; Darwin QoS / Mach diagnostics removed; host -> guest
 * SDK calls go through GCALL. */
/* Original header: Adapted from pinned D2R Mobile host/hle_thread.c, MIT.
 * Game-specific diagnostics and optional timing accounting removed.
 * Core affinity is virtual bookkeeping; Darwin schedules pthreads. */
// Horizon 内核的线程与同步 SVC，原生实现：每个客体线程 = 一个宿主 pthread，跑在 SDK 给的客体栈上。
// SDK 的 nn::os 原样运行，只把它下面的「内核」换成宿主：
//   线程   0x08 CreateThread 0x09 StartThread 0x0A ExitThread 0x0B SleepThread 0x0C/0x0D 优先级 0x0E/0x0F 核掩码
//          0x10 GetCurrentProcessorNumber 0x25 GetThreadId 0x19 CancelSynchronization
//   锁     0x1A ArbitrateLock 0x1B ArbitrateUnlock（标签字 = 持有者句柄 | 0x40000000 有等待者）
//   条件   0x1C WaitProcessWideKeyAtomic 0x1D SignalProcessWideKey
//   地址   0x34 WaitForAddress 0x35 SignalToAddress
//   事件   0x45 CreateEvent 0x11 SignalEvent 0x12 ClearEvent 0x17 ResetSignal 0x18 WaitSynchronization 0x16 CloseHandle
// 语义按公开 Horizon 内核行为（Atmosphère mesosphère 的 KConditionVariable / KAddressArbiter / KSynchronization）。
// 所有等待挂在一把全局锁 G 上，每线程一个条件变量；先求正确，性能后量。
// Thread and synchronization SVCs of the Horizon kernel, implemented natively: each guest thread = one host pthread, running on the guest stack given by the SDK.
// The SDK's nn::os runs as is; only the "kernel" underneath it is replaced by the host:
//   Threads    0x08 CreateThread 0x09 StartThread 0x0A ExitThread 0x0B SleepThread 0x0C/0x0D priority 0x0E/0x0F core mask
//              0x10 GetCurrentProcessorNumber 0x25 GetThreadId 0x19 CancelSynchronization
//   Locks      0x1A ArbitrateLock 0x1B ArbitrateUnlock (tag word = owner handle | 0x40000000 has waiters)
//   Condition  0x1C WaitProcessWideKeyAtomic 0x1D SignalProcessWideKey
//   Address    0x34 WaitForAddress 0x35 SignalToAddress
//   Events     0x45 CreateEvent 0x11 SignalEvent 0x12 ClearEvent 0x17 ResetSignal 0x18 WaitSynchronization 0x16 CloseHandle
// Semantics follow the public Horizon kernel behavior (KConditionVariable / KAddressArbiter / KSynchronization of Atmosphère mesosphère).
// All waits hang on one global lock G, with one condition variable per thread; correctness first, performance to be measured later.
#include "host.h"
#include "ipc.h"
static void g_relock(pthread_mutex_t *m);   // [UIT 0246] below: the WaitOnAddress path takes G back with g_lock
#define TW_RELOCK(m) g_relock(m)
#include "thr_wait.h"   // [UIT 0087] the per-thread wait (TOTK_WAIT_ON_ADDRESS)
#include <errno.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#undef LOG
#define LOG(...) do {fprintf(stderr,"[totk-thread] " __VA_ARGS__);fputc('\n',stderr);}while(0)
#define die(...) do {LOG(__VA_ARGS__);runtime_fail("thread runtime failure");}while(0)
#define now_ns runtime_now_ns
#define guest_ns runtime_now_ns
static inline int dm_detclock_enabled(void) {return 0;}

#define R_TIMEOUT     0xEA01u   // 内核模块 1，描述 117 | Kernel module 1, description 117
#define R_CANCELLED   0xEC01u   // 118
#define R_BAD_HANDLE  0xE401u   // 114
#define R_BAD_STATE   0xFA01u   // 125
#define R_OUT_OF_RES  0xCE01u   // 103
#define WAITERS_BIT   0x40000000u
#define H_CUR_THREAD  0xFFFF8000u

enum { O_FREE, O_THREAD, O_EVENT_R, O_EVENT_W, O_SESSION, O_TMEM };   // O_SESSION / O_TMEM: IPC objects owned by ipc.c
typedef struct event { int signaled; int refs; } event_t;
typedef struct thr {
    // [UIT 0076] first, in this order: dispatch.c and guest.c read them through Cpu.thread
    int preempt;               // a higher-priority thread waits for this thread's core (set only with switch points on)
    int host_depth;            // host calls and SVCs on the thread's stack (counted only with switch points on)
    uint32_t handle; uint64_t id;
    uint64_t entry, arg, stack_top; uint64_t host_context[2]; int prio, core; uint64_t mask;
    uint64_t born_ns, exit_ns; uint32_t born_tick, exit_tick;   // 建立 / 退出的时刻与当时的游戏拍（现场导出用：作业工人是不是在卡住前退的） | Time of creation / exit and the game tick at that time (for the state dump: did the job worker exit before the stall)
    int started, exited, cancel;
    pthread_t pt; thr_wait_t cv;   // [UIT 0087] thr_wait.h
    // 当前等待：kind 0=无 1=锁 2=条件 3=地址 4=同步对象
    // Current wait: kind 0=none 1=lock 2=condition 3=address 4=synchronization object
    int wkind, woke; uint64_t waddr, wkey; uint32_t wtag; int64_t wres;
    uint64_t wseq;             // 登记等待的先后序号：同优先级先到先醒（与 Horizon 内核 KThread 等待树同序） | Sequence number of wait registration: at equal priority first come, first woken (same order as the Horizon kernel's KThread wait tree)
    struct thr *next;          // 等待链 | Wait chain
    struct thr *all;           // 全部线程链（现场导出用） | All-threads chain (for the state dump)
    int last_svc; uint64_t last_lr, last_fp;
    int closed;                // 句柄已关：与 exited 同时成立才释放对象（Horizon：线程对象随最后一个引用消失） | Handle closed: the object is freed only when this holds together with exited (Horizon: the thread object goes away with the last reference)
    uint64_t nsvc;             // SVC 次数（现场导出用） | SVC count (for the state dump)
    uint32_t qos;
    int cur_core;              // virtual core held while running guest code, -1 while blocked / not started
    uint64_t cseq, core_since; struct thr *cnext;
    uint32_t wh[4]; int nwh;   // handles of the current WaitSynchronization (state dump)
    int last_hle;              // 1 + wrapper index of the last host call made while "hletrace" was on (state dump)
    uint64_t yrun_ns, yrun_svc;   // the current run of yields: when it started, and nsvc at its last yield (yield_backoff_ns)
    uint64_t tlr;              // [UIT 0076] the thread's TLR (tpidrro_el0): +0x100 disable_count, +0x102 interrupt_flag
    void *hthr;                // [UIT 0076] TOTK_ASYNC_PREEMPT: a Windows handle that may suspend the thread
    uint64_t ap_rip, ap_last; int ap_pending;   // the interrupted rip, the last interruption, one on its way
    Cpu *cpu;                  // [UIT 0122] its guest registers (runtime_thread_bind_cpu): the exclusive monitor is cleared at a core switch
    const uint32_t *whs;       // [UIT 0247] all handles of the current WaitSynchronization (the game's array; NULL when not waiting)
} thr_t;
static thr_t *g_all;

static pthread_mutex_t G = PTHREAD_MUTEX_INITIALIZER;
// [UIT 0246] TOTK_GUEST_LOCK_SPIN (default on, 40 tries; =0 as before; =<n> tries): taking G when another thread holds it
// first retries for a few microseconds (n tries, 8 pause instructions apart) before sleeping in the lock. G is held for
// about a microsecond at a time (scheduling bookkeeping of every guest wait, signal, yield and core change), but
// winpthreads' mutex never spins: the first try that fails sleeps on a kernel event, and the wake-up takes several to
// tens of microseconds (and makes every later unlock a SetEvent call). Same lock, same order: only how a contended
// lock is waited for changes. Counts: runtime_glock_stats.
static int g_lk_spin = -1;
static uint64_t g_lk_won, g_lk_slept, g_lk_slept_ns;   // (statistics: relaxed atomic adds)
static inline void g_relax(void) {
#if defined(__x86_64__) || defined(__i386__)
    __builtin_ia32_pause();
#elif defined(__aarch64__)
    __asm__ volatile("yield");
#endif
}
static int g_lk_spin_init(void) {
    const char *e = getenv("TOTK_GUEST_LOCK_SPIN");
    int n = 40;
    if (e && *e) { n = atoi(e); if (n < 0) n = 0; if (n > 100000) n = 100000; if (!strcmp(e, "on")) n = 40; }
    int init = -1;
    if (__atomic_compare_exchange_n(&g_lk_spin, &init, n, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
        fprintf(stderr, "[totk-thread] [UIT 0246] guest scheduling lock: %s (TOTK_GUEST_LOCK_SPIN: %d tries%s)\n",
                n ? "a busy lock is retried briefly before sleeping" : "sleeps at once when busy, as before", n, e && *e ? "" : ", default; =0 as before");
    return __atomic_load_n(&g_lk_spin, __ATOMIC_ACQUIRE);
}
static uint64_t g_lk_now(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return (uint64_t)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec; }
static void g_lock_slow(void) {
    int n = __atomic_load_n(&g_lk_spin, __ATOMIC_RELAXED);
    if (n < 0) n = g_lk_spin_init();
    for (int i = 0; i < n; i++) {
        for (int k = 0; k < 8; k++) g_relax();
        if (!pthread_mutex_trylock(&G)) { __atomic_add_fetch(&g_lk_won, 1, __ATOMIC_RELAXED); return; }
    }
    uint64_t t0 = g_lk_now();
    pthread_mutex_lock(&G);
    __atomic_add_fetch(&g_lk_slept, 1, __ATOMIC_RELAXED);
    __atomic_add_fetch(&g_lk_slept_ns, g_lk_now() - t0, __ATOMIC_RELAXED);
}
static inline void g_lock(void) { if (__builtin_expect(pthread_mutex_trylock(&G) != 0, 0)) g_lock_slow(); }
static void g_relock(pthread_mutex_t *m) { if (m == &G) g_lock(); else pthread_mutex_lock(m); }
// [UIT 0246] cumulative: G taken after retrying, G waited for by sleeping in the lock, and the time of those sleeps (ns)
void runtime_glock_stats(uint64_t v[3]) {
    v[0] = __atomic_load_n(&g_lk_won, __ATOMIC_RELAXED); v[1] = __atomic_load_n(&g_lk_slept, __ATOMIC_RELAXED);
    v[2] = __atomic_load_n(&g_lk_slept_ns, __ATOMIC_RELAXED);
}
#define MAX_H 4096
static struct { int type; void *obj; } g_h[MAX_H];
/* [port] the calling guest thread: a thread-local instead of a pthread key (winpthreads hands out key 0, which
   MacNativeGames' "g_keythr ? ... : NULL" checks took for "no key") */
static __thread thr_t *t_self;
static uint64_t g_next_id = 0x51;
static thr_t *g_wait;          // 全部在等的线程（锁 / 条件 / 地址 / 同步） | All waiting threads (lock / condition / address / synchronization)

static uint32_t h_new(int type, void *obj) {     // 调用方持 G | Caller holds G
    for (int i = 1; i < MAX_H; i++) if (!g_h[i].type) { g_h[i].type = type; g_h[i].obj = obj; return 0x1000 + i; }
    die("句柄表满");
}
static void *h_peek(uint32_t h, int type) {      // 只查不报 | Lookup only, no report
    if (h == H_CUR_THREAD && type == O_THREAD) return t_self;
    unsigned i = h - 0x1000;
    if (i == 0 || i >= MAX_H || g_h[i].type != type) return NULL;
    return g_h[i].obj;
}
static void *h_get(uint32_t h, int type) {       // 查不到即报（句柄错是真错误） | Reports if not found (a wrong handle is a real error)
    void *o = h_peek(h, type);
    if (!o) LOG("句柄无效 %#x（要类型 %d）", h, type);
    return o;
}
static thr_t *cur(void) {
    thr_t *t = t_self;
    if (!t) die("非客体线程调用了线程 SVC");
    return t;
}

// ---------- 等待原语（持 G 调用）----------
// ---------- Wait primitives (called holding G) ----------
static void wait_unlink(thr_t *t) {
    for (thr_t **p = &g_wait; *p; p = &(*p)->next) if (*p == t) { *p = t->next; break; }
    t->next = NULL;
}
// 按内核顺序挑等待者：优先级数值小者优先，同优先级先登记者优先（持 G）。kind 1 按锁地址、2 按条件键、3 按地址匹配
// Pick a waiter in kernel order: numerically lower priority first, at equal priority the earlier registered first (holding G). kind 1 matches by lock address, 2 by condition key, 3 by address
static uint64_t g_wseq;
static thr_t *pick(int kind, uint64_t a) {
    thr_t *b = NULL;
    for (thr_t *t = g_wait; t; t = t->next) {
        if (t->wkind != kind || (kind == 2 ? t->wkey : t->waddr) != a) continue;
        if (!b || t->prio < b->prio || (t->prio == b->prio && t->wseq < b->wseq)) b = t;
    }
    return b;
}
static void wake(thr_t *t, int64_t res) { wait_unlink(t); t->wkind = 0; t->woke = 1; t->wres = res; tw_signal(&t->cv); }
// 挂起直到被唤醒或超时（ns<0 无限、0 立即超时）；返回 wres 或 R_TIMEOUT
// Suspend until woken or timed out (ns<0 infinite, 0 immediate timeout); returns wres or R_TIMEOUT
static int64_t block_inner(thr_t *t, int kind, int64_t ns);
// ---------- Virtual cores ----------
// Horizon runs at most one thread per core, and TOTK keys per-core data (allocators, job queues) by
// GetCurrentCoreNumber without locks. Guest threads therefore hold one of the three application cores while they
// run guest code; a core is given up only where Horizon would switch threads (waits, sleep, yield, exit, blocking
// host services) and handed to the best waiting thread (lowest priority value, then first come). A thread that a
// higher-priority waiter wants to preempt gives its core up at its next SVC. TOTK_FREE_CORES=1 disables this.
#define NCORE 3
static thr_t *g_core_owner[NCORE];
static thr_t *g_core_wait;
static int g_cores_on = -1;
static int cores_enabled(void) { if (g_cores_on < 0) g_cores_on = getenv("TOTK_FREE_CORES") ? 0 : 1; return g_cores_on; }
static int core_allowed(const thr_t *t, int c) { return (int)((t->mask >> c) & 1); }

// ---- [UIT 0076] switch points and asynchronous preemption
// Adapted from valentderah/totk-pc-recomp runtime/kernel.c @ c3b9a4e (MIT, Copyright (c) 2026 Valiantsin Dzerakh),
// itself from MacNativeGames games/totk/runtime/threads.c @ 33e8fa8 (MIT, Copyright (c) 2026 MacNativeGames
// contributors). Horizon gives a core to a strictly better thread as soon as it is ready. Before this, a better
// thread waiting for a virtual core marked the owner (preempt) but nothing read the mark: the core changed hands only
// when the owner waited, slept or yielded.
// - TOTK_PREEMPT_POINTS=1: the owner gives its core up at its next top-level SVC or indirect call (dispatch.c) if a
//   better thread still waits for it. "Top level": no host call on its stack (host_depth 0), so it holds no host lock.
// - TOTK_ASYNC_PREEMPT=1 (needs switch points): also, a waiter that has waited 250 us interrupts the owner (at most
//   once a millisecond) if the owner runs top-level game code (its host rip in the lifted modules): src/host/
//   preempt_win.c moves its rip to a stub that saves every register, gives the core up and jumps back. With it,
//   Horizon's priority inheritance (a guest mutex owner counts at the priority of the best thread waiting for that
//   mutex) and preemption disable (TLR+0x100, the SDK's spin locks; the switch then happens at SVC 0x36), which
//   MacNativeGames found necessary: without them the game stopped at its first "Continue".
// Both off: the old scheduling exactly (the mark is not even set).
#define AP_FIRST_NS 250000ull   // the waiter spins this long before the first interruption (most waits end sooner)
#define AP_GAP_NS 1000000ull    // and interrupts the same owner at most this often
extern int rt_host_track;       // dispatch.c: count host calls in host_depth
static int g_points, g_ap;
static uint64_t g_ap_sent, g_ap_tries;
int uit_ap_init(char *why, size_t nwhy);   // preempt_win.c: 1 if the lifted code is known exactly and holds no host code
void *uit_ap_self_handle(void);
void uit_ap_close(void *h);
int uit_ap_redirect(void *h, const volatile int *depth, const volatile uint16_t *tl, uint64_t *rip, int *pending);
static int env_on(const char *k) { const char *e = getenv(k); return e && *e && strcmp(e, "0") && strcmp(e, "off"); }
static void sched_init(void) {   // once, on the guest main thread before guest code runs
    static int done; if (done) return; done = 1;
    g_points = env_on("TOTK_PREEMPT_POINTS");
    g_ap = g_points && env_on("TOTK_ASYNC_PREEMPT");
    if (g_ap) {   // [UIT 0257] the check says what it found either way
        char why[320] = "";
        if (uit_ap_init(why, sizeof why)) LOG("asynchronous preemption: %s", why);
        else { LOG("asynchronous preemption off: the lifted code range is unknown or holds host code: %s", why); g_ap = 0; }
    }
    rt_host_track = g_points;
    // [UIT 0257] which per-thread wait is in use (thr_wait.h), so a log shows whether TOTK_WAIT_ON_ADDRESS took effect
    LOG("thread waits: %s", tw_on() ? "WaitOnAddress (TOTK_WAIT_ON_ADDRESS=1); [sched] lines every 300 frames count them"
                                    : "condition variables (TOTK_WAIT_ON_ADDRESS off)");
    if (g_points) LOG("switch points on (TOTK_PREEMPT_POINTS); asynchronous preemption %s", g_ap ? "on (TOTK_ASYNC_PREEMPT)" : "off");
}
static volatile uint16_t *ap_tlr(const thr_t *t) { return (volatile uint16_t *)(uintptr_t)(t->tlr + 0x100); }
static int ap_disabled(const thr_t *t) { return g_ap && t->tlr && ap_tlr(t)[0]; }
// Priority inheritance (TOTK_ASYNC_PREEMPT): o counts at the priority of the best thread blocked on a guest mutex it
// owns (the owner is read from the mutex word, so it follows hand-offs). Holding G.
static int prio_of(const thr_t *o) {
    int p = o->prio;
    if (!g_ap) return p;
    for (const thr_t *w = g_wait; w; w = w->next)
        if (w->wkind == 1 && w->prio < p && w->waddr &&
            (__atomic_load_n((const uint32_t *)(uintptr_t)w->waddr, __ATOMIC_RELAXED) & ~WAITERS_BIT) == o->handle)
            p = w->prio;
    return p;
}
// Whether a thread better than t waits for a core t holds (c). Holding G.
static int core_better_waiter(const thr_t *t, int c) {
    int tp = prio_of(t);
    for (const thr_t *w = g_core_wait; w; w = w->cnext) if (core_allowed(w, c) && prio_of(w) < tp) return 1;
    return 0;
}

static void core_grant(int c) {   // holding G: give free core c to the best waiter
    thr_t *b = NULL, **bp = NULL; int bpri = 0;
    for (thr_t **p = &g_core_wait; *p; p = &(*p)->cnext) {
        if (!core_allowed(*p, c)) continue;
        int pr = prio_of(*p);
        if (!b || pr < bpri || (pr == bpri && (*p)->cseq < b->cseq)) { b = *p; bp = p; bpri = pr; }
    }
    if (!b) return;
    *bp = b->cnext; b->cnext = NULL; b->cur_core = c; b->core_since = now_ns(); g_core_owner[c] = b; tw_signal(&b->cv);
}
// TOTK_ASYNC_PREEMPT: waiter w interrupts the worst owner worse than itself on a core it may run on, if that owner
// runs top-level game code. Picked again on every call (an owner may have exited since). Holding G; the owner is
// suspended only inside uit_ap_redirect, which takes no lock. Returns 1 while such an owner keeps its core.
static int ap_try(thr_t *w) {
    int wp = prio_of(w), vp = 0; thr_t *o = NULL;
    for (int i = 0; i < NCORE; i++) {
        thr_t *x = g_core_owner[i]; if (!x || !core_allowed(w, i)) continue;
        int xp = prio_of(x); if (xp > wp && (!o || xp > vp)) { o = x; vp = xp; }
    }
    if (!o) return 0;
    __atomic_store_n(&o->preempt, 1, __ATOMIC_RELAXED);
    if (!o->hthr || o->ap_pending || __atomic_load_n(&o->host_depth, __ATOMIC_RELAXED) > 0) return 1;
    if (ap_disabled(o)) { ap_tlr(o)[1] = 1; return 1; }   // preemption disabled: it switches at SVC 0x36
    uint64_t now = now_ns();
    if (o->ap_last && now - o->ap_last < AP_GAP_NS) return 1;
    o->ap_last = now; g_ap_tries++;
    if (uit_ap_redirect(o->hthr, &o->host_depth, ap_tlr(o), &o->ap_rip, &o->ap_pending)) g_ap_sent++;
    else if (ap_disabled(o)) ap_tlr(o)[1] = 1;
    return 1;
}
// [UIT 0122] The guest's exclusive monitor is lost when its thread leaves the core, as on the Switch (Horizon clears
// it on every context switch). cpu.h emulates LDXR/STXR with a compare-and-swap against the loaded value, so a store-
// exclusive after a switch succeeds whenever the word reads the same again (ABA): a lock-free list pop that loaded
// head A and next B, lost the core for a millisecond while other threads popped A and B and pushed A back, then
// stored B as the head, a node now in use (likely the 2026-10-30 crash: a list walk read next 0x01000002ffff0001).
// TOTK_ASYNC_PREEMPT made that window as long as the core is away; with the monitor cleared here the store fails and
// the game's loop loads again, as on hardware. A failed store-exclusive is always allowed, so this is safe everywhere.
// [UIT 0134] TOTK_EXCL_CLEAR=0: not cleared, as before 0122 (for a test only: brings back the ABA window)
static int g_excl_clear = -1;
static void excl_clear(thr_t *t) {
    if (g_excl_clear < 0) { const char *e = getenv("TOTK_EXCL_CLEAR"); g_excl_clear = !(e && e[0] == '0'); }
    if (g_excl_clear && t->cpu) *(volatile uint64_t *)&t->cpu->excl_addr = ~0ull;
}
static void core_put(thr_t *t) {   // holding G
    if (t->cur_core < 0) return;
    excl_clear(t);
    int c = t->cur_core; t->cur_core = -1; t->preempt = 0; g_core_owner[c] = NULL; core_grant(c);
}
static __thread uint64_t t_tw[8];   // [UIT 0238] see runtime_thread_waits
static void core_take(thr_t *t) {   // holding G; waits until a core in t's mask is handed over
    if (!cores_enabled() || t->cur_core >= 0) return;
    if (!(t->mask & ((1u << NCORE) - 1))) die("thread %#x core mask %#llx has no application core", t->handle, (unsigned long long)t->mask);
    int c = t->core >= 0 && t->core < NCORE && core_allowed(t, t->core) && !g_core_owner[t->core] ? t->core : -1;
    for (int i = 0; c < 0 && i < NCORE; i++) if (core_allowed(t, i) && !g_core_owner[i]) c = i;
    if (c >= 0) { t->cur_core = c; t->core_since = now_ns(); g_core_owner[c] = t; return; }
    t->cseq = ++g_wseq; t->cnext = g_core_wait; g_core_wait = t;
    uint64_t cw0 = now_ns();   // [UIT 0238]
    if (!g_points) {   // the old wait, exactly
        while (t->cur_core < 0) tw_wait(&t->cv, &G, NULL);
        if (t == t_self) { t_tw[0] += now_ns() - cw0; t_tw[4]++; }
        return;
    }
    int tp = prio_of(t);
    for (int i = 0; i < NCORE; i++)
        if (core_allowed(t, i) && g_core_owner[i] && prio_of(g_core_owner[i]) > tp) __atomic_store_n(&g_core_owner[i]->preempt, 1, __ATOMIC_RELAXED);
    if (g_ap) {   // most waits end within microseconds: spin before interrupting anyone
        uint64_t end = now_ns() + AP_FIRST_NS;
        pthread_mutex_unlock(&G);
        while (__atomic_load_n(&t->cur_core, __ATOMIC_RELAXED) < 0 && now_ns() < end) __builtin_ia32_pause();
        g_lock();
    }
    while (t->cur_core < 0) {
        if (g_ap && ap_try(t)) {   // an owner keeps its core: look again in a millisecond
            struct timespec dl; clock_gettime(CLOCK_REALTIME, &dl); dl.tv_nsec += 1000000;
            if (dl.tv_nsec >= 1000000000) { dl.tv_sec++; dl.tv_nsec -= 1000000000; }
            tw_wait(&t->cv, &G, &dl);
        } else tw_wait(&t->cv, &G, NULL);
    }
    if (t == t_self) { t_tw[0] += now_ns() - cw0; t_tw[4]++; }   // [UIT 0238]
}
static thr_t *me_or_null(void) { return t_self; }
// [UIT 0238] where a game thread's time goes besides running: cumulative, per thread (the [critical path] lines read the
// frame thread's at present, uit_cp_add_own in src/gfx/nvn/hle_nvn.c). 0 waiting for a virtual core (core_take when no
// core was free: the 3 application cores were busy), 1 the game's own blocking waits (mutexes, condition variables,
// events: SVC waits with a timeout other than 0, without the core wait after them), 2 SleepThread(ns > 0), 3 yields
// (SleepThread 0 / -1 / -2: the game's polling loops, with their back-off sleeps, without core waits). Counts in
// [4..7]. A clock read around each wait that already gives up the core; nothing on the running path.
void runtime_thread_waits(uint64_t v[8]) { for (int i = 0; i < 8; i++) v[i] = t_tw[i]; }
void runtime_thread_note_hle(unsigned i) { thr_t *t = me_or_null(); if (t) t->last_hle = (int)i + 1; }   // hle_stack.c, while tracing
uint32_t runtime_thread_current_handle(void) { thr_t *t = me_or_null(); return t ? t->handle : 0; }
// Host services that wait for other threads or the GPU give the core up around the wait (no host locks held).
int runtime_core_away(void) {
    thr_t *t = me_or_null(); if (!t || t->cur_core < 0) return 0;
    g_lock(); core_put(t); pthread_mutex_unlock(&G); return 1;
}
void runtime_core_back(int *away) {
    thr_t *t = me_or_null(); if (!*away || !t) return;
    g_lock(); core_take(t); pthread_mutex_unlock(&G);
}
// [UIT 0076] switch point (TOTK_PREEMPT_POINTS): at a top-level SVC (guest.c) or indirect call (dispatch.c). The core
// goes over only if a better thread still waits for it; a stale mark is dropped. Inside a host call the mark is kept
// for the next top-level point (the thread may hold a host lock).
void runtime_core_checkpoint(void) {
    thr_t *t = me_or_null();
    if (!t || !__atomic_load_n(&t->preempt, __ATOMIC_RELAXED) || __atomic_load_n(&t->host_depth, __ATOMIC_RELAXED) > 0) return;
    g_lock();
    if (t->cur_core >= 0 && core_better_waiter(t, t->cur_core)) {
        if (ap_disabled(t)) { ap_tlr(t)[1] = 1; t->preempt = 0; }   // the SDK holds a spin lock: the switch is at SVC 0x36
        else { core_put(t); core_take(t); }
    } else t->preempt = 0;
    pthread_mutex_unlock(&G);
}
// preempt_win.c's stub, on the interrupted thread: where to jump back to, then the core given up as at a switch point
uint64_t uit_ap_rip(void) { return t_self->ap_rip; }
void uit_ap_yield(void) {
    thr_t *t = t_self;
    g_lock();
    t->ap_pending = 0;
    if (t->preempt && t->cur_core >= 0 && !ap_disabled(t) && core_better_waiter(t, t->cur_core)) { core_put(t); core_take(t); }
    pthread_mutex_unlock(&G);
}
void runtime_thread_bind_cpu(Cpu *c) {
    thr_t *t = t_self; if (!t) return;
    c->thread = t; t->tlr = c->tpidrro; t->cpu = c;
    if (g_ap) t->hthr = uit_ap_self_handle();
}
void runtime_sched_stats(uint64_t *tries, uint64_t *sent) { *tries = g_ap_tries; *sent = g_ap_sent; }
// [UIT 0257] Every 300 frames (hle_nvn.c, the present): what the WaitOnAddress waits and the interruptions did since
// the last line. Nothing when both are off.
void runtime_sched_log(unsigned long long frame) {
    static uint64_t pw, pk, pt, pa, ps;
    if (tw_on()) {
        uint64_t w = __atomic_load_n(&tw_n_wait, __ATOMIC_RELAXED), k = __atomic_load_n(&tw_n_wake, __ATOMIC_RELAXED),
                 t = __atomic_load_n(&tw_n_timeout, __ATOMIC_RELAXED);
        LOG("[sched] through frame %llu: WaitOnAddress %llu waits (%llu timed out), %llu wakes over the last 300 frames",
            frame, (unsigned long long)(w - pw), (unsigned long long)(t - pt), (unsigned long long)(k - pk));
        pw = w; pk = k; pt = t;
    }
    if (g_ap) {
        uint64_t a = __atomic_load_n(&g_ap_tries, __ATOMIC_RELAXED), s = __atomic_load_n(&g_ap_sent, __ATOMIC_RELAXED);
        LOG("[sched] through frame %llu: asynchronous preemption %llu tries, %llu threads interrupted over the last 300 "
            "frames", frame, (unsigned long long)(a - pa), (unsigned long long)(s - ps));
        pa = a; ps = s;
    }
}

// ---- [UIT 0077] spin waits (tools/lifter/spins.py: the back-edges of spin_waits_v143.txt call this)
// Adapted from valentderah/totk-pc-recomp runtime/kernel.c rt_spin_gate @ c3b9a4e (MIT, Copyright (c) 2026 Valiantsin
// Dzerakh), itself from MacNativeGames threads.c runtime_spin_wait @ a038e6b and nxnative dm/host/hle_thread.c
// dm_spin_wait @ 6d34984 (MIT, Copyright (c) 2026 MacNativeGames contributors, Copyright (c) 2026 seedds).
// A pass of a guest loop that polls memory until another thread changes it. On the Switch it fills only its own core;
// here it also holds a virtual core, so the thread it waits for may be unable to run. With TOTK_SPIN_GATE=1 every
// pass pauses the host core once, and once a wait (passes through the same loop at most SPIN_GAP_NS apart) has lasted
// TOTK_SPIN_HANDOFF_US (default 50) while some thread waits for a virtual core, each pass gives the core up, yields
// the host CPU and takes a core again. Shorter waits keep the core (a lock held for a moment). Off (the default):
// returns at once, the loop runs as before. Every register is kept: the gate only adds time.
#define SPIN_GAP_NS 20000ull
static int g_spin_on = -1;
static uint64_t g_spin_handoff_ns, g_spin_passes, g_spin_handoffs;
static __thread uint64_t sw_head, sw_t0, sw_last;
void rt_spin_gate(Cpu *c, uint64_t head) {
    (void)c;
    if (__builtin_expect(g_spin_on < 0, 0)) {
        const char *e = getenv("TOTK_SPIN_HANDOFF_US");
        g_spin_handoff_ns = (uint64_t)(e && *e ? atoll(e) : 50) * 1000;
        g_spin_on = env_on("TOTK_SPIN_GATE");
        if (g_spin_on) LOG("spin-wait gate on (TOTK_SPIN_GATE): a wait longer than %llu us lends the core out", (unsigned long long)(g_spin_handoff_ns / 1000));
    }
    if (!g_spin_on) return;
    __builtin_ia32_pause();
    thr_t *t = t_self;
    if (!t) return;
    g_spin_passes++;   // (racy statistics)
    uint64_t now = now_ns();
    if (head != sw_head || now - sw_last > SPIN_GAP_NS) { sw_head = head; sw_t0 = now; }   // a new wait
    sw_last = now;
    if (t->cur_core < 0 || now - sw_t0 < g_spin_handoff_ns || !__atomic_load_n(&g_core_wait, __ATOMIC_RELAXED)) return;
    g_spin_handoffs++;
    int away = runtime_core_away();
    sched_yield();
    runtime_core_back(&away);
    sw_last = now_ns();
}
void runtime_spin_stats(uint64_t *passes, uint64_t *handoffs) { *passes = g_spin_passes; *handoffs = g_spin_handoffs; }
int runtime_current_core(void) { thr_t *t = me_or_null(); return t ? (t->cur_core >= 0 ? t->cur_core : t->core) : -1; }

static int64_t block(thr_t *t,int kind,int64_t ns) {
    if (ns == 0) return block_inner(t, kind, ns);
    uint64_t b0 = now_ns();
    core_put(t); int64_t r = block_inner(t, kind, ns);
    if (t == t_self) { t_tw[1] += now_ns() - b0; t_tw[5]++; }   // [UIT 0238] the game's own blocking wait
    core_take(t); return r;
}
static int64_t block_inner(thr_t *t, int kind, int64_t ns) {
    t->wkind = kind; t->woke = 0; t->next = g_wait; g_wait = t; t->wseq = ++g_wseq;
    if (ns == 0) { wait_unlink(t); t->wkind = 0; return R_TIMEOUT; }
    struct timespec dl;
    int det_wait = ns > 0 && dm_detclock_enabled();
    uint64_t guest_deadline = det_wait ? guest_ns() + (uint64_t)ns : 0;
    if (ns > 0 && !det_wait) { clock_gettime(CLOCK_REALTIME, &dl); dl.tv_sec += ns / 1000000000; dl.tv_nsec += ns % 1000000000;
                  if (dl.tv_nsec >= 1000000000) { dl.tv_sec++; dl.tv_nsec -= 1000000000; } }
    while (!t->woke) {
        int rc;
        if (det_wait) {
            uint64_t now = guest_ns();
            if (now >= guest_deadline) { wait_unlink(t); t->wkind = 0; return R_TIMEOUT; }
            (void)(guest_deadline - now);
            rc = tw_wait(&t->cv, &G, NULL);
        } else rc = ns > 0 ? tw_wait(&t->cv, &G, &dl) : tw_wait(&t->cv, &G, NULL);
        if (rc == ETIMEDOUT && !t->woke && (!det_wait || guest_ns() >= guest_deadline)) { wait_unlink(t); t->wkind = 0; return R_TIMEOUT; }
    }
    return t->wres;
}

// ---------- 锁 ----------
// ---------- Locks ----------
static inline uint32_t ld32(uint64_t a) { return __atomic_load_n((uint32_t *)a, __ATOMIC_SEQ_CST); }
static inline void st32(uint64_t a, uint32_t v) { __atomic_store_n((uint32_t *)a, v, __ATOMIC_SEQ_CST); }
static inline int cas32(uint64_t a, uint32_t *exp, uint32_t v) { return __atomic_compare_exchange_n((uint32_t *)a, exp, v, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); }

// 释放 addr 处的锁：交给等在它上面的第一个线程，或清零（持 G）
// Release the lock at addr: hand it to the first thread waiting on it, or clear it to zero (holding G)
static void lock_handoff(uint64_t addr) {
    thr_t *w = pick(1, addr); int more = 0;
    if (!w) { st32(addr, 0); return; }
    for (thr_t *t = g_wait; t; t = t->next) if (t != w && t->wkind == 1 && t->waddr == addr) { more = 1; break; }
    st32(addr, w->wtag | (more ? WAITERS_BIT : 0));
    wake(w, 0);
}
// 让 t 去拿 addr 处的锁：空闲就直接拿到并唤醒，否则置等待者位并转为锁等待（持 G）
// Make t take the lock at addr: if free, it gets it directly and is woken, otherwise set the waiters bit and turn into a lock wait (holding G)
static void lock_acquire_for(thr_t *t, uint64_t addr, uint32_t tag) {
    uint32_t v = ld32(addr);
    for (;;) {
        if (v == 0) { if (cas32(addr, &v, tag)) { wake(t, 0); return; } continue; }
        if (cas32(addr, &v, v | WAITERS_BIT)) break;
    }
    t->wkind = 1; t->waddr = addr; t->wtag = tag;      // 仍在 g_wait 里，改为锁等待 | Still in g_wait, changed to a lock wait
    t->wseq = ++g_wseq;                                  // 内核把它新插进锁持有者的等待队列：同优先级排在已有锁等待者之后 | The kernel inserts it anew into the lock owner's wait queue: at equal priority it goes after the existing lock waiters
}

// ---------- 线程 ----------
// ---------- Threads ----------
uint32_t (*dm_thread_tick_fn)(void);   // the current game tick, when diagnostics register it
static void *thread_main(void *p) {
    thr_t *t = p;
    t_self = t;
    { void plat_guest_thread_init(void); plat_guest_thread_init(); }   // [UIT 0121] room for the report after a stack overflow
    { int plat_thread_game(void); plat_thread_game(); }   // [UIT 0089] TOTK_HYBRID_PLACEMENT: performance cores (hybrid CPUs)
    runtime_profiler_guest_thread();   // [UIT 0147] TOTK_PROF / TOTK_PROF_GAME: marks the thread as game code in the report
    Cpu *c = guest_cpu_new(t->stack_top, guest_tls_alloc());
    guest_cpu_set(c);
    runtime_thread_bind_cpu(c);
    g_lock(); while (!t->started) tw_wait(&t->cv, &G, NULL); core_take(t); pthread_mutex_unlock(&G);
    c->x[0] = t->arg;
    guest_call(c, t->entry);   // does not return: the guest leaves through ExitThread (svc 0x0A)
    die("guest thread entry returned (Horizon requires ExitThread)");
}
static void thr_free(thr_t *t) {   // 持 G；从全部线程链摘下并释放 | Holding G; unlink from the all-threads chain and free
    for (thr_t **p = &g_all; *p; p = &(*p)->all) if (*p == t) { *p = t->all; break; }
    if (t->hthr) uit_ap_close(t->hthr);
    tw_destroy(&t->cv); free(t);
}

static void wake_sync_waiters(const void *obj);   // [UIT 0247]
static _Noreturn void thread_exit_on_host(void *argument) {
    thr_t *t=argument;
    t->exit_ns = now_ns(); t->exit_tick = dm_thread_tick_fn ? dm_thread_tick_fn() : 0;
    // 这里不打日志：退出发生在客体线程自己的栈上（有的只有十几 KiB），带浮点的 printf 会把栈撑爆（真机冒烟：LibcurlResolver 退出时 SIGBUS，回溯 fprintf → __dtoa）。时刻只记下来，现场导出时再打
    // No logging here: the exit happens on the guest thread's own stack (some are only a dozen or so KiB), and a printf with floating point would blow the stack (real-device smoke test: SIGBUS when LibcurlResolver exits, backtrace fprintf → __dtoa). Only the time is recorded, and it is printed later in the state dump
    g_lock();
    core_put(t);
    t->exited = 1;
    wake_sync_waiters(t);   // 同步等待者重查 | Synchronization waiters re-check ([UIT 0247] those that wait on this thread)
    t_self = NULL;
    if (t->closed) thr_free(t);
    pthread_mutex_unlock(&G);
    pthread_exit(NULL);
    __builtin_unreachable();   // winpthreads does not declare pthread_exit noreturn
}
static _Noreturn void thread_exit(thr_t *t) { thread_exit_on_host(t); }   // already on the thread's host stack

// 主线程登记，返回其句柄（交给 nnosInitialize 作主线程句柄）
// Register the main thread and return its handle (passed to nnosInitialize as the main thread handle)
uint32_t svc_thread_main_init(void) {
    sched_init();
    thr_t *t = calloc(1, sizeof *t);
    tw_init(&t->cv);
    t->started = 1; t->prio = 44; t->core = 0; t->mask = 7; t->pt = pthread_self();
    t->cur_core = -1;
    g_lock(); t->handle = h_new(O_THREAD, t); t->id = g_next_id++; t->all = g_all; g_all = t; core_take(t); pthread_mutex_unlock(&G);
    t_self = t;
    return t->handle;
}

static _Atomic uint64_t g_main_yield_n, g_main_yield_ns;   // 只有客体主线程写；诊断行在同一线程读 | Written only by the guest main thread; the diagnostic line reads it on the same thread
void dm_main_yield_stats(uint64_t *n, uint64_t *ns) { *n = g_main_yield_n; *ns = g_main_yield_ns; }

// ---------- 同步对象 ----------
// ---------- Synchronization objects ----------
static int sig_obj(uint32_t h, int *bad) {    // 1=已触发（持 G） | 1 = signaled (holding G)
    unsigned i = h - 0x1000;
    if (h == H_CUR_THREAD) return 0;
    if (i == 0 || i >= MAX_H) { *bad = 1; return 0; }
    switch (g_h[i].type) {
    case O_THREAD: return ((thr_t *)g_h[i].obj)->exited;
    case O_EVENT_R: return ((event_t *)g_h[i].obj)->signaled;
    default: *bad = 1; return 0;
    }
}
// [UIT 0247] TOTK_SYNC_WAKE_TARGETED (default on; =0 as before): a signaled event (or an exiting thread) wakes only the
// WaitSynchronization waiters that wait on it. Before, every signal woke every thread blocked in WaitSynchronization on
// anything; each one took a virtual core again (waiting behind running game threads when all were busy) only to find its
// own objects still unsignaled and block once more. Horizon wakes only the waiters of the signaled object. A waiter whose
// list holds a handle that is no longer a thread or readable event is still woken (it then sees the bad handle, as
// before). Counts: runtime_syncwake_stats.
static int g_swt = -1;
static uint64_t g_sw_woken, g_sw_skipped;   // holding G
static int sync_wake_targeted(void) {   // holding G
    if (g_swt < 0) {
        const char *e = getenv("TOTK_SYNC_WAKE_TARGETED");
        g_swt = !(e && e[0] == '0' && !e[1]);
        LOG("[UIT 0247] event signals wake %s (TOTK_SYNC_WAKE_TARGETED%s)", g_swt ? "only the threads waiting on that event" : "every waiting thread, as before",
            g_swt ? ", default; =0 as before" : "=0");
    }
    return g_swt;
}
static int sync_may_end(const thr_t *w, const void *obj) {   // holding G
    if (!w->whs) return 1;
    for (int i = 0; i < w->nwh; i++) {
        uint32_t h = w->whs[i];
        if (h == H_CUR_THREAD) continue;   // never signaled (sig_obj)
        uint32_t k = h - 0x1000u;
        if (k == 0 || k >= MAX_H) return 1;
        if (g_h[k].type != O_THREAD && g_h[k].type != O_EVENT_R) return 1;
        if (g_h[k].obj == obj) return 1;
    }
    return 0;
}
static void wake_sync_waiters(const void *obj) {   // holding G; obj: the event_t signaled or the thr_t that exited
    int on = sync_wake_targeted();
    for (thr_t *w = g_wait; w; ) {
        thr_t *n = w->next;
        if (w->wkind == 4) {
            if (!on || sync_may_end(w, obj)) { wake(w, -1); g_sw_woken++; }
            else g_sw_skipped++;
        }
        w = n;
    }
}
void runtime_syncwake_stats(uint64_t v[2]) { g_lock(); v[0] = g_sw_woken; v[1] = g_sw_skipped; pthread_mutex_unlock(&G); }

static int64_t now_mono_ns(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return ts.tv_sec * 1000000000ll + ts.tv_nsec; }
void dm_guest_clock_advanced(void) {
    if (!dm_detclock_enabled()) return;
    g_lock();
    for (thr_t *t = g_wait; t; t = t->next) if (t->wkind) tw_signal(&t->cv);
    pthread_mutex_unlock(&G);
}

// 处理线程 / 同步类 SVC；返回 1 = 已处理
// Handle thread / synchronization SVCs; returns 1 = handled
/* Yield back-off (after nxnative e06a8ec / b703be2). On the Switch a yield with nothing else runnable costs nothing; here
   a guest loop that polls and yields becomes a host busy spin (on the iPhone the system logged about 1,300 wake-ups a
   second for the game). A run of yields with no other SVC between them stays plain sched_yield for its first 2 ms:
   shorter waits are handoffs between working threads, such as the main thread waiting for its ModuleSystem workers on
   the frame's critical path (nxnative started at 200 µs and 1.4.3 then dropped itself to 20 fps). After that each yield
   sleeps an eighth of the run so far, 10 to 250 µs, with the virtual core given up, so a wait ends at most an eighth late.
   Off by default: on the Mac at about 100 fps (1.2.1 field save, 300 s each) it saved 3% CPU (708 -> 685 s) but frames
   dropped steadily, 10-15 per 300 instead of 0-3. TOTK_YIELD_BACKOFF=on turns it on (250:2000); =<cap µs>[:<start µs>]
   sets the cap and the start. Returns the sleep for a yield whose run started run_ns ago, 0 for a plain yield. */
// [UIT 0245] TOTK_PRECISE_SLEEP (default on; =0 as before): a guest sleep (SleepThread with ns > 0, and the yield
// back-off) lasts what the game asked for, never less: plat_sleep_ns_precise (Windows: a high-resolution waitable timer).
// Before, winpthreads' nanosleep rounded each one up to whole milliseconds (a 0.1 ms sleep took about 1-2 ms).
static int precise_sleep_on(void) {
    static int on = -1;
    int v = __atomic_load_n(&on, __ATOMIC_RELAXED);
    if (v < 0) {
        const char *e = getenv("TOTK_PRECISE_SLEEP");
        int want = !(e && e[0] == '0' && !e[1]), init = -1;
        if (__atomic_compare_exchange_n(&on, &init, want, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
            LOG("[UIT 0245] guest sleeps: %s (TOTK_PRECISE_SLEEP%s)", want ? "precise (high-resolution timer, not rounded up to ms)" : "nanosleep, as before",
                want ? ", default; =0 as before" : "=0");
        v = __atomic_load_n(&on, __ATOMIC_RELAXED);
    }
    return v;
}
static void guest_sleep_ns(uint64_t ns) {
    int plat_sleep_ns_precise(uint64_t ns);
    if (precise_sleep_on() && plat_sleep_ns_precise(ns)) return;
    struct timespec ts = {(time_t)(ns / 1000000000u), (long)(ns % 1000000000u)}; nanosleep(&ts, 0);
}

static uint64_t yield_backoff_ns(uint64_t run_ns) {
    static int64_t cap = -1, start = 2000000;
    if (cap < 0) {
        const char *e = getenv("TOTK_YIELD_BACKOFF"); int64_t c = 0;
        if (e && !strcmp(e, "on")) c = 250000;
        else if (e && *e) { c = atoll(e) * 1000; const char *colon = strchr(e, ':'); if (colon) start = atoll(colon + 1) * 1000; }
        if (c) LOG("TOTK_YIELD_BACKOFF: a run of yields backs off after %lld us, sleeping at most %lld us", (long long)(start / 1000), (long long)(c / 1000));
        cap = c;
    }
    if (!cap || run_ns < (uint64_t)start) return 0;
    uint64_t s = run_ns / 8; if (s < 10000) s = 10000;
    return s < (uint64_t)cap ? s : (uint64_t)cap;
}

int svc_thread(int num, uint64_t *regs) {
    { thr_t *me = t_self; if (me) { me->last_svc = num; me->last_lr = regs[30]; me->last_fp = regs[29]; me->nsvc++; } }

    switch (num) {
    case 0x08: {  // CreateThread(→w1 句柄; x1 入口, x2 参数, x3 栈顶, w4 优先级, w5 核) | CreateThread(→w1 handle; x1 entry, x2 argument, x3 stack top, w4 priority, w5 core)
        if(!regs[1]||!regs[3]||(regs[3]&15)||(int)regs[4]<28||(int)regs[4]>59||(int)regs[5]<-3||(int)regs[5]>2){regs[0]=R_BAD_STATE;return 1;}
        thr_t *t = calloc(1, sizeof *t);
        if(!t)die("thread allocation");
        t->cur_core = -1;
        tw_init(&t->cv);
        t->entry = regs[1]; t->arg = regs[2]; t->stack_top = regs[3] & ~0xFull;
        t->prio = (int)regs[4]; t->core = (int)regs[5]; t->mask = t->core >= 0 ? 1ull << t->core : 7;
        t->born_ns = now_ns(); t->born_tick = dm_thread_tick_fn ? dm_thread_tick_fn() : 0;
        g_lock(); t->handle = h_new(O_THREAD, t); t->id = g_next_id++; t->all = g_all; g_all = t; pthread_mutex_unlock(&G);
        // the host stack of this guest thread (translated code, host services): [UIT 0121] Windows 64 MiB reserved and
        // committed as used (the exe's --stack; was 8 MiB committed, too small for the biggest -O0 frames)
        pthread_attr_t a; pthread_attr_init(&a);
        { size_t plat_guest_stack_size(size_t); size_t ss = plat_guest_stack_size(8 << 20); if (ss) pthread_attr_setstacksize(&a, ss); }
        pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);   // 退出即回收宿主资源（客体经句柄等待，不 join） | Host resources are reclaimed on exit (the guest waits through the handle, no join)
        if (pthread_create(&t->pt, &a, thread_main, t)) die("pthread_create");
        pthread_attr_destroy(&a);
        LOG("CreateThread h=%#x entry=%#llx sp=%#llx", t->handle,t->entry,t->stack_top);
        regs[0] = 0; regs[1] = t->handle; return 1;
    }
    case 0x09: {  // StartThread(w0)
        g_lock();
        thr_t *t = h_get((uint32_t)regs[0], O_THREAD);
        if (t) { t->started = 1; tw_signal(&t->cv); }
        pthread_mutex_unlock(&G);
        regs[0] = t ? 0 : R_BAD_HANDLE; return 1;
    }
    case 0x0A: thread_exit(cur()); return 1;
    case 0x0B: {  // SleepThread(x0 ns)；0/-1/-2 = 各类让出 | SleepThread(x0 ns); 0/-1/-2 = the various kinds of yield
        int64_t ns = (int64_t)regs[0];

        thr_t *self = cur();
        uint64_t s0 = now_ns(), cw0 = t_tw[0];   // [UIT 0238]
        if (ns > 0) { g_lock(); core_put(self); pthread_mutex_unlock(&G); }
        if (ns > 0 && dm_detclock_enabled()) {
            uint64_t deadline = guest_ns() + (uint64_t)ns;
            while (guest_ns() < deadline) { struct timespec ts = {0, 1000000}; nanosleep(&ts, 0); }
        } else if (ns > 0) guest_sleep_ns((uint64_t)ns);   // [UIT 0245]
        else {   // 让出（0 / -1 / -2）。主线程的让出次数与耗时记账：游戏渲染任务图「等全部做完」是纯轮询 + 让出（main+0x15ee898），量它占一帧多少 | Yield (0 / -1 / -2). Count and time of the main thread's yields are recorded: the game's render task graph "wait for everything to finish" is pure polling + yield (main+0x15ee898); measure how much of a frame it takes
            thr_t *me = t_self;
            uint64_t y0 = now_ns();
            if (me && me->nsvc != me->yrun_svc + 1) me->yrun_ns = y0;   // another SVC since the last yield: a new run
            if (me) me->yrun_svc = me->nsvc;
            g_lock();   // Horizon yield: hand the core to a waiting thread of equal or higher priority
            int give = 0; for (thr_t *w = g_core_wait; w && !give; w = w->cnext) give = me->cur_core >= 0 && core_allowed(w, me->cur_core) && w->prio <= me->prio;
            if (give) { core_put(me); core_take(me); }
            pthread_mutex_unlock(&G);
            uint64_t backoff = !give && me ? yield_backoff_ns(y0 - me->yrun_ns) : 0;
            if (backoff) {   // a long poll: sleep with the core given up, as SleepThread(ns) does
                g_lock(); core_put(me); pthread_mutex_unlock(&G);
                guest_sleep_ns(backoff);   // [UIT 0245]
                g_lock(); core_take(me); pthread_mutex_unlock(&G);
            } else if (!give) sched_yield();
            if (me && me->handle == 0x1001) { g_main_yield_ns += now_ns() - y0; g_main_yield_n++; }
        }
        if (ns > 0) { t_tw[2] += now_ns() - s0; t_tw[6]++; g_lock(); core_take(self); pthread_mutex_unlock(&G); }
        else { uint64_t d = now_ns() - s0, c = t_tw[0] - cw0; t_tw[3] += d > c ? d - c : 0; t_tw[7]++; }   // [UIT 0238] yields

        return 1;
    }
    case 0x0C: { g_lock();thr_t *t = h_get((uint32_t)regs[1], O_THREAD); regs[0] = t ? 0 : R_BAD_HANDLE; regs[1] = t ? (uint64_t)t->prio : 0; pthread_mutex_unlock(&G);return 1; }
    case 0x0D: { if(regs[1]<28||regs[1]>59){regs[0]=R_BAD_STATE;return 1;}g_lock();thr_t *t = h_get((uint32_t)regs[0], O_THREAD); if (t) t->prio = (int)regs[1]; regs[0] = t ? 0 : R_BAD_HANDLE;pthread_mutex_unlock(&G); return 1; }
    case 0x0E: { g_lock();thr_t *t = h_get((uint32_t)regs[2], O_THREAD);
                 regs[0] = t ? 0 : R_BAD_HANDLE; regs[1] = t ? (uint64_t)(uint32_t)t->core : 0; regs[2] = t ? t->mask : 0;pthread_mutex_unlock(&G); return 1; }
    case 0x0F: {  // SetThreadCoreMask(w0, w1 理想核, x2 掩码)：-1 = 用进程缺省、-2/-3 = 不改理想核；宿主只记账 | SetThreadCoreMask(w0, w1 ideal core, x2 mask): -1 = use the process default, -2/-3 = leave the ideal core unchanged; the host only keeps the bookkeeping
        g_lock();thr_t *t = h_get((uint32_t)regs[0], O_THREAD);
        if (t) { int c = (int)regs[1]; if (c >= 0) t->core = c; else if (c == -1) t->core = 0; if (regs[2]) t->mask = regs[2];
                 if (t == cur() && t->cur_core >= 0 && !core_allowed(t, t->cur_core)) { core_put(t); core_take(t); } }   // migrate off a core no longer allowed
        regs[0] = t ? 0 : R_BAD_HANDLE;pthread_mutex_unlock(&G); return 1;
    }
    case 0x10: { thr_t *t = cur(); regs[0] = (uint64_t)(uint32_t)(t->cur_core >= 0 ? t->cur_core : t->core); return 1; }   // GetCurrentProcessorNumber
    case 0x25: { g_lock();thr_t *t = h_get((uint32_t)regs[1], O_THREAD); regs[0] = t ? 0 : R_BAD_HANDLE; regs[1] = t ? t->id : 0;pthread_mutex_unlock(&G); return 1; }

    case 0x1A: {  // ArbitrateLock(w0 持有者句柄, x1 地址, w2 本线程标签) | ArbitrateLock(w0 owner handle, x1 address, w2 this thread's tag)
        thr_t *t = cur(); uint64_t addr = regs[1];
        g_lock();
        int64_t r = 0;
        if (ld32(addr) == ((uint32_t)regs[0] | WAITERS_BIT)) { t->waddr = addr; t->wtag = (uint32_t)regs[2]; r = block(t, 1, -1); }
        pthread_mutex_unlock(&G);
        regs[0] = (uint64_t)r; return 1;
    }
    case 0x36: {  // [UIT 0076] SynchronizePreemptionState: a switch deferred by the TLR disable_count happens here
        thr_t *t = cur();
        g_lock();
        if (t->tlr) ap_tlr(t)[1] = 0;
        if (g_ap && t->cur_core >= 0 && core_better_waiter(t, t->cur_core)) { core_put(t); core_take(t); }
        pthread_mutex_unlock(&G);
        regs[0] = 0; return 1;
    }
    case 0x1B: g_lock(); lock_handoff(regs[0]); pthread_mutex_unlock(&G); regs[0] = 0; return 1;
    case 0x1C: {  // WaitProcessWideKeyAtomic(x0 锁地址, x1 条件键, w2 标签, x3 超时) | WaitProcessWideKeyAtomic(x0 lock address, x1 condition key, w2 tag, x3 timeout)
        thr_t *t = cur(); uint64_t addr = regs[0], key = regs[1];
        g_lock();
        // 顺序要紧：先置条件键 = 1，再放互斥锁。SDK 的 InternalConditionVariable::Signal 在用户态先读条件键、为 0 就不发 svc 0x1D；
        // 用户态抢互斥锁也不经过 G。原先先 lock_handoff 后置键：放锁后、置键前，别的线程可在用户态抢到锁、改完条件、读到键 0 就不唤醒，
        // 本线程随后挂上去永远等不到（并行负载下资源表读写锁 main+0x320e5e0 卡死：读者计数 0、锁空闲、写者条件键 1、写者仍在等）。
        // 挂进等待链在 block 里、仍持 G：svc 0x1D 要取 G，不会落在这之前
        // The order matters: first set the condition key = 1, then release the mutex. The SDK's InternalConditionVariable::Signal first reads the condition key in user mode, and does not issue svc 0x1D if it is 0;
        // grabbing the mutex in user mode does not go through G either. It used to do lock_handoff first and set the key after: after the release and before the key was set, another thread could grab the lock in user mode, change the condition, read key 0 and not wake anyone,
        // and this thread would then link itself in and wait forever (under parallel load the resource table read-write lock main+0x320e5e0 deadlocked: reader count 0, lock free, writer condition key 1, writer still waiting).
        // Linking into the wait chain happens inside block, still holding G: svc 0x1D has to take G, so it cannot land before this
#ifdef DM_TEST_HOOKS
#define CV_TEST_HOOK() { extern void (*dm_test_cv_after_release)(void); if (dm_test_cv_after_release) dm_test_cv_after_release(); }   // 单测：紧跟放锁之后、挂链之前 | Unit test: right after releasing the lock, before linking into the wait chain
#else
#define CV_TEST_HOOK()
#endif
#ifndef DM_TEST_CV_OLD_ORDER
        st32(key, 1);
        t->wkey = key; t->waddr = addr; t->wtag = (uint32_t)regs[2];
        lock_handoff(addr); CV_TEST_HOOK();
#else   // 仅单测 t_condvar 的负对照：还原修复前「先放锁、后置键」的顺序 | Negative control for the unit test t_condvar only: restores the pre-fix order "release the lock first, set the key after"
        lock_handoff(addr); CV_TEST_HOOK();
        st32(key, 1);
        t->wkey = key; t->waddr = addr; t->wtag = (uint32_t)regs[2];
#endif
        int64_t r = block(t, 2, (int64_t)regs[3]);   // 超时：不重新拿锁（用户态自己拿） | Timeout: the lock is not re-acquired (user mode takes it itself)
        pthread_mutex_unlock(&G);
        regs[0] = (uint64_t)r; return 1;
    }
    case 0x1D: {  // SignalProcessWideKey(x0 条件键, w1 个数，<=0 全部) | SignalProcessWideKey(x0 condition key, w1 count, <=0 all)
        uint64_t key = regs[0]; int cnt = (int)regs[1], n = 0;
        g_lock();
        for (thr_t *w; (cnt <= 0 || n < cnt) && (w = pick(2, key)); n++) lock_acquire_for(w, w->waddr, w->wtag);   // 按内核顺序；转为锁等待或被唤醒后不再是 kind 2 | In kernel order; once turned into a lock wait or woken it is no longer kind 2
        int left = 0; for (thr_t *w = g_wait; w; w = w->next) if (w->wkind == 2 && w->wkey == key) { left = 1; break; }
        if (!left) st32(key, 0);
        pthread_mutex_unlock(&G);
        regs[0] = 0; return 1;
    }

    case 0x34: {  // WaitForAddress(x0 地址, w1 类型, w2 值, x3 超时)：0 小于则等、1 小于则减一再等、2 相等则等 | WaitForAddress(x0 address, w1 type, w2 value, x3 timeout): 0 wait if less than, 1 decrement if less than then wait, 2 wait if equal
        thr_t *t = cur(); uint64_t addr = regs[0]; int type = (int)regs[1]; int32_t val = (int32_t)regs[2];
        g_lock();
        int64_t r; uint32_t v = ld32(addr);
        int ok = type == 2 ? (int32_t)v == val : (int32_t)v < val;
        if (ok && type == 1) { while (!cas32(addr, &v, v - 1)) if (!((int32_t)v < val)) { ok = 0; break; } }
        if (!ok) r = R_BAD_STATE;
        else { t->waddr = addr; r = block(t, 3, (int64_t)regs[3]); }
        pthread_mutex_unlock(&G);
        regs[0] = (uint64_t)r; return 1;
    }
    case 0x35: {  // SignalToAddress(x0 地址, w1 类型, w2 值, w3 个数)：0 唤醒、1 相等则加一并唤醒、2 相等则按等待数改值并唤醒 | SignalToAddress(x0 address, w1 type, w2 value, w3 count): 0 wake, 1 increment if equal and wake, 2 if equal modify the value by waiter count and wake
        uint64_t addr = regs[0]; int type = (int)regs[1]; int32_t val = (int32_t)regs[2]; int cnt = (int)regs[3];
        g_lock();
        int nw = 0; for (thr_t *w = g_wait; w; w = w->next) if (w->wkind == 3 && w->waddr == addr) nw++;
        int64_t r = 0;
        if (type != 0) {
            int32_t nv = val + 1;
            if (type == 2) {
                if (cnt <= 0) nv = nw > 0 ? val - 1 : val + 1;
                else nv = nw == 0 ? val + 1 : nw <= cnt ? val - 1 : val;
            }
            uint32_t e = (uint32_t)val;
            if (!cas32(addr, &e, (uint32_t)nv)) r = R_BAD_STATE;
        }
        if (!r) { int n = 0;
            for (thr_t *w; (cnt <= 0 || n < cnt) && (w = pick(3, addr)); n++) wake(w, 0); }
        pthread_mutex_unlock(&G);
        regs[0] = (uint64_t)r; return 1;
    }

    case 0x45: {  // CreateEvent(→w1 写端, w2 读端) | CreateEvent(→w1 write end, w2 read end)
        event_t *e = calloc(1, sizeof *e); e->refs = 2;
        g_lock(); uint32_t hw = h_new(O_EVENT_W, e), hr = h_new(O_EVENT_R, e); pthread_mutex_unlock(&G);
        regs[0] = 0; regs[1] = hw; regs[2] = hr; return 1;
    }
    case 0x11: case 0x12: case 0x17: {  // SignalEvent(写端) / ClearEvent(任一端) / ResetSignal(读端，未触发报错) | SignalEvent(write end) / ClearEvent(either end) / ResetSignal(read end, error if not signaled)
        g_lock();
        // SignalEvent 只认写端；ClearEvent 两端都认；ResetSignal 只认读端
        // SignalEvent accepts only the write end; ClearEvent accepts both ends; ResetSignal accepts only the read end
        uint32_t h = (uint32_t)regs[0];
        event_t *e = num == 0x11 ? h_peek(h, O_EVENT_W) : num == 0x17 ? h_peek(h, O_EVENT_R)
                   : (h_peek(h, O_EVENT_W) ? h_peek(h, O_EVENT_W) : h_peek(h, O_EVENT_R));
        int64_t r = 0;
        if (!e) { r = (num == 0x17 && h_peek(h, O_THREAD)) ? R_BAD_STATE : R_BAD_HANDLE;
                  if (r == R_BAD_HANDLE) LOG("事件 SVC %#x 句柄无效 %#x", num, h); }
        else if (num == 0x11) { e->signaled = 1; wake_sync_waiters(e); }
        else if (num == 0x17 && !e->signaled) r = R_BAD_STATE;
        else e->signaled = 0;
        pthread_mutex_unlock(&G);
        regs[0] = (uint64_t)r; return 1;
    }
    case 0x18: {  // WaitSynchronization(→w1 下标; x1 句柄数组, w2 个数, x3 超时) | WaitSynchronization(→w1 index; x1 handle array, w2 count, x3 timeout)
        thr_t *t = cur(); const uint32_t *hs = (const uint32_t *)regs[1]; int n = (int)regs[2]; int64_t ns = (int64_t)regs[3];
        if(n<0||n>64||(!hs&&n)){regs[0]=R_BAD_STATE;return 1;}
        int64_t deadline = ns > 0 ? (dm_detclock_enabled() ? (int64_t)guest_ns() : now_mono_ns()) + ns : 0;
        g_lock();
        t->nwh = n; t->whs = hs; for (int i = 0; i < n && i < 4; i++) t->wh[i] = hs[i];
        int64_t r; int idx = -1;
        for (;;) {
            if (t->cancel) { t->cancel = 0; r = R_CANCELLED; break; }
            int bad = 0;
            for (int i = 0; i < n && idx < 0; i++) if (sig_obj(hs[i], &bad)) idx = i;
            if (bad) { r = R_BAD_HANDLE; break; }
            if (idx >= 0) { r = 0; break; }
            int64_t left = ns < 0 ? -1 : ns == 0 ? 0 : deadline - (dm_detclock_enabled() ? (int64_t)guest_ns() : now_mono_ns());
            if (ns > 0 && left <= 0) { r = R_TIMEOUT; break; }
            if (block(t, 4, left) == R_TIMEOUT) { r = R_TIMEOUT; break; }
        }
        t->whs = NULL;
        pthread_mutex_unlock(&G);
        regs[0] = (uint64_t)r; regs[1] = idx >= 0 ? (uint64_t)idx : 0; return 1;
    }
    case 0x19: {  // CancelSynchronization(w0 线程) | CancelSynchronization(w0 thread)
        g_lock();
        thr_t *t = h_get((uint32_t)regs[0], O_THREAD);
        if (t) { t->cancel = 1; if (t->wkind == 4) wake(t, -1); }
        pthread_mutex_unlock(&G);
        regs[0] = t ? 0 : R_BAD_HANDLE; return 1;
    }
    case 0x16: {  // CloseHandle(w0)：事件两端都关才释放；线程句柄关闭后槽位回收，对象在线程也退出后释放 | CloseHandle(w0): an event is freed only when both ends are closed; after a thread handle is closed the slot is recycled, and the object is freed once the thread has also exited
        g_lock();
        unsigned i = (uint32_t)regs[0] - 0x1000;int valid=0;
        if (i > 0 && i < MAX_H && (g_h[i].type == O_EVENT_R || g_h[i].type == O_EVENT_W)) {
            valid=1;event_t *e = g_h[i].obj; if (--e->refs == 0) free(e);
            g_h[i].type = O_FREE; g_h[i].obj = NULL;
        } else if (i > 0 && i < MAX_H && (g_h[i].type == O_SESSION || g_h[i].type == O_TMEM)) {
            valid=1;void *o = g_h[i].obj; int session = g_h[i].type == O_SESSION;
            g_h[i].type = O_FREE; g_h[i].obj = NULL;
            pthread_mutex_unlock(&G);
            if (session) runtime_ipc_session_closed(o); else runtime_ipc_tmem_closed(o);
            g_lock();
        } else if (i > 0 && i < MAX_H && g_h[i].type == O_THREAD) {
            valid=1;thr_t *t = g_h[i].obj;
            g_h[i].type = O_FREE; g_h[i].obj = NULL;
            t->closed = 1;
            if (t->exited) thr_free(t);
        }
        pthread_mutex_unlock(&G);
        regs[0] = valid?0:R_BAD_HANDLE; return 1;
    }
    }
    return 0;
}

// ---------- 服务层的 SystemEvent ----------
// 真机：服务把内核事件的读端交给客户端，客体经 SDK nn::os 等待。宿主：在客体线程上调 SDK 自己的
// CreateSystemEvent(跨进程=true)，SDK 经 SVC 0x45 建出内核事件；记下写端句柄，之后任何线程（含宿主音频线程）都可触发。
// ---------- SystemEvent of the service layer ----------
// Real device: the service hands the read end of a kernel event to the client, and the guest waits on it through SDK nn::os. Host: on a guest thread, call the SDK's own
// CreateSystemEvent(inter-process=true), and the SDK creates the kernel event through SVC 0x45; the write-end handle is recorded, after which any thread (including the host audio thread) can signal it.
static struct { void *ev; uint32_t wh; } g_se[512];
static int g_nse;
static pthread_mutex_t g_se_lock = PTHREAD_MUTEX_INITIALIZER;
static uint32_t se_find(void *ev) {
    for (int i = 0; i < g_nse; i++) if (g_se[i].ev == ev) return g_se[i].wh;
    return 0;
}
void hle_bind_system_event(void *ev, int autoclear) {      // 必须在客体线程上调（要执行 SDK 代码） | Must be called on a guest thread (it has to execute SDK code)
    pthread_mutex_lock(&g_se_lock); uint32_t wh = se_find(ev); pthread_mutex_unlock(&g_se_lock);
    if (wh) return;
    if (!t_self) die("hle_bind_system_event 须在客体线程上调用");
    static void *create, *getw;
    if (!create) { create = runtime_symbol("_ZN2nn2os17CreateSystemEventEPNS0_15SystemEventTypeENS0_14EventClearModeEb");
                   getw = runtime_symbol("_ZN2nn2os30GetWritableHandleOfSystemEventEPKNS0_15SystemEventTypeE"); }
    uint32_t r = (uint32_t)GCALL(create, (uint64_t)(uintptr_t)ev, (uint64_t)autoclear, 1);
    if (r) die("CreateSystemEvent 失败 %#x", r);
    wh = (uint32_t)GCALL(getw, (uint64_t)(uintptr_t)ev);
    pthread_mutex_lock(&g_se_lock);
    if (g_nse >= (int)(sizeof g_se / sizeof g_se[0])) die("SystemEvent 绑定表满");
    g_se[g_nse].ev = ev; g_se[g_nse++].wh = wh;
    pthread_mutex_unlock(&g_se_lock);
}
void hle_signal_system_event(void *ev) {   // 未绑定的按手动清除即时绑定（只能在客体线程上发生） | An unbound one is bound on the spot as manual-clear (can only happen on a guest thread)
    pthread_mutex_lock(&g_se_lock); uint32_t wh = se_find(ev); pthread_mutex_unlock(&g_se_lock);
    if (!wh) { hle_bind_system_event(ev, 0); pthread_mutex_lock(&g_se_lock); wh = se_find(ev); pthread_mutex_unlock(&g_se_lock); }
    g_lock();
    event_t *e = h_get(wh, O_EVENT_W);
    if (!e) die("SystemEvent 写端句柄失效 %#x", wh);
    e->signaled = 1; wake_sync_waiters(e);
    pthread_mutex_unlock(&G);
}
// [UIT 0175] nn::nfp (nfp.c): each Attach*Event call initialises the game's event anew (the game destroys its events
// after nfp::Finalize and may attach the same memory again), so the old binding of that address is dropped first.
void hle_forget_system_event(void *ev) {
    pthread_mutex_lock(&g_se_lock);
    for (int i = 0; i < g_nse; i++) if (g_se[i].ev == ev) { g_se[i] = g_se[--g_nse]; break; }
    pthread_mutex_unlock(&g_se_lock);
}
// [UIT 0175] Any thread: signals a bound event whose handle is still open; 0 (nothing done) otherwise, never fatal.
int hle_signal_system_event_alive(void *ev) {
    pthread_mutex_lock(&g_se_lock); uint32_t wh = se_find(ev); pthread_mutex_unlock(&g_se_lock);
    if (!wh) return 0;
    g_lock();
    event_t *e = h_peek(wh, O_EVENT_W);
    if (e) { e->signaled = 1; wake_sync_waiters(e); }
    pthread_mutex_unlock(&G);
    return e != NULL;
}

// ---------- IPC support (ipc.c): sessions, transfer memory and service-owned events ----------
// Weak fallbacks keep standalone thread tests linkable; the runtime links the real ipc.c versions.
__attribute__((weak)) void runtime_ipc_session_closed(void *s) { (void)s; die("IPC session closed without ipc.c"); }
__attribute__((weak)) void runtime_ipc_tmem_closed(void *t) { (void)t; die("transfer memory closed without ipc.c"); }
uint32_t runtime_handle_new(int session, void *obj) { g_lock(); uint32_t h = h_new(session ? O_SESSION : O_TMEM, obj); pthread_mutex_unlock(&G); return h; }
void *runtime_handle_get(uint32_t h, int session) { g_lock(); void *o = h_peek(h, session ? O_SESSION : O_TMEM); pthread_mutex_unlock(&G); return o; }
// Service-side event: returns the writable handle kept by the service.
uint32_t runtime_event_new(void) {
    event_t *e = calloc(1, sizeof *e); if (!e) die("event allocation"); e->refs = 1;
    g_lock(); uint32_t hw = h_new(O_EVENT_W, e); pthread_mutex_unlock(&G); return hw;
}
// Copy-handle transfer of the readable end: each call gives the client its own handle to the same event.
uint32_t runtime_event_readable(uint32_t hw) {
    g_lock(); event_t *e = h_get(hw, O_EVENT_W); if (!e) die("service event handle %#x", hw);
    e->refs++; uint32_t hr = h_new(O_EVENT_R, e); pthread_mutex_unlock(&G); return hr;
}
void runtime_event_signal(uint32_t hw) {
    g_lock(); event_t *e = h_get(hw, O_EVENT_W); if (!e) die("service event handle %#x", hw);
    e->signaled = 1; wake_sync_waiters(e); pthread_mutex_unlock(&G);
}

// ---------- Diagnostics ----------
// MacNativeGames dumps every guest thread's wait state and guest return chain through Mach APIs (TOTK_DUMP_AFTER).
// TODO(port): a Windows version for hang diagnosis.
void runtime_dump_threads(void) { LOG("thread dump not ported yet"); }
