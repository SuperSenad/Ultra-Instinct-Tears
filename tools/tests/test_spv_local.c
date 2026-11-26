// [UIT 0391] vkb_spv_set_local_size (src/gfx/vkb_spv.c): a compute module's OpExecutionMode LocalSize takes the
// thread-group size of the game's program (its control section +0x748), as translated with 1 1 1 before. Modules made
// here word by word: the shape the translator emits (capability, memory model, entry point, LocalSize, a function),
// one with only LocalSizeId, one with a WorkgroupSize built-in, and cut-short / non-SPIR-V input.
//   cc -O2 -Wall -Isrc/gfx tools/tests/test_spv_local.c src/gfx/vkb_spv.c -o /tmp/tsl && /tmp/tsl
#include <stdio.h>
#include <string.h>
#include "vkb_spv.h"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)
#define OP(op, wc) (((uint32_t)(wc) << 16) | (op))

int main(void) {
    uint32_t m[] = {
        0x07230203, 0x00010300, 0, 20, 0,
        OP(17, 2), 1,                                  // OpCapability Shader
        OP(14, 3), 0, 1,                               // OpMemoryModel Logical GLSL450
        OP(15, 5), 5, 4, 0x6e69616d, 0,                // OpEntryPoint GLCompute %4 "main"
        OP(16, 6), 4, 17, 1, 1, 1,                     // OpExecutionMode %4 LocalSize 1 1 1
        OP(19, 2), 2,                                  // OpTypeVoid %2
        OP(33, 3), 3, 2,                               // OpTypeFunction %3 %2
        OP(54, 5), 2, 4, 0, 3,                         // OpFunction
        OP(248, 2), 5, OP(253, 1), OP(56, 1),          // OpLabel, OpReturn, OpFunctionEnd
    };
    size_t n = sizeof m / sizeof m[0];
    const uint32_t s64[3] = {64, 1, 1}, s16[3] = {16, 16, 1};
    CHECK(vkb_spv_set_local_size(m, n, s64) == 1);
    CHECK(m[18] == 64 && m[19] == 1 && m[20] == 1);
    CHECK(vkb_spv_set_local_size(m, n, s64) == 0);   // again: nothing to do
    CHECK(vkb_spv_set_local_size(m, n, s16) == 1);
    CHECK(m[18] == 16 && m[19] == 16 && m[20] == 1);
    CHECK(m[0] == 0x07230203 && m[22] == 2);         // nothing else moved

    uint32_t id[sizeof m / sizeof m[0]];
    memcpy(id, m, sizeof m);
    id[15] = OP(331, 6); id[17] = 38;                // OpExecutionModeId LocalSizeId: not a literal, left alone
    CHECK(vkb_spv_set_local_size(id, n, s64) == -1);
    CHECK(id[18] == 16);

    uint32_t wg[sizeof m / sizeof m[0] + 4];
    memcpy(wg, m, 21 * 4);
    wg[21] = OP(71, 4); wg[22] = 9; wg[23] = 11; wg[24] = 25;   // OpDecorate %9 BuiltIn WorkgroupSize
    memcpy(wg + 25, m + 21, (n - 21) * 4);
    CHECK(vkb_spv_set_local_size(wg, n + 4, s64) == -2);
    CHECK(wg[18] == 16);

    CHECK(vkb_spv_set_local_size(m, 17, s64) == -1);   // cut inside the LocalSize instruction
    uint32_t bad[8] = {0x12345678};
    CHECK(vkb_spv_set_local_size(bad, 8, s64) == -1);
    uint32_t zero[] = {0x07230203, 0x00010000, 0, 4, 0, 0};   // a zero word count: no endless loop
    CHECK(vkb_spv_set_local_size(zero, 6, s64) == -1);
    printf(fails ? "test_spv_local: %d FAILED\n" : "test_spv_local: ok\n", fails);
    return fails != 0;
}
