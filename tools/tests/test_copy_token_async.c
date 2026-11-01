// [UIT native-1031] Box test: recording-copy marks with the translation thread (hle_nvn.c copy_token_async, copy_queue,
// copy_release_ok, pin_scan, release_copies).
//   cc -O2 -pthread tools/tests/test_copy_token_async.c -o /tmp/test_copy_token_async && /tmp/test_copy_token_async
// The model: game threads record command lists (a host copy each; EndRecording writes its mark into the control memory,
// then enters it in the copy table), submit them to the translation thread's queue and reuse the control memory at once
// (the mark is overwritten), as TOTK does when it frees a command buffer's control memory. Before replaying each
// submission the translation thread sweeps: a copy whose mark is gone is released (poisoned). The replay reads every copy
// it was given: a poisoned one is a use after free.
// 1. the old hazard (release on a gone mark alone): uses after free - why marks were off with the translation thread.
// 2. the fix, one and two submitting game threads: a copy goes only when no queued submission uses it (a count: + before
//    the job is queued, - after its replay): no use after free, and the copies still go (live copies stay bounded instead
//    of growing like the 12.9 -> 109.6 MiB of the 2026-10-30 longplay).
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails;
#define CHECK(c, what) do { int ok_ = !!(c); printf("%s %s\n", ok_ ? "ok  " : "FAIL", what); fails += !ok_; } while (0)

