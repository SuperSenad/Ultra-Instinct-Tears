// [perf-mt] Box test: TOTK_VK_ASYNC_SUBMIT's submit queue (src/gfx/vkb_subq.h, as backend_vk.c uses it). Threads:
//   recorders: take fences (++g_fence, as mtl_sub_commit), wait for a free command slot (done >= fence - SLOTS, as
//              mtl_sub_begin), sometimes no GPU work (a skip: g_ok != 1), sometimes a frame to present; push
//   submit:    vkb_subq_pop -> "vkQueueSubmit2" (appends to the GPU's list; its timeline values must increase) ->
//              the retire job -> vkb_subq_done -> the frame's ready for the present thread
//   GPU:       runs the submitted list in order; done = fence (a wait on an unsubmitted value would hang)
//   present:   a GPU wait on a ready frame's value: that value must be submitted already (else the queue deadlocks)
//   waiters:   mtl_wait(fence) on random recent fences, as ReadTexels does
// Checks: submits in fence order, every fence once, no ready before its submit, everything retires (no hang).
// Modes: (none) the protocol; parts: [UIT 0236] 0-3 early parts (no signal) pushed under a fence before its last item; noorder: hand out in push order (the timeline order breaks with two recorders);
//        early: the frame ready at push time (before its submit)
// cc -O2 -pthread -Isrc/gfx tools/tests/test_async_submit.c -o /tmp/t/asub && /tmp/t/asub && /tmp/t/asub noorder && /tmp/t/asub early && /tmp/t/asub parts
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "vkb_subq.h"

enum { SUBS = 200000, RECORDERS = 3, SLOTS = 8, RING = 1 << 12 };
typedef struct { vkb_subq_item q; int skip, present, partial; } req;
static vkb_subq q = VKB_SUBQ_INIT;
static _Atomic uint64_t g_fence;
static uint64_t g_done;
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_cv = PTHREAD_COND_INITIALIZER;
static _Atomic uint64_t g_gpu_sub;   // last value "vkQueueSubmit2" signalled
static uint64_t g_gpu_list[RING];    // submitted, not yet run
static _Atomic uint64_t g_gpu_head, g_gpu_tail;
static int g_mode;   // 0 protocol, 1 noorder, 2 early, 3 parts
static _Atomic long g_part_bad, g_parts;   // [UIT 0236] parts mode: an early part not between fence-1's and its own last item
static _Atomic long g_out_of_order, g_ready_early, g_seen_twice, g_presented;
static _Atomic uint8_t g_seen[SUBS + 64];
static _Atomic int g_stop;
// noorder: a plain FIFO
static req *g_fifo[RING]; static uint64_t g_fh, g_ft;
static pthread_mutex_t g_fmu = PTHREAD_MUTEX_INITIALIZER; static pthread_cond_t g_fcv = PTHREAD_COND_INITIALIZER;

