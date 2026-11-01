// Copyright (c) 2026 Valiantsin Dzerakh (valentderah/totk-pc-recomp, MIT; see THIRD_PARTY_NOTICES / third_party/README.md)
// Adapted from valentderah/totk-pc-recomp runtime/kernel.c @ c3b9a4e (kern_ap_stub, ap_try, ap_init), itself adapted
// from MacNativeGames games/totk/runtime/threads.c @ 33e8fa8 (MIT, Copyright (c) 2026 MacNativeGames contributors).
// [UIT 0076] Asynchronous preemption of virtual cores (TOTK_ASYNC_PREEMPT, src/host/threads.c): the Windows side.
// A thread waiting for a virtual core suspends the owner; if the owner's host rip is in the lifted game code and it
// is at host depth 0 (no host call on its stack, so it holds no host lock), its rip is moved to uit_ap_stub, which
// saves every register the game code may have live, gives the core up (threads.c uit_ap_yield) and jumps back.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#endif

#include "../runtime/cpu.h"
#include "ap_ranges.h"

#define GAME_TABLE(n) extern const GuestFn n##_fn_ptrs[]; extern const uint64_t n##_fn_count;
GAME_TABLE(main)
GAME_TABLE(subsdk0)
GAME_TABLE(sdk)

void uit_ap_stub(void);
void rt_dispatch(Cpu *c);
void rt_svc(Cpu *c, uint32_t imm, uint64_t pc);
void runtime_core_checkpoint(void);
void guest_call(Cpu *c, uint64_t addr);
uint64_t uit_ap_rip(void);
void uit_ap_yield(void);

static uintptr_t g_lo, g_hi;   // host code of the lifted modules: first and last function start
static ap_range *g_rng;        // [UIT 0257] TOTK_AP_EXACT_RANGES (default on): one range per lifted function
static size_t g_nrng;

#ifdef _WIN32
// why: a line for the log either way (what was checked, and why the mode cannot start if it cannot).
int uit_ap_init(char *why, size_t nwhy) {
    struct { const GuestFn *f; uint64_t n; } m[] = {{main_fn_ptrs, main_fn_count}, {subsdk0_fn_ptrs, subsdk0_fn_count},
                                                    {sdk_fn_ptrs, sdk_fn_count}};
    const uintptr_t host[] = {(uintptr_t)uit_ap_stub, (uintptr_t)rt_dispatch, (uintptr_t)rt_svc,
                              (uintptr_t)runtime_core_checkpoint, (uintptr_t)guest_call, (uintptr_t)uit_ap_yield,
                              (uintptr_t)uit_ap_rip, (uintptr_t)memcpy};
    static const char *const host_name[] = {"uit_ap_stub", "rt_dispatch", "rt_svc", "runtime_core_checkpoint",
                                            "guest_call", "uit_ap_yield", "uit_ap_rip", "memcpy"};
    const int nh = (int)(sizeof host / sizeof *host);
    uintptr_t lo = UINTPTR_MAX, hi = 0;
    uint64_t total = 0;
    for (unsigned i = 0; i < sizeof m / sizeof *m; i++)
        for (uint64_t k = 0; k < m[i].n; k++) {
            uintptr_t f = (uintptr_t)m[i].f[k];
            if (!f) continue;
            total++;
            if (f < lo) lo = f;
            if (f > hi) hi = f;
        }
    if (lo >= hi) { snprintf(why, nwhy, "no lifted functions found"); return 0; }
    int span = -1;   // the first host function between the first and the last lifted function start
    for (int i = 0; i < nh; i++) if (host[i] >= lo && host[i] <= hi) { span = i; break; }
    const char *e = getenv("TOTK_AP_EXACT_RANGES");
    if (e && (!strcmp(e, "0") || !strcmp(e, "off"))) {   // the old check exactly: one range, first to last start
        if (span >= 0) {
            snprintf(why, nwhy, "one range %p..%p (TOTK_AP_EXACT_RANGES=0) holds host code (%s at %p)", (void *)lo,
                     (void *)hi, host_name[span], (void *)host[span]);
            return 0;
        }
        g_lo = lo;
        g_hi = hi;
        snprintf(why, nwhy, "one range %p..%p (TOTK_AP_EXACT_RANGES=0), %llu functions", (void *)lo, (void *)hi,
                 (unsigned long long)total);
        return 1;
    }
    ap_range *r = (ap_range *)malloc((size_t)total * sizeof *r);
    if (!r) { snprintf(why, nwhy, "out of memory for %llu ranges", (unsigned long long)total); return 0; }
    size_t n = 0;
    uint64_t no_unwind = 0;
    for (unsigned i = 0; i < sizeof m / sizeof *m; i++)
        for (uint64_t k = 0; k < m[i].n; k++) {
            uintptr_t f = (uintptr_t)m[i].f[k];
            if (!f) continue;
            DWORD64 base = 0;
            PRUNTIME_FUNCTION rf = RtlLookupFunctionEntry((DWORD64)f, &base, NULL);
            // only a function's own entry, starting at it (a leaf function has none: it is never interrupted)
            if (!rf || base + rf->BeginAddress != (DWORD64)f || rf->EndAddress <= rf->BeginAddress) { no_unwind++; continue; }
            r[n].b = f;
            r[n].e = (uintptr_t)(base + rf->EndAddress);
            n++;
        }
    n = apr_build(r, n);
    int in = apr_first_inside(r, n, host, nh);
    if (!n || in >= 0) {
        if (in >= 0)
            snprintf(why, nwhy, "host code inside a lifted function's range (%s at %p)", host_name[in], (void *)host[in]);
        else
            snprintf(why, nwhy, "no lifted function has an unwind entry (%llu functions)", (unsigned long long)total);
        free(r);
        return 0;
    }
    g_rng = r;
    __atomic_store_n(&g_nrng, n, __ATOMIC_RELEASE);
    snprintf(why, nwhy, "%llu lifted functions in %zu exact ranges (%llu without an unwind entry, not interrupted); "
             "the old single range %p..%p %s", (unsigned long long)total, n, (unsigned long long)no_unwind, (void *)lo,
             (void *)hi, span >= 0 ? "holds host code (" : "holds no host code");
    if (span >= 0) {
        size_t l = strlen(why);
        snprintf(why + l, nwhy > l ? nwhy - l : 0, "%s), so it would have refused", host_name[span]);
    }
    return 1;
}

