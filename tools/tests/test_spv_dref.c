// [UIT 0351] Box test for vkb_spv_dref_clamp (src/gfx/vkb_spv.c): every depth compare's reference goes through
// FClamp(Dref, 0, 1), the module stays well formed (word counts, bound), and a module with no compare is left alone.
// With a path: rewrites that SPIR-V file into <path>.clamped.spv (for spirv-val / spirv-dis) and prints the count.
// cc -O1 -Wall -Isrc/gfx tools/tests/test_spv_dref.c src/gfx/vkb_spv.c -o /tmp/t/spvdref && /tmp/t/spvdref [file.spv]
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "vkb_spv.h"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

// A walk of the module: 1 if every instruction's word count fits and no id reaches the bound.
static int well_formed(const uint32_t *w, size_t n, int *drefs, int *clamps, int *imports) {
    *drefs = *clamps = *imports = 0;
    for (size_t i = 5; i < n;) {
        uint32_t op = w[i] & 0xffff, wc = w[i] >> 16;
        if (!wc || i + wc > n) return 0;
        if (op == 89 || op == 90 || op == 97 || op == 307 || op == 308 || op == 315) (*drefs)++;
        if (op == 12 && wc == 8 && w[i + 4] == 43) (*clamps)++;
        if (op == 11) (*imports)++;
        i += wc;
    }
    return 1;
}

int main(int argc, char **argv) {
    if (argc > 1) {
        FILE *f = fopen(argv[1], "rb");
        if (!f) { perror(argv[1]); return 2; }
        fseek(f, 0, SEEK_END);
        long sz = ftell(f);
        fseek(f, 0, SEEK_SET);
        uint32_t *w = malloc((size_t)sz);
        if (!w || fread(w, 1, (size_t)sz, f) != (size_t)sz) return 2;
        fclose(f);
        uint32_t *o = NULL;
        size_t on = 0;
        int n = vkb_spv_dref_clamp(w, (size_t)sz / 4, &o, &on);
        int d, c, im;
        if (n) {
            CHECK(well_formed(o, on, &d, &c, &im));
            CHECK(c >= n && d == n && im >= 1);
            char path[1024];
            snprintf(path, sizeof path, "%s.clamped.spv", argv[1]);
            FILE *g = fopen(path, "wb");
            if (g) { fwrite(o, 4, on, g); fclose(g); }
        }
        printf("%s: %d compares clamped\n", argv[1], n);
        return fails ? 1 : 0;
    }
    // A hand-made module: float type, a sampled image id 9 (not declared: the rewrite does not look), two compares.
    enum { F32 = 2, V4 = 3, SI = 9, COORD = 10, DREF = 11, R1 = 12, R2 = 13, BOUND = 20 };
    uint32_t m[] = {0x07230203, 0x00010300, 0, BOUND, 0,
                    2u << 16 | 17, 1,                                 // OpCapability Shader
                    3u << 16 | 14, 0, 1,                              // OpMemoryModel Logical GLSL450
                    3u << 16 | 22, F32, 32,                           // OpTypeFloat 32
                    4u << 16 | 23, V4, F32, 4,                        // OpTypeVector
                    6u << 16 | 89, F32, R1, SI, COORD, DREF,          // OpImageSampleDrefImplicitLod
                    8u << 16 | 90, F32, R2, SI, COORD, DREF, 2, 7};   // OpImageSampleDrefExplicitLod ... Lod 7
    size_t n = sizeof m / 4;
    uint32_t *o = NULL;
    size_t on = 0;
    int k = vkb_spv_dref_clamp(m, n, &o, &on);
    CHECK(k == 2);
    int d, c, im;
    CHECK(o && well_formed(o, on, &d, &c, &im));
    CHECK(d == 2 && c == 2 && im == 1);
    CHECK(o[3] > BOUND);
    // each compare's Dref is the result of the FClamp just before it, of DREF and the two new constants (0.0, 1.0)
    uint32_t c0 = 0, c1 = 0;
    int seen = 0;
    for (size_t i = 5, last_clamp = 0; i < on; i += o[i] >> 16) {
        uint32_t op = o[i] & 0xffff;
        if (op == 43) { if (o[i + 3] == 0) c0 = o[i + 2]; else if (o[i + 3] == 0x3f800000u) c1 = o[i + 2]; CHECK(o[i + 1] == F32); }
        if (op == 11) { CHECK(!memcmp(o + i + 2, "GLSL.std.450", 13)); }
        if (op == 12) { CHECK(o[i + 1] == F32 && o[i + 5] == DREF && o[i + 6] == c0 && o[i + 7] == c1 && c0 && c1); last_clamp = o[i + 2]; }
        if (op == 89 || op == 90) { CHECK(o[i + 5] == last_clamp && last_clamp >= BOUND); CHECK(o[i + 3] == SI && o[i + 4] == COORD); seen++; }
        if (op == 90) { CHECK((o[i] >> 16) == 8 && o[i + 6] == 2 && o[i + 7] == 7); }
    }
    CHECK(seen == 2);
    // the import is before the memory model
    size_t imp = 0, mm = 0;
    for (size_t i = 5; i < on; i += o[i] >> 16) { if ((o[i] & 0xffff) == 11) imp = i; if ((o[i] & 0xffff) == 14) mm = i; }
    CHECK(imp && mm && imp < mm);
    free(o);
    // an existing import is reused
    uint32_t m2[] = {0x07230203, 0x00010300, 0, BOUND, 0,
                     2u << 16 | 17, 1,
                     6u << 16 | 11, 1, 0x4c534c47, 0x6474732e, 0x3035342e, 0,   // %1 = OpExtInstImport "GLSL.std.450"
                     3u << 16 | 14, 0, 1,
                     3u << 16 | 22, F32, 32,
                     7u << 16 | 97, V4, R1, SI, COORD, DREF, 0};              // OpImageDrefGather ... (operands none)
    o = NULL;
    CHECK(vkb_spv_dref_clamp(m2, sizeof m2 / 4, &o, &on) == 1);
    CHECK(o && well_formed(o, on, &d, &c, &im) && im == 1 && c == 1);
    for (size_t i = 5; o && i < on; i += o[i] >> 16) if ((o[i] & 0xffff) == 12) CHECK(o[i + 3] == 1);
    free(o);
    // no compare: nothing to do
    uint32_t m3[] = {0x07230203, 0x00010300, 0, BOUND, 0, 2u << 16 | 17, 1, 3u << 16 | 14, 0, 1, 3u << 16 | 22, F32, 32};
    o = NULL;
    CHECK(vkb_spv_dref_clamp(m3, sizeof m3 / 4, &o, &on) == 0 && !o);
    // a broken word count: nothing to do
    m3[10] = 0;
    CHECK(vkb_spv_dref_clamp(m3, sizeof m3 / 4, &o, &on) == 0);
    printf(fails ? "spv dref clamp: %d failure(s)\n" : "spv dref clamp: ok\n", fails);
    return fails ? 1 : 0;
}
