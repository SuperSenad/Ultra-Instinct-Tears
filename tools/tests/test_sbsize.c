// [UIT 0331] vkb_sbptr_size: the size bound for a storage buffer reached through a pointer outside c0.
#include <stdio.h>
#include "vkb_sbsize.h"
static int bad;
static void chk(uint64_t word, uint64_t left, uint64_t want, int want_fixed) {
    int f;
    uint64_t got = vkb_sbptr_size(word, left, VKB_SBPTR_CAP, VKB_SBPTR_FALLBACK, &f);
    if (got != want || f != want_fixed) { printf("word %llu left %llu: %llu fixed %d (want %llu %d)\n", (unsigned long long)word,
        (unsigned long long)left, (unsigned long long)got, f, (unsigned long long)want, want_fixed); bad = 1; }
}
int main(void) {
    const uint64_t M = 1ull << 20;
    chk(4096, 100 * M, 4096, 0);            // a plausible size: kept
    chk(64 * M, 100 * M, 64 * M, 0);        // as large as the cap: kept
    chk(0, 100 * M, M, 1);                  // 0 (an empty buffer before): the rest of the pool, at most 1 MiB
    chk(270545005, 400 * M, M, 1);          // the Depths' word above the cap
    chk(5000, 4096, 4096, 1);               // past the end of the pool: what the pool holds
    chk(0, 512, 512, 1);
    chk(123, 0, 123, 0);                    // not in a pool: unchanged
    if (!bad) printf("test_sbsize: ok\n");
    return bad;
}
