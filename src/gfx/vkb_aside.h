// [UIT 0075] Textures set aside (vkb_tex.c): when the oldest goes. Pure, for tools/tests/test_vkb_mem.c.
// Before: up to 512 aside, and every one older than 120 presents went in the same call (after a scene change hundreds
// of vkDestroyImage / vkFreeMemory at once on the replay thread). Spread (TOTK_TEX_RELEASE_SPREAD, default on): up to
// 2048 aside, one dropped per release past that, and at most VKB_ASIDE_PER_CALL old ones per call; the rest go on the
// next calls (each texture release or take-back calls it, hundreds a frame).
#pragma once
enum { VKB_ASIDE_OLD = 512, VKB_ASIDE_SPREAD = 2048, VKB_ASIDE_PER_CALL = 2 };
// n aside (oldest first), cap the most kept, expired: the oldest is past its frames, *budget old ones this call may
// still drop (<0: no limit, the old rule). 1: drop the oldest now. The release itself makes room for the new one.
static inline int vkb_aside_go(int n, int cap, int expired, int *budget) {
    if (!n) return 0;
    if (n > cap || (*budget < 0 && n >= cap)) return 1;   // old rule (no limit): at the cap too, as before UIT 0075
    if (!expired || !*budget) return 0;
    if (*budget > 0) --*budget;
    return 1;
}