static int ap_in_game(uintptr_t rip) {
    if (g_rng) return apr_find(g_rng, g_nrng, rip);
    return rip >= g_lo && rip <= g_hi;
}

void *uit_ap_self_handle(void) {
    HANDLE h = NULL;
    if (!DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &h,
                         THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_SET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, 0))
        return NULL;
    return h;
}

void uit_ap_close(void *h) { if (h) CloseHandle(h); }

// Holding the scheduler lock. Returns 1 if the thread was redirected (*rip and *pending set before it runs again).
int uit_ap_redirect(void *h, const volatile int *depth, const volatile uint16_t *tl, uint64_t *rip, int *pending) {
    if (SuspendThread(h) == (DWORD)-1) return 0;
    CONTEXT ctx;
    memset(&ctx, 0, sizeof ctx);
    ctx.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;   // set back whole, so every GPR is restored
    int r = 0;
    if (GetThreadContext(h, &ctx) && ap_in_game((uintptr_t)ctx.Rip) && *depth <= 0 && !tl[0]) {
        *rip = ctx.Rip;
        *pending = 1;
        ctx.Rip = (DWORD64)(uintptr_t)uit_ap_stub;
        if (SetThreadContext(h, &ctx)) r = 1;
        else *pending = 0;
    }
    ResumeThread(h);
    return r;
}

