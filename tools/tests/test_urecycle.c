// [UIT 0144] Box test for src/gfx/vkb_urecycle.h (the uniform capture's blocks kept between submissions):
//  1. a producer ("game thread") takes blocks of varying sizes (0.1-3 MiB, a slowly growing scene with spikes), fills
//     them, and hands them to a consumer ("replay thread") through a queue of up to 6 in flight; the consumer checks
//     the bytes and gives the block back. Every block big enough, the contents intact, at most URC_KEEP kept, the rest
//     freed (no leak under ASan), most takes reused
//  2. the cost it removes: a 3 MiB capture block (bytes + tables) in fresh demand-zero pages every frame (what a block
//     beyond the Windows heap's large-block limit is: VirtualAlloc / VirtualFree; mmap / munmap here) vs a kept one
// cc -O2 -pthread -Isrc/gfx tools/tests/test_urecycle.c -o /tmp/t/urc && /tmp/t/urc
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include "vkb_urecycle.h"

static urc_pool P = URC_POOL_INIT;
enum { Q = 6, FRAMES = 3000 };
typedef struct { uint8_t *p; size_t need, cap; uint8_t tag; } item;
static item q[Q];
static int qh, qt;
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv = PTHREAD_COND_INITIALIZER;
static int fin;
static unsigned long bad, small;

static double now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e3 + t.tv_nsec / 1e6; }
static void *consumer(void *a) {
    (void)a;
    for (;;) {
        pthread_mutex_lock(&mu);
        while (qh == qt && !fin) pthread_cond_wait(&cv, &mu);
        if (qh == qt && fin) { pthread_mutex_unlock(&mu); return NULL; }
        item it = q[qh % Q]; qh++;
        pthread_cond_broadcast(&cv);
        pthread_mutex_unlock(&mu);
        for (size_t i = 0; i < it.need; i += 4093) if (it.p[i] != (uint8_t)(it.tag + i)) { bad++; break; }
        if (it.p[it.need - 1] != (uint8_t)(it.tag + it.need - 1)) bad++;
        urc_put(&P, it.p, it.cap);
    }
}
int main(void) {
    pthread_t c;
    pthread_create(&c, NULL, consumer, NULL);
    uint64_t rs = 12345;
    size_t base = 100 << 10;
    for (int f = 0; f < FRAMES; f++) {
        rs = rs * 6364136223846793005ull + 1442695040888963407ull;
        if (f % 10 == 0 && base < (2u << 20)) base += 8 << 10;         // the scene grows slowly
        size_t need = base + (rs >> 40) % (64 << 10);
        if ((rs >> 33) % 97 == 0) need = 3u << 20;                       // a spike now and then
        size_t cap;
        uint8_t *p = urc_get(&P, need, &cap);
        if (!p || cap < need) { small++; continue; }
        uint8_t tag = (uint8_t)f;
        for (size_t i = 0; i < need; i += 4093) p[i] = (uint8_t)(tag + i);
        p[need - 1] = (uint8_t)(tag + need - 1);
        pthread_mutex_lock(&mu);
        while (qt - qh == Q) pthread_cond_wait(&cv, &mu);
        q[qt % Q] = (item){p, need, cap, tag}; qt++;
        pthread_cond_broadcast(&cv);
        pthread_mutex_unlock(&mu);
    }
    pthread_mutex_lock(&mu); fin = 1; pthread_cond_broadcast(&cv); pthread_mutex_unlock(&mu);
    pthread_join(c, NULL);
    int kept = P.n;
    for (int i = 0; i < P.n; i++) free(P.b[i].p);
    P.n = 0;
    printf("1. %d frames, up to %d in flight: %lu blocks too small, %lu with wrong bytes, %llu new blocks, %llu reused (%.1f%%), %d kept at the end (max %d)\n",
           FRAMES, Q, small, bad, P.made, P.reused, 100.0 * (double)P.reused / (double)(P.made + P.reused), kept, URC_KEEP);
    int ok = !bad && !small && kept <= URC_KEEP && P.reused > P.made * 10;

    // 2. fresh pages every frame vs a kept block: write 3 MiB (the capture's copy touches every byte)
    const size_t SZ = 3u << 20;
    double fresh = 1e9, kept_ms = 1e9;
    for (int r = 0; r < 5; r++) {
        double t0 = now_ms();
        for (int f = 0; f < 50; f++) {
            uint8_t *p = mmap(NULL, SZ, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
            memset(p, f, SZ);
            munmap(p, SZ);
        }
        double t1 = now_ms();
        size_t cap;
        uint8_t *p = urc_get(&P, SZ, &cap);
        for (int f = 0; f < 50; f++) {
            memset(p, f, SZ);
            urc_put(&P, p, cap);
            p = urc_get(&P, SZ, &cap);
        }
        double t2 = now_ms();
        urc_put(&P, p, cap);
        if ((t1 - t0) / 50 < fresh) fresh = (t1 - t0) / 50;
        if ((t2 - t1) / 50 < kept_ms) kept_ms = (t2 - t1) / 50;
    }
    for (int i = 0; i < P.n; i++) free(P.b[i].p);
    P.n = 0;
    printf("2. a 3 MiB capture block per frame: fresh pages %.3f ms, kept block %.3f ms (best of 5 x 50)\n", fresh, kept_ms);
    printf(ok ? "ok\n" : "FAILED\n");
    return !ok;
}
