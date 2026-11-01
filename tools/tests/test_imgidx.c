// [UIT 0230] Box test for vkb_imgidx.h (TOTK_VK_IMAGE_INDEX): the indexed image scans against the old walks of the
// image list.
//   cc -O2 -Wall -pthread -Isrc/gfx tools/tests/test_imgidx.c -o /tmp/test_imgidx && /tmp/test_imgidx [quick | bench]
// 1. Differential: two copies of the same image set (list walk / index) take the same random sequence of images made,
//    dropped, drawn (pending), copied back (unpended), and the vkb_tex.c scans (range scan of a first upload or an
//    invalidation, pending scans of buffer reads / copy-back checks, the copy-back picks and their superseding
//    unpends during the scan): every scan must see the same images in the same order, and the states must stay equal.
// 2. Cost: one scan, list walk vs index, over 2,459 and 4,002 images of ~1.3 KB each (the 2026-10-31 log's standing
//    and running counts), 14 / 48 pending, warm and with the caches cleared between scans.
// 3. Lock replay: six threads take the lock for short lookups while one thread runs the scans; the lookups' wait.
// Exit 0 = pass.
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "vkb_imgidx.h"

typedef struct ent {
    uint64_t storage, size;
    int gpu_written, wb_pending, alias, refs;
    uint64_t seq, write_seq;
    int ix_all, ix_pend;
    struct ent *all_next, **all_pprev;
    unsigned char pad[1240];   // the rest of a vkb_tex.c entry (image, 32 views and their keys)
    int id;
} ent;

typedef struct {
    ent *all;
    vkb_ix ia, ip;
    uint64_t seqc;
    int use_ix;
    int ranged;   // [UIT 0254] TOTK_VK_BUFREAD_RANGE: the pending index keeps ranges, scans over a range ask for it
    void **q; int qcap;
    ent **byid; int nid;
} world;

static int g_negative;   // TOTK_PROBE_NEGATIVE=1: the ranged index keeps each pending range one page short; must FAIL
static uint64_t rs = 88172645463325252ull;
static uint64_t rnd(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return rs; }
static uint64_t now_ns(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec; }

static void w_make(world *w, int id, uint64_t storage, uint64_t size) {
    ent *e = calloc(1, sizeof *e);
    e->id = id; e->storage = storage; e->size = size; e->refs = 1;
    e->all_next = w->all; e->all_pprev = &w->all;
    if (w->all) w->all->all_pprev = &e->all_next;
    w->all = e;
    e->seq = ++w->seqc;
    if (storage) vkb_ix_add(&w->ia, e, e->seq, storage, storage + size, &e->ix_all);
    w->byid[id] = e;
}
static void w_pend(world *w, ent *e) {   // vkb_tex.c iwb_stamp -> ix_pend_add
    if (e->wb_pending) return;
    e->wb_pending = 1;
    uint64_t lo = 0, hi = 0;
    if (w->ranged) { lo = e->storage; hi = lo + e->size; if (hi < lo) hi = UINT64_MAX; if (g_negative && e->size > 0x2000) hi -= 0x1000; }
    vkb_ix_add(&w->ip, e, e->seq, lo, hi, &e->ix_pend);
}
// vkb_tex.c pend_q: the query range of a scan for pending images over [lo, hi)
static void pend_q(const world *w, uint64_t lo, uint64_t hi, uint64_t *qlo, uint64_t *qhi) {
    if (w->ranged && hi > lo) *qlo = lo, *qhi = hi; else *qlo = *qhi = 0;
}
static void w_unpend(world *w, ent *e) { e->wb_pending = 0; if (e->ix_pend) vkb_ix_del(&w->ip, &e->ix_pend); }
static void w_drop(world *w, ent *e) {
    *e->all_pprev = e->all_next;
    if (e->all_next) e->all_next->all_pprev = e->all_pprev;
    if (e->ix_all) vkb_ix_del(&w->ia, &e->ix_all);
    w_unpend(w, e);
    w->byid[e->id] = NULL;
    free(e);
}
// The iteration of vkb_tex.c (it_first / it_next)
typedef struct { int n, i; ent *cur; } it_t;
static ent *it_first(world *w, it_t *it, vkb_ix *x, uint64_t lo, uint64_t hi) {
    it->i = 0;
    it->n = w->use_ix ? vkb_ix_query(x, lo, hi, &w->q, &w->qcap) : -1;
    if (w->use_ix && it->n < 0) { fprintf(stderr, "out of memory\n"); exit(2); }
    return it->cur = it->n >= 0 ? (it->n ? (ent *)w->q[0] : NULL) : w->all;
}
static ent *it_next(world *w, it_t *it) { return it->cur = it->n >= 0 ? (++it->i < it->n ? (ent *)w->q[it->i] : NULL) : it->cur->all_next; }
static int over(const ent *e, uint64_t lo, uint64_t hi) { return e->storage < lo + (hi - lo) && lo < e->storage + e->size; }

