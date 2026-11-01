// [UIT 0203] The line width a draw sets (vkCmdSetLineWidth) from the game's nvnCommandBufferSetLineWidth. Box test:
// tools/tests/test_line_width.c. Without the wideLines feature Vulkan only accepts 1.0; with it, the GPU's
// lineWidthRange. Not set (0), not a number, or off: 1 pixel, as before.
#pragma once
static inline float vkb_line_width(float game, int on, int wide, float lo, float hi) {
    if (!on || !wide || !(game > 0.0f) || game != game) return 1.0f;
    if (lo < 1.0f) lo = 1.0f;   // lines are at least one pixel (NVN and Vulkan)
    if (game < lo) return lo;
    if (hi >= lo && game > hi) return hi;
    return game;
}
