// Windows host layer (runtime/platform.h).
#include <windows.h>
#include <dbghelp.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <shellapi.h>
#include <timeapi.h>
#include <io.h>

#include "../platform.h"

// ---- guest address space
// The window is reserved up front and committed per region (plat_commit from the loader / kernel). Anything
// the guest touches outside a committed region is committed on its first access by the exception handler
// below, so the window behaves like macOS's all-RW reservation. The OS itself (ReadFile into a guest buffer)
// does not fault: such buffers must be inside committed regions.
static uint64_t g_lo, g_hi;
#define COMMIT_CHUNK 0x10000ull

static bool lazy_commit(uint64_t a) {
    if (a < g_lo || a >= g_hi) return false;
    MEMORY_BASIC_INFORMATION mi;
    if (!VirtualQuery((void *)(uintptr_t)a, &mi, sizeof mi)) return false;
    // Another thread faulted on the same chunk and committed it first: retry the access.
    if (mi.State == MEM_COMMIT) return mi.Protect == PAGE_READWRITE;
    if (mi.State != MEM_RESERVE) return false;
    uint64_t base = a & ~(COMMIT_CHUNK - 1);
    return VirtualAlloc((void *)(uintptr_t)base, COMMIT_CHUNK, MEM_COMMIT, PAGE_READWRITE) != NULL;
}

static bool g_report_crashes;

static LONG CALLBACK vectored_handler(EXCEPTION_POINTERS *ep) {
    EXCEPTION_RECORD *er = ep->ExceptionRecord;
    if (er->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || er->NumberParameters < 2) return EXCEPTION_CONTINUE_SEARCH;
    uint64_t fa = er->ExceptionInformation[1];
    if (lazy_commit(fa)) return EXCEPTION_CONTINUE_EXECUTION;
    if (g_report_crashes && rt_fault_watch(fa, ep->ContextRecord->Rip)) return EXCEPTION_CONTINUE_EXECUTION;
    return EXCEPTION_CONTINUE_SEARCH;
}

// The window keeps write tracking (MEM_WRITE_WATCH: the Vulkan renderer's vertex / index cache re-copies only pages the
// CPU wrote, runtime/nvn/vulkan/exec.c). Without it (refused): no tracking, the renderer reads guest memory in place.
static bool g_write_watch;

bool plat_reserve_guest(uint64_t lo, uint64_t hi) {
    void *p = VirtualAlloc((void *)(uintptr_t)lo, hi - lo, MEM_RESERVE | MEM_WRITE_WATCH, PAGE_READWRITE);
    if (p == (void *)(uintptr_t)lo) g_write_watch = true;
    else p = VirtualAlloc((void *)(uintptr_t)lo, hi - lo, MEM_RESERVE, PAGE_READWRITE);
    if (p != (void *)(uintptr_t)lo) return false;
    g_lo = lo;
    g_hi = hi;
    AddVectoredExceptionHandler(1, vectored_handler);
    return true;
}

int plat_written_pages(uint64_t addr, uint64_t size, uint64_t *pages, int max, uint64_t *page) {
    if (!g_write_watch || addr < g_lo || addr + size > g_hi || max <= 0) return -1;
    ULONG_PTR n = (ULONG_PTR)max;
    DWORD gran = 4096;
    if (GetWriteWatch(WRITE_WATCH_FLAG_RESET, (void *)(uintptr_t)addr, (SIZE_T)size, (PVOID *)pages, &n, &gran) != 0) return -1;
    *page = gran;
    // More than fit: the whole span is reset, the pages not listed are lost - callers give room for every page
    // (nvn_dirty_win.c scan_pages, UIT 0030).
    if (n >= (ULONG_PTR)max) ResetWriteWatch((void *)(uintptr_t)addr, (SIZE_T)size);
    return (int)n;
}

// Decommit everything: the range stays reserved, and pages come back zeroed when committed again (lazily).
bool plat_reset_guest(uint64_t lo, uint64_t hi) {
    return VirtualFree((void *)(uintptr_t)lo, hi - lo, MEM_DECOMMIT) != 0;
}

void plat_commit(uint64_t addr, uint64_t size) {
    if (!size) return;
    uint64_t a = addr & ~0xFFFull, e = (addr + size + 0xFFF) & ~0xFFFull;
    if (!VirtualAlloc((void *)(uintptr_t)a, e - a, MEM_COMMIT, PAGE_READWRITE))
        fprintf(stderr, "[plat] commit %llx+%llx failed (%lu)\n", (unsigned long long)addr,
                (unsigned long long)size, GetLastError());
}

int plat_protect(uint64_t addr, uint64_t size, int prot) {
    DWORD p = prot == PLAT_PROT_NONE ? PAGE_NOACCESS : prot == PLAT_PROT_READ ? PAGE_READONLY : PAGE_READWRITE, old;
    if (addr >= g_lo && addr < g_hi) plat_commit(addr, size);
    return VirtualProtect((void *)(uintptr_t)addr, size, p, &old) ? 0 : -1;
}

// ---- time
static LARGE_INTEGER qpf;
// [UIT 0074] QPC -> ns as a fixed-point multiply set before main (no thread can read it half written): the old
// 128-bit division per call (__udivti3) was ~5% of the game's samples in valentderah's profile (their 8e15d70).
static PlatScale qpc_ns;
static int timer_exact;   // TOTK_TIMER_EXACT, read in plat_init (after boot.c put the cfg into the environment)

__attribute__((constructor)) static void qpc_ns_init(void) {
    QueryPerformanceFrequency(&qpf);
    qpc_ns = plat_scale(1000000000u, (uint64_t)qpf.QuadPart);
}

int plat_timer_exact(void) { return timer_exact; }

uint64_t plat_counter(void) {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return (uint64_t)t.QuadPart;
}

uint64_t plat_counter_freq(void) {
    if (!qpf.QuadPart) QueryPerformanceFrequency(&qpf);
    return (uint64_t)qpf.QuadPart;
}

uint64_t plat_now_ns(void) {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    if (__builtin_expect(!qpc_ns.q && !qpc_ns.f, 0) || timer_exact == 1) {   // before the constructor, or the old way
        if (!qpf.QuadPart) QueryPerformanceFrequency(&qpf);
        return (uint64_t)((unsigned __int128)t.QuadPart * 1000000000u / (uint64_t)qpf.QuadPart);
    }
    return plat_scaled(qpc_ns, (uint64_t)t.QuadPart);
}

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x2
#endif

void plat_sleep_until_ns(uint64_t t) {
    static __thread HANDLE timer;
    if (!timer) {
        timer = CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
        if (!timer) timer = CreateWaitableTimerExW(NULL, NULL, 0, TIMER_ALL_ACCESS);
    }
    for (;;) {
        uint64_t now = plat_now_ns();
        if (now >= t) return;
        uint64_t left = t - now;
        if (left < 200000) { YieldProcessor(); continue; }  // last 0.2 ms: spin (timer slack)
        LARGE_INTEGER due = {.QuadPart = -(LONGLONG)((left - 100000) / 100)};  // relative, 100 ns units
        if (!timer || !SetWaitableTimer(timer, &due, 0, NULL, NULL, FALSE)) { Sleep(1); continue; }
        WaitForSingleObject(timer, INFINITE);
    }
}

// [UIT 0245] A guest sleep of ns (SleepThread), precise: a high-resolution waitable timer (Windows 10 1803+) of the
// calling thread, waited again for what is left until plat_now_ns() has passed the end; only the last 20 us spin. Never
// returns early. winpthreads' nanosleep (the old way) rounds every sleep up to whole milliseconds and sleeps in Sleep(1)
// steps, so a guest sleep of 0.1-1 ms took about 1-2 ms with timeBeginPeriod(1). Returns 0 (nothing slept) when this
// thread has no high-resolution timer: the caller sleeps the old way.
int plat_sleep_ns_precise(uint64_t ns) {
    static __thread HANDLE timer;
    static __thread int tried;
    if (!tried) { tried = 1; timer = CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS); }
    if (!timer) return 0;
    uint64_t end = plat_now_ns() + ns;
    for (;;) {
        uint64_t now = plat_now_ns();
        if (now >= end) return 1;
        uint64_t left = end - now;
        if (left <= 20000) { YieldProcessor(); continue; }
        LARGE_INTEGER due = {.QuadPart = -(LONGLONG)((left - 10000) / 100)};   // relative, 100 ns units
        if (!SetWaitableTimer(timer, &due, 0, NULL, NULL, FALSE)) { Sleep(0); continue; }
        WaitForSingleObject(timer, INFINITE);
    }
}