// The scans: each writes the ids it met (passing its test) to out, returns how many
static int scan_upload(world *w, ent *im, int *out) {   // tex_upload's first upload
    int n = 0; it_t it;
    for (ent *e = it_first(w, &it, &w->ia, im->storage, im->storage + im->size); e; e = it_next(w, &it)) {
        if (e == im || !e->gpu_written || !e->storage) continue;
        if (!(e->storage < im->storage + im->size && im->storage < e->storage + e->size)) continue;
        e->alias = 1; out[n++] = e->id;
    }
    return n;
}
static int scan_inval(world *w, uint64_t lo, uint64_t hi, int *out) {   // vkb_iwb_note_inval
    int n = 0; it_t it;
    for (ent *e = it_first(w, &it, &w->ia, lo, hi); e; e = it_next(w, &it))
        if ((e->gpu_written || e->wb_pending) && e->storage && over(e, lo, hi)) out[n++] = e->id;
    return n;
}
static int scan_pend(world *w, uint64_t lo, uint64_t hi, int *out) {   // vkb_iwb_buf_read / vkb_iwb_wanted
    int n = 0; it_t it; uint64_t qlo, qhi; pend_q(w, lo, hi, &qlo, &qhi);
    for (ent *e = it_first(w, &it, &w->ip, qlo, qhi); e; e = it_next(w, &it))
        if (e->wb_pending && over(e, lo, hi)) out[n++] = e->id;
    return n;
}
static int scan_record(world *w, int *out) {   // iwb_record: extra, picks, superseding unpends during the scan
    int n = 0, extra = 0, np = 0; it_t it; ent *pick[16];
    for (ent *e = it_first(w, &it, &w->ip, 0, 0); e; e = it_next(w, &it)) extra += e->wb_pending && e->alias && e->refs > 0;
    for (ent *e = it_first(w, &it, &w->ip, 0, 0); e && np < 4 + (extra & 7); e = it_next(w, &it))
        if (e->wb_pending && e->alias && e->refs > 0 && np < 16) { pick[np++] = e; out[n++] = e->id; }
    if (np)
        for (ent *e = it_first(w, &it, &w->ip, 0, 0); e; e = it_next(w, &it)) {
            if (!e->wb_pending) continue;
            for (int i = 0; i < np; i++)
                if (e != pick[i] && e->write_seq < pick[i]->write_seq && e->storage < pick[i]->storage + pick[i]->size &&
                    pick[i]->storage < e->storage + e->size) { w_unpend(w, e); out[n++] = 100000 + e->id; break; }
        }
    for (int i = 0; i < np; i++) w_unpend(w, pick[i]);
    out[n++] = -extra;
    return n;
}
static int scan_explicit(world *w, uint64_t lo, uint64_t hi, int *out) {   // iwb_record why=1: unpends in the scan
    int n = 0; it_t it; uint64_t qlo, qhi; pend_q(w, lo, hi, &qlo, &qhi);
    for (ent *e = it_first(w, &it, &w->ip, qlo, qhi); e; e = it_next(w, &it)) {
        if (!e->wb_pending || !over(e, lo, hi)) continue;
        out[n++] = e->id;
        if (n & 1) w_unpend(w, e);
    }
    return n;
}

static int same_state(world *a, world *b) {
    ent *x = a->all, *y = b->all;
    for (; x && y; x = x->all_next, y = y->all_next)
        if (x->id != y->id || x->wb_pending != y->wb_pending || x->gpu_written != y->gpu_written || x->alias != y->alias) return 0;
    if (x || y) return 0;
    int np = 0;
    for (ent *e = b->all; e; e = e->all_next) {
        np += e->wb_pending;
        if (!!e->wb_pending != !!e->ix_pend || !!e->storage != !!e->ix_all) return 0;
        if (e->ix_all && b->ia.obj[e->ix_all - 1] != e) return 0;
        if (e->ix_pend && b->ip.obj[e->ix_pend - 1] != e) return 0;
    }
    return np == b->ip.n;
}

