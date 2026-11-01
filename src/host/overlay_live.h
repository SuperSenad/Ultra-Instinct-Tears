// [UIT 0171] The in-game overlay on the window (SDL main) thread: window_sdl.c feeds it events, overlay_live.c applies
// the settings (src/host/uit_live.h) and publishes what to draw (overlay_share.h). Every function: main thread only.
#pragma once
#include <SDL3/SDL.h>
#include <stdint.h>

#include "swkbd.h"

void uit_overlay_init(SDL_Window *w, void (*fullscreen)(void), void (*mark)(void));
int uit_overlay_key(const SDL_KeyboardEvent *k);   // 1: the overlay took the key (not for the game)
int uit_overlay_mouse(const SDL_Event *e);         // 1: the overlay took it (open)
// The physical buttons the game must not see now (all of them = ~0u while open); runs the hold-to-open combo.
uint32_t uit_overlay_pad(SDL_Gamepad *pad);
int uit_overlay_open(void);
int uit_overlay_busy(void);                        // open or the combo held: poll the controller, redraw at ~60 Hz
int uit_overlay_wait_ms(int idle_ms);              // how long the window loop may wait for an event
void uit_overlay_frame(void);                      // after the events: save, rebuild, publish
void uit_overlay_shutdown(void);                   // window closing: save what is pending, free the atlas
// [UIT 0177] SDL_EVENT_TEXT_INPUT / _EDITING: the keyboard dialog's text (1: taken). SDL's text input (and an IME) is
// on only while the dialog is up.
int uit_overlay_text(const SDL_Event *e);
// The game's thread (swkbd_host.c): shows the keyboard dialog for r and waits for it. 1 OK (out / *n: the text, UTF-16,
// NUL-terminated), 0 cancelled, -1 no dialog (no window, the font failed, one already up): the caller decides.
int uit_overlay_kb_run(const UitSwkbdReq *r, uint16_t *out, int *n);
