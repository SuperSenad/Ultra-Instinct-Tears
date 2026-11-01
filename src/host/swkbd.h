// [UIT 0177] nn::swkbd (the software keyboard applet) without the applet: the game's ShowKeyboard is answered here
// (src/host/swkbd_host.c, an HLE override) and the text is typed into the overlay's dialog (uit_overlay.c, on the
// window thread). This header is the portable part: ShowKeyboardArg read into a request, the filters the real
// keyboard applies (length, disabled characters, the keyboard mode), UTF-8 <-> UTF-16 and the String written back.
// No SDL, no runtime: tools/tests/test_swkbd.c runs it on synthetic arguments.
// The layout is switchbrew's KeyboardConfig (public) and the MIT reference's swkbd_win.c (MacNativeGames lineage);
// nothing from Ryujinx or yuzu.
#pragma once
#include <stddef.h>
#include <stdint.h>

#define UIT_SWKBD_LIMIT 500                 // the most UTF-16 units a request may ask for (and the default)
#define UIT_SWKBD_RESULT_CANCELED 0x29fu    // nn::swkbd::ResultCanceled (module 159, description 1)
#define UIT_SWKBD_ARG_SIZE 0x548            // ShowKeyboardArg as the game passes it (KeyboardConfig + work buffer + ...)

enum {   // KeyboardConfig.KeyboardMode
    UIT_KBMODE_FULL = 0, UIT_KBMODE_NUMERIC = 1, UIT_KBMODE_ASCII = 2, UIT_KBMODE_FULL_LATIN = 3, UIT_KBMODE_ALPHABET = 4,
    UIT_KBMODE_SIMPLIFIED_CHINESE = 5, UIT_KBMODE_TRADITIONAL_CHINESE = 6, UIT_KBMODE_KOREAN = 7,
    UIT_KBMODE_LANGUAGE_SET2 = 8, UIT_KBMODE_LANGUAGE_SET2_LATIN = 9
};
enum {   // KeyboardConfig.InvalidCharFlag
    UIT_KBINV_SPACE = 1 << 1, UIT_KBINV_AT = 1 << 2, UIT_KBINV_PERCENT = 1 << 3, UIT_KBINV_SLASH = 1 << 4,
    UIT_KBINV_BACKSLASH = 1 << 5, UIT_KBINV_NUMBERS = 1 << 6
};

typedef struct {
    uint32_t mode, invalid;
    int max_len, min_len;          // UTF-16 units: 1 <= max_len <= UIT_SWKBD_LIMIT, 0 <= min_len <= max_len
    int password, newline, utf8, text_check, cursor_start;   // cursor_start: 0 at the end, 1 at the start
    uint16_t left_key, right_key;  // the numeric pad's two optional keys (0: none)
    uint16_t ok[10], header[0x41], sub[0x81], guide[0x101];  // NUL-terminated
    uint16_t initial[UIT_SWKBD_LIMIT + 1];
    int initial_len;               // units, after the filters
    uint32_t work_at;              // where the work buffer {ptr, size} was found in the argument (0: none)
} UitSwkbdReq;

// arg: the game's ShowKeyboardArg (at least UIT_SWKBD_ARG_SIZE bytes). The initial text is read from the work
// buffer it names, when that looks like one (a size the SDK uses). Returns 1.
int uit_swkbd_parse(const uint8_t *arg, UitSwkbdReq *r);
// The keyboard would let cp (a code point) be typed.
int uit_swkbd_allowed(const UitSwkbdReq *r, uint32_t cp);
// Drops what may not be typed and keeps at most max_len units (a surrogate pair is never split). Returns the new length.
size_t uit_swkbd_clean(const UitSwkbdReq *r, uint16_t *t, size_t n);
// out may be NULL (only counts). Malformed input is U+FFFD. The result is not NUL-terminated; returns the units / bytes.
size_t uit_swkbd_utf8_to_utf16(const char *s, size_t n, uint16_t *out, size_t cap);
size_t uit_swkbd_utf16_to_utf8(const uint16_t *s, size_t n, char *out, size_t cap);   // stops before a char that does not fit
size_t uit_swkbd_u16len(const uint16_t *s, size_t max);
// The answer into the game's String {buf, size bytes}: UTF-16 (or UTF-8 when the request asks), NUL-terminated,
// cut to fit. ok = 0 or a text shorter than min_len: cancelled (an empty string). Returns the Result for the game.
uint32_t uit_swkbd_finish(const UitSwkbdReq *r, int ok, const uint16_t *t, size_t n, void *buf, uint64_t size);
