// [UIT 0234] vkb_bufmemo.h: buffer reads remembered as changing nothing are exactly the walks that would change
// nothing. Two copies of a model of vkb_iwb_buf_read's state (pending drawings over memory, wb_alias/wb_rel, the log
// counter, the per-thread last-range memo of UIT 0155) run the same random steps: one walks every time (as before),
// the other skips walks found in its per-thread bqm. After every step the states and the logs must be identical.
// The generation is bumped where vkb_tex.c bumps g_bidx_gen (bidx_mod; wb_rel set; wb_rel/wb_alias reset).
// cc -O2 -Wall -Wextra -Isrc/gfx tools/tests/test_bufmemo.c -o /tmp/t/bufmemo && /tmp/t/bufmemo
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "vkb_bufmemo.h"

enum { NI = 48, NT = 4, SAY = 32, EXPLICIT_MAX = 1 << 16 };
typedef struct { uint64_t lo, sz; int live, pending, alias, rel, in_pend; } img;
typedef struct {
    img im[NI];
    uint64_t gen, said, marked, logs;
    uint64_t q_addr[NT], q_len[NT], q_gen[NT];   // the UIT 0155 memo
    bqm m[NT];
    uint64_t walks, skips;
} model;

static uint64_t rs = 88172645463325252ull;
static uint64_t rnd(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return rs; }
static int over(const img *i, uint64_t lo, uint64_t hi) { return i->lo < hi && lo < i->lo + i->sz; }   // iwb_overlap

static void buf_read(model *M, int memo, int th, int on, uint64_t addr, uint64_t len) {
    if (!len) return;
    uint64_t gen = M->gen;
    if (M->q_addr[th] == addr && M->q_len[th] == len && M->q_gen[th] == gen) return;
    M->q_addr[th] = addr; M->q_len[th] = len; M->q_gen[th] = gen;
    if (memo && bqm_hit(&M->m[th], addr, len, gen)) { M->skips++; return; }
    int eff = 0;
    for (int k = 0; k < NI; k++) {   // the pending list (order does not matter to the effects)
        img *i = &M->im[k];
        if (!i->in_pend || !i->pending || !over(i, addr, addr + len)) continue;
        if (on && !i->alias && i->sz <= EXPLICIT_MAX) { i->alias = 1; M->marked++; eff = 1; }
        else if (on && i->rel) { i->rel = 0; eff = 1; }
        if (M->said >= SAY) continue;
        M->said++; M->logs = M->logs * 31 + (uint64_t)k + 1; eff = 1;
    }
    if (memo && !eff) bqm_put(&M->m[th], addr, len, gen);
    M->walks++;
}

// the writers (vkb_tex.c, g_mu held), the same on both copies
static void step_write(model *M, int op, int k, uint64_t lo, uint64_t sz) {
    img *i = &M->im[k];
    switch (op) {
    case 0:   // new entry (vkb_image_of)
        if (!i->live) { memset(i, 0, sizeof *i); i->live = 1; i->lo = lo; i->sz = sz; }
        break;
    case 1:   // iwb_stamp
        if (i->live && !i->pending) { i->pending = 1; i->in_pend = 1; M->gen++; }
        break;
    case 2:   // iwb_unpend
        if (i->pending) { i->pending = 0; M->gen++; }
        i->in_pend = 0;
        break;
    case 3:   // released (vkb_tex release: marked copied back)
        if (i->live && !i->alias) { i->alias = i->rel = 1; M->gen++; }
        break;
    case 4:   // taken back from the side
        if (i->live && i->rel) { i->rel = i->alias = 0; M->gen++; }
        break;
    case 5:   // the upload alias scan marks it
        if (i->live) i->alias = 1;
        break;
    case 6:   // entry_drop: out of the pending list (no bump: it only removes)
        i->in_pend = 0; i->live = 0;
        break;
    }
}

static int same(const model *a, const model *b) {
    if (a->said != b->said || a->marked != b->marked || a->logs != b->logs || a->gen != b->gen) return 0;
    for (int k = 0; k < NI; k++)
        if (a->im[k].alias != b->im[k].alias || a->im[k].rel != b->im[k].rel || a->im[k].pending != b->im[k].pending) return 0;
    return 1;
}

int main(int argc, char **argv) {
    long steps = argc > 1 ? atol(argv[1]) : 400000;
    static model A, B;
    uint64_t fails = 0, total_walks = 0, total_skips = 0;
    for (int run = 0; run < 64; run++) {
        memset(&A, 0, sizeof A);
        int on = run & 1, say0 = run & 2;   // half the runs start with the log counter used up
        if (say0) A.said = SAY;
        B = A;
        uint64_t span = (run & 4) ? 1 << 18 : 1 << 22;   // dense / sparse memory
        for (long s = 0; s < steps / 64; s++) {
            uint64_t r = rnd();
            if (r % 100 < 70) {   // a buffer read: often the same few ranges (draw after draw)
                int th = (int)(rnd() % NT);
                uint64_t addr, len;
                if (rnd() % 4) { uint64_t j = rnd() % 24; addr = (j * 7919 * 4096) % span; len = 256 << (j % 6); }
                else { addr = rnd() % span; len = 1 + rnd() % 65536; }
                buf_read(&A, 0, th, on, addr, len);
                buf_read(&B, 1, th, on, addr, len);
            } else {
                int op = (int)(rnd() % 7), k = (int)(rnd() % NI);
                uint64_t lo = rnd() % span, sz = (rnd() % 3) ? 4096 + rnd() % 65536 : rnd() % (1 << 20);
                step_write(&A, op, k, lo, sz);
                step_write(&B, op, k, lo, sz);
            }
            if (!same(&A, &B)) { if (fails++ < 5) printf("FAIL run %d step %ld\n", run, s); break; }
        }
        total_walks += B.walks; total_skips += B.skips;
    }
    printf("bufmemo: %ld steps, walks with the memo %llu, skipped %llu (%.0f%%); %s\n", steps,
           (unsigned long long)total_walks, (unsigned long long)total_skips,
           100.0 * (double)total_skips / (double)(total_walks + total_skips + 1), fails ? "FAIL" : "identical");
    return fails ? 1 : 0;
}
