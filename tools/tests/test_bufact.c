// [UIT 0243] vkb_bufact.h: buffer reads skipped because no pending drawing in their chunks could be marked or cleared
// are exactly walks that would change nothing. Two copies of a model of vkb_iwb_buf_read's state (pending drawings
// over memory, wb_alias/wb_rel, the log counter, the UIT 0155 last-range memo and the UIT 0234 per-thread memo) run
// the same random steps: A walks as before, B first asks the actionable table (kept by bact_fix at every write of
// wb_pending / wb_alias / wb_rel and when an entry goes, as vkb_tex.c does). After every step the states and logs
// must be identical, and once everything is unpended and dropped the table must be all zero. A second part runs
// readers against a writer on threads (for TSAN): the readers never take the writer's lock.
// cc -O2 -Wall -Wextra -pthread -Isrc/gfx tools/tests/test_bufact.c -o /tmp/t/bufact && /tmp/t/bufact
// [UIT 0254] A third copy C asks the table whatever the log counter says (TOTK_VK_BUFREAD_RANGE, which drops 0243's
// g_bq_said >= 32 condition): its marks, clears and pending state must match A after every step; only its log lines
// differ (fewer: none for reads that change nothing), so said/logs are not compared for C.
// TOTK_PROBE_NEGATIVE=1: B and C forget one bact_fix (the wb_rel 0 -> 1 site) and the comparison must fail.
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "vkb_bufmemo.h"
#include "vkb_bufact.h"

enum { NI = 48, NT = 4, SAY = 32, EXPLICIT_MAX = 1 << 16 };
typedef struct { uint64_t lo, sz; int live, pending, alias, rel, in_pend, act; } img;
typedef struct {
    img im[NI];
    uint64_t gen, said, marked, logs;
    uint64_t q_addr[NT], q_len[NT], q_gen[NT];
    bqm m[NT];
    bact_tab t;
    int use_act;
    int gate_any;   // [UIT 0254] skip on the table without waiting for 32 log lines
    uint64_t walks, skips, noact;
} model;

static int negative;
static uint64_t rs = 0x2545F4914F6CDD1Dull;
static uint64_t rnd(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return rs; }
static int over(const img *i, uint64_t lo, uint64_t hi) { return i->lo < hi && lo < i->lo + i->sz; }
static int g_on;

static void fix(model *M, img *i) {   // vkb_tex.c bact_fix
    if (!M->use_act) return;
    if (!i->pending && !i->act) return;
    bact_set(&M->t, &i->act, bact_want(g_on, i->pending, i->alias, i->rel, i->sz, EXPLICIT_MAX), i->lo, i->sz);
}

static void buf_read(model *M, int th, uint64_t addr, uint64_t len) {
    if (!len) return;
    int any = 0;   // the g_bidx test: any pending drawing in the chunks (the same first GiB)
    { uint64_t c0 = addr >> 20, c1 = (addr + len - 1) >> 20; if (c1 - c0 > 1024) c1 = c0 + 1024;
      for (int k = 0; k < NI && !any; k++) { img *i = &M->im[k]; if (!i->pending || !i->sz || !i->lo) continue;
          for (uint64_t c = i->lo >> 20; c <= (i->lo + i->sz - 1) >> 20 && !any; c++) any = c >= c0 && c <= c1; } }
    if (!any) return;
    if (M->use_act && (M->gate_any || M->said >= SAY) && !bact_any(&M->t, addr, len)) { M->noact++; return; }
    uint64_t gen = M->gen;
    if (M->q_addr[th] == addr && M->q_len[th] == len && M->q_gen[th] == gen) return;
    M->q_addr[th] = addr; M->q_len[th] = len; M->q_gen[th] = gen;
    if (bqm_hit(&M->m[th], addr, len, gen)) { M->skips++; return; }
    int eff = 0;
    for (int k = 0; k < NI; k++) {
        img *i = &M->im[k];
        if (!i->in_pend || !i->pending || !over(i, addr, addr + len)) continue;
        if (g_on && !i->alias && i->sz <= EXPLICIT_MAX) { i->alias = 1; M->marked++; eff = 1; fix(M, i); }
        else if (g_on && i->rel) { i->rel = 0; eff = 1; fix(M, i); }
        if (M->said >= SAY) continue;
        M->said++; M->logs = M->logs * 31 + (uint64_t)k + 1; eff = 1;
    }
    if (!eff) bqm_put(&M->m[th], addr, len, gen);
    M->walks++;
}

