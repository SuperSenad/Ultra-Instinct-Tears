// [UIT 0144] The uniform capture's memory, kept from one submission to the next instead of allocated and freed each
// time. Every submit call (game thread, vkb_ucap.c mtl_sub_capture_ubos) built ~1-2 MiB of captured bytes plus the
// range tables (tens of thousands of 24-byte entries, grown by doubling) and the sort's scratch, and the replay thread
// freed the capture a submission later. Blocks that size are beyond the Windows heap's large-block limit (about 0.5-1 MiB
// on x64, by Windows version): each one is a fresh VirtualAlloc, so every page is a demand-zero fault on first touch, every frame, on the
// game thread. Now the game thread's tables and scratch live as long as the thread, and the capture itself is one block
// taken from and given back to a small pool (urc_*): the replay thread puts it back, the next capture takes it.
#ifndef VKB_URECYCLE_H
#define VKB_URECYCLE_H
#include <pthread.h>
#include <stdlib.h>

enum { URC_KEEP = 4 };   // captures in flight between the game thread and the replay: more than that are malloc'd
typedef struct { void *p; size_t cap; } urc_blk;
typedef struct { pthread_mutex_t mu; int n; urc_blk b[URC_KEEP]; unsigned long long made, reused; } urc_pool;
#define URC_POOL_INIT {PTHREAD_MUTEX_INITIALIZER, 0, {{0, 0}}, 0, 0}

// A block of at least need bytes (*cap: its size): the smallest kept one that fits, else a new one with a quarter more
// room (rounded to 64 KiB) so a slowly growing scene does not make a new one each time. NULL if malloc fails.
static inline void *urc_get(urc_pool *P, size_t need, size_t *cap) {
    if (!need) need = 1;
    pthread_mutex_lock(&P->mu);
    int best = -1;
    for (int i = 0; i < P->n; i++)
        if (P->b[i].cap >= need && (best < 0 || P->b[i].cap < P->b[best].cap)) best = i;
    if (best >= 0) {
        urc_blk b = P->b[best];
        P->b[best] = P->b[--P->n];
        P->reused++;
        pthread_mutex_unlock(&P->mu);
        *cap = b.cap;
        return b.p;
    }
    P->made++;
    pthread_mutex_unlock(&P->mu);
    size_t c = need + need / 4;
    c = (c + 65535) & ~(size_t)65535;
    void *p = malloc(c);
    *cap = p ? c : 0;
    return p;
}
// Gives a block back (from any thread). With URC_KEEP kept, the smallest of them and this one is freed.
static inline void urc_put(urc_pool *P, void *p, size_t cap) {
    if (!p) return;
    pthread_mutex_lock(&P->mu);
    if (P->n < URC_KEEP) { P->b[P->n++] = (urc_blk){p, cap}; pthread_mutex_unlock(&P->mu); return; }
    int small = 0;
    for (int i = 1; i < P->n; i++) if (P->b[i].cap < P->b[small].cap) small = i;
    if (P->b[small].cap < cap) { void *q = P->b[small].p; P->b[small] = (urc_blk){p, cap}; p = q; }
    pthread_mutex_unlock(&P->mu);
    free(p);
}
#endif
