// [UIT 0238] uit_crit.h uit_cp_own_split: the frame thread's own work split into virtual-core waits, the game's
// blocking waits, sleeps, polling yields and runnable time (CPU time and ready-but-not-running).
// cc -O1 -Wall -Werror -Isrc/gfx tools/tests/test_cp_split.c -o /tmp/t/cps && /tmp/t/cps
#include <string.h>
#include "uit_crit.h"
static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)
int main(void) {
    char b[512];
    uit_cp_own_split(15.13, 1.20, 2.00, 0.0, 0.93, 9.0, b, sizeof b);   // runnable 11.00, CPU 9.00: ready >= 2.00 (> 15%)
    CHECK(strstr(b, "own work 15.13 = waiting for a virtual core 1.20 + the game's blocking waits 2.00 + sleeps 0.00 + polling yields 0.93 + runnable 11.00"));
    CHECK(strstr(b, "its CPU time 9.00; ready but not running at least 2.00: other threads take its CPU"));
    uit_cp_own_split(10, 0, 0, 0, 0, 9.5, b, sizeof b);                   // 0.5 of 10: not flagged
    CHECK(strstr(b, "at least 0.50)") && !strstr(b, "other threads"));
    uit_cp_own_split(10, 0, 0, 0, 0, 12, b, sizeof b);                    // spins counted as CPU: ready 0, not negative
    CHECK(strstr(b, "at least 0.00)"));
    uit_cp_own_split(5, 3, 3, 0, 0, -1, b, sizeof b);                     // waits over own (clock skew): runnable 0; no CPU
    CHECK(strstr(b, "runnable 0.00") && !strstr(b, "CPU time"));
    char s[40];                                                           // a short buffer: cut, never overrun
    uit_cp_own_split(15.13, 1.2, 2, 0, 0.93, 9, s, sizeof s);
    CHECK(strlen(s) == sizeof s - 1);
    printf("%s\n", fails ? "FAILED" : "cp split: all ok");
    return fails != 0;
}
