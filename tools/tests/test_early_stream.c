// [UIT 0236] Box test: TOTK_VK_EARLY_STREAM's threads, with the real src/gfx/vkb_early.h, vkb_early_gate.h and
// vkb_subq.h, as backend_vk.c vkb_mt_submit / mt_thread / early_try / submit_thread use them.
// Every mutex is an error-checking one, like a Windows SRW lock or critical section used without recursion: locking a
// mutex the thread holds, unlocking one it does not hold, or taking two in the wrong order (gate -> g_mt_mu -> g_pro_mu
// -> the submit queue -> the Vulkan queue lock) fails the test instead of passing on Linux's forgiving default mutex.
// A watchdog fails it if anything hangs.
// Threads:
//   translation: per submission, the walk cuts chunks one by one (recorders take them at once), then opens the gate,
//                early_try(1), records chunks itself (early_try(0) after each), waits for the rest (early_try(1) on
//                each wake), closes the gate, builds the last part (open segment + remaining chunks + the fence) and
//                takes the fence (++g_fence: it alone commits); mtl_sub_commit's check: early parts used that fence
//   recorders:   take a chunk, put prologue work into the segment open now (under g_pro_mu), mark it recorded, then
//                early_try(0) (try only)
//   submit:      vkb_subq_pop; an early part goes to the "GPU" list without a signal and without vkb_subq_done; the
//                last part signals its fence, vkb_subq_done
// Checked over the GPU list: per submission every chunk exactly once and in order, none before it was recorded, each
// chunk's prologue work in a segment in front of it, early parts before their submission's last part and after every
// earlier submission, signalled fences increasing, each early part has >= VKB_EARLY_MIN_DRAWS draws, at most
// STREAM_PARTS (VKB_EARLY_STREAM_MAX, or 14 with -DSTREAM_PARTS=14) parts, no early part after the gate closed, and no hang.
// Modes: (none) the protocol, with TOTK_VK_ASYNC_SUBMIT (parts queued); sync: parts submitted by the sending thread
// (TOTK_VK_ASYNC_SUBMIT=0); broken modes that must be caught: noprefix (a part takes recorded chunks past an
// unrecorded one), lifo (the queue puts an item before others of its fence: the old vkb_subq_push), nogate (recorders
// send without the gate), noseg (a part without the open prologue segment in front).
// cc -O2 -pthread -Isrc/gfx tools/tests/test_early_stream.c -o /tmp/t/estream && for m in "" sync noprefix lifo nogate noseg; do /tmp/t/estream $m || exit 1; done
// (also under -fsanitize=thread and -fsanitize=address,undefined: the protocol and sync modes)
#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

