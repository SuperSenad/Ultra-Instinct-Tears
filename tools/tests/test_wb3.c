// [UIT 0035] Box test for vkb_wb3.h (TOTK_VK_WB_MERGE3): the three-way merge of a page both the CPU and the GPU wrote.
// cc -O2 -Wall -Isrc/gfx tools/tests/test_wb3.c -o /tmp/t/wb3 && /tmp/t/wb3
#include <stdio.h>
#include <stdlib.h>
#include "vkb_wb3.h"
static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)
typedef struct { uint8_t *mem; const uint8_t *gpu; } ctx_t;
static void put(void *c, size_t off, size_t len) { ctx_t *x = c; memcpy(x->mem + off, x->gpu + off, len); }
int main(void) {
    enum { P = 4096 };
    static uint8_t base[P], mem[P], gpu[P], want[P];
    srand(7);
    uint64_t rec = 0, lost_before = 0, rounds = 20000, bad = 0, lost_after = 0;
    for (uint64_t it = 0; it < rounds; it++) {
        for (int i = 0; i < P; i++) base[i] = (uint8_t)rand();
        memcpy(mem, base, P); memcpy(gpu, base, P);
        // GPU and CPU each write some 4-byte-aligned spans with fresh values (never equal to what was there), disjoint
        // or overlapping; where both wrote, the CPU's is the right result (written after)
        uint8_t who[P / 4] = {0};   // 1 GPU, 2 CPU, 3 both
        for (int k = 0; k < 6; k++) {
            int a = rand() % (P / 4), l = 1 + rand() % 64;
            for (int w = a; w < a + l && w < P / 4; w++) { for (int b = 0; b < 4; b++) gpu[4 * w + b] = (uint8_t)(base[4 * w + b] + 1 + rand() % 254); who[w] |= 1; }
        }
        for (int k = 0; k < 6; k++) {
            int a = rand() % (P / 4), l = 1 + rand() % 64;
            for (int w = a; w < a + l && w < P / 4; w++) { for (int b = 0; b < 4; b++) mem[4 * w + b] = (uint8_t)(base[4 * w + b] + 1 + rand() % 254); who[w] |= 2; }
        }
        for (int w = 0; w < P / 4; w++) for (int b = 0; b < 4; b++) want[4 * w + b] = who[w] & 2 ? mem[4 * w + b] : gpu[4 * w + b];
        for (int i = 0; i < P; i++) lost_before += mem[i] != want[i];   // the old rule: the CPU's page whole
        ctx_t c = {mem, gpu};
        rec += wb3_merge(mem, gpu, base, P, put, &c);
        for (int i = 0; i < P; i++) { bad += mem[i] != want[i]; lost_after += mem[i] != want[i]; }
    }
    CHECK(bad == 0);
    // short tail, nothing written, CPU-only
    uint8_t b2[7] = {1, 2, 3, 4, 5, 6, 7}, m2[7], g2[7];
    memcpy(m2, b2, 7); memcpy(g2, b2, 7); g2[6] = 9;
    ctx_t c2 = {m2, g2};
    CHECK(wb3_merge(m2, g2, b2, 7, put, &c2) == 3 && m2[6] == 9);
    memcpy(m2, b2, 7); memcpy(g2, b2, 7); m2[0] = 0;
    CHECK(wb3_merge(m2, g2, b2, 7, put, &c2) == 0 && m2[0] == 0);
    // the documented wrong case: the CPU rewrote a word with its old value after the GPU's write
    memcpy(m2, b2, 7); memcpy(g2, b2, 7); g2[1] = 99;   // CPU "wrote" b2 again
    CHECK(wb3_merge(m2, g2, b2, 7, put, &c2) == 4 && m2[1] == 99);
    printf("wb3: %llu pages: GPU bytes recovered %llu; bytes wrong with the old rule %llu, with the merge %llu\n",
           (unsigned long long)rounds, (unsigned long long)rec, (unsigned long long)lost_before, (unsigned long long)lost_after);
    printf("%s\n", fails ? "FAILED" : "wb3: all ok");
    return fails != 0;
}