static int differential(int steps) {
    enum { MAXID = 1 << 20 };
    world A = {0}, B = {0}, C = {0};
    A.byid = calloc(MAXID, sizeof *A.byid); B.byid = calloc(MAXID, sizeof *B.byid); B.use_ix = 1;
    C.byid = calloc(MAXID, sizeof *C.byid); C.use_ix = 1; C.ranged = 1;   // [UIT 0254]
    static int oa[1 << 16], ob[1 << 16], oc[1 << 16];
    int nid = 0, live = 0;
    long scans = 0, met = 0;
    const uint64_t BASE = 0x10000000, SPAN = 64ull << 20;
    for (int s = 0; s < steps; s++) {
        int op = (int)(rnd() % 100);
        if (op < 22 || live < 50) {   // make (some with no memory, some 0-size, some sharing an address)
            if (nid >= MAXID) break;
            uint64_t st = rnd() % 10 == 0 ? 0 : BASE + (rnd() % SPAN & ~0xfffull), sz = rnd() % 25 == 0 ? 0 : 0x1000 + rnd() % (1 << 20);
            if (rnd() % 8 == 0 && live) { ent *o = NULL; for (int k = 0; k < 8 && !o; k++) o = A.byid[rnd() % (uint64_t)nid]; if (o) st = o->storage; }
            w_make(&A, nid, st, sz); w_make(&B, nid, st, sz); w_make(&C, nid, st, sz); nid++; live++;
        } else if (op < 36 && live > 50) {   // drop
            ent *a = NULL;
            for (int k = 0; k < 64 && !a; k++) a = A.byid[rnd() % (uint64_t)nid];
            if (a) { int id = a->id; w_drop(&A, a); w_drop(&B, B.byid[id]); w_drop(&C, C.byid[id]); live--; }
        } else if (op < 52) {   // drawn: gpu_written, sometimes pending (write order)
            ent *a = NULL;
            for (int k = 0; k < 64 && !a; k++) a = A.byid[rnd() % (uint64_t)nid];
            if (a) {
                ent *b = B.byid[a->id], *c = C.byid[a->id];
                a->gpu_written = b->gpu_written = c->gpu_written = 1; a->write_seq = b->write_seq = c->write_seq = (uint64_t)s;
                if (rnd() % 3) { w_pend(&A, a); w_pend(&B, b); w_pend(&C, c); }
                if (rnd() % 4 == 0) a->alias = b->alias = c->alias = 1;
                if (rnd() % 10 == 0) a->refs = b->refs = c->refs = 0;
            }
        } else if (op < 58) {   // copied back
            ent *a = NULL;
            for (int k = 0; k < 64 && !a; k++) a = A.byid[rnd() % (uint64_t)nid];
            if (a) { w_unpend(&A, a); w_unpend(&B, B.byid[a->id]); w_unpend(&C, C.byid[a->id]); }
        } else {
            int na, nb, nc, kind = op % 5;
            uint64_t lo = BASE + rnd() % SPAN, hi = lo + (rnd() % 7 == 0 ? 0 : rnd() % (2 << 20));
            if (rnd() % 50 == 0) hi = lo - 1;   // empty / reversed ranges: every entry is a candidate
            if (kind == 0) {
                ent *a = NULL;
                for (int k = 0; k < 64 && !a; k++) a = A.byid[rnd() % (uint64_t)nid];
                if (!a) continue;
                na = scan_upload(&A, a, oa); nb = scan_upload(&B, B.byid[a->id], ob); nc = scan_upload(&C, C.byid[a->id], oc);
            } else if (kind == 1) { na = scan_inval(&A, lo, hi, oa); nb = scan_inval(&B, lo, hi, ob); nc = scan_inval(&C, lo, hi, oc); }
            else if (kind == 2) { na = scan_pend(&A, lo, hi, oa); nb = scan_pend(&B, lo, hi, ob); nc = scan_pend(&C, lo, hi, oc); }
            else if (kind == 3) { na = scan_record(&A, oa); nb = scan_record(&B, ob); nc = scan_record(&C, oc); }
            else { na = scan_explicit(&A, lo, hi, oa); nb = scan_explicit(&B, lo, hi, ob); nc = scan_explicit(&C, lo, hi, oc); }
            scans++; met += na;
            if (na != nb || memcmp(oa, ob, sizeof oa[0] * (size_t)na)) {
                printf("FAIL step %d scan kind %d: walk met %d, index %d\n", s, kind, na, nb);
                return 1;
            }
            if (na != nc || memcmp(oa, oc, sizeof oa[0] * (size_t)na)) {
                printf("FAIL step %d scan kind %d: walk met %d, ranged index (0254) %d\n", s, kind, na, nc);
                return 1;
            }
        }
        if (s % 997 == 0 && (!same_state(&A, &B) || !same_state(&A, &C))) { printf("FAIL step %d: states differ\n", s); return 1; }
    }
    if (!same_state(&A, &B) || !same_state(&A, &C)) { printf("FAIL: final states differ\n"); return 1; }
    printf("differential: %d steps, %ld scans (%ld images met), %d images alive at the end, %d pending: same images, same order, same state (list walk, index, ranged index)\n",
           steps, scans, met, live, B.ip.n);
    return 0;
}

