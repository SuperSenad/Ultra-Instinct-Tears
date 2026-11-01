// [UIT 0174] The controller and keyboard bindings as data the game can change while it runs (input_map.h has the
// tables and tokens). Portable C with no SDL calls (input_map.h's SDL headers give it the button and key numbers only),
// so the overlay core (uit_overlay.c) and tools/tests/test_overlay.cpp use it as the window does.
//  - UitBinds: for each of the 16 Switch buttons, the controller inputs and the keys that press it (indices into
//    kUitPadInputs / kUitKeys), read from TOTK_PAD_<button> / TOTK_KEY_<button> as window_sdl.c always read them;
//  - uit_binds_assign: a new binding, with launcher/main.cpp's rule for a button or key another Switch button already
//    uses (it moves over, and the other button takes this one's old binding: a swap);
//  - uit_binds_cfg_value: the cfg line, byte for byte what launcher/model.cpp write_value writes;
//  - uit_binds_compile: the lookup tables the input path reads (window_sdl.c swaps them in, see uit_input_apply_binds).
#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define UIT_BIND_BUTTONS 16    // kUitButtons (input_map.h; checked in input_bind.c)
#define UIT_BIND_PAD_INPUTS 23 // kUitPadInputs
#define UIT_BIND_MAX 32        // inputs per Switch button (the launcher binds one; more only by hand in the cfg)
enum { UIT_BIND_PAD = 0, UIT_BIND_KEY = 1 };
enum { UIT_PADFAM_GENERIC, UIT_PADFAM_XBOX, UIT_PADFAM_PS, UIT_PADFAM_NINTENDO };   // launcher/model.h PadFamily

typedef struct { uint8_t n; uint8_t in[UIT_BIND_MAX]; } UitBindList;
typedef struct UitBinds { UitBindList b[2][UIT_BIND_BUTTONS]; } UitBinds;   // [UIT_BIND_PAD / UIT_BIND_KEY][button]

// The Switch buttons
const char *uit_bind_cfg_key(int kind, int button);    // "TOTK_PAD_A" / "TOTK_KEY_A"
const char *uit_bind_button_name(int button);           // "A", "+ (Plus)", "Left stick press", ...
const char *uit_bind_button_does(int button);           // what it does in the game ("talk, pick up, open and confirm")
uint64_t uit_bind_button_bit(int button);                // libnx HidNpadButton bit

// Inputs
int uit_bind_input_count(int kind);
const char *uit_bind_token(int kind, int input);         // the cfg token ("east", "LShift")
// Its name as the launcher shows it: the connected controller's label (pad_label in launcher/model.cpp), the key cap
const char *uit_bind_label(int kind, int input, int pad_family);
int uit_bind_key_is_stick(int input);                    // 1: W A S D (the left stick), 2: the arrows (the right one); never bindable
// Physical controller bit as overlay_live.c / uit_overlay.c count them (1 << SDL_GamepadButton below 30, 30 / 31 the
// left / right trigger past half way) -> kUitPadInputs index, or -1; and back (0 = none).
int uit_bind_pad_from_phys(int bit);
uint32_t uit_bind_pad_phys(int input);
int uit_bind_key_from_scancode(int sdl_scancode);       // kUitKeys index, or -1

// Lists
void uit_binds_default(UitBinds *b);
// Parses a cfg value the way the game and the launcher read it: "none" (or nothing usable but "none" and blanks)
// unbinds; tokens that are not inputs are skipped; a value with no usable token at all means the default.
// Returns the number of unusable tokens.
int uit_bind_parse(int kind, int button, const char *value, UitBindList *out);
// Every binding from getenv-like get (NULL = the default for all). Returns how many differ from the defaults; with
// log != 0 says each one on stderr as window_sdl.c did ("[totk] controls: ...").
int uit_binds_load(UitBinds *b, const char *(*get)(const char *key), int log);
int uit_bind_list_eq(const UitBindList *a, const UitBindList *b);
int uit_bind_is_default(const UitBinds *b, int kind, int button);
// The value written for it: 0 = no line (the default), else 1 with the comma list, or "none" when unbound.
int uit_binds_cfg_value(const UitBinds *b, int kind, int button, char *out, size_t n);
// The names, joined with " or " ("nothing" when unbound)
void uit_binds_text(const UitBinds *b, int kind, int button, int pad_family, char *out, size_t n);
// Switch button `button` takes `input` alone. Another Switch button that had it loses it, and takes this one's old
// binding if it is left with nothing (launcher/main.cpp bind_apply). Returns 0 when it already was exactly that,
// else 1; *moved = the other button changed (or -1).
int uit_binds_assign(UitBinds *b, int kind, int button, int input, int *moved);
void uit_binds_clear(UitBinds *b, int kind, int button);       // unbound ("none")
void uit_binds_reset(UitBinds *b, int kind, int button);       // the default again (button < 0: every one of kind)
// The input path's tables: pad[kUitPadInputs index] and key[Mac key code] = the Switch button bits they press.
void uit_binds_compile(const UitBinds *b, uint64_t pad[UIT_BIND_PAD_INPUTS], uint64_t key[128]);

// window_sdl.c (the game; not in tools/tests): the bindings in use, and a change while the game runs. The new tables
// are built aside and swapped in with one pointer store (an atomic release store for the controller's table, read
// with one acquire load per controller update; the key table through runtime_input_set_keymap). Window thread only.
const UitBinds *uit_input_binds(void);
void uit_input_apply_binds(const UitBinds *b);

#ifdef __cplusplus
}
#endif
