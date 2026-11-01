// [UIT 0140] Box test for src/gfx/vkb_pfor.c (the texture preparation threads, TOTK_TEX_THREADS):
//  1. every index of a job runs exactly once, for jobs of every size (0, 1, 2 .. 5000) one after another
//  2. several threads calling at once (one gets the pool, the others run on their own) and a call from inside fn
//  3. the real work: the first look of an upload (nvn_deswizzle + the BC7 cache key of every surface, vkb_tex.c
//     astc_look) of a 1024x1024 texture with 121 layers and 11 levels (the 57 ms upload of the 2026-10-30 21:08 log),
//     serial and through vkb_pfor: the rows and keys must be the same bytes; and the time of both
// cc -O2 -pthread -Isrc/gfx -Isrc/gfx/nvn tools/tests/test_pfor.c src/gfx/tex/nvn_deswizzle.c src/gfx/nvn/nvn_tex.c \
//    src/gfx/tex/bc7key.c -o /tmp/t/pfor && /tmp/t/pfor
#define VKB_PFOR_NO_NAME
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "../../src/gfx/vkb_pfor.c"
#include "../../src/gfx/tex/nvn_deswizzle.h"
#include "../../src/gfx/tex/bc7key.h"

int vkb_cpu_count(void) { long n = sysconf(_SC_NPROCESSORS_ONLN); return n > 0 ? (int)n : 4; }
static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)
static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e3 + t.tv_nsec / 1e6; }

typedef struct { _Atomic int *hits; int n; } cnt_ctx;
static void count(void *c, int i) { cnt_ctx *x = c; if (i >= 0 && i < x->n) atomic_fetch_add(&x->hits[i], 1); }
static int run_count(int n) {   // 0 = every index once
    _Atomic int *h = calloc((size_t)n + 1, sizeof *h);
    cnt_ctx c = {h, n};
    vkb_pfor(n, count, &c);
    int bad = 0;
    for (int i = 0; i < n; i++) bad += atomic_load(&h[i]) != 1;
    free(h);
    return bad;
}
static void *caller(void *arg) {
    int *bad = arg;
    for (int r = 0; r < 300; r++) *bad += run_count(1 + (r * 37) % 700);
    return NULL;
}
static void nested(void *c, int i) { (void)i; *(int *)c += 0; atomic_fetch_add((_Atomic int *)c, run_count(5)); }

// ---- 3: the first look of an ASTC upload
typedef struct { const nvn_texdesc *d; const uint8_t *src; uint8_t *rows; size_t *off; uint32_t *lv, *ly, *pitch, *lw, *lh; bc7_key *k; int *bad; } look_ctx;
static void look_one(void *c, int i) {
    look_ctx *x = c;
    uint8_t *r = x->rows + x->off[i];
    if (nvn_deswizzle(x->d, x->lv[i], x->ly[i], x->src, r, x->pitch[i])) { x->bad[i] = 1; return; }
    size_t bytes = x->off[i + 1] - x->off[i];
    x->k[i] = bc7_key_of(r, bytes, x->lw[i], x->lh[i], 1, 8, 8);
}

int main(void) {
    printf("threads %d (auto for %d cores; 24 cores: %d)\n", vkb_pfor_threads(), vkb_cpu_count(), vkb_pfor_auto(24));
    CHECK(vkb_pfor_auto(1) == 2 && vkb_pfor_auto(8) == 4 && vkb_pfor_auto(24) == 12 && vkb_pfor_auto(64) == 12);
    int bad = 0;
    for (int n = 0; n <= 2000; n++) bad += run_count(n);
    bad += run_count(5000);
    CHECK(bad == 0);
    printf("1. 2002 jobs of 0..5000 items: %d indices not run exactly once\n", bad);
    pthread_t t[6];
    int tb[6] = {0};
    for (int i = 0; i < 6; i++) pthread_create(&t[i], NULL, caller, &tb[i]);
    for (int i = 0; i < 6; i++) { pthread_join(t[i], NULL); bad += tb[i]; }
    _Atomic int nb = 0;
    vkb_pfor(64, nested, (void *)&nb);
    CHECK(bad == 0 && atomic_load(&nb) == 0);
    printf("2. 6 threads x 300 jobs at once and 64 nested calls: %d + %d wrong\n", bad, (int)atomic_load(&nb));

    nvn_texdesc d = {0};
    d.target = 4; d.format = 0x8e; d.width = 1024; d.height = 1024; d.depth = 121; d.levels = 11;   // 2D array
    int bw, bh;
    CHECK(nvn_fmt_bpb(d.format, &bw, &bh) == 16);
    nvn_texlayout L;
    nvn_tex_layout(&d, &L);
    uint8_t *src = malloc(L.size);
    uint64_t s = 88172645463325252ull;
    for (size_t i = 0; i + 8 <= L.size; i += 8) { s ^= s << 13; s ^= s >> 7; s ^= s << 17; memcpy(src + i, &s, 8); }
    int ns = 11 * (int)L.layers;
    size_t *off = calloc((size_t)ns + 1, sizeof *off);
    uint32_t *lv = calloc(ns, 4), *ly = calloc(ns, 4), *pitch = calloc(ns, 4), *lw = calloc(ns, 4), *lh = calloc(ns, 4);
    int k = 0;
    for (uint32_t level = 0; level < 11; level++) {
        uint32_t w = 1024 >> level, h = 1024 >> level, bx = (w + bw - 1) / bw, by = (h + bh - 1) / bh;
        for (uint32_t layer = 0; layer < L.layers; layer++, k++) {
            lv[k] = level; ly[k] = layer; pitch[k] = bx * 16; lw[k] = w; lh[k] = h;
            off[k + 1] = off[k] + (size_t)bx * 16 * by;
        }
    }
    uint8_t *ra = malloc(off[ns]), *rb = malloc(off[ns]);
    bc7_key *ka = calloc(ns, sizeof *ka), *kb = calloc(ns, sizeof *kb);
    int *ba = calloc(ns, sizeof(int)), *bb = calloc(ns, sizeof(int));
    look_ctx ca = {&d, src, ra, off, lv, ly, pitch, lw, lh, ka, ba}, cb = {&d, src, rb, off, lv, ly, pitch, lw, lh, kb, bb};
    memset(ra, 0, off[ns]); memset(rb, 0, off[ns]);   // touched once before timing (the game's memory is)
    double best_s = 1e9, best_p = 1e9;
    for (int r = 0; r < 5; r++) {
        double t0 = now();
        for (int i = 0; i < ns; i++) look_one(&ca, i);
        double t1 = now();
        vkb_pfor(ns, look_one, &cb);
        double t2 = now();
        if (t1 - t0 < best_s) best_s = t1 - t0;
        if (t2 - t1 < best_p) best_p = t2 - t1;
    }
    int diff = memcmp(ra, rb, off[ns]) != 0, kd = 0, nb2 = 0;
    for (int i = 0; i < ns; i++) { kd += ka[i].a != kb[i].a || ka[i].b != kb[i].b; nb2 += ba[i] != bb[i] || ba[i]; }
    CHECK(!diff && !kd && !nb2);
    printf("3. first look of %d surfaces (%.1f MiB of rows): serial %.1f ms, %d threads %.1f ms; rows differ %d, keys differ %d, failures %d\n",
           ns, off[ns] / 1048576.0, best_s, vkb_pfor_threads(), best_p, diff, kd, nb2);
    printf(fails ? "FAILED\n" : "ok\n");
    return fails != 0;
}
