// [UIT 0258] Box test of src/gfx/vkb_presync_chunk.h (TOTK_VK_PRESYNC_BY_CHUNK).
// cc -O2 -pthread -I src/gfx tools/tests/test_presync_chunk.c && ./a.out ; -DPROBE=n breaks one rule (must fail).
#include "vkb_presync_chunk.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#ifndef PROBE
#define PROBE 0
#endif
static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static uint64_t g_fc;
static void *stamper(void *a) {
    int base = (int)(intptr_t)a;
    for (int r = 0; r < 20000; r++) {
#if PROBE == 2   // a plain store: the last use wins, not the lowest chunk
        __atomic_store_n(&g_fc, vkb_pbc_pack(5, base + (r % 50)), __ATOMIC_RELAXED);
#else
        vkb_pbc_stamp(&g_fc, 5, base + (r % 50));
#endif
    }
    return NULL;
}

// The model: P pools, C chunks; pool i's first use this submission is chunk use[i] (-1: unused). A plan from the last
// submission's stamps; stages synced before each chunk is handed out; a draw finding its pool not current syncs it
// (the draw path) unless PROBE 1 turns that off. Every chunk must find each pool it reads current.
enum { P = 440, C = 16 };
static unsigned rng = 12345;
static unsigned rnd(void) { rng = rng * 1103515245u + 12345u; return rng >> 8; }
static int model(int frames, int jitter, int lead, int *late_out, int *early_out, int *stale_out) {
    static uint64_t fc[P];
    static int use[P], cur[P], sync_count[P];
    uintptr_t addr[P], paddr[P]; int ch[P], pch[P];
    vkb_pbc_plan plan = {paddr, pch, 0, 0, P};
    int late = 0, early = 0, stale = 0, twice = 0;
    memset(fc, 0, sizeof fc);   // each run starts without stamps
    for (int i = 0; i < P; i++) use[i] = (int)(rnd() % (C + 4)) - 4 < 0 ? 0 : (int)(rnd() % C);   // many in chunk 0
    for (uint64_t sub = 10; sub < 10 + (uint64_t)frames; sub++) {
        for (int i = 0; i < P; i++) {   // this frame's first uses drift a little from the last one's
            if (jitter && rnd() % 4 == 0) { use[i] += (int)(rnd() % (2 * jitter + 1)) - jitter; }
            if (use[i] < 0) use[i] = 0;
            if (use[i] >= C) use[i] = C - 1;
            cur[i] = 0; sync_count[i] = 0;
        }
        int n = 0;
        for (int i = 0; i < P; i++) { addr[n] = (uintptr_t)i; ch[n] = vkb_pbc_predict(fc[i], sub, 2, lead); n++; }
        vkb_pbc_order(&plan, addr, ch, n);
        int a, b;
        for (int c = 0; c < C; c++) {
            if (vkb_pbc_take(&plan, c, &a, &b))   // before chunk c is handed out
                for (int k = a; k < b; k++) {
                    int i = (int)plan.addr[k];
                    if (!cur[i]) { cur[i] = 1; sync_count[i]++; if (c == 0) early++; }
                }
            for (int i = 0; i < P; i++)        // chunk c's draws
                if (use[i] == c) {
                    if (!cur[i]) {
                        if (PROBE == 1) { stale++; continue; }
                        cur[i] = 1; sync_count[i]++; late++;   // the draw path
                    }
                    vkb_pbc_stamp(&fc[i], sub, c);
                }
        }
        if (vkb_pbc_take(&plan, 1 << 30, &a, &b))   // after the walk: the rest
            for (int k = a; k < b; k++) { int i = (int)plan.addr[k]; if (!cur[i]) { cur[i] = 1; sync_count[i]++; } }
        for (int i = 0; i < P; i++) { if (sync_count[i] > 1) twice++; if (!cur[i]) stale++; }
    }
    *late_out = late; *early_out = early; *stale_out = stale;
    return twice;
}

