// [UIT 0244] TOTK_VK_UCAP_FASTSORT: vkb_rsort.h ucap_rng_sort1_with gives the same array as ucap_rng_sort_with (by lo,
// the longest first, ties in input order), and vkb_geodedup.h's dedupe + check + sort of the selected vertex / index
// ranges picks the same ranges in the same order as the old sort + dedupe + check (vkb_ucap.c, with the 32 MiB cut-off
// modelled). Random and adversarial inputs (long tie runs over RS_TIE_MAX, all equal, descending, sizes around
// RS_SMALL). The geo field carries the input index, so an unstable tie order shows. Also prints the time of both.
// Negative probes (must be caught): -DTOTK_PROBE_UNSTABLE_TIES (ties moved past equal ones), -DTOTK_PROBE_PICK_REVERSED
// (geometry ranges picked out of input order, so the wrong one of equal ranges is kept).
// cc -O2 -Wall -Werror -Isrc/gfx tools/tests/test_ucap_fastsort.c -o /tmp/ufs && /tmp/ufs
#include <stdio.h>
#include <time.h>
#include "vkb_geodedup.h"

static uint64_t rs;
static uint64_t rnd(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return rs; }
static uint64_t now_ns(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec; }

static void fill(ucap_rng *v, int n, int kind) {
    for (int i = 0; i < n; i++) {
        uint64_t lo, hi;
        switch (kind) {
        case 0: lo = 0x2000000000ull + (rnd() % (1ull << 30)) / 64 * 64; hi = lo + 64 * (1 + rnd() % 2048); break;
        case 1: lo = 0x2000000000ull + (rnd() % 64) * 4096; hi = lo + 64 * (1 + rnd() % 8); break;           // many ties, short runs of equal (lo, hi)
        case 2: lo = 0x2000000000ull + (rnd() % 3) * 4096; hi = lo + 64 * (1 + rnd() % 200); break;            // tie runs far over RS_TIE_MAX
        case 3: lo = 0x5000; hi = 0x6000; break;                                                                 // all equal
        case 4: lo = 0x9000000000ull - (uint64_t)i * 256; hi = lo + 256 + (uint64_t)(i % 5) * 64; break;      // descending
        default: lo = (rnd() & 1) ? 0 : ~0ull >> 1; hi = lo + 1 + rnd() % 3; break;                            // a span of 63 bits
        }
        if (i && rnd() % 5 == 0) {   // repeats: an earlier range exactly, or its start with another length
            ucap_rng e = v[rnd() % (uint64_t)i];
            lo = e.lo; hi = rnd() & 1 ? lo + 64 * (1 + rnd() % 4) : e.hi;
        }
        v[i] = (ucap_rng){lo, hi, (uint64_t)i};
    }
}

// the write watch, modelled: a page generation table over the address space used
#define NPG 4096
static uint64_t gen[NPG];
static int written_after(uint64_t lo, uint64_t len, uint64_t since) {
    for (uint64_t p = (lo >> 12) % NPG, e = p + ((lo & 4095) + len + 4095) / 4096; p < e; p++) if (gen[p % NPG] > since) return 1;
    return 0;
}
// vkb_ucap.c's selection before 0244 (sort, skip exact repeats, check, the cut-off in order) and with it
static int pick_old(ucap_rng *g, int n, ucap_rng *tmp, ucap_rng *out, uint64_t since, uint64_t cut) {
    if (n >= 2 && !ucap_rng_small(g, n)) ucap_rng_sort_with(g, tmp, n);
    int k = 0; uint64_t total = 0;
    for (int i = 0; i < n; i++) {
        if (i && g[i].lo == g[i - 1].lo && g[i].hi == g[i - 1].hi) continue;
        uint64_t len = g[i].hi - g[i].lo;
        if (!written_after(g[i].lo, len, since)) continue;
        if (total + len > cut) break;
        total += len; out[k++] = g[i];
    }
    return k;
}
static int pick_new(ucap_rng *g, int n, ucap_rng *tmp, ucap_rng *out, uint64_t since, uint64_t cut, ucap_dset *ds) {
#ifdef TOTK_PROBE_PICK_REVERSED   // the second negative probe: the dedupe keeps the wrong one of equal ranges
    for (int i = 0, j = n - 1; i < j; i++, j--) { ucap_rng x = g[i]; g[i] = g[j]; g[j] = x; }
#endif
    int u = ucap_dedup(g, n, ds);
    if (u < 0) return -1;
    int m = 0;
    for (int i = 0; i < u; i++) if (written_after(g[i].lo, g[i].hi - g[i].lo, since)) g[m++] = g[i];
    if (m >= 2 && !ucap_rng_small(g, m)) ucap_rng_sort1_with(g, tmp, m);
    int k = 0; uint64_t total = 0;
    for (int i = 0; i < m; i++) {
        uint64_t len = g[i].hi - g[i].lo;
        if (total + len > cut) break;
        total += len; out[k++] = g[i];
    }
    return k;
}