// ---- Windows-like locks: error-checking, with a lock order
static _Atomic long g_lock_errors;
static int g_quiet;   // the self-check at the start
static int lock_rank(pthread_mutex_t *m);
static __thread int t_held[8], t_nheld;
static void lock_note(pthread_mutex_t *m, int rc, const char *what) {
    if (rc) { atomic_fetch_add(&g_lock_errors, 1); if (!g_quiet) fprintf(stderr, "lock error: %s returned %d (%s)\n", what, rc, strerror(rc)); return; }
    int r = lock_rank(m);
    if (r && t_nheld && t_held[t_nheld - 1] >= r) {
        atomic_fetch_add(&g_lock_errors, 1);
        if (!g_quiet) fprintf(stderr, "lock order: rank %d taken while holding rank %d\n", r, t_held[t_nheld - 1]);
    }
    if (r && t_nheld < 8) t_held[t_nheld++] = r;
}
static int x_lock(pthread_mutex_t *m) { int rc = pthread_mutex_lock(m); lock_note(m, rc, "lock"); return rc; }
static int x_trylock(pthread_mutex_t *m) {
    int rc = pthread_mutex_trylock(m);
    if (rc == EBUSY) return rc;
    lock_note(m, rc, "trylock");
    return rc;
}
static int x_unlock(pthread_mutex_t *m) {
    int r = lock_rank(m);
    if (r) {   // released in reverse order of taking, or at least held
        int i = t_nheld - 1;
        while (i >= 0 && t_held[i] != r) i--;
        if (i < 0) { atomic_fetch_add(&g_lock_errors, 1); if (!g_quiet) fprintf(stderr, "unlock of rank %d not held\n", r); }
        else { memmove(t_held + i, t_held + i + 1, sizeof *t_held * (size_t)(t_nheld - i - 1)); t_nheld--; }
    }
    int rc = pthread_mutex_unlock(m);
    if (rc) { atomic_fetch_add(&g_lock_errors, 1); if (!g_quiet) fprintf(stderr, "unlock returned %d (%s)\n", rc, strerror(rc)); }
    return rc;
}
static int x_cond_wait(pthread_cond_t *c, pthread_mutex_t *m) {   // the mutex is given up and taken back
    int r = lock_rank(m);
    int rc = pthread_cond_wait(c, m);
    if (rc) { atomic_fetch_add(&g_lock_errors, 1); fprintf(stderr, "cond_wait returned %d\n", rc); }
    (void)r;
    return rc;
}
#define pthread_mutex_lock x_lock
#define pthread_mutex_trylock x_trylock
#define pthread_mutex_unlock x_unlock
#define pthread_cond_wait x_cond_wait
#include "vkb_early.h"
#ifndef STREAM_PARTS   // [UIT 0241] -DSTREAM_PARTS=14: the part limit with TOTK_VK_CHUNK_RAMP (vkb_ramp_parts)
#define STREAM_PARTS VKB_EARLY_STREAM_MAX
#endif
#include "vkb_early_gate.h"
#include "vkb_subq.h"

enum { SUBS = 3000, RECORDERS = 5, MAXCH = 96, MAXENT = 2 * MAXCH + 8, LOGN = 1 << 22 };
static int g_mode;   // 0 protocol, 1 sync, 2 noprefix, 3 lifo, 4 nogate, 5 noseg
static vkb_early_gate g_gate = VKB_EARLY_GATE_INIT;
static vkb_subq g_subq = VKB_SUBQ_INIT;
static pthread_mutex_t g_mt_mu = PTHREAD_MUTEX_INITIALIZER, g_pro_mu = PTHREAD_MUTEX_INITIALIZER, g_queue_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_mt_cv = PTHREAD_COND_INITIALIZER, g_done_cv = PTHREAD_COND_INITIALIZER;
static int lock_rank(pthread_mutex_t *m) {
    return m == &g_gate.mu ? 1 : m == &g_mt_mu ? 2 : m == &g_pro_mu ? 3 : m == &g_subq.mu ? 4 : m == &g_queue_mu ? 5 : 0;
}

// the submission being recorded (backend_vk.c g_ch, g_nch, g_mt_* )
static int g_sub;                           // its number (1..SUBS)
static int g_nch, g_avail, g_take, g_ndone; // chunks cut / cut and published / taken / recorded
static int g_walking;                       // the walk still cuts chunks
static unsigned char g_done[MAXCH];
static uint64_t g_draws[MAXCH];
static _Atomic int g_recorded[MAXCH];       // set when recording really ended (before mt_recorded)
static int g_need_seg[MAXCH];               // the newest prologue segment chunk k put work in
static int g_seg;                           // the open prologue segment (g_pro_mu)
static int g_early_next, g_early_sent;      // (g_gate)
static uint64_t g_early_fence;
static int g_sent_by_rec;
static uint64_t g_fence;                    // the translation thread's ++g_fence
static _Atomic int g_stop;

