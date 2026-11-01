// [UIT 0346] Screenshots: F12 (Game > Screenshots > Screenshot key, TOTK_SCREENSHOT_KEY), an optional controller
// button or combo (Screenshot controller combo, TOTK_SCREENSHOT_PAD, none by default) or Overlay > Take a screenshot.
// The next frame with a game picture is copied once from the window's image, as shown but without the overlay
// (src/vk/present.c vkp_screenshot), and a thread of its own writes it as a PNG to
// <Pictures>\Ultra Instinct Tears\UIT-YYYYMMDD-HHMMSS.png. Nothing is copied or allocated on the other frames.
#pragma once
#include <stdint.h>

#include "../vk/present.h"

// window (SDL main) thread
void uit_shot_init(void);                 // reads both settings (before the overlay's init)
int uit_shot_key(void);                   // the SDL scancode that takes one, 0 = no key
int uit_shot_key_choice(void);            // the index in its choices (the overlay's row), -1 = a value not listed
void uit_shot_set_key_choice(int i);
int uit_shot_key_of_choice(int i);       // the scancode of choice i (0: none)
uint32_t uit_shot_pad(void);              // the combo: physical button bits as the overlay's (30 / 31 = ZL / ZR), 0 = none
const char *uit_shot_pad_name(void);      // "ZL + Minus"
int uit_shot_pad_choice(void);
void uit_shot_set_pad_choice(int i);
void uit_shot_take(const char *how);      // asks for the next frame (how: for the log)
void uit_shot_poll(uint64_t now_ms);      // each window loop: a finished one becomes the toast
// The toast (top right): "Saving the screenshot..." then where it went for a few seconds; NULL: nothing.
const char *uit_shot_text(uint64_t now_ms, const char **line2);

// present thread: the copy is done (the encoder thread owns s and releases it); or why there is none
void uit_shot_ready(VkpShot *s);
void uit_shot_failed(const char *why);
