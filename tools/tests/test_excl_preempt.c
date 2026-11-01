// [UIT 0122] Box test: a store-exclusive after the thread lost its core must fail, as on the Switch.
// src/runtime/cpu.h emulates LDXR/STXR with a compare-and-swap against the loaded value. When TOTK_ASYNC_PREEMPT
// interrupts the thread between the two (src/host/preempt_win.c), src/host/threads.c core_put clears the monitor
// (Cpu.excl_addr = ~0). Here a timer signal plays that interruption: in the window it does an A -> B -> A change of
// the word (what other threads may do while the core is away) and clears the monitor. The store must then fail; with
// no clear (the runtime before 0122; "noclear" control run) the store succeeds and publishes a stale value (ABA).
// Build: clang -O2 -march=x86-64-v3 -I src/runtime tools/tests/test_excl_preempt.c (tools/tests/run_excl_preempt.sh)
#define _GNU_SOURCE
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include "cpu.h"

static Cpu C;
static uint64_t g_word = 0x1234;
static volatile int g_in_win, g_noclear;
static volatile int g_cleared, g_ticks;   // (int: a long could alias excl_addr and hide the bug)

static void on_sig(int s) {
    (void)s;
    g_ticks++;
    if (!g_in_win) return;
    uint64_t v = __atomic_load_n(&g_word, __ATOMIC_RELAXED);
    __atomic_store_n(&g_word, v ^ 1, __ATOMIC_RELAXED);   // another thread pops ...
    __atomic_store_n(&g_word, v, __ATOMIC_RELAXED);       // ... and pushes the same node back
    if (!g_noclear) *(volatile uint64_t *)&C.excl_addr = ~0ull;   // the core switch (threads.c excl_clear)
    g_cleared++;
}

__attribute__((noinline)) static int one(Cpu *c, uint64_t a, unsigned spin, int *cleared) {
    int before = g_cleared;
    uint64_t v = ldx64(c, a);
    g_in_win = 1;
    uint64_t x = v;
    for (unsigned i = 0; i < spin; i++) __asm__ volatile("" : "+r"(x));   // registers only: no memory clobber
    g_in_win = 0;
    *cleared = g_cleared != before;
    return (int)stx64(c, a, x + 1);
}

int main(int argc, char **argv) {
    // "noclear": the runtime before 0122 (no clear on a core switch); the ABA must then go through
    g_noclear = argc > 1 && !strcmp(argv[1], "noclear");
    struct sigaction sa; memset(&sa, 0, sizeof sa); sa.sa_handler = on_sig; sigaction(SIGALRM, &sa, NULL);
    struct itimerval it = {{0, 200}, {0, 200}}; setitimer(ITIMER_REAL, &it, NULL);
    long wins = 0, bad = 0, ok = 0;
    for (long k = 0; k < 2000000 && wins < 2000; k++) {
        int cleared, r = one(&C, (uint64_t)(uintptr_t)&g_word, 4000, &cleared);
        if (cleared) { wins++; if (r == 0) bad++; }
        else if (r == 0) ok++;
    }
    struct itimerval off = {{0, 0}, {0, 0}}; setitimer(ITIMER_REAL, &off, NULL);
    printf("excl_preempt: %ld interrupted windows, %ld stores that wrongly succeeded, %ld clean stores, %ld ticks\n",
           wins, bad, ok, (long)g_ticks);
    if (wins < 50 || ok < 50) { printf("excl_preempt: not enough samples\n"); return 2; }
    if (g_noclear) return bad ? 0 : 1;
    return bad ? 1 : 0;
}
