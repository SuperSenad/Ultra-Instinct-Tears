// [UIT 0200] The image view cache key (src/gfx/vkb_viewkey.h), from deko-1031 0108, plus the 0124 swizzle rule.
// cc -O1 -Wall -Werror -Isrc/gfx -Isrc/gfx/nvn -Isrc/gfx/nvn/include tools/tests/test_view_key.c -o /tmp/t && /tmp/t
#include <stdio.h>
#include <stdlib.h>
#include "nvn/nvn_obj.h"
#include "vkb_viewkey.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static int same(const view_t *a, int aa, int at, const view_t *b, int ba, int bt, int fixed) {
    uint8_t x[VKB_VIEW_KEY], y[VKB_VIEW_KEY];
    vkb_view_key(x, a, aa, at, 0, fixed);
    vkb_view_key(y, b, ba, bt, 0, fixed);
    return !memcmp(x, y, sizeof x);
}

int main(void) {
    view_t a, b;
    memset(&a, 0, sizeof a); memset(&b, 0xA5, sizeof b);
    b.set_levels = b.set_layers = b.set_format = b.set_target = b.set_swizzle = 0;   // nothing set, garbage elsewhere
    CHECK(same(&a, 0, 1, &b, 0, 1, 1), "unset fields must not matter");
    CHECK(!same(&a, 0, 1, &b, 0, 1, 0), "the old key compared them (the bug)");
    memset(&a, 0, sizeof a); memset(&b, 0x5A, sizeof b);
    a.set_levels = b.set_levels = 1; a.base_level = b.base_level = 2; a.nlevels = b.nlevels = 1;
    b.set_layers = b.set_format = b.set_target = b.set_swizzle = 0;
    CHECK(same(&a, 0, 1, &b, 0, 1, 1), "levels set equal, rest garbage");
    b.base_level = 3;
    CHECK(!same(&a, 0, 1, &b, 0, 1, 1), "different level");
    memset(&a, 0, sizeof a); memset(&b, 0, sizeof b);
    a.set_format = b.set_format = 1; a.format = 0x25; b.format = 0x26;
    CHECK(!same(&a, 0, 1, &b, 0, 1, 1), "different format");
    b.format = 0x25; b.set_swizzle = 1; b.swz[0] = 3;
    CHECK(!same(&a, 0, 1, &b, 0, 1, 1), "swizzle set on one");
    memset(&b, 0, sizeof b); b.set_format = 1; b.format = 0x25;
    CHECK(same(&a, 0, 1, &b, 0, 1, 1), "equal views");
    CHECK(!same(&a, 1, 0, &b, 2, 0, 1), "render target vs storage view must differ");
    CHECK(same(&a, 1, 0, &b, 1, 5, 1), "attachment: view type ignored");
    CHECK(!same(&a, 0, 1, &b, 0, 2, 1), "shader view types differ");
    CHECK(!same(&a, 0, -1, &b, 1, 0, 1), "sampled (no type) vs attachment");
    CHECK(same(NULL, 0, 1, NULL, 0, 1, 1), "no view");
    uint8_t x[VKB_VIEW_KEY], y[VKB_VIEW_KEY];
    vkb_view_key(x, &a, 0, 1, 2, 1); vkb_view_key(y, &a, 0, 1, 3, 1);
    CHECK(memcmp(x, y, sizeof x), "resident base");
    // every set bit combination with random garbage elsewhere: equal keys exactly when the set fields are equal
    srand(7);
    for (int it = 0; it < 200000; it++) {
        view_t p, q;
        for (int k = 0; k < 16; k++) { ((uint8_t *)&p)[k] = (uint8_t)rand(); ((uint8_t *)&q)[k] = (uint8_t)rand(); }
        q.set_levels = p.set_levels; q.set_layers = p.set_layers; q.set_format = p.set_format; q.set_target = p.set_target; q.set_swizzle = p.set_swizzle;
        if (rand() & 1) { q.base_level = p.base_level; q.nlevels = p.nlevels; }
        if (rand() & 1) { q.min_layer = p.min_layer; q.nlayers = p.nlayers; }
        if (rand() & 1) q.format = p.format;
        if (rand() & 1) q.target = p.target;
        if (rand() & 1) memcpy(q.swz, p.swz, 4);
        int eq = (!p.set_levels || (p.base_level == q.base_level && p.nlevels == q.nlevels)) &&
                 (!p.set_layers || (p.min_layer == q.min_layer && p.nlayers == q.nlayers)) &&
                 (!p.set_format || p.format == q.format) && (!p.set_target || p.target == q.target) &&
                 (!p.set_swizzle || !memcmp(p.swz, q.swz, 4));
        if (same(&p, 0, 1, &q, 0, 1, 1) != eq) { CHECK(0, "random view %d: key equality %d, fields %d", it, !eq, eq); break; }
    }
    // [UIT 0124] targets and storage views take no swizzle, in both keys; sampled views keep it
    memset(&a, 0, sizeof a); memset(&b, 0, sizeof b);
    b.set_swizzle = 1; b.swz[0] = 3; b.swz[1] = 2;
    for (int fx = 0; fx < 2; fx++) {
        CHECK(same(&a, 1, 0, &b, 1, 0, fx), "render target: swizzle ignored (fixed %d)", fx);
        CHECK(same(&a, 2, 0, &b, 2, 0, fx), "storage: swizzle ignored (fixed %d)", fx);
        CHECK(!same(&a, 0, 1, &b, 0, 1, fx), "sampled: swizzle kept (fixed %d)", fx);
    }
    CHECK(same(&a, 1, 0, &b, 2, 0, 0), "the old key: target and storage shared one key (kept for A/B)");
    // the inline + block layout covers exactly VKB_VIEW_MAX views
    CHECK(VKB_VIEWS_INLINE + VKB_VIEW_BLOCK * VKB_VIEW_BLOCKS == VKB_VIEW_MAX, "layout");
    CHECK((VKB_VIEW_MAX - 1 - VKB_VIEWS_INLINE) / VKB_VIEW_BLOCK == VKB_VIEW_BLOCKS - 1, "last index in the last block");
    CHECK(VKB_VIEW_MAX == 480, "capacity");
    if (fails) { printf("%d failures\n", fails); return 1; }
    printf("ok\n");
    return 0;
}