// ---- cost ----
static unsigned char *g_junk; static size_t g_junk_n = 32u << 20;
static void flush(void) { for (size_t i = 0; i < g_junk_n; i += 64) g_junk[i]++; }
static void build(world *w, int n, int npend, int use_ix) {
    memset(w, 0, sizeof *w);
    w->byid = calloc((size_t)n, sizeof *w->byid); w->use_ix = use_ix != 0; w->ranged = use_ix == 2;
    void **hole = calloc((size_t)n, sizeof *hole);
    rs = 12345;
    for (int i = 0; i < n; i++) {
        hole[i] = malloc(64 + rnd() % 4000);   // other allocations in between, as in the game
        w_make(w, i, 0x10000000 + (uint64_t)i * 0x40000, 0x1000 + rnd() % 0x3f000);
        w->byid[i]->gpu_written = rnd() % 3 == 0;
    }
    for (int k = 0; k < npend; k++) { ent *e = w->byid[rnd() % (uint64_t)n]; e->gpu_written = 1; w_pend(w, e); }
    for (int i = 0; i < n; i += 2) free(hole[i]);
    free(hole);
}
static volatile int g_sink;
static double cost_ns(world *w, int kind, int cold, int reps) {
    static int out[1 << 16];
    uint64_t tot = 0;
    for (int r = 0; r < reps; r++) {
        if (cold) flush();
        ent *im = w->byid[(r * 7919) % w->nid];
        uint64_t lo = im->storage, hi = lo + 0x8000;
        uint64_t t = now_ns();
        int n = kind == 0 ? scan_upload(w, im, out) : kind == 1 ? scan_pend(w, lo, hi, out) : scan_inval(w, lo, hi, out);
        tot += now_ns() - t; g_sink += n;
    }
    return (double)tot / reps;
}

// ---- lock replay ----
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static _Atomic int g_stop;   // [UIT 0235] was volatile: a data race under TSan (harness only)
static world *g_w;
static int g_scans_per_ms;
typedef struct { uint64_t wait_ns, waits, n; } lstat;
static void *looker(void *p) {   // a recording thread: short lookups under the lock (vkb_image_of hits), work between
    lstat *st = p; volatile uint64_t x = 0;
    while (!g_stop) {
        uint64_t t = now_ns();
        if (pthread_mutex_trylock(&g_mu)) { pthread_mutex_lock(&g_mu); st->wait_ns += now_ns() - t; st->waits++; }
        for (int i = 0; i < 40; i++) x += (uint64_t)i;   // ~0.1 us held
        pthread_mutex_unlock(&g_mu);
        st->n++;
        for (int i = 0; i < 20000; i++) x += (uint64_t)i;   // recording work between lookups (a few us)
    }
    return NULL;
}
static void *scanner(void *p) {   // the scans, under the same lock, at a fixed rate
    static int out[1 << 16]; (void)p; int r = 0;
    while (!g_stop) {
        uint64_t t0 = now_ns();
        for (int k = 0; k < g_scans_per_ms && !g_stop; k++) {
            pthread_mutex_lock(&g_mu);
            ent *im = g_w->byid[(r++ * 7919) % g_w->nid];
            g_sink += (k & 1) ? scan_upload(g_w, im, out) : scan_pend(g_w, im->storage, im->storage + 0x8000, out);
            pthread_mutex_unlock(&g_mu);
        }
        while (now_ns() - t0 < 1000000 && !g_stop) { struct timespec s = {0, 50000}; nanosleep(&s, NULL); }
    }
    return NULL;
}
static void replay(world *w, int scans_per_ms, double secs, double *wait_ms_per_s, double *per_wait_us, double *lookups_per_s) {
    g_w = w; g_scans_per_ms = scans_per_ms; g_stop = 0;
    pthread_t th[7]; lstat st[6]; memset(st, 0, sizeof st);
    for (int i = 0; i < 6; i++) pthread_create(&th[i], NULL, looker, &st[i]);
    pthread_create(&th[6], NULL, scanner, NULL);
    struct timespec s = {(time_t)secs, (long)((secs - (int)secs) * 1e9)}; nanosleep(&s, NULL);
    g_stop = 1;
    for (int i = 0; i < 7; i++) pthread_join(th[i], NULL);
    uint64_t wn = 0, wc = 0, n = 0;
    for (int i = 0; i < 6; i++) { wn += st[i].wait_ns; wc += st[i].waits; n += st[i].n; }
    *wait_ms_per_s = wn / 1e6 / secs; *per_wait_us = wc ? wn / 1e3 / wc : 0; *lookups_per_s = n / secs;
}