static void complete(uint64_t f) { pthread_mutex_lock(&g_mu); if (f > g_done) g_done = f; pthread_cond_broadcast(&g_cv); pthread_mutex_unlock(&g_mu); }
static void wait_done(uint64_t f) { pthread_mutex_lock(&g_mu); while (g_done < f) pthread_cond_wait(&g_cv, &g_mu); pthread_mutex_unlock(&g_mu); }
static void present_ready(uint64_t value) {   // present.c: a GPU wait on `value` queued now
    if (value > atomic_load(&g_gpu_sub)) atomic_fetch_add(&g_ready_early, 1);
    atomic_fetch_add(&g_presented, 1);
}
static void push(req *r) {
    if (g_mode == 1) {
        pthread_mutex_lock(&g_fmu); g_fifo[g_ft++ % RING] = r; pthread_cond_signal(&g_fcv); pthread_mutex_unlock(&g_fmu);
    } else vkb_subq_push(&q, &r->q);
}
static req *pop(void) {
    if (g_mode != 1) return (req *)vkb_subq_pop(&q);
    pthread_mutex_lock(&g_fmu);
    while (g_fh == g_ft) pthread_cond_wait(&g_fcv, &g_fmu);
    req *r = g_fifo[g_fh++ % RING];
    pthread_mutex_unlock(&g_fmu);
    return r;
}
static void *recorder(void *arg) {
    unsigned seed = (unsigned)(uintptr_t)arg * 7919u;
    for (;;) {
        uint64_t f = atomic_fetch_add(&g_fence, 1) + 1;   // mtl_sub_commit's ++g_fence
        if (f > SUBS) { atomic_fetch_sub(&g_fence, 1); break; }   // (no more: give the number back - only the last ones race here)
        if (f > SLOTS) wait_done(f - SLOTS);   // a free command slot (completion, so after its submit)
        seed = seed * 1103515245u + 12345u;
        req *r = calloc(1, sizeof *r);
        r->q.fence = f; r->skip = f < 50 && (seed >> 16) % 4 == 0; r->present = !r->skip && (seed >> 20) % 3 == 0;   // (a skip has no frame: mtl_present needs Vulkan)
        if ((seed >> 8) % 16 == 0) sched_yield();   // between ++fence and the push: another recorder's later fence goes first
        if (g_mode == 2 && r->present) { present_ready(f); r->present = 0; }
        if (g_mode == 3 && !r->skip)   // [UIT 0236] TOTK_VK_EARLY_STREAM: early parts of fence f, pushed before its last item
            for (int k = (int)((seed >> 24) % 4); k > 0; k--) {
                req *e = calloc(1, sizeof *e);
                e->q.fence = f; e->partial = 1;
                push(e);
                if ((seed >> 4) % 8 == 0) sched_yield();
            }
        push(r);
    }
    return NULL;
}
static void *submitter(void *unused) {
    (void)unused;
    uint64_t last_done = 0;
    for (uint64_t got = 0; got < SUBS; got++) {
        req *r = pop();
        uint64_t f = r->q.fence;
        if (r->partial) {   // no signal, no done: it must come after fence f-1's last item and before f's
            atomic_fetch_add(&g_parts, 1);
            if (last_done != f - 1 || atomic_load(&g_seen[f])) atomic_fetch_add(&g_part_bad, 1);
            free(r); got--;
            continue;
        }
        last_done = f;
        if (atomic_exchange(&g_seen[f], 1)) atomic_fetch_add(&g_seen_twice, 1);
        if (!r->skip) {
            if (f <= atomic_load(&g_gpu_sub)) atomic_fetch_add(&g_out_of_order, 1);   // a timeline value not above the last
            else {
                atomic_store(&g_gpu_sub, f);
                g_gpu_list[atomic_load(&g_gpu_tail) % RING] = f;
                atomic_fetch_add(&g_gpu_tail, 1);
            }
        } else complete(f);   // (as g_ok != 1: complete at once)
        if (g_mode != 1) vkb_subq_done(&q, f);
        if (r->present) present_ready(f);
        free(r);
    }
    atomic_store(&g_stop, 1);
    return NULL;
}
static void *gpu(void *unused) {
    (void)unused;
    for (;;) {
        uint64_t h = atomic_load(&g_gpu_head);
        if (h == atomic_load(&g_gpu_tail)) { if (atomic_load(&g_stop) && h == atomic_load(&g_gpu_tail)) break; sched_yield(); continue; }
        uint64_t f = g_gpu_list[h % RING];
        atomic_store(&g_gpu_head, h + 1);
        complete(f);   // the retire thread: done = fence
    }
    return NULL;
}
static void *waiter(void *unused) {
    (void)unused;
    while (!atomic_load(&g_stop)) { uint64_t f = atomic_load(&g_fence); if (f) wait_done(f > 3 ? f - 3 : f); }
    return NULL;
}
int main(int argc, char **argv) {
    g_mode = argc > 1 && !strcmp(argv[1], "noorder") ? 1 : argc > 1 && !strcmp(argv[1], "early") ? 2 : argc > 1 && !strcmp(argv[1], "parts") ? 3 : 0;
    pthread_t rec[RECORDERS], s, g, w;
    pthread_create(&s, NULL, submitter, NULL);
    pthread_create(&g, NULL, gpu, NULL);
    pthread_create(&w, NULL, waiter, NULL);
    for (int i = 0; i < RECORDERS; i++) pthread_create(&rec[i], NULL, recorder, (void *)(uintptr_t)(i + 1));
    struct timespec t0, t1; clock_gettime(CLOCK_MONOTONIC, &t0);
    for (int i = 0; i < RECORDERS; i++) pthread_join(rec[i], NULL);
    pthread_join(s, NULL); pthread_join(g, NULL);
    wait_done(1);   // (the waiter ends with g_stop)
    pthread_join(w, NULL);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    long missing = 0;
    for (uint64_t f = 1; f <= SUBS; f++) if (!g_seen[f]) missing++;
    printf("%s: %d submissions by %d recorders in %.2f s: out of fence order %ld, ready before submitted %ld, twice %ld, "
           "missing %ld, frames %ld, done %llu, queued at most %llu, early parts %ld (misplaced %ld)\n",
           g_mode == 1 ? "noorder" : g_mode == 2 ? "early" : g_mode == 3 ? "parts" : "protocol",
           SUBS, RECORDERS, (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9, (long)g_out_of_order,
           (long)g_ready_early, (long)g_seen_twice, missing, (long)g_presented, (unsigned long long)g_done,
           (unsigned long long)q.max_depth, (long)g_parts, (long)g_part_bad);
    int bad = g_out_of_order || g_ready_early || g_seen_twice || missing || g_part_bad || (g_mode == 3 && !g_parts);
    if (g_mode == 0 || g_mode == 3) { puts(bad ? "FAIL" : "PASS"); return bad; }
    puts(bad ? "PASS (the broken mode is caught)" : "FAIL (the broken mode was not caught)");
    return !bad;
}
