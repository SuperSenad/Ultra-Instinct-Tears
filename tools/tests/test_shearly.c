// [UIT 0146] TOTK_SHADER_EARLY's two steps (vkb_shader.c), as a model on the real queue (vkb_shqueue.h): shaders
// arrive in a burst, 59% already known (as 7,522 of 12,751 in the 2026-10-30 log); a known one costs a lookup, a new
// one a build (~32 ms there; here 1/100 of the times). One queue (the old way) vs a sorting thread + build queue.
// Checks: every shader is either looked up or counted dropped, each new one built at most once, and the two-step
// queue drops far fewer. Build: cc -O2 -pthread -Isrc/gfx tools/tests/test_shearly.c -o /tmp/t/shearly
#define _GNU_SOURCE
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#include "vkb_shqueue.h"

enum { N = 12751, BUILDERS = 3 };
typedef struct { shq_job q; int id; } job;
static shq g_in, g_bq;
static _Atomic int g_built[N], g_looked, g_bdrop, g_left;
static int is_new(int id) { return (id * 2654435761u) % 100 >= 59; }
static void spin_us(int us) { struct timespec a, b; clock_gettime(CLOCK_MONOTONIC, &a); do clock_gettime(CLOCK_MONOTONIC, &b); while ((b.tv_sec - a.tv_sec) * 1000000L + (b.tv_nsec - a.tv_nsec) / 1000 < us); }
static int g_split;
static void build(job *j) { spin_us(320); atomic_fetch_add(&g_built[j->id], 1); }
static void *builder(void *u) {
    shq *q = g_split ? &g_bq : &g_in;
    for (;;) {
        job *j = (job *)shq_pop(q);
        if (j->id < 0) { shq_done(q, j->q.bytes); free(j); return NULL; }
        if (!g_split) { spin_us(2); atomic_fetch_add(&g_looked, 1); if (is_new(j->id)) build(j); }
        else build(j);
        shq_done(q, j->q.bytes); free(j); atomic_fetch_sub(&g_left, 1);
    }
}
static void *sorter(void *u) {
    for (;;) {
        job *j = (job *)shq_pop(&g_in);
        shq_done(&g_in, j->q.bytes);
        if (j->id < 0) { free(j); return NULL; }
        spin_us(2); atomic_fetch_add(&g_looked, 1);
        if (is_new(j->id) && shq_push(&g_bq, &j->q)) continue;
        if (is_new(j->id)) atomic_fetch_add(&g_bdrop, 1);
        free(j); atomic_fetch_sub(&g_left, 1);
    }
}
static int run(int split, int *dropped) {
    g_split = split;
    shq a = SHQ_INIT(2048, 128u << 20), b = SHQ_INIT(4096, 256u << 20);
    g_in = a; g_bq = b;
    atomic_store(&g_looked, 0); atomic_store(&g_bdrop, 0); atomic_store(&g_left, 0);
    for (int i = 0; i < N; i++) atomic_store(&g_built[i], 0);
    pthread_t t[BUILDERS + 1];
    for (int i = 0; i < BUILDERS; i++) pthread_create(&t[i], NULL, builder, NULL);
    if (split) pthread_create(&t[BUILDERS], NULL, sorter, NULL);
    int drop = 0;
    for (int i = 0; i < N; i++) {
        job *j = malloc(sizeof *j); j->id = i; j->q.bytes = 40 << 10;
        atomic_fetch_add(&g_left, 1);
        if (!shq_push(&g_in, &j->q)) { free(j); drop++; atomic_fetch_sub(&g_left, 1); }
        if (i % 4 == 0) usleep(1);   // the game loading: a few shaders per microsecond-ish
    }
    while (atomic_load(&g_left) > 0) usleep(1000);
    shq *stopq = split ? &g_bq : &g_in;
    for (int i = 0; i < BUILDERS; i++) { job *j = malloc(sizeof *j); j->id = -1; j->q.bytes = 0; while (!shq_push(stopq, &j->q)) usleep(100); }
    if (split) { job *j = malloc(sizeof *j); j->id = -1; j->q.bytes = 0; while (!shq_push(&g_in, &j->q)) usleep(100); }
    for (int i = 0; i < BUILDERS + split; i++) pthread_join(t[i], NULL);
    int bad = 0, nb = 0;
    for (int i = 0; i < N; i++) { int k = atomic_load(&g_built[i]); if (k > 1 || (k && !is_new(i))) bad++; nb += k; }
    if (atomic_load(&g_looked) + drop != N) bad++;
    if (atomic_load(&g_bdrop) + nb + drop < 0) bad++;
    *dropped = drop + atomic_load(&g_bdrop);
    printf("%s: %d shaders, dropped %d (queue full %d, build queue full %d), built %d, looked up %d%s\n",
           split ? "two steps" : "one queue", N, *dropped, drop, atomic_load(&g_bdrop), nb, atomic_load(&g_looked), bad ? "  BAD" : "");
    return bad;
}
int main(void) {
    int d1, d2, bad = run(0, &d1) + run(1, &d2);
    if (d2 * 2 > d1) { printf("two steps did not halve the drops\n"); bad++; }
    printf(bad ? "FAIL\n" : "ok\n");
    return bad != 0;
}