// A thread every frame waits for (the replay of NVN command lists into Vulkan): above the game's ~50 worker threads,
// which otherwise preempt it on a busy CPU (MacNativeGames measured 6.6 -> 13 ms per frame on macOS; on the Great Sky
// Island the replay ran ~40 ms of a 42 ms frame with ~29 ms of CPU time, 2026-10-24). Not time-critical: Windows and the
// game's audio threads stay above it.
void plat_thread_boost(void) { SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST); plat_thread_mmcss(); }

// [UIT native-1031] TOTK_THREAD_MMCSS=1 (launcher: Performance > CPU > Multimedia scheduling for frame threads): the
// threads every frame waits for (the translation thread, the recording threads, the game's present thread) join the
// Multimedia Class Scheduler Service task "Games" (AvSetMmThreadCharacteristicsW, priority AVRT_PRIORITY_HIGH), the
// Windows counterpart of the macOS time-constraint policy the Mac code asks for (on Windows that call is a stub: the
// longplay log's "time-constraint policy ...: failed" and "realtime policy inactive"). MMCSS raises the thread into the
// real-time priority band while it runs and keeps Windows' share for the rest of the system (SystemResponsiveness),
// and it is what Windows itself uses for game and audio threads. Through GetProcAddress: no import of avrt.dll.
int plat_thread_mmcss(void) {
    static int on = -1;
    static __thread int done;   // 1 joined, -1 failed
    if (on < 0) { const char *e = getenv("TOTK_THREAD_MMCSS"); on = e && *e == '1'; }
    if (!on) return 0;
    if (done) return done;
    typedef HANDLE (WINAPI *set_fn)(LPCWSTR, LPDWORD);
    typedef BOOL (WINAPI *prio_fn)(HANDLE, int);
    static set_fn set; static prio_fn prio; static int loaded;
    if (!__atomic_load_n(&loaded, __ATOMIC_ACQUIRE)) {
        HMODULE m = LoadLibraryA("avrt.dll");
        set = m ? (set_fn)(void *)GetProcAddress(m, "AvSetMmThreadCharacteristicsW") : NULL;
        prio = m ? (prio_fn)(void *)GetProcAddress(m, "AvSetMmThreadPriority") : NULL;
        __atomic_store_n(&loaded, 1, __ATOMIC_RELEASE);
    }
    DWORD idx = 0;
    HANDLE h = set ? set(L"Games", &idx) : NULL;
    if (h && prio) prio(h, 1 /* AVRT_PRIORITY_HIGH */);
    done = h ? 1 : -1;
    fprintf(stderr, "[totk] TOTK_THREAD_MMCSS=1: thread %lu %s (priority now %d)\n", (unsigned long)GetCurrentThreadId(),
            h ? "joined MMCSS task \"Games\" at high priority" : set ? "could not join MMCSS (AvSetMmThreadCharacteristicsW failed)" : "could not join MMCSS (avrt.dll missing)",
            GetThreadPriority(GetCurrentThread()));
    return done;
}
int plat_thread_priority(void) { int p = GetThreadPriority(GetCurrentThread()); return p == THREAD_PRIORITY_ERROR_RETURN ? 99 : p; }

