// [UIT 0045] Box test: a memory pool deleted while another thread records a submission (backend_vk.c mtl_pool_del,
// on the real queue of src/gfx/vkb_defer.h). Threads, as in the game:
//   translation: begin (open = 1), presync - every pool in the table is the source of a copy in this submission (as
//                shadow_presync's copies in), commit (fence + 1, vkd_tag, queued, open = 0)
//   GPU/retire:  runs the submissions in order - a copy from a destroyed import is the lost device - then retires
//                them (done = fence) and destroys what waited for them (vkd_take)
//   game:        imports pools over a few host ranges and deletes them (x_drain does not stop the translation thread:
//                another game thread's submission is recorded meanwhile)
// Modes: (none) the 0045 protocol: 0 copies from destroyed imports, 0 overlapping live imports of one range;
//        old      destroy after waiting for submitted work (mtl_finish), as before 0045: copies from destroyed imports
//        noguard  0045 without the wait in mtl_pool_add: two live imports of one host range
// cc -O2 -pthread -Isrc/gfx tools/tests/test_pool_defer.c -o /tmp/t/pooldefer && /tmp/t/pooldefer && /tmp/t/pooldefer old && /tmp/t/pooldefer noguard
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
typedef struct { int id; } vkd_obj;
#include "vkb_defer.h"

enum { NOBJ = 1 << 20, NRANGE = 4, MAXP = 16, SUBS = 20000, MAXREF = 32 };
static vkd_q q = VKD_Q_INIT;
static _Atomic int g_open;
static _Atomic uint64_t g_fence;
static uint64_t g_done;
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_cv = PTHREAD_COND_INITIALIZER;
static pthread_rwlock_t g_pool_mu = PTHREAD_RWLOCK_INITIALIZER;
typedef struct { int id; uintptr_t lo, hi; } pool;
static pool g_pools[MAXP];
static int g_npools;
static _Atomic uint8_t g_dead[NOBJ];      // destroyed import
static _Atomic uint8_t g_gone[NOBJ];      // deleted by the game (its import may wait to be destroyed)
static uintptr_t g_lo[NOBJ], g_hi[NOBJ];
static _Atomic int g_next = 1;
static _Atomic int g_stop;
static long g_uaf, g_overlap, g_deferred, g_waits, g_copies;
static int mode;   // 0: 0045, 1: old, 2: noguard
typedef struct sub { uint64_t fence; int n; int ref[MAXREF]; struct sub *next; } sub;
static sub *g_q, **g_qt = &g_q;

static int wait_done(uint64_t f, int block) {
    pthread_mutex_lock(&g_mu);
    while (block && g_done < f) pthread_cond_wait(&g_cv, &g_mu);   // every submitted one retires
    int ok = g_done >= f;
    pthread_mutex_unlock(&g_mu);
    return ok;
}
static void spin(int n) { for (volatile int i = 0; i < n; i++) {} }
static void destroy(int id) { atomic_store(&g_dead[id], 1); }
static void reap(uint64_t done) {
    vkd_ent *l = vkd_take(&q, done);
    for (vkd_ent *d = l; d; d = d->out) { spin(200); destroy(d->o.id); }   // vkDestroyBuffer + vkFreeMemory take a while
    vkd_release(&q, l);
}

