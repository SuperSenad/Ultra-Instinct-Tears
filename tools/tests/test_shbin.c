// [UIT 0034] Box test for vkb_shbin.h (TOTK_SHADER_BINARY_CACHE): a cache file is used only when everything matches.
// cc -O2 -Wall -Isrc/gfx tools/tests/test_shbin.c -o /tmp/t/shbin && /tmp/t/shbin
#include <stdio.h>
#include <stdlib.h>
#include "vkb_shbin.h"
static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)
static size_t make(uint8_t *out, const uint8_t uuid[16], uint32_t drv, uint32_t st, shbin_key k, const uint8_t *d, size_t n) {
    shbin_hdr h; shbin_fill(&h, uuid, drv, st, k, d, n);
    memcpy(out, &h, sizeof h); memcpy(out + sizeof h, d, n); return sizeof h + n;
}
int main(void) {
    uint32_t spv[64];
    for (int i = 0; i < 64; i++) spv[i] = 0x07230203u * (i + 1);
    uint8_t uuid[16] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16}, uuid2[16];
    memcpy(uuid2, uuid, 16); uuid2[7] ^= 1;
    uint8_t bin[300];
    for (int i = 0; i < 300; i++) bin[i] = (uint8_t)(i * 7 + 3);
    shbin_key k = shbin_make_key(spv, sizeof spv, 1, 16, 42);
    static uint8_t f[4096]; size_t n = make(f, uuid, 7, 1, k, bin, sizeof bin), dn = 0;
    const void *d = shbin_check(f, n, uuid, 7, 1, k, &dn);
    CHECK(d && dn == sizeof bin && !memcmp(d, bin, dn));
    CHECK(!shbin_check(f, n, uuid2, 7, 1, k, &dn));   // another GPU / driver build
    CHECK(!shbin_check(f, n, uuid, 8, 1, k, &dn));    // driver version
    CHECK(!shbin_check(f, n, uuid, 7, 16, k, &dn));   // stage
    CHECK(!shbin_check(f, n - 1, uuid, 7, 1, k, &dn));   // truncated
    CHECK(!shbin_check(f, 10, uuid, 7, 1, k, &dn));
    CHECK(!shbin_check(NULL, 0, uuid, 7, 1, k, &dn));
    f[sizeof(shbin_hdr) + 100] ^= 0x40;   // damaged
    CHECK(!shbin_check(f, n, uuid, 7, 1, k, &dn));
    f[sizeof(shbin_hdr) + 100] ^= 0x40;
    CHECK(shbin_check(f, n, uuid, 7, 1, k, &dn));
    // the key changes with anything that decides the binary
    shbin_key k2;
    k2 = shbin_make_key(spv, sizeof spv, 1, 16, 43); CHECK(k2.a != k.a && k2.b != k.b);    // layout
    k2 = shbin_make_key(spv, sizeof spv, 1, 0, 42); CHECK(k2.a != k.a && k2.b != k.b);     // next stage
    k2 = shbin_make_key(spv, sizeof spv - 4, 1, 16, 42); CHECK(k2.a != k.a && k2.b != k.b);   // size
    CHECK(!shbin_check(f, n, uuid, 7, 1, k2, &dn));
    // [UIT 0136] a file from 0135 or before ("UIB1" header) is refused, so it is made again from SPIR-V
    { static uint8_t g[4096]; memcpy(g, f, n); uint32_t old = 0x31424955u; memcpy(g, &old, 4);
      CHECK(!shbin_check(g, n, uuid, 7, 1, k, &dn)); }
    // [UIT 0138] nor one from 0136 / 0137 ("UIB2", loaded by the two runs that lost the device on 2026-10-31)
    { static uint8_t g[4096]; memcpy(g, f, n); uint32_t old = 0x32424955u; memcpy(g, &old, 4);
      CHECK(!shbin_check(g, n, uuid, 7, 1, k, &dn)); CHECK(SHBIN_MAGIC == 0x33424955u); }
    // the device fingerprint (vkc.dev_fp, mixed into the layout value as vkb_shader.c does) changes the key
    { uint64_t fp1 = 0x1111, fp2 = 0x1112, l1 = shbin_fnv(&fp1, 8, shbin_fnv("UIT-0136", 8, 42)), l2 = shbin_fnv(&fp2, 8, shbin_fnv("UIT-0136", 8, 42));
      shbin_key a1 = shbin_make_key(spv, sizeof spv, 1, 16, l1), a2 = shbin_make_key(spv, sizeof spv, 1, 16, l2);
      CHECK(a1.a != a2.a && a1.b != a2.b && a1.a != k.a); }
    // every one-bit change of the SPIR-V changes both halves; no collisions over many random shaders
    int same = 0;
    for (int w = 0; w < 64; w++)
        for (int bit = 0; bit < 32; bit++) {
            spv[w] ^= 1u << bit;
            k2 = shbin_make_key(spv, sizeof spv, 1, 16, 42);
            same += k2.a == k.a || k2.b == k.b;
            spv[w] ^= 1u << bit;
        }
    CHECK(same == 0);
    enum { N = 200000 };
    uint64_t *a = malloc(N * sizeof *a);
    srand(1);
    for (int i = 0; i < N; i++) {
        for (int w = 0; w < 16; w++) spv[w] = (uint32_t)rand() ^ (uint32_t)rand() << 16;
        a[i] = shbin_make_key(spv, 64, 1, 16, 42).a;
    }
    int col = 0;
    for (int i = 0; i < N; i++) for (int j = i + 1; j < i + 64 && j < N; j++) col += a[i] == a[j];
    CHECK(col == 0);
    free(a);
    printf("%s\n", fails ? "FAILED" : "shbin: all ok");
    return fails != 0;
}