// [UIT 0033] TOTK_VK_PCORES=1: the calling thread (the translation thread, the recording threads) runs on the
// performance cores of a hybrid CPU (pcores.h), not where Windows puts it (its efficiency cores are ~half as fast for
// this work). Returns the number of performance-core CPUs it was given, 0 when nothing was done (not hybrid, off,
// or the calls are missing: Windows 10 before 1709). Through GetProcAddress: the exe still starts on older systems.
// [UIT 0089] TOTK_HYBRID_PLACEMENT: p = also the game's threads (and the presentation thread) on the performance
// cores; 1 = that, and the background work (shader compiling, the staging-memory thread, save flushing, unpacking of
// big files) on the efficiency cores (plat_thread_ecores). Affinity only (CPU sets): priorities are not changed.
#include <stdint.h>
#include "pcores.h"
static int g_place = -1;   // TOTK_HYBRID_PLACEMENT: 0 off, 1 P-cores, 2 P-cores and E-cores
static int place_mode(void) {
    int m = __atomic_load_n(&g_place, __ATOMIC_RELAXED);
    if (m < 0) {
        const char *e = getenv("TOTK_HYBRID_PLACEMENT");
        m = !e || !*e || !strcmp(e, "0") || !strcmp(e, "-") ? 0 : !strcmp(e, "p") ? 1 : 2;
        __atomic_store_n(&g_place, m, __ATOMIC_RELAXED);
    }
    return m;
}
typedef BOOL (WINAPI *cpuset_set_fn)(HANDLE, const ULONG *, ULONG);
static int g_np = -1, g_ne;
static ULONG g_pids[256], g_eids[256];
// [UIT 0237] TOTK_FRAME_CORE (on unless 0; needs P-core placement: TOTK_VK_PCORES=1 or TOTK_HYBRID_PLACEMENT): the
// game's frame thread (the one that acquires and presents, on the critical path of every frame) gets one physical
// performance core to itself; every other thread placed on the performance cores (the game's other threads, the
// translation, recording and submit threads, the presentation thread) gets the remaining ones (pcores.h,
// frame_core_split). Before, the recording and translation threads (MMCSS: the real-time band) shared every P-core with
// the frame thread and could preempt it or run on its hyperthread sibling. g_nr 0: no split (off, not hybrid, or fewer
// than 4 physical P-cores).
static int g_nf, g_nr;
static ULONG g_fids[8], g_rids[256];
// [UIT 0358] TOTK_VK_RECORD_CORES=N (1-4, off unless set; needs the frame core split above): N physical P-cores of the
// shared ones for the GPU command threads alone (pcores.h rec_core_split); g_nk 0: not split
static int g_nk;
static ULONG g_kids[64];
static int rec_cores_want(void) { const char *e = getenv("TOTK_VK_RECORD_CORES"); int v = e && *e ? atoi(e) : 0; return v < 0 ? 0 : v > 4 ? 4 : v; }
static int frame_core_on(void) { const char *e = getenv("TOTK_FRAME_CORE"); return !(e && e[0] == '0'); }
static cpuset_set_fn g_cpuset_set;
static int cpu_sets(void) {   // once (the first thread; the others reuse the lists): the P-core and E-core CPU sets
    if (__atomic_load_n(&g_np, __ATOMIC_ACQUIRE) >= 0) return g_np;
    static SRWLOCK mu = SRWLOCK_INIT;
    AcquireSRWLockExclusive(&mu);
    if (g_np < 0) {
        typedef BOOL (WINAPI *get_fn)(PSYSTEM_CPU_SET_INFORMATION, ULONG, PULONG, HANDLE, ULONG);
        HMODULE k32 = GetModuleHandleA("kernel32.dll");
        get_fn get = k32 ? (get_fn)(void *)GetProcAddress(k32, "GetSystemCpuSetInformation") : NULL;
        g_cpuset_set = k32 ? (cpuset_set_fn)(void *)GetProcAddress(k32, "SetThreadSelectedCpuSets") : NULL;
        int c = 0, k = 0, ke = 0;
        uint8_t cls[256], core[256]; uint32_t sid[256], pick[256];
        if (get && g_cpuset_set) {
            ULONG len = 0;
            get(NULL, 0, &len, GetCurrentProcess(), 0);
            uint8_t *buf = len ? malloc(len) : NULL;
            if (buf && get((PSYSTEM_CPU_SET_INFORMATION)buf, len, &len, GetCurrentProcess(), 0))
                for (ULONG o = 0; o < len && c < 256;) {
                    PSYSTEM_CPU_SET_INFORMATION i = (PSYSTEM_CPU_SET_INFORMATION)(buf + o);
                    if (!i->Size) break;
                    if (i->Type == CpuSetInformation) { cls[c] = i->CpuSet.EfficiencyClass; sid[c] = i->CpuSet.Id; core[c] = i->CpuSet.CoreIndex; c++; }
                    o += i->Size;
                }
            free(buf);
            k = pcore_pick(cls, sid, c, pick);
            for (int j = 0; j < k; j++) g_pids[j] = pick[j];
            ke = k ? ecore_pick(cls, sid, c, pick) : 0;
            for (int j = 0; j < ke; j++) g_eids[j] = pick[j];
            if (k && frame_core_on()) {   // [UIT 0237]
                uint32_t fr[256], rest[256]; int nf = 0;
                int nr = frame_core_split(cls, sid, core, c, fr, &nf, rest);
                if (nr > 0 && nf > 0 && nf <= 8) {
                    for (int j = 0; j < nf; j++) g_fids[j] = fr[j];
                    for (int j = 0; j < nr; j++) g_rids[j] = rest[j];
                    g_nf = nf; g_nr = nr;
                    int want = rec_cores_want();   // [UIT 0358]
                    if (want) {
                        uint8_t rco[256]; uint32_t rec[256], rest2[256]; int nk = 0;
                        for (int j = 0; j < nr; j++)
                            for (int q = 0; q < c; q++) if (sid[q] == rest[j]) { rco[j] = core[q]; break; }
                        int n2 = rec_core_split(rest, rco, nr, want, rec, &nk, rest2);
                        if (n2 > 0 && nk > 0 && nk <= 64) {
                            for (int j = 0; j < nk; j++) g_kids[j] = rec[j];
                            for (int j = 0; j < n2; j++) g_rids[j] = rest2[j];
                            g_nk = nk; g_nr = n2;
                        }
                    }
                }
            }
        }
        g_ne = ke;
        fprintf(stderr, "[totk] hybrid CPU: %d logical CPUs, %s\n", c,
                !g_cpuset_set ? "CPU sets not available (Windows 10 before 1709): nothing to do"
                : k ? "performance and efficiency cores found" : "not hybrid (one core class): nothing to do");
        if (k) fprintf(stderr, "[totk] hybrid CPU: %d performance-core CPUs, %d efficiency-core CPUs for background work; "
                       "TOTK_VK_PCORES %s, TOTK_HYBRID_PLACEMENT %s\n", k, ke,
                       getenv("TOTK_VK_PCORES") && *getenv("TOTK_VK_PCORES") == '1' ? "on" : "off",
                       place_mode() == 2 ? "on (game and graphics threads on P-cores, background work on E-cores)"
                       : place_mode() == 1 ? "on (game and graphics threads on P-cores)" : "off");
        if (k) fprintf(stderr, "[totk] [UIT 0237] TOTK_FRAME_CORE: %s\n",
                       !frame_core_on() ? "off (TOTK_FRAME_CORE=0): the frame thread shares the performance cores"
                       : g_nr ? "on (default): one physical performance core for the game's frame thread once it has presented 60 times in a row, the other P-core threads on the rest"
                       : "on, but fewer than 4 physical performance cores: not split");
        if (g_nr) fprintf(stderr, "[totk] [UIT 0237] frame thread core: %d CPU(s); the other P-core threads: %d CPUs\n", g_nf, g_nr);
        if (rec_cores_want()) fprintf(stderr, "[totk] [UIT 0358] TOTK_VK_RECORD_CORES=%d: %s\n", rec_cores_want(),
                                      g_nk ? "the GPU command threads have their own physical performance cores" :
                                      "not split (needs the frame core split and 3 performance cores left for the other threads)");
        if (g_nk) fprintf(stderr, "[totk] [UIT 0358] GPU command threads: %d CPUs of their own; the other P-core threads: %d CPUs\n", g_nk, g_nr);
        __atomic_store_n(&g_np, k, __ATOMIC_RELEASE);
    }
    ReleaseSRWLockExclusive(&mu);
    return g_np;
}
int plat_thread_pcores(void) {
    static int on = -1;
    if (on < 0) { const char *e = getenv("TOTK_VK_PCORES"); on = (e && *e == '1') || place_mode() >= 1; }
    if (!on) return 0;
    int k = cpu_sets();
    if (k > 0 && g_nr > 0) k = g_cpuset_set(GetCurrentThread(), g_rids, (ULONG)g_nr) ? g_nr : 0;   // [UIT 0237]
    else if (k > 0 && !g_cpuset_set(GetCurrentThread(), g_pids, (ULONG)k)) k = 0;
    return k > 0 ? k : 0;
}
// [UIT 0358] a GPU command (recording) thread: its own physical cores with TOTK_VK_RECORD_CORES, else as plat_thread_pcores
int plat_thread_record(void) {
    int k = plat_thread_pcores();
    if (k > 0 && g_nk > 0 && g_cpuset_set(GetCurrentThread(), g_kids, (ULONG)g_nk)) return g_nk;
    return k;
}
// [UIT 0237] the calling thread is the game's frame thread (at each acquire / present). The reserved physical P-core
// goes to the thread that has presented FRAME_CORE_STREAK times in a row (a boot or loading thread that presented
// first gives it up to the game's real frame thread once that one has presented that often; two threads presenting
// in turn do not move it back and forth). The previous owner goes back to the shared performance cores. Needs the
// P-core placement on (only then are the CPU sets of the others split). Returns the CPUs it runs on now (0: shared).
enum { FRAME_CORE_STREAK = 60 };
int plat_thread_frame(void) {
    static __thread int streak, mine, failed;
    static LONG last, owner;   // thread ids (atomics; owner written under mu)
    static HANDLE owner_h;
    static SRWLOCK mu = SRWLOCK_INIT;
    static int on = -1;
    if (mine) {   // the owner still records that it presented (so another thread's streak restarts)
        LONG me = (LONG)GetCurrentThreadId();
        if (__atomic_load_n(&owner, __ATOMIC_ACQUIRE) == me) {
            if (__atomic_load_n(&last, __ATOMIC_RELAXED) != me) __atomic_store_n(&last, me, __ATOMIC_RELAXED);
            return g_nf;
        }
        mine = 0; streak = 0;   // the core went to another thread
    }
    int o = __atomic_load_n(&on, __ATOMIC_RELAXED);
    if (o < 0) {
        const char *e = getenv("TOTK_VK_PCORES");
        o = (e && *e == '1') || place_mode() >= 1;
        __atomic_store_n(&on, o, __ATOMIC_RELAXED);
    }
    if (!o || failed || cpu_sets() <= 0 || g_nr <= 0) return 0;
    LONG me = (LONG)GetCurrentThreadId();
    if (__atomic_exchange_n(&last, me, __ATOMIC_RELAXED) != me) streak = 0;
    if (++streak < FRAME_CORE_STREAK) return 0;
    HANDLE h = OpenThread(THREAD_SET_LIMITED_INFORMATION | THREAD_QUERY_LIMITED_INFORMATION, FALSE, (DWORD)me);
    if (!h || !g_cpuset_set(GetCurrentThread(), g_fids, (ULONG)g_nf)) {
        failed = 1;
        fprintf(stderr, "[totk] [UIT 0237] frame thread %lu: could not take its core (%lu): it keeps the shared cores\n", (unsigned long)me, (unsigned long)GetLastError());
        if (h) CloseHandle(h);
        return 0;
    }
    AcquireSRWLockExclusive(&mu);
    LONG prev = owner;
    if (owner_h) {   // the previous frame thread back on the shared cores (it may have exited: then this fails harmlessly)
        g_cpuset_set(owner_h, g_rids, (ULONG)g_nr);
        CloseHandle(owner_h);
    }
    owner_h = h; __atomic_store_n(&owner, me, __ATOMIC_RELEASE); mine = 1;
    ReleaseSRWLockExclusive(&mu);
    if (prev) fprintf(stderr, "[totk] [UIT 0237] frame core moves from thread %lu to thread %lu (it presented %d times in a row)\n", (unsigned long)prev, (unsigned long)me, FRAME_CORE_STREAK);
    else fprintf(stderr, "[totk] [UIT 0237] frame thread %lu now on its own performance core (%d CPUs)\n", (unsigned long)me, g_nf);
    return g_nf;
}
// [UIT 0089] background work: the efficiency cores, with TOTK_HYBRID_PLACEMENT=1 on a hybrid CPU (0: nothing done)
int plat_thread_ecores(void) {
    if (place_mode() != 2 || cpu_sets() <= 0 || g_ne <= 0) return 0;
    return g_cpuset_set(GetCurrentThread(), g_eids, (ULONG)g_ne) ? g_ne : 0;
}
// [UIT 0089] a game thread (TOTK_HYBRID_PLACEMENT p or 1): the performance cores (0: nothing done)
int plat_thread_game(void) { return place_mode() >= 1 ? plat_thread_pcores() : 0; }