// the GPU list: entries in the order the "driver" got them
typedef struct { int sub, kind, id; uint64_t fence; } ent;   // kind 0 segment, 1 chunk, 2 fence signal, 3 part start
static ent *g_log; static long g_nlog;
typedef struct { vkb_subq_item q; int partial, n, sub; ent e[MAXENT]; } req;
static void gpu_append(const req *r) {   // vkQueueSubmit2 under the Vulkan queue lock
    pthread_mutex_lock(&g_queue_mu);
    g_log[g_nlog++] = (ent){r->sub, 3, r->partial, r->q.fence};
    for (int i = 0; i < r->n && g_nlog < LOGN; i++) g_log[g_nlog++] = r->e[i];
    pthread_mutex_unlock(&g_queue_mu);
}
static void push_lifo(vkb_subq *q, vkb_subq_item *it) {   // the old insertion: before items of the same fence
    pthread_mutex_lock(&q->mu);
    vkb_subq_item **p = &q->head;
    while (*p && (*p)->fence < it->fence) p = &(*p)->next;
    it->next = *p; *p = it;
    q->depth++;
    pthread_cond_broadcast(&q->cv);
    pthread_mutex_unlock(&q->mu);
}
static void submit(req *r) {
    if (g_mode == 3) push_lifo(&g_subq, &r->q); else vkb_subq_push(&g_subq, &r->q);
}

static uint64_t rng_next(uint64_t *s) { *s ^= *s << 13; *s ^= *s >> 7; *s ^= *s << 17; return *s; }
static __thread uint64_t t_rng;
static __thread int t_worker;
static _Atomic long g_bad_send;   // checks done at send time

static void early_try(int wait) {
    int gated = !(g_mode == 4 && t_worker > 0);
    if (gated && !vkb_early_enter(&g_gate, wait)) return;
    if (!gated && !g_gate.open) return;
    if (g_early_sent >= STREAM_PARTS - 1) { if (gated) vkb_early_leave(&g_gate); return; }
    unsigned char done[MAXCH]; uint64_t draws[MAXCH];
    pthread_mutex_lock(&g_mt_mu);
    int nch = g_nch, sub = g_sub;
    for (int k = 0; k < nch; k++) { done[k] = g_done[k]; draws[k] = g_draws[k]; }
    pthread_mutex_unlock(&g_mt_mu);
    int next = g_early_next;
    int to = vkb_early_stream_plan(next, nch, done, draws, STREAM_PARTS, g_early_sent, VKB_EARLY_MIN_DRAWS);
    if (g_mode == 2 && to == next) {   // noprefix: also past an unrecorded chunk (any recorded one further on)
        uint64_t cum = 0; int p = next;
        for (int k = next; k < nch - 1; k++) if (done[k]) { cum += draws[k]; p = k + 1; }
        if (cum >= VKB_EARLY_MIN_DRAWS) to = p;
    }
    if (to <= next) { if (gated) vkb_early_leave(&g_gate); return; }
    req *r = calloc(1, sizeof *r);
    r->partial = 1; r->sub = sub;
    uint64_t cum = 0;
    pthread_mutex_lock(&g_pro_mu);
    if (g_mode != 5) { r->e[r->n++] = (ent){sub, 0, g_seg, 0}; g_seg++; }   // noseg: the open segment is not sent in front
    for (int k = next; k < to; k++) {   // (fence: the newest segment the chunk put work in, + 1)
        r->e[r->n++] = (ent){sub, 1, atomic_load(&g_recorded[k]) ? k : -1 - k, (uint64_t)(g_need_seg[k] + 1)};
        cum += draws[k];
    }
    pthread_mutex_unlock(&g_pro_mu);
    if (cum < VKB_EARLY_MIN_DRAWS) atomic_fetch_add(&g_bad_send, 1);
    g_early_fence = g_fence + 1;   // mtl_last_fence() + 1
    r->q.fence = g_early_fence;
    if (g_mode == 1) { gpu_append(r); free(r); }   // sync: every earlier submission is on the queue already
    else submit(r);
    g_early_next = to; g_early_sent++;
    if (t_worker > 0) g_sent_by_rec++;
    if (gated) vkb_early_leave(&g_gate);
}

