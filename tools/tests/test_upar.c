// [UIT 0360] Box test for src/gfx/vkb_upar.h (TOTK_VK_UCAP_HELPERS): every part runs exactly once per job, jobs back to
// back never mix, and the split copy gives the same bytes as one memcpy per range.
// cc -O2 -pthread tools/tests/test_upar.c -o /tmp/tu && /tmp/tu
#include <stdio.h>
#include <stdlib.h>
#include "../../src/gfx/vkb_upar.h"
static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)
typedef struct { _Atomic int hits[64]; int job; _Atomic int wrong; } cnt;
static void count_part(void *ctx, int p, int parts) {
    cnt *c = ctx;
    (void)parts;
    atomic_fetch_add(&c->hits[p], 1);
    for (volatile int k = 0; k < (p * 37) % 500; k++) {}
}
typedef struct { uint64_t lo, hi, geo; } rng;   // the capture's layout (vkb_rsort.h ucap_rng)
static uint64_t seed = 88172645463325252ull;
static uint64_t rnd(void) { seed ^= seed << 13; seed ^= seed >> 7; seed ^= seed << 17; return seed; }
int main(void) {
    static upar P = UPAR_INIT;
    CHECK(upar_start(&P, 3) == 3);
    for (int j = 0; j < 20000; j++) {   // many short jobs: a helper late for one must not run parts of the next
        cnt c = {.job = j};
        int parts = 1 + (int)(rnd() % 64);
        upar_run(&P, count_part, &c, parts);
        int bad = 0;
        for (int i = 0; i < 64; i++) if (atomic_load(&c.hits[i]) != (i < parts)) bad++;
        if (bad) { CHECK(!bad); printf("  job %d parts %d\n", j, parts); break; }
    }
    size_t mem = 8 << 20;
    uint8_t *src = malloc(mem), *a = malloc(16 << 20), *b = malloc(16 << 20);
    for (size_t i = 0; i < mem; i++) src[i] = (uint8_t)rnd();
    for (int t = 0; t < 300; t++) {
        int n = 1 + (int)(rnd() % 3000);
        rng *r = malloc(sizeof *r * (size_t)n); size_t *off = malloc(sizeof *off * (size_t)n), total = 0;
        for (int i = 0; i < n; i++) {
            uint64_t len = t % 7 == 0 ? 1 + rnd() % (1 << 20) : 1 + rnd() % 4096;
            if (total + len > (16u << 20)) { n = i; break; }
            uint64_t at = rnd() % (mem - len);
            r[i] = (rng){(uint64_t)(uintptr_t)(src + at), (uint64_t)(uintptr_t)(src + at + len), 0};
            off[i] = total; total += len;
        }
        for (int i = 0; i < n; i++) memcpy(a + off[i], (void *)(uintptr_t)r[i].lo, (size_t)(r[i].hi - r[i].lo));
        memset(b, 0xa5, total);
        upar_copy c = {b, off, &r[0].lo, &r[0].hi, sizeof *r, n, total};
        int parts = 1 + (int)(rnd() % 8);
        upar_run(&P, upar_copy_part, &c, parts);
        if (memcmp(a, b, total)) { CHECK(!"split copy differs"); printf("  case %d: %d ranges, %zu bytes, %d parts\n", t, n, total, parts); break; }
        free(r); free(off);
    }
    free(src); free(a); free(b);
    upar_stop(&P);
    static upar Q = UPAR_INIT;   // no helpers: the caller runs every part
    cnt c = {0};
    upar_run(&Q, count_part, &c, 5);
    CHECK(atomic_load(&c.hits[0]) == 1 && atomic_load(&c.hits[4]) == 1 && atomic_load(&c.hits[5]) == 0);
    printf("%s\n", fails ? "FAILED" : "upar: all ok");
    return fails != 0;
}