#define NCTL 64          // control memory slots each game thread cycles through
#define QN 8             // submissions queued at most
#define MAGIC 0x434f5059u
typedef struct { _Atomic uint32_t magic; _Atomic uint32_t queued; } copy_t;
typedef struct { copy_t *copy; _Atomic uint64_t *mark; int live; } rec_t;   // the copy table (g_cbk)
static _Atomic uint64_t g_mark[2][NCTL];   // control memory: the mark is the copy's address
static rec_t *g_tab; static int g_ntab, g_captab;
static pthread_mutex_t g_copy_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_q_mu = PTHREAD_MUTEX_INITIALIZER; static pthread_cond_t g_q_cv = PTHREAD_COND_INITIALIZER;
static copy_t *g_q[QN][4]; static int g_qn[QN]; static uint64_t g_in, g_done;
static long g_uaf, g_released, g_peak_live, g_kept;
static int g_fixed, g_end;
static int release_ok(copy_t *c) {   // copy_release_ok
    if (!g_fixed || !atomic_load_explicit(&c->queued, memory_order_acquire)) return 1;
    g_kept++; return 0;
}
static void sweep(void) {   // pin_scan (the translation thread, before a replay)
    pthread_mutex_lock(&g_copy_mu);
    long live = 0; int w = 0;
    for (int i = 0; i < g_ntab; i++) {
        rec_t r = g_tab[i];
        if (atomic_load(r.mark) != (uint64_t)(uintptr_t)r.copy && release_ok(r.copy)) {
            atomic_store(&r.copy->magic, 0xdead); g_released++; continue;   // released: poisoned (kept allocated, so a late read is defined)
        }
        g_tab[w++] = r; live++;
    }
    g_ntab = w;
    if (live > g_peak_live) g_peak_live = live;
    pthread_mutex_unlock(&g_copy_mu);
}
static void *xthread(void *a) {
    (void)a;
    for (;;) {
        pthread_mutex_lock(&g_q_mu); while (g_done == g_in && !g_end) pthread_cond_wait(&g_q_cv, &g_q_mu);
        if (g_done == g_in) { pthread_mutex_unlock(&g_q_mu); return NULL; }
        int k = (int)(g_done % QN), n = g_qn[k]; copy_t *h[4]; memcpy(h, g_q[k], sizeof h); pthread_mutex_unlock(&g_q_mu);
        sweep();
        for (int i = 0; i < n; i++) if (atomic_load(&h[i]->magic) != MAGIC) g_uaf++;   // the replay reads the copy
        if (g_fixed) for (int i = 0; i < n; i++) atomic_fetch_sub_explicit(&h[i]->queued, 1, memory_order_release);   // after the replay
        pthread_mutex_lock(&g_q_mu); g_done++; pthread_cond_broadcast(&g_q_cv); pthread_mutex_unlock(&g_q_mu);
    }
}
typedef struct { int id; long subs; } game_arg;
static void *game(void *a) {
    game_arg *g = a; unsigned rnd = 12345u + (unsigned)g->id; int slot = 0;
    for (long s = 0; s < g->subs; s++) {
        copy_t *h[4]; int n = 1 + (int)((rnd = rnd * 1103515245u + 12345u) >> 16) % 4;
        for (int i = 0; i < n; i++) {   // EndRecording: the mark first, then the table (as hle_nvn.c)
            copy_t *c = malloc(sizeof *c); atomic_store(&c->magic, MAGIC); atomic_store(&c->queued, 0);
            _Atomic uint64_t *m = &g_mark[g->id][slot]; slot = (slot + 1) % NCTL;
            atomic_store(m, (uint64_t)(uintptr_t)c);
            pthread_mutex_lock(&g_copy_mu);
            if (g_ntab == g_captab) { g_captab = g_captab ? g_captab * 2 : 1024; g_tab = realloc(g_tab, sizeof *g_tab * (size_t)g_captab); }
            g_tab[g_ntab++] = (rec_t){c, m, 1};
            pthread_mutex_unlock(&g_copy_mu);
            h[i] = c;
        }
        if (g_fixed) for (int i = 0; i < n; i++) atomic_fetch_add_explicit(&h[i]->queued, 1, memory_order_relaxed);   // submit_call: before the queue
        pthread_mutex_lock(&g_q_mu); while (g_in - g_done >= QN) pthread_cond_wait(&g_q_cv, &g_q_mu);
        memcpy(g_q[g_in % QN], h, sizeof h); g_qn[g_in % QN] = n; g_in++; pthread_cond_broadcast(&g_q_cv); pthread_mutex_unlock(&g_q_mu);
        // the submit call returned: the game frees this control memory to its heap, other data overwrites the marks
        for (int i = 0; i < n; i++) atomic_store(&g_mark[g->id][(slot + NCTL - 1 - i) % NCTL], 0x1234);
    }
    return NULL;
}
static void run(int fixed, int threads, long subs) {
    g_fixed = fixed; g_uaf = g_released = g_peak_live = g_kept = 0; g_in = g_done = 0; g_ntab = 0; g_end = 0;
    memset(g_mark, 0, sizeof g_mark);
    pthread_t x, gt[2]; game_arg ga[2];
    pthread_create(&x, NULL, xthread, NULL);
    for (int i = 0; i < threads; i++) { ga[i] = (game_arg){i, subs}; pthread_create(&gt[i], NULL, game, &ga[i]); }
    for (int i = 0; i < threads; i++) pthread_join(gt[i], NULL);
    pthread_mutex_lock(&g_q_mu); g_end = 1; pthread_cond_broadcast(&g_q_cv); pthread_mutex_unlock(&g_q_mu);
    pthread_join(x, NULL);
    printf("     %s, %d game thread(s): %ld uses after free, %ld released, %ld kept while queued, peak live %ld\n",
           fixed ? "fixed" : "old rule", threads, g_uaf, g_released, g_kept, g_peak_live);
}
int main(void) {
    const long subs = 40000;
    run(0, 1, subs);
    CHECK(g_uaf > 0, "1. releasing on a gone mark alone frees copies a queued submission still reads");
    run(1, 1, subs);
    CHECK(g_uaf == 0, "2. one game thread: released only once no queued submission uses it: no use after free");
    CHECK(g_released > subs && g_peak_live < 2 * NCTL, "2. one game thread: the copies still go, live copies stay bounded");
    run(1, 2, subs);
    CHECK(g_uaf == 0, "2. two game threads: no use after free");
    CHECK(g_released > 2 * subs && g_peak_live < 4 * NCTL, "2. two game threads: the copies still go, live copies stay bounded");
    printf(fails ? "FAILED\n" : "all ok\n");
    return fails != 0;
}
