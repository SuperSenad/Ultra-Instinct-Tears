// [UIT 0203] Box test: vkb_linew.h (the width the replay sets) and the plumbing from nvnCommandBufferSetLineWidth to
// vkCmdSetLineWidth (source checks).
// cc -Wall -Werror -Isrc/gfx tools/tests/test_line_width.c -o /tmp/t && /tmp/t   (from the repo root)
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "vkb_linew.h"
static int fails;
#define CHECK(c, m) do { if (!(c)) { fails++; printf("FAIL: %s\n", m); } } while (0)
static char *slurp(const char *p) {
    FILE *f = fopen(p, "rb"); if (!f) return NULL;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    char *b = malloc((size_t)n + 1); b[fread(b, 1, (size_t)n, f)] = 0; fclose(f); return b;
}
static int count(const char *s, const char *pat) { int n = 0; for (const char *p = s; (p = strstr(p, pat)); p++) n++; return n; }
int main(void) {
    CHECK(vkb_line_width(4.5f, 1, 1, 1.0f, 64.0f) == 4.5f, "TOTK's 4.5 on an RTX (range 1..64)");
    CHECK(vkb_line_width(4.5f, 0, 1, 1.0f, 64.0f) == 1.0f, "off: 1 pixel");
    CHECK(vkb_line_width(4.5f, 1, 0, 1.0f, 1.0f) == 1.0f, "no wideLines: 1 pixel (anything else is invalid)");
    CHECK(vkb_line_width(0.0f, 1, 1, 1.0f, 64.0f) == 1.0f, "never set: 1 pixel");
    CHECK(vkb_line_width(NAN, 1, 1, 1.0f, 64.0f) == 1.0f, "NaN: 1 pixel");
    CHECK(vkb_line_width(-3.0f, 1, 1, 1.0f, 64.0f) == 1.0f, "negative: 1 pixel");
    CHECK(vkb_line_width(100.0f, 1, 1, 1.0f, 64.0f) == 64.0f, "clamped to the GPU's maximum");
    CHECK(vkb_line_width(0.5f, 1, 1, 0.0f, 64.0f) == 1.0f, "at least one pixel");
    CHECK(vkb_line_width(8.0f, 1, 1, 1.0f, 7.9921875f) == 7.9921875f, "a smaller range (8 - 1/128)");
    struct { const char *file, *pat; int n; } src[] = {
        {"src/gfx/nvn/nvn_obj.h", "OP_PRIM_RESTART, OP_LINE_WIDTH, OP_COUNT_", 1},
        {"src/gfx/nvn/hle_nvn.c", "REC(c, OP_LINE_WIDTH, p_line_width)", 1},
        {"src/gfx/nvn/totk_nvn_ext.c", "NVN(nvnCommandBufferSetLineWidth", 0},
        {"src/gfx/vkb_draw.c", "case OP_LINE_WIDTH: S.line_w =", 1},
        {"src/gfx/vkb_draw.c", "vkb_line_width(S.line_w, line_width_on(), vkc.has_wide_lines", 1},
        {"src/gfx/backend_vk.c", "case OP_LINE_WIDTH:", 2},
        {"src/vk/vk.c", "vkc.has_wide_lines = f2.features.wideLines", 1},
    };
    for (unsigned i = 0; i < sizeof src / sizeof *src; i++) {
        char *s = slurp(src[i].file);
        if (!s) { CHECK(0, src[i].file); continue; }
        int n = count(s, src[i].pat);
        if (n != src[i].n) { fails++; printf("FAIL: %s: '%s' %d times, expected %d\n", src[i].file, src[i].pat, n, src[i].n); }
        free(s);
    }
    printf(fails ? "FAIL\n" : "ok\n");
    return fails != 0;
}
