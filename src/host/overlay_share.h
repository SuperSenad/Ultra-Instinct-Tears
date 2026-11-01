// [UIT 0171] What the in-game overlay hands the present thread (src/host/overlay_live.c -> src/vk/overlay_vk.c).
// g_uit_ov_visible is the one check the present path makes while the overlay is closed (a relaxed atomic load);
// the rest is read under uit_ov_share_lock, held only while the quads (and a new atlas) are copied.
#pragma once
#include <stdatomic.h>
#include <stdint.h>
#include "uit_overlay.h"

#define UIT_OV_MAX_QUADS 3072
typedef struct {
    const UitOvQuad *q;
    int n;
    uint64_t seq;          // +1 each time the quads change
    const uint8_t *pix;    // the font atlas (R8), NULL = none
    int aw, ah;
    uint64_t atlas_gen;    // +1 each time the atlas changes
} UitOvShared;

extern _Atomic int g_uit_ov_visible;      // 1: something to draw (the overlay, the FPS counter, the hold ring)
const UitOvShared *uit_ov_share_lock(void);
void uit_ov_share_unlock(void);