void plat_thread_realtime(uint64_t period_ns) {
    (void)period_ns;
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
}

// ---- process
// [UIT 0259] UltraInstinctTears.exe is a GUI-subsystem executable (tools/build_game.py links it with
// -Wl,--subsystem,windows), so Windows never gives it a console window. Before 0259 it was a console program: started by
// the launcher's Play (no standard handles) Windows opened a console for it, and every line the game wrote went to that
// console one character at a time (stderr is unbuffered: one WriteFile per character, each a round trip to conhost,
// under the stream lock the other game threads wait on; a click in the console with QuickEdit on stopped every thread
// that wrote a line until a key was pressed). Play with log gave it a file instead, which is why it ran smoother.
// Where its standard error goes now:
//   - a file or a pipe (the launcher's Play with log, tools/warm_shaders.py, a redirect): written as before, unbuffered,
//     so nothing is lost if the game stops;
//   - the NUL device (the launcher's Play since 0259): kept nothing, so it gets a 64 KiB buffer: a line is a copy into
//     memory, not a system call per character;
//   - nothing (UltraInstinctTears.exe started by double-click): lasrecomp.log next to the exe, made new each run (no
//     append mode, which seeks to the end before every write), 64 KiB buffered and written out by a low-priority thread
//     every 200 ms, so the game threads only copy into memory. Fatal errors, crash reports and plat_terminate flush it.
static char g_log_path[MAX_PATH];
static int g_log_buffered;   // 1: stderr has a buffer (NUL or lasrecomp.log): flushed before the process ends

static bool is_nul_device(HANDLE h) {   // a character device that is not a console: NUL
    DWORD mode;
    return GetFileType(h) == FILE_TYPE_CHAR && !GetConsoleMode(h, &mode);
}

static DWORD WINAPI log_flusher(void *arg) {   // lasrecomp.log: written off the game threads
    (void)arg;
    for (;;) {
        Sleep(200);
        fflush(stderr);
    }
    return 0;
}

static void log_to_file_without_console(void) {
    HANDLE h = GetStdHandle(STD_ERROR_HANDLE);
    if (h && h != INVALID_HANDLE_VALUE) {   // given by whoever started the game
        if (is_nul_device(h)) {
            setvbuf(stderr, NULL, _IOFBF, 1 << 16);
            g_log_buffered = 1;
        }
        return;
    }
    char exe[MAX_PATH];
    DWORD n = GetModuleFileNameA(NULL, exe, sizeof exe);
    if (!n || n >= sizeof exe) return;
    char *slash = strrchr(exe, '\\');
    if (slash) slash[1] = 0;
    snprintf(g_log_path, sizeof g_log_path, "%slasrecomp.log", exe);
    if (!freopen(g_log_path, "wb", stderr)) { g_log_path[0] = 0; return; }
    setvbuf(stderr, NULL, _IOFBF, 1 << 16);
    g_log_buffered = 1;
    _dup2(_fileno(stderr), _fileno(stdout));
    HANDLE t = CreateThread(NULL, 64 << 10, log_flusher, NULL, STACK_SIZE_PARAM_IS_A_RESERVATION, NULL);
    if (t) {
        SetThreadPriority(t, THREAD_PRIORITY_BELOW_NORMAL);
        CloseHandle(t);
    } else {   // no writer thread: unbuffered as before 0259
        setvbuf(stderr, NULL, _IONBF, 0);
        g_log_buffered = 0;
    }
}

// The recompiled game code is built for x86-64-v3 (Makefile GENARCH: AVX2, FMA, BMI2): say so instead of crashing
// with an illegal instruction on an older CPU.
static void require_cpu(void) {
    __builtin_cpu_init();
    if (__builtin_cpu_supports("avx2") && __builtin_cpu_supports("fma") && __builtin_cpu_supports("bmi2")) return;
    MessageBoxA(NULL, "LAS Recomp needs a CPU with AVX2 (Intel Haswell / 2013 or newer, AMD Excavator / Zen or newer).",
                "Ultra Instinct Tears", MB_ICONERROR | MB_OK);
    ExitProcess(1);
}

// Fatal error (rt_fatal_trace): the last FATAL / exception line of the log in a message box, when there is no console.
void plat_fatal_notice(void) {
    if (!g_log_path[0]) return;
    fflush(stderr);
    char text[1024] = "Ultra Instinct Tears stopped because of an error.";
    FILE *f = fopen(g_log_path, "rb");
    if (f) {
        static char tail[16384];
        fseek(f, 0, SEEK_END);
        long sz = ftell(f), from = sz > (long)sizeof tail - 1 ? sz - (long)sizeof tail + 1 : 0;
        fseek(f, from, SEEK_SET);
        size_t got = fread(tail, 1, sizeof tail - 1, f);
        tail[got] = 0;
        fclose(f);
        char *line = NULL;
        for (char *p = tail; (p = strstr(p, "FATAL: ")) != NULL; p++) line = p;
        if (!line) for (char *p = tail; (p = strstr(p, "*** ")) != NULL; p++) line = p;
        if (line) {
            char *end = strchr(line, '\n');
            if (end) *end = 0;
            snprintf(text, sizeof text, "Ultra Instinct Tears stopped because of an error:\n\n%.600s", line);
            if (strstr(line, "Vulkan") || strstr(line, "VK_") || strstr(line, "vk")) {
                size_t l = strlen(text);
                snprintf(text + l, sizeof text - l, "\n\nUltra Instinct Tears requires Vulkan 1.3 and VK_EXT_shader_object. "
                         "Install the latest driver for your GPU. If the error remains, the GPU is not supported.");
            }
        }
    }
    size_t l = strlen(text);
    snprintf(text + l, sizeof text - l, "\n\nDetails: %s", g_log_path);
    MessageBoxA(NULL, text, "Ultra Instinct Tears", MB_ICONERROR | MB_OK);
}

// [UIT 0295] started by plat_restart_self: the old process may still be ending (its log file, its save commit). Wait
// for it before anything else, lasrecomp.log included; one getenv when the variable is not there.
static void restart_wait(void) {
    const char *e = getenv("UIT_RESTART_WAIT_PID");   // UIT_ENV_RESTART_WAIT
    if (!e || !*e) return;
    DWORD pid = (DWORD)strtoul(e, NULL, 10);
    _putenv_s("UIT_RESTART_WAIT_PID", "");   // not passed on to anything this process starts
    HANDLE h = pid ? OpenProcess(SYNCHRONIZE, FALSE, pid) : NULL;
    if (!h) return;   // already gone
    WaitForSingleObject(h, 15000);
    CloseHandle(h);
}

void plat_init(void) {
    restart_wait();
    log_to_file_without_console();
    // [UIT 0259] a GUI program's abort() shows the CRT's own "abort() has been called" box: plat_fatal_notice has
    // already said what happened (the Windows Error Reporting step after it is left as it was)
    _set_abort_behavior(0, _WRITE_ABORT_MSG);
    require_cpu();
    _fmode = _O_BINARY;  // fopen/open default to binary (the runtime's "r"/"w" files are binary or LF text)
    _setmode(_fileno(stdout), _O_BINARY);
    _setmode(_fileno(stderr), _O_BINARY);
    SetConsoleOutputCP(CP_UTF8);
    { const char *e = getenv("TOTK_TIMER_EXACT"); timer_exact = e && *e == '1'; }   // [UIT 0074]
    timeBeginPeriod(1);  // 1 ms Sleep / condition-variable timeouts (default 15.6 ms)
    // Opt out of EcoQoS and of Windows 11 ignoring the timer resolution while the window is occluded.
    PROCESS_POWER_THROTTLING_STATE pt = {
        .Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION,
        .ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED | PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION,
        .StateMask = 0,
    };
    SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &pt, sizeof pt);
}

