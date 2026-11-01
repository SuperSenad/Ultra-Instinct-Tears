/* [UIT 0068] The display settings several parts of the port need (no Vulkan, no Windows headers):
 *   TOTK_OUTPUT_RES  the game's output canvas (1920x1080 = the docked console, the default; 1440p; 2160p; display);
 *   TOTK_ASPECT      the picture's aspect ratio for ultrawide (0 = 16:9, the game's own);
 * and the memory the game needs for a bigger canvas or scene (the HeapDef heaps and the application pool). */
#ifndef UIT_DISPLAY_H
#define UIT_DISPLAY_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/* the output canvas; 1 when it is not the docked 1920x1080 (and the lift applied the output-res patch) */
int uit_output_res(unsigned *w, unsigned *h);
/* the canvas from the setting alone, without the lift check (unit tests, memory planning) */
int uit_output_res_setting(unsigned *w, unsigned *h);
/* [native-1031] the main display's size in pixels (TOTK_OUTPUT_RES=display); 0 when unknown */
int uit_display_size(unsigned *w, unsigned *h);
/* the canvas width over 1920 (1 at the default): the game multiplies the scene size it reads by this */
double uit_output_scale(void);
/* TOTK_ASPECT: width over height of the picture (0: 16:9, the game's own) */
double uit_aspect(void);
/* the picture's shape: TOTK_ASPECT, else the canvas's own when TOTK_OUTPUT_RES is not 16:9 (0: 16:9) */
double uit_picture_aspect(void);
/* the camera patch is in this build (main_aspect_patch) and TOTK_ASPECT / the canvas ask for another shape */
int totk_aspect_on(void);
/* the shape the present pass gives the game's picture (0: the picture's own) */
double uit_present_aspect(void);
/* the largest scene the planned memory holds (pixels); 1600*900 at the default */
uint64_t uit_scene_budget(void);
/* the scene the settings will ask for, for memory planning (0x0: the game's own) */
void uit_scene_plan(unsigned *w, unsigned *h);
/* extra KiB for the DynamicTextureAllocatorMem and Graphics heaps (0 at the default) */
uint64_t uit_heap_extra_kib(void);
/* [UIT 0154] TOTK_HEAP_EXTRA_MB in KiB (0 unset) */
uint64_t uit_heap_extra_setting_kib(void);
/* the HeapDef could not be grown (romfix.c): from now on the canvas is 1080p and the scene plan the game's own */
void uit_heap_refused(void);
/* the formula itself: output canvas and scene (0x0 = the game's own) */
uint64_t uit_heap_extra_kib_for(unsigned ow, unsigned oh, unsigned sw, unsigned sh);
/* [UIT 0070] TOTK_SHADOW_RES: 512 / 1024 / 2048 / 4096, 0 for the game's own (the setting alone) */
unsigned uit_shadow_res_setting(void);
/* ... only when the lift put the confirmed shadow-size patch in (main_shadow_patch); 0 otherwise */
unsigned uit_shadow_res(void);
/* the extra heap KiB a shadow size needs (counted in uit_heap_extra_kib) */
uint64_t uit_shadow_extra_kib_for(unsigned size);
#ifdef __cplusplus
}
#endif
#endif
