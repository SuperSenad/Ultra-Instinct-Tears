// Controller and keyboard remapping (0029). The game's 16 Switch buttons, the controller inputs and keyboard keys that
// can press them, and the ultrainstincttears.cfg keys and tokens that bind them:
//
//   TOTK_PAD_A=east            the controller input(s) for the Switch A button (comma list; "none" = unbound)
//   TOTK_KEY_B=K,Space         the keyboard key(s) for the Switch B button
//
// A missing key means the default below. Shared by the game (window_sdl.c builds its tables from it) and the launcher
// (launcher/main.cpp draws and writes the same tokens), so the two cannot disagree.
//
// Controller tokens name the button's position, not its label: "east" is the right face button (A on a Switch or
// Nintendo controller, B on Xbox, Circle on PlayStation). The game expects Switch positions, so by default Switch A is
// "east" on every controller. The sticks are not remappable, and neither are the keys that move them
// (W A S D for the left stick, the arrow keys for the right stick).
#ifndef UIT_INPUT_MAP_H
#define UIT_INPUT_MAP_H

#include <SDL3/SDL_gamepad.h>
#include <SDL3/SDL_scancode.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

// One Switch button: its cfg keys, its short name, the defaults, and its libnx HidNpadButton bit.
typedef struct {
    const char *pad_key, *key_key, *id, *name, *pad_def, *key_def;
    uint64_t bit;
} UitSwitchButton;

static const UitSwitchButton kUitButtons[] = {
    {"TOTK_PAD_A", "TOTK_KEY_A", "A", "A", "east", "J", 1},
    {"TOTK_PAD_B", "TOTK_KEY_B", "B", "B", "south", "K,Space", 2},
    {"TOTK_PAD_X", "TOTK_KEY_X", "X", "X", "north", "I", 4},
    {"TOTK_PAD_Y", "TOTK_KEY_Y", "Y", "Y", "west", "U", 8},
    {"TOTK_PAD_L", "TOTK_KEY_L", "L", "L", "leftshoulder", "Q", 64},
    {"TOTK_PAD_R", "TOTK_KEY_R", "R", "R", "rightshoulder", "E", 128},
    {"TOTK_PAD_ZL", "TOTK_KEY_ZL", "ZL", "ZL", "lefttrigger", "LAlt", 256},
    {"TOTK_PAD_ZR", "TOTK_KEY_ZR", "ZR", "ZR", "righttrigger", "LShift", 512},
    {"TOTK_PAD_PLUS", "TOTK_KEY_PLUS", "PLUS", "+ (Plus)", "start", "Enter", 1024},
    {"TOTK_PAD_MINUS", "TOTK_KEY_MINUS", "MINUS", "- (Minus)", "back", "Tab,Esc", 2048},
    {"TOTK_PAD_LSTICK", "TOTK_KEY_LSTICK", "LSTICK", "Left stick press", "leftstick", "Z", 16},
    {"TOTK_PAD_RSTICK", "TOTK_KEY_RSTICK", "RSTICK", "Right stick press", "rightstick", "C", 32},
    {"TOTK_PAD_DUP", "TOTK_KEY_DUP", "DUP", "D-pad up", "dpup", "T", 8192},
    {"TOTK_PAD_DDOWN", "TOTK_KEY_DDOWN", "DDOWN", "D-pad down", "dpdown", "G", 32768},
    {"TOTK_PAD_DLEFT", "TOTK_KEY_DLEFT", "DLEFT", "D-pad left", "dpleft", "F", 4096},
    {"TOTK_PAD_DRIGHT", "TOTK_KEY_DRIGHT", "DRIGHT", "D-pad right", "dpright", "H", 16384},
};
#define UIT_BUTTON_COUNT ((int)(sizeof kUitButtons / sizeof *kUitButtons))

// Controller inputs: a gamepad button, or a trigger (pressed past half way).
typedef struct {
    const char *token;
    int button;   // SDL_GamepadButton, or -1
    int axis;     // SDL_GamepadAxis (a trigger), or -1
} UitPadInput;

