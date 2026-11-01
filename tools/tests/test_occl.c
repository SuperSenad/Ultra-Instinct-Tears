// [UIT 0049/0050] Box test for src/gfx/vkb_occl.h: the samples-passed counter model behind TOTK_VK_OCCLUSION.
// cc -O2 -Wall -Isrc/gfx tools/tests/test_occl.c -o /tmp/t/occl && /tmp/t/occl
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "vkb_occl.h"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

static oq_walk W;
static uint64_t res[2 * OQ_CAP];
static void put(uint32_t q, uint64_t v) { res[2 * q] = v; res[2 * q + 1] = 1; }

static void t_visible(void) {   // 0049: every report differs, grows, and is large; also in the low 32 bits
    oq_acc a = {0};
    uint64_t prev = oq_visible(&a);
    CHECK(prev == 1u << 20);
    for (int i = 0; i < 3000000; i++) {
        uint64_t v = oq_visible(&a);
        CHECK(v > prev && v >= 1u << 20);
        CHECK((uint32_t)v - (uint32_t)prev == 4096u);   // later minus earlier, as a 32-bit game would compute it
        if (v <= prev) break;
        prev = v;
    }
    // the pre-0049 trap: two reports compared "not equal" (render if they differ) were always equal
    oq_acc b = {0};
    uint64_t x = oq_visible(&b), y = oq_visible(&b);
    CHECK(x != y);
}

static void t_window_one_sub(void) {   // Reset; 3 draws; Report A; 2 draws; Report B (no reset between)
    memset(&W, 0, sizeof W);
    memset(res, 0, sizeof res);
    oq_sub_begin(&W);
    CHECK(oq_draw(&W, 5) == -1);   // before any reset: not counted
    oq_reset(&W);
    int q0 = oq_draw(&W, 10), q1 = oq_draw(&W, 11), q2 = oq_draw(&W, 12);
    CHECK(q0 == 0 && q1 == 1 && q2 == 2);
    uint32_t qa, qb; int carry;
    CHECK(oq_report(&W, &qa, &qb, &carry) && qa == 0 && qb == 3 && !carry);
    int q3 = oq_draw(&W, 20), q4 = oq_draw(&W, 21);
    uint32_t qa2, qb2; int carry2;
    CHECK(oq_report(&W, &qa2, &qb2, &carry2) && qa2 == 0 && qb2 == 5 && !carry2);
    put(q0, 100); put(q1, 0); put(q2, 7); put(q3, 0); put(q4, 0);   // q1: fully hidden; q3, q4: hidden
    oq_acc acc = {0};
    uint64_t A = oq_value(&acc, res, qa, qb, carry), B = oq_value(&acc, res, qa2, qb2, carry2);
    CHECK(A == 107 && B == 107);   // B == A: what B gates (render if not equal) is hidden, as on the Switch
    // a query never begun (its draw returned early) counts 0
    res[2 * q2 + 1] = 0;
    CHECK(oq_value(&acc, res, qa, qb, carry) == 100);
}

static void t_carry(void) {   // the window opens in submission 1 and is reported in submission 2
    memset(&W, 0, sizeof W);
    memset(res, 0, sizeof res);
    oq_acc acc = {0};
    oq_sub_begin(&W);
    oq_draw(&W, 1);   // not armed
    oq_reset(&W);
    int a0 = oq_draw(&W, 2), a1 = oq_draw(&W, 3);
    CHECK(a0 == 0 && a1 == 1);
    uint32_t lr = W.last_reset, n1 = W.n;
    put(0, 30); put(1, 12);
    oq_job_end(&acc, res, lr, n1);
    CHECK(acc.carry == 42);
    // submission 2: two more draws, then the report
    memset(res, 0, sizeof res);
    oq_sub_begin(&W);
    int b0 = oq_draw(&W, 50), b1 = oq_draw(&W, 51);
    CHECK(b0 == 0 && b1 == 1);
    uint32_t qa, qb; int carry;
    CHECK(oq_report(&W, &qa, &qb, &carry) && qa == 0 && qb == 2 && carry);
    put(0, 1); put(1, 2);
    CHECK(oq_value(&acc, res, qa, qb, carry) == 45);
    oq_job_end(&acc, res, W.last_reset, W.n);   // no reset in it: the window keeps adding
    CHECK(acc.carry == 45);
    // submission 3: reset then report with no draws: 0
    memset(res, 0, sizeof res);
    oq_sub_begin(&W);
    oq_draw(&W, 60);   // still the old window
    oq_reset(&W);
    CHECK(oq_report(&W, &qa, &qb, &carry) && qa == 1 && qb == 1 && !carry);
    put(0, 99);
    CHECK(oq_value(&acc, res, qa, qb, carry) == 0);
    oq_job_end(&acc, res, W.last_reset, W.n);
    CHECK(acc.carry == 0);
}

