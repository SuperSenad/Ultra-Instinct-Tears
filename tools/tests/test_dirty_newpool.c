// [UIT 0201] Box test: a pool freed and a new one made over the same address. An image kept from the first pool (set
// aside, or found by memory and shape) is checked with its upload stamp; the game's writes there while no pool tracked
// the address (and the ones the new pool's add scan sees) must make it upload again. Images uploaded over the new pool
// and pool copies made after it must not see a change. The mock is GetWriteWatch(WRITE_WATCH_FLAG_RESET) as in
// test_dirty_bigscan.c.
//   cc -O2 -pthread tools/tests/test_dirty_newpool.c src/gfx/nvn/nvn_dirty_win.c -o /tmp/t && /tmp/t && TOTK_DIRTY_NEW_POOL=0 /tmp/t old
// "old" (TOTK_DIRTY_NEW_POOL=0): the stale image is expected (the bug, kept as the A/B), the rest as above.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../src/gfx/nvn/nvn_dirty.h"

#define NP 64
static uint8_t *g_mem;
static uint8_t g_bit[NP];
int plat_written_pages(uint64_t addr, uint64_t size, uint64_t *pages, int max, uint64_t *page) {
    *page = 4096;
    uintptr_t b = (uintptr_t)g_mem;
    if (addr < b || addr + size > b + (uint64_t)NP * 4096) return 0;
    int n = 0;
    uint64_t p0 = (addr - b) / 4096, p1 = (addr + size - b + 4095) / 4096;
    for (uint64_t p = p0; p < p1 && n < max; p++) if (g_bit[p]) pages[n++] = b + p * 4096;
    for (int i = 0; i < n; i++) g_bit[(pages[i] - b) / 4096] = 0;
    return n;
}
static void w(size_t p) { g_mem[p * 4096] ^= 1; g_bit[p] = 1; }   // the OS marks a written page
static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)
static uint64_t upload(void *p, size_t n) { nvn_dirty_protect(p, n); return nvn_dirty_now(); }   // as vkb_tex: protect, then the stamp

static int run(int old) {
    size_t sz = (size_t)NP * 4096;
    uint8_t *tex = g_mem + 8 * 4096; size_t tsz = 4 * 4096;
    // 1. pool A, a texture uploaded over it
    nvn_dirty_add_range(g_mem, sz);
    uint64_t s1 = upload(tex, tsz);
    CHECK(!nvn_dirty_since(tex, tsz, s1), "unchanged right after the upload");
    // 2. pool A freed (the image is set aside); the game writes new texels there before the next pool exists
    nvn_dirty_remove_range(g_mem);
    for (int p = 8; p < 12; p++) w(p);
    // 3. pool B over the same memory; the image is taken back by memory and shape
    nvn_dirty_add_range(g_mem, sz);
    int stale = !nvn_dirty_since(tex, tsz, s1);
    if (old) CHECK(stale, "old mode: the earlier writes are forgotten (the bug kept as the A/B)");
    else CHECK(!stale, "the kept image must upload again (it would draw pool A's texels)");
    // 4. after uploading again: unchanged until the next write
    uint64_t s2 = upload(tex, tsz);
    CHECK(!nvn_dirty_since(tex, tsz, s2), "unchanged after its new upload");
    w(9);
    CHECK(nvn_dirty_since(tex, tsz, s2), "a later write is seen");
    // 5. a texture first uploaded over pool C: no extra upload; a pool copy (first fill: scan, then the stamp)
    nvn_dirty_remove_range(g_mem);
    nvn_dirty_add_range(g_mem, sz);
    uint64_t s3 = upload(g_mem + 20 * 4096, 4096);
    CHECK(!nvn_dirty_since(g_mem + 20 * 4096, 4096, s3), "a texture uploaded over the new pool is unchanged");
    nvn_dirty_since(g_mem, sz, UINT64_MAX);
    uint64_t pc = nvn_dirty_last_stamp();
    CHECK(!nvn_dirty_since(g_mem, sz, pc), "a pool copy made after the add sees no change");
    w(30);
    CHECK(nvn_dirty_since(g_mem, sz, pc) && !nvn_dirty_since(g_mem + 20 * 4096, 4096, s3), "only the written page changes");
    // 6. another range elsewhere is not touched by an add
    nvn_dirty_remove_range(g_mem);
    return 0;
}

int main(int argc, char **argv) {
    g_mem = aligned_alloc(4096, (size_t)NP * 4096);
    memset(g_mem, 0, (size_t)NP * 4096);
    run(argc > 1 && !strcmp(argv[1], "old"));
    printf(fails ? "%d FAIL\n" : "ok\n", fails);
    return fails != 0;
}
