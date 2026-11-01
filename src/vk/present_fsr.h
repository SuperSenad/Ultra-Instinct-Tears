// FSR 1 at the window present (present_fsr.c): when the window shows the game's frame larger than it is, the present
// thread upscales it with AMD FidelityFX Super Resolution 1 (EASU, then RCAS sharpening) instead of a bilinear blit.
// TOTK_FSR=1 turns it on (ultrainstincttears.cfg or the environment; off by default while untested), TOTK_FSR_SHARPNESS the
// sharpening in stops (0 the sharpest .. 2, default 0.25). F8 turns it on / off, F9 steps the sharpness.
#pragma once
#include "vk.h"

int vkp_fsr_wanted(void);        // the setting (F8 changes it)
void vkp_fsr_toggle(void);       // F8
void vkp_fsr_sharpness_next(void);   // F9: 0, 0.25, 0.5, 1, 2 stops

// Present thread: records the upscale of src's rectangle crop (x, y, w, h; src is in TRANSFER_SRC_OPTIMAL, usage
// SAMPLED) into dst's rectangle (dx, dy, dw, dh; dst in TRANSFER_DST_OPTIMAL) and leaves both layouts as they were.
// The caller's submit must wait for src's write at the COMPUTE_SHADER stage too, and must wait for the submit's
// fence before the next call (the work of one call is reused by the next). 0 = nothing recorded (no FSR: blit).
int vkp_fsr_record(VkCommandBuffer cb, VkImage src, const int crop[4], VkImage dst, int dx, int dy, int dw, int dh);
