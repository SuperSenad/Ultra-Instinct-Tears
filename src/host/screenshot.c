// [UIT 0346] screenshots (src/host/screenshot.h). The window thread asks (a key, the controller combo, the overlay's
// row); the present thread copies one frame (src/vk/present.c) and hands the pixels to a low priority thread of
// their own, which writes the PNG (SDL_SavePNG) and leaves the result for the toast. Unused, it costs nothing: the
// present thread checks one flag a frame.
//   TOTK_SCREENSHOT_KEY=F12    the key (- or no line: F12; 0: none); F7..F11 are the window's own keys
//   TOTK_SCREENSHOT_PAD=lefttrigger+back   a controller button or combo (SDL button names joined by '+', lefttrigger /
//                              righttrigger for ZL / ZR; - or none: no combo). It takes one when the last of its
//                              buttons goes down, once per press; the game does not see that button until let go.
#include "screenshot.h"

#include <SDL3/SDL.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "uit_overlay.h"

#ifdef _WIN32
#define SEP "\\"
#else
#define SEP "/"
#endif

static SDL_Scancode key = SDL_SCANCODE_F12;
static int key_choice;              // index in the row's choices
static uint32_t pad;
static int pad_choice;
static char pad_name[64];
static _Atomic int in_flight;       // asked for or being written
static SDL_Mutex *mx;               // done* from the writer threads
static SDL_Mutex *wmx;              // one writer at a time (the file name, the disk)
static char done1[96], done2[320];
static int done_new;
static char res1[96], res2[320];
static uint64_t res_until;

static int choice_value(int id, int i, char *v, size_t vn, char *l, size_t ln) {
    return uit_ov_choice(&kUitOvSettings[id], i, v, vn, l, ln);
}

static SDL_Scancode key_of(const char *v) {
    if (!strcmp(v, "-")) return SDL_SCANCODE_F12;
    if (!strcmp(v, "0") || !SDL_strcasecmp(v, "none")) return SDL_SCANCODE_UNKNOWN;
    SDL_Scancode s = SDL_GetScancodeFromName(v);
    if (s >= SDL_SCANCODE_F7 && s <= SDL_SCANCODE_F11) return SDL_SCANCODE_UNKNOWN;
    return s;
}

// "lefttrigger+back" -> the bits (0: none / not one known button)
static uint32_t pad_of(const char *v, char *name, size_t n) {
    static const struct { const char *sdl, *shown; } kNames[] = {
        {"leftstick", "L3"}, {"rightstick", "R3"}, {"back", "Minus"}, {"start", "Plus"}, {"guide", "Home"},
        {"leftshoulder", "L"}, {"rightshoulder", "R"}, {"lefttrigger", "ZL"}, {"righttrigger", "ZR"},
        {"touchpad", "Touchpad"}, {"dpup", "D-pad up"}, {"dpdown", "D-pad down"}, {"dpleft", "D-pad left"},
        {"dpright", "D-pad right"}};
    uint32_t bits = 0;
    if (name) name[0] = 0;
    if (!strcmp(v, "-") || !SDL_strcasecmp(v, "none")) return 0;
    char buf[128];
    snprintf(buf, sizeof buf, "%s", v);
    for (char *save = NULL, *t = SDL_strtok_r(buf, "+ ", &save); t; t = SDL_strtok_r(NULL, "+ ", &save)) {
        uint32_t b = 0;
        if (!SDL_strcasecmp(t, "lefttrigger")) b = 1u << 30;
        else if (!SDL_strcasecmp(t, "righttrigger")) b = 1u << 31;
        else {
            SDL_GamepadButton g = SDL_GetGamepadButtonFromString(t);
            if (g == SDL_GAMEPAD_BUTTON_MISC1) {   // the Capture button stays the system's / Steam's
                fprintf(stderr, "[shot] TOTK_SCREENSHOT_PAD: the Capture / Share button is not used (\"%s\" ignored)\n", t);
                continue;
            }
            if (g != SDL_GAMEPAD_BUTTON_INVALID && g < 30) b = 1u << g;
        }
        if (!b) { fprintf(stderr, "[shot] TOTK_SCREENSHOT_PAD: unknown button \"%s\" (ignored)\n", t); continue; }
        bits |= b;
        if (name) {
            const char *shown = t;
            for (size_t i = 0; i < sizeof kNames / sizeof *kNames; i++)
                if (!SDL_strcasecmp(kNames[i].sdl, t)) shown = kNames[i].shown;
            size_t l = strlen(name);
            snprintf(name + l, n - l, "%s%s", l ? " + " : "", shown);
        }
    }
    return bits;
}

