// [UIT native] vkb_shqueue.h: the shader background threads' job queue (order, limits, blocking pop with several
// threads) and the "handed over again" hint of TOTK_SHADER_EARLY.
//   cc -O2 -Wall -pthread -Isrc/gfx tools/tests/test_shqueue.c -o /tmp/test_shqueue && /tmp/test_shqueue
#include <assert.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include "vkb_shqueue.h"

typedef struct { shq_job q; int v; } job;
static shq g_q = SHQ_INIT(1u << 20, 1u << 30);
static _Atomic long g_sum, g_cnt;
enum { NPROD = 4, NCONS = 3, PER = 20000 };
static void *prod(void *a) {
    int base = (int)(intptr_t)a * PER;
    for (int i = 0; i < PER; i++) {
        job *j = malloc(sizeof *j);
        j->q.bytes = 1 + (size_t)(i & 7); j->v = base + i;
        while (!shq_push(&g_q, &j->q)) ;
    }
    return NULL;
}
static void *cons(void *a) {
    (void)a;
    for (;;) {
        job *j = (job *)shq_pop(&g_q);
        if (j->v < 0) { free(j); return NULL; }
        size_t b = j->q.bytes;
        atomic_fetch_add(&g_sum, j->v); atomic_fetch_add(&g_cnt, 1);
        free(j);
        shq_done(&g_q, b);
    }
}
int main(void) {
    // order and limits
    shq q = SHQ_INIT(3, 100);
    job a = {{0, 40}, 1}, b = {{0, 40}, 2}, c = {{0, 40}, 3}, d = {{0, 10}, 4}, e = {{0, 10}, 5};
    assert(shq_push(&q, &a.q) && shq_push(&q, &b.q));
    assert(!shq_push(&q, &c.q));          // 120 bytes > 100
    assert(shq_push(&q, &d.q));           // 90
    assert(!shq_push(&q, &e.q));          // a 4th job > 3
    assert(((job *)shq_pop(&q))->v == 1);
    assert(!shq_push(&q, &c.q));          // a's bytes still counted until done
    shq_done(&q, 40);
    assert(shq_push(&q, &c.q));
    assert(((job *)shq_pop(&q))->v == 2 && ((job *)shq_pop(&q))->v == 4 && ((job *)shq_pop(&q))->v == 3);
    assert(q.n == 0 && q.head == NULL && q.tail == NULL);
    // several producers and consumers: every job once
    pthread_t t[NPROD + NCONS];
    for (int i = 0; i < NCONS; i++) pthread_create(&t[NPROD + i], NULL, cons, NULL);
    for (int i = 0; i < NPROD; i++) pthread_create(&t[i], NULL, prod, (void *)(intptr_t)i);
    for (int i = 0; i < NPROD; i++) pthread_join(t[i], NULL);
    for (int i = 0; i < NCONS; i++) { job *s = malloc(sizeof *s); s->q.bytes = 0; s->v = -1; while (!shq_push(&g_q, &s->q)) ; }
    for (int i = 0; i < NCONS; i++) pthread_join(t[NPROD + i], NULL);
    long n = (long)NPROD * PER;
    assert(g_cnt == n && g_sum == n * (n - 1) / 2);
    assert(g_q.bytes == 0 && g_q.n == 0);
    // the hint
    static _Atomic uint64_t seen[4096];
    uint64_t t1 = shq_tag(0x1000, 0x2000, 512), t2 = shq_tag(0x1000, 0x2000, 513), t3 = shq_tag(0x1040, 0x2000, 512);
    assert(t1 != t2 && t1 != t3 && shq_tag(0, 0, 0) != 0);
    assert(!shq_seen(seen, t1) && shq_seen(seen, t1) && !shq_seen(seen, t2));
    printf("test_shqueue: ok (%ld jobs over %d threads)\n", n, NCONS);
    return 0;
}