static int mt_take(void) {   // the next chunk to record, -1 when the walk is over and every chunk is taken
    pthread_mutex_lock(&g_mt_mu);
    int k = g_take < g_avail ? g_take++ : -1;
    pthread_mutex_unlock(&g_mt_mu);
    return k;
}
static void mt_record(int k) {
    int works = (int)(rng_next(&t_rng) % 4);
    for (int w = 0; w < works; w++) {   // prologue work: into the segment open now
        if (rng_next(&t_rng) % 4 == 0) sched_yield();
        pthread_mutex_lock(&g_pro_mu);
        g_need_seg[k] = g_seg;
        pthread_mutex_unlock(&g_pro_mu);
    }
    if (rng_next(&t_rng) % 3 == 0) sched_yield();
    atomic_store(&g_recorded[k], 1);
}
static void mt_recorded(int k) {
    pthread_mutex_lock(&g_mt_mu);
    g_done[k] = 1; g_ndone++;
    pthread_cond_broadcast(&g_done_cv);
    pthread_mutex_unlock(&g_mt_mu);
}
static void *recorder(void *arg) {
    t_worker = (int)(intptr_t)arg; t_rng = 0x9E3779B97F4A7C15ull * (uint64_t)(t_worker + 1);
    for (;;) {
        pthread_mutex_lock(&g_mt_mu);
        while (!atomic_load(&g_stop) && g_take >= g_avail) pthread_cond_wait(&g_mt_cv, &g_mt_mu);
        if (atomic_load(&g_stop)) { pthread_mutex_unlock(&g_mt_mu); break; }
        int k = g_take++;
        pthread_mutex_unlock(&g_mt_mu);
        mt_record(k);
        mt_recorded(k);
        early_try(0);
    }
    return NULL;
}
static long g_parts_total, g_rec_total;
static void *translation(void *unused) {
    (void)unused;
    t_worker = 0; t_rng = 0x12345678abcdefull;
    for (int s = 1; s <= SUBS; s++) {
        int nch = 1 + (int)(rng_next(&t_rng) % (s % 7 == 0 ? 4 : MAXCH));
        pthread_mutex_lock(&g_mt_mu);   // a new submission (no thread records: the last one waited for all)
        g_sub = s; g_nch = g_avail = g_take = g_ndone = 0; g_walking = 1;
        pthread_mutex_unlock(&g_mt_mu);
        pthread_mutex_lock(&g_pro_mu); g_seg = 0; pthread_mutex_unlock(&g_pro_mu);
        g_early_next = g_early_sent = 0; g_sent_by_rec = 0;
        for (int k = 0; k < nch; k++) {   // the walk: chunk_open / chunk_close; recorders start at once
            atomic_store(&g_recorded[k], 0);
            g_need_seg[k] = -1;
            pthread_mutex_lock(&g_mt_mu);
            g_done[k] = 0;
            g_draws[k] = rng_next(&t_rng) % 700;
            g_avail = g_nch = k + 1;
            pthread_cond_broadcast(&g_mt_cv);
            pthread_mutex_unlock(&g_mt_mu);
            if (rng_next(&t_rng) % 8 == 0) sched_yield();
        }
        vkb_early_set_open(&g_gate, 1);
        early_try(1);
        for (int k; (k = mt_take()) >= 0;) { mt_record(k); mt_recorded(k); early_try(0); }
        pthread_mutex_lock(&g_mt_mu);
        while (g_ndone < g_nch) {
            pthread_cond_wait(&g_done_cv, &g_mt_mu);
            if (g_ndone < g_nch) { pthread_mutex_unlock(&g_mt_mu); early_try(1); pthread_mutex_lock(&g_mt_mu); }
        }
        pthread_mutex_unlock(&g_mt_mu);
        vkb_early_set_open(&g_gate, 0);
        int next_at_close = g_early_next;
        // the last part: the open segment, the remaining chunks, the fence (mtl_sub_commit)
        req *r = calloc(1, sizeof *r);
        r->sub = s;
        pthread_mutex_lock(&g_pro_mu);
        r->e[r->n++] = (ent){s, 0, g_seg, 0};
        pthread_mutex_unlock(&g_pro_mu);
        for (int k = g_early_next; k < nch; k++) r->e[r->n++] = (ent){s, 1, atomic_load(&g_recorded[k]) ? k : -1 - k, (uint64_t)(g_need_seg[k] + 1)};
        uint64_t fence = ++g_fence;
        if (g_early_sent && g_early_fence != fence) atomic_fetch_add(&g_bad_send, 1);   // mtl_sub_commit's check
        r->e[r->n++] = (ent){s, 2, 0, fence};
        r->q.fence = fence;
        if (g_mode == 4) sched_yield();
        if (g_early_next != next_at_close) atomic_fetch_add(&g_bad_send, 1);   // a part after the gate closed
        g_parts_total += g_early_sent; g_rec_total += g_sent_by_rec;
        if (g_mode == 1) { gpu_append(r); free(r); vkb_subq_done(&g_subq, fence); }
        else submit(r);
    }
    vkb_subq_wait_submitted(&g_subq, g_fence);
    return NULL;
}
static void *submitter(void *unused) {
    (void)unused;
    for (;;) {
        req *r = (req *)vkb_subq_pop(&g_subq);
        if (!r) break;
        uint64_t f = r->q.fence;
        int last = !r->partial;
        gpu_append(r);
        free(r);
        if (last) vkb_subq_done(&g_subq, f);
        if (last && f >= (uint64_t)SUBS) break;
    }
    return NULL;
}
static void watchdog(int sig) {   // a hang: the protocol fails; a broken mode is caught
    (void)sig;
    static const char f[] = "FAIL: hang (watchdog)\n", c[] = "hang (watchdog)\nPASS (the broken mode is caught)\n";
    if (g_mode >= 2) { if (write(1, c, sizeof c - 1)) {} _exit(0); }
    if (write(1, f, sizeof f - 1)) {}
    _exit(2);
}