// uit_ap_stub: entered in place of the interrupted instruction of top-level game code. It keeps every register the
// game code may have live (volatile GPRs, flags, YMM0-15, MXCSR; the C calls preserve the rest; the build targets
// x86-64-v3, so no ZMM or mask registers), gives the core up in uit_ap_yield and jumps back to uit_ap_rip(), stored
// first in the return slot it reserved at entry, so a stack walk sees the interrupted function as its caller.
// Windows x64 has no red zone, so the pushes below the interrupted rsp overwrite nothing live.
__asm__(".text\n"
        ".globl uit_ap_stub\n"
        ".def uit_ap_stub; .scl 2; .type 32; .endef\n"
        ".p2align 4\n"
        "uit_ap_stub:\n"
        ".seh_proc uit_ap_stub\n"
        "    leaq -8(%rsp), %rsp\n"   // not sub: the flags are still the interrupted code's until pushfq
        ".seh_stackalloc 8\n"
        "    pushfq\n"
        ".seh_stackalloc 8\n"
        "    pushq %rax\n"
        ".seh_pushreg %rax\n"
        "    pushq %rcx\n"
        ".seh_pushreg %rcx\n"
        "    pushq %rdx\n"
        ".seh_pushreg %rdx\n"
        "    pushq %r8\n"
        ".seh_pushreg %r8\n"
        "    pushq %r9\n"
        ".seh_pushreg %r9\n"
        "    pushq %r10\n"
        ".seh_pushreg %r10\n"
        "    pushq %r11\n"
        ".seh_pushreg %r11\n"
        "    pushq %rbp\n"
        ".seh_pushreg %rbp\n"
        "    movq %rsp, %rbp\n"
        ".seh_setframe %rbp, 0\n"
        ".seh_endprologue\n"
        "    andq $-32, %rsp\n"
        "    subq $576, %rsp\n"   // 0: shadow space, 32: MXCSR saved, 36: MXCSR default, 64: YMM0-15
        "    vmovdqa %ymm0, 64(%rsp)\n"
        "    vmovdqa %ymm1, 96(%rsp)\n"
        "    vmovdqa %ymm2, 128(%rsp)\n"
        "    vmovdqa %ymm3, 160(%rsp)\n"
        "    vmovdqa %ymm4, 192(%rsp)\n"
        "    vmovdqa %ymm5, 224(%rsp)\n"
        "    vmovdqa %ymm6, 256(%rsp)\n"
        "    vmovdqa %ymm7, 288(%rsp)\n"
        "    vmovdqa %ymm8, 320(%rsp)\n"
        "    vmovdqa %ymm9, 352(%rsp)\n"
        "    vmovdqa %ymm10, 384(%rsp)\n"
        "    vmovdqa %ymm11, 416(%rsp)\n"
        "    vmovdqa %ymm12, 448(%rsp)\n"
        "    vmovdqa %ymm13, 480(%rsp)\n"
        "    vmovdqa %ymm14, 512(%rsp)\n"
        "    vmovdqa %ymm15, 544(%rsp)\n"
        "    stmxcsr 32(%rsp)\n"
        "    movl $0x1f80, 36(%rsp)\n"
        "    ldmxcsr 36(%rsp)\n"
        "    cld\n"
        "    vzeroupper\n"
        "    call uit_ap_rip\n"
        "    movq %rax, 72(%rbp)\n"   // the return slot: rbp + 8 pushes + pushfq
        "    call uit_ap_yield\n"
        "    ldmxcsr 32(%rsp)\n"
        "    vmovdqa 64(%rsp), %ymm0\n"
        "    vmovdqa 96(%rsp), %ymm1\n"
        "    vmovdqa 128(%rsp), %ymm2\n"
        "    vmovdqa 160(%rsp), %ymm3\n"
        "    vmovdqa 192(%rsp), %ymm4\n"
        "    vmovdqa 224(%rsp), %ymm5\n"
        "    vmovdqa 256(%rsp), %ymm6\n"
        "    vmovdqa 288(%rsp), %ymm7\n"
        "    vmovdqa 320(%rsp), %ymm8\n"
        "    vmovdqa 352(%rsp), %ymm9\n"
        "    vmovdqa 384(%rsp), %ymm10\n"
        "    vmovdqa 416(%rsp), %ymm11\n"
        "    vmovdqa 448(%rsp), %ymm12\n"
        "    vmovdqa 480(%rsp), %ymm13\n"
        "    vmovdqa 512(%rsp), %ymm14\n"
        "    vmovdqa 544(%rsp), %ymm15\n"
        "    movq %rbp, %rsp\n"
        "    popq %rbp\n"
        "    popq %r11\n"
        "    popq %r10\n"
        "    popq %r9\n"
        "    popq %r8\n"
        "    popq %rdx\n"
        "    popq %rcx\n"
        "    popq %rax\n"
        "    popfq\n"
        "    leaq 8(%rsp), %rsp\n"
        "    jmpq *-8(%rsp)\n"
        ".seh_endproc\n");
#else
int uit_ap_init(char *why, size_t nwhy) {
    snprintf(why, nwhy, "not a Windows build");
    (void)g_rng; (void)g_nrng; (void)g_lo; (void)g_hi;
    return 0;
}
void *uit_ap_self_handle(void) { return NULL; }
void uit_ap_close(void *h) { (void)h; }
int uit_ap_redirect(void *h, const volatile int *depth, const volatile uint16_t *tl, uint64_t *rip, int *pending) {
    (void)h; (void)depth; (void)tl; (void)rip; (void)pending;
    return 0;
}
void uit_ap_stub(void) {}
#endif
