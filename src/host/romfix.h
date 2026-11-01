/* [UIT 0065 / 0068 / 0069] Game data edits made in memory when the game opens a file (no file is written to the game's
 * folder and nothing is downloaded): the edited file is served in place of the game's own, or a Mods-tab mod's.
 *   UI/LayoutArchive/Common.Product.140.Nin_NX_NVN.blarc.zs
 *       the quick-menu background fix (TOTK_MENU_BG_FIX, on unless =0) and, with an aspect ratio wider than 16:9
 *       (TOTK_ASPECT / TOTK_OUTPUT_RES), the UI layouts for it (TOTK_ASPECT_HUD corner | center | off)
 *   System/HeapSize/Product.Nin_NX_NVN.HeapDef.byml.zs
 *       the texture heaps a bigger canvas or scene needs (TOTK_OUTPUT_RES, TOTK_RENDER_RES)
 * Every edit first checks that the bytes are the ones 1.4.3 ships (or a mod keeps); anything else leaves the file as
 * it is and says why in the log. Decoded sizes never change, so the resource size table stays valid. */
#ifndef ROMFIX_H
#define ROMFIX_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/* reads a whole RomFS file (the mod's when a mod replaces it): malloc'd, NULL when there is none */
typedef uint8_t *(*RomfixRead)(const char *romfs_path, size_t *n);

/* 1: the current settings may change this RomFS path */
int romfix_wants(const char *path);
/* in: the file the game would get (from_mod: a Mods-tab mod's, else the game's). 1: *out (malloc'd, *on bytes) is to be served instead; 0: serve in as it is.
   cache_dir (may be NULL): a folder for the recompressed results, so later starts skip the slow compression. */
int romfix_apply(const char *path, const uint8_t *in, size_t n, int from_mod, RomfixRead rd, const char *cache_dir, uint8_t **out, size_t *on);

/* ---- the pieces, for tests (they work on decoded data in place) ---- */
#define ROMFIX_COMMON "UI/LayoutArchive/Common.Product.140.Nin_NX_NVN.blarc.zs"
#define ROMFIX_HEAPDEF "System/HeapSize/Product.Nin_NX_NVN.HeapDef.byml.zs"
typedef struct { const char *name; size_t lo, hi; } RomfixMember;
/* SARC member table (names point into d); count, or -1 when d is not a SARC this reads. *out malloc'd */
int romfix_sarc(const uint8_t *d, size_t n, RomfixMember **out);
/* the quick-menu capture keys: 1 moved, 0 already moved (a mod's fix: all at frame 0), <0 not the shipping data */
int romfix_menu_bg(uint8_t *d, size_t n, const RomfixMember *m, int nm);
/* the UI layouts for aspect (> 16:9); corner: the HUD in the screen corners. 1 done, <0 not the expected data
   (d may be partly edited then: work on a copy) */
int romfix_layouts(uint8_t *d, size_t n, const RomfixMember *m, int nm, double aspect, int corner);
/* test hook: the i-th pane field the layouts check and write (corner: the corner list): 0 past the end. The anim
   fields are i >= the pane count; kind 1: a pane field, 2: an animation float (pane = NULL, field = byte offset) */
int romfix_layout_field(unsigned i, int corner, const char **layout, const char **pane, unsigned *field, float *shipped);
extern const char *const romfix_keep_root[10];
/* HeapDef BYML: DynamicTextureAllocatorMem and Graphics grow by extra_kib. shipped: also require 1.4.3's sizes.
   1 done, <0 not a HeapDef this reads (or other sizes with shipped) */
int romfix_heapdef(uint8_t *d, size_t n, uint32_t extra_kib, int shipped);
#ifdef __cplusplus
}
#endif
#endif
