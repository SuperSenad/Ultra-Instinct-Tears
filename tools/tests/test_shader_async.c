// [UIT 0039] Box test for asynchronous shader compilation (TOTK_SHADER_ASYNC, src/gfx/vkb_shasync.h).
//   gcc -O2 -Wall -pthread tools/tests/test_shader_async.c -Isrc/gfx -o /tmp/tsa && /tmp/tsa
// 1. Two background compilers and four drawing threads race on many shaders (the drawing threads either wait or
//    compile a still-queued shader themselves): each shader is compiled exactly once, and whoever reads "done" sees
//    the compiler's result. 2. The every-frame streak that decides which targets a skipped draw may render into.
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include "vkb_shasync.h"

enum { NS = 20000, NBG = 2, NFG = 4 };
typedef struct { int pending; int compiled; uint64_t obj; } sh_t;
static sh_t g_sh[NS];
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv = PTHREAD_COND_INITIALIZER;
static int g_next;   // background queue: next index
static int g_bad;

static void compile(sh_t *s) {
    __atomic_fetch_add(&s->compiled, 1, __ATOMIC_RELAXED);
    s->obj = 0x1234567800000000ull | (uint64_t)(s - g_sh);   // plain write, published by the release below
    shasync_publish(&s->pending, &mu, &cv);
}
static void *bg(void *p) {
    (void)p;
    for (;;) {
        int i = __atomic_fetch_add(&g_next, 1, __ATOMIC_RELAXED);
        if (i >= NS) return NULL;
        if (shasync_claim(&g_sh[i].pending)) compile(&g_sh[i]);
    }
}
static void *fg(void *p) {
    unsigned x = (unsigned)(uintptr_t)p * 2654435761u + 1;
    for (int k = 0; k < NS * 2; k++) {
        x = x * 1103515245u + 12345u;
        sh_t *s = &g_sh[(x >> 8) % NS];
        if (__atomic_load_n(&s->pending, __ATOMIC_ACQUIRE)) {
            if (shasync_claim(&s->pending)) compile(s); else shasync_wait_done(&s->pending, &mu, &cv);
        }
        if (s->obj != (0x1234567800000000ull | (uint64_t)(s - g_sh))) __atomic_fetch_add(&g_bad, 1, __ATOMIC_RELAXED);
    }
    return NULL;
}

int main(void) {
    int fails = 0;
    for (int round = 0; round < 5; round++) {
        for (int i = 0; i < NS; i++) g_sh[i] = (sh_t){1, 0, 0};
        g_next = 0; g_bad = 0;
        pthread_t t[NBG + NFG];
        for (int i = 0; i < NFG; i++) pthread_create(&t[NBG + i], NULL, fg, (void *)(uintptr_t)(i + round * 7 + 1));
        for (int i = 0; i < NBG; i++) pthread_create(&t[i], NULL, bg, NULL);
        for (int i = 0; i < NBG + NFG; i++) pthread_join(t[i], NULL);
        int once = 1;
        for (int i = 0; i < NS; i++) if (g_sh[i].compiled != 1 || g_sh[i].pending) once = 0;
        printf("1.%d each shader compiled exactly once: %s; done read before the result: %d\n", round, once ? "yes" : "NO", g_bad);
        fails += !once || g_bad;
    }
    // 2. streaks
    uint64_t last = 0; uint32_t st = 0;
    for (uint64_t f = 1; f <= 40; f++) { st = shasync_streak_next(last, f, st); last = f; }
    int ok = st == 40 && shasync_every_frame(last, st, 41, 30) && shasync_every_frame(last, st, 42, 30) && !shasync_every_frame(last, st, 43, 30);
    printf("2.1 a target drawn into every frame for 40 frames counts (and stops counting 3 frames later): %s\n", ok ? "yes" : "NO"); fails += !ok;
    st = shasync_streak_next(last, 42, st); last = 42;   // one frame skipped (interval 2): the streak goes on
    ok = st == 41; printf("2.2 a one-frame gap keeps the streak: %s\n", ok ? "yes" : "NO"); fails += !ok;
    st = shasync_streak_next(last, 42, st);
    ok = st == 41; printf("2.3 a second draw in the same frame does not count twice: %s\n", ok ? "yes" : "NO"); fails += !ok;
    st = shasync_streak_next(last, 50, st); last = 50;
    ok = st == 1 && !shasync_every_frame(last, st, 50, 30); printf("2.4 a target drawn now and then (a shadow map refreshed every 8 frames) never counts: %s\n", ok ? "yes" : "NO"); fails += !ok;
    ok = !shasync_every_frame(0, 0, 5, 30); printf("2.5 a target never drawn into does not count: %s\n", ok ? "yes" : "NO"); fails += !ok;
    ok = shasync_within(100, 130, 30) && !shasync_within(100, 131, 30) && shasync_within(0, 5, 30);
    printf("2.6 a draw is skipped for at most 30 frames, then waits: %s\n", ok ? "yes" : "NO"); fails += !ok;
    printf(fails ? "FAILED (%d)\n" : "all passed\n", fails);
    return fails != 0;
}
