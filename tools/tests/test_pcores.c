// [UIT 0033] Box test for pcores.h (TOTK_VK_PCORES): which CPU sets a hybrid CPU's performance cores are.
// cc -O2 -Isrc/runtime/platform tools/tests/test_pcores.c -o /tmp/t/pc && /tmp/t/pc
#include <stdio.h>
#include "../../src/runtime/platform/pcores.h"
static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)
int main(void) {
    uint32_t out[64];
    // i7-12700K: 8 P-cores with hyperthreading (class 1, 16 logical) then 4 E-cores (class 0)
    uint8_t c1[20]; uint32_t i1[20];
    for (int i = 0; i < 20; i++) { c1[i] = i < 16; i1[i] = 256 + i; }
    int k = pcore_pick(c1, i1, 20, out);
    CHECK(k == 16 && out[0] == 256 && out[15] == 271);
    // Ryzen / older Intel: one class: nothing to prefer
    uint8_t c2[16] = {0}; uint32_t i2[16];
    for (int i = 0; i < 16; i++) i2[i] = 256 + i;
    CHECK(pcore_pick(c2, i2, 16, out) == 0);
    // E-cores listed first, three classes (Core Ultra: LP E-cores 0, E-cores 1, P-cores 2)
    uint8_t c3[8] = {0, 0, 1, 1, 1, 1, 2, 2}; uint32_t i3[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    k = pcore_pick(c3, i3, 8, out);
    CHECK(k == 2 && out[0] == 7 && out[1] == 8);
    CHECK(pcore_pick(c3, i3, 0, out) == 0);
    // [UIT 0089] ecore_pick: background work on the class just below the fastest
    k = ecore_pick(c1, i1, 20, out);                     // i7-12700K: the 4 E-cores
    CHECK(k == 4 && out[0] == 272 && out[3] == 275);
    CHECK(ecore_pick(c2, i2, 16, out) == 0);             // not hybrid
    k = ecore_pick(c3, i3, 8, out);                      // Core Ultra: the E-cores, not the low-power ones
    CHECK(k == 4 && out[0] == 3 && out[3] == 6);
    uint8_t c4[6] = {2, 0, 2, 0, 1, 1}; uint32_t i4[6] = {10, 11, 12, 13, 14, 15};
    k = ecore_pick(c4, i4, 6, out);                      // classes interleaved
    CHECK(k == 2 && out[0] == 14 && out[1] == 15);
    CHECK(ecore_pick(c3, i3, 0, out) == 0);
    // [UIT 0237] frame_core_split: an i9-12900K: 8 P-cores x 2 hyperthreads (class 1; both threads of a core share
    // its CoreIndex), then 8 E-cores (class 0, one CPU each, CoreIndex 8..15)
    {
        uint8_t c[24], co[24]; uint32_t id[24], fr[24], rest[24]; int nf = -1;
        for (int i = 0; i < 24; i++) { c[i] = i < 16; id[i] = 256 + i; co[i] = i < 16 ? (uint8_t)(i / 2) : (uint8_t)(8 + i - 16); }
        int nr = frame_core_split(c, id, co, 24, fr, &nf, rest);
        CHECK(nr == 14 && nf == 2);                              // core 7 (both threads) for the frame thread, 7 cores left
        CHECK(fr[0] == 270 && fr[1] == 271);
        int bad = 0;
        for (int i = 0; i < nr; i++) { if (rest[i] == 270 || rest[i] == 271 || rest[i] >= 272) bad++; }
        CHECK(!bad && rest[0] == 256 && rest[13] == 269);        // no E-core and no frame-core CPU among the rest
        // the 8 E-cores have higher CoreIndex values (8..15): still only P-cores are considered
        // hyperthreading off: one CPU per P-core
        uint8_t c2[16], co2[16]; uint32_t id2[16];
        for (int i = 0; i < 16; i++) { c2[i] = i < 8; id2[i] = 300 + i; co2[i] = (uint8_t)i; }
        nr = frame_core_split(c2, id2, co2, 16, fr, &nf, rest);
        CHECK(nr == 7 && nf == 1 && fr[0] == 307);
        // E-cores listed first (CoreIndex 0..3 for E, 4..7 for P)
        uint8_t c3b[12] = {0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1}, co3[12] = {0, 1, 2, 3, 4, 4, 5, 5, 6, 6, 7, 7};
        uint32_t id3[12] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12};
        nr = frame_core_split(c3b, id3, co3, 12, fr, &nf, rest);
        CHECK(nr == 6 && nf == 2 && fr[0] == 11 && fr[1] == 12);
        // only 3 physical P-cores (e.g. a 2+8 laptop with HT gives 2): nothing split
        uint8_t c4b[10] = {1, 1, 1, 1, 1, 1, 0, 0, 0, 0}, co4[10] = {0, 0, 1, 1, 2, 2, 3, 4, 5, 6};
        nf = -1;
        CHECK(frame_core_split(c4b, id3, co4, 10, fr, &nf, rest) == 0 && nf == 0);
        // not hybrid (Ryzen): nothing split
        uint8_t c5[16] = {0}, co5[16];
        for (int i = 0; i < 16; i++) co5[i] = (uint8_t)(i / 2);
        CHECK(frame_core_split(c5, id2, co5, 16, fr, &nf, rest) == 0 && nf == 0);
        CHECK(frame_core_split(c5, id2, co5, 0, fr, &nf, rest) == 0 && nf == 0);
        // Core Ultra: 3 classes; only the fastest (2) counts: 6 P-cores
        uint8_t c6[14] = {0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 2, 2, 1, 1}, co6[14] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13};
        uint32_t id6[14];
        for (int i = 0; i < 14; i++) id6[i] = 400 + i;
        nr = frame_core_split(c6, id6, co6, 14, fr, &nf, rest);
        CHECK(nr == 5 && nf == 1 && fr[0] == 411);
    }
    printf("%s\n", fails ? "FAILED" : "pcores: all ok");
    return fails != 0;
}
