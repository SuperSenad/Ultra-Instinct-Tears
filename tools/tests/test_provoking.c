// [UIT 0205] Box test of vkb_provoke.h and its plumbing (source checks; run from the repo root).
// cc -Wall -Werror -Isrc/gfx -Isrc/vk/include tools/tests/test_provoking.c -o /tmp/t && /tmp/t
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "vkb_provoke.h"
static int fails;
#define CHECK(c, m) do { if (!(c)) { fails++; printf("FAIL: %s\n", m); } } while (0)
static char *slurp(const char *p) {
    FILE *f = fopen(p, "rb"); if (!f) return NULL;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    char *b = malloc((size_t)n + 1); b[fread(b, 1, (size_t)n, f)] = 0; fclose(f); return b;
}
int main(void) {
    CHECK(vkb_provoking_mode(0, 1) == VKB_PV_FIRST, "default: first vertex (as before)");
    CHECK(vkb_provoking_mode(1, 1) == VKB_PV_LAST, "on and supported: last vertex");
    CHECK(vkb_provoking_mode(1, 0) == VKB_PV_FIRST, "on but not supported: first (LAST would be invalid)");
    CHECK(VKB_PV_FIRST == 0 && VKB_PV_LAST == 1, "VkProvokingVertexModeEXT values (FIRST_VERTEX 0, LAST_VERTEX 1)");
    char *d = slurp("src/gfx/vkb_draw.c"), *v = slurp("src/vk/vk.c");
    CHECK(d && strstr(d, "vkb_provoking_mode(provoking_last(), vkc.has_pv_last)"), "vkb_draw.c sets the mode through vkb_provoking_mode");
    CHECK(d && strstr(d, "r = e && *e == '1';"), "default off unless TOTK_VK_PROVOKING_LAST=1");
    CHECK(v && strstr(v, "vkc.has_pv_last = fpv.provokingVertexLast;"), "vk.c reads the feature");
    printf(fails ? "FAIL\n" : "ok\n");
    return fails != 0;
}