int main(int argc, char **argv) {
    int quick = argc > 1 && !strcmp(argv[1], "quick"), bench = argc > 1 && !strcmp(argv[1], "bench");   // bench: cost and replay only
    const char *ng = getenv("TOTK_PROBE_NEGATIVE"); g_negative = ng && !strcmp(ng, "1");
    if (g_negative) {
        int r = differential(50000);
        printf("negative probe (ranged index one page short): %s\n", r ? "failed as it must" : "DID NOT FAIL");
        return r ? 0 : 1;
    }
    if (!bench && differential(quick ? 50000 : 300000)) return 1;
    rs = 999; if (!bench && differential(quick ? 50000 : 300000)) return 1;
    g_junk = malloc(g_junk_n); memset(g_junk, 1, g_junk_n);
    static const char *const kn[3] = {"first-upload range scan", "pending scan (buffer read)", "invalidation range scan"};
    int sizes[2][2] = {{2459, 14}, {4002, 48}};
    for (int z = 0; z < 2; z++) {
        world wl, wi;
        build(&wl, sizes[z][0], sizes[z][1], 0); build(&wi, sizes[z][0], sizes[z][1], 1);
        wl.nid = wi.nid = sizes[z][0];
        printf("cost per scan, %d images, %d pending (lock held this long):\n", sizes[z][0], sizes[z][1]);
        for (int k = 0; k < 3; k++)
            for (int cold = 0; cold < 2; cold++) {
                int reps = cold ? 200 : 3000;
                double a = cost_ns(&wl, k, cold, reps), b = cost_ns(&wi, k, cold, reps);
                printf("  %-28s %-5s walk %8.1f us   index %6.2f us   (%.0fx)\n", kn[k], cold ? "cold" : "warm", a / 1e3, b / 1e3, a / (b > 1 ? b : 1));
            }
        if (!quick) {
            double wm, pw, lk, wm2, pw2, lk2;
            replay(&wl, 4, 2.0, &wm, &pw, &lk);
            replay(&wi, 4, 2.0, &wm2, &pw2, &lk2);
            printf("  lock replay (6 lookup threads, 4 scans per ms): walk: lookups waited %.1f ms per s (%.1f us per wait), %.0f lookups/s;"
                   " index: %.2f ms per s (%.1f us per wait), %.0f lookups/s\n", wm, pw, lk, wm2, pw2, lk2);
        }
    }
    // [UIT 0254] the buffer-read scan with many pending (2026-11-01 logs: ~140 pending images met per buffer read after a
    // load): every pending one (0230) against only those over the read (0254), 0 overlapping
    int pend[3] = {48, 140, 400};
    for (int z = 0; z < 3; z++) {
        world wi, wr;
        build(&wi, 4002, pend[z], 1); build(&wr, 4002, pend[z], 2);
        wi.nid = wr.nid = 4002;
        for (int cold = 0; cold < 2; cold++) {
            int reps = cold ? 200 : 3000;
            double a = cost_ns(&wi, 1, cold, reps), b = cost_ns(&wr, 1, cold, reps);
            printf("  [0254] pending scan, %3d pending, %-5s every pending %6.2f us   over the range only %6.2f us   (%.1fx)\n", pend[z],
                   cold ? "cold" : "warm", a / 1e3, b / 1e3, a / (b > 1 ? b : 1));
        }
    }
    printf("PASS\n");
    return 0;
}
