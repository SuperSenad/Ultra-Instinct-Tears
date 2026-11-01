// [perf-mt] The submit queue of TOTK_VK_ASYNC_SUBMIT=1 (backend_vk.c mtl_sub_commit; box test tools/tests/test_async_submit.c).
// A submission is recorded and its command buffers ended by the thread that recorded it; vkQueueSubmit2 then runs on
// one thread of its own ("totk-vk-submit"), so the time the driver takes in it (up to ~9 ms a frame, 2026-10 logs: the
// "queue submits" of the per-frame log line) is no longer the translation thread's.
// The rules:
//   - every fence taken (mtl_sub_commit's ++g_fence) is pushed exactly once: a real submission, or a skip (no GPU work)
//   - they are handed out strictly in fence order (vkb_subq_pop waits for the next one): the timeline semaphore is
//     signalled with increasing values, and the retire thread gets its jobs in order, as before
//   - vkb_subq_done publishes "submitted up to": what waits for the GPU (mtl_wait, a GPU wait on the timeline in
//     present.c) needs the signal submitted first, so the frame's "ready" (vkp_out_ready) is called by the submit
//     thread after its vkQueueSubmit2
// Items are the caller's (the first member of its own struct); the queue only links them.
// [UIT 0236] Several items may carry one fence (TOTK_VK_EARLY_STREAM's early parts, then the submission itself): they
// are handed out in push order, and the submit thread calls vkb_subq_done only for the last (the one that signals).
#ifndef VKB_SUBQ_H
#define VKB_SUBQ_H
#include <pthread.h>
#include <stdint.h>
typedef struct vkb_subq_item { struct vkb_subq_item *next; uint64_t fence; uint64_t t_push; } vkb_subq_item;
typedef struct {
    pthread_mutex_t mu;
    pthread_cond_t cv;        // an item came, or one was submitted
    vkb_subq_item *head;      // by fence, ascending
    uint64_t next;            // the fence to hand out next
    uint64_t submitted;       // every fence <= this one is submitted (or skipped)
    uint64_t depth, max_depth;   // items in the queue; the most at once (the per-frame log line)
} vkb_subq;
#define VKB_SUBQ_INIT {PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER, NULL, 1, 0, 0, 0}   // fences start at 1

static inline void vkb_subq_push(vkb_subq *q, vkb_subq_item *it) {
    pthread_mutex_lock(&q->mu);
    vkb_subq_item **p = &q->head;   // nearly always appended: fences are taken in order and pushed soon after
    // [UIT 0236] after items of the same fence: TOTK_VK_EARLY_STREAM's parts (no signal) go before their fence's last part
    while (*p && (*p)->fence <= it->fence) p = &(*p)->next;
    it->next = *p; *p = it;
    if (++q->depth > q->max_depth) q->max_depth = q->depth;
    pthread_cond_broadcast(&q->cv);
    pthread_mutex_unlock(&q->mu);
}
// The next item in fence order (blocks until it is pushed)
static inline vkb_subq_item *vkb_subq_pop(vkb_subq *q) {
    pthread_mutex_lock(&q->mu);
    while (!q->head || q->head->fence != q->next) pthread_cond_wait(&q->cv, &q->mu);
    vkb_subq_item *it = q->head;
    q->head = it->next;
    q->depth--;
    pthread_mutex_unlock(&q->mu);
    return it;
}
// The popped item `fence` is submitted (or was a skip): the next one may go
static inline void vkb_subq_done(vkb_subq *q, uint64_t fence) {
    pthread_mutex_lock(&q->mu);
    if (fence > q->submitted) q->submitted = fence;
    q->next = fence + 1;
    pthread_cond_broadcast(&q->cv);
    pthread_mutex_unlock(&q->mu);
}
static inline uint64_t vkb_subq_submitted(vkb_subq *q) {
    pthread_mutex_lock(&q->mu);
    uint64_t s = q->submitted;
    pthread_mutex_unlock(&q->mu);
    return s;
}
static inline void vkb_subq_wait_submitted(vkb_subq *q, uint64_t fence) {
    pthread_mutex_lock(&q->mu);
    while (q->submitted < fence) pthread_cond_wait(&q->cv, &q->mu);
    pthread_mutex_unlock(&q->mu);
}
static inline uint64_t vkb_subq_take_max_depth(vkb_subq *q) {
    pthread_mutex_lock(&q->mu);
    uint64_t m = q->max_depth;
    q->max_depth = q->depth;
    pthread_mutex_unlock(&q->mu);
    return m;
}
#endif