static const UitPadInput kUitPadInputs[] = {
    {"south", SDL_GAMEPAD_BUTTON_SOUTH, -1},
    {"east", SDL_GAMEPAD_BUTTON_EAST, -1},
    {"west", SDL_GAMEPAD_BUTTON_WEST, -1},
    {"north", SDL_GAMEPAD_BUTTON_NORTH, -1},
    {"leftshoulder", SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, -1},
    {"rightshoulder", SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, -1},
    {"lefttrigger", -1, SDL_GAMEPAD_AXIS_LEFT_TRIGGER},
    {"righttrigger", -1, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER},
    {"start", SDL_GAMEPAD_BUTTON_START, -1},
    {"back", SDL_GAMEPAD_BUTTON_BACK, -1},
    {"guide", SDL_GAMEPAD_BUTTON_GUIDE, -1},
    {"leftstick", SDL_GAMEPAD_BUTTON_LEFT_STICK, -1},
    {"rightstick", SDL_GAMEPAD_BUTTON_RIGHT_STICK, -1},
    {"dpup", SDL_GAMEPAD_BUTTON_DPAD_UP, -1},
    {"dpdown", SDL_GAMEPAD_BUTTON_DPAD_DOWN, -1},
    {"dpleft", SDL_GAMEPAD_BUTTON_DPAD_LEFT, -1},
    {"dpright", SDL_GAMEPAD_BUTTON_DPAD_RIGHT, -1},
    {"misc1", SDL_GAMEPAD_BUTTON_MISC1, -1},
    {"touchpad", SDL_GAMEPAD_BUTTON_TOUCHPAD, -1},
    {"rightpaddle1", SDL_GAMEPAD_BUTTON_RIGHT_PADDLE1, -1},
    {"leftpaddle1", SDL_GAMEPAD_BUTTON_LEFT_PADDLE1, -1},
    {"rightpaddle2", SDL_GAMEPAD_BUTTON_RIGHT_PADDLE2, -1},
    {"leftpaddle2", SDL_GAMEPAD_BUTTON_LEFT_PADDLE2, -1},
};
#define UIT_PAD_INPUT_COUNT ((int)(sizeof kUitPadInputs / sizeof *kUitPadInputs))

// Keyboard keys the game can see: the SDL scancode, the Mac virtual key code the game's input code uses (the port's
// keyboard layout comes from MacNativeGames), the cfg token and its label. stick = 1: moves a stick, not bindable.
typedef struct {
    const char *token, *label;
    SDL_Scancode sc;
    int mac, stick;
} UitKey;