static int find_choice(int id, const char *v) {
    char cv[64], cl[96];
    for (int i = 0; choice_value(id, i, cv, sizeof cv, cl, sizeof cl); i++)
        if (!SDL_strcasecmp(cv, v)) return i;
    return -1;
}

int uit_shot_key_of_choice(int i) {
    char v[64], l[96];
    return choice_value(OV_SHOTKEY, i, v, sizeof v, l, sizeof l) ? (int)key_of(v) : 0;
}

void uit_shot_set_key_choice(int i) {
    char v[64], l[96];
    if (!choice_value(OV_SHOTKEY, i, v, sizeof v, l, sizeof l)) return;
    key_choice = i;
    key = key_of(v);
}
void uit_shot_set_pad_choice(int i) {
    char v[64], l[96];
    if (!choice_value(OV_SHOTPAD, i, v, sizeof v, l, sizeof l)) return;
    pad_choice = i;
    pad = pad_of(v, pad_name, sizeof pad_name);
}

void uit_shot_init(void) {
    if (!mx) mx = SDL_CreateMutex();
    if (!wmx) wmx = SDL_CreateMutex();
    const char *e = SDL_getenv("TOTK_SCREENSHOT_KEY");
    key_choice = find_choice(OV_SHOTKEY, "-");
    if (e && *e) {
        key = key_of(e);
        key_choice = find_choice(OV_SHOTKEY, e);
        if (!key && strcmp(e, "0") && SDL_strcasecmp(e, "none")) {
            fprintf(stderr, "[shot] TOTK_SCREENSHOT_KEY=%s is not a free key: F12\n", e);
            key = SDL_SCANCODE_F12;
            key_choice = find_choice(OV_SHOTKEY, "-");
        }
    }
    e = SDL_getenv("TOTK_SCREENSHOT_PAD");
    pad_choice = find_choice(OV_SHOTPAD, "-");
    if (e && *e) {
        pad = pad_of(e, pad_name, sizeof pad_name);
        pad_choice = pad ? find_choice(OV_SHOTPAD, e) : pad_choice;
    }
    if (key || pad)
        fprintf(stderr, "[shot] screenshots: %s%s%s\n", key ? SDL_GetScancodeName(key) : "",
                key && pad ? ", or " : "", pad ? pad_name : "");
}

int uit_shot_key(void) { return (int)key; }
int uit_shot_key_choice(void) { return key_choice; }
uint32_t uit_shot_pad(void) { return pad; }
const char *uit_shot_pad_name(void) { return pad_name; }
int uit_shot_pad_choice(void) { return pad_choice; }

static void finish(const char *a, const char *b) {
    if (mx) SDL_LockMutex(mx);
    snprintf(done1, sizeof done1, "%s", a);
    snprintf(done2, sizeof done2, "%s", b ? b : "");
    done_new = 1;
    if (mx) SDL_UnlockMutex(mx);
    atomic_fetch_sub(&in_flight, 1);
}

void uit_shot_take(const char *how) {
    if (atomic_load(&in_flight) >= 2) {   // two waiting or being written: enough memory in use
        res_until = SDL_GetTicks() + 2500;
        snprintf(res1, sizeof res1, "Still saving the last screenshot");
        res2[0] = 0;
        return;
    }
    if (!vkp_active()) {
        res_until = SDL_GetTicks() + 2500;
        snprintf(res1, sizeof res1, "No picture to save");
        res2[0] = 0;
        return;
    }
    atomic_fetch_add(&in_flight, 1);
    if (!vkp_screenshot()) { atomic_fetch_sub(&in_flight, 1); return; }   // the last press is still waiting for a frame
    fprintf(stderr, "[shot] screenshot (%s)\n", how);
}

void uit_shot_failed(const char *why) {
    fprintf(stderr, "[shot] no screenshot: %s\n", why);
    finish("Screenshot failed", why);
}

