// [UIT 0047] Box test: one thread records GPU work at a time (src/gfx/vkb_recown.h, nvn_x_call in hle_nvn.c).
//   cc -O2 -pthread -Isrc/gfx tools/tests/test_recown.c -o /tmp/test_recown && /tmp/test_recown
// 1. the detector: the owner may re-enter, another thread is a clash, leaving frees it.
// 2. a forced overlap (thread B begins while A is between begin and commit, as ReadTexels on a game thread did beside
//    the translation thread): detected, and the unlocked "recording state" (a current slot and post ops, like g_cur and
//    g_npost) is corrupted - what the old code risked.
// 3. the fix's shape: game threads hand their GPU work to the translation thread (a job queue, like nvn_x_call) while
//    it replays its own submissions: no clash, every submission's post ops intact, every caller returns after its job.
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "vkb_recown.h"

static int fails;
#define CHECK(c, what) do { int ok_ = !!(c); printf("%s %s\n", ok_ ? "ok  " : "FAIL", what); fails += !ok_; } while (0)

// ---- the "recording state" (backend_vk.c: g_cur, g_npost, g_post) and its begin / post / commit
static vkb_recown g_own;
static int g_cur = -1, g_npost, g_post[64];
static long g_bad;   // a submission whose post ops were not its own
static void sub_begin(int who) { vkb_recown_enter(&g_own); g_cur = who; g_npost = 0; }
static void sub_post(int v) { if (g_npost < 64) g_post[g_npost++] = v; }
static void sub_commit(int who, int nexp) {
    int bad = g_cur != who || g_npost != nexp;
    for (int i = 0; i < g_npost && !bad; i++) bad = g_post[i] != who;
    if (bad) __atomic_add_fetch(&g_bad, 1, __ATOMIC_RELAXED);
    g_npost = 0; g_cur = -1;
    vkb_recown_leave(&g_own);
}
static void record(int who, int nops) { sub_begin(who); for (int i = 0; i < nops; i++) sub_post(who); sub_commit(who, nops); }

// ---- 2. forced overlap
static volatile int stage;
static void *overlap_b(void *p) {
    (void)p;
    while (stage != 1) ;
    sub_begin(2); sub_post(2); stage = 2;   // begins while A records (no turn taken)
    while (stage != 3) ;
    sub_commit(2, 1);
    return NULL;
}

// ---- 3. the job queue model (hle_nvn.c x_push / x_thread / x_wait_job)
typedef struct { void (*fn)(void *); void *arg; } job;
static job q[64];
static unsigned long q_in, q_done;
static int q_stop;
static pthread_mutex_t q_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t q_cv = PTHREAD_COND_INITIALIZER;
static unsigned long push(job j) {
    pthread_mutex_lock(&q_mu);
    while (q_in - q_done >= 64) pthread_cond_wait(&q_cv, &q_mu);
    q[q_in % 64] = j;
    unsigned long no = ++q_in;
    pthread_cond_broadcast(&q_cv);
    pthread_mutex_unlock(&q_mu);
    return no;
}
static void wait_job(unsigned long no) {
    pthread_mutex_lock(&q_mu); while (q_done < no) pthread_cond_wait(&q_cv, &q_mu); pthread_mutex_unlock(&q_mu);
}
static void *x_thread(void *p) {
    (void)p;
    for (;;) {
        pthread_mutex_lock(&q_mu);
        while (q_done == q_in && !q_stop) pthread_cond_wait(&q_cv, &q_mu);
        if (q_done == q_in && q_stop) { pthread_mutex_unlock(&q_mu); return NULL; }
        job j = q[q_done % 64];
        pthread_mutex_unlock(&q_mu);
        j.fn(j.arg);
        pthread_mutex_lock(&q_mu); q_done++; pthread_cond_broadcast(&q_cv); pthread_mutex_unlock(&q_mu);
    }
}
typedef struct { int who, n, ran; } texels;
static void texels_gpu(void *p) { texels *t = p; record(t->who, t->n); t->ran = 1; }   // read/write_texels_gpu
static void submit_job(void *p) { record(1, (int)(long)p); }                          // a replayed submission
static void x_call(void (*fn)(void *), void *arg) { wait_job(push((job){fn, arg})); }   // nvn_x_call
enum { GAME = 4, CALLS = 3000 };
static long lost_calls;
static void *game_thread(void *p) {
    int who = 10 + (int)(long)p;
    for (int i = 0; i < CALLS; i++) {
        texels t = {who, 1 + i % 5, 0};
        x_call(texels_gpu, &t);
        if (!t.ran) __atomic_add_fetch(&lost_calls, 1, __ATOMIC_RELAXED);
    }
    return NULL;
}
static void *submitter(void *p) {   // the game's render thread: queues submissions without waiting
    (void)p;
    for (int i = 0; i < 4 * CALLS; i++) push((job){submit_job, (void *)(long)(1 + i % 7)});
    return NULL;
}

int main(void) {
    // 1
    CHECK(vkb_recown_enter(&g_own) == 0 && vkb_recown_enter(&g_own) == 0, "owner enters, and may enter again (a split submission)");
    vkb_recown_leave(&g_own);
    CHECK(atomic_load(&g_own.owner) == 0, "leaving frees it");
    // 2
    pthread_t b;
    pthread_create(&b, NULL, overlap_b, NULL);
    sub_begin(1); sub_post(1); stage = 1;
    while (stage != 2) ;
    int clash = atomic_load(&g_own.clashes);
    sub_post(1);
    sub_commit(1, 2);
    stage = 3;
    pthread_join(b, NULL);
    CHECK(clash == 1, "forced overlap: the second recorder is reported");
    CHECK(g_bad >= 1, "forced overlap: the shared recording state is corrupted (the race the old ReadTexels path had)");
    // 3
    atomic_store(&g_own.clashes, 0); atomic_store(&g_own.owner, 0); g_bad = 0;
    pthread_t x, sub, g[GAME];
    pthread_create(&x, NULL, x_thread, NULL);
    pthread_create(&sub, NULL, submitter, NULL);
    for (long i = 0; i < GAME; i++) pthread_create(&g[i], NULL, game_thread, (void *)i);
    for (int i = 0; i < GAME; i++) pthread_join(g[i], NULL);
    pthread_join(sub, NULL);
    pthread_mutex_lock(&q_mu); q_stop = 1; pthread_cond_broadcast(&q_cv); pthread_mutex_unlock(&q_mu);
    pthread_join(x, NULL);
    printf("     %d game threads x %d texel calls, %d submissions: %lu jobs\n", GAME, CALLS, 4 * CALLS, q_done);
    CHECK(atomic_load(&g_own.clashes) == 0, "through the translation thread: never two recorders");
    CHECK(g_bad == 0, "through the translation thread: every submission's state intact");
    CHECK(lost_calls == 0 && q_done == (unsigned long)(GAME * CALLS + 4 * CALLS), "every caller returned after its own job ran");
    printf(fails ? "FAILURES\n" : "all ok\n");
    return fails != 0;
}
