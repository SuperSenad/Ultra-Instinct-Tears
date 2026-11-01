// [UIT 0028] Box test: the retire thread's copy back of GPU writes (wb_put -> nvn_dirty_gpu_write) must not hide the
// game's CPU writes elsewhere in the same pages from the write watch. Game threads bump a counter at the start of
// random pages; a GPU-writeback thread writes the upper half of random pages (as wb_merge does for pages not CPU-written
// since its stamp); a copy is kept current like a pool's device copy (nvn_dirty_runs_stamp). Long run, and the pool is
// removed and added again at the same address now and then (address reuse, as pools come and go at a shrine exit).
// Afterwards every page's counter in the copy must equal memory. With nvn_dirty_win.c built with
// -DNVN_DIRTY_OLD_GPU_WRITE: the 0027 write (memcpy, then a scan of the page without generation), which loses them.
// cc -O2 -pthread tools/tests/test_dirty_gpuwrite.c src/gfx/nvn/nvn_dirty_win.c -o /tmp/t && /tmp/t 4
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "../../src/gfx/nvn/nvn_dirty.h"

#define NP 1024
static uint8_t *g_mem;
static _Atomic uint8_t g_bit[NP];   // the OS's write-watch bits
static _Atomic int g_stop, g_stop_w, g_pause, g_paused, g_resume;   // pause request n: acked with g_paused = n, until g_resume = n
static uint64_t g_shadow[NP], g_stamp, g_reuse;

int plat_written_pages(uint64_t addr, uint64_t size, uint64_t *pages, int max, uint64_t *page) {
    *page = 4096;
    uintptr_t b = (uintptr_t)g_mem;
    if (addr < b || addr >= b + (uint64_t)NP * 4096) return 0;
    int n = 0;
    for (uint64_t a = addr; a < addr + size && n < max; a += 4096) {
        uint64_t p = (a - b) / 4096;
        if (atomic_exchange(&g_bit[p], 0)) pages[n++] = a;
        if ((p & 63) == 0) sched_yield();
    }
    return n;
}
static uint64_t rnd(uint64_t *s) { *s ^= *s << 13; *s ^= *s >> 7; *s ^= *s << 17; return *s; }
static _Atomic int g_phase, g_done;
// A streaming load: in each phase each game thread writes 64 random pages once (a write that is lost is never
// repeated, as when a new area's meshes and textures are written once).
static void *writer(void *arg) {
    uint64_t s = (uintptr_t)arg * 0x9E3779B97F4A7C15ull + 1;
    int seen = 0;
    while (!atomic_load(&g_stop_w)) {
        int ph = atomic_load(&g_phase);
        if (ph == seen) { sched_yield(); continue; }
        seen = ph;
        for (int i = 0; i < 64; i++) {
            uint64_t p = rnd(&s) % NP;
            atomic_store(&g_bit[p], 1);   // the OS marks the page as the write happens
            atomic_fetch_add((_Atomic uint64_t *)(g_mem + p * 4096), 1);
            atomic_store(&g_bit[p], 1);
            for (int k = rnd(&s) % 64; k > 0; k--) atomic_signal_fence(memory_order_seq_cst);
        }
        atomic_fetch_add(&g_done, 1);
    }
    return NULL;
}
// The GPU's bytes; the OS's watch bit for that write is not modelled (the write's own scan resets it either way).
static void gw(uint8_t *d, const uint8_t *s, size_t n) { nvn_dirty_gpu_write(d, s, n); }
static void *gpu(void *arg) {
    uint64_t s = (uintptr_t)arg * 31 + 7, k = 0;
    static uint8_t src[2048];
    while (!atomic_load(&g_stop)) {
        int rq = atomic_load(&g_pause);
        if (rq != atomic_load(&g_paused)) { atomic_store(&g_paused, rq); while (atomic_load(&g_resume) != rq && !atomic_load(&g_stop)) sched_yield(); }
        memset(src, (int)(++k & 255), sizeof src);
        uint64_t p = rnd(&s) % NP;
        uint64_t stamp = nvn_dirty_now();
        if (nvn_dirty_since(g_mem + p * 4096, 4096, stamp)) continue;   // wb_merge: CPU-written page left
        gw(g_mem + p * 4096 + 2048, src, 2048);
    }
    return NULL;
}
static void copy_run(void *ctx, size_t off, size_t n) {
    (void)ctx;
    for (size_t p = off / 4096; p < (off + n + 4095) / 4096; p++) g_shadow[p] = atomic_load((_Atomic uint64_t *)(g_mem + p * 4096));
}
static void sync_once(void) { nvn_dirty_runs_stamp(g_mem, (size_t)NP * 4096, g_stamp, 0, copy_run, NULL, &g_stamp); }
static void full_copy(void) {
    nvn_dirty_since(g_mem, (size_t)NP * 4096, UINT64_MAX);
    g_stamp = nvn_dirty_last_stamp();
    for (int p = 0; p < NP; p++) g_shadow[p] = atomic_load((_Atomic uint64_t *)(g_mem + p * 4096));
}
int main(int argc, char **argv) {
    int secs = argc > 1 ? atoi(argv[1]) : 4;
    if (posix_memalign((void **)&g_mem, 4096, (size_t)NP * 4096)) return 2;
    memset(g_mem, 0, (size_t)NP * 4096);
    nvn_dirty_add_range(g_mem, (size_t)NP * 4096);
    full_copy();
    pthread_t w[3], g;
    for (int i = 0; i < 3; i++) pthread_create(&w[i], NULL, writer, (void *)(uintptr_t)i);
    pthread_create(&g, NULL, gpu, NULL);
    struct timespec t0, t;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    uint64_t phases = 0, lost = 0;
    for (uint64_t it = 0;; it++) {
        atomic_store(&g_done, 0);
        atomic_fetch_add(&g_phase, 1);
        while (atomic_load(&g_done) < 3) sync_once();   // the device copy kept current while the game writes
        int rq = (int)it + 1;
        atomic_store(&g_pause, rq); while (atomic_load(&g_paused) != rq) sched_yield();
        sync_once(); sync_once();   // nothing running: what is still missing was lost
        int bad = 0;
        for (int p = 0; p < NP; p++) bad += g_shadow[p] != *(uint64_t *)(g_mem + p * 4096);
        lost += bad; phases++;
        if (bad) full_copy();
        if (it % 50 == 49) {   // the pool goes and comes back at the same address: a new device copy, full fill
            nvn_dirty_remove_range(g_mem);
            nvn_dirty_add_range(g_mem, (size_t)NP * 4096);
            full_copy(); g_reuse++;
        }
        atomic_store(&g_resume, rq);
        clock_gettime(CLOCK_MONOTONIC, &t);
        if (t.tv_sec - t0.tv_sec >= secs) break;
    }
    atomic_store(&g_stop_w, 1);
    for (int i = 0; i < 3; i++) pthread_join(w[i], NULL);
    atomic_store(&g_stop, 1);
    pthread_join(g, NULL);
    int bad = (int)lost;
    printf("%s: %d page writes lost to the copy in %llu streaming phases of 192 writes (%llu pool reuses, %llu CPU writes caught during GPU writes)\n", bad ? "FAIL" : "ok", bad, (unsigned long long)phases,
           (unsigned long long)g_reuse, (unsigned long long)nvn_dirty_gpu_raced());
    return bad != 0;
}