// the folder: the user's Pictures (as Windows has it, also when moved) \ Ultra Instinct Tears
static int shot_dir(char *dir, size_t n) {
    const char *p = SDL_GetUserFolder(SDL_FOLDER_PICTURES);
    if (p && *p) snprintf(dir, n, "%s%sUltra Instinct Tears", p, p[strlen(p) - 1] == SEP[0] ? "" : SEP);
    else {
        const char *h = SDL_getenv("USERPROFILE");
        if (!h || !*h) h = SDL_getenv("HOME");
        if (!h || !*h) return 0;
        snprintf(dir, n, "%s" SEP "Pictures" SEP "Ultra Instinct Tears", h);
    }
    SDL_CreateDirectory(dir);   // with its parents; already there is fine
    SDL_PathInfo pi;
    return SDL_GetPathInfo(dir, &pi) && pi.type == SDL_PATHTYPE_DIRECTORY;
}

static int write_one(VkpShot *s);
static int SDLCALL writer(void *arg) {
    VkpShot *s = (VkpShot *)arg;
    SDL_SetCurrentThreadPriority(SDL_THREAD_PRIORITY_LOW);   // the game's threads first
    if (wmx) SDL_LockMutex(wmx);
    int r = write_one(s);
    if (wmx) SDL_UnlockMutex(wmx);
    return r;
}

static int write_one(VkpShot *s) {
    uint64_t t0 = SDL_GetTicksNS();
    char dir[1024], path[1200], part[1210], name[64];
    if (!shot_dir(dir, sizeof dir)) {
        fprintf(stderr, "[shot] no Pictures folder: %s\n", SDL_GetError());
        vkp_shot_release(s);
        finish("Screenshot failed", "Could not make the Pictures\\Ultra Instinct Tears folder (see the log)");
        return 0;
    }
    SDL_Time now = 0;
    SDL_DateTime dt = {0};
    SDL_GetCurrentTime(&now);
    SDL_TimeToDateTime(now, &dt, true);
    snprintf(name, sizeof name, "UIT-%04d%02d%02d-%02d%02d%02d", dt.year, dt.month, dt.day, dt.hour, dt.minute, dt.second);
    snprintf(path, sizeof path, "%s" SEP "%s.png", dir, name);
    for (int i = 2; i < 100 && SDL_GetPathInfo(path, NULL); i++)   // two in the same second
        snprintf(path, sizeof path, "%s" SEP "%s-%d.png", dir, name, i);
    snprintf(part, sizeof part, "%s.part", path);
    // x: the alpha byte is not the picture's (the window's image is opaque); the PNG gets 255
    SDL_Surface *sf = SDL_CreateSurfaceFrom((int)s->w, (int)s->h, s->bgr ? SDL_PIXELFORMAT_BGRX32 : SDL_PIXELFORMAT_RGBX32,
                                            (void *)s->px, (int)s->pitch);
    int ok = sf && SDL_SavePNG(sf, part) && SDL_RenamePath(part, path);
    if (!ok) {
        fprintf(stderr, "[shot] %s: %s\n", path, SDL_GetError());
        SDL_RemovePath(part);
    }
    SDL_DestroySurface(sf);
    uint32_t w = s->w, h = s->h;
    vkp_shot_release(s);
    if (!ok) { finish("Screenshot failed", "Could not write the PNG (see the log)"); return 0; }
    const char *file = strrchr(path, SEP[0]);
    fprintf(stderr, "[shot] %ux%u saved to %s in %.0f ms\n", w, h, path, (SDL_GetTicksNS() - t0) / 1e6);
    char where[320];
    snprintf(where, sizeof where, "Pictures" SEP "Ultra Instinct Tears" SEP "%s", file ? file + 1 : path);
    finish("Screenshot saved", where);
    return 0;
}

void uit_shot_ready(VkpShot *s) {
    SDL_Thread *t = SDL_CreateThread(writer, "uit-shot", s);
    if (t) SDL_DetachThread(t);
    else {
        vkp_shot_release(s);
        uit_shot_failed("no thread to write it");
    }
}

void uit_shot_poll(uint64_t now) {
    if (!mx) return;
    SDL_LockMutex(mx);
    if (done_new) {
        done_new = 0;
        memcpy(res1, done1, sizeof res1);
        memcpy(res2, done2, sizeof res2);
        res_until = now + 4000;
    }
    SDL_UnlockMutex(mx);
}

const char *uit_shot_text(uint64_t now, const char **line2) {
    *line2 = NULL;
    if (res_until && now < res_until) { *line2 = res2[0] ? res2 : NULL; return res1; }
    return NULL;
}
