// [UIT 0207] Box test of vkb_sampkeep.h: the outcomes of sampler()'s race with another thread and with mtl_reg_samp.
// cc -Wall -Werror -Isrc/gfx tools/tests/test_sampler_keep.c -o /tmp/t && /tmp/t
#include <stdio.h>
#include "vkb_sampkeep.h"
static int fails;
#define CHECK(c, m) do { if (!(c)) { fails++; printf("FAIL: %s\n", m); } } while (0)
int main(void) {
    for (int fr = 0; fr < 2; fr++) {
        CHECK(vkb_samp_keep(1, 1, 0, fr) == VKB_SAMP_KEEP, "unchanged, empty slot: kept by the registry");
        CHECK(vkb_samp_keep(1, 1, 1, fr) == VKB_SAMP_USE_OTHER, "another thread stored one: use it, ours never used");
        int un = fr ? VKB_SAMP_ONE_USE : VKB_SAMP_LEAK;
        CHECK(vkb_samp_keep(1, 0, 0, fr) == un, "re-registered with other parameters");
        CHECK(vkb_samp_keep(0, 0, 0, fr) == un, "unregistered meanwhile");
        CHECK(vkb_samp_keep(0, 1, 1, fr) == un, "not valid: never kept nor swapped");
    }
    // every outcome: exactly one owner (registry, destroyed now, destroyed after the submission) unless leaking is chosen
    int owners = 0;
    for (int a = 0; a < 2; a++) for (int b = 0; b < 2; b++) for (int c = 0; c < 2; c++) owners += vkb_samp_keep(a, b, c, 1) != VKB_SAMP_LEAK;
    CHECK(owners == 8, "with the fix nothing leaks");
    printf(fails ? "FAIL\n" : "ok\n");
    return fails != 0;
}
