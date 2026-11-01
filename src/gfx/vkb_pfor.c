// [UIT 0140] vkb_pfor.h. One job at a time: the caller that owns g_job_mu publishes (fn, ctx, n), wakes the workers,
// takes indices itself and waits until every index is done and no worker is still inside the job (so the next job
// cannot be confused with this one). A worker that wakes late finds the job closed and takes nothing.
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>

#include "vkb_pfor.h"

int vkb_cpu_count(void);   // vkb_bc7.c (Windows); the tests give their own
#ifndef VKB_PFOR_NO_NAME
#include "compat/gfx_compat.h"
#define PFOR_NAME() pthread_setname_np("totk-texprep")
#else
#define PFOR_NAME() ((void)0)
#endif

enum { PFOR_MAX = 16 };
static pthread_mutex_t g_job_mu = PTHREAD_MUTEX_INITIALIZER;   // held by the caller for the whole job
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;       // the fields below
static pthread_cond_t g_cv = PTHREAD_COND_INITIALIZER, g_done_cv = PTHREAD_COND_INITIALIZER;
static vkb_pfor_fn g_fn;
static void *g_ctx;
static int g_n, g_open, g_active;
static uint64_t g_serial;
static _Atomic int g_next, g_done;

int vkb_pfor_auto(int cpus) {   // half the logical cores (the game, translation and recording threads keep the rest)
    int t = cpus / 2;
    return t < 2 ? 2 : t > 12 ? 12 : t;
}
int vkb_pfor_threads(void) {
    static int t;
    if (!t) {
        const char *e = getenv("TOTK_TEX_THREADS");
        int v = e && *e ? atoi(e) : 0;
        int c = v > 0 ? v : vkb_pfor_auto(vkb_cpu_count());
        t = c < 1 ? 1 : c > PFOR_MAX ? PFOR_MAX : c;
    }
    return t;
}
static void work(vkb_pfor_fn fn, void *ctx, int n) {
    for (;;) {
        int i = atomic_fetch_add(&g_next, 1);
        if (i >= n) return;
        fn(ctx, i);
        if (atomic_fetch_add(&g_done, 1) + 1 == n) {
            pthread_mutex_lock(&g_mu);
            pthread_cond_broadcast(&g_done_cv);
            pthread_mutex_unlock(&g_mu);
        }
    }
}
static void *worker(void *unused) {
    (void)unused;
    PFOR_NAME();
    uint64_t seen = 0;
    for (;;) {
        pthread_mutex_lock(&g_mu);
        while (g_serial == seen) pthread_cond_wait(&g_cv, &g_mu);
        seen = g_serial;
        int open = g_open;
        vkb_pfor_fn fn = g_fn;
        void *ctx = g_ctx;
        int n = g_n;
        if (open) g_active++;
        pthread_mutex_unlock(&g_mu);
        if (!open) continue;
        work(fn, ctx, n);
        pthread_mutex_lock(&g_mu);
        g_active--;
        pthread_cond_broadcast(&g_done_cv);
        pthread_mutex_unlock(&g_mu);
    }
    return NULL;
}
static int g_workers;
static void start(void) {
    for (int i = 0; i < vkb_pfor_threads() - 1; i++) {
        pthread_t t;
        if (pthread_create(&t, NULL, worker, NULL) == 0) { pthread_detach(t); g_workers++; }
    }
}
void vkb_pfor(int n, vkb_pfor_fn fn, void *ctx) {
    if (n <= 0) return;
    if (n == 1 || vkb_pfor_threads() <= 1 || pthread_mutex_trylock(&g_job_mu)) {   // one item, serial, or busy: here
        for (int i = 0; i < n; i++) fn(ctx, i);
        return;
    }
    static pthread_once_t once = PTHREAD_ONCE_INIT;
    pthread_once(&once, start);
    if (!g_workers) {
        pthread_mutex_unlock(&g_job_mu);
        for (int i = 0; i < n; i++) fn(ctx, i);
        return;
    }
    atomic_store(&g_next, 0);
    atomic_store(&g_done, 0);
    pthread_mutex_lock(&g_mu);
    g_fn = fn; g_ctx = ctx; g_n = n; g_open = 1;
    g_serial++;
    pthread_cond_broadcast(&g_cv);
    pthread_mutex_unlock(&g_mu);
    work(fn, ctx, n);
    pthread_mutex_lock(&g_mu);
    while (atomic_load(&g_done) < n) pthread_cond_wait(&g_done_cv, &g_mu);
    g_open = 0;
    while (g_active) pthread_cond_wait(&g_done_cv, &g_mu);
    pthread_mutex_unlock(&g_mu);
    pthread_mutex_unlock(&g_job_mu);
}