int main(void) {
    // [UIT 0312] off by default: only 1 / on turn it on
    CHECK(vkb_pbc_parse(NULL) == 0 && vkb_pbc_parse("") == 0 && vkb_pbc_parse("-") == 0 && vkb_pbc_parse("0") == 0 &&
          vkb_pbc_parse("off") == 0 && vkb_pbc_parse("1") == 1 && vkb_pbc_parse("on") == 1,
          "parse: default off, 1/on on");
    uint64_t fc = 0;
    vkb_pbc_stamp(&fc, 7, 9);  CHECK(fc == vkb_pbc_pack(7, 9), "first stamp");
    vkb_pbc_stamp(&fc, 7, 12); CHECK(fc == vkb_pbc_pack(7, 9), "a later chunk of the same submission keeps the lower");
    vkb_pbc_stamp(&fc, 7, 3);  CHECK(fc == vkb_pbc_pack(7, 3), "a lower chunk replaces");
    vkb_pbc_stamp(&fc, 6, 0);  CHECK(fc == vkb_pbc_pack(7, 3), "an older submission's use is ignored");
    vkb_pbc_stamp(&fc, 8, 40); CHECK(fc == vkb_pbc_pack(8, 40), "a newer submission replaces");
    CHECK(vkb_pbc_predict(fc, 9, 2, 0) == 40 && vkb_pbc_predict(fc, 10, 2, 0) == 40, "predicted within the window");
    CHECK(vkb_pbc_predict(fc, 9, 2, 2) == 38 && vkb_pbc_predict(vkb_pbc_pack(8, 1), 9, 2, 2) == 0, "the lead");
    CHECK(vkb_pbc_predict(fc, 11, 2, 0) == 0, "too old: the prologue");
    CHECK(vkb_pbc_predict(fc, 8, 2, 0) == 0, "this submission's own stamp is not a prediction");
    CHECK(vkb_pbc_predict(0, 9, 2, 0) == 0, "no stamp: the prologue");
    CHECK(vkb_pbc_pack(3, 500) == vkb_pbc_pack(3, VKB_PBC_NOCH - 1) && vkb_pbc_pack(3, -2) == vkb_pbc_pack(3, 0), "clamped");
    pthread_t th[8];
    for (int t = 0; t < 8; t++) pthread_create(&th[t], NULL, stamper, (void *)(intptr_t)(t * 3 + 2));
    for (int t = 0; t < 8; t++) pthread_join(th[t], NULL);
    CHECK(g_fc == vkb_pbc_pack(5, 2), "8 threads stamping: the lowest chunk (%llu)", (unsigned long long)(g_fc & 127));

    uintptr_t a8[8] = {10, 11, 12, 13, 14, 15, 16, 17}, pa[8]; int c8[8] = {3, 0, 3, 1, 0, 200, -1, 1}, pc[8];
    vkb_pbc_plan p = {pa, pc, 0, 0, 8};
    vkb_pbc_order(&p, a8, c8, 8);
    uintptr_t want[8] = {11, 14, 16, 13, 17, 10, 12, 15};
    int ok = 1;
    for (int i = 0; i < 8; i++) ok &= pa[i] == want[i];
    CHECK(ok, "ordered by chunk, stable, clamped");
    int a, b;
    CHECK(vkb_pbc_take(&p, 0, &a, &b) && a == 0 && b == 3, "stage 0: 3 pools");
    CHECK(!vkb_pbc_take(&p, 0, &a, &b), "stage 0 again: none");
    CHECK(vkb_pbc_take(&p, 2, &a, &b) && a == 3 && b == 5, "up to chunk 2");
    CHECK(!vkb_pbc_take(&p, 1, &a, &b), "an earlier chunk later: nothing again");
    CHECK(vkb_pbc_take(&p, 1 << 30, &a, &b) && a == 5 && b == 8, "the rest after the walk");

    int late, early, stale;
    for (int lead = 0; lead <= 3; lead++) {
        int twice = model(200, 0, lead, &late, &early, &stale);
        CHECK(!stale && !twice, "steady uses, lead %d: none stale (%d), none synced twice (%d)", lead, stale, twice);
        CHECK(late <= P, "steady uses, lead %d: draw-path syncs only in the first submission (%d)", lead, late);
        int e0 = early;
        twice = model(200, 3, lead, &late, &early, &stale);
        CHECK(!stale && !twice, "drifting uses, lead %d: none stale (%d), none synced twice (%d)", lead, stale, twice);
        printf("lead %d: steady %d of %d pools at the prologue; drifting (+-3 chunks for 1 pool in 4 a frame) %d at the "
               "prologue, %.1f draw-path syncs a frame\n", lead, e0 / 200, P, early / 200, late / 200.0);
    }
    printf("%s (%d failures)\n", fails ? "FAILED" : "PASS", fails);
    return fails != 0;
}
