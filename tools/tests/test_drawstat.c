// Box test for src/gfx/vkb_drawstat.h ([UIT 0053]): cc -O2 -Wall -Isrc/gfx tools/tests/test_drawstat.c -o /tmp/t/ds && /tmp/t/ds
#include <stdio.h>
#include <stdlib.h>
#include "vkb_drawstat.h"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)
typedef struct { int gone, born; char vs[16], fs[16]; double before, now; } got_t;
static void out(void *arg, int sign, double before, double now, const char *vs, const char *fs) {
    got_t *g = arg;
    if (sign < 0) g->gone++; else g->born++;
    snprintf(g->vs, sizeof g->vs, "%s", vs); snprintf(g->fs, sizeof g->fs, "%s", fs);
    g->before = before; g->now = now;
}
int main(void) {
    ds_table *t = calloc(1, sizeof *t);
    int a, b, c;   // three "shaders"
    // window 1: grass pair 10 a frame, terrain pair 20 a frame (60 frames)
    for (int f = 0; f < 60; f++) {
        for (int i = 0; i < 10; i++) ds_hit(t, &a, &b, "aaaaaaaaaaaaaaaaaaaa", "bbbbbbbbbbbbbbbb");
        for (int i = 0; i < 20; i++) ds_hit(t, &a, &c, "aaaaaaaaaaaaaaaaaaaa", NULL);
    }
    got_t g = {0};
    CHECK(ds_window(t, 60, 2, 8, out, &g) == 0);   // first window: nothing to compare with
    CHECK(g.gone == 0 && g.born == 0);
    // window 2: grass gone, terrain stays, a new pair appears
    for (int f = 0; f < 60; f++) {
        for (int i = 0; i < 20; i++) ds_hit(t, &a, &c, "aaaaaaaaaaaaaaaaaaaa", NULL);
        for (int i = 0; i < 3; i++) ds_hit(t, &b, &c, "cccc", "dddd");
    }
    memset(&g, 0, sizeof g);
    CHECK(ds_window(t, 60, 2, 8, out, &g) == 2);
    CHECK(g.gone == 1 && g.born == 1);
    // window 3: steady: no changes
    for (int f = 0; f < 60; f++) {
        for (int i = 0; i < 20; i++) ds_hit(t, &a, &c, "x", NULL);
        for (int i = 0; i < 3; i++) ds_hit(t, &b, &c, "cccc", "dddd");
    }
    memset(&g, 0, sizeof g);
    CHECK(ds_window(t, 60, 2, 8, out, &g) == 0);
    // names: 12 characters kept, NULL is "-"
    ds_table *u = calloc(1, sizeof *u);
    ds_hit(u, &a, &b, "0123456789abcdefXYZ", NULL);
    for (int f = 0; f < 60; f++) for (int i = 0; i < 5; i++) ds_hit(u, &a, &b, "zz", NULL);
    ds_window(u, 60, 2, 8, NULL, NULL);
    memset(&g, 0, sizeof g);
    CHECK(ds_window(u, 60, 2, 8, out, &g) == 1 && g.gone == 1);
    CHECK(!strcmp(g.vs, "0123456789ab") && !strcmp(g.fs, "-"));
    CHECK(g.before > 4.9 && g.now == 0);
    // order of the arguments matters (vs, fs) and the key is never 0
    CHECK(ds_key(&a, &b) != ds_key(&b, &a));
    CHECK(ds_key(NULL, NULL) != 0);
    // a full table counts what it cannot hold
    ds_table *w = calloc(1, sizeof *w);
    static char objs[DS_SLOTS + 200];
    for (unsigned i = 0; i < DS_SLOTS + 200; i++) ds_hit(w, &objs[i], NULL, "v", NULL);
    CHECK(w->full > 0);
    printf(fails ? "drawstat: %d FAILED\n" : "drawstat: all passed\n", fails);
    return fails != 0;
}
