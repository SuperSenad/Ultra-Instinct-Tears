// [UIT 0030] Box test: more than 512 pages written into one pool between two scans (streaming). The mock below is
// GetWriteWatch(WRITE_WATCH_FLAG_RESET) as src/runtime/platform/windows.c plat_written_pages uses it: when the page
// list fills, the whole span's watch bits are reset (ResetWriteWatch). Every written page must still be reported
// (nvn_dirty_since true) - a page not reported keeps an old device copy (TOTK_VK_SHADOW_VERIFY's stale pages) and an
// old texture.
// cc -O2 -pthread tools/tests/test_dirty_bigscan.c src/gfx/nvn/nvn_dirty_win.c -o /tmp/t && /tmp/t          (new: 0 lost)
// cc -O2 -pthread -DNVN_DIRTY_OLD_SCAN tools/tests/test_dirty_bigscan.c src/gfx/nvn/nvn_dirty_win.c -o /tmp/t && /tmp/t
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../src/gfx/nvn/nvn_dirty.h"

#define NP 16384   // a 64 MiB pool
static uint8_t *g_mem;
static uint8_t g_bit[NP];

int plat_written_pages(uint64_t addr, uint64_t size, uint64_t *pages, int max, uint64_t *page) {
    *page = 4096;
    uintptr_t b = (uintptr_t)g_mem;
    if (addr < b || addr + size > b + (uint64_t)NP * 4096) return 0;   // (the probe page: watched, unwritten)
    int n = 0;
    uint64_t p0 = (addr - b) / 4096, p1 = (addr + size - b + 4095) / 4096;
    for (uint64_t p = p0; p < p1 && n < max; p++) if (g_bit[p]) pages[n++] = b + p * 4096;
    for (uint64_t p = p0; p < p1; p++) {   // reset: the reported pages, and the whole span once the list is full
        if (n >= max) g_bit[p] = 0;
    }
    for (int i = 0; i < n; i++) g_bit[(pages[i] - b) / 4096] = 0;
    return n;
}
static void w(size_t p) { g_mem[p * 4096] ^= 1; g_bit[p] = 1; }

int main(void) {
    g_mem = aligned_alloc(4096, (size_t)NP * 4096);
    memset(g_mem, 0, (size_t)NP * 4096);
    nvn_dirty_add_range(g_mem, (size_t)NP * 4096);
    int fails = 0;
    size_t counts[] = {1, 511, 512, 513, 2048, 9236, NP};
    for (size_t c = 0; c < sizeof counts / sizeof *counts; c++) {
        uint64_t stamp = nvn_dirty_now();
        size_t first = (c * 977) % 64;
        size_t n = counts[c] < NP - first ? counts[c] : NP - first;
        for (size_t i = 0; i < n; i++) w(first + i);
        nvn_dirty_since(g_mem, (size_t)NP * 4096, stamp);   // a scan of the whole pool (a pool copy, an epoch's first look)
        size_t lost = 0;
        for (size_t i = 0; i < n; i++) lost += !nvn_dirty_since(g_mem + (first + i) * 4096, 4096, stamp);
        printf("%6zu pages written: %zu not reported\n", n, lost);
        fails += lost != 0;
    }
    printf(fails ? "FAIL\n" : "ok\n");
    return fails != 0;
}