bool plat_exe_path(char *buf, size_t size) {
    DWORD n = GetModuleFileNameA(NULL, buf, (DWORD)size);
    return n > 0 && n < size;
}

// Quote one argument the way CommandLineToArgvW / the CRT parse it back.
static void append_arg(char *out, size_t cap, const char *a) {
    size_t n = strlen(out);
    bool q = !*a || strpbrk(a, " \t\"") != NULL;
    if (n && n + 1 < cap) out[n++] = ' ';
    if (q && n + 1 < cap) out[n++] = '"';
    for (const char *p = a; *p; p++) {
        int bs = 0;
        while (*p == '\\') { bs++; p++; }
        int reps = !*p ? (q ? 2 * bs : bs) : *p == '"' ? 2 * bs + 1 : bs;
        for (int i = 0; i < reps && n + 1 < cap; i++) out[n++] = '\\';
        if (!*p) break;
        if (n + 1 < cap) out[n++] = *p;
    }
    if (q && n + 1 < cap) out[n++] = '"';
    out[n] = 0;
}

// No exec() on Windows: start the new process with the same console / standard handles, then exit.
void plat_exec(char *const argv[]) {
    STARTUPINFOW si = {.cb = sizeof si};
    PROCESS_INFORMATION pi;
    BOOL ok;
    fflush(stdout);
    fflush(stderr);
    // [UIT 0259] a GUI program's standard handles are not passed on by themselves: hand over this one's (its stderr
    // file: the given handle, or lasrecomp.log, which the CRT opened inheritable), so the new process logs to the same
    // place and does not start a second lasrecomp.log over this one.
    HANDLE eh = (HANDLE)_get_osfhandle(_fileno(stderr));
    if (eh && eh != INVALID_HANDLE_VALUE) {
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
        si.hStdOutput = eh;
        si.hStdError = eh;
    }
    if (!argv) {
        ok = CreateProcessW(NULL, GetCommandLineW(), NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi);
    } else {
        static char cmd[32768];
        cmd[0] = 0;
        for (int i = 0; argv[i]; i++) append_arg(cmd, sizeof cmd, argv[i]);
        static wchar_t wcmd[32768];
        MultiByteToWideChar(CP_UTF8, 0, cmd, -1, wcmd, 32768);
        ok = CreateProcessW(NULL, wcmd, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi);
    }
    if (!ok) return;
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    ExitProcess(0);
}

// ---- diagnostics
// DLL frames are named module+offset; frames in our own executable are left to the caller, which maps
// them to guest functions (runtime/dispatch.c), and [UIT 0120] named from the PDB the link writes
// (build/UltraInstinctTears.pdb, plat_symbol_pdb) when it is there.
const char *plat_symbol(uint64_t pc, uint64_t *offset) {
    static __thread char name[MAX_PATH];
    HMODULE m;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCSTR)(uintptr_t)pc, &m))
        return NULL;
    if (m == GetModuleHandleA(NULL)) return NULL;
    char path[MAX_PATH];
    if (!GetModuleFileNameA(m, path, sizeof path)) return NULL;
    const char *b = strrchr(path, '\\');
    snprintf(name, sizeof name, "%s", b ? b + 1 : path);
    if (offset) *offset = pc - (uint64_t)(uintptr_t)m;
    return name;
}

const char *rt_host_fn_name(uint64_t pc, uint64_t *offset);  // runtime/dispatch.c

void plat_print_backtrace(void) {
    void *bt[64];
    USHORT n = RtlCaptureStackBackTrace(0, 64, bt, NULL);
    uint64_t exe = (uint64_t)(uintptr_t)GetModuleHandleA(NULL);
    for (int i = 0; i < n; i++) {
        uint64_t pc = (uint64_t)(uintptr_t)bt[i], off = 0;
        const char *s = plat_symbol(pc, &off);
        if (!s) s = rt_host_fn_name(pc, &off);
        uint64_t po = 0;
        const char *line = NULL, *ps = plat_symbol_pdb(pc, &po, &line);   // (NULL outside our executable)
        char pdb[600] = "";
        if (ps) snprintf(pdb, sizeof pdb, " [%s+%llx%s%s]", ps, (unsigned long long)po, line ? " " : "", line ? line : "");
        if (s) fprintf(stderr, "  #%-2d %016llx %s+%llx%s\n", i, (unsigned long long)pc, s, (unsigned long long)off, pdb);
        else fprintf(stderr, "  #%-2d %016llx lasrecomp+%llx%s\n", i, (unsigned long long)pc,
                     (unsigned long long)(pc - exe), pdb);
    }
}

// ---- [UIT 0120] crash report detail
// 0104's crash ("f_8055ded64+453aa", host lr 0x866ff8) could not be placed: the f_ name is only the nearest table
// function (the lifted bodies of native_libc fronts and all runtime / library code are not in the table) and [rsp] was
// no return address. The report now names the function from the PDB, says whether the pc has an unwind entry, and
// prints what is needed to find the culprit without the user's files: registers, code bytes, the allocation the
// fault address lies next to (every 0104 fault was a read of the free page just below a 64 KiB-aligned allocation:
// the thread's own stack, probed below its base by compiler-rt ___chkstk_ms for a ~8.4 MiB frame), and the stack
// words ([rsp+0x10] is then the return address into the function whose frame did not fit).
static int g_pdb = -1;   // -1 not set up yet, 0 off / failed, 1 ready
static bool exe_range(uint64_t a, uint64_t *base) {
    HMODULE m;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCSTR)(uintptr_t)a, &m) || m != GetModuleHandleA(NULL))
        return false;
    if (base) *base = (uint64_t)(uintptr_t)m;
    return true;
}
static void pdb_setup(void) {
    const char *e = getenv("TOTK_CRASH_PDB");
    g_pdb = 0;
    if (e && (!strcmp(e, "0") || !_stricmp(e, "off"))) return;
    char dir[MAX_PATH];
    DWORD n = GetModuleFileNameA(NULL, dir, sizeof dir);
    if (!n || n >= sizeof dir) return;
    char *b = strrchr(dir, '\\');
    if (b) *b = 0;
    HANDLE p = GetCurrentProcess();
    SymSetOptions(SymGetOptions() | SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES | SYMOPT_FAIL_CRITICAL_ERRORS |
                  SYMOPT_NO_PROMPTS);
    // FALSE: only our executable (the DLLs are named by plat_symbol); the profiler (TOTK_PROF) may have set it up already
    // the link writes the PDB to build\UltraInstinctTears.pdb (tools/build_game.py); search there and next to the exe
    char path[2 * MAX_PATH + 16];
    snprintf(path, sizeof path, "%s;%s\\build", dir, dir);
    if (!SymInitialize(p, path, FALSE) && GetLastError() != ERROR_INVALID_PARAMETER) return;
    char exe[MAX_PATH];
    if (!GetModuleFileNameA(NULL, exe, sizeof exe)) return;
    uint64_t base = (uint64_t)(uintptr_t)GetModuleHandleA(NULL);
    if (!SymLoadModuleEx(p, NULL, exe, NULL, base, 0, NULL, 0) && GetLastError() != ERROR_SUCCESS) {
        // already loaded (the profiler) is fine; anything else: no names
        IMAGEHLP_MODULE64 mi = {.SizeOfStruct = sizeof mi};
        if (!SymGetModuleInfo64(p, base, &mi)) return;
    }
    g_pdb = 1;
}
const char *plat_symbol_pdb(uint64_t pc, uint64_t *offset, const char **line) {
    static __thread char name[512], where[600];
    if (line) *line = NULL;
    if (!exe_range(pc, NULL)) return NULL;
    if (g_pdb < 0) pdb_setup();
    if (g_pdb != 1) return NULL;
    union { SYMBOL_INFO si; char buf[sizeof(SYMBOL_INFO) + 256]; } u;
    memset(&u, 0, sizeof u);
    u.si.SizeOfStruct = sizeof u.si;
    u.si.MaxNameLen = 255;
    DWORD64 off = 0;
    HANDLE p = GetCurrentProcess();
    if (!SymFromAddr(p, pc, &off, &u.si)) return NULL;
    snprintf(name, sizeof name, "%s", u.si.Name);
    if (offset) *offset = off;
    IMAGEHLP_LINE64 ln = {.SizeOfStruct = sizeof ln};
    DWORD disp = 0;
    if (line && SymGetLineFromAddr64(p, pc, &disp, &ln) && ln.FileName) {
        const char *f = ln.FileName, *s1 = strrchr(f, '\\'), *s2 = strrchr(f, '/');
        if (s2 > s1) s1 = s2;
        snprintf(where, sizeof where, "%s:%lu", s1 ? s1 + 1 : f, ln.LineNumber);
        *line = where;
    }
    return name;
}

