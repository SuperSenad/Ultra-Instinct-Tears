// NVIDIA DLSS through NVIDIA Streamline (optional; see BUILD-WINDOWS-FSR.md, "DLSS").
//
// The game's own upscale (FSR 1 EASU from its scene size to the 1920x1080 canvas, before the HUD) is replaced by
// DLSS: src/gfx/vkb_dlss.c finds the G-buffer depth, the composed scene colour and the camera, makes camera motion
// vectors, jitters the scene's projection, and calls dlss_sl_evaluate in place of the game's EASU draw. The present
// (src/vk/present_fsr.c) then enlarges the canvas to the window as before.
//
// Streamline is never linked: src/vk/dlss_sl.c (plain C on src/vk/sl_abi.h; no SDK headers needed to build) loads
// sl.interposer.dll from the exe's folder at run time (the user downloads the SDK; tools/setup.py copies its DLLs).
// Built with --no-dlss, or without the DLLs, an RTX GPU or a driver that supports DLSS, everything here reports
// "not available" and the game runs as before (FSR).
//
// ultrainstincttears.cfg keys (cfg-keys.md): TOTK_UPSCALER=fsr|dlss|off, TOTK_DLSS_MODE=dlaa|quality|balanced|performance|
// ultraperf, TOTK_DLSS_PRESET, TOTK_RENDER_RES, TOTK_DLSS_JITTER, TOTK_DLSS_JITTER_SIGN. F10 turns DLSS on and off.
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <vulkan/vulkan.h>

#ifdef __cplusplus
extern "C" {
#endif

enum { UPSCALER_LEGACY = -1, UPSCALER_OFF = 0, UPSCALER_FSR = 1, UPSCALER_DLSS = 2 };
enum { DLSS_DLAA, DLSS_QUALITY, DLSS_BALANCED, DLSS_PERFORMANCE, DLSS_ULTRAPERF };

int dlss_upscaler(void);          // TOTK_UPSCALER (UPSCALER_LEGACY when unset: TOTK_FSR alone decides, as before)
int dlss_mode(void);              // TOTK_DLSS_MODE (default quality)
const char *dlss_mode_name(int mode);

// Instance creation (src/vk/vk.c): with TOTK_UPSCALER=dlss and Streamline present, the loader entry the instance and
// device are created through (the interposer's); otherwise `loader` itself.
PFN_vkGetInstanceProcAddr dlss_vk_loader(PFN_vkGetInstanceProcAddr loader);
void dlss_device_created(VkPhysicalDevice gpu);   // after vkCreateDevice: is DLSS supported on this GPU?
int dlss_available(void);         // Streamline is up and DLSS is supported (fixed after device creation)
int dlss_active(void);            // available and not turned off with F10
void dlss_toggle(void);           // F10
void dlss_note_failure(void);     // an evaluate failed: the replay falls back to the game's EASU for a while

// The scene (render) size asked for: 0 = the game's own (DLAA, or DLSS not available). Decided once, the first time
// the game picks its render size (tools/lifter/patches.py: totk_render_res_on / totk_render_size).
int dlss_render_size(unsigned *w, unsigned *h);

typedef struct {
    VkImage image; VkImageView view; VkDeviceMemory memory; VkFormat format;
    uint32_t width, height; VkImageLayout layout; VkImageUsageFlags usage;
} dlss_image;

typedef struct {
    VkCommandBuffer cb;
    uint32_t frame;                      // the present count (Streamline's frame index)
    dlss_image color, depth, motion, bias, output;   // bias.image may be VK_NULL_HANDLE
    uint32_t in_w, in_h;                 // the rendered region of color/depth/motion (top left)
    float jitter_x, jitter_y;            // render pixels: the picture's shift this frame (x right, y down), = Streamline jitterOffset
    int reset, hdr;
    float clip_to_prev[16], prev_to_clip[16];   // row major, without jitter
    float near_z, far_z, fov_y;
    // [perf-mt] how the motion vectors and the depth are to be read (Streamline constants). All 0, as a zeroed
    // dlss_frame has them, is what vkb_dlss.c's camera vectors are: render pixels, previous minus current, unjittered,
    // camera motion included, depth 0 near .. 1 far. A future source (a game velocity buffer, design note option A)
    // sets these instead of converting its data first. dlss_sl.c reads them through dlss_mv_conv below.
    float mv_scale_x, mv_scale_y;        // 0: 1 / in_w, 1 / in_h (the vectors are in render pixels)
    int mv_jittered;                     // the vectors include the jitter
    int mv_object_only;                  // the vectors leave the camera's motion out (cameraMotionIncluded false)
    int mv_dilated;                      // the vectors are already dilated (low resolution / nearest-depth)
    int depth_inverted;                  // reversed Z: 1 near .. 0 far
} dlss_frame;

// the Streamline constants for f's motion vectors and depth (dlss_sl.c; pure, tools/tests/test_mvprobe.c)
typedef struct { float scale_x, scale_y; int jittered, camera_included, dilated, depth_inverted; } dlss_mv_conv_t;
static inline dlss_mv_conv_t dlss_mv_conv(const dlss_frame *f) {
    dlss_mv_conv_t c;
    c.scale_x = f->mv_scale_x != 0.0f ? f->mv_scale_x : f->in_w ? 1.0f / (float)f->in_w : 0.0f;
    c.scale_y = f->mv_scale_y != 0.0f ? f->mv_scale_y : f->in_h ? 1.0f / (float)f->in_h : 0.0f;
    c.jittered = f->mv_jittered != 0;
    c.camera_included = !f->mv_object_only;
    c.dilated = f->mv_dilated != 0;
    c.depth_inverted = f->depth_inverted != 0;
    return c;
}

int dlss_evaluate(const dlss_frame *f);   // records DLSS into f->cb; 0 when it could not

// src/vk/dlss_sl.c (Streamline; stubs with TOTK_NO_DLSS)
int dlss_sl_built(void);
PFN_vkGetInstanceProcAddr dlss_sl_load(const wchar_t *dir);
int dlss_sl_supported(VkPhysicalDevice gpu);
int dlss_sl_optimal(int mode, uint32_t out_w, uint32_t out_h, uint32_t *w, uint32_t *h);
int dlss_sl_evaluate(const dlss_frame *f, int mode, int preset);

#ifdef __cplusplus
}
#endif
