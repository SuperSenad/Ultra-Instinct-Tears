// [UIT 0331, UIT 0333] vkb_sbptr_size: the size bound for a storage buffer reached through a pointer outside c0.
#include <stdio.h>
#include "vkb_sbsize.h"
static int bad;
static void chk(uint64_t word, uint64_t left, uint64_t limit, uint64_t want, int want_fixed) {
    int f;
    uint64_t got = vkb_sbptr_size(word, left, VKB_SBPTR_FALLBACK, limit, &f);
    if (got != want || f != want_fixed) { printf("word %llu left %llu limit %llu: %llu fixed %d (want %llu %d)\n",
        (unsigned long long)word, (unsigned long long)left, (unsigned long long)limit, (unsigned long long)got, f,
        (unsigned long long)want, want_fixed); bad = 1; }
}
int main(void) {
    const uint64_t M = 1ull << 20, G = 1ull << 30;
    chk(4096, 100 * M, 0, 4096, 0);                     // a plausible size: kept
    chk(64 * M, 100 * M, 0, 64 * M, 0);
    chk(270560515, 400 * M, 0, 270560515, 0);           // the Depths' ground data (~258 MiB): kept, not cut to 1 MiB
    chk(270560515, 400 * M, 4 * G - 1, 270560515, 0);
    chk(270560515, 200 * M, 0, 200 * M, 1);             // past the end of the pool: what the pool holds
    chk(0, 100 * M, 0, M, 1);                           // 0 (an empty buffer before): the rest of the pool, at most 1 MiB
    chk(0, 512, 0, 512, 1);
    chk(5000, 4096, 0, 4096, 1);
    chk(123, 0, 0, 123, 0);                             // not in a pool: unchanged
    chk(3 * G, 4 * G, 2 * G, 2 * G, 1);                 // above the device's storage buffer range: the range
    chk(3 * G, 0, 2 * G, 2 * G, 1);
    if (!bad) printf("test_sbsize: ok\n");
    return bad;
}