static void step_write(model *M, int op, int k, uint64_t lo, uint64_t sz) {
    img *i = &M->im[k];
    switch (op) {
    case 0: if (!i->live) { memset(i, 0, sizeof *i); i->live = 1; i->lo = lo; i->sz = sz; } break;   // new entry
    case 1: if (i->live && !i->pending) { i->pending = 1; i->in_pend = 1; M->gen++; fix(M, i); } break;   // iwb_stamp
    case 2: if (i->pending) { i->pending = 0; M->gen++; fix(M, i); } i->in_pend = 0; break;   // iwb_unpend
    case 3: if (i->live && !i->alias) { i->alias = i->rel = 1; M->gen++; if (!negative) fix(M, i); } break;   // released
    case 4: if (i->live && i->rel) { i->rel = i->alias = 0; M->gen++; fix(M, i); } break;   // taken back
    case 5: if (i->live && !i->alias) { i->alias = 1; fix(M, i); } break;   // the upload alias scan marks it
    case 6:   // entry_drop: iwb_forget (unpends), then bact_drop
        if (!i->live) break;
        if (i->pending) { i->pending = 0; M->gen++; fix(M, i); }
        i->in_pend = 0;
        if (M->use_act && i->act) bact_set(&M->t, &i->act, 0, i->lo, i->sz);
        i->live = 0; break;
    }
}

static int same(const model *a, const model *b) {
    if (a->said != b->said || a->marked != b->marked || a->logs != b->logs || a->gen != b->gen) return 0;
    for (int k = 0; k < NI; k++)
        if (a->im[k].alias != b->im[k].alias || a->im[k].rel != b->im[k].rel || a->im[k].pending != b->im[k].pending) return 0;
    return 1;
}
static int same_marks(const model *a, const model *b) {   // [UIT 0254] all but the log lines
    if (a->marked != b->marked || a->gen != b->gen) return 0;
    for (int k = 0; k < NI; k++)
        if (a->im[k].alias != b->im[k].alias || a->im[k].rel != b->im[k].rel || a->im[k].pending != b->im[k].pending) return 0;
    return 1;
}
static int table_zero(const bact_tab *t) { for (int i = 0; i < BACT_N; i++) if (atomic_load(&t->c[i])) return 0; return 1; }

// ---- threads: one writer (with the lock) against readers without it
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static bact_tab tt;
static _Atomic int stop;
static void *reader(void *p) {
    uint64_t r = (uint64_t)(uintptr_t)p * 0x9E3779B97F4A7C15ull + 1, hits = 0;
    while (!atomic_load(&stop)) { r ^= r << 13; r ^= r >> 7; r ^= r << 17; hits += bact_any(&tt, (r % (1ull << 30)) | (1ull << 32), 1 + r % (4 << 20)); }
    return (void *)(uintptr_t)hits;
}
static void threads(long n) {
    pthread_t th[4];
    for (int i = 0; i < 4; i++) pthread_create(&th[i], NULL, reader, (void *)(uintptr_t)(i + 1));
    static img w[64];
    for (long s = 0; s < n; s++) {
        img *i = &w[rnd() % 64];
        pthread_mutex_lock(&mu);
        if (!i->sz) { i->lo = (1ull << 32) | (rnd() % (1ull << 30)); i->sz = 4096 + rnd() % (8 << 20); }
        int want = (int)(rnd() & 1);
        bact_set(&tt, &i->act, want, i->lo, i->sz);
        pthread_mutex_unlock(&mu);
    }
    pthread_mutex_lock(&mu);
    for (int k = 0; k < 64; k++) bact_set(&tt, &w[k].act, 0, w[k].lo, w[k].sz);
    pthread_mutex_unlock(&mu);
    atomic_store(&stop, 1);
    for (int i = 0; i < 4; i++) pthread_join(th[i], NULL);
}