DWORD WINAPI K32GetMappedFileNameA(HANDLE, LPVOID, LPSTR, DWORD);   // kernel32 (no psapi in the builder's sysroot)
static CONTEXT t_fault_ctx;   // (one report per process: the filter ends it)
static uint64_t t_fault_addr;
static DWORD t_fault_code;    // [UIT 0121] the exception code (EXCEPTION_STACK_OVERFLOW: the overflow line below)
static volatile bool t_fault_set;

static bool readable(uint64_t a, size_t n) {
    MEMORY_BASIC_INFORMATION mi;
    if (!VirtualQuery((void *)(uintptr_t)a, &mi, sizeof mi) || mi.State != MEM_COMMIT) return false;
    if (mi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) return false;
    return a + n <= (uint64_t)(uintptr_t)mi.BaseAddress + mi.RegionSize;
}
static const char *state_name(DWORD s) { return s == MEM_COMMIT ? "commit" : s == MEM_RESERVE ? "reserve" : s == MEM_FREE ? "free" : "?"; }
static const char *type_name(DWORD t) { return t == MEM_IMAGE ? "image" : t == MEM_MAPPED ? "mapped" : t == MEM_PRIVATE ? "private" : "-"; }
// one address: its module (+offset) or our function
static void name_addr(uint64_t a, char *out, size_t cap) {
    uint64_t off = 0, base = 0;
    const char *s, *line = NULL;
    out[0] = 0;
    if ((s = plat_symbol(a, &off))) snprintf(out, cap, "%s+%llx", s, (unsigned long long)off);
    else if (exe_range(a, &base)) {
        uint64_t po = 0;
        const char *ps = plat_symbol_pdb(a, &po, &line);
        if (ps) snprintf(out, cap, "exe+%llx %s+%llx%s%s", (unsigned long long)(a - base), ps, (unsigned long long)po,
                         line ? " " : "", line ? line : "");
        else snprintf(out, cap, "exe+%llx", (unsigned long long)(a - base));
    }
}
// the allocation (VirtualAlloc reservation, mapped view or image) at a, whole: base and size
static void describe_alloc(const char *what, uint64_t a) {
    MEMORY_BASIC_INFORMATION mi;
    if (!VirtualQuery((void *)(uintptr_t)a, &mi, sizeof mi)) { fprintf(stderr, "    %s %#llx: unknown\n", what, (unsigned long long)a); return; }
    if (mi.State == MEM_FREE) {
        fprintf(stderr, "    %s %#llx: free %p+%#llx\n", what, (unsigned long long)a, mi.BaseAddress, (unsigned long long)mi.RegionSize);
        return;
    }
    uint64_t ab = (uint64_t)(uintptr_t)mi.AllocationBase, end = ab;
    for (int i = 0; i < 4096; i++) {   // the allocation's regions in turn, to its end
        MEMORY_BASIC_INFORMATION r;
        if (!VirtualQuery((void *)(uintptr_t)end, &r, sizeof r) || (uint64_t)(uintptr_t)r.AllocationBase != ab || r.State == MEM_FREE) break;
        end = (uint64_t)(uintptr_t)r.BaseAddress + r.RegionSize;
    }
    char file[MAX_PATH] = "";
    if (mi.Type == MEM_MAPPED || mi.Type == MEM_IMAGE) {
        char full[MAX_PATH];
        if (K32GetMappedFileNameA(GetCurrentProcess(), (void *)(uintptr_t)a, full, sizeof full)) {
            const char *b = strrchr(full, '\\');
            snprintf(file, sizeof file, " file %s", b ? b + 1 : full);
        }
    }
    NT_TIB *tib = (NT_TIB *)NtCurrentTeb();
    uint64_t sb = (uint64_t)(uintptr_t)tib->StackBase, sl = (uint64_t)(uintptr_t)tib->StackLimit;
    fprintf(stderr, "    %s %#llx: %s %s protect %#lx, region %p+%#llx; allocation %#llx..%#llx (%#llx bytes)%s%s\n", what,
            (unsigned long long)a, state_name(mi.State), type_name(mi.Type), mi.Protect, mi.BaseAddress,
            (unsigned long long)mi.RegionSize, (unsigned long long)ab, (unsigned long long)end, (unsigned long long)(end - ab),
            file, a >= sl && a < sb ? " (this thread's stack)" : "");
}

void plat_fault_detail(void) {
    if (!t_fault_set) return;
    const CONTEXT *x = &t_fault_ctx;
    uint64_t pc = x->Rip, fa = t_fault_addr;
    char nm[700];
    fprintf(stderr, "    [UIT 0120] host detail (thread %lu):\n", GetCurrentThreadId());
    name_addr(pc, nm, sizeof nm);
    if (!nm[0]) snprintf(nm, sizeof nm, "not in a module");
    fprintf(stderr, "    host pc %llx = %s%s\n", (unsigned long long)pc, nm,
            g_pdb == 1 ? "" : g_pdb == 0 ? " (no PDB names: TOTK_CRASH_PDB=0, or build\\UltraInstinctTears.pdb missing)" : "");
    DWORD64 ib = 0;
    RUNTIME_FUNCTION *rf = RtlLookupFunctionEntry(pc, &ib, NULL);
    if (rf) {
        char fn[700];
        name_addr(ib + rf->BeginAddress, fn, sizeof fn);
        fprintf(stderr, "    unwind entry: function rva %#lx..%#lx (+%#llx into it) %s\n", rf->BeginAddress, rf->EndAddress,
                (unsigned long long)(pc - ib - rf->BeginAddress), fn);
    } else
        fprintf(stderr, "    unwind entry: none (a leaf with no frame, or code without unwind info: the backtrace's next frame "
                        "is a guess from [rsp])\n");
    fprintf(stderr, "    rax %016llx rbx %016llx rcx %016llx rdx %016llx\n    rsi %016llx rdi %016llx rbp %016llx rsp %016llx\n"
                    "    r8  %016llx r9  %016llx r10 %016llx r11 %016llx\n    r12 %016llx r13 %016llx r14 %016llx r15 %016llx\n"
                    "    eflags %08lx mxcsr %08lx\n",
            (unsigned long long)x->Rax, (unsigned long long)x->Rbx, (unsigned long long)x->Rcx, (unsigned long long)x->Rdx,
            (unsigned long long)x->Rsi, (unsigned long long)x->Rdi, (unsigned long long)x->Rbp, (unsigned long long)x->Rsp,
            (unsigned long long)x->R8, (unsigned long long)x->R9, (unsigned long long)x->R10, (unsigned long long)x->R11,
            (unsigned long long)x->R12, (unsigned long long)x->R13, (unsigned long long)x->R14, (unsigned long long)x->R15,
            x->EFlags, x->MxCsr);
    // which registers point near the fault address (the base the code used)
    if (fa) {
        static const char *const rn[16] = {"rax", "rcx", "rdx", "rbx", "rsp", "rbp", "rsi", "rdi", "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15"};
        const DWORD64 *r = &x->Rax;   // CONTEXT keeps Rax..R15 in this order
        char near_[256] = "";
        size_t at = 0;
        for (int i = 0; i < 16 && at < sizeof near_ - 32; i++) {
            int64_t d = (int64_t)(fa - r[i]);
            if (d > -0x10000 && d < 0x10000)
                at += (size_t)snprintf(near_ + at, sizeof near_ - at, " %s%s%#llx", rn[i], d < 0 ? "-" : "+", (unsigned long long)(d < 0 ? -d : d));
        }
        fprintf(stderr, "    fault address from:%s\n", at ? near_ : " no register within 64 KiB");
    }
    if (readable(pc - 32, 48)) {
        const uint8_t *b = (const uint8_t *)(uintptr_t)(pc - 32);
        char hex[200];
        size_t at = 0;
        for (int i = 0; i < 48; i++) at += (size_t)snprintf(hex + at, sizeof hex - at, i == 32 ? "|%02x" : " %02x", b[i]);
        fprintf(stderr, "    code pc-32..pc+16:%s\n", hex);
    }
    if (fa) {
        describe_alloc("fault", fa);
        uint64_t pg = fa & ~0xfffull;
        describe_alloc("below", pg - 1);
        describe_alloc("above", pg + 0x1000);
    }
    NT_TIB *tib = (NT_TIB *)NtCurrentTeb();
    // TEB DeallocationStack (x64 offset 0x1478): the base of this thread's whole stack reservation
    uint64_t sres = *(const uint64_t *)((const char *)NtCurrentTeb() + 0x1478), stop = (uint64_t)(uintptr_t)tib->StackBase;
    fprintf(stderr, "    thread stack %#llx..%#llx (%#llx bytes reserved), committed down to %p, rsp %#llx bytes below its top\n",
            (unsigned long long)sres, (unsigned long long)stop, (unsigned long long)(stop - sres), tib->StackLimit,
            (unsigned long long)(stop - x->Rsp));
    // [UIT 0121] an overflow is either the guard page at the end of the reservation (EXCEPTION_STACK_OVERFLOW, the fault
    // address just above its base) or, on a fully committed stack without one, an access below its base
    if (t_fault_code == EXCEPTION_STACK_OVERFLOW || (fa && sres && fa < sres && sres - fa <= (64ull << 20))) {
        char where[64];
        if (fa && sres && fa < sres) snprintf(where, sizeof where, "%#llx bytes below this thread's stack", (unsigned long long)(sres - fa));
        else if (fa && sres) snprintf(where, sizeof where, "%#llx bytes above its base (the guard page)", (unsigned long long)(fa - sres));
        else snprintf(where, sizeof where, "not given");
        fprintf(stderr, "    STACK OVERFLOW: the fault address is %s (a frame bigger than the room left; in ___chkstk_ms "
                        "the frame size asked for is [rsp] = %#llx and the function asking is [rsp+0x10])\n",
                where, readable(x->Rsp, 8) ? (unsigned long long)*(const uint64_t *)(uintptr_t)x->Rsp : 0ull);
    }
    fprintf(stderr, "    stack words at rsp (code addresses named):\n");
    for (int i = 0; i < 32; i++) {
        uint64_t a = x->Rsp + 8ull * (uint64_t)i;
        if (!readable(a, 8)) break;
        uint64_t v = *(const uint64_t *)(uintptr_t)a;
        name_addr(v, nm, sizeof nm);
        fprintf(stderr, "      rsp+%02x %016llx%s%s\n", i * 8, (unsigned long long)v, nm[0] ? " " : "", nm);
    }
}

