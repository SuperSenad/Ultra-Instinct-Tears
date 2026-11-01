// [UIT 0158] tests for primitive restart in the Vulkan replay: src/gfx/vkb_restart.h (when a draw restarts), the recorded
// command (src/gfx/nvn/nvn_obj.h) and what a 16-bit terrain strip split by 0xFFFF draws with and without restart.
// cc -O2 -Isrc/gfx -Isrc/gfx/nvn -Isrc/gfx/nvn/include tools/tests/test_prim_restart.c -o /tmp/t/prs && /tmp/t/prs .
#include "vkb_restart.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int fails, checks;
#define CHECK(c) do { checks++; if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

// Triangle-strip assembly as a GPU does it: restart (when on) at 0xFFFF; returns the triangles, vertex indices with the
// base vertex added. Degenerate triangles (two equal indices) are dropped, as rasterisers draw nothing for them.
static int strip_tris(const uint16_t *ix, int n, int restart, int base, int (*out)[3], int max) {
    int k = 0, run = 0, a = 0, b = 0;
    for (int i = 0; i < n; i++) {
        if (restart && ix[i] == 0xFFFF) { run = 0; continue; }
        int v = ix[i] + base;
        if (run >= 2 && k < max) {
            int t[3] = {a, b, v};
            if (run & 1) { t[0] = b; t[1] = a; }   // winding alternates
            if (t[0] != t[1] && t[1] != t[2] && t[0] != t[2]) memcpy(out[k++], t, sizeof t);
        }
        a = b; b = v; run++;
    }
    return k;
}

int main(int argc, char **argv) {
    // when a draw restarts
    for (int strip = 0; strip < 2; strip++)
        for (int set = 0; set < 2; set++)
            for (int on = 0; on < 2; on++) {
                CHECK(vkb_restart_for(RS_OFF, strip, set, on) == 0);           // 0: as before 0158
                CHECK(vkb_restart_for(RS_STRIPS, strip, set, on) == strip);    // strips: every strip and fan
                CHECK(vkb_restart_for(RS_GAME, strip, set, on) == (strip && (!set || on)));   // default: the game's, on until set
            }
    CHECK(vkb_restart_for(RS_GAME, 0, 1, 1) == 0);   // lists never (Vulkan: primitiveTopologyListRestart)

    // A 3x3-vertex terrain patch (two rows of quads) drawn as two strips split by 0xFFFF, with a base vertex, in a
    // vertex buffer that also holds vertex 0xFFFF + base (TOTK's terrain stream holds 281515 vertices).
    const int base = 5864;
    const uint16_t ix[] = {0, 3, 1, 4, 2, 5, 0xFFFF, 3, 6, 4, 7, 5, 8};
    int tri[32][3];
    int n_on = strip_tris(ix, 13, 1, base, tri, 32);
    CHECK(n_on == 8);   // 2 rows x 2 quads x 2 triangles
    for (int i = 0; i < n_on; i++)
        for (int j = 0; j < 3; j++) CHECK(tri[i][j] >= base && tri[i][j] <= base + 8);   // only the patch's own vertices
    int n_off = strip_tris(ix, 13, 0, base, tri, 32);
    int stray = 0;
    for (int i = 0; i < n_off; i++)
        for (int j = 0; j < 3; j++) if (tri[i][j] == base + 0xFFFF) { stray++; break; }
    CHECK(n_off == 11);   // the 8 plus 3 joining the rows through vertex 0xFFFF + base: the floating triangles
    CHECK(stray == 3);

    // the game's SetPrimitiveRestart reaches the replay: recorded (hle_nvn.c), executed as state (backend_vk.c, both
    // walks), kept in the draw state (vkb_draw.c) and used for vkCmdSetPrimitiveRestartEnable (not a constant)
    static const struct { const char *file, *text; int min; } src[] = {
        {"src/gfx/nvn/nvn_obj.h", "OP_PRIM_RESTART, ", 1},   // [UIT 0203] OP_LINE_WIDTH follows it now
        {"src/gfx/nvn/hle_nvn.c", "REC(c, OP_PRIM_RESTART, p_prim_restart)", 1},
        {"src/gfx/backend_vk.c", "case OP_RASTER_DISCARD: case OP_PRIM_RESTART:", 2},
        {"src/gfx/vkb_draw.c", "case OP_PRIM_RESTART: S.restart_set = 1;", 1},
        {"src/gfx/vkb_draw.c", "DYN(D_RESTART, VkBool32, (VkBool32)on,", 1},
    };
    for (size_t i = 0; i < sizeof src / sizeof src[0]; i++) {
        char path[512]; snprintf(path, sizeof path, "%s/%s", argc > 1 ? argv[1] : ".", src[i].file);
        FILE *f = fopen(path, "rb");
        CHECK(f != NULL);
        if (!f) continue;
        static char buf[4 << 20]; size_t len = fread(buf, 1, sizeof buf - 1, f); fclose(f); buf[len] = 0;
        int count = 0;
        for (const char *q = buf; (q = strstr(q, src[i].text)); q++) count++;
        if (count < src[i].min) printf("missing in %s: %s\n", src[i].file, src[i].text);
        CHECK(count >= src[i].min);
    }
    {   // the old constant is gone
        char path[512]; snprintf(path, sizeof path, "%s/src/gfx/vkb_draw.c", argc > 1 ? argv[1] : ".");
        FILE *f = fopen(path, "rb"); static char buf[4 << 20]; size_t len = f ? fread(buf, 1, sizeof buf - 1, f) : 0; if (f) fclose(f); buf[len] = 0;
        CHECK(len > 0 && !strstr(buf, "DYN(D_RESTART, VkBool32, VK_FALSE"));
    }
    printf("%s: %d checks, %d failed\n", fails ? "FAIL" : "PASS", checks, fails);
    return fails != 0;
}