int main(int argc, char **argv) {
    long steps = argc > 1 ? atol(argv[1]) : 400000;
    const char *ne = getenv("TOTK_PROBE_NEGATIVE"); negative = ne && !strcmp(ne, "1");
    static model A, B, C;
    uint64_t fails = 0, walks_a = 0, walks_b = 0, noact = 0, zero_fail = 0, fails_c = 0, walks_c = 0, noact_c = 0, said_a = 0, said_c = 0;
    for (int run = 0; run < 64; run++) {
        memset(&A, 0, sizeof A);
        g_on = !(run & 1) || run == 1;   // mostly on (the default); a few runs off
        if (run & 2) A.said = SAY;
        B = A; B.use_act = 1;
        C = B; C.gate_any = 1;
        uint64_t span = (run & 4) ? 1 << 22 : (run & 8) ? 1ull << 28 : 1ull << 24;
        for (long s = 0; s < steps / 64; s++) {
            uint64_t r = rnd();
            if (r % 100 < 70) {
                int th = (int)(rnd() % NT);
                uint64_t addr, len;
                if (rnd() % 4) { uint64_t j = rnd() % 24; addr = 4096 + (j * 7919 * 4096) % span; len = 256 << (j % 6); }
                else { addr = 4096 + rnd() % span; len = 1 + rnd() % (rnd() % 8 ? 65536 : 1 << 22); }
                buf_read(&A, th, addr, len);
                buf_read(&B, th, addr, len);
                buf_read(&C, th, addr, len);
            } else {
                int op = (int)(rnd() % 7), k = (int)(rnd() % NI);
                uint64_t lo = 4096 + rnd() % span, sz = (rnd() % 3) ? 4096 + rnd() % 65536 : rnd() % (1 << 21);
                step_write(&A, op, k, lo, sz);
                step_write(&B, op, k, lo, sz);
                step_write(&C, op, k, lo, sz);
            }
            if (!same(&A, &B)) { if (fails++ < 5) printf("FAIL run %d step %ld: the walk changed something the table said could not\n", run, s); break; }
            if (!same_marks(&A, &C)) { if (fails_c++ < 5) printf("FAIL run %d step %ld: 0254 gate (no log condition) changed the marks\n", run, s); break; }
        }
        for (int k = 0; k < NI; k++) step_write(&B, 6, k, 0, 0);
        if (!negative && !table_zero(&B.t)) { zero_fail++; printf("FAIL run %d: the table is not zero once every entry went\n", run); }
        walks_a += A.walks; walks_b += B.walks; noact += B.noact; walks_c += C.walks; noact_c += C.noact; said_a += A.said; said_c += C.said;
    }
    threads(negative ? 1000 : steps / 4);
    if (!table_zero(&tt)) { zero_fail++; printf("FAIL threads: the table is not zero\n"); }
    int bad = fails || zero_fail || fails_c;
    printf("bufact: %ld steps, walks before %llu, with the table %llu, skipped as nothing to change %llu; %s\n", steps,
           (unsigned long long)walks_a, (unsigned long long)walks_b, (unsigned long long)noact,
           bad ? "FAIL" : "identical");
    printf("bufact 0254 gate: walks %llu, skipped as nothing to change %llu, log lines %llu (before %llu); marks %s\n",
           (unsigned long long)walks_c, (unsigned long long)noact_c, (unsigned long long)said_c, (unsigned long long)said_a, fails_c ? "DIFFER" : "identical");
    if (negative) { printf("negative probe: %s\n", fails && fails_c ? "failed as it must (both gates)" : "DID NOT FAIL"); return fails && fails_c ? 0 : 1; }
    return bad;
}
