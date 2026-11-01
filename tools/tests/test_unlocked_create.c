// [UIT 0231] Box test for TOTK_VK_IMAGE_CREATE_UNLOCKED: vkb_image_of's make-without-the-lock-then-look-again, as a
// model (the same steps: look up under the lock; miss: unlock, make (a timed stand-in for vkCreateImage + memory,
// 150 us), lock, look again, use the one already there and throw ours away, else put ours in).
//   cc -O2 -Wall -pthread tools/tests/test_unlocked_create.c -o /tmp/test_uc && /tmp/test_uc
// Checks: with six threads asking for the same and different serials at once, every thread gets the same object per
// serial, each serial's object is put in once, every thrown-away one is counted. Measures the lookups' lock waits with
// the lock held over the make (before) and not (after).
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

enum { NSER = 1 << 14, THREADS = 6 };
typedef struct obj { uint64_t serial; int made_by; } obj;
static obj *g_map[NSER];
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static _Atomic uint64_t g_made, g_lost, g_wait_ns, g_waits, g_lookups;
static int g_unlocked;
static uint64_t now_ns(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec; }
static void lock(void) {
    uint64_t t = now_ns();
    if (pthread_mutex_trylock(&g_mu)) { pthread_mutex_lock(&g_mu); g_wait_ns += now_ns() - t; g_waits++; }
}
static obj *make(uint64_t serial, int who) {   // the stand-in for create(): 150 us of work
    uint64_t t = now_ns();
    while (now_ns() - t < 150000) {}
    obj *o = calloc(1, sizeof *o); o->serial = serial; o->made_by = who;
    g_made++;
    return o;
}
static obj *image_of(uint64_t serial, int who) {
    obj *pre = NULL;
    lock();
    for (;;) {
        obj *o = g_map[serial];
        if (o) { if (pre) { free(pre); g_lost++; } pthread_mutex_unlock(&g_mu); return o; }
        if (!pre && g_unlocked) { pthread_mutex_unlock(&g_mu); pre = make(serial, who); lock(); continue; }
        if (!pre) pre = make(serial, who);
        g_map[serial] = pre;
        pthread_mutex_unlock(&g_mu);
        return pre;
    }
}
static obj *g_seen[THREADS][NSER];
static volatile int g_phase;
static uint64_t rnd(uint64_t *s) { *s ^= *s << 13; *s ^= *s >> 7; *s ^= *s << 17; return *s; }
typedef struct { int id; int frames; } targ;
static void *worker(void *p) {
    targ *a = p; uint64_t rs = 0x9E3779B97F4A7C15ull * (uint64_t)(a->id + 1);
    uint64_t next_new = 1;
    for (int f = 0; f < a->frames; f++) {
        // a frame: 600 lookups of textures already there, and 1 new serial per thread (5-6 new per frame while running);
        // every 4th frame all threads ask for the same new serial at once (the race)
        for (int k = 0; k < 600; k++) {
            uint64_t s = 1 + rnd(&rs) % (next_new > 1 ? next_new : 1);
            obj *o = image_of(s % NSER, a->id); g_seen[a->id][s % NSER] = o; g_lookups++;
            for (volatile int w = 0; w < 300; w++) {}
        }
        uint64_t ns = (f % 4 == 0) ? 4000 + (uint64_t)f : 8000 + (uint64_t)f * THREADS + (uint64_t)a->id;
        obj *o = image_of(ns % NSER, a->id); g_seen[a->id][ns % NSER] = o;
        next_new = 2000;
    }
    return NULL;
}
static int run(int unlocked, int frames, double *wait_ms, double *wall_ms) {
    memset(g_map, 0, sizeof g_map); memset(g_seen, 0, sizeof g_seen);
    g_made = g_lost = g_wait_ns = g_waits = g_lookups = 0; g_unlocked = unlocked;
    for (int i = 1; i <= 2000; i++) { g_map[i] = calloc(1, sizeof(obj)); g_map[i]->serial = (uint64_t)i; }
    pthread_t th[THREADS]; targ a[THREADS];
    uint64_t t0 = now_ns();
    for (int i = 0; i < THREADS; i++) { a[i] = (targ){i, frames}; pthread_create(&th[i], NULL, worker, &a[i]); }
    for (int i = 0; i < THREADS; i++) pthread_join(th[i], NULL);
    *wall_ms = (now_ns() - t0) / 1e6; *wait_ms = g_wait_ns / 1e6;
    int bad = 0, distinct = 0;
    for (int s = 0; s < NSER; s++) {
        obj *o = g_map[s];
        if (o) distinct++;
        for (int t = 0; t < THREADS; t++) if (g_seen[t][s] && g_seen[t][s] != o) bad++;
    }
    uint64_t newly = (uint64_t)distinct - 2000;
    if (g_made != newly + g_lost) bad++;
    printf("  %s: %d frames x %d threads, %.0f ms; lookups waited %.1f ms in all (%.2f ms per frame), %llu new objects put in, %llu made in all, %llu thrown away; %s\n",
           unlocked ? "made without the lock" : "made with the lock held", frames, THREADS, *wall_ms, *wait_ms, *wait_ms / frames,
           (unsigned long long)newly, (unsigned long long)g_made, (unsigned long long)g_lost, bad ? "MISMATCH" : "every thread got the same object per serial");
    return bad;
}
int main(void) {
    double w0, t0, w1, t1;
    printf("unlocked create replay (150 us per new object, 6 threads, 600 lookups each per frame):\n");
    if (run(0, 300, &w0, &t0) | run(1, 300, &w1, &t1)) { printf("FAIL\n"); return 1; }
    printf("  lock wait %.1f -> %.1f ms (%.1fx less), wall %.0f -> %.0f ms\nPASS\n", w0, w1, w0 / (w1 > 0.001 ? w1 : 0.001), t0, t1);
    return 0;
}
