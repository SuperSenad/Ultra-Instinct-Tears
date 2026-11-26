/* [UIT 0452] Button prompts that match the controller: Game > Controller > Button prompts (TOTK_BUTTON_PROMPTS).
 *
 *   TOTK_BUTTON_PROMPTS=switch | auto | xbox | ps4 | ps5     ([UIT 0453] absent: auto; "switch": the game's own Switch
 *   buttons, nothing changed)
 *
 * The game draws its button prompts from three kinds of data, all edited in memory when the game opens the file
 * (romfix.c serves the result; no file in the game folder is written):
 *   - the button font scft/External.bfotf inside every Font/Font<region>.Nin_NX_NVN.bfarc.zs: the glyphs at U+E040.. (A B X Y,
 *     L R ZL ZR, Plus, Minus) that every text prompt uses;
 *   - the button images Nt_KeyTex{A,B,X,Y,L,ZL}_00 of timg/__Combined.bntx in the Common layout archive: the action
 *     prompt ("A Talk") and the button guide in the corner;
 *   - [Xbox] the one-letter text panes T_{A,B,X,Y}_<n> around the action prompt, in the Common archive's layouts and in
 *     LayoutMsg/DoCommand_00.msbt of the Mals/<language> archives.
 * Which picture a Switch button gets follows the controller button that presses it: the first controller input bound
 * to it (TOTK_PAD_<button>, input_map.h; by default Switch A is the right face button). So with the default bindings
 * Switch A shows Xbox B or PlayStation Circle, the button the player actually presses.
 * Xbox face buttons reuse the game's own letter glyphs and images (A B X Y are letters on both); the PlayStation
 * symbols, the shoulder and trigger names (LB RT L1 R2 ...) and Menu / View / Options / Share / Create are drawn from
 * prompt_icons.h (our own drawings, made by tools/gen_prompt_icons.py).
 * Every edit keeps the file's decoded size, so the resource size table stays valid. Each one checks the data first
 * and leaves the file as it is (and says why in the log) when it is not what it expects. Auto is decided once, when
 * the game starts: the controller connected then, else the last one Auto saw (userdata/button-prompts-auto.txt), else
 * the Switch buttons. */
#ifndef UIT_PROMPTS_H
#define UIT_PROMPTS_H
#include <stddef.h>
#include <stdint.h>
#include "romfix.h"
#ifdef __cplusplus
extern "C" {
#endif
enum { UIT_PROMPTS_SWITCH, UIT_PROMPTS_XBOX, UIT_PROMPTS_PS4, UIT_PROMPTS_PS5 };

/* window_sdl.c: the family of the connected controller (UIT_PROMPTS_*), -1 when none is connected */
void uit_prompts_set_detector(int (*detect)(void));
/* the style in effect (decided once, the first time it is asked) */
int uit_prompts_style(void);
const char *uit_prompts_name(int style);

/* What each prompt shows: for the 10 Switch buttons (PB_*) either the game's own picture of another button (game >= 0:
   a PB_ index) or one of our icons (icon >= 0: a PI_ index), or neither (both -1: unchanged). */
enum { PB_A, PB_B, PB_X, PB_Y, PB_L, PB_R, PB_ZL, PB_ZR, PB_PLUS, PB_MINUS, PB_COUNT };
typedef struct { signed char game[PB_COUNT], icon[PB_COUNT]; int style; char letter[4]; } PromptMap;
/* the map for a style and the current bindings (get: the cfg lookup, NULL = getenv). 0: nothing to change */
int prompts_map(int style, const char *(*get)(const char *), PromptMap *m);

/* 1: path is a file the prompts may change: a font archive (Font/Font<region>.Nin_NX_NVN.bfarc.zs) or, for Xbox, a
   message archive (Mals/<language>.Product.<n>.sarc.zs) */
int prompts_wants(const char *path);

/* ---- the edits, on decoded data, in place (sizes never change). Each returns the number of things changed, 0 when
   none was needed, <0 when the data is not what it expects (then d is left as it was) ---- */
int prompts_common(uint8_t *d, size_t n, const RomfixMember *m, int nm, const PromptMap *map);
int prompts_font_archive(uint8_t *d, size_t n, const PromptMap *map);
int prompts_mals(uint8_t *d, size_t n, const PromptMap *map);

/* ---- pieces, for tests ---- */
/* a bfotf (8-byte header, the rest XORed) as OpenType in place; 1 ok. Encoding again is the same call. */
int prompts_bfotf_xor(uint8_t *blob, size_t n);
/* edit an OpenType CFF font in place (map's font glyphs); >0 glyphs changed */
int prompts_otf(uint8_t *otf, size_t n, const PromptMap *map);
/* the Type 2 charstring of icon fitted into advance (font units): bytes written to out (cap), or 0 */
size_t prompts_charstring(int icon, int advance, int nominal_width, uint8_t *out, size_t cap, int *xmin);
/* coverage mask (0..255, rows top down) of icon fitted into a size x size square with margin px */
void prompts_raster(int icon, int size, int margin, uint8_t *out);
uint32_t prompts_gob_offset(uint32_t x, uint32_t y, uint32_t width_gobs, uint32_t block_height);
#ifdef __cplusplus
}
#endif
#endif
