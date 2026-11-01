// [UIT 0088] Box test of src/runtime/dispatch_flat.h: on a random module of 300000 functions, every lookup (function
// starts, addresses inside functions, misaligned, outside) gives what dispatch_lookup's binary search gives, first
// and later (filled slots), also with 8 threads looking up and filling at once.
//   clang -O2 -march=x86_64_v3 -pthread -Isrc/runtime tools/tests/test_dispatch_flat.c -o /tmp/df && /tmp/df
#include <pthread.h>
#include <stdio.h>
#include <time.h>
#include "dispatch_flat.h"

#define NF 300000
static uint64_t addrs[NF];
static FlatMap fm;
static int64_t ref(uint64_t a) {
    uint64_t lo = 0, hi = NF;
    while (lo < hi) { uint64_t mid = (lo + hi) / 2; if (addrs[mid] < a) lo = mid + 1; else hi = mid; }
    return lo < NF && addrs[lo] == a ? (int64_t)lo : -1;
}
static uint64_t rnd(uint64_t *s) { *s ^= *s << 13; *s ^= *s >> 7; *s ^= *s << 17; return *s; }
static uint64_t pick(uint64_t *s) {
    uint64_t r = rnd(s), i = r % NF;
    switch ((r >> 32) % 6) {
    case 0: case 1: case 2: return addrs[i];
    case 3: return addrs[i] + 4 * (1 + (r >> 40) % 8);              // inside a function
    case 4: return addrs[i] + 1 + (r >> 40) % 3;                    // misaligned
    default: return (r >> 40) & 1 ? addrs[0] - 4 * (1 + (r >> 41) % 100) : addrs[NF - 1] + 4 * (1 + (r >> 41) % 100);
    }
}
static long bad;
static void *worker(void *p) {
    uint64_t s = 0x9E3779B97F4A7C15ull ^ (uintptr_t)p;
    for (int k = 0; k < 2000000; k++) {
        uint64_t a = pick(&s);
        if (flat_find(&fm, addrs, NF, a) != ref(a)) __atomic_add_fetch(&bad, 1, __ATOMIC_RELAXED);
    }
    return NULL;
}
static double secs(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }
int main(void) {
    uint64_t s = 12345, a = 0x800000000ull;
    for (int i = 0; i < NF; i++) { a += 4 * (1 + rnd(&s) % 64); addrs[i] = a; }
    int ok = flat_init(&fm, addrs[0], addrs[NF - 1], NF);
    printf("%s table: %llu slots\n", ok ? "ok  " : "FAIL", (unsigned long long)fm.n);
    pthread_t th[8];
    for (int i = 0; i < 8; i++) pthread_create(&th[i], NULL, worker, (void *)(intptr_t)(i + 1));
    for (int i = 0; i < 8; i++) pthread_join(th[i], NULL);
    printf("%s 8 threads x 2000000 lookups (starts, inside, misaligned, outside): %ld differ from the binary search\n", bad ? "FAIL" : "ok  ", bad);
    ok &= !bad;
    long filled = 0, wrong = 0;
    for (uint64_t i = 0; i < fm.n; i++) if (fm.slot[i]) { filled++; wrong += addrs[fm.slot[i] - 1] != fm.lo + 4 * i; }
    printf("%s %ld slots filled, %ld point at another address\n", wrong ? "FAIL" : "ok  ", filled, wrong);
    ok &= !wrong;
    FlatMap none = {0}; int64_t r1 = flat_find(&none, addrs, NF, addrs[77]), r2 = flat_find(&none, addrs, NF, addrs[77] + 4);
    printf("%s without a table (setting off): the binary search (%lld, %lld)\n", r1 == 77 && r2 == -1 ? "ok  " : "FAIL", (long long)r1, (long long)r2);
    ok &= r1 == 77 && r2 == -1;
    // speed: random function starts, after filling (in the game: dispatch-cache misses)
    volatile int64_t sink = 0; uint64_t q = 99; double t0 = secs();
    for (int k = 0; k < 5000000; k++) sink += ref(addrs[rnd(&q) % NF]);
    double t1 = secs(); q = 99;
    for (int k = 0; k < 5000000; k++) sink += flat_find(&fm, addrs, NF, addrs[rnd(&q) % NF]);
    double t2 = secs();
    printf("     lookup of a random function: binary search %.0f ns, flat table %.0f ns\n", (t1 - t0) / 5e6 * 1e9, (t2 - t1) / 5e6 * 1e9);
    puts(ok ? "PASS" : "FAIL");
    return !ok;
}
