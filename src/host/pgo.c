// [UIT 0019] Profile-guided optimization (tools/setup.py --pgo-profile, then --pgo): in the profiling build
// (-fprofile-generate, -DUIT_PGO_GEN) the counters go to <exe folder>\build\pgo\uit-<pid>.profraw every 30 s and at
// exit. Each write holds the counts so far (they are never reset), goes to a .tmp first and then replaces the
// .profraw, so a crash or a closed window loses at most the last 30 s and never leaves a broken file.
// In every other build this file is empty.
#ifdef UIT_PGO_GEN
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

int __llvm_profile_write_file(void);
void __llvm_profile_set_filename(const char *name);

static char g_tmp[MAX_PATH + 64], g_out[MAX_PATH + 64];
static SRWLOCK g_lock = SRWLOCK_INIT;

static void pgo_dump(void) {
    AcquireSRWLockExclusive(&g_lock);
    if (g_tmp[0] && __llvm_profile_write_file() == 0) MoveFileExA(g_tmp, g_out, MOVEFILE_REPLACE_EXISTING);
    ReleaseSRWLockExclusive(&g_lock);
}
static DWORD WINAPI pgo_thread(void *arg) {
    (void)arg;
    for (;;) { Sleep(30000); pgo_dump(); }
    return 0;
}
static void pgo_exit(void) { pgo_dump(); }

__attribute__((constructor)) static void pgo_init(void) {
    char dir[MAX_PATH];
    DWORD n = GetModuleFileNameA(NULL, dir, sizeof dir);
    if (!n || n >= sizeof dir) return;
    char *slash = strrchr(dir, '\\');
    if (!slash) return;
    *slash = 0;
    char sub[MAX_PATH + 16];
    snprintf(sub, sizeof sub, "%s\\build", dir); CreateDirectoryA(sub, NULL);
    snprintf(sub, sizeof sub, "%s\\build\\pgo", dir); CreateDirectoryA(sub, NULL);
    DWORD pid = GetCurrentProcessId();
    snprintf(g_tmp, sizeof g_tmp, "%s\\uit-%lu.profraw.tmp", sub, (unsigned long)pid);
    snprintf(g_out, sizeof g_out, "%s\\uit-%lu.profraw", sub, (unsigned long)pid);
    __llvm_profile_set_filename(g_tmp);
    fprintf(stderr, "[pgo] profiling build: counters go to %s every 30 s and at exit; play normally (10+ minutes, the "
                    "areas and actions you care about), then run setup.py --pgo\n", g_out);
    atexit(pgo_exit);
    HANDLE t = CreateThread(NULL, 0, pgo_thread, NULL, 0, NULL);
    if (t) { SetThreadPriority(t, THREAD_PRIORITY_BELOW_NORMAL); CloseHandle(t); }
}
#else
typedef int uit_pgo_unused;   // ISO C: a translation unit is not empty
#endif