static void t_unarmed_and_cap(void) {
    memset(&W, 0, sizeof W);
    uint32_t qa, qb; int carry;
    oq_sub_begin(&W);
    CHECK(!oq_report(&W, &qa, &qb, &carry));   // no reset ever: not countable (visible value instead)
    oq_reset(&W);
    for (uint32_t i = 0; i < OQ_CAP; i++) CHECK(oq_draw(&W, 100 + i) == (int)i);
    CHECK(oq_draw(&W, 100 + OQ_CAP) == -1);   // the pool is full: the window lost a draw
    CHECK(!oq_report(&W, &qa, &qb, &carry));
    oq_sub_begin(&W);
    CHECK(!oq_report(&W, &qa, &qb, &carry));   // still the same window: still not exact
    oq_reset(&W);
    CHECK(oq_report(&W, &qa, &qb, &carry));    // a new window is
}

static void t_find(void) {   // chunks look up their draws' queries by command position, up to the count at their cut
    memset(&W, 0, sizeof W);
    oq_sub_begin(&W);
    oq_reset(&W);
    uint64_t seqs[] = {3, 9, 10, 40, 41, 1000};
    for (int i = 0; i < 6; i++) oq_draw(&W, seqs[i]);
    for (int i = 0; i < 6; i++) CHECK(oq_find(&W, W.n, seqs[i]) == i);
    CHECK(oq_find(&W, W.n, 4) == -1 && oq_find(&W, W.n, 0) == -1 && oq_find(&W, W.n, 2000) == -1);
    CHECK(oq_find(&W, 3, 40) == -1);   // past the chunk's bound: not seen (written later by the walk)
    CHECK(oq_find(&W, 0, 3) == -1);
    // a stale entry past the bound with a smaller position (the previous submission's) must not be found
    W.seq[6] = 1;
    CHECK(oq_find(&W, 6, 1) == -1);
}

static void t_cond(void) {   // [UIT 0052] TOTK_VK_COND_DRAWS: Off / 1 draws only / 2 draws and clears
    CHECK(cond_mode_parse(NULL) == 0 && cond_mode_parse("") == 0 && cond_mode_parse("-") == 0 && cond_mode_parse("0") == 0);
    CHECK(cond_mode_parse("1") == 1 && cond_mode_parse("2") == 2 && cond_mode_parse("x") == 0);
    for (int m = 0; m <= 2; m++) for (int c = 0; c <= 1; c++) {
        CHECK(cond_skip(0, m, c) == 0);   // rendering on: never skipped
        CHECK(cond_skip(1, m, c) == 1);   // SetRenderEnable(false): always obeyed
    }
    CHECK(cond_skip(2, 0, 0) == 1 && cond_skip(2, 0, 1) == 1);   // Off: conditional obeyed
    CHECK(cond_skip(2, 1, 0) == 0 && cond_skip(2, 1, 1) == 1);   // Draws only (0048)
    CHECK(cond_skip(2, 2, 0) == 0 && cond_skip(2, 2, 1) == 0);   // Draws and clears
    // the [occl] line: every 60 presents, never at 0, off when every is 0
    int k = 0;
    for (uint64_t n = 1; n <= 600; n++) k += occl_log_due(n, 60);
    CHECK(k == 10 && occl_log_due(60, 60) && !occl_log_due(59, 60) && !occl_log_due(0, 60) && !occl_log_due(60, 0));
}

int main(void) {
    t_cond();
    t_visible();
    t_window_one_sub();
    t_carry();
    t_unarmed_and_cap();
    t_find();
    if (fails) { printf("%d failures\n", fails); return 1; }
    printf("occl: all checks passed\n");
    return 0;
}