static LONG WINAPI unhandled_filter(EXCEPTION_POINTERS *ep) {
    EXCEPTION_RECORD *er = ep->ExceptionRecord;
    uint64_t fa = er->NumberParameters >= 2 ? er->ExceptionInformation[1] : 0;
    char what[64];
    uint64_t sres = *(const uint64_t *)((const char *)NtCurrentTeb() + 0x1478);   // TEB DeallocationStack
    if (er->ExceptionCode == EXCEPTION_STACK_OVERFLOW || (er->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && sres && fa < sres && sres - fa <= (64ull << 20)))
        snprintf(what, sizeof what, "STACK OVERFLOW (exception %08lx)", er->ExceptionCode);   // [UIT 0121]
    else
        snprintf(what, sizeof what, "exception %08lx", er->ExceptionCode);
    MEMORY_BASIC_INFORMATION fm;
    if (er->NumberParameters >= 2 && VirtualQuery((void *)(uintptr_t)fa, &fm, sizeof fm))
        fprintf(stderr, "[plat] fault %s %#llx: region %p+%#llx state %#lx protect %#lx (allocation %p)\n",
                er->ExceptionInformation[0] == 1 ? "write" : er->ExceptionInformation[0] == 8 ? "execute" : "read",
                (unsigned long long)fa, fm.BaseAddress, (unsigned long long)fm.RegionSize, fm.State, fm.Protect,
                fm.AllocationBase);
    // x64 has no link register: report the return address on top of the stack (valid at function entry).
    uint64_t lr = 0;
    MEMORY_BASIC_INFORMATION mi;
    if (VirtualQuery((void *)(uintptr_t)ep->ContextRecord->Rsp, &mi, sizeof mi) && mi.State == MEM_COMMIT)
        lr = *(uint64_t *)(uintptr_t)ep->ContextRecord->Rsp;
    t_fault_ctx = *ep->ContextRecord;   // [UIT 0120] plat_fault_detail (rt_fault_report prints it after its first lines)
    t_fault_addr = fa;
    t_fault_code = er->ExceptionCode;
    t_fault_set = true;
    rt_fault_report(what, fa, ep->ContextRecord->Rip, lr);
}

// [UIT 0121] Kept free at the end of a stack for the crash report after an overflow (the report names symbols through
// dbghelp, which needs more than 64 KiB the first time it reads the PDB).
#define REPORT_STACK (256u << 10)

void plat_crash_handler_install(void) {
    ULONG guarantee = REPORT_STACK;  // room for the report after a stack overflow
    SetThreadStackGuarantee(&guarantee);
    if (g_report_crashes) return;
    g_report_crashes = true;
    SetUnhandledExceptionFilter(unhandled_filter);
}

// [UIT 0121] Host stacks for translated code. A lifted function compiled at -O0 can need megabytes of frame, which
// ___chkstk_ms touches page by page. winpthreads passes pthread_attr_setstacksize to _beginthreadex as the COMMIT
// size, so the old 8 MiB guest-thread stacks were fully committed with no guard page: an 8.4 MiB frame read below the
// reservation (exception c0000005 in ___chkstk_ms, the 0x80559cc20 crash). tools/build_game.py links the exe with
// --stack PLAT_GUEST_STACK: every thread reserves 64 MiB and commits only what it uses, and a thread created with the
// default size gets that. An exe linked without it (an old build_game.py) keeps the old committed size.
size_t plat_guest_stack_size(size_t fallback) {
    const IMAGE_DOS_HEADER *dos = (const IMAGE_DOS_HEADER *)GetModuleHandleW(NULL);
    const IMAGE_NT_HEADERS64 *nt = (const IMAGE_NT_HEADERS64 *)((const char *)dos + dos->e_lfanew);
    uint64_t reserve = nt->OptionalHeader.SizeOfStackReserve, commit = nt->OptionalHeader.SizeOfStackCommit;
    bool ok = reserve >= PLAT_GUEST_STACK;
    static volatile LONG said;
    if (!InterlockedExchange(&said, 1))
        fprintf(stderr, "[plat] game thread stacks: %s (exe stack reserve %llu MiB, commit %llu KiB)\n",
                ok ? "64 MiB reserved each, committed as used" : "fully committed (the exe was linked without --stack)",
                (unsigned long long)(reserve >> 20), (unsigned long long)(commit >> 10));
    return ok ? 0 : fallback;
}

void plat_guest_thread_init(void) {
    ULONG guarantee = REPORT_STACK;
    SetThreadStackGuarantee(&guarantee);
}

// ---- POSIX shims (win_compat.h)
// The kernel does not take the lazy-commit fault: a guest buffer handed to ReadFile / WriteFile fails with
// ERROR_NOACCESS on reserved pages. Commit those first (only reserved ones: committed pages keep their protection).
static void commit_reserved(uint64_t a, uint64_t n) {
    if (!n || a >= g_hi || a + n <= g_lo) return;
    uint64_t end = a + n;
    while (a < end) {
        MEMORY_BASIC_INFORMATION mi;
        if (!VirtualQuery((void *)(uintptr_t)a, &mi, sizeof mi)) return;
        uint64_t next = (uint64_t)(uintptr_t)mi.BaseAddress + mi.RegionSize;
        if (mi.State == MEM_RESERVE) plat_commit(a, (next < end ? next : end) - a);
        a = next;
    }
}