int main(int argc, char **argv) {
    const char *m = argc > 1 ? argv[1] : "";
    g_mode = !strcmp(m, "sync") ? 1 : !strcmp(m, "noprefix") ? 2 : !strcmp(m, "lifo") ? 3 : !strcmp(m, "nogate") ? 4 : !strcmp(m, "noseg") ? 5 : 0;
    pthread_mutexattr_t a; pthread_mutexattr_init(&a); pthread_mutexattr_settype(&a, PTHREAD_MUTEX_ERRORCHECK);
    pthread_mutex_t *all[] = {&g_gate.mu, &g_subq.mu, &g_mt_mu, &g_pro_mu, &g_queue_mu};
    for (unsigned i = 0; i < sizeof all / sizeof *all; i++) { pthread_mutex_destroy(all[i]); pthread_mutex_init(all[i], &a); }
#if defined(__has_feature)
#if __has_feature(thread_sanitizer)
#define NO_SELF_CHECK   // (ThreadSanitizer reports the deliberate errors itself)
#endif
#endif
#ifndef NO_SELF_CHECK
    {   // the lock checks themselves: a lock taken twice (EDEADLK), the wrong order, an unlock not held (EPERM)
        g_quiet = 1;
        pthread_mutex_lock(&g_mt_mu); pthread_mutex_lock(&g_mt_mu); pthread_mutex_unlock(&g_mt_mu);
        pthread_mutex_lock(&g_pro_mu); pthread_mutex_lock(&g_mt_mu); pthread_mutex_unlock(&g_mt_mu); pthread_mutex_unlock(&g_pro_mu);
        pthread_mutex_unlock(&g_queue_mu);
        g_quiet = 0;
        long e = atomic_exchange(&g_lock_errors, 0);
        if (e < 4) { printf("FAIL: the lock checks caught %ld of 4 errors\n", e); return 1; }
    }
#endif
    g_log = calloc(LOGN, sizeof *g_log);
    signal(SIGALRM, watchdog); alarm(g_mode >= 2 ? 15 : 120);   // (the protocol runs in ~1 min on a slow box)
    pthread_t tr, sb, rec[RECORDERS];
    if (g_mode != 1) pthread_create(&sb, NULL, submitter, NULL);
    for (int i = 0; i < RECORDERS; i++) pthread_create(&rec[i], NULL, recorder, (void *)(intptr_t)(i + 1));
    pthread_create(&tr, NULL, translation, NULL);
    pthread_join(tr, NULL);
    if (g_mode != 1) pthread_join(sb, NULL);
    pthread_mutex_lock(&g_mt_mu); atomic_store(&g_stop, 1); pthread_cond_broadcast(&g_mt_cv); pthread_mutex_unlock(&g_mt_mu);
    for (int i = 0; i < RECORDERS; i++) pthread_join(rec[i], NULL);
    alarm(0);
    // the checks over the GPU list
    long bad_order = 0, unrecorded = 0, seg_missing = 0, fence_bad = 0, part_after = 0, missing = 0, toomany = 0;
    int cur = 0, last_chunk = -1, parts = 0; uint64_t last_sig = 0;
    static long seg_pos[MAXCH * 2 + 8];
    for (long i = 0; i < g_nlog; i++) {
        ent e = g_log[i];
        if (e.sub != cur) {   // a new submission starts in the list
            if (cur && last_chunk != -2) missing++;
            if (e.sub != cur + 1) bad_order++;
            cur = e.sub; last_chunk = -1; parts = 0;
            for (unsigned j = 0; j < sizeof seg_pos / sizeof *seg_pos; j++) seg_pos[j] = -1;
        }
        if (last_chunk == -2) { part_after++; continue; }   // anything of this submission after its fence
        if (e.kind == 3) { if (e.id && ++parts > STREAM_PARTS - 1) toomany++; }
        else if (e.kind == 0) { if (e.id < (int)(sizeof seg_pos / sizeof *seg_pos)) seg_pos[e.id] = i; }
        else if (e.kind == 1) {
            int k = e.id < 0 ? -1 - e.id : e.id;
            if (e.id < 0) unrecorded++;
            if (k != last_chunk + 1) bad_order++;
            int ns = (int)e.fence - 1;   // its prologue work: that segment must be in front of it
            if (ns >= 0 && (ns >= (int)(sizeof seg_pos / sizeof *seg_pos) || seg_pos[ns] < 0)) seg_missing++;
            last_chunk = k;
        } else {
            if (e.fence <= last_sig) fence_bad++;
            last_sig = e.fence;
            last_chunk = -2;
        }
    }
    if (cur != SUBS || last_chunk != -2) missing++;
    long bad = seg_missing + bad_order + unrecorded + fence_bad + part_after + missing + toomany + atomic_load(&g_bad_send) + atomic_load(&g_lock_errors);
    printf("%s: %d submissions, %.2f early parts each (%.2f sent by recording threads): out of order %ld, unrecorded %ld, "
           "prologue not in front %ld, fence order %ld, after the fence %ld, missing %ld, too many parts %ld, bad sends %ld, lock errors %ld\n",
           *m ? m : "protocol", SUBS, g_parts_total / (double)SUBS, g_rec_total / (double)SUBS, bad_order, unrecorded, seg_missing, fence_bad,
           part_after, missing, toomany, (long)atomic_load(&g_bad_send), (long)atomic_load(&g_lock_errors));
    if (g_mode <= 1) {
        int ok = !bad && g_parts_total > SUBS / 2;   // (the stream does send parts)
        puts(ok ? "PASS" : "FAIL");
        return !ok;
    }
    puts(bad ? "PASS (the broken mode is caught)" : "FAIL (the broken mode was not caught)");
    return !bad;
}
