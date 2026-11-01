// [UIT 0087] Box test of src/host/thr_wait.h (the Linux futex form of the WaitOnAddress path, and the condition
// variable path), used the way threads.c uses it: one global lock, a flag per thread, signals with the lock held,
// waits in loops. A missed wakeup would hang a ping-pong (a watchdog fails the test); timed waits must not report a
// timeout before the deadline.
//   clang -O2 -march=x86_64_v3 -pthread -Isrc/host tools/tests/test_thr_wait.c -o /tmp/tw
//   /tmp/tw && TOTK_WAIT_ON_ADDRESS=1 /tmp/tw
// Windows build (to run on a PC): zwcc -O2 -Isrc/host tools/tests/test_thr_wait.c -lapi-ms-win-core-synch-l1-2-0 -o tw.exe
#include <stdio.h>
#include <unistd.h>
#include "thr_wait.h"

#define NT 8
#define ROUNDS 100000
static pthread_mutex_t G = PTHREAD_MUTEX_INITIALIZER;
static thr_wait_t W[NT];
static int turn[NT];            // thread i may go when turn[i] (protected by G)
static volatile long done;
static void *ring(void *p) {    // a token passed round NT threads ROUNDS times
    int i = (int)(intptr_t)p;
    pthread_mutex_lock(&G);
    for (int r = 0; r < ROUNDS; r++) {
        while (!turn[i]) tw_wait(&W[i], &G, NULL);
        turn[i] = 0; int j = (i + 1) % NT; turn[j] = 1; tw_signal(&W[j]);
    }
    pthread_mutex_unlock(&G);
    __atomic_add_fetch(&done, 1, __ATOMIC_SEQ_CST);
    return NULL;
}
static void *watchdog(void *p) { (void)p; sleep(60); fprintf(stderr, "FAIL: hang (a wakeup was lost)\n"); _exit(1); }
static int64_t ns_now(void) { struct timespec t; clock_gettime(CLOCK_REALTIME, &t); return (int64_t)t.tv_sec * 1000000000 + t.tv_nsec; }

static int flag2; static thr_wait_t W2;
static void *late_signal(void *p) {   // signals after a delay, while the other thread may be in a timed wait
    usleep((useconds_t)(intptr_t)p);
    pthread_mutex_lock(&G); flag2 = 1; tw_signal(&W2); pthread_mutex_unlock(&G);
    return NULL;
}

int main(void) {
    int ok = 1;
    const char *mode = getenv("TOTK_WAIT_ON_ADDRESS"); mode = mode && !strcmp(mode, "1") ? "WaitOnAddress (futex)" : "condition variable";
    pthread_t wd; pthread_create(&wd, NULL, watchdog, NULL); pthread_detach(wd);
    for (int i = 0; i < NT; i++) tw_init(&W[i]);
    turn[0] = 1;
    pthread_t th[NT];
    int64_t t0 = ns_now();
    for (int i = 0; i < NT; i++) pthread_create(&th[i], NULL, ring, (void *)(intptr_t)i);
    for (int i = 0; i < NT; i++) pthread_join(th[i], NULL);
    double us = (ns_now() - t0) / 1e3 / (NT * (double)ROUNDS);
    printf("ok   %s: token passed %d times round %d threads, no wakeup lost (%.2f us per hand-over)\n", mode, ROUNDS, NT, us);
    // timed waits: nothing signals -> ETIMEDOUT, not before the deadline
    tw_init(&W2);
    int early = 0, late_ms = 0;
    pthread_mutex_lock(&G);
    for (int k = 0; k < 20; k++) {
        struct timespec dl; int64_t want = ns_now() + 3000000 + k * 250000;
        dl.tv_sec = want / 1000000000; dl.tv_nsec = want % 1000000000;
        int rc; while ((rc = tw_wait(&W2, &G, &dl)) != ETIMEDOUT) {}
        int64_t now = ns_now(); if (now < want) early++; if ((now - want) / 1000000 > late_ms) late_ms = (int)((now - want) / 1000000);
    }
    pthread_mutex_unlock(&G);
    printf("%s %s: 20 timed waits, %d ended before the deadline, latest %d ms after it\n", early ? "FAIL" : "ok  ", mode, early, late_ms);
    ok &= !early;
    // a signal during a long timed wait wakes it at once
    int woke_fast = 0;
    for (int k = 0; k < 200; k++) {
        flag2 = 0; pthread_t s; pthread_create(&s, NULL, late_signal, (void *)(intptr_t)(k % 3 ? 200 : 0));
        int64_t a = ns_now(), want = a + 2000000000LL; struct timespec dl = {want / 1000000000, want % 1000000000};
        pthread_mutex_lock(&G);
        while (!flag2) if (tw_wait(&W2, &G, &dl) == ETIMEDOUT && !flag2) break;
        int got = flag2; pthread_mutex_unlock(&G);
        pthread_join(s, NULL);
        woke_fast += got && ns_now() - a < 500000000;
    }
    printf("%s %s: %d of 200 signals during a 2 s timed wait woke it at once\n", woke_fast == 200 ? "ok  " : "FAIL", mode, woke_fast);
    ok &= woke_fast == 200;
    puts(ok ? "PASS" : "FAIL");
    return !ok;
}
