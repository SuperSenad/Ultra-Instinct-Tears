// Host OS layer: everything the runtime needs that differs between macOS and Windows.
// Implementations: runtime/platform/macos.c, runtime/platform/windows.c (the Makefile picks one).
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef _WIN32
#include "platform/win_compat.h"
#elif defined(__linux__)
#include "platform/linux_compat.h"
#endif

#ifdef __cplusplus
extern "C" {
#endif

// ---- guest address space
// Claim [lo, hi) for the guest at exactly that address, readable/writable (pages may be backed lazily).
bool plat_reserve_guest(uint64_t lo, uint64_t hi);
// Make sure [addr, addr+size) inside the guest window is backed (before the OS itself writes there, e.g.
// file reads); a no-op where the whole window is backed from the start.
void plat_commit(uint64_t addr, uint64_t size);
// In-place reboot: every page of [lo, hi) back to zero (and unbacked), the window still claimed.
bool plat_reset_guest(uint64_t lo, uint64_t hi);
// Pages of [addr, addr+size) in the guest window the CPU wrote since the last call over them (write tracking: Windows
// MEM_WRITE_WATCH), their addresses into pages[] (at most max), and the tracking reset for the range. Returns the
// count (max: maybe more), or -1 where there is no tracking (macOS, Linux). Page size: *page.
int plat_written_pages(uint64_t addr, uint64_t size, uint64_t *pages, int max, uint64_t *page);
enum { PLAT_PROT_NONE = 0, PLAT_PROT_READ = 1, PLAT_PROT_RW = 3 };
int plat_protect(uint64_t addr, uint64_t size, int prot);  // 0 on success
#define PLAT_PAGE 0x4000ull  // largest page size we deal with (macOS 16K; multiple of Windows' 4K)

// ---- time
uint64_t plat_now_ns(void);                // monotonic
// [UIT 0074] the raw monotonic counter (Windows: QueryPerformanceCounter; Linux: nanoseconds) and its rate in Hz
uint64_t plat_counter(void);
uint64_t plat_counter_freq(void);
#include "platform/timer_scale.h"   // [UIT 0074] PlatScale: t * num / den without a division per call
// [UIT 0074] TOTK_TIMER_EXACT=1 (Advanced > Troubleshooting > Exact timer division): the old per-call 128-bit division
int plat_timer_exact(void);
void plat_sleep_until_ns(uint64_t t);      // precise wait for plat_now_ns() >= t
int plat_sleep_ns_precise(uint64_t ns);    // [UIT 0245] a guest sleep: never early, not rounded up to ms (0: not available, sleep the old way)
void plat_thread_realtime(uint64_t period_ns);
int plat_thread_pcores(void);  // [UIT 0033] TOTK_VK_PCORES=1: on a hybrid CPU, the calling thread prefers the performance cores (0: nothing done)
int plat_thread_record(void);  // [UIT 0358] a GPU command thread: plat_thread_pcores, or its own cores with TOTK_VK_RECORD_CORES
int plat_thread_ecores(void);  // [UIT 0089] TOTK_HYBRID_PLACEMENT=1: background work on the efficiency cores (0: nothing done)
int plat_thread_game(void);    // [UIT 0089] TOTK_HYBRID_PLACEMENT p or 1: a game thread on the performance cores (0: nothing done)
int plat_thread_frame(void);   // [UIT 0237] TOTK_FRAME_CORE: the game's frame thread on its own physical P-core (0: shared)
void plat_thread_boost(void);   // a thread the frame waits for (the replay thread): above the game's workers (TOTK_THREAD_MMCSS=1: also MMCSS "Games")
int plat_thread_mmcss(void);    // [UIT native-1031] TOTK_THREAD_MMCSS=1: the calling thread joins MMCSS "Games" (1 joined now or before, 0 off, -1 failed)
int plat_thread_priority(void);
void plat_io_stats(uint64_t *ns, uint64_t *bytes);   // [UIT native] file reads so far (Windows: pread's ReadFile time and bytes) // [UIT native-1031] the calling thread's priority (Windows THREAD_PRIORITY_*; Linux: nice); 99 = unknown  // periodic timing thread (vsync): don't coalesce its timers

// ---- process
void plat_init(void);                                // first thing in main()
bool plat_exe_path(char *buf, size_t size);
void plat_exec(char *const argv[]);  // replace the process (NULL = same command line); returns only on failure
#if defined(_WIN32) || defined(__linux__)
void plat_terminate(void);  // end the process now, skipping DLL teardown (exit status 0)
void plat_terminate_code(int code);   // [UIT 0295] the same, with this exit status (the stderr buffer flushed first)
// [UIT 0295] the overlay's Restart game without the launcher: start this executable again with the same command line,
// folder, environment and standard error, plus UIT_RESTART_WAIT_PID (src/host/game_exit.h) so the new process waits
// in plat_init (at most 15 s) until this one has ended. 1: started (the caller then exits), 0: failed.
int plat_restart_self(void);
#endif

// Child processes (seed generator, game-file extraction). stdout + stderr go to log_path (NULL = /dev/null).
typedef intptr_t PlatProc;  // 0 = failed to start
PlatProc plat_spawn(const char *const argv[], const char *log_path, bool append_log);
int plat_proc_wait(PlatProc p, bool block);  // exit code, or -1 while still running (block = false)
void plat_proc_kill(PlatProc p);             // end it and what it started (then it's gone: no wait)
bool plat_remove_tree(const char *dir);      // rm -rf
#ifdef _WIN32
#define PLAT_EXE_SUFFIX ".exe"
#define PLAT_VENV_PYTHON "venv/Scripts/python.exe"  // relative to the venv's parent
#else
#define PLAT_EXE_SUFFIX ""
#define PLAT_VENV_PYTHON "venv/bin/python"
#endif

// ---- diagnostics
// Name of the host function containing pc (guest functions are f_<addr>), NULL if unknown.
const char *plat_symbol(uint64_t pc, uint64_t *offset);
void plat_print_backtrace(void);
// [UIT 0120] Our own executable's function at pc from its PDB (Windows: dbghelp, build/UltraInstinctTears.pdb next to
// the build; TOTK_CRASH_PDB=0 skips it), NULL if unknown or not ours. line (may be NULL): "file:line" when known.
const char *plat_symbol_pdb(uint64_t pc, uint64_t *offset, const char **line);
// [UIT 0120] The crash handler's host detail for the faulting thread (registers, unwind entry, code bytes, the memory
// around the fault address, the stack words): printed by rt_fault_report after its first lines. Nothing elsewhere.
void plat_fault_detail(void);
void plat_fatal_notice(void);  // a fatal error is about to end the process: tell the user (Windows release: message box)
void plat_crash_handler_install(void);  // per thread where the OS needs it (alternate signal stack)
// [UIT 0121] The host stack of a thread that runs translated game code (every guest thread, the game's main thread):
// the size to give pthread_attr_setstacksize, or 0 = leave the default. Windows: 0 when the exe reserves
// PLAT_GUEST_STACK per thread (linked with --stack: reserved only, committed as the stack grows through its guard
// page), else `fallback` (the old fully committed size). Elsewhere: PLAT_GUEST_STACK (mmap'd, committed as touched).
#define PLAT_GUEST_STACK (64u << 20)
size_t plat_guest_stack_size(size_t fallback);
// [UIT 0121] First thing on such a thread: Windows keeps stack room for the crash report after a stack overflow.
void plat_guest_thread_init(void);
// Called by the crash handler (runtime/dispatch.c): true = fault handled (watchpoint), resume.
bool rt_fault_watch(uint64_t fault_addr, uint64_t host_pc);
void rt_fault_report(const char *what, uint64_t fault_addr, uint64_t host_pc, uint64_t host_lr) __attribute__((noreturn));

#ifdef __cplusplus
}
#endif