static const UitKey kUitKeys[] = {
    {"A", "A", SDL_SCANCODE_A, 0, 1}, {"S", "S", SDL_SCANCODE_S, 1, 1}, {"D", "D", SDL_SCANCODE_D, 2, 1},
    {"W", "W", SDL_SCANCODE_W, 13, 1},
    {"Left", "Left arrow", SDL_SCANCODE_LEFT, 123, 1}, {"Right", "Right arrow", SDL_SCANCODE_RIGHT, 124, 1},
    {"Down", "Down arrow", SDL_SCANCODE_DOWN, 125, 1}, {"Up", "Up arrow", SDL_SCANCODE_UP, 126, 1},
    {"F", "F", SDL_SCANCODE_F, 3, 0}, {"H", "H", SDL_SCANCODE_H, 4, 0}, {"G", "G", SDL_SCANCODE_G, 5, 0},
    {"Z", "Z", SDL_SCANCODE_Z, 6, 0}, {"X", "X", SDL_SCANCODE_X, 7, 0}, {"C", "C", SDL_SCANCODE_C, 8, 0},
    {"V", "V", SDL_SCANCODE_V, 9, 0}, {"B", "B", SDL_SCANCODE_B, 11, 0}, {"Q", "Q", SDL_SCANCODE_Q, 12, 0},
    {"E", "E", SDL_SCANCODE_E, 14, 0}, {"R", "R", SDL_SCANCODE_R, 15, 0}, {"Y", "Y", SDL_SCANCODE_Y, 16, 0},
    {"T", "T", SDL_SCANCODE_T, 17, 0}, {"O", "O", SDL_SCANCODE_O, 31, 0}, {"U", "U", SDL_SCANCODE_U, 32, 0},
    {"I", "I", SDL_SCANCODE_I, 34, 0}, {"P", "P", SDL_SCANCODE_P, 35, 0}, {"L", "L", SDL_SCANCODE_L, 37, 0},
    {"J", "J", SDL_SCANCODE_J, 38, 0}, {"K", "K", SDL_SCANCODE_K, 40, 0}, {"N", "N", SDL_SCANCODE_N, 45, 0},
    {"M", "M", SDL_SCANCODE_M, 46, 0},
    {"1", "1", SDL_SCANCODE_1, 18, 0}, {"2", "2", SDL_SCANCODE_2, 19, 0}, {"3", "3", SDL_SCANCODE_3, 20, 0},
    {"4", "4", SDL_SCANCODE_4, 21, 0}, {"5", "5", SDL_SCANCODE_5, 23, 0}, {"6", "6", SDL_SCANCODE_6, 22, 0},
    {"7", "7", SDL_SCANCODE_7, 26, 0}, {"8", "8", SDL_SCANCODE_8, 28, 0}, {"9", "9", SDL_SCANCODE_9, 25, 0},
    {"0", "0", SDL_SCANCODE_0, 29, 0},
    {"Minus", "-", SDL_SCANCODE_MINUS, 27, 0}, {"Equals", "=", SDL_SCANCODE_EQUALS, 24, 0},
    {"LeftBracket", "[", SDL_SCANCODE_LEFTBRACKET, 33, 0}, {"RightBracket", "]", SDL_SCANCODE_RIGHTBRACKET, 30, 0},
    {"Backslash", "\\", SDL_SCANCODE_BACKSLASH, 42, 0}, {"Semicolon", ";", SDL_SCANCODE_SEMICOLON, 41, 0},
    {"Apostrophe", "'", SDL_SCANCODE_APOSTROPHE, 39, 0}, {"Grave", "`", SDL_SCANCODE_GRAVE, 50, 0},
    {"Comma", ",", SDL_SCANCODE_COMMA, 43, 0}, {"Period", ".", SDL_SCANCODE_PERIOD, 47, 0},
    {"Slash", "/", SDL_SCANCODE_SLASH, 44, 0},
    {"Enter", "Enter", SDL_SCANCODE_RETURN, 36, 0}, {"Tab", "Tab", SDL_SCANCODE_TAB, 48, 0},
    {"Space", "Space", SDL_SCANCODE_SPACE, 49, 0}, {"Esc", "Esc", SDL_SCANCODE_ESCAPE, 53, 0},
    {"Backspace", "Backspace", SDL_SCANCODE_BACKSPACE, 51, 0}, {"CapsLock", "Caps Lock", SDL_SCANCODE_CAPSLOCK, 57, 0},
    {"LShift", "Left Shift", SDL_SCANCODE_LSHIFT, 56, 0}, {"RShift", "Right Shift", SDL_SCANCODE_RSHIFT, 60, 0},
    {"LAlt", "Left Alt", SDL_SCANCODE_LALT, 58, 0}, {"RAlt", "Right Alt", SDL_SCANCODE_RALT, 61, 0},
    {"LCtrl", "Left Ctrl", SDL_SCANCODE_LCTRL, 59, 0}, {"RCtrl", "Right Ctrl", SDL_SCANCODE_RCTRL, 62, 0},
};
#define UIT_KEY_COUNT ((int)(sizeof kUitKeys / sizeof *kUitKeys))

static inline int uit_token_eq(const char *a, size_t n, const char *b) {
    size_t m = strlen(b);
    if (m != n) return 0;
    for (size_t i = 0; i < n; i++) {
        char x = a[i], y = b[i];
        if (x >= 'A' && x <= 'Z') x = (char)(x - 'A' + 'a');
        if (y >= 'A' && y <= 'Z') y = (char)(y - 'A' + 'a');
        if (x != y) return 0;
    }
    return 1;
}

// Index into kUitPadInputs / kUitKeys for a token (case-insensitive), or -1.
static inline int uit_pad_find(const char *t, size_t n) {
    for (int i = 0; i < UIT_PAD_INPUT_COUNT; i++)
        if (uit_token_eq(t, n, kUitPadInputs[i].token)) return i;
    return -1;
}
static inline int uit_key_find(const char *t, size_t n) {
    for (int i = 0; i < UIT_KEY_COUNT; i++)
        if (uit_token_eq(t, n, kUitKeys[i].token)) return i;
    return -1;
}
static inline int uit_key_from_scancode(SDL_Scancode sc) {
    for (int i = 0; i < UIT_KEY_COUNT; i++)
        if (kUitKeys[i].sc == sc) return i;
    return -1;
}

// Calls fn(token, length, user) for each comma-separated token of a cfg value, spaces trimmed.
static inline void uit_tokens(const char *v, void (*fn)(const char *, size_t, void *), void *user) {
    while (v && *v) {
        while (*v == ' ' || *v == ',') v++;
        const char *e = v;
        while (*e && *e != ',') e++;
        const char *t = e;
        while (t > v && t[-1] == ' ') t--;
        if (t > v) fn(v, (size_t)(t - v), user);
        v = e;
    }
}

#endif