// [UIT native] file reads, for the long-frame log (plat_io_stats): time in ReadFile and bytes read, all threads.
static volatile LONG64 g_io_ns, g_io_bytes;
void plat_io_stats(uint64_t *ns, uint64_t *bytes) { *ns = (uint64_t)g_io_ns; *bytes = (uint64_t)g_io_bytes; }
static ssize_t prw_(int fd, void *buf, size_t n, off_t off, bool write);
static ssize_t prw(int fd, void *buf, size_t n, off_t off, bool write) {
    if (write) return prw_(fd, buf, n, off, write);
    LARGE_INTEGER a, b, fq;
    QueryPerformanceCounter(&a);
    ssize_t r = prw_(fd, buf, n, off, write);
    QueryPerformanceCounter(&b); QueryPerformanceFrequency(&fq);
    InterlockedAdd64(&g_io_ns, (LONG64)((double)(b.QuadPart - a.QuadPart) * 1e9 / (double)fq.QuadPart));
    if (r > 0) InterlockedAdd64(&g_io_bytes, (LONG64)r);
    return r;
}
static ssize_t prw_(int fd, void *buf, size_t n, off_t off, bool write) {
    HANDLE h = (HANDLE)_get_osfhandle(fd);
    if (h == INVALID_HANDLE_VALUE) return -1;
    commit_reserved((uint64_t)(uintptr_t)buf, n);
    size_t done = 0;
    while (done < n) {
        OVERLAPPED ov = {0};
        uint64_t o = (uint64_t)off + done;
        ov.Offset = (DWORD)o;
        ov.OffsetHigh = (DWORD)(o >> 32);
        DWORD chunk = n - done > 0x40000000 ? 0x40000000 : (DWORD)(n - done), got = 0;
        BOOL ok = write ? WriteFile(h, (const char *)buf + done, chunk, &got, &ov)
                        : ReadFile(h, (char *)buf + done, chunk, &got, &ov);
        if (!ok) {
            if (GetLastError() == ERROR_HANDLE_EOF) break;
            return done ? (ssize_t)done : -1;
        }
        if (!got) break;
        done += got;
    }
    return (ssize_t)done;
}
ssize_t pread(int fd, void *buf, size_t n, off_t off) { return prw(fd, buf, n, off, false); }
ssize_t pwrite(int fd, const void *buf, size_t n, off_t off) { return prw(fd, (void *)buf, n, off, true); }

// ---- child processes
PlatProc plat_spawn(const char *const argv[], const char *log_path, bool append_log) {
    static __thread char cmd[32768];
    cmd[0] = 0;
    for (int i = 0; argv[i]; i++) append_arg(cmd, sizeof cmd, argv[i]);
    static __thread wchar_t wcmd[32768], wlog[MAX_PATH];
    MultiByteToWideChar(CP_UTF8, 0, cmd, -1, wcmd, 32768);
    MultiByteToWideChar(CP_UTF8, 0, log_path ? log_path : "NUL", -1, wlog, MAX_PATH);
    SECURITY_ATTRIBUTES sa = {.nLength = sizeof sa, .bInheritHandle = TRUE};
    HANDLE log = CreateFileW(wlog, append_log ? FILE_APPEND_DATA : GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                             &sa, append_log ? OPEN_ALWAYS : CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (log == INVALID_HANDLE_VALUE) return 0;
    STARTUPINFOW si = {.cb = sizeof si, .dwFlags = STARTF_USESTDHANDLES, .hStdInput = NULL, .hStdOutput = log,
                       .hStdError = log};
    PROCESS_INFORMATION pi;
    // CreateProcess doesn't resolve relative paths written with '/' (e.g. a helper under tools/): pass the program
    // as a full path, like posix_spawn takes it as given
    static __thread wchar_t wprog[MAX_PATH], wfull[MAX_PATH];
    MultiByteToWideChar(CP_UTF8, 0, argv[0], -1, wprog, MAX_PATH);
    for (wchar_t *p = wprog; *p; p++) if (*p == L'/') *p = L'\\';
    const wchar_t *app = GetFullPathNameW(wprog, MAX_PATH, wfull, NULL) ? wfull : NULL;
    BOOL ok = CreateProcessW(app, wcmd, NULL, NULL, TRUE, CREATE_NO_WINDOW | CREATE_SUSPENDED, NULL, NULL, &si, &pi);
    if (!ok) fprintf(stderr, "[plat] could not start %s (error %lu)\n", argv[0], GetLastError());
    CloseHandle(log);
    if (!ok) return 0;
    // every child in one job that ends with this process (a compiler's own children too: no leftovers on quit)
    static HANDLE job;
    if (!job && (job = CreateJobObjectW(NULL, NULL))) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION li = {0};
        li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(job, JobObjectExtendedLimitInformation, &li, sizeof li);
    }
    if (job) AssignProcessToJobObject(job, pi.hProcess);
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    return (PlatProc)pi.hProcess;
}

void plat_proc_kill(PlatProc p) {  // (what it started ends with this process: the job above)
    TerminateProcess((HANDLE)p, 255);
    WaitForSingleObject((HANDLE)p, INFINITE);
    CloseHandle((HANDLE)p);
}

int plat_proc_wait(PlatProc p, bool block) {
    HANDLE h = (HANDLE)p;
    DWORD w = WaitForSingleObject(h, block ? INFINITE : 0);
    if (w == WAIT_TIMEOUT) return -1;
    if (w != WAIT_OBJECT_0) return 255;  // (not a process we can wait for: ended as far as the caller is concerned)
    DWORD code = 255;
    GetExitCodeProcess(h, &code);
    CloseHandle(h);
    return code > 255 ? 255 : (int)code;  // (a crash's NTSTATUS, e.g. 0xC0000409, is not "still running" (-1))
}

bool plat_remove_tree(const char *dir) {
    // SHFileOperation wants a double-NUL-terminated path with backslashes
    static __thread wchar_t w[MAX_PATH + 2];
    int n = MultiByteToWideChar(CP_UTF8, 0, dir, -1, w, MAX_PATH);
    if (n <= 1) return false;
    w[n] = 0;
    for (wchar_t *p = w; *p; p++) if (*p == L'/') *p = L'\\';
    SHFILEOPSTRUCTW op = {.wFunc = FO_DELETE, .pFrom = w, .fFlags = FOF_NO_UI};
    return SHFileOperationW(&op) == 0 && !op.fAnyOperationsAborted;
}

// End the process at once, skipping DLL teardown (from an atexit handler, after flushing). Exit status 0.
void plat_terminate(void) { plat_terminate_code(0); }
void plat_terminate_code(int code) {
    if (g_log_buffered) fflush(stderr);   // [UIT 0259] TerminateProcess skips the CRT's own flush
    TerminateProcess(GetCurrentProcess(), (UINT)code);
}

// [UIT 0295] Restart game, started by hand (no launcher). The new process gets this one's standard error only if this
// one was given it (a redirect, NUL); otherwise none, so it opens its own lasrecomp.log as a double-click does (after
// restart_wait, so this process has finished writing the old one).
int plat_restart_self(void) {
    char pid[16];
    snprintf(pid, sizeof pid, "%lu", (unsigned long)GetCurrentProcessId());
    SetEnvironmentVariableA("UIT_RESTART_WAIT_PID", pid);   // UIT_ENV_RESTART_WAIT; the new process reads it
    fflush(stderr);
    STARTUPINFOW si = {.cb = sizeof si, .dwFlags = STARTF_USESTDHANDLES};
    BOOL inherit = FALSE;
    HANDLE eh = (HANDLE)_get_osfhandle(_fileno(stderr));
    if (!g_log_path[0] && eh && eh != INVALID_HANDLE_VALUE) {
        si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
        si.hStdOutput = eh;
        si.hStdError = eh;
        inherit = TRUE;
    }
    wchar_t exe[MAX_PATH];
    DWORD n = GetModuleFileNameW(NULL, exe, MAX_PATH);
    PROCESS_INFORMATION pi;
    BOOL ok = n > 0 && n < MAX_PATH &&
              CreateProcessW(exe, GetCommandLineW(), NULL, NULL, inherit, 0, NULL, NULL, &si, &pi);
    SetEnvironmentVariableA("UIT_RESTART_WAIT_PID", NULL);
    if (!ok) return 0;
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return 1;
}
