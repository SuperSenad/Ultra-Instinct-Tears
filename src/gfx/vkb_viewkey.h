// [UIT 0200] The image view cache key (vkb_tex.c vkb_view), tested by tools/tests/test_view_key.c. Include after
// view_t (nvn/nvn_obj.h). Brought over from deko-1031 (its 0108), with the batch's 0124 rule kept: targets and
// storage views take no swizzle.
// The key used to be the view's 16 raw bytes plus one byte for the use: (1) fields the view does not set (the game's
// stack bytes after SetDefaults only clear what it sets; pad bits and bytes; the level range without set_levels...)
// made new keys for the same view, filling the 32 views a texture kept, after which every lookup made and destroyed a
// view (t22x logs: "more than 32 views" on ten textures); (2) a storage view and a render-target view shared the use
// byte (0xFF), so one could be served for the other (sRGB images: a storage view is the UNORM form; the usage
// differs). The fixed key holds only the fields the view sets and the kind of use; past the first 32 views a texture
// keeps up to VKB_VIEW_MAX in blocks (lock-free readers).
#pragma once
#include <stdint.h>
#include <string.h>

enum { VKB_VIEW_KEY = 18, VKB_VIEWS_INLINE = 32, VKB_VIEW_BLOCK = 64, VKB_VIEW_BLOCKS = 7,
       VKB_VIEW_MAX = VKB_VIEWS_INLINE + VKB_VIEW_BLOCK * VKB_VIEW_BLOCKS };

// fixed: 1 = the fields the view sets (UIT 0200); 0 = the raw bytes as before (A/B; 0124's swizzle rule kept)
static inline void vkb_view_key(uint8_t key[VKB_VIEW_KEY], const view_t *v, int attachment, int view_type, uint32_t resident_base, int fixed) {
    memset(key, 0, VKB_VIEW_KEY);
    if (!fixed) {
        if (v) {
            view_t k = *v;
            if (attachment) { k.set_swizzle = 0; memset(k.swz, 0, sizeof k.swz); }
            memcpy(key, &k, 16);
        }
        key[16] = (uint8_t)(attachment ? 0xFF : view_type + 1);
        key[17] = (uint8_t)resident_base;
        return;
    }
    if (v) {
        int swz = v->set_swizzle && !attachment;   // [UIT 0124] targets and storage views: no swizzle
        key[0] = (uint8_t)(v->set_levels | v->set_layers << 1 | v->set_format << 2 | v->set_target << 3 | swz << 4);
        if (v->set_levels) { key[1] = v->base_level; key[2] = v->nlevels; }
        if (v->set_target) key[3] = v->target;
        if (v->set_layers) { memcpy(key + 4, &v->min_layer, 2); memcpy(key + 6, &v->nlayers, 2); }
        if (v->set_format) memcpy(key + 8, &v->format, 2);
        if (swz) memcpy(key + 10, v->swz, 4);
    }
    key[16] = (uint8_t)(attachment ? 0xF0 | (attachment & 0xF) : view_type + 1);
    key[17] = (uint8_t)resident_base;
}
