/* [UIT 0074] Box test of the fixed-point timer scale (src/runtime/platform.h plat_scale / plat_scaled) that replaced the
 * 128-bit division in plat_now_ns and rt_ticks: for counter rates from 1 MHz to 10 GHz (the 10 MHz every Windows 10+
 * PC reports, TSC rates, the old 3.579545 MHz ACPI timer) and counts up to 30 years of running time, the result is the
 * exact quotient or one below it, never above, and it never goes backwards.
 *   cc -O2 tools/tests/test_timer_scale.c -o /tmp/t/tscale && /tmp/t/tscale */
#include <stdint.h>
#include <stdio.h>

#include "../../src/runtime/platform/timer_scale.h"

static uint64_t rng = 0x9E3779B97F4A7C15ull;
static uint64_t next(void) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return rng; }

int main(void) {
    const uint64_t freqs[] = {1000000, 3579545, 10000000, 14318180, 19200000, 24000000, 1000000000, 2400000000ull,
                              2995200000ull, 3417600000ull, 10000000000ull};
    const uint64_t nums[] = {1000000000u, 19200000u};
    int fails = 0;
    uint64_t checked = 0;
    for (unsigned fi = 0; fi < sizeof freqs / sizeof *freqs; fi++)
        for (unsigned ni = 0; ni < 2; ni++) {
            uint64_t f = freqs[fi], num = nums[ni];
            PlatScale s = plat_scale(num, f);
            uint64_t span = f * 3600ull * 24 * 365 * 30;   /* 30 years of counts */
            if (span / f != 3600ull * 24 * 365 * 30) span = UINT64_MAX / (num / f + 1);
            uint64_t prev_t = 0, prev_v = 0;
            for (int i = 0; i < 2000000; i++) {
                uint64_t t = i < 1000 ? (uint64_t)i : i < 1000000 ? next() % span : prev_t + next() % (f / 1000 + 2);
                if (t > span) t = span;
                uint64_t want = (uint64_t)((unsigned __int128)t * num / f), got = plat_scaled(s, t);
                if (got > want || want - got > 1) {
                    if (fails++ < 10) printf("FAIL f=%llu num=%llu t=%llu: %llu, exact %llu\n", (unsigned long long)f,
                                             (unsigned long long)num, (unsigned long long)t, (unsigned long long)got, (unsigned long long)want);
                }
                if (i >= 1000000 && t >= prev_t && got < prev_v) {
                    if (fails++ < 10) printf("FAIL went backwards f=%llu t=%llu\n", (unsigned long long)f, (unsigned long long)t);
                }
                if (i >= 999999) prev_t = t, prev_v = got;
                checked++;
            }
        }
    printf(fails ? "timer scale: %d failures in %llu values\n" : "timer scale: all ok (%d failures in %llu values)\n", fails,
           (unsigned long long)checked);
    return fails != 0;
}
