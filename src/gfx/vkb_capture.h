// [UIT 0157] A frame capture for diagnosis: what every draw of one frame read (vkb_draw.c). Asked for in game (Shift+F7,
// or the overlay's Overlay > Capture a frame for diagnosis); the frame after the next present is written to
// diag/frame-capture-<date>-<time>.txt in the game folder, and the log names the file when it is complete.
// No Vulkan here: the window and the overlay include it.
#pragma once
#include <stdint.h>

void vkb_capture_request(void);   // any thread: capture the next whole frame
int vkb_capture_on(void);         // the replay: this frame is being captured
void vkb_capture_present(uint64_t present);   // the replay, at each present (backend_vk.c mtl_present)
void vkb_capture_note(const char *fmt, ...)   // a line in the capture (any recording thread)
#if defined(__GNUC__) || defined(__clang__)
    __attribute__((format(printf, 1, 2)))
#endif
    ;
