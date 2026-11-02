// [UIT 0360] TOTK_VK_UCAP_HELPERS: helper threads for the uniform capture in the submit call (vkb_ucap.c
// mtl_sub_capture_ubos). The capture has to read the game's memory before the submit call returns (other game threads
// rebuild the constants right after it), so it cannot be handed to another thread and left behind: the game thread
// would return before the bytes are taken. Instead its two parts that only read (which vertex / index ranges the CPU
// wrote recently, and copying the bytes) are split into parts that the game thread and the helpers take in turn, and
// the game thread waits for the last one before it goes on. Same bytes, same order; less time in the call.
// Pure pthread code, tested on the box: tools/tests/test_upar.c.
#ifndef VKB_UPAR_H
#define VKB_UPAR_H
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <string.h>

enum { UPAR_MAX = 4 };
typedef void (*upar_fn)(void *ctx, int part, int parts);
typedef struct {
    pthread_mutex_t mu;
    pthread_cond_t job, done;
    int nthr, busy, quit, parts;
    uint64_t gen;
    upar_fn fn;
    void *ctx;
    _Atomic int next, left;
    pthread_t th[UPAR_MAX];
    void (*on_start)(void);   // each helper calls it once (thread name, placement), may be NULL
} upar;
#define UPAR_INIT {PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER, PTHREAD_COND_INITIALIZER, 0, 0, 0, 0, 0, 0, 0, 0, 0, {0}, 0}

static inline void upar_take(upar *P, upar_fn fn, void *ctx, int parts) {   // parts of this job until none are left
    for (;;) {
        int i = atomic_fetch_add_explicit(&P->next, 1, memory_order_acq_rel);
        if (i >= parts) return;
        fn(ctx, i, parts);
        if (atomic_fetch_sub_explicit(&P->left, 1, memory_order_acq_rel) == 1) {
            pthread_mutex_lock(&P->mu);
            pthread_cond_broadcast(&P->done);
            pthread_mutex_unlock(&P->mu);
        }
    }
}
static void *upar_thread(void *arg) {
    upar *P = (upar *)arg;
    if (P->on_start) P->on_start();
    uint64_t seen = 0;
    pthread_mutex_lock(&P->mu);
    for (;;) {
        while (!P->quit && P->gen == seen) pthread_cond_wait(&P->job, &P->mu);
        if (P->quit) break;
        seen = P->gen;
        upar_fn fn = P->fn; void *ctx = P->ctx; int parts = P->parts;
        P->busy++;
        pthread_mutex_unlock(&P->mu);
        upar_take(P, fn, ctx, parts);
        pthread_mutex_lock(&P->mu);
        if (--P->busy == 0) pthread_cond_broadcast(&P->done);
    }
    pthread_mutex_unlock(&P->mu);
    return NULL;
}
// Starts n helpers (at most UPAR_MAX) once; returns how many run. 0: none (run upar_run on the caller alone)
static inline int upar_start(upar *P, int n) {
    if (P->nthr || n <= 0) return P->nthr;
    if (n > UPAR_MAX) n = UPAR_MAX;
    for (int i = 0; i < n; i++) {
        if (pthread_create(&P->th[i], NULL, upar_thread, P)) break;
        P->nthr++;
    }
    return P->nthr;
}
// fn(ctx, part, parts) for every part 0..parts-1, on the caller and the helpers; returns when all have run.
// One caller at a time (the game thread's submit call).
static inline void upar_run(upar *P, upar_fn fn, void *ctx, int parts) {
    if (parts <= 0) return;
    if (!P->nthr || parts == 1) { for (int i = 0; i < parts; i++) fn(ctx, i, parts); return; }
    pthread_mutex_lock(&P->mu);
    while (P->busy) pthread_cond_wait(&P->done, &P->mu);   // a helper still leaving the last job: it reads next / parts
    P->fn = fn; P->ctx = ctx; P->parts = parts;
    atomic_store_explicit(&P->left, parts, memory_order_relaxed);
    atomic_store_explicit(&P->next, 0, memory_order_release);
    P->gen++;
    pthread_cond_broadcast(&P->job);
    pthread_mutex_unlock(&P->mu);
    upar_take(P, fn, ctx, parts);
    for (int s = 0; s < 2000 && atomic_load_explicit(&P->left, memory_order_acquire); s++) {
#if defined(__x86_64__) || defined(_M_X64)
        __builtin_ia32_pause();
#endif
    }
    if (atomic_load_explicit(&P->left, memory_order_acquire)) {
        pthread_mutex_lock(&P->mu);
        while (atomic_load_explicit(&P->left, memory_order_acquire)) pthread_cond_wait(&P->done, &P->mu);
        pthread_mutex_unlock(&P->mu);
    }
}
static inline void upar_stop(upar *P) {   // for the tests
    pthread_mutex_lock(&P->mu);
    P->quit = 1;
    pthread_cond_broadcast(&P->job);
    pthread_mutex_unlock(&P->mu);
    for (int i = 0; i < P->nthr; i++) pthread_join(P->th[i], NULL);
    P->nthr = 0;
}

// The capture's copy, split by bytes: part p copies bytes [p*total/parts, (p+1)*total/parts) of the concatenated
// output, whichever ranges they fall in (off: each range's start in out, ascending; src: where each range is read from)
typedef struct { uint8_t *out; const size_t *off; const uint64_t *lo, *hi; size_t stride; int n; size_t total; } upar_copy;
static inline uint64_t upar_lo(const upar_copy *c, int i) { return *(const uint64_t *)((const uint8_t *)c->lo + c->stride * (size_t)i); }
static inline uint64_t upar_hi(const upar_copy *c, int i) { return *(const uint64_t *)((const uint8_t *)c->hi + c->stride * (size_t)i); }
static void upar_copy_part(void *ctx, int p, int parts) {
    const upar_copy *c = (const upar_copy *)ctx;
    size_t a = c->total / (size_t)parts * (size_t)p, b = p == parts - 1 ? c->total : c->total / (size_t)parts * (size_t)(p + 1);
    if (a >= b) return;
    int l = 0, h = c->n - 1;   // the last range starting at or before a
    while (l < h) { int m = (l + h + 1) / 2; if (c->off[m] <= a) l = m; else h = m - 1; }
    for (int i = l; i < c->n && a < b; i++) {
        size_t s = c->off[i], len = (size_t)(upar_hi(c, i) - upar_lo(c, i));
        if (s + len <= a) continue;
        size_t from = a - s, to = (b < s + len ? b : s + len) - s;
        memcpy(c->out + s + from, (const uint8_t *)(uintptr_t)upar_lo(c, i) + from, to - from);
        a = s + to;
    }
}
#endif
