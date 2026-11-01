// [UIT 0029] Box test: a pool's device copy brought up to date while GPU writes of earlier submissions are not in game
// memory yet. A model of backend_vk.c's protocol on the real write watch bookkeeping (nvn_dirty_win.c):
//   record(i): sync_gather - CPU-written pages since the copy's stamp (nvn_dirty_runs_stamp, gap vkb_shadow_gap) are
//              planned as copies game memory -> copy; the submission's GPU writes are planned, with the stamp (wb_add)
//   exec(i):   on the GPU: the copies in (they read game memory as the GPU runs them), then the GPU writes into the
//              copy, then the written ranges into the slot's host memory (shadow_writeback)
//   retire(i): wb_merge - pages not CPU-written since the stamp into game memory (wb_put -> nvn_dirty_gpu_write)
// One iteration: CPU writes, record(i), exec(i-1), retire(i-2): the copies in of a submission run on the GPU right after
// the one before it completes, before the retire thread has merged that one's writes (as on the GPU).
// Afterwards (all retired, a last sync) the copy must equal game memory on every page. With gap 64 KiB while writes
// are pending (argument "old", the 0028 behaviour) the pages between CPU-written ones went back in the copy.
// cc -O2 -Isrc/gfx tools/tests/test_shadow_pending.c src/gfx/nvn/nvn_dirty_win.c -o /tmp/t && /tmp/t && /tmp/t old
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../src/gfx/nvn/nvn_dirty.h"
#include "../../src/gfx/vkb_shadow_gap.h"

#define NP 2048
#define PG 4096
static uint8_t *g_mem, *g_sh;
static uint8_t g_bit[NP];   // the OS's write-watch bits
int plat_written_pages(uint64_t addr, uint64_t size, uint64_t *pages, int max, uint64_t *page) {
    *page = PG;
    uintptr_t b = (uintptr_t)g_mem;
    if (addr < b || addr >= b + (uint64_t)NP * PG) return 0;
    int n = 0;
    for (uint64_t a = addr; a < addr + size && n < max; a += PG) {
        uint64_t p = (a - b) / PG;
        if (g_bit[p]) { g_bit[p] = 0; pages[n++] = a; }
    }
    return n;
}
static uint64_t rnd(uint64_t *s) { *s ^= *s << 13; *s ^= *s >> 7; *s ^= *s << 17; return *s; }

#define D 4   // submissions in flight (slots)
typedef struct { int ncp; uint32_t cp[NP][2]; int ngw; uint32_t gw[64]; uint64_t stamp; uint8_t st[64][PG / 2]; } sub_t;
static sub_t g_sub[D];
static uint64_t g_stamp, g_wb_sub, g_retired, g_old, g_copied_pages, g_kept;
static sub_t *g_cur;
static void plan(void *ctx, size_t off, size_t n) {
    (void)ctx;
    g_cur->cp[g_cur->ncp][0] = (uint32_t)(off / PG); g_cur->cp[g_cur->ncp][1] = (uint32_t)((n + PG - 1) / PG); g_cur->ncp++;
    g_copied_pages += (n + PG - 1) / PG;
}
static void record(uint64_t i, uint64_t *s, int gpu) {
    sub_t *u = g_cur = &g_sub[i % D];
    u->ncp = u->ngw = 0;
    int pending = g_wb_sub && g_wb_sub > g_retired;
    nvn_dirty_runs_stamp(g_mem, (size_t)NP * PG, g_stamp, vkb_shadow_gap(pending, (int)g_old), plan, NULL, &g_stamp);
    u->stamp = g_stamp;
    if (!gpu) return;
    for (int k = 0; k < 8; k++) u->gw[u->ngw++] = (uint32_t)(rnd(s) % NP);   // GPU writes the upper half of 8 pages
    g_wb_sub = i;
}
static void exec(uint64_t i) {
    sub_t *u = &g_sub[i % D];
    for (int k = 0; k < u->ncp; k++) memcpy(g_sh + (size_t)u->cp[k][0] * PG, g_mem + (size_t)u->cp[k][0] * PG, (size_t)u->cp[k][1] * PG);
    for (int k = 0; k < u->ngw; k++) {
        uint8_t *d = g_sh + (size_t)u->gw[k] * PG + PG / 2;
        memset(d, (int)(i * 7 + (uint64_t)k + 1) & 255, PG / 2);
        memcpy(u->st[k], d, PG / 2);
    }
}
static void retire(uint64_t i) {
    sub_t *u = &g_sub[i % D];
    for (int k = 0; k < u->ngw; k++) {
        uint8_t *d = g_mem + (size_t)u->gw[k] * PG;
        if (nvn_dirty_since(d, PG, u->stamp)) { g_kept++; continue; }   // left to the CPU's write
        nvn_dirty_gpu_write(d + PG / 2, u->st[k], PG / 2);
    }
    g_retired = i;
}
static void cpu_write(uint64_t p, uint64_t v) {
    g_bit[p] = 1;
    memcpy(g_mem + p * PG, &v, 8);   // the lower half: the CPU's
}
int main(int argc, char **argv) {
    g_old = argc > 1 && !strcmp(argv[1], "old");
    if (posix_memalign((void **)&g_mem, PG, (size_t)NP * PG) || !(g_sh = malloc((size_t)NP * PG))) return 2;
    memset(g_mem, 0, (size_t)NP * PG);
    nvn_dirty_add_range(g_mem, (size_t)NP * PG);
    nvn_dirty_since(g_mem, (size_t)NP * PG, UINT64_MAX);   // the first fill: the whole pool
    g_stamp = nvn_dirty_last_stamp();
    memcpy(g_sh, g_mem, (size_t)NP * PG);
    uint64_t s = 0x9E3779B97F4A7C15ull, i = 1, phases = 0;
    long bad = 0, cpu_bad = 0;
    for (int ph = 0; ph < 400; ph++, phases++) {   // a phase: 50 frames of streaming, then nothing runs and the copy is checked
        uint64_t i0 = i;
        for (; i < i0 + 50; i++) {
            for (int k = 0; k < 6; k++) cpu_write(rnd(&s) % NP, i);   // the game writes a few pages a frame
            record(i, &s, 1);
            if (i >= 2) exec(i - 1);
            if (i >= 3) retire(i - 2);
        }
        for (int k = 0; k < 3; k++, i++) {   // drain: no more writes, no more GPU work
            record(i, &s, 0);
            exec(i - 1);
            retire(i - 2);
        }
        record(i, &s, 0); exec(i - 1); exec(i); retire(i - 1); retire(i); i++;   // a last sync with everything retired
        int b = 0;
        for (int p = 0; p < NP; p++) {
            if (memcmp(g_sh + (size_t)p * PG, g_mem + (size_t)p * PG, PG)) b++;
            if (memcmp(g_sh + (size_t)p * PG, g_mem + (size_t)p * PG, 8)) cpu_bad++;
        }
        bad += b;
        if (b) memcpy(g_sh, g_mem, (size_t)NP * PG);   // the next phase from a good copy
    }
    uint64_t N = i;
    printf("%s (%s): %ld stale pages in the device copy (%ld in the CPU's bytes) over %llu checks of %d pages, %llu submissions; "
           "pages copied in %llu, GPU pages left to CPU writes %llu, CPU writes caught while copying back %llu\n",
           bad ? "FAIL" : "ok", g_old ? "0028 gap" : "0029 gap", bad, cpu_bad, (unsigned long long)phases, NP, (unsigned long long)N, (unsigned long long)g_copied_pages,
           (unsigned long long)g_kept, (unsigned long long)nvn_dirty_gpu_raced());
    return bad != 0;
}
