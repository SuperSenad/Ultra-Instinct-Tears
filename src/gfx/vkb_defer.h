// [UIT 0045] Deferred destruction (backend_vk.c, "deferred destruction"; box test tools/tests/test_pool_defer.c).
// An object given up while a submission may be recorded on another thread can be referred to by that submission's
// commands: it is destroyed once the first submission committed after it was given up is complete.
//   vkd_push   given up (no fence yet: the open submission, if any, has not committed)
//   vkd_tag    a submission commits (before it is queued): what was given up so far waits for its fence
//   vkd_take   the GPU finished up to `done`: the entries to destroy now (the caller destroys them, outside the lock,
//              then vkd_release: until then vkd_find still sees them)
//   vkd_tag_range  vkd_tag for given-up objects of a host range only (see there)
//   vkd_find   an entry whose host range overlaps [lo, hi): 0 none, 1 waiting for *fence, 2 no fence yet, 3 being destroyed
// The includer defines vkd_obj (the handles) before including this.
#ifndef VKB_DEFER_H
#define VKB_DEFER_H
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
typedef struct vkd_ent { uint64_t fence; uintptr_t lo, hi; vkd_obj o; int dying; struct vkd_ent *next, *out; } vkd_ent;
typedef struct { vkd_ent *fresh, *tagged; pthread_mutex_t mu; uint64_t pushed, taken; } vkd_q;
#define VKD_Q_INIT {NULL, NULL, PTHREAD_MUTEX_INITIALIZER, 0, 0}
static inline int vkd_push(vkd_q *q, vkd_obj o, uintptr_t lo, uintptr_t hi) {
    vkd_ent *d = malloc(sizeof *d);
    if (!d) return 0;
    *d = (vkd_ent){0, lo, hi, o, 0, NULL, NULL};
    pthread_mutex_lock(&q->mu);
    d->next = q->fresh; q->fresh = d;
    q->pushed++;
    pthread_mutex_unlock(&q->mu);
    return 1;
}
static inline void vkd_tag(vkd_q *q, uint64_t fence) {
    pthread_mutex_lock(&q->mu);
    while (q->fresh) { vkd_ent *d = q->fresh; q->fresh = d->next; d->fence = fence; d->next = q->tagged; q->tagged = d; }
    pthread_mutex_unlock(&q->mu);
}
// Entries with a host range overlapping [lo, hi) that have no fence yet wait for `fence` instead. Only for objects no
// submission begun later can find (a deleted pool: out of the table first), and only once the submission open when it
// was given up has committed (fence: the last committed one).
static inline void vkd_tag_range(vkd_q *q, uintptr_t lo, uintptr_t hi, uint64_t fence) {
    pthread_mutex_lock(&q->mu);
    for (vkd_ent **p = &q->fresh; *p;) {
        vkd_ent *d = *p;
        if (d->lo < hi && lo < d->hi) { *p = d->next; d->fence = fence; d->next = q->tagged; q->tagged = d; } else p = &d->next;
    }
    pthread_mutex_unlock(&q->mu);
}
static inline vkd_ent *vkd_take(vkd_q *q, uint64_t done) {   // linked by ->out; still listed (dying) until vkd_release
    vkd_ent *out = NULL;
    pthread_mutex_lock(&q->mu);
    for (vkd_ent *d = q->tagged; d; d = d->next)
        if (d->fence <= done && !d->dying) { d->dying = 1; d->out = out; out = d; }
    pthread_mutex_unlock(&q->mu);
    return out;
}
static inline void vkd_release(vkd_q *q, vkd_ent *out) {   // destroyed: off the list
    pthread_mutex_lock(&q->mu);
    while (out) {
        vkd_ent *d = out; out = d->out;
        for (vkd_ent **p = &q->tagged; *p; p = &(*p)->next) if (*p == d) { *p = d->next; break; }
        q->taken++;
        free(d);
    }
    pthread_mutex_unlock(&q->mu);
}
static inline int vkd_find(vkd_q *q, uintptr_t lo, uintptr_t hi, uint64_t *fence) {
    int r = 0;
    pthread_mutex_lock(&q->mu);
    for (vkd_ent *d = q->fresh; d && !r; d = d->next) if (d->lo < hi && lo < d->hi) r = 2;
    for (vkd_ent *d = q->tagged; d && !r; d = d->next) if (d->lo < hi && lo < d->hi) { r = d->dying ? 3 : 1; *fence = d->fence; }
    pthread_mutex_unlock(&q->mu);
    return r;
}
static inline uint64_t vkd_waiting(vkd_q *q) {   // given up, not destroyed yet
    pthread_mutex_lock(&q->mu);
    uint64_t n = q->pushed - q->taken;
    pthread_mutex_unlock(&q->mu);
    return n;
}
#endif
