// [UIT 0204] Whether a draw's depth bias is on: NVN enables polygon offset per polygon mode (NVNpolygonOffsetEnable:
// POINT 1, LINE 2, FILL 4; NVNpolygonMode: POINT 0, LINE 1, FILL 2), Vulkan has one switch for the draw. The replay
// used the FILL bit whatever the mode, so a triangle drawn as lines or points with its own mode's bit had no bias (and
// one with only the FILL bit had it). by_mode=0 (TOTK_VK_POLY_OFFSET_MODE=0): as before. Box test:
// tools/tests/test_poly_offset.c.
#pragma once
static inline int vkb_bias_on(int has_poly, unsigned offset_en, unsigned mode, int by_mode) {
    if (!has_poly) return 0;
    if (!by_mode || mode > 2) return (offset_en & 4) != 0;
    return (offset_en >> mode & 1) != 0;
}
