// [UIT 0027] GPU writes the copies taken at the submit call must know of (vkb_gpuw.h).
#include "vkb_gpuw.h"

#include <stdio.h>
#include <stdlib.h>

static void die(const char *what) { fprintf(stderr, "out of memory: %s\n", what); exit(2); }

void gpuw_add(gpuw_set *s, uint64_t lo, uint64_t hi, uint64_t fence) {
    if (hi <= lo) return;
    s->ixok = 0;
    if (s->npend) {   // the same buffer written again by the same submission (every draw of a pass): one entry
        gpuw_t *l = &s->pend[s->npend - 1];
        if (l->fence == fence && lo <= l->hi && l->lo <= hi) { if (lo < l->lo) l->lo = lo; if (hi > l->hi) l->hi = hi; return; }
    }
    if (s->npend == s->cappend) { s->cappend = s->cappend ? s->cappend * 2 : 1024; if (!(s->pend = realloc(s->pend, sizeof *s->pend * (size_t)s->cappend))) die("pending GPU writes"); }
    s->pend[s->npend++] = (gpuw_t){lo, hi, fence};
}
static int pend_overlaps(const gpuw_set *s, uint64_t lo, uint64_t hi) {
    if (!s->ixok) {
        for (int j = 0; j < s->npend; j++) if (s->pend[j].lo < hi && lo < s->pend[j].hi) return 1;
        return 0;
    }
    int a = 0, b = s->nix;   // entries starting below hi: [0, a)
    while (a < b) { int m = (a + b) / 2; if (s->ix[m].lo < hi) a = m + 1; else b = m; }
    return a && s->ixhi[a - 1] > lo;
}
int gpuw_inflight(const gpuw_set *s, uint64_t lo, uint64_t hi, uint64_t cdone) {
    if (s->ixok) return pend_overlaps(s, lo, hi);   // after retire(cdone) every pending write is later than cdone
    for (int j = 0; j < s->npend; j++)
        if (s->pend[j].fence > cdone && s->pend[j].lo < hi && lo < s->pend[j].hi) return 1;
    return 0;
}
int gpuw_recent(const gpuw_set *s, uint64_t lo, uint64_t hi, uint64_t cdone, uint64_t keep) {
    if (pend_overlaps(s, lo, hi)) return 1;   // pending: in flight at the capture, or completed since (the copy is older)
    int a = 0, b = s->nrec;   // the first interval ending above lo
    while (a < b) { int m = (a + b) / 2; if (s->rec[m].hi <= lo) a = m + 1; else b = m; }
    return a < s->nrec && s->rec[a].lo < hi && gpuw_kept(s->rec[a].fence, keep, cdone);
}
int gpuw_each(const gpuw_set *s, uint64_t lo, uint64_t hi, uint64_t cdone, int recent, uint64_t keep, gpuw_fn fn, void *ctx) {
    int n = 0;
    for (int j = 0; j < s->npend; j++) {   // pending: in flight at the capture (after retire every one is), or added since
        const gpuw_t *w = &s->pend[j];
        if (w->lo >= hi || lo >= w->hi || (!recent && !s->ixok && w->fence <= cdone)) continue;
        fn(ctx, w->lo > lo ? w->lo : lo, w->hi < hi ? w->hi : hi);
        n++;
    }
    if (!recent) return n;
    int a = 0, b = s->nrec;   // completed: the first interval ending above lo, then on while they start below hi
    while (a < b) { int m = (a + b) / 2; if (s->rec[m].hi <= lo) a = m + 1; else b = m; }
    for (; a < s->nrec && s->rec[a].lo < hi; a++) {
        if (!gpuw_kept(s->rec[a].fence, keep, cdone)) continue;
        fn(ctx, s->rec[a].lo > lo ? s->rec[a].lo : lo, s->rec[a].hi < hi ? s->rec[a].hi : hi);
        n++;
    }
    return n;
}
static int cmp(const void *x, const void *y) {
    const gpuw_t *a = x, *b = y;
    return a->lo < b->lo ? -1 : a->lo > b->lo;
}
static void grow(gpuw_t **a, int *cap, int need) {
    if (*cap >= need) return;
    *cap = need + need / 2 + 64;
    if (!(*a = realloc(*a, sizeof **a * (size_t)*cap))) die("pending GPU writes");
}
void gpuw_retire(gpuw_set *s, uint64_t cdone, uint64_t keep) {
    static _Thread_local gpuw_t *mv, *out;
    static _Thread_local int capmv, capout;
    int k = 0, nmv = 0;
    grow(&mv, &capmv, s->npend);
    for (int j = 0; j < s->npend; j++)
        if (s->pend[j].fence > cdone) s->pend[k++] = s->pend[j];
        else mv[nmv++] = s->pend[j];
    s->npend = k;
    if (s->capix < k) {
        s->capix = k + k / 2 + 64;
        if (!(s->ix = realloc(s->ix, sizeof *s->ix * (size_t)s->capix)) || !(s->ixhi = realloc(s->ixhi, sizeof *s->ixhi * (size_t)s->capix))) die("pending GPU writes");
    }
    for (int j = 0; j < k; j++) s->ix[j] = s->pend[j];
    qsort(s->ix, (size_t)k, sizeof *s->ix, cmp);
    for (int j = 0; j < k; j++) s->ixhi[j] = j && s->ixhi[j - 1] > s->ix[j].hi ? s->ixhi[j - 1] : s->ix[j].hi;
    s->nix = k; s->ixok = 1;
    int expired = 0;
    for (int j = 0; j < s->nrec && !expired; j++) expired = !gpuw_kept(s->rec[j].fence, keep, cdone);
    if (!nmv && !expired) return;
    qsort(mv, (size_t)nmv, sizeof *mv, cmp);   // the completed ones, merged into rec (sorted) in one pass
    grow(&out, &capout, s->nrec + nmv);
    int i = 0, j = 0, m = -1;
    while (i < s->nrec || j < nmv) {
        gpuw_t w = j >= nmv || (i < s->nrec && s->rec[i].lo <= mv[j].lo) ? s->rec[i++] : mv[j++];
        if (!gpuw_kept(w.fence, keep, cdone)) continue;   // long done (keep submissions; GPUW_FOREVER: never)
        if (m >= 0 && w.lo <= out[m].hi) {
            if (w.hi > out[m].hi) out[m].hi = w.hi;
            if (w.fence > out[m].fence) out[m].fence = w.fence;
        } else out[++m] = w;
    }
    grow(&s->rec, &s->caprec, m + 1);
    for (int q = 0; q <= m; q++) s->rec[q] = out[q];
    s->nrec = m + 1;
}
