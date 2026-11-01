// [UIT native] A small job queue for the shader background threads (vkb_shader.c: TOTK_SHADER_EARLY's compiles and
// TOTK_SHADER_BIN_BG's file writes), on its own so tools/tests/test_shqueue.c can run it without Vulkan.
// First in, first out; bounded by a job count and a byte count (a push past either is refused: the caller does the work
// itself or drops a hint). pop blocks until a job arrives; done(bytes) returns a job's bytes once its work is finished.
#pragma once
#include <pthread.h>
#include <stddef.h>
#include <stdint.h>

typedef struct shq_job { struct shq_job *next; size_t bytes; } shq_job;   // the first member of the caller's job
typedef struct {
    pthread_mutex_t mu;
    pthread_cond_t cv;
    shq_job *head, *tail;
    size_t n, bytes, max_n, max_bytes;   // bytes: queued or being worked on
} shq;
#define SHQ_INIT(maxn, maxb) {PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER, NULL, NULL, 0, 0, (maxn), (maxb)}

static inline int shq_push(shq *q, shq_job *j) {   // 1: queued (the queue's now); 0: full
    pthread_mutex_lock(&q->mu);
    if (q->n >= q->max_n || q->bytes + j->bytes > q->max_bytes) { pthread_mutex_unlock(&q->mu); return 0; }
    j->next = NULL;
    if (q->tail) q->tail->next = j; else q->head = j;
    q->tail = j;
    q->n++;
    q->bytes += j->bytes;
    pthread_cond_signal(&q->cv);
    pthread_mutex_unlock(&q->mu);
    return 1;
}
static inline shq_job *shq_pop(shq *q) {
    pthread_mutex_lock(&q->mu);
    while (!q->head) pthread_cond_wait(&q->cv, &q->mu);
    shq_job *j = q->head;
    q->head = j->next;
    if (!q->head) q->tail = NULL;
    q->n--;
    pthread_mutex_unlock(&q->mu);
    return j;
}
static inline void shq_done(shq *q, size_t bytes) {
    pthread_mutex_lock(&q->mu);
    q->bytes -= bytes;
    pthread_mutex_unlock(&q->mu);
}

// TOTK_SHADER_EARLY: the same shader code handed over again (same code address, control address and length) is not
// copied and hashed again. A 4096-entry table of tags; only a hint (racy by design): a miss costs one background
// lookup, a wrong hit leaves the shader to its first draw, as without the option. Returns 1 when seen before.
static inline uint64_t shq_tag(uint64_t data, uint64_t control, uint32_t length) {
    uint64_t t = data * 0x9E3779B97F4A7C15ull ^ control * 0xC2B2AE3D27D4EB4Full ^ length;
    return t ? t : 1;
}
static inline int shq_seen(_Atomic uint64_t tab[4096], uint64_t tag) {
    return __atomic_exchange_n((uint64_t *)&tab[tag >> 52], tag, __ATOMIC_RELAXED) == tag;
}