int main(void) {
    enum { MAXN = 40000 };
    static ucap_rng v[MAXN], a[MAXN], b[MAXN], t[MAXN], oa[MAXN], ob[MAXN];
    static const int sizes[] = {0, 1, 2, 3, 64, 65, 95, 96, 97, 98, 200, 1000, 5000, 10132, 24329, MAXN};
    long bad_sort = 0, bad_pick = 0, cases = 0, fellback = 0;
    ucap_dset ds = {0};
    rs = 0x9E3779B97F4A7C15ull;
    for (int round = 0; round < 6; round++)
        for (int kind = 0; kind < 6; kind++)
            for (unsigned si = 0; si < sizeof sizes / sizeof *sizes; si++) {
                int n = sizes[si];
                fill(v, n, kind);
                memcpy(a, v, sizeof *v * (size_t)n); memcpy(b, v, sizeof *v * (size_t)n);
                ucap_rng_sort_with(a, t, n);
                ucap_rng_sort1_with(b, t, n);
                cases++;
                if (memcmp(a, b, sizeof *a * (size_t)n)) {
                    if (bad_sort++ < 3) {
                        int i = 0; while (a[i].lo == b[i].lo && a[i].hi == b[i].hi && a[i].geo == b[i].geo) i++;
                        printf("sort differs: kind %d n %d at %d: (%llx %llx #%llu) vs (%llx %llx #%llu)\n", kind, n, i, (unsigned long long)a[i].lo,
                               (unsigned long long)a[i].hi, (unsigned long long)a[i].geo, (unsigned long long)b[i].lo, (unsigned long long)b[i].hi, (unsigned long long)b[i].geo);
                    }
                }
                if (kind == 2 && n > 1000) fellback++;
                for (int g = 0; g < NPG; g++) gen[g] = rnd() % 100 < (uint64_t)(round * 7) ? 50 + rnd() % 10 : rnd() % 50;
                uint64_t since = 49 + (uint64_t)(round % 3) * 3, cut = round & 1 ? 32u << 20 : 1u << 20;
                memcpy(a, v, sizeof *v * (size_t)n); memcpy(b, v, sizeof *v * (size_t)n);
                int ka = pick_old(a, n, t, oa, since, cut), kb = pick_new(b, n, t, ob, since, cut, &ds);
                // the picked list as vkb_ucap.c appends it (geo field: the input index of the first of equal ranges)
                if (ka != kb || memcmp(oa, ob, sizeof *oa * (size_t)ka)) {
                    if (bad_pick++ < 3) printf("pick differs: kind %d n %d round %d: %d vs %d ranges\n", kind, n, round, ka, kb);
                }
            }
    // time at the t254 sizes
    double ts[2] = {1e18, 1e18}, tp[2] = {1e18, 1e18};
    for (int it = 0; it < 40; it++) {
        fill(v, 24329, 0);
        memcpy(a, v, sizeof *v * 24329); uint64_t t0 = now_ns(); ucap_rng_sort_with(a, t, 24329); uint64_t t1 = now_ns();
        memcpy(b, v, sizeof *v * 24329); ucap_rng_sort1_with(b, t, 24329); uint64_t t2 = now_ns();
        if ((double)(t1 - t0) < ts[0]) ts[0] = (double)(t1 - t0);
        if ((double)(t2 - t1) < ts[1]) ts[1] = (double)(t2 - t1);
        fill(v, 10132, 1);
        for (int g = 0; g < NPG; g++) gen[g] = rnd() % 100 < 3 ? 60 : 1;
        memcpy(a, v, sizeof *v * 10132); t0 = now_ns(); pick_old(a, 10132, t, oa, 50, 32u << 20); t1 = now_ns();
        memcpy(b, v, sizeof *v * 10132); pick_new(b, 10132, t, ob, 50, 32u << 20, &ds); t2 = now_ns();
        if ((double)(t1 - t0) < tp[0]) tp[0] = (double)(t1 - t0);
        if ((double)(t2 - t1) < tp[1]) tp[1] = (double)(t2 - t1);
    }
    free(ds.slot);
    printf("%ld cases (%ld with tie runs over %d): sort differs %ld, picked ranges differ %ld; 24329 ranges sorted in %.0f -> %.0f us, "
           "10132 geometry ranges picked in %.0f -> %.0f us\n", cases, fellback, RS_TIE_MAX, bad_sort, bad_pick, ts[0] / 1e3, ts[1] / 1e3, tp[0] / 1e3, tp[1] / 1e3);
    int bad = bad_sort + bad_pick > 0;
#if defined TOTK_PROBE_UNSTABLE_TIES || defined TOTK_PROBE_PICK_REVERSED
    printf("%s\n", bad ? "PASS (the negative probe is caught)" : "FAIL: the unstable tie order was not caught");
    return bad ? 0 : 1;
#else
    printf("%s\n", bad ? "FAIL" : "PASS");
    return bad;
#endif
}