static void *translation(void *u) {
    (void)u;
    for (int s = 0; s < SUBS; s++) {
        atomic_store(&g_open, 1);
        sub *b = calloc(1, sizeof *b);
        pthread_rwlock_rdlock(&g_pool_mu);   // presync: the copies in
        for (int i = 0; i < g_npools && b->n < MAXREF; i++) b->ref[b->n++] = g_pools[i].id;
        pthread_rwlock_unlock(&g_pool_mu);
        spin(rand() % 3000);                 // the rest of the recording
        b->fence = atomic_fetch_add(&g_fence, 1) + 1;
        vkd_tag(&q, b->fence);
        pthread_mutex_lock(&g_mu);
        *g_qt = b; g_qt = &b->next;
        pthread_cond_broadcast(&g_cv);
        pthread_mutex_unlock(&g_mu);
        atomic_store(&g_open, 0);
        if (rand() % 4 == 0) sched_yield();
    }
    atomic_store(&g_stop, 1);
    pthread_mutex_lock(&g_mu); pthread_cond_broadcast(&g_cv); pthread_mutex_unlock(&g_mu);
    return NULL;
}
static void *gpu(void *u) {
    (void)u;
    for (;;) {
        pthread_mutex_lock(&g_mu);
        while (!g_q && !atomic_load(&g_stop)) pthread_cond_wait(&g_cv, &g_mu);
        sub *b = g_q;
        if (b) { g_q = b->next; if (!g_q) g_qt = &g_q; }
        pthread_mutex_unlock(&g_mu);
        if (!b) break;
        spin(rand() % 2000);   // the GPU reaches the copies some time after the submit
        for (int i = 0; i < b->n; i++) { g_copies++; if (atomic_load(&g_dead[b->ref[i]])) g_uaf++; }
        pthread_mutex_lock(&g_mu);
        g_done = b->fence;
        pthread_cond_broadcast(&g_cv);
        pthread_mutex_unlock(&g_mu);
        reap(b->fence);
        free(b);
    }
    return NULL;
}
static void pool_gone_tag(uintptr_t a, uintptr_t b) { if (!atomic_load(&g_open)) vkd_tag_range(&q, a, b, atomic_load(&g_fence)); }
static void pool_add(uintptr_t lo, uintptr_t hi) {
    if (mode == 0) {   // pool_gone_wait
        uint64_t f; int r, counted = 0;
        while ((r = vkd_find(&q, lo, hi, &f))) {
            if (!counted++) g_waits++;
            if (atomic_load(&g_stop)) return;
            if (r == 2) pool_gone_tag(lo, hi);
            r = vkd_find(&q, lo, hi, &f);
            if (r == 1) { wait_done(f, 1); reap(f); } else if (r) sched_yield();
        }
    }
    int id = atomic_fetch_add(&g_next, 1);
    if (id >= NOBJ) return;
    for (int k = 1; k < id; k++)   // a deleted pool's import of this range, not destroyed yet (live pools may overlap: the game's)
        if (atomic_load(&g_gone[k]) && !atomic_load(&g_dead[k]) && g_lo[k] < hi && lo < g_hi[k]) { g_overlap++; break; }
    g_lo[id] = lo; g_hi[id] = hi;
    pthread_rwlock_wrlock(&g_pool_mu);
    if (g_npools < MAXP) g_pools[g_npools++] = (pool){id, lo, hi};
    else { pthread_rwlock_unlock(&g_pool_mu); destroy(id); return; }
    pthread_rwlock_unlock(&g_pool_mu);
}
static void pool_del(int i) {
    pthread_rwlock_wrlock(&g_pool_mu);
    if (i >= g_npools) { pthread_rwlock_unlock(&g_pool_mu); return; }
    pool e = g_pools[i];
    memmove(&g_pools[i], &g_pools[i + 1], sizeof *g_pools * (size_t)(g_npools - i - 1));
    g_npools--;
    atomic_store(&g_gone[e.id], 1);
    atomic_thread_fence(memory_order_seq_cst);
    int open = atomic_load(&g_open);
    pthread_rwlock_unlock(&g_pool_mu);
    wait_done(atomic_load(&g_fence), 1);   // mtl_finish
    if (mode != 1 && open) {
        vkd_push(&q, (vkd_obj){e.id}, e.lo, e.hi);
        pool_gone_tag(e.lo, e.hi);
        g_deferred++;
    } else destroy(e.id);
}
static void *game(void *u) {
    (void)u;
    while (!atomic_load(&g_stop)) {
        int r = rand() % NRANGE, used = 1;
        uintptr_t lo = 0;
        for (int k = 0; k < NRANGE && used; k++) {   // a range no pool in the table has
            lo = 0x10000000u + (uintptr_t)((r + k) % NRANGE) * 0x40000u;
            pthread_rwlock_rdlock(&g_pool_mu);
            used = 0;
            for (int i = 0; i < g_npools; i++) if (g_pools[i].lo == lo) used = 1;
            pthread_rwlock_unlock(&g_pool_mu);
        }
        if (used) { pool_del(0); continue; }
        pool_add(lo, lo + 0x40000u);
        spin(rand() % 1500);
        pthread_rwlock_rdlock(&g_pool_mu);
        int n = g_npools;
        pthread_rwlock_unlock(&g_pool_mu);
        if (n > NRANGE / 2) pool_del(rand() % n);
        if (rand() % 3 == 0) pool_del(0);   // deleted right after it was added (as the log's pool deleted twice)
    }
    return NULL;
}
int main(int argc, char **argv) {
    mode = argc > 1 && !strcmp(argv[1], "old") ? 1 : argc > 1 && !strcmp(argv[1], "noguard") ? 2 : 0;
    srand(1);
    pthread_t t[3];
    pthread_create(&t[0], NULL, gpu, NULL);
    pthread_create(&t[1], NULL, game, NULL);
    pthread_create(&t[2], NULL, translation, NULL);
    pthread_join(t[2], NULL);
    pthread_join(t[1], NULL);
    pthread_join(t[0], NULL);
    reap(UINT64_MAX >> 1);   // at exit (mtl_finish)
    printf("%s: %d submissions, %ld copies, %d imports, %ld deletes deferred, %ld imports waited; copies from destroyed imports %ld, "
           "overlapping live imports %ld, left waiting %llu\n", mode == 1 ? "old" : mode == 2 ? "noguard" : "0045", SUBS, g_copies,
           atomic_load(&g_next) - 1, g_deferred, g_waits, g_uaf, g_overlap, (unsigned long long)vkd_waiting(&q));
    int bad = mode == 1 ? g_uaf == 0 : mode == 2 ? g_overlap == 0 : (g_uaf || g_overlap);
    printf("%s\n", bad ? "FAIL" : "ok");
    return bad;
}
