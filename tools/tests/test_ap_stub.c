// [UIT 0076] Box test of the asynchronous-preemption stub body (src/host/preempt_win.c uit_ap_stub) on Linux: a
// signal handler plays the waiter's SetThreadContext (moves rip to the stub), the stub calls uit_ap_yield, which
// clobbers every volatile register, and the interrupted loop's result must equal an uninterrupted run. The stub text
// is taken from preempt_win.c with the Windows-only .def/.seh lines removed (tools/tests/run_ap_stub.sh).
// Build: cc -O2 -march=x86-64-v3 -mno-red-zone -pthread test_ap_stub.c stub.s
#define _GNU_SOURCE
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <ucontext.h>
#include <unistd.h>
#include <immintrin.h>

void uit_ap_stub(void);
static __thread uint64_t t_rip;
static volatile long g_yields;
uint64_t uit_ap_rip(void) { return t_rip; }
__attribute__((noinline)) void uit_ap_yield(void) {
    g_yields++;
    // clobber volatile state the way C code may
    volatile __m256 a = _mm256_set1_ps(1234.5f), b = _mm256_set1_ps(-7.f);
    __m256 c = _mm256_mul_ps(a, b); (void)c;
    __asm__ volatile("xor %%eax,%%eax; xor %%ecx,%%ecx; xor %%edx,%%edx; xor %%r8d,%%r8d; xor %%r9d,%%r9d;"
                     "xor %%r10d,%%r10d; xor %%r11d,%%r11d; vpxor %%ymm0,%%ymm0,%%ymm0; vpxor %%ymm5,%%ymm5,%%ymm5;"
                     "vpxor %%ymm15,%%ymm15,%%ymm15; cmp %%eax,%%eax" ::: "rax","rcx","rdx","r8","r9","r10","r11",
                     "xmm0","xmm5","xmm15","cc","memory");
    _mm_setcsr(0x1f80 | 0x6000);   // rounding toward zero: the stub must restore the interrupted MXCSR
    sched_yield();
}

static void on_sig(int s, siginfo_t *i, void *uc_) {
    (void)s; (void)i;
    ucontext_t *uc = uc_;
    t_rip = uc->uc_mcontext.gregs[REG_RIP];
    uc->uc_mcontext.gregs[REG_RIP] = (greg_t)(uintptr_t)uit_ap_stub;
}

__attribute__((noinline)) static double work(uint64_t n, uint64_t *ih) {
    double d = 0.5, e = 1.25; float f = 3.f; uint64_t h = 1469598103934665603ull, g = 7;
    __m256d v = _mm256_set1_pd(1.0);
    for (uint64_t k = 0; k < n; k++) {
        h = (h ^ k) * 1099511628211ull; g = g * 6364136223846793005ull + (h >> 7);
        d = d * 1.0000001 + (double)(k & 7) * 0.001; e = e * 0.9999999 + d * 1e-9; f = f * 1.0000001f + 0.5f;
        v = _mm256_fmadd_pd(v, _mm256_set1_pd(1.0000000001), _mm256_set1_pd((double)(g & 3)));
        if ((k & 63) == 0) d = d + (h > g ? 1e-12 : -1e-12);
    }
    double s[4]; _mm256_storeu_pd(s, v);
    *ih = h ^ g;
    return d + e + f + s[0] + s[1] + s[2] + s[3];
}

static pthread_t g_worker;
static volatile int g_done;
static double g_res; static uint64_t g_ih;
static void *worker(void *a) { (void)a; g_res = work(200000000, &g_ih); g_done = 1; return NULL; }

int main(void) {
    uint64_t ih0; double r0 = work(200000000, &ih0);
    struct sigaction sa; memset(&sa, 0, sizeof sa); sa.sa_sigaction = on_sig; sa.sa_flags = SA_SIGINFO | SA_RESTART;
    sigaction(SIGUSR1, &sa, NULL);
    pthread_create(&g_worker, NULL, worker, NULL);
    while (!g_done) { usleep(300); if (!g_done) pthread_kill(g_worker, SIGUSR1); }
    pthread_join(g_worker, NULL);
    int ok = r0 == g_res && ih0 == g_ih && g_yields > 100;
    printf("%s: %ld interruptions, result %.17g / %.17g, ints %llx / %llx\n", ok ? "PASS" : "FAIL", g_yields, r0, g_res,
           (unsigned long long)ih0, (unsigned long long)g_ih);
    return !ok;
}
