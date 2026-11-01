// [UIT 0171] The in-game overlay's core (see uit_overlay.h). Portable C: tools/tests/test_overlay.cpp builds it on Linux.
#include "uit_overlay.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#endif

// ======================================================================== the settings
// Only what changes while the game runs (src/host/uit_live.h has the setters, each one checked in code); the values
// and words mirror launcher/settings.ini (tools/tests/test_overlay.cpp compares them), so the launcher reads back what
// the overlay saved. Kept out on purpose (the reasons are in docs/overlay.md): the resolution, aspect ratio, DLSS
// mode / preset, anisotropic filtering, the game-code features (Lift), 30 <-> 60 FPS, CPU and shader options, the
// audio device, language, folders and mods: the game reads them once, at start. The controller buttons and keyboard
// keys are not settings rows: the Controls tab ([UIT 0174], below) edits them as the launcher's bind dialog does.
#define B(on_, off_) .on = on_, .off = off_
// [UIT 0346] never the Capture / Share button (the system's, Steam's); L3 + R3 is also the overlay's default hold
#define SHOT_PAD_CHOICES                                                                                                 \
    "-:None|lefttrigger+back:ZL + Minus|righttrigger+back:ZR + Minus|leftshoulder+back:L + Minus|back+start:Minus + Plus|" \
    "leftstick+rightstick:L3 + R3 (both stick clicks)|guide:Guide / PS / Home button|touchpad:Touchpad press (PlayStation)"
const UitOvSetting kUitOvSettings[OV_COUNT] = {
    {OV_FSR, UIT_OV_TAB_DISPLAY, UIT_OVS_BOOL, "AMD FSR 1", "Sharper upscaling to the window size (F8). Off: plain bilinear.",
     "TOTK_UPSCALER", "off", B("on", "off"), NULL, 0, 0, 0, NULL, NULL, NULL,
     "Graphics > Image quality > Upscaler: AMD FSR 1"},
    {OV_DLSS, UIT_OV_TAB_DISPLAY, UIT_OVS_BOOL, "NVIDIA DLSS", "DLSS on or off for this session (F10); the upscaler itself is chosen in the launcher.",
     NULL, "on", B("on", "off"), NULL, 0, 0, 0, NULL, NULL, NULL,
     "Graphics > Image quality > Upscaler: NVIDIA DLSS"},
    {OV_SHARP, UIT_OV_TAB_DISPLAY, UIT_OVS_FLOAT, "Sharpening", "FSR 1 sharpening: 0 is the strongest, 2 the softest (F9 cycles).",
     "TOTK_FSR_SHARPNESS", "0.25", B(NULL, NULL), NULL, 0, 2, 0.05, NULL, "Maximum", NULL,
     "Graphics > Image quality > Sharpening"},
    {OV_FULLSCREEN, UIT_OV_TAB_DISPLAY, UIT_OVS_BOOL, "Fullscreen", "Borderless fullscreen (F11, Alt+Enter). Saved as how the game starts.",
     "TOTK_FULLSCREEN", "off", B("1", "-"), NULL, 0, 0, 0, NULL, NULL, NULL,
     "Graphics > Display > Start in fullscreen"},
    {OV_WINSIZE, UIT_OV_TAB_DISPLAY, UIT_OVS_CHOICE, "Window size", "The window's size when not fullscreen (the picture is scaled to it).",
     "TOTK_WINDOW_SIZE", "-", B(NULL, NULL),
     "-:1280 x 720|1600x900:1600 x 900|1920x1080:1920 x 1080|2560x1440:2560 x 1440|3840x2160:3840 x 2160", 0, 0, 0, NULL, NULL, NULL,
     "Graphics > Display > Window size"},
    {OV_PRESENT, UIT_OV_TAB_DISPLAY, UIT_OVS_CHOICE, "Present mode", "How frames reach the display. V-Sync never tears; Immediate has the least lag.",
     "TOTK_PRESENT_MODE", "-", B(NULL, NULL),
     "-:Automatic (mailbox when available)|mailbox:Mailbox (no tearing, low latency)|fifo:V-Sync (FIFO)|immediate:Immediate (tearing allowed)",
     0, 0, 0, NULL, NULL, NULL, "Graphics > More display settings > Present mode"},
    {OV_TRIPLE, UIT_OV_TAB_DISPLAY, UIT_OVS_CHOICE, "Triple buffering", "More images in flight: smoother under load, a little more lag.",
     "TOTK_TRIPLE_BUFFER", "-", B(NULL, NULL), "-:Automatic|on:On (3 images)|off:Off (as few as the display allows)",
     0, 0, 0, NULL, NULL, NULL, "Graphics > More display settings > Triple buffering"},

    {OV_FPS, UIT_OV_TAB_PERF, UIT_OVS_CHOICE, "Frame rate target", "60 or 120 while playing; 30 is chosen in the launcher (the game sets up for it at start).",
     "TOTK_FPS", "30", B(NULL, NULL), "30:30 FPS (original)|60:60 FPS|120:120 FPS (experimental)", 0, 0, 0, NULL, NULL, NULL,
     "Performance > Frame rate > Frame rate target"},
    {OV_HOLD, UIT_OV_TAB_PERF, UIT_OVS_INT, "Frame pacing hold", "Frames the game must run fast before it goes back up a frame rate step.",
     "TOTK_INTERVAL_HOLD", "8", B(NULL, NULL), NULL, 0, 30, 1, "frames", "Game decides", NULL,
     "Performance > Frame rate > Frame pacing hold"},
    {OV_MAXIV, UIT_OV_TAB_PERF, UIT_OVS_CHOICE, "Lowest frame rate step", "How far the frame rate may drop when the game falls behind.",
     "TOTK_MAX_INTERVAL", "-", B(NULL, NULL), "-:Half rate (default)|1:Never drop|3:One third rate|0:Game decides",
     0, 0, 0, NULL, NULL, NULL, "Performance > Frame rate > Lowest frame rate step"},
    // [UIT 0303] the window without the focus (another window in front, minimised): fewer frames, less GPU and CPU
    {OV_BGFPS, UIT_OV_TAB_PERF, UIT_OVS_CHOICE, "Frame rate in the background", "While the game window is not in front. Full speed again the moment you switch back.",
     "TOTK_BG_FPS", "-", B(NULL, NULL), "0:Off (no limit)|15:15 FPS|-:30 FPS (default)|60:60 FPS", 0, 0, 0, NULL, NULL, NULL,
     "Performance > Frame rate > Frame rate in the background"},
    {OV_TEXBUD, UIT_OV_TAB_PERF, UIT_OVS_INT, "Texture upload budget", "Texture data sent to the GPU per frame. Higher: textures sooner, more hitching.",
     "TOTK_TEX_BUDGET_MB", "256", B(NULL, NULL), NULL, 8, 1536, 8, "MB per frame", NULL, NULL,
     "Performance > Stutter > Textures > Texture upload budget"},
    {OV_TEXGRACE, UIT_OV_TAB_PERF, UIT_OVS_FLOAT, "Wait for nearly-ready textures", "How long a frame waits for a texture that is almost done (fewer pop-ins).",
     "TOTK_TEX_GRACE_MS", "2", B(NULL, NULL), NULL, 0, 50, 0.5, "ms", "Never wait", NULL,
     "Performance > Stutter > More texture settings > Wait for nearly-ready textures"},

    {OV_VOLUME, UIT_OV_TAB_GAME, UIT_OVS_INT, "Volume", "The game's overall volume.",
     "TOTK_VOLUME", "100", B(NULL, NULL), NULL, 0, 100, 5, "%", NULL, NULL, "Game > Sound > Volume"},
    {OV_RUMBLE, UIT_OV_TAB_GAME, UIT_OVS_BOOL, "Controller vibration", "Rumble on the controller.",
     "TOTK_RUMBLE", "on", B("-", "0"), NULL, 0, 0, 0, NULL, NULL, NULL, "Game > Controller > Controller vibration"},
    {OV_GYRO, UIT_OV_TAB_GAME, UIT_OVS_BOOL, "Motion controls (gyro)", "Aim with the controller's motion sensor.",
     "TOTK_GYRO", "on", B("-", "0"), NULL, 0, 0, 0, NULL, NULL, NULL, "Game > Controller > Motion controls (gyro)"},
    {OV_GYRODZ, UIT_OV_TAB_GAME, UIT_OVS_FLOAT, "Gyro deadzone", "Turning slower than this is ignored (hides sensor drift).",
     "TOTK_GYRO_DEADZONE", "-", B(NULL, NULL), NULL, 0, 20, 0.5, "deg/s", NULL, "Auto (none)",
     "Game > More controller settings > Gyro deadzone"},
    // [UIT 0290] stick deadzones, [UIT 0291] trigger press point (window_sdl.c pad_update, the next poll)
    {OV_LSDZ, UIT_OV_TAB_GAME, UIT_OVS_INT, "Left stick deadzone", "Stick movement nearer the centre than this is ignored (stops drift); the rest is rescaled.",
     "TOTK_STICK_DEADZONE_L", "0", B(NULL, NULL), NULL, 0, 50, 1, "%", "Off (as before)", NULL,
     "Game > Controller > Left stick deadzone"},
    {OV_RSDZ, UIT_OV_TAB_GAME, UIT_OVS_INT, "Right stick deadzone", "Stick movement nearer the centre than this is ignored (stops drift); the rest is rescaled.",
     "TOTK_STICK_DEADZONE_R", "0", B(NULL, NULL), NULL, 0, 50, 1, "%", "Off (as before)", NULL,
     "Game > Controller > Right stick deadzone"},
    {OV_STICKOUT, UIT_OV_TAB_GAME, UIT_OVS_INT, "Stick outer deadzone", "Both sticks reach full tilt this much before the edge.",
     "TOTK_STICK_OUTER", "0", B(NULL, NULL), NULL, 0, 30, 1, "%", "Off (as before)", NULL,
     "Game > More controller settings > Stick outer deadzone"},
    {OV_TRIGPT, UIT_OV_TAB_GAME, UIT_OVS_INT, "Trigger press point", "How far ZL / ZR must be pulled to count as pressed.",
     "TOTK_TRIGGER_POINT", "50", B(NULL, NULL), NULL, 5, 95, 5, "%", NULL, NULL,
     "Game > More controller settings > Trigger press point"},

    // [UIT 0175] native patches of the game code, switched while playing (src/host/cheats.c, tools/lifter/patches.py)
    {OV_CHEAT_DUR, UIT_OV_TAB_CHEATS, UIT_OVS_BOOL, "Infinite item durability", "Weapons, bows and shields never wear out from use (fire still burns wood).",
     "TOTK_CHEAT_DURABILITY", "off", B("1", "-"), NULL, 0, 0, 0, NULL, NULL, NULL, "Game > Cheats > Infinite item durability"},
    {OV_CHEAT_AMIIBO, UIT_OV_TAB_CHEATS, UIT_OVS_BOOL, "Limitless amiibo scanning", "The same amiibo again the same day: no \"can't be used again today\".",
     "TOTK_CHEAT_AMIIBO", "off", B("1", "-"), NULL, 0, 0, 0, NULL, NULL, NULL, "Game > Cheats > Limitless amiibo scanning"},
    // [UIT 0344]
    {OV_CHEAT_STAMINA, UIT_OV_TAB_CHEATS, UIT_OVS_BOOL, "Infinite stamina", "Running, climbing, swimming and gliding never use up the stamina wheel.",
     "TOTK_CHEAT_STAMINA", "off", B("1", "-"), NULL, 0, 0, 0, NULL, NULL, NULL, "Game > Cheats > Infinite stamina"},
    // [UIT 0345]
    {OV_CHEAT_ARROWS, UIT_OV_TAB_CHEATS, UIT_OVS_BOOL, "Infinite arrows", "Shooting a bow never uses up arrows.",
     "TOTK_CHEAT_ARROWS", "off", B("1", "-"), NULL, 0, 0, 0, NULL, NULL, NULL, "Game > Cheats > Infinite arrows"},

    {OV_SHOWFPS, UIT_OV_TAB_OVERLAY, UIT_OVS_BOOL, "Frame rate counter", "A small FPS counter in the corner while you play.",
     "TOTK_OVERLAY_FPS", "off", B("1", "-"), NULL, 0, 0, 0, NULL, NULL, NULL, "Game > In-game overlay > Frame rate counter"},
    // [UIT 0301] the last 3 s of frame times as bars, with a line at 16.7 ms (60 FPS)
    {OV_FTGRAPH, UIT_OV_TAB_OVERLAY, UIT_OVS_BOOL, "Frame-time graph", "The last 3 seconds of frame times under the counter; spikes are stutters.",
     "TOTK_OVERLAY_FRAMEGRAPH", "off", B("1", "-"), NULL, 0, 0, 0, NULL, NULL, NULL, "Game > In-game overlay > Frame-time graph"},
    {OV_SAVE, UIT_OV_TAB_OVERLAY, UIT_OVS_BOOL, "Save changes to the settings file", "Off: changes last for this session only (turning it on saves them).",
     NULL, "on", B("on", "off"), NULL, 0, 0, 0, NULL, NULL, NULL, NULL},
    {OV_MARK, UIT_OV_TAB_OVERLAY, UIT_OVS_ACTION, "Mark in log", "Writes a numbered mark to the log, as F7 does (for bug reports).",
     NULL, NULL, B(NULL, NULL), NULL, 0, 0, 0, NULL, NULL, NULL, NULL},
    // [UIT 0157] what every draw of the next frame read, for a bug report of misplaced ground (as Shift+F7)
    {OV_CAPTURE, UIT_OV_TAB_OVERLAY, UIT_OVS_ACTION, "Capture a frame for diagnosis",
     "Writes what the next frame's draws read to diag\\frame-capture-*.txt (as Shift+F7) for a bug report.",
     NULL, NULL, B(NULL, NULL), NULL, 0, 0, 0, NULL, NULL, NULL, NULL},
    // [UIT 0302] a timed frame-time recording: benchmarks\<date>-<time>.txt (summary) and .csv (every frame)
    {OV_BENCHLEN, UIT_OV_TAB_OVERLAY, UIT_OVS_INT, "Benchmark length", "How long a benchmark records.",
     "TOTK_BENCH_SECONDS", "60", B(NULL, NULL), NULL, 10, 300, 10, "s", NULL, NULL, "Game > In-game overlay > Benchmark length"},
    {OV_BENCH, UIT_OV_TAB_OVERLAY, UIT_OVS_ACTION, "Start benchmark",
     "Records frame times, then saves a report in the benchmarks folder (Shift+F8 too).",
     NULL, NULL, B(NULL, NULL), NULL, 0, 0, 0, NULL, NULL, NULL, NULL},
    // [UIT 0346] screenshots (screenshot.c): the picture as shown, without this menu, to Pictures\Ultra Instinct Tears
    {OV_SHOT, UIT_OV_TAB_OVERLAY, UIT_OVS_ACTION, "Take a screenshot",
     "Saves the next frame, without this menu, as a PNG in Pictures\\Ultra Instinct Tears.",
     NULL, NULL, B(NULL, NULL), NULL, 0, 0, 0, NULL, NULL, NULL, NULL},
    {OV_SHOTKEY, UIT_OV_TAB_OVERLAY, UIT_OVS_CHOICE, "Screenshot key", "The key that saves a screenshot while you play.",
     "TOTK_SCREENSHOT_KEY", "-", B(NULL, NULL),
     "-:F12|F2:F2|F3:F3|F4:F4|F5:F5|F6:F6|Insert:Insert|Home:Home|End:End|PageUp:Page Up|PageDown:Page Down|Pause:Pause|0:None",
     0, 0, 0, NULL, NULL, NULL, "Game > Screenshots > Screenshot key"},
    {OV_SHOTPAD, UIT_OV_TAB_OVERLAY, UIT_OVS_CHOICE, "Screenshot controller combo",
     "Buttons that save a screenshot together; the game does not see the last one pressed.",
     "TOTK_SCREENSHOT_PAD", "-", B(NULL, NULL), SHOT_PAD_CHOICES, 0, 0, 0, NULL, NULL, NULL,
     "Game > Screenshots > Screenshot controller combo"},
    {OV_KEYS, UIT_OV_TAB_OVERLAY, UIT_OVS_INFO, "Keys", NULL,
     NULL, NULL, B(NULL, NULL), NULL, 0, 0, 0, NULL, NULL, NULL, NULL},
    // [UIT 0292] both ask first (Cancel focused), then end the game the way closing its window does
    {OV_RESTART, UIT_OV_TAB_OVERLAY, UIT_OVS_ACTION, "Restart game",
     "Ends the game and starts it again as you launched it, with saved settings.",
     NULL, NULL, B(NULL, NULL), NULL, 0, 0, 0, NULL, NULL, NULL, NULL},
    {OV_QUIT, UIT_OV_TAB_OVERLAY, UIT_OVS_ACTION, "Quit game",
     "Closes the game and goes back to the desktop, as closing the window does.",
     NULL, NULL, B(NULL, NULL), NULL, 0, 0, 0, NULL, NULL, NULL, NULL},
};
#undef B
static const char *const kTabNames[UIT_OV_TABS] = {"Display", "Performance", "Game", "Controls", "Amiibo", "Cheats",
                                                             "Overlay"};

int uit_ov_choice_count(const UitOvSetting *s) {
    if (!s->choices) return 0;
    int n = 1;
    for (const char *p = s->choices; *p; p++) n += *p == '|';
    return n;
}

int uit_ov_choice(const UitOvSetting *s, int i, char *value, size_t vn, char *label, size_t ln) {
    if (!s->choices || i < 0) return 0;
    const char *p = s->choices;
    for (int k = 0; k < i; k++) {
        p = strchr(p, '|');
        if (!p) return 0;
        p++;
    }
    const char *end = strchr(p, '|'), *colon = strchr(p, ':');
    if (!end) end = p + strlen(p);
    if (!colon || colon > end) colon = end;
    if (value) snprintf(value, vn, "%.*s", (int)(colon - p), p);
    if (label) {
        const char *l = colon < end ? colon + 1 : p;
        snprintf(label, ln, "%.*s", (int)(end - l), l);
    }
    return 1;
}

static int choice_has(const UitOvSetting *s, const char *value) {
    char v[64];
    for (int i = 0; uit_ov_choice(s, i, v, sizeof v, NULL, 0); i++)
        if (!strcmp(v, value)) return 1;
    return 0;
}

// ======================================================================== saving (launcher/model.cpp)
// write_value: the line a value is saved as; 0 = no line (the default, or "-")
static int write_value(const UitOvSetting *s, double v, char *out, size_t n) {
    switch (s->kind) {
    case UIT_OVS_BOOL: {
        int b = v >= 0.5;
        if (b == !strcmp(s->def, "on")) return 0;
        const char *w = b ? s->on : s->off;
        if (!strcmp(w, "-")) return 0;
        snprintf(out, n, "%s", w);
        return 1;
    }
    case UIT_OVS_CHOICE: {
        char w[64];
        if (!uit_ov_choice(s, (int)v, w, sizeof w, NULL, 0)) return 0;
        if (!strcmp(w, "-") || (!strcmp(w, s->def) && !choice_has(s, "-"))) return 0;
        snprintf(out, n, "%s", w);
        return 1;
    }
    case UIT_OVS_INT:
    case UIT_OVS_FLOAT:
        if (s->autolabel && v < s->min) return 0;    // Value::Auto
        if (strcmp(s->def, "-") && !s->autolabel && fabs(v - atof(s->def)) < 1e-9) return 0;
        if (s->kind == UIT_OVS_INT) snprintf(out, n, "%lld", (long long)llround(v));
        else {
            snprintf(out, n, "%.3f", v);
            size_t l = strlen(out);
            while (l && out[l - 1] == '0') out[--l] = 0;
            if (l && out[l - 1] == '.') out[--l] = 0;
        }
        return 1;
    default:
        return 0;
    }
}

static int edit(UitCfgEdit *out, int max, int n, const char *key, const char *value) {
    if (n >= max) return n;
    out[n].key = key;
    out[n].remove = value == NULL;
    snprintf(out[n].value, sizeof out[n].value, "%s", value ? value : "");
    return n + 1;
}

int uit_ov_cfg_edits(int id, double v, int dlss_run, UitCfgEdit *out, int max) {
    if (id < 0 || id >= OV_COUNT) return 0;
    const UitOvSetting *s = &kUitOvSettings[id];
    if (id == OV_FSR) {
        // As the launcher keeps them: with DLSS as the upscaler, FSR 1 is on unless TOTK_FSR=0 (src/vk/present_fsr.c);
        // otherwise the upscaler choice says it, with the legacy TOTK_FSR switch kept in step (main.cpp App::set).
        if (dlss_run) return edit(out, max, 0, "TOTK_FSR", v >= 0.5 ? NULL : "0");
        int n = edit(out, max, 0, "TOTK_UPSCALER", v >= 0.5 ? "fsr" : NULL);
        return edit(out, max, n, "TOTK_FSR", v >= 0.5 ? "1" : NULL);
    }
    if (!s->key) return 0;
    char buf[64];
    return edit(out, max, 0, s->key, write_value(s, v, buf, sizeof buf) ? buf : NULL);
}

// The file as lines (model.cpp CfgFile::load / save / set)
typedef struct { char **l; int n, cap; } Lines;

static int lines_push(Lines *L, const char *p, size_t n) {
    if (L->n == L->cap) {
        int cap = L->cap ? L->cap * 2 : 64;
        char **nl = realloc(L->l, (size_t)cap * sizeof *nl);
        if (!nl) return 0;
        L->l = nl;
        L->cap = cap;
    }
    char *s = malloc(n + 1);
    if (!s) return 0;
    memcpy(s, p, n);
    s[n] = 0;
    L->l[L->n++] = s;
    return 1;
}

static void lines_free(Lines *L) {
    for (int i = 0; i < L->n; i++) free(L->l[i]);
    free(L->l);
    memset(L, 0, sizeof *L);
}

// CfgFile::line_key: the text before '=' with trailing blanks dropped; none for blank and '#' lines
static size_t line_key(const char *l, const char **k) {
    if (!*l || *l == '#') return 0;
    const char *eq = strchr(l, '=');
    if (!eq) return 0;
    size_t n = (size_t)(eq - l);
    while (n && (l[n - 1] == ' ' || l[n - 1] == '\t')) n--;
    *k = l;
    return n;
}

static int key_is(const char *l, const char *key) {
    const char *k;
    size_t n = line_key(l, &k);
    return n && n == strlen(key) && !memcmp(k, key, n);
}

static int cfg_set(Lines *L, const char *key, const char *value /* NULL: erase */) {
    int found = 0;
    for (int i = 0; i < L->n;) {
        if (!key_is(L->l[i], key)) { i++; continue; }
        if (!found && value) {
            size_t n = strlen(key) + strlen(value) + 2;
            char *s = malloc(n);
            if (!s) return 0;
            snprintf(s, n, "%s=%s", key, value);
            free(L->l[i]);
            L->l[i++] = s;
            found = 1;
            continue;
        }
        free(L->l[i]);
        memmove(&L->l[i], &L->l[i + 1], (size_t)(L->n - i - 1) * sizeof *L->l);
        L->n--;
    }
    if (found || !value) return 1;
    if (!strncmp(key, "TOTK_", 5)) {
        int header = 0;
        for (int i = 0; i < L->n && !header; i++) header = !strncmp(L->l[i], "# Your settings", 15);
        if (!header) {
            if (L->n && L->l[L->n - 1][0] && !lines_push(L, "", 0)) return 0;
            static const char h[] = "# Your settings (kept when tools/setup.py runs again):";
            if (!lines_push(L, h, sizeof h - 1)) return 0;
        }
    }
    char line[256];
    int n = snprintf(line, sizeof line, "%s=%s", key, value);
    return lines_push(L, line, (size_t)n);
}

int uit_cfg_edit_text(const char *in, size_t n, const UitCfgEdit *e, int ne, char **out, size_t *outn) {
    Lines L = {0};
    int crlf = 1;
    if (in && n) {
        crlf = memchr(in, '\n', n) == NULL;
        for (size_t i = 0; i + 1 < n && !crlf; i++) crlf = in[i] == '\r' && in[i + 1] == '\n';
        if (n >= 3 && (unsigned char)in[0] == 0xEF && (unsigned char)in[1] == 0xBB && (unsigned char)in[2] == 0xBF) { in += 3; n -= 3; }
        size_t a = 0;
        for (size_t i = 0; i <= n; i++) {
            if (i < n && in[i] != '\n') continue;
            size_t b = i;
            if (b > a && in[b - 1] == '\r') b--;
            if (!lines_push(&L, in + a, b - a)) { lines_free(&L); return 0; }
            a = i + 1;
        }
        if (L.n && !L.l[L.n - 1][0]) { free(L.l[--L.n]); }   // the newline that ends the file
    }
    for (int i = 0; i < ne; i++)
        if (!cfg_set(&L, e[i].key, e[i].remove ? NULL : e[i].value)) { lines_free(&L); return 0; }
    size_t total = 1;
    for (int i = 0; i < L.n; i++) total += strlen(L.l[i]) + 2;
    char *buf = malloc(total), *p = buf;
    if (!buf) { lines_free(&L); return 0; }
    for (int i = 0; i < L.n; i++) {
        size_t l = strlen(L.l[i]);
        memcpy(p, L.l[i], l);
        p += l;
        if (crlf) *p++ = '\r';
        *p++ = '\n';
    }
    *p = 0;
    *out = buf;
    *outn = (size_t)(p - buf);
    lines_free(&L);
    return 1;
}

int uit_cfg_apply(const char *path, const UitCfgEdit *e, int ne, char *err, size_t errn) {
    if (!path || !*path) { snprintf(err, errn, "no settings file"); return 0; }
    char *in = NULL;
    size_t n = 0;
    FILE *f = fopen(path, "rb");
    if (f) {
        fseek(f, 0, SEEK_END);
        long sz = ftell(f);
        fseek(f, 0, SEEK_SET);
        in = malloc(sz > 0 ? (size_t)sz : 1);
        if (in && sz > 0) n = fread(in, 1, (size_t)sz, f);
        fclose(f);
    }
    char *text;
    size_t tn;
    int ok = uit_cfg_edit_text(in, n, e, ne, &text, &tn);
    free(in);
    if (!ok) { snprintf(err, errn, "out of memory"); return 0; }
    char tmp[1024];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    f = fopen(tmp, "wb");
    if (!f) { free(text); snprintf(err, errn, "cannot write %s", tmp); return 0; }
    ok = fwrite(text, 1, tn, f) == tn;
    ok = fclose(f) == 0 && ok;
    free(text);
    if (ok) {
#ifdef _WIN32
        ok = MoveFileExA(tmp, path, MOVEFILE_REPLACE_EXISTING) != 0;
#else
        ok = rename(tmp, path) == 0;
#endif
    }
    if (!ok) { remove(tmp); snprintf(err, errn, "cannot save %s", path); return 0; }
    return 1;
}

// ======================================================================== fonts (stb_truetype: public domain / MIT)
#define STBTT_STATIC
#define STB_TRUETYPE_IMPLEMENTATION
#if defined(__clang__) || defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#endif
#include "../../third_party/imgui/imstb_truetype.h"
#if defined(__clang__) || defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

// beyond ASCII: [UIT 0177] whole ranges for the text the keyboard dialog shows (Inter has them all), then the chevrons,
// bullets, dashes, arrows and the keyboard's key marks the overlay draws
static const unsigned kRanges[][2] = {{0x20, 0x7F}, {0xA0, 0x100}, {0x100, 0x180}, {0x380, 0x3D0}, {0x400, 0x460}};
static const unsigned kExtra[16] = {0x2039, 0x203A, 0x2022, 0x2013, 0x2014, 0x2026, 0x2190, 0x2191, 0x2192, 0x2193,
                                    0x232B, 0x21E7, 0x2423, 0x21B5, 0x2018, 0x2019};
#define KRANGES ((int)(sizeof kRanges / sizeof kRanges[0]))
_Static_assert(95 + 96 + 128 + 80 + 96 + 16 == UIT_OV_GLYPHS, "kRanges + kExtra = UIT_OV_GLYPHS");
static const float kFontPx[UIT_OVF_COUNT] = {17.0f, 17.0f, 23.0f, 13.5f};   // at scale 1 (launcher: body, semibold, title, small)

static int glyph_index(unsigned cp) {
    if (cp >= 32 && cp < 127) return (int)cp - 32;
    int base = 0;
    for (int r = 0; r < KRANGES; r++) {
        if (cp >= kRanges[r][0] && cp < kRanges[r][1]) return base + (int)(cp - kRanges[r][0]);
        base += (int)(kRanges[r][1] - kRanges[r][0]);
    }
    for (int i = 0; i < 16; i++)
        if (kExtra[i] == cp) return base + i;
    return '?' - 32;
}

static unsigned glyph_cp(int i) {
    for (int r = 0; r < KRANGES; r++) {
        int n = (int)(kRanges[r][1] - kRanges[r][0]);
        if (i < n) return kRanges[r][0] + (unsigned)i;
        i -= n;
    }
    return kExtra[i];
}

int uit_ov_atlas_build(UitOvAtlas *a, const unsigned char *regular, const unsigned char *semi, float scale) {
    memset(a, 0, sizeof *a);
    stbtt_fontinfo fi[2];
    if (!stbtt_InitFont(&fi[0], regular, stbtt_GetFontOffsetForIndex(regular, 0)) ||
        !stbtt_InitFont(&fi[1], semi, stbtt_GetFontOffsetForIndex(semi, 0))) return 0;
    int W = scale > 1.6f ? 1024 : 512, H = 4096;
    uint8_t *pix = calloc((size_t)W * H, 1);
    if (!pix) return 0;
    int x = 1, y = 1, shelf = 0;
    for (int f = 0; f < UIT_OVF_COUNT; f++) {
        const stbtt_fontinfo *font = &fi[f == UIT_OVF_BODY || f == UIT_OVF_SMALL ? 0 : 1];
        float px = roundf(kFontPx[f] * scale);
        if (px < 6) px = 6;
        float sc = stbtt_ScaleForPixelHeight(font, px);
        int asc, desc, gap;
        stbtt_GetFontVMetrics(font, &asc, &desc, &gap);
        a->ascent[f] = roundf((float)asc * sc);
        a->descent[f] = roundf((float)-desc * sc);
        a->size[f] = px;
        for (int i = 0; i < UIT_OV_GLYPHS; i++) {
            unsigned cp = glyph_cp(i);
            int adv, lsb, x0, y0, x1, y1;
            stbtt_GetCodepointHMetrics(font, (int)cp, &adv, &lsb);
            stbtt_GetCodepointBitmapBox(font, (int)cp, sc, sc, &x0, &y0, &x1, &y1);
            UitOvGlyph *g = &a->g[f][i];
            g->adv = (float)adv * sc;
            int gw = x1 - x0, gh = y1 - y0;
            if (gw <= 0 || gh <= 0) continue;
            if (x + gw + 1 > W) { x = 1; y += shelf + 1; shelf = 0; }
            if (y + gh + 1 > H) { free(pix); return 0; }
            stbtt_MakeCodepointBitmap(font, pix + (size_t)y * W + x, gw, gh, W, sc, sc, (int)cp);
            *g = (UitOvGlyph){(float)x0, (float)y0, (float)x1, (float)y1, (float)x, (float)y, (float)(x + gw), (float)(y + gh), g->adv};
            x += gw + 1;
            if (gh > shelf) shelf = gh;
        }
    }
    int used = y + shelf + 1;
    used = (used + 63) & ~63;
    uint8_t *fit = realloc(pix, (size_t)W * used);
    a->pix = fit ? fit : pix;
    a->w = W;
    a->h = used;
    a->scale = scale;
    // texel coordinates to 0..1
    for (int f = 0; f < UIT_OVF_COUNT; f++)
        for (int i = 0; i < UIT_OV_GLYPHS; i++) {
            UitOvGlyph *g = &a->g[f][i];
            g->u0 /= (float)W; g->u1 /= (float)W; g->v0 /= (float)used; g->v1 /= (float)used;
        }
    return 1;
}

void uit_ov_atlas_free(UitOvAtlas *a) {
    free(a->pix);
    memset(a, 0, sizeof *a);
}

static unsigned utf8_next(const char **p) {
    const unsigned char *s = (const unsigned char *)*p;
    unsigned c = *s++;
    if (c >= 0xF0 && (s[0] & 0xC0) == 0x80 && (s[1] & 0xC0) == 0x80 && (s[2] & 0xC0) == 0x80) {
        c = ((c & 7) << 18) | ((s[0] & 63u) << 12) | ((s[1] & 63u) << 6) | (s[2] & 63u); s += 3;
    } else if (c >= 0xE0 && (s[0] & 0xC0) == 0x80 && (s[1] & 0xC0) == 0x80) {
        c = ((c & 15) << 12) | ((s[0] & 63u) << 6) | (s[1] & 63u); s += 2;
    } else if (c >= 0xC0 && (s[0] & 0xC0) == 0x80) {
        c = ((c & 31) << 6) | (s[0] & 63u); s += 1;
    }
    *p = (const char *)s;
    return c;
}

float uit_ov_text_width(const UitOvAtlas *a, int font, const char *t) {
    float w = 0;
    while (t && *t) w += a->g[font][glyph_index(utf8_next(&t))].adv;
    return w;
}

// ======================================================================== the overlay
static const UitOvSetting *S(int id) { return &kUitOvSettings[id]; }

static double get(UitOv *o, int id) {
    if (id == OV_SAVE) return o->save;
    if (id == OV_SHOWFPS) return o->show_fps;
    if (id == OV_FTGRAPH) return o->show_graph;
    return o->h && o->h->get ? o->h->get(id) : 0;
}

static const char *why(UitOv *o, int id) {
    if (id == OV_SAVE || id == OV_SHOWFPS || id == OV_FTGRAPH || id == OV_KEYS || id == OV_RESTART || id == OV_QUIT) return NULL;
    return o->h && o->h->why ? o->h->why(id) : NULL;
}

static int shown(UitOv *o, int id) {
    if (id == OV_RESTART || id == OV_QUIT) return o->h && o->h->quit;   // [UIT 0292] only where the game can be ended
    return !(o->h && o->h->shown) || o->h->shown(id);
}

static void status(UitOv *o, int bad, const char *fmt, const char *arg) {
    snprintf(o->status, sizeof o->status, fmt, arg ? arg : "");
    o->status_bad = bad;
    o->status_ms = o->now;
    o->dirty = 1;
}

static void rows_for_tab(UitOv *o) {
    o->nrows = 0;
    if (o->tab == UIT_OV_TAB_CONTROLS) {   // its own rows: the Switch buttons, then "All buttons"
        if (o->sel[o->tab] >= UIT_OV_BIND_ROWS) o->sel[o->tab] = UIT_OV_BIND_ROWS - 1;
        return;
    }
    if (o->tab == UIT_OV_TAB_AMIIBO) {   // its own rows too: choose a file, scan again, the folder's files
        if (o->sel[o->tab] >= UIT_OV_AM_ROWS(o)) o->sel[o->tab] = UIT_OV_AM_ROWS(o) - 1;
        return;
    }
    for (int i = 0; i < OV_COUNT; i++)
        if (S(i)->tab == o->tab && shown(o, i)) o->rows[o->nrows++] = i;
    if (o->sel[o->tab] >= o->nrows) o->sel[o->tab] = o->nrows ? o->nrows - 1 : 0;
}

void uit_ov_init(UitOv *o, const UitOvHooks *h, uint32_t combo, uint32_t hold_ms) {
    memset(o, 0, sizeof *o);
    o->h = h;
    o->save = 1;
    o->combo = combo;
    o->hold_ms = hold_ms ? hold_ms : 1000;
    o->dirty = 1;
}

void uit_ov_set_combo2(UitOv *o, uint32_t combo2) { o->combo2 = combo2 == o->combo ? 0 : combo2; }

uint32_t uit_ov_face_nav(uint32_t phys, int confirm_game) {
    uint32_t nav = 0, south = phys & 1u, east = phys & 2u;
    if (confirm_game ? east : south) nav |= OVNAV_CONFIRM;
    if (confirm_game ? south : east) nav |= OVNAV_BACK;
    if (phys & 4u) nav |= OVNAV_RESET;
    if (phys & 8u) nav |= OVNAV_Y;
    return nav;
}

const char *uit_ov_face_label(int family, int pos) {
    static const char *const k[4][4] = {{"A", "B", "X", "Y"}, {"A", "B", "X", "Y"},   // generic (named as Xbox), Xbox
                                        {"Cross", "Circle", "Square", "Triangle"}, {"B", "A", "Y", "X"}};
    return k[family >= 0 && family < 4 ? family : 0][pos & 3];
}

static const char *cfg_path(UitOv *o) {
    const char *p = o->h && o->h->cfg_path ? o->h->cfg_path() : NULL;
    return p && *p ? p : NULL;
}

static int any_pending(const UitOv *o) {
    int any = 0;
    for (int i = 0; i < OV_COUNT; i++) any |= o->pending[i];
    for (int k = 0; k < 2; k++)
        for (int b = 0; b < UIT_BIND_BUTTONS; b++) any |= o->bpending[k][b];
    return any;
}

int uit_ov_flush(UitOv *o, uint64_t now) {
    o->now = now;
    if (!any_pending(o) || !o->save) return 0;
    const char *path = cfg_path(o);
    if (!path) {
        memset(o->pending, 0, sizeof o->pending);
        memset(o->bpending, 0, sizeof o->bpending);
        status(o, 1, "No ultrainstincttears.cfg was loaded: changes last for this session%s", NULL);
        return 0;
    }
    UitCfgEdit e[2 * OV_COUNT + 2 * UIT_BIND_BUTTONS];
    int n = 0, max = (int)(sizeof e / sizeof e[0]);
    int dlss = o->h && o->h->dlss_run ? o->h->dlss_run() : 0;
    for (int i = 0; i < OV_COUNT; i++)
        if (o->pending[i]) n += uit_ov_cfg_edits(i, get(o, i), dlss, e + n, max - n);
    // the bindings: the launcher's lines (model.cpp write_value: the comma list, "none", or no line for the default)
    for (int k = 0; k < 2; k++)
        for (int b = 0; b < UIT_BIND_BUTTONS && n < max; b++)
            if (o->bpending[k][b]) {
                e[n].key = uit_bind_cfg_key(k, b);
                e[n].remove = !uit_binds_cfg_value(&o->binds, k, b, e[n].value, sizeof e[n].value);
                n++;
            }
    char err[140];
    int ok = uit_cfg_apply(path, e, n, err, sizeof err);
    if (ok) {
        memset(o->pending, 0, sizeof o->pending);
        memset(o->bpending, 0, sizeof o->bpending);
        status(o, 0, "Saved to ultrainstincttears.cfg%s", NULL);
    } else
        status(o, 1, "Not saved: %s", err);   // kept pending: tried again with the next change or on close
    return ok;
}

int uit_ov_tick(UitOv *o, uint64_t now) {
    o->now = now;
    if (o->cap && now - o->cap_t0 >= UIT_OV_BIND_MS) uit_ov_capture_cancel(o, "No press in time");
    if (any_pending(o) && o->save && now - o->change_ms >= 400) return uit_ov_flush(o, now);
    return 0;
}

static void changed(UitOv *o, int id) {
    o->dirty = 1;
    if (id == OV_SAVE || id == OV_MARK || id == OV_CAPTURE || id == OV_BENCH || id == OV_SHOT || id == OV_KEYS || id == OV_DLSS ||
        id == OV_RESTART || id == OV_QUIT) {
        if (id == OV_DLSS) status(o, 0, "DLSS on / off lasts for this session (the launcher picks the upscaler)%s", NULL);
        return;
    }
    o->pending[id] = 1;
    o->change_ms = o->now;
    if (!o->save) status(o, 0, "This session only (saving is off on the Overlay tab)%s", NULL);
    else o->status[0] = 0;
}

static void set(UitOv *o, int id, double v) {
    if (id == OV_SAVE) {
        o->save = v >= 0.5;
        o->dirty = 1;
        if (o->save) {
            if (any_pending(o)) uit_ov_flush(o, o->now);
            else status(o, 0, "Changes are saved to ultrainstincttears.cfg%s", NULL);
        } else status(o, 0, "Changes now last for this session only%s", NULL);
        return;
    }
    if (id == OV_SHOWFPS) o->show_fps = v >= 0.5;
    else if (id == OV_FTGRAPH) o->show_graph = v >= 0.5;
    else if (o->h && o->h->set) o->h->set(id, v);
    changed(o, id);
}

static int choice_ok(UitOv *o, int id, int i) {
    return !(o->h && o->h->choice_ok) || o->h->choice_ok(id, i);
}

// One step of a setting: dir -1 / +1 (left / right), 0 = confirm (toggle, next choice, run)
static void step(UitOv *o, int id, int dir, int fast) {
    const UitOvSetting *s = S(id);
    if (s->kind == UIT_OVS_INFO) return;
    const char *w = why(o, id);
    if (w) { status(o, 1, "%s", w); return; }
    double v = get(o, id);
    switch (s->kind) {
    case UIT_OVS_BOOL: {
        int b = v >= 0.5, nb = dir ? dir > 0 : !b;
        if (nb != b) set(o, id, nb);
        break;
    }
    case UIT_OVS_CHOICE: {
        int n = uit_ov_choice_count(s), i = (int)v, d = dir ? dir : 1;
        for (int k = 0; k < n; k++) {
            int j = i < 0 ? (d > 0 ? 0 : n - 1) : i + d;
            if (!dir) j = ((j % n) + n) % n;       // confirm wraps around; the arrows stop at the ends
            if (j < 0 || j >= n) return;
            if (choice_ok(o, id, j)) { set(o, id, j); return; }
            i = j;
        }
        break;
    }
    case UIT_OVS_INT:
    case UIT_OVS_FLOAT: {
        if (!dir) return;
        double st = s->step * (fast ? ((s->max - s->min) / s->step > 60 ? 8 : 2) : 1), nv;
        if (s->autolabel && v < s->min) nv = dir > 0 ? s->min : v;
        else {
            nv = v + dir * st;
            nv = s->min + floor((nv - s->min) / s->step + 0.5) * s->step;
            if (nv > s->max) nv = s->max;
            if (nv < s->min) nv = s->autolabel && v <= s->min + 1e-9 ? s->min - 1 : s->min;
        }
        if (fabs(nv - v) > 1e-9) set(o, id, nv);
        break;
    }
    case UIT_OVS_ACTION:
        if (id == OV_RESTART || id == OV_QUIT) {   // [UIT 0292] ask first: Cancel is focused
            if (dir == 0 && o->h && o->h->quit && !o->exit_req) {
                o->ask = id;
                o->ask_sel = 0;
                o->ask_panel = o->ask_btn[0] = o->ask_btn[1] = (UitOvBox){0};   // laid out by the next build
                o->drag = 0;
                o->dirty = 1;
            }
            break;
        }
        if (dir == 0 && o->h && o->h->action) {
            o->h->action(id);
            if (id == OV_SHOT) status(o, 0, "%s", "Screenshot: the next frame, without this menu, goes to Pictures\\Ultra Instinct Tears");
            else if (id == OV_CAPTURE) status(o, 0, "%s: the next frame goes to diag\\frame-capture-*.txt (the log names it)", s->name);
            else if (id == OV_BENCH)   // the glue has started / stopped it (bench_on)
                status(o, 0, "%s", o->bench_on ? "Benchmark running: close the overlay and play (Shift+F8 stops it)"
                                               : "Benchmark stopped: the report goes to the benchmarks folder");
            else status(o, 0, "%s: done", s->name);
        }
        break;
    default:
        break;
    }
}

static void amiibo_refresh(UitOv *o) {   // [UIT 0175] the folder is read when the tab is shown (and on Del / X)
    int n = o->h && o->h->amiibo_list ? o->h->amiibo_list(1) : 0;
    o->am_n = n < 0 ? 0 : n;
    o->dirty = 1;
}

static void set_tab(UitOv *o, int t) {
    o->tab = (t + UIT_OV_TABS) % UIT_OV_TABS;
    if (o->tab == UIT_OV_TAB_AMIIBO) amiibo_refresh(o);
    rows_for_tab(o);
    o->dirty = 1;
}

// ======================================================================== [UIT 0174] the Controls tab
// The launcher's bind dialog (launcher/main.cpp bind_*), with its words: pick a Switch button, press the new button
// or key (8 s, Esc cancels, Delete clears), and a button another Switch button had swaps over. Each change goes to the
// game at once (hooks->binds_set: window_sdl.c builds new tables and swaps them in) and is saved like the settings.
static int ok_kind(int kind) { return kind == UIT_BIND_PAD || kind == UIT_BIND_KEY; }
static int ok_bind(int b) { return b >= 0 && b < UIT_BIND_BUTTONS; }
static const char *kind_where(int kind) { return kind == UIT_BIND_PAD ? "on the controller" : "on the keyboard"; }

static void flash(UitOv *o, int bad, const char *msg) {
    if (!bad && !o->save) {
        char m[sizeof o->status];
        snprintf(m, sizeof m, "%s  \xc2\xb7  this session only", msg);
        status(o, bad, "%s", m);
    } else
        status(o, bad, "%s", msg);
}

static void binds_changed(UitOv *o, int kind, int b, int also) {
    if (o->h && o->h->binds_set) o->h->binds_set(&o->binds);
    if (ok_bind(b)) o->bpending[kind][b] = 1;
    if (ok_bind(also)) o->bpending[kind][also] = 1;
    o->change_ms = o->now;
    o->dirty = 1;
}

void uit_ov_set_binds(UitOv *o, const UitBinds *b) {
    if (!b) return;
    o->binds = *b;
    o->have_binds = 1;
    memset(o->bpending, 0, sizeof o->bpending);
    o->dirty = 1;
}

int uit_ov_capturing(const UitOv *o) { return o->cap ? 1 + o->cap_kind : 0; }

void uit_ov_capture_start(UitOv *o, int kind, int b) {
    if (!o->open || !ok_kind(kind) || !ok_bind(b)) return;
    if (!o->have_binds) { status(o, 1, "The bindings were not loaded, so they cannot change here%s", NULL); return; }
    if (kind == UIT_BIND_PAD && !o->pad_present) {
        status(o, 1, "Connect a controller to change its buttons%s", NULL);
        return;
    }
    o->cap = 1;
    o->cap_kind = kind;
    o->cap_btn = b;
    o->cap_t0 = o->now;
    o->cap_held = o->phys_now;   // A (or the click's button) is still down: it binds only once let go and pressed again
    o->status[0] = 0;
    o->dirty = 1;
}

void uit_ov_capture_cancel(UitOv *o, const char *why) {
    if (!o->cap) return;
    o->cap = 0;
    char m[sizeof o->status];
    snprintf(m, sizeof m, "%s: Switch %s is unchanged", why ? why : "Cancelled", uit_bind_button_name(o->cap_btn));
    status(o, 0, "%s", m);
}

void uit_ov_capture_clear(UitOv *o) {
    if (!o->cap) return;
    o->cap = 0;
    int k = o->cap_kind, b = o->cap_btn;
    if (o->binds.b[k][b].n) {
        uit_binds_clear(&o->binds, k, b);
        binds_changed(o, k, b, -1);
    }
    char m[sizeof o->status];
    snprintf(m, sizeof m, "Switch %s is not bound %s now (Del / %s on the row puts the default back)", uit_bind_button_name(b), kind_where(k),
             uit_ov_face_label(o->pad_present ? o->pad_family : UIT_PADFAM_GENERIC, 2));
    flash(o, 0, m);
}

void uit_ov_capture_refuse(UitOv *o, const char *why) {
    if (o->cap) status(o, 1, "%s", why);
}

static void capture_apply(UitOv *o, int in) {
    int k = o->cap_kind, b = o->cap_btn, moved = -1;
    o->cap = 0;
    o->dirty = 1;
    const char *what = uit_bind_label(k, in, o->pad_family);
    char m[sizeof o->status];
    if (!uit_binds_assign(&o->binds, k, b, in, &moved)) {
        snprintf(m, sizeof m, "Switch %s already uses %s", uit_bind_button_name(b), what);
        status(o, 0, "%s", m);
        return;
    }
    binds_changed(o, k, b, moved);
    if (moved >= 0) {
        char t[96];
        uit_binds_text(&o->binds, k, moved, o->pad_family, t, sizeof t);
        snprintf(m, sizeof m, "Switch %s: %s  \xc2\xb7  Switch %s now uses %s", uit_bind_button_name(b), what, uit_bind_button_name(moved), t);
    } else
        snprintf(m, sizeof m, "Switch %s: %s", uit_bind_button_name(b), what);
    flash(o, 0, m);
}

void uit_ov_capture_key(UitOv *o, int sc, const char *name) {
    if (!o->cap || o->cap_kind != UIT_BIND_KEY) return;   // waiting for the controller: only Esc and Delete count
    int k = uit_bind_key_from_scancode(sc);
    char m[sizeof o->status];
    if (k < 0) {
        snprintf(m, sizeof m, "%s is not a key the game can read. Pick another one.", name && *name ? name : "That key");
        uit_ov_capture_refuse(o, m);
        return;
    }
    int st = uit_bind_key_is_stick(k);
    if (st) {
        snprintf(m, sizeof m, "%s%s, so it cannot press a button. Pick another key.", uit_bind_label(UIT_BIND_KEY, k, 0),
                 st == 1 ? " moves Link (the left stick)" : " moves the camera (the right stick)");
        uit_ov_capture_refuse(o, m);
        return;
    }
    capture_apply(o, k);
}

// A press of physical bit `bit` while waiting for the controller (1: it bound, 0: refused)
static int capture_pad(UitOv *o, int bit) {
    char m[sizeof o->status];
    if (o->combo == 1u << bit || o->combo2 == 1u << bit) {   // the overlay combo is this one button: the game never sees it, so it cannot press one
        int in = uit_bind_pad_from_phys(bit);
        snprintf(m, sizeof m, "%s opens the overlay (Game > In-game overlay > Overlay controller combo), so it cannot press a Switch button. Pick another one.",
                 in >= 0 ? uit_bind_label(UIT_BIND_PAD, in, o->pad_family) : "That button");
        uit_ov_capture_refuse(o, m);
        return 0;
    }
    int in = uit_bind_pad_from_phys(bit);
    if (in < 0) {
        uit_ov_capture_refuse(o, "That button is not one the game can read. Pick another one.");
        return 0;
    }
    capture_apply(o, in);
    return 1;
}

void uit_ov_bind_reset(UitOv *o, int kind, int b) {
    if (!o->have_binds || !ok_kind(kind)) return;
    char m[sizeof o->status];
    if (b < 0) {
        int any = 0;
        for (int i = 0; i < UIT_BIND_BUTTONS; i++)
            if (!uit_bind_is_default(&o->binds, kind, i)) { uit_binds_reset(&o->binds, kind, i); o->bpending[kind][i] = 1; any = 1; }
        if (any) binds_changed(o, kind, -1, -1);
        snprintf(m, sizeof m, any ? "Every Switch button %s is back to its default" : "Every Switch button %s already uses its default", kind_where(kind));
        flash(o, 0, m);
        return;
    }
    if (!ok_bind(b)) return;
    char t[96];
    if (uit_bind_is_default(&o->binds, kind, b)) {
        uit_binds_text(&o->binds, kind, b, o->pad_family, t, sizeof t);
        snprintf(m, sizeof m, "Switch %s already uses its default, %s", uit_bind_button_name(b), t);
        status(o, 0, "%s", m);
        return;
    }
    uit_binds_reset(&o->binds, kind, b);
    binds_changed(o, kind, b, -1);
    uit_binds_text(&o->binds, kind, b, o->pad_family, t, sizeof t);
    int n = snprintf(m, sizeof m, "Switch %s: %s (the default)", uit_bind_button_name(b), t);
    // the default may be what another Switch button took in a swap: say so (both press now; pick one to change)
    const UitBindList *l = &o->binds.b[kind][b];
    int warn = 0;
    for (int j = 0; j < UIT_BIND_BUTTONS && n > 0 && n < (int)sizeof m; j++) {
        if (j == b) continue;
        const UitBindList *x = &o->binds.b[kind][j];
        int shared = 0;
        for (int p = 0; p < l->n; p++)
            for (int q = 0; q < x->n; q++) shared |= l->in[p] == x->in[q];
        if (shared) warn = 1;
        if (shared) n += snprintf(m + n, sizeof m - (size_t)n, "  \xc2\xb7  Switch %s uses it too", uit_bind_button_name(j));
    }
    flash(o, warn, m);
}

void uit_ov_set_open(UitOv *o, int open, uint32_t held) {
    open = !!open;
    if (o->open == open) return;
    o->open = open;
    o->drag = 0;
    o->dirty = 1;
    o->cap = 0;   // closing ends a capture (nothing changed); nothing pressed for it reaches the game (the mask, input clear)
    o->ask = 0;   // [UIT 0292] and the Restart / Quit question (as Cancel)
    if (open) {
        if (o->tab == UIT_OV_TAB_AMIIBO) amiibo_refresh(o);
        rows_for_tab(o);
        o->status[0] = 0;
    } else {
        o->suppress |= held;   // the button that closed it (B, the combo) must not reach the game when the game sees input again
        uit_ov_flush(o, o->now);
    }
    o->nav_prev = 0;
}

static void controls_input(UitOv *o, UitOvInput in) {
    int *sel = &o->sel[UIT_OV_TAB_CONTROLS], all = *sel == UIT_BIND_BUTTONS;
    switch (in) {
    case OVIN_UP: if (*sel > 0) (*sel)--; break;
    case OVIN_DOWN: if (*sel + 1 < UIT_OV_BIND_ROWS) (*sel)++; break;
    case OVIN_LEFT: o->bcol = UIT_BIND_PAD; break;
    case OVIN_RIGHT: o->bcol = UIT_BIND_KEY; break;
    case OVIN_CONFIRM:
        if (all) uit_ov_bind_reset(o, o->bcol, -1);
        else uit_ov_capture_start(o, o->bcol, *sel);
        break;
    case OVIN_RESET: uit_ov_bind_reset(o, o->bcol, all ? -1 : *sel); break;
    default: break;
    }
    o->dirty = 1;
}

// ======================================================================== [UIT 0175] the Amiibo tab
// Row 0 opens the system's file dialog (the glue answers through uit_ov_note), row 1 scans the last amiibo again,
// rows 2.. are the amiibo folder's files. A scan puts the figure on the game's reader (src/host/amiibo.h): at once when
// the game is searching, else it waits there UIT_NFP_HOLD_MS for the game to ask.
void uit_ov_note(UitOv *o, int bad, const char *msg) { status(o, bad, "%s", msg); }

static void amiibo_do(UitOv *o, int row) {
    if (!o->h || !o->h->amiibo_scan) { status(o, 1, "Amiibo scanning is not available in this build%s", NULL); return; }
    if (row == 0) {
        if (o->h->amiibo_pick) o->h->amiibo_pick();
        else status(o, 1, "No file dialog here: put the file in the amiibo folder%s", NULL);
        return;
    }
    if (row == 1 && !(o->h->amiibo_last && o->h->amiibo_last())) { status(o, 1, "Nothing scanned yet: choose a file first%s", NULL); return; }
    char m[sizeof o->status];
    m[0] = 0;
    int bad = o->h->amiibo_scan(row == 1 ? -1 : row - 2, m, sizeof m);
    if (!m[0]) snprintf(m, sizeof m, "%s", bad ? "Not scanned" : "Scanned");
    status(o, bad != 0, "%s", m);
}

static void amiibo_input(UitOv *o, UitOvInput in) {
    int *sel = &o->sel[UIT_OV_TAB_AMIIBO];
    switch (in) {
    case OVIN_UP: if (*sel > 0) (*sel)--; break;
    case OVIN_DOWN: if (*sel + 1 < UIT_OV_AM_ROWS(o)) (*sel)++; break;
    case OVIN_CONFIRM: amiibo_do(o, *sel); break;
    case OVIN_RESET: {   // read the folder again
        amiibo_refresh(o);
        rows_for_tab(o);
        char m[128];
        int nf = o->h && o->h->amiibo_files ? o->h->amiibo_files() : o->am_n;   // [UIT 0181] the rest are built in
        if (nf < 0 || nf > o->am_n) nf = o->am_n;
        snprintf(m, sizeof m, "%d amiibo file%s in the folder", nf, nf == 1 ? "" : "s");
        if (o->am_n > nf) snprintf(m + strlen(m), sizeof m - strlen(m), ", %d built-in figures", o->am_n - nf);
        status(o, 0, "%s", m);
        break;
    }
    default: break;
    }
    o->dirty = 1;
}

// ======================================================================== [UIT 0292] Restart / Quit game: the question
// "Quit to desktop?" / "Restart the game?" with Cancel (focused first) and Quit / Restart. While it is shown it takes every
// input: left / right (and up / down, the tab keys) move between the two, confirm presses the focused one, back cancels,
// a click outside it cancels, closing the overlay cancels. Confirmed: the quit hook ends the game (overlay_live.c).
static void ask_end(UitOv *o, int go) {
    int id = o->ask;
    if (!id) return;
    o->ask = 0;
    o->ask_sel = 0;
    o->dirty = 1;
    if (!go) { status(o, 0, "%s: cancelled", S(id)->name); return; }
    int restart = id == OV_RESTART;
    o->exit_req = restart ? 2 : 1;
    status(o, 0, restart ? "Restarting the game%s" : "Quitting the game%s", "\xe2\x80\xa6");
    if (o->h && o->h->quit && !o->h->quit(restart)) {
        o->exit_req = 0;
        status(o, 1, "Could not close the game%s: use the window's close button", "");
    }
}

static void ask_input(UitOv *o, UitOvInput in) {
    switch (in) {
    case OVIN_LEFT: o->ask_sel = 0; break;
    case OVIN_RIGHT: o->ask_sel = 1; break;
    case OVIN_UP: case OVIN_DOWN: case OVIN_TAB_PREV: case OVIN_TAB_NEXT: o->ask_sel = !o->ask_sel; break;
    case OVIN_CONFIRM: ask_end(o, o->ask_sel == 1); return;
    case OVIN_BACK: ask_end(o, 0); return;
    default: return;
    }
    o->dirty = 1;
}

void uit_ov_input(UitOv *o, UitOvInput in) {
    if (!o->open || o->kb.active) return;   // [UIT 0177] the dialog takes its keys through uit_ov_kb_edit / _text
    rows_for_tab(o);
    if (o->cap) {   // the glue sends the capture its own keys; anything else only ends it
        if (in == OVIN_BACK) uit_ov_capture_cancel(o, "Cancelled");
        else if (in == OVIN_RESET) uit_ov_capture_clear(o);
        return;
    }
    if (o->ask) { ask_input(o, in); return; }   // [UIT 0292]
    if (o->tab == UIT_OV_TAB_CONTROLS && in != OVIN_BACK && in != OVIN_TAB_PREV && in != OVIN_TAB_NEXT) {
        controls_input(o, in);
        return;
    }
    if (o->tab == UIT_OV_TAB_AMIIBO && in != OVIN_BACK && in != OVIN_TAB_PREV && in != OVIN_TAB_NEXT) {
        amiibo_input(o, in);
        return;
    }
    int *sel = &o->sel[o->tab], id = o->nrows ? o->rows[*sel] : -1;
    switch (in) {
    case OVIN_UP: if (*sel > 0) (*sel)--; o->dirty = 1; break;
    case OVIN_DOWN: if (*sel + 1 < o->nrows) (*sel)++; o->dirty = 1; break;
    case OVIN_LEFT: if (id >= 0) step(o, id, -1, o->nav_rep > 8); break;
    case OVIN_RIGHT: if (id >= 0) step(o, id, +1, o->nav_rep > 8); break;
    case OVIN_CONFIRM: if (id >= 0) step(o, id, 0, 0); break;
    case OVIN_BACK: uit_ov_set_open(o, 0, 0); break;
    case OVIN_TAB_PREV: set_tab(o, o->tab - 1); break;
    case OVIN_TAB_NEXT: set_tab(o, o->tab + 1); break;
    case OVIN_RESET: break;
    }
}

static int in_box(const UitOvBox *b, float x, float y) {
    return b->x1 > b->x0 && x >= b->x0 && x < b->x1 && y >= b->y0 && y < b->y1;
}

// ======================================================================== [UIT 0177] the keyboard dialog: input
// The grid: 4 rows of 10 characters per page (UTF-8, one character each), then the special keys (UIT_KBK_*), whose
// widths (in character keys) add up to a row's.
static const char *const kKbPages[UIT_KB_PAGES][4] = {
    {"1234567890", "qwertyuiop", "asdfghjkl'", "zxcvbnm,.-"},
    {"!@#$%^&*()", "-_=+[]{}\\|", ";:'\",.<>/?", "~`\xc2\xa1\xc2\xbf\xc2\xa3\xc2\xa5\xc2\xa7\xc2\xb0\xc3\x97\xc3\xb7"},
    {"\xc3\xa0\xc3\xa1\xc3\xa2\xc3\xa4\xc3\xa3\xc3\xa5\xc3\xa6\xc3\xa7\xc3\xa8\xc3\xa9",
     "\xc3\xaa\xc3\xab\xc3\xac\xc3\xad\xc3\xae\xc3\xaf\xc3\xb1\xc3\xb2\xc3\xb3\xc3\xb4",
     "\xc3\xb6\xc3\xb5\xc3\xb8\xc5\x93\xc3\xb9\xc3\xba\xc3\xbb\xc3\xbc\xc3\xbd\xc3\xbf",
     "\xc3\x9f\xc3\xb0\xc3\xbe\xc5\x82\xc5\x9b\xc5\xba\xc5\xbc\xc4\x8d\xc5\xa1\xc5\xbe"}};
static const float kKbSpecialW[UIT_KBK_COUNT] = {1.5f, 1.5f, 3.0f, 1.4f, 1.3f, 1.3f};
#define KB_MSG_MS 3000

static uint32_t kb_upper(uint32_t c) {
    if (c >= 'a' && c <= 'z') return c - 32;
    if (c >= 0xE0 && c <= 0xFE && c != 0xF7) return c - 0x20;
    if (c == 0xFF) return 0x178;
    if (c == 0x131) return 'I';
    if (((c >= 0x100 && c <= 0x137) || (c >= 0x14A && c <= 0x177)) && (c & 1)) return c - 1;
    if (((c >= 0x139 && c <= 0x148) || (c >= 0x179 && c <= 0x17E)) && !(c & 1)) return c - 1;
    if (c >= 0x430 && c <= 0x44F) return c - 0x20;
    if (c >= 0x450 && c <= 0x45F) return c - 0x50;
    return c;
}

uint32_t uit_ov_kb_char(const UitOvKb *k, int row, int col) {
    if (row < 0 || row >= 4 || col < 0 || col >= UIT_KB_COLS) return 0;
    const char *t = kKbPages[k->page][row];
    uint32_t c = 0;
    for (int i = 0; i <= col && *t; i++) c = utf8_next(&t);
    if (k->shift && k->page != UIT_KB_PAGE_SYMBOLS) c = kb_upper(c);
    return c;
}

static int kb_cols(int row) { return row < 4 ? UIT_KB_COLS : UIT_KBK_COUNT; }
static float kb_center(int row, int col) {   // in character keys from the left
    if (row < 4) return (float)col + 0.5f;
    float x = 0;
    for (int i = 0; i < col; i++) x += kKbSpecialW[i];
    return x + kKbSpecialW[col] * 0.5f;
}
static int kb_nearest(int row, float x) {
    int best = 0;
    for (int c = 1; c < kb_cols(row); c++)
        if (fabsf(kb_center(row, c) - x) < fabsf(kb_center(row, best) - x)) best = c;
    return best;
}

static void kb_say(UitOv *o, const char *msg) {
    snprintf(o->kb.msg, sizeof o->kb.msg, "%s", msg);
    o->kb.msg_ms = o->now;
    o->dirty = 1;
}

static int kb_is_high(uint16_t u) { return u >= 0xD800 && u < 0xDC00; }
static int kb_is_low(uint16_t u) { return u >= 0xDC00 && u < 0xE000; }

// One character at the cursor. 0: not typed (kb.msg says why).
static int kb_type(UitOv *o, uint32_t cp) {
    UitOvKb *k = &o->kb;
    if (!uit_swkbd_allowed(&k->req, cp)) {
        kb_say(o, k->req.mode == UIT_KBMODE_NUMERIC ? "Only numbers here" : "That character can't be used here");
        return 0;
    }
    int w = cp >= 0x10000 ? 2 : 1;
    if (k->n + w > k->req.max_len) {
        char m[64];
        snprintf(m, sizeof m, "At most %d character%s", k->req.max_len, k->req.max_len == 1 ? "" : "s");
        kb_say(o, m);
        return 0;
    }
    memmove(k->t + k->cur + w, k->t + k->cur, (size_t)(k->n - k->cur) * 2);
    if (w == 2) {
        k->t[k->cur] = (uint16_t)(0xD800 + ((cp - 0x10000) >> 10));
        k->t[k->cur + 1] = (uint16_t)(0xDC00 + (cp & 0x3FF));
    } else k->t[k->cur] = (uint16_t)cp;
    k->n += w;
    k->cur += w;
    k->t[k->n] = 0;
    k->t0 = o->now;   // the caret shows at once
    o->dirty = 1;
    return 1;
}

static void kb_delete(UitOv *o, int before) {
    UitOvKb *k = &o->kb;
    int at = k->cur, w = 1;
    if (before) {
        if (at <= 0) return;
        at--;
        if (at > 0 && kb_is_low(k->t[at]) && kb_is_high(k->t[at - 1])) { at--; w = 2; }
    } else {
        if (at >= k->n) return;
        if (at + 1 < k->n && kb_is_high(k->t[at]) && kb_is_low(k->t[at + 1])) w = 2;
    }
    memmove(k->t + at, k->t + at + w, (size_t)(k->n - at - w) * 2);
    k->n -= w;
    k->cur = at;
    k->t[k->n] = 0;
    k->t0 = o->now;
    o->dirty = 1;
}

static void kb_move(UitOv *o, int dir) {
    UitOvKb *k = &o->kb;
    if (dir < 0 && k->cur > 0) {
        k->cur--;
        if (k->cur > 0 && kb_is_low(k->t[k->cur]) && kb_is_high(k->t[k->cur - 1])) k->cur--;
    } else if (dir > 0 && k->cur < k->n) {
        k->cur++;
        if (k->cur < k->n && kb_is_low(k->t[k->cur]) && kb_is_high(k->t[k->cur - 1])) k->cur++;
    }
    k->t0 = o->now;
    o->dirty = 1;
}

static void kb_finish(UitOv *o, int ok) {
    UitOvKb *k = &o->kb;
    if (ok) {
        int need = k->req.min_len > 0 ? k->req.min_len : 1;   // an empty answer is not one (the applet greys out OK)
        if (k->n < need) {
            char m[64];
            snprintf(m, sizeof m, need == 1 ? "Type something first (or Cancel)" : "At least %d characters", need);
            kb_say(o, m);
            return;
        }
    }
    k->done = 1;
    k->ok = ok;
    o->dirty = 1;
}

void uit_ov_kb_begin(UitOv *o, const UitSwkbdReq *r, uint64_t now) {
    UitOvKb *k = &o->kb;
    memset(k, 0, sizeof *k);
    k->active = 1;
    k->req = *r;
    k->n = r->initial_len < 0 ? 0 : r->initial_len > r->max_len ? r->max_len : r->initial_len;
    memcpy(k->t, r->initial, (size_t)k->n * 2);
    k->t[k->n] = 0;
    k->cur = r->cursor_start ? 0 : k->n;
    k->row = r->mode == UIT_KBMODE_NUMERIC ? 0 : 1;   // the digits, else the first letter row
    k->nav_prev = 0xFFFFFFFFu;   // what is held now does nothing until let go
    k->t0 = now;
    o->now = now;
    o->drag = 0;
    o->cap = 0;
    if (o->cstate != OVC_IDLE) o->cstate = OVC_DISARMED;   // a combo being held does not open the menu over it
    o->dirty = 1;
}

void uit_ov_kb_text(UitOv *o, const char *s) {
    UitOvKb *k = &o->kb;
    if (!k->active || k->done || !s) return;
    uint16_t u[UIT_SWKBD_LIMIT * 2];
    size_t n = uit_swkbd_utf8_to_utf16(s, strlen(s), u, sizeof u / 2);
    char keep[96] = "";
    int typed = 0, refused = 0;
    for (size_t i = 0; i < n; i++) {
        uint32_t cp = u[i];
        if (kb_is_high(u[i]) && i + 1 < n && kb_is_low(u[i + 1])) { cp = 0x10000 + ((cp - 0xD800) << 10) + (u[i + 1] - 0xDC00u); i++; }
        if (cp == '\r') continue;
        if (cp == '\t') cp = ' ';
        if (kb_type(o, cp)) typed++;
        else {
            if (!refused) snprintf(keep, sizeof keep, "%s", k->msg);
            refused++;
            if (k->n >= k->req.max_len) break;
        }
    }
    if (refused) kb_say(o, keep);
    else if (typed) k->msg[0] = 0;
    k->pre[0] = 0;
}

void uit_ov_kb_compose(UitOv *o, const char *s) {
    if (!o->kb.active) return;
    snprintf(o->kb.pre, sizeof o->kb.pre, "%s", s ? s : "");
    o->dirty = 1;
}

void uit_ov_kb_edit(UitOv *o, int e) {
    UitOvKb *k = &o->kb;
    if (!k->active || k->done) return;
    switch (e) {
    case UIT_KBE_BACKSPACE: kb_delete(o, 1); break;
    case UIT_KBE_DELETE: kb_delete(o, 0); break;
    case UIT_KBE_LEFT: kb_move(o, -1); break;
    case UIT_KBE_RIGHT: kb_move(o, +1); break;
    case UIT_KBE_HOME: k->cur = 0; k->t0 = o->now; o->dirty = 1; break;
    case UIT_KBE_END: k->cur = k->n; k->t0 = o->now; o->dirty = 1; break;
    case UIT_KBE_OK: kb_finish(o, 1); break;
    case UIT_KBE_CANCEL: kb_finish(o, 0); break;
    case UIT_KBE_NEWLINE: if (k->req.newline) kb_type(o, '\n'); else kb_finish(o, 1); break;
    }
}

void uit_ov_kb_key(UitOv *o, int row, int col) {
    UitOvKb *k = &o->kb;
    if (!k->active || k->done || row < 0 || row >= UIT_KB_ROWS || col < 0 || col >= kb_cols(row)) return;
    k->row = row;
    k->col = col;
    o->dirty = 1;
    if (row < 4) {
        uint32_t c = uit_ov_kb_char(k, row, col);
        if (c && kb_type(o, c)) {
            k->msg[0] = 0;
            if (k->shift == 1 && k->page != UIT_KB_PAGE_SYMBOLS) k->shift = 0;   // shift once: one capital
        }
        return;
    }
    switch (col) {
    case UIT_KBK_SHIFT: k->shift = (k->shift + 1) % 3; break;
    case UIT_KBK_PAGE: k->page = (k->page + 1) % UIT_KB_PAGES; break;
    case UIT_KBK_SPACE: kb_type(o, ' '); break;
    case UIT_KBK_BACK: kb_delete(o, 1); break;
    case UIT_KBK_CANCEL: kb_finish(o, 0); break;
    case UIT_KBK_OK: kb_finish(o, 1); break;
    }
}

int uit_ov_kb_result(const UitOv *o, uint16_t *out) {
    int n = o->kb.done && o->kb.ok ? o->kb.n : 0;
    memcpy(out, o->kb.t, (size_t)n * 2);
    out[n] = 0;
    return n;
}

void uit_ov_kb_end(UitOv *o, uint32_t phys_held) {
    if (!o->kb.active) return;
    o->kb.active = 0;
    o->kb.done = 0;
    o->kb.pre[0] = 0;
    o->suppress |= phys_held;   // the A / + that pressed OK does not reach the game
    o->nav_prev = 0xFFFFFFFFu;  // nor navigate the menu, if it is open underneath
    o->dirty = 1;
}

static void kb_focus(UitOv *o, int drow, int dcol) {
    UitOvKb *k = &o->kb;
    if (drow) {
        float x = kb_center(k->row, k->col);
        k->row = (k->row + drow + UIT_KB_ROWS) % UIT_KB_ROWS;
        k->col = kb_nearest(k->row, x);
    }
    if (dcol) k->col = (k->col + dcol + kb_cols(k->row)) % kb_cols(k->row);
    o->dirty = 1;
}

static uint32_t kb_pad(UitOv *o, uint32_t nav, uint64_t now) {
    UitOvKb *k = &o->kb;
    if (k->done) { k->nav_prev = nav; return 0xFFFFFFFFu; }
    // new presses act at once; a held direction (or B: backspace) repeats after 380 ms, every 70 ms
    uint32_t down = nav & ~k->nav_prev, rep = nav & (OVNAV_UP | OVNAV_DOWN | OVNAV_LEFT | OVNAV_RIGHT | OVNAV_BACK);
    if (k->nav_prev == 0xFFFFFFFFu) {   // the first look: what is held does nothing, nor repeats, until let go
        k->nav_prev = nav;
        k->nav_rep = rep ? -1 : 0;
        return 0xFFFFFFFFu;
    }
    if (down) { k->nav_t = now; k->nav_rep = 0; }
    else if (rep && k->nav_rep >= 0 && rep == (k->nav_prev & rep) && now - k->nav_t >= (k->nav_rep ? 70u : 380u)) {
        down = rep;
        k->nav_t = now;
        k->nav_rep++;
    }
    if (!rep) k->nav_rep = 0;
    k->nav_prev = nav;
    if (down & OVNAV_UP) kb_focus(o, -1, 0);
    if (down & OVNAV_DOWN) kb_focus(o, +1, 0);
    if (down & OVNAV_LEFT) kb_focus(o, 0, -1);
    if (down & OVNAV_RIGHT) kb_focus(o, 0, +1);
    if (down & OVNAV_TABL) kb_move(o, -1);
    if (down & OVNAV_TABR) kb_move(o, +1);
    if (down & OVNAV_CONFIRM) uit_ov_kb_key(o, k->row, k->col);
    if (down & OVNAV_BACK) kb_delete(o, 1);
    if (down & OVNAV_RESET) kb_type(o, ' ');
    if (down & OVNAV_Y) { k->page = (k->page + 1) % UIT_KB_PAGES; o->dirty = 1; }
    if (down & OVNAV_L3) { k->shift = k->shift ? 0 : 1; o->dirty = 1; }
    if (down & OVNAV_PLUS) kb_finish(o, 1);
    else if (down & OVNAV_MINUS) kb_finish(o, 0);
    return 0xFFFFFFFFu;
}

static void kb_mouse(UitOv *o, float x, float y, int button) {
    UitOvKb *k = &o->kb;
    if (k->done) return;
    for (int r = 0; r < UIT_KB_ROWS; r++)
        for (int c = 0; c < kb_cols(r); c++) {
            if (!in_box(&k->key[r][c], x, y)) continue;
            if (button == 1) uit_ov_kb_key(o, r, c);
            else if (button == 0 && (k->row != r || k->col != c)) { k->row = r; k->col = c; o->dirty = 1; }
            return;
        }
    if (button == 1 && in_box(&k->field, x, y)) uit_ov_kb_edit(o, UIT_KBE_END);
}

static void bar_set(UitOv *o, int id, float x) {
    const UitOvSetting *s = S(id);
    const UitOvBox *b = &o->bar[id];
    if (why(o, id)) return;
    double t = (x - b->x0) / (b->x1 - b->x0);
    if (t < 0) t = 0;
    if (t > 1) t = 1;
    double v = s->min + floor(t * (s->max - s->min) / s->step + 0.5) * s->step;
    if (fabs(v - get(o, id)) > 1e-9) set(o, id, v);
}

void uit_ov_mouse(UitOv *o, float x, float y, int button, float wheel) {
    if (o->kb.active) { o->mx = x; o->my = y; kb_mouse(o, x, y, button); return; }   // [UIT 0177]
    if (!o->open) return;
    o->mx = x;
    o->my = y;
    rows_for_tab(o);
    if (o->cap) {   // a click anywhere ends the capture (as the launcher's dialog)
        if (button == 1) uit_ov_capture_cancel(o, "Cancelled");
        return;
    }
    if (button == 2) { o->drag = 0; return; }
    if (o->ask) {   // [UIT 0292] pointing at a button focuses it, a click presses it; a click outside the question cancels
        if (wheel != 0) return;
        for (int b = 0; b < 2; b++) {
            if (!in_box(&o->ask_btn[b], x, y)) continue;
            if (o->ask_sel != b) { o->ask_sel = b; o->dirty = 1; }
            if (button == 1) ask_end(o, b == 1);
            return;
        }
        if (button == 1 && o->ask_panel.x1 > o->ask_panel.x0 && !in_box(&o->ask_panel, x, y)) ask_end(o, 0);
        return;
    }
    if (o->tab == UIT_OV_TAB_CONTROLS) {
        int *sel = &o->sel[UIT_OV_TAB_CONTROLS];
        if (wheel != 0) {
            *sel += wheel > 0 ? -1 : 1;
            if (*sel < 0) *sel = 0;
            if (*sel >= UIT_OV_BIND_ROWS) *sel = UIT_OV_BIND_ROWS - 1;
            o->dirty = 1;
            return;
        }
        for (int r = 0; r < UIT_OV_BIND_ROWS; r++) {
            if (!in_box(&o->brow[r], x, y)) continue;
            if (*sel != r) { *sel = r; o->dirty = 1; }
            for (int c = 0; c < 2; c++) {
                if (!in_box(&o->cell[r][c], x, y)) continue;
                if (o->bcol != c) { o->bcol = c; o->dirty = 1; }
                if (button != 1) return;
                if (r == UIT_BIND_BUTTONS) uit_ov_bind_reset(o, c, -1);
                else if (in_box(&o->cellreset[r][c], x, y)) uit_ov_bind_reset(o, c, r);
                else uit_ov_capture_start(o, c, r);
                return;
            }
            return;
        }
    }
    if (o->tab == UIT_OV_TAB_AMIIBO) {
        int *sel = &o->sel[UIT_OV_TAB_AMIIBO];
        if (wheel != 0) {
            *sel += wheel > 0 ? -1 : 1;
            if (*sel < 0) *sel = 0;
            if (*sel >= UIT_OV_AM_ROWS(o)) *sel = UIT_OV_AM_ROWS(o) - 1;
            o->dirty = 1;
            return;
        }
        for (int b = 0; b < o->am_nbox; b++) {
            if (!in_box(&o->am_box[b], x, y)) continue;
            if (*sel != o->am_boxrow[b]) { *sel = o->am_boxrow[b]; o->dirty = 1; }
            if (button == 1) amiibo_do(o, *sel);
            return;
        }
    }
    if (o->drag && button == 0) { bar_set(o, o->drag - 1, x); return; }
    if (wheel != 0) {
        int *sel = &o->sel[o->tab];
        *sel += wheel > 0 ? -1 : 1;
        if (*sel < 0) *sel = 0;
        if (*sel >= o->nrows) *sel = o->nrows ? o->nrows - 1 : 0;
        o->dirty = 1;
        return;
    }
    for (int r = 0; r < o->nrows; r++) {
        int id = o->rows[r];
        if (!in_box(&o->rowbox[id], x, y)) continue;
        if (o->sel[o->tab] != r) { o->sel[o->tab] = r; o->dirty = 1; }
        if (button != 1) return;
        const UitOvSetting *s = S(id);
        if ((s->kind == UIT_OVS_INT || s->kind == UIT_OVS_FLOAT) && in_box(&o->bar[id], x, y)) {
            if (!why(o, id)) { o->drag = id + 1; bar_set(o, id, x); }
            else step(o, id, 1, 0);   // shows why
        } else if (in_box(&o->dec[id], x, y)) step(o, id, -1, 0);
        else if (in_box(&o->inc[id], x, y)) step(o, id, +1, 0);
        else if (s->kind == UIT_OVS_BOOL || s->kind == UIT_OVS_CHOICE || s->kind == UIT_OVS_ACTION) step(o, id, 0, 0);
        return;
    }
    if (button != 1) return;
    for (int t = 0; t < UIT_OV_TABS; t++)
        if (in_box(&o->tabbox[t], x, y)) { set_tab(o, t); return; }
    if (in_box(&o->closebox, x, y)) uit_ov_set_open(o, 0, 0);
}

uint32_t uit_ov_shot_pad(UitOv *o, uint32_t phys) {
    uint32_t c = o->shot_combo, prev = o->shot_prev;
    o->shot_prev = phys;
    if (!c) return o->suppress;
    int full = (phys & c) == c;
    if (full && !o->shot_down && !o->open && !o->kb.active && !o->cap) {
        o->shot_fire = 1;
        uint32_t fresh = phys & ~prev & c;   // the button that completed it (the others already reached the game)
        o->suppress |= fresh ? fresh : c;
    }
    o->shot_down = full;
    return o->suppress;
}

uint32_t uit_ov_pad(UitOv *o, uint32_t phys, uint32_t nav, uint64_t now) {
    o->now = now;
    o->phys_now = phys;
    o->suppress &= phys;   // a hidden button shows again once it has been let go
    if (o->shot_combo) uit_ov_shot_pad(o, phys);
    if (o->kb.active) return kb_pad(o, nav, now);   // [UIT 0177] the keyboard dialog: no combo, the game sees nothing
    uint32_t all = o->combo | o->combo2;
    if (all) {
        // [UIT 0271] two combos (combo2, e.g. Guide alone): either one, held with nothing else, starts the hold
        uint32_t c = phys & o->ccur, others = phys & ~o->ccur;
        switch (o->cstate) {
        case OVC_IDLE: {
            int part = 0, dis = 0;
            for (int k = 0; k < 2; k++) {
                uint32_t cb = k ? o->combo2 : o->combo, ck = phys & cb;
                if (!cb || !ck) continue;
                if (ck == cb && !(phys & ~cb)) { o->ccur = cb; o->cstate = OVC_HOLD; o->ct0 = now; o->dirty = 1; break; }
                part = 1;
                if (phys & ~cb) dis++;
            }
            // part of a combo during play, with other buttons, and no combo can still start: not the start of one
            if (o->cstate == OVC_IDLE && part && dis == (o->combo && (phys & o->combo)) + (o->combo2 && (phys & o->combo2)))
                o->cstate = OVC_DISARMED;
            break;
        }
        case OVC_HOLD:
            if (others) {                   // another button: cancelled (and the combo stays hidden until let go)
                o->suppress |= c;
                o->cstate = OVC_DISARMED;
                o->dirty = 1;
            } else if (c != o->ccur) {
                o->suppress |= c;
                o->cstate = OVC_IDLE;
                o->dirty = 1;
            } else if (now - o->ct0 >= o->hold_ms) {
                uit_ov_set_open(o, !o->open, 0);
                o->cstate = OVC_LATCHED;
                o->nav_prev = nav;          // what is held now does not navigate
            }
            break;
        case OVC_DISARMED:
            if (!(phys & all)) o->cstate = OVC_IDLE;
            break;
        case OVC_LATCHED:
            if (!c) o->cstate = OVC_IDLE;
            break;
        }
    }
    uint32_t mask = o->suppress;
    if (o->cstate == OVC_HOLD || o->cstate == OVC_LATCHED) mask |= o->ccur;
    if (!o->open) {
        if (o->cstate == OVC_LATCHED) o->suppress |= phys & o->ccur;   // closed by the combo: still hidden until let go
        o->nav_prev = nav;
        return mask;
    }
    if (o->cap && now - o->cap_t0 >= UIT_OV_BIND_MS) uit_ov_capture_cancel(o, "No press in time");
    if (o->cap) {   // capturing: nothing navigates, and the game sees a controller at rest
        uint32_t down = nav & ~o->nav_prev;
        o->nav_prev = nav;
        if (o->cap_kind == UIT_BIND_PAD) {
            o->cap_held &= phys;                   // let go: may bind when pressed again
            uint32_t fresh = phys & ~o->cap_held;
            o->cap_held |= fresh;                  // each press counts once
            for (int bit = 0; bit < 32 && o->cap; bit++)
                if (fresh >> bit & 1) capture_pad(o, bit);
        } else {                                   // waiting for a key: B cancels, X clears (launcher: east / west)
            if (down & OVNAV_BACK) uit_ov_capture_cancel(o, "Cancelled");
            else if (down & OVNAV_RESET) uit_ov_capture_clear(o);
        }
        return 0xFFFFFFFFu;
    }
    if (o->cstate == OVC_LATCHED || o->cstate == OVC_HOLD) { o->nav_prev = nav; return 0xFFFFFFFFu; }
    // navigation: new presses act at once; a held direction repeats after 380 ms, every 90 ms
    uint32_t down = nav & ~o->nav_prev, dirs = nav & (OVNAV_UP | OVNAV_DOWN | OVNAV_LEFT | OVNAV_RIGHT);
    if (down) { o->nav_t = now; o->nav_rep = 0; }
    else if (dirs && dirs == (o->nav_prev & dirs) && now - o->nav_t >= (o->nav_rep ? 90u : 380u)) {
        down = dirs;
        o->nav_t = now;
        o->nav_rep++;
    }
    if (!dirs) o->nav_rep = 0;
    o->nav_prev = nav;
    if (down & OVNAV_UP) uit_ov_input(o, OVIN_UP);
    if (down & OVNAV_DOWN) uit_ov_input(o, OVIN_DOWN);
    if (down & OVNAV_LEFT) uit_ov_input(o, OVIN_LEFT);
    if (down & OVNAV_RIGHT) uit_ov_input(o, OVIN_RIGHT);
    if (down & OVNAV_TABL) uit_ov_input(o, OVIN_TAB_PREV);
    if (down & OVNAV_TABR) uit_ov_input(o, OVIN_TAB_NEXT);
    if (down & OVNAV_CONFIRM) uit_ov_input(o, OVIN_CONFIRM);
    if (down & OVNAV_RESET) uit_ov_input(o, OVIN_RESET);
    if (down & OVNAV_BACK) {
        if (o->ask) uit_ov_input(o, OVIN_BACK);   // [UIT 0292] back cancels the question, the overlay stays
        else uit_ov_set_open(o, 0, phys);
    }
    return 0xFFFFFFFFu;
}

float uit_ov_combo_progress(const UitOv *o, uint64_t now) {
    if (o->cstate != OVC_HOLD || now - o->ct0 < 150) return 0;
    float p = (float)(now - o->ct0) / (float)o->hold_ms;
    return p > 1 ? 1 : p;
}

// ======================================================================== drawing
// launcher/ui.h ut::pal
#define RGBA(r, g, b, a) ((uint32_t)(r) | (uint32_t)(g) << 8 | (uint32_t)(b) << 16 | (uint32_t)(a) << 24)
static const uint32_t P_PANEL = RGBA(20, 27, 30, 255), P_RAISED = RGBA(27, 37, 40, 255), P_FOCUS = RGBA(31, 44, 47, 255),
                      P_LINE = RGBA(40, 53, 55, 255), P_BG = RGBA(14, 19, 21, 255);
static const uint32_t C_TEXT = RGBA(232, 228, 217, 255), C_BODY = RGBA(196, 200, 192, 255), C_DIM = RGBA(143, 155, 150, 255),
                      C_FAINT = RGBA(98, 112, 108, 255), C_TEAL = RGBA(111, 183, 168, 255), C_TEAL_SOFT = RGBA(111, 183, 168, 46),
                      C_GOLD = RGBA(205, 174, 106, 255), C_GOLD_SOFT = RGBA(205, 174, 106, 40), C_INK = RGBA(20, 22, 20, 255),
                      C_SCRIM = RGBA(6, 9, 10, 184);

typedef struct { UitOvQuad *q; int n, cap; const UitOvAtlas *a; float s; } Dl;

static uint32_t fade(uint32_t c, float k) {
    uint32_t a = (uint32_t)((float)(c >> 24) * k + 0.5f);
    return (c & 0xFFFFFFu) | a << 24;
}

static void quad(Dl *d, float x0, float y0, float x1, float y1, uint32_t c, float r, int kind, float stroke) {
    if (d->n >= d->cap) return;
    d->q[d->n++] = (UitOvQuad){x0, y0, x1, y1, 0, 0, 0, 0, c, r, (float)kind, stroke};
}
static void rect(Dl *d, float x0, float y0, float x1, float y1, uint32_t c, float r) { quad(d, x0, y0, x1, y1, c, r, UIT_OVQ_RECT, 0); }
static void outline(Dl *d, float x0, float y0, float x1, float y1, uint32_t c, float r, float w) { quad(d, x0, y0, x1, y1, c, r, UIT_OVQ_OUTLINE, w); }

// Text with its top at y (whole pixels: the atlas is made at this size, so each texel lands on one pixel). maxw > 0:
// cut with an ellipsis to fit. Returns the width drawn.
static float text(Dl *d, int f, float x, float y, uint32_t c, const char *t, float maxw) {
    const UitOvAtlas *a = d->a;
    float pen = floorf(x + 0.5f), base = floorf(y + 0.5f) + a->ascent[f], start = pen;
    float ell = a->g[f][glyph_index(0x2026)].adv;
    int cut = maxw > 0 && uit_ov_text_width(a, f, t) > maxw;
    while (t && *t) {
        const UitOvGlyph *g = &a->g[f][glyph_index(utf8_next(&t))];
        if (cut && pen + g->adv + ell - start > maxw) { g = &a->g[f][glyph_index(0x2026)]; t = ""; }
        if (g->x1 > g->x0 && d->n < d->cap)
            d->q[d->n++] = (UitOvQuad){pen + g->x0, base + g->y0, pen + g->x1, base + g->y1, g->u0, g->v0, g->u1, g->v1, c, 0,
                                       UIT_OVQ_TEXT, 0};
        pen += g->adv;
    }
    return pen - start;
}
static float text_h(const Dl *d, int f) { return d->a->ascent[f] + d->a->descent[f]; }
static float text_r(Dl *d, int f, float right, float y, uint32_t c, const char *t) {   // right-aligned
    return text(d, f, right - uit_ov_text_width(d->a, f, t), y, c, t, 0);
}
static float text_c(Dl *d, int f, float cx, float y, uint32_t c, const char *t, float maxw) {   // centred
    float w = uit_ov_text_width(d->a, f, t);
    if (maxw > 0 && w > maxw) w = maxw;
    return text(d, f, cx - w * 0.5f, y, c, t, maxw);
}

float uit_ov_scale(float w, float h) {
    float s = h / 900.0f, sw = w / 1500.0f;
    if (sw < s) s = sw;
    if (s < 0.6f) s = 0.6f;
    if (s > 4.0f) s = 4.0f;
    return roundf(s * 8.0f) / 8.0f;   // steps of 1/8: a window being resized does not rebuild the atlas every pixel
}

static void num_text(const UitOvSetting *s, double v, char *buf, size_t n) {
    if (s->autolabel && v < s->min) { snprintf(buf, n, "%s", s->autolabel); return; }
    if (s->zero && fabs(v) < 1e-9) { snprintf(buf, n, "%s", s->zero); return; }
    char num[32];
    if (s->kind == UIT_OVS_INT) snprintf(num, sizeof num, "%lld", (long long)llround(v));
    else {
        snprintf(num, sizeof num, "%.2f", v);
        size_t l = strlen(num);
        while (l && num[l - 1] == '0') num[--l] = 0;
        if (l && num[l - 1] == '.') num[--l] = 0;
    }
    if (!s->unit) snprintf(buf, n, "%s", num);
    else snprintf(buf, n, "%s%s%s", num, strcmp(s->unit, "%") ? " " : "", s->unit);
}

static const char *value_text(UitOv *o, int id, double v, char *buf, size_t n) {
    const char *t = o->h && o->h->text ? o->h->text(id, v, buf, n) : NULL;
    if (t) return t;
    const UitOvSetting *s = S(id);
    switch (s->kind) {
    case UIT_OVS_BOOL: snprintf(buf, n, "%s", v >= 0.5 ? "On" : "Off"); break;
    case UIT_OVS_CHOICE: if (!uit_ov_choice(s, (int)v, NULL, 0, buf, n)) snprintf(buf, n, "Other"); break;
    case UIT_OVS_INT: case UIT_OVS_FLOAT: num_text(s, v, buf, n); break;
    default: buf[0] = 0;
    }
    return buf;
}

// [UIT 0301] the frame-time graph: UIT_OV_GRAPH_COLS bars (each the slowest frame of its slice of the last 3 s), a line
// at 16.7 ms; bars over 16.7 ms gold, over 33.3 ms red. Scale 0..40 ms (taller frames are cut at the top).
#define UIT_OV_GRAPH_COLS 120
static void frame_graph(Dl *d, const UitOvFrame *f, float x, float y) {
    float s = d->s, w = 240 * s, h = 64 * s, pad = 6 * s, gx0 = x + pad, gx1 = x + w - pad, gy0 = y + pad, gy1 = y + h - pad;
    const float top_ms = 40;
    rect(d, x, y, x + w, y + h, fade(P_BG, 0.78f), 8 * s);
    float total = 0;
    for (int i = 0; i < f->ftn; i++) total += f->ft[i];
    if (f->ftn > 0 && total > 0) {
        float cw = (gx1 - gx0) / UIT_OV_GRAPH_COLS, acc = 0;
        int i = 0;
        for (int c = 0; c < UIT_OV_GRAPH_COLS && i < f->ftn; c++) {   // slices of equal time, newest on the right
            float end = total * (float)(c + 1) / UIT_OV_GRAPH_COLS, mx = 0;
            while (i < f->ftn && (acc < end || mx == 0)) { if (f->ft[i] > mx) mx = f->ft[i]; acc += f->ft[i++]; }
            if (mx <= 0) continue;
            float bh = (mx > top_ms ? top_ms : mx) / top_ms * (gy1 - gy0);
            if (bh < 1 * s) bh = 1 * s;
            uint32_t col = mx > 33.4f ? RGBA(214, 106, 92, 235) : mx > 16.9f ? C_GOLD : fade(C_TEAL, 0.9f);
            rect(d, gx0 + c * cw, gy1 - bh, gx0 + (c + 1) * cw, gy1, col, 0);
        }
    }
    float ly = gy1 - 16.7f / top_ms * (gy1 - gy0);
    rect(d, gx0, ly - 0.5f * s, gx1, ly + 0.5f * s, fade(C_TEXT, 0.55f), 0);
    text_r(d, UIT_OVF_SMALL, gx1, gy0, fade(C_DIM, 0.9f), "16.7 ms");
}

static void hud(UitOv *o, Dl *d, const UitOvFrame *f) {
    float s = d->s, hy = 16 * s;
    if (o->show_fps && !o->open) {
        char b[32];
        if (f->fps >= 0) snprintf(b, sizeof b, "%.0f FPS", f->fps);
        else snprintf(b, sizeof b, "-- FPS");
        float pad = 10 * s, w = uit_ov_text_width(d->a, UIT_OVF_SEMI, b), h = text_h(d, UIT_OVF_SEMI);
        float x = 16 * s, y = 16 * s;
        rect(d, x, y, x + w + pad * 2, y + h + pad, fade(P_BG, 0.78f), 8 * s);
        text(d, UIT_OVF_SEMI, x + pad, y + pad * 0.5f, C_TEXT, b, 0);
        hy = y + h + pad + 6 * s;
    }
    if (o->show_graph && !o->open) frame_graph(d, f, 16 * s, hy);
    if (f->bench && f->bench[0]) {   // [UIT 0302] top right: "Benchmarking 42 s", then the result for a few seconds
        float pad = 12 * s, h1 = text_h(d, UIT_OVF_SEMI), h2 = f->bench2 && f->bench2[0] ? text_h(d, UIT_OVF_SMALL) + 4 * s : 0;
        float w = uit_ov_text_width(d->a, UIT_OVF_SEMI, f->bench), w2 = h2 > 0 ? uit_ov_text_width(d->a, UIT_OVF_SMALL, f->bench2) : 0;
        if (w2 > w) w = w2;
        float x1 = f->w - 16 * s, x0 = x1 - w - pad * 2, y0 = 16 * s, y1 = y0 + h1 + h2 + pad;
        rect(d, x0, y0, x1, y1, fade(P_BG, 0.82f), 8 * s);
        outline(d, x0, y0, x1, y1, o->bench_on ? C_GOLD : P_LINE, 8 * s, 1);
        text(d, UIT_OVF_SEMI, x0 + pad, y0 + pad * 0.5f, o->bench_on ? C_GOLD : C_TEXT, f->bench, 0);
        if (h2 > 0) text(d, UIT_OVF_SMALL, x0 + pad, y0 + pad * 0.5f + h1 + 4 * s, C_BODY, f->bench2, 0);
    }
    float p = uit_ov_combo_progress(o, f->now_ms);
    if (p > 0) {   // the controller combo being held: a ring that fills, and what it does
        const char *msg = o->open ? "Keep holding to close the overlay" : "Keep holding to open the overlay";
        float tw = uit_ov_text_width(d->a, UIT_OVF_BODY, msg), r = 15 * s, pad = 12 * s;
        float w = r * 2 + 12 * s + tw + pad * 2, h = r * 2 + pad * 2, x = floorf((f->w - w) * 0.5f), y = floorf(64 * s);
        rect(d, x, y, x + w, y + h, fade(P_PANEL, 0.92f), h * 0.5f);
        outline(d, x, y, x + w, y + h, P_LINE, h * 0.5f, 1);
        float cx = x + pad + r, cy = y + pad + r;
        if (d->n < d->cap)
            d->q[d->n++] = (UitOvQuad){cx - r, cy - r, cx + r, cy + r, p, 0, p, 0, C_TEAL, r, UIT_OVQ_RING, 3.5f * s};
        text(d, UIT_OVF_BODY, cx + r + 12 * s, cy - text_h(d, UIT_OVF_BODY) * 0.5f, C_BODY, msg, 0);
    }
}

static void control(UitOv *o, Dl *d, int id, float x0, float x1, float cy, int on_row, int off) {
    const UitOvSetting *s = S(id);
    float sc = d->s, k = off ? 0.45f : 1;
    double v = get(o, id);
    char buf[96];
    const char *vt = value_text(o, id, v, buf, sizeof buf);
    float bh = text_h(d, UIT_OVF_BODY);
    o->dec[id] = o->inc[id] = o->bar[id] = (UitOvBox){0};
    switch (s->kind) {
    case UIT_OVS_BOOL: {
        float w = 46 * sc, h = 26 * sc, x = x1 - w, y = cy - h * 0.5f;
        int b = v >= 0.5;
        rect(d, x, y, x + w, y + h, fade(b ? C_TEAL : P_LINE, k), h * 0.5f);
        float kn = h - 6 * sc, kx = b ? x + w - 3 * sc - kn : x + 3 * sc;
        rect(d, kx, y + 3 * sc, kx + kn, y + 3 * sc + kn, fade(b ? C_INK : C_DIM, k), kn * 0.5f);
        text_r(d, UIT_OVF_BODY, x - 12 * sc, cy - bh * 0.5f, fade(on_row ? C_TEXT : C_BODY, k), vt);
        break;
    }
    case UIT_OVS_CHOICE: {
        float cw = 26 * sc;
        uint32_t cc = fade(on_row ? C_TEAL : C_FAINT, k);
        text_c(d, UIT_OVF_SEMI, x0 + cw * 0.5f, cy - bh * 0.5f, cc, "\xe2\x80\xb9", 0);
        text_c(d, UIT_OVF_SEMI, x1 - cw * 0.5f, cy - bh * 0.5f, cc, "\xe2\x80\xba", 0);
        text_c(d, UIT_OVF_BODY, (x0 + x1) * 0.5f, cy - bh * 0.5f, fade(on_row ? C_TEXT : C_BODY, k), vt, x1 - x0 - cw * 2);
        o->dec[id] = (UitOvBox){x0, cy - 20 * sc, x0 + cw, cy + 20 * sc};
        o->inc[id] = (UitOvBox){x1 - cw, cy - 20 * sc, x1, cy + 20 * sc};
        break;
    }
    case UIT_OVS_INT:
    case UIT_OVS_FLOAT: {
        float bw = 150 * sc, th = 6 * sc, bx = x0, by = cy - th * 0.5f;
        double t = s->autolabel && v < s->min ? 0 : (v - s->min) / (s->max - s->min);
        if (t < 0) t = 0;
        if (t > 1) t = 1;
        rect(d, bx, by, bx + bw, by + th, fade(P_LINE, k), th * 0.5f);
        rect(d, bx, by, bx + (float)(bw * t), by + th, fade(on_row ? C_TEAL : C_DIM, k), th * 0.5f);
        float kn = 16 * sc, kx = bx + (float)(bw * t) - kn * 0.5f;
        rect(d, kx, cy - kn * 0.5f, kx + kn, cy + kn * 0.5f, fade(on_row ? C_TEXT : C_BODY, k), kn * 0.5f);
        text_r(d, UIT_OVF_BODY, x1, cy - bh * 0.5f, fade(on_row ? C_TEXT : C_BODY, k), vt);
        o->bar[id] = (UitOvBox){bx - kn * 0.5f, cy - 16 * sc, bx + bw + kn * 0.5f, cy + 16 * sc};
        break;
    }
    case UIT_OVS_ACTION: {
        float w = 120 * sc, h = 32 * sc, x = x1 - w, y = cy - h * 0.5f;
        if (on_row) rect(d, x, y, x + w, y + h, C_TEAL_SOFT, 8 * sc);
        outline(d, x, y, x + w, y + h, on_row ? C_TEAL : P_LINE, 8 * sc, 1.5f * sc);
        text_c(d, UIT_OVF_SEMI, x + w * 0.5f, cy - bh * 0.5f, on_row ? C_TEXT : C_BODY,
               id == OV_SHOT ? "Take" : id == OV_CAPTURE ? "Capture" : id == OV_BENCH ? (o->bench_on ? "Stop" : "Start") :
               id == OV_RESTART ? "Restart\xe2\x80\xa6" : id == OV_QUIT ? "Quit\xe2\x80\xa6" : "Mark", 0);
        break;
    }
    default:
        break;
    }
}

// [UIT 0174] the Controls tab: a compact list (17 rows do not fit the panel's height, so it scrolls with the
// selection), the controller and the keyboard side by side
static void controls_draw(UitOv *o, Dl *d, float x0, float x1, float ytop, float ybot, float pad) {
    float s = d->s, rx0 = x0 + pad * 0.5f, rx1 = x1 - pad * 0.5f;
    float cw = 230 * s, gap = 10 * s, c1x0 = rx1 - 22 * s - cw, c0x0 = c1x0 - gap - cw, hh = 26 * s, rowh = 38 * s;
    float sh = text_h(d, UIT_OVF_SMALL), bh = text_h(d, UIT_OVF_BODY), nh = text_h(d, UIT_OVF_SEMI);
    static const char *const kFam[4] = {"Controller", "Controller (Xbox)", "Controller (PlayStation)", "Controller (Nintendo)"};
    int fam = o->pad_family >= 0 && o->pad_family < 4 ? o->pad_family : 0;
    float cx[2] = {c0x0, c1x0};
    for (int r = 0; r < UIT_OV_BIND_ROWS; r++) {
        o->brow[r] = (UitOvBox){0};
        o->cell[r][0] = o->cell[r][1] = (UitOvBox){0};
        if (r < UIT_BIND_BUTTONS) o->cellreset[r][0] = o->cellreset[r][1] = (UitOvBox){0};
    }
    text(d, UIT_OVF_SMALL, rx0 + pad * 0.5f + 8 * s, ytop + (hh - sh) * 0.5f, C_FAINT, "SWITCH BUTTON", 0);
    text(d, UIT_OVF_SMALL, c0x0 + 10 * s, ytop + (hh - sh) * 0.5f, o->bcol == UIT_BIND_PAD ? C_TEAL : C_FAINT,
         o->pad_present ? kFam[fam] : "Controller (none connected)", cw - 12 * s);
    text(d, UIT_OVF_SMALL, c1x0 + 10 * s, ytop + (hh - sh) * 0.5f, o->bcol == UIT_BIND_KEY ? C_TEAL : C_FAINT, "Keyboard", 0);
    if (!o->have_binds) {
        text(d, UIT_OVF_BODY, rx0 + pad * 0.5f + 8 * s, ytop + hh + 12 * s, C_GOLD, "The bindings were not loaded: change them in the launcher.", 0);
        return;
    }
    float ly0 = ytop + hh;
    int vis = (int)((ybot - ly0) / rowh), *sel = &o->sel[UIT_OV_TAB_CONTROLS];
    if (vis < 1) vis = 1;
    if (vis > UIT_OV_BIND_ROWS) vis = UIT_OV_BIND_ROWS;
    if (*sel < o->bscroll) o->bscroll = *sel;
    if (*sel >= o->bscroll + vis) o->bscroll = *sel - vis + 1;
    if (o->bscroll > UIT_OV_BIND_ROWS - vis) o->bscroll = UIT_OV_BIND_ROWS - vis;
    if (o->bscroll < 0) o->bscroll = 0;
    for (int v = 0; v < vis; v++) {
        int r = o->bscroll + v, on = r == *sel, all = r == UIT_BIND_BUTTONS;
        float ry = ly0 + rowh * (float)v, cy = ry + rowh * 0.5f;
        o->brow[r] = (UitOvBox){rx0, ry, rx1, ry + rowh};
        if (on) {
            rect(d, rx0, ry + 1 * s, rx1, ry + rowh - 1 * s, P_FOCUS, 8 * s);
            rect(d, rx0, ry + 9 * s, rx0 + 3 * s, ry + rowh - 9 * s, C_TEAL, 1.5f * s);
        }
        float lx = rx0 + pad * 0.5f + 8 * s, maxw = c0x0 - 14 * s - lx;
        const char *name = all ? "All buttons" : uit_bind_button_name(r);
        float w = text(d, UIT_OVF_SEMI, lx, cy - nh * 0.5f, on ? C_TEXT : C_BODY, name, maxw);
        const char *does = all ? "every binding back to the launcher's default" : uit_bind_button_does(r);
        if (maxw - w - 12 * s > 40 * s)
            text(d, UIT_OVF_SMALL, lx + w + 12 * s, cy - sh * 0.5f + 1 * s, on ? C_DIM : C_FAINT, does, maxw - w - 12 * s);
        for (int c = 0; c < 2; c++) {
            float a0 = cx[c], a1 = a0 + cw, ch = 30 * s, y0 = cy - ch * 0.5f, y1 = y0 + ch;
            int focus = on && o->bcol == c, hov = in_box(&o->cell[r][c], o->mx, o->my) || (o->mx >= a0 && o->mx < a1 && o->my >= y0 && o->my < y1);
            int capt = o->cap && !all && o->cap_btn == r && o->cap_kind == c;
            int nodev = c == UIT_BIND_PAD && !o->pad_present;
            o->cell[r][c] = (UitOvBox){a0, y0, a1, y1};
            if (all) {   // the resets, as buttons
                const char *t = c == UIT_BIND_PAD ? "Reset controller" : "Reset keyboard";
                if (focus || hov) rect(d, a0, y0, a1, y1, C_TEAL_SOFT, 8 * s);
                outline(d, a0, y0, a1, y1, focus ? C_TEAL : P_LINE, 8 * s, 1.5f * s);
                text_c(d, UIT_OVF_SEMI, (a0 + a1) * 0.5f, cy - nh * 0.5f, focus || hov ? C_TEXT : C_BODY, t, cw - 16 * s);
                continue;
            }
            rect(d, a0, y0, a1, y1, capt ? C_GOLD_SOFT : focus ? P_RAISED : fade(P_RAISED, 0.6f), 8 * s);
            if (capt || focus) outline(d, a0, y0, a1, y1, capt ? C_GOLD : C_TEAL, 8 * s, 1.5f * s);
            if (capt) {   // "press the button you want", and the seconds left
                uint64_t el = o->now - o->cap_t0;
                int left = el >= UIT_OV_BIND_MS ? 0 : (int)((UIT_OV_BIND_MS - el + 999) / 1000);
                char t[48];
                snprintf(t, sizeof t, c == UIT_BIND_PAD ? "Press a button\xe2\x80\xa6  %d" : "Press a key\xe2\x80\xa6  %d", left);
                text_c(d, UIT_OVF_SEMI, (a0 + a1) * 0.5f, cy - nh * 0.5f, C_GOLD, t, cw - 16 * s);
                continue;
            }
            char t[128];
            uit_binds_text(&o->binds, c, r, fam, t, sizeof t);
            int custom = !uit_bind_is_default(&o->binds, c, r);
            float tx0 = a0 + 12 * s, tx1 = a1 - 10 * s;
            if (custom) {   // changed from the default: a gold dot, and "Reset" for the mouse
                float dr = 3 * s;
                rect(d, a0 + 8 * s, cy - dr, a0 + 8 * s + dr * 2, cy + dr, C_GOLD, dr);
                tx0 = a0 + 20 * s;
                if (hov || focus) {
                    float rw = uit_ov_text_width(d->a, UIT_OVF_SMALL, "Reset") + 14 * s, b0 = a1 - 4 * s - rw;
                    int rh = in_box(&o->cellreset[r][c], o->mx, o->my);
                    rect(d, b0, y0 + 4 * s, a1 - 4 * s, y1 - 4 * s, rh ? C_GOLD_SOFT : P_LINE, 6 * s);
                    text_c(d, UIT_OVF_SMALL, b0 + rw * 0.5f, cy - sh * 0.5f, rh ? C_GOLD : C_BODY, "Reset", 0);
                    o->cellreset[r][c] = (UitOvBox){b0, y0, a1, y1};
                    tx1 = b0 - 6 * s;
                }
            }
            uint32_t col = nodev ? C_FAINT : !o->binds.b[c][r].n ? C_DIM : focus ? C_TEXT : C_BODY;
            text(d, UIT_OVF_BODY, tx0, cy - bh * 0.5f, col, t, tx1 - tx0);
        }
    }
    if (vis < UIT_OV_BIND_ROWS) {   // the scroll bar
        float tx = rx1 - 8 * s, th = rowh * (float)vis, t0 = ly0, k = th / (float)UIT_OV_BIND_ROWS;
        rect(d, tx, t0, tx + 4 * s, t0 + th, P_LINE, 2 * s);
        rect(d, tx, t0 + k * (float)o->bscroll, tx + 4 * s, t0 + k * (float)(o->bscroll + vis), C_DIM, 2 * s);
    }
}

// [UIT 0175] the Amiibo tab: what the reader is doing, then the rows (scrolling with the selection)
static void amiibo_draw(UitOv *o, Dl *d, float x0, float x1, float ytop, float ybot, float pad) {
    float s = d->s, rx0 = x0 + pad * 0.5f, rx1 = x1 - pad * 0.5f, lx = rx0 + pad * 0.5f + 8 * s, rowh = 46 * s;
    float sh = text_h(d, UIT_OVF_SMALL), nh = text_h(d, UIT_OVF_SEMI);
    o->am_nbox = 0;
    if (!o->h || !o->h->amiibo_scan) {
        text(d, UIT_OVF_BODY, lx, ytop + 12 * s, C_GOLD, "Amiibo scanning is not available in this build.", rx1 - lx - 16 * s);
        return;
    }
    // the reader: does the game ask for an amiibo now, what was scanned and what the game made of it
    char line[200];
    int bad = 0;
    const char *lt = o->h->amiibo_line ? o->h->amiibo_line(line, sizeof line, &bad) : NULL;
    float ch = nh + sh + 22 * s;
    rect(d, rx0, ytop + 2 * s, rx1, ytop + ch, bad ? C_GOLD_SOFT : P_RAISED, 10 * s);
    text(d, UIT_OVF_SEMI, lx, ytop + 9 * s, bad ? C_GOLD : C_TEXT, lt && *lt ? lt : "Amiibo reader", rx1 - lx - 16 * s);
    text(d, UIT_OVF_SMALL, lx, ytop + 9 * s + nh + 5 * s, C_DIM,
         "In the game: hold L, choose amiibo, let go of L and aim. Then scan here (a file waits 20 s for the game).",
         rx1 - lx - 16 * s);
    float ly0 = ytop + ch + 8 * s;
    int rows = UIT_OV_AM_ROWS(o), vis = (int)((ybot - ly0) / rowh), *sel = &o->sel[UIT_OV_TAB_AMIIBO];
    if (vis < 1) vis = 1;
    if (vis > UIT_OV_AM_VIS) vis = UIT_OV_AM_VIS;
    if (vis > rows) vis = rows;
    if (*sel < o->am_scroll) o->am_scroll = *sel;
    if (*sel >= o->am_scroll + vis) o->am_scroll = *sel - vis + 1;
    if (o->am_scroll > rows - vis) o->am_scroll = rows - vis;
    if (o->am_scroll < 0) o->am_scroll = 0;
    const char *last = o->h->amiibo_last ? o->h->amiibo_last() : NULL;
    const char *dir = o->h->amiibo_dir ? o->h->amiibo_dir() : NULL;
    for (int v = 0; v < vis; v++) {
        int r = o->am_scroll + v, on = r == *sel;
        float ry = ly0 + rowh * (float)v, cy = ry + rowh * 0.5f;
        int hov = o->mx >= rx0 && o->mx < rx1 && o->my >= ry && o->my < ry + rowh;
        o->am_box[o->am_nbox] = (UitOvBox){rx0, ry, rx1, ry + rowh};
        o->am_boxrow[o->am_nbox++] = r;
        if (on || hov) rect(d, rx0, ry + 1 * s, rx1, ry + rowh - 1 * s, on ? P_FOCUS : fade(P_FOCUS, 0.6f), 8 * s);
        if (on) rect(d, rx0, ry + 11 * s, rx0 + 3 * s, ry + rowh - 11 * s, C_TEAL, 1.5f * s);
        char nm[200];
        const char *name, *note = NULL;
        uint32_t col = on ? C_TEXT : C_BODY;
        if (r == 0) {
            name = "Choose a file\xe2\x80\xa6";
            note = "any amiibo .bin (540, 572 or 532 bytes), .json or .nfc on this PC";
            col = on ? C_TEAL : C_TEXT;
        } else if (r == 1) {
            if (last) snprintf(nm, sizeof nm, "Scan again: %s", last);
            else snprintf(nm, sizeof nm, "Scan again");
            name = nm;
            if (!last) { note = "nothing scanned yet"; col = C_DIM; }
        } else {
            const char *fn = o->h->amiibo_file ? o->h->amiibo_file(r - 2) : NULL;
            name = fn ? fn : "?";
        }
        float maxw = rx1 - lx - 120 * s;
        float w = text(d, r < 2 ? UIT_OVF_SEMI : UIT_OVF_BODY, lx, cy - nh * 0.5f, col, name, maxw);
        if (note && maxw - w - 14 * s > 60 * s)
            text(d, UIT_OVF_SMALL, lx + w + 14 * s, cy - sh * 0.5f + 1 * s, on ? C_DIM : C_FAINT, note, maxw - w - 14 * s);
        if (on && (r != 1 || last)) {   // what Enter / A does on this row
            const char *t = r == 0 ? "Browse" : "Scan";
            float bw = 96 * s, bh = 30 * s, bx = rx1 - 16 * s - bw;
            rect(d, bx, cy - bh * 0.5f, bx + bw, cy + bh * 0.5f, C_TEAL_SOFT, 8 * s);
            outline(d, bx, cy - bh * 0.5f, bx + bw, cy + bh * 0.5f, C_TEAL, 8 * s, 1.5f * s);
            text_c(d, UIT_OVF_SEMI, bx + bw * 0.5f, cy - nh * 0.5f, C_TEXT, t, 0);
        }
    }
    float ey = ly0 + rowh * (float)vis + 6 * s;
    int nfiles = o->h->amiibo_files ? o->h->amiibo_files() : o->am_n;   // [UIT 0181]
    if (!nfiles && ey + sh < ybot) {
        char m[300];
        snprintf(m, sizeof m, "No amiibo files in %s (Game > Amiibo > Amiibo folder in the launcher)%s.", dir && *dir ? dir : "the amiibo folder",
                 o->am_n ? ": the built-in figures above need none" : "");
        text(d, UIT_OVF_SMALL, lx, ey, C_FAINT, m, rx1 - lx - 16 * s);
    }
    if (vis < rows) {   // the scroll bar
        float tx = rx1 - 8 * s, th = rowh * (float)vis, k = th / (float)rows;
        rect(d, tx, ly0, tx + 4 * s, ly0 + th, P_LINE, 2 * s);
        rect(d, tx, ly0 + k * (float)o->am_scroll, tx + 4 * s, ly0 + k * (float)(o->am_scroll + vis), C_DIM, 2 * s);
    }
}

// ======================================================================== [UIT 0177] the keyboard dialog: drawing
static void kb_u8(const uint16_t *t, size_t n, char *out, size_t cap) {
    size_t w = uit_swkbd_utf16_to_utf8(t, uit_swkbd_u16len(t, n), out, cap - 1);
    out[w] = 0;
}

// Text clipped to [cx0, cx1) (whole glyphs: one that would cross an edge is left out). Returns the pen's advance.
static float text_clip(Dl *d, int f, float x, float y, uint32_t c, const char *t, float cx0, float cx1) {
    const UitOvAtlas *a = d->a;
    float pen = floorf(x + 0.5f), base = floorf(y + 0.5f) + a->ascent[f], start = pen;
    while (t && *t) {
        const UitOvGlyph *g = &a->g[f][glyph_index(utf8_next(&t))];
        if (g->x1 > g->x0 && pen + g->x0 >= cx0 && pen + g->x1 <= cx1 && d->n < d->cap)
            d->q[d->n++] = (UitOvQuad){pen + g->x0, base + g->y0, pen + g->x1, base + g->y1, g->u0, g->v0, g->u1, g->v1, c, 0,
                                       UIT_OVQ_TEXT, 0};
        pen += g->adv;
    }
    return pen - start;
}

static const char *kb_special_label(const UitOvKb *k, int col, char *buf, size_t n) {
    switch (col) {
    case UIT_KBK_SHIFT: return k->shift == 2 ? "\xe2\x87\xa7 Caps" : "\xe2\x87\xa7 Shift";
    case UIT_KBK_PAGE: return k->page == UIT_KB_PAGE_LETTERS ? "#+=" : k->page == UIT_KB_PAGE_SYMBOLS ? "\xc3\xa0\xc3\xa9\xc3\xbc" : "abc";
    case UIT_KBK_SPACE: return "Space";
    case UIT_KBK_BACK: return "\xe2\x8c\xab";
    case UIT_KBK_CANCEL: return "Cancel";
    default:
        if (k->req.ok[0]) { kb_u8(k->req.ok, 9, buf, n); return buf; }
        return "OK";
    }
}

static void kb_draw(UitOv *o, Dl *d, const UitOvFrame *f) {
    UitOvKb *k = &o->kb;
    float s = d->s, pad = 26 * s, keyh = 48 * s, gap = 6 * s, fieldh = 56 * s;
    char buf[UIT_SWKBD_LIMIT * 4 + 8], tmp[600];
    float pw = 900 * s;
    if (pw > f->w - 16) pw = f->w - 16;
    int has_sub = k->req.sub[0] != 0;
    float ph = pad + text_h(d, UIT_OVF_TITLE) + (has_sub ? 6 * s + text_h(d, UIT_OVF_SMALL) : 0) + 16 * s + fieldh + 8 * s +
               text_h(d, UIT_OVF_SMALL) + 14 * s + keyh * UIT_KB_ROWS + gap * (UIT_KB_ROWS - 1) + 16 * s +
               2 * text_h(d, UIT_OVF_SMALL) + 6 * s + pad;
    float x0 = floorf((f->w - pw) * 0.5f), y0 = floorf((f->h - ph) * 0.5f), x1 = x0 + pw;
    if (y0 < 8) y0 = 8;
    float y1 = y0 + ph;
    k->panel = (UitOvBox){x0, y0, x1, y1};
    rect(d, 0, 0, f->w, f->h, C_SCRIM, 0);
    rect(d, x0, y0, x1, y1, P_PANEL, 14 * s);
    outline(d, x0, y0, x1, y1, P_LINE, 14 * s, 1);
    float y = y0 + pad;
    // the title (the game's header text), its sub text
    if (k->req.header[0]) kb_u8(k->req.header, 0x40, tmp, sizeof tmp);
    else snprintf(tmp, sizeof tmp, "%s", k->req.mode == UIT_KBMODE_NUMERIC ? "Enter a number" : "Enter text");
    text(d, UIT_OVF_TITLE, x0 + pad, y, C_TEXT, tmp, pw - 2 * pad);
    y += text_h(d, UIT_OVF_TITLE);
    if (has_sub) {
        y += 6 * s;
        kb_u8(k->req.sub, 0x80, tmp, sizeof tmp);
        text(d, UIT_OVF_SMALL, x0 + pad, y, C_DIM, tmp, pw - 2 * pad);
        y += text_h(d, UIT_OVF_SMALL);
    }
    y += 16 * s;
    // the field: the text (bullets for a password), the IME's composition at the cursor, the caret
    float fx0 = x0 + pad, fx1 = x1 - pad, fy0 = y, fy1 = y + fieldh, ip = 16 * s;
    k->field = (UitOvBox){fx0, fy0, fx1, fy1};
    rect(d, fx0, fy0, fx1, fy1, P_BG, 10 * s);
    outline(d, fx0, fy0, fx1, fy1, C_TEAL, 10 * s, 1.5f * s);
    int font = UIT_OVF_SEMI;
    float ty = fy0 + (fieldh - text_h(d, font)) * 0.5f, cx0 = fx0 + ip, cx1 = fx1 - ip;
    char before[sizeof buf], after[sizeof buf];
    if (k->req.password) {   // one bullet a character
        size_t nb = 0, na = 0;
        for (int i = 0; i < k->n; i++) {
            if (kb_is_low(k->t[i]) && i && kb_is_high(k->t[i - 1])) continue;
            char *dst = i < k->cur ? before + nb : after + na;
            memcpy(dst, "\xe2\x80\xa2", 3);
            if (i < k->cur) nb += 3; else na += 3;
        }
        before[nb] = 0;
        after[na] = 0;
    } else {
        kb_u8(k->t, (size_t)k->cur, before, sizeof before);
        kb_u8(k->t + k->cur, (size_t)(k->n - k->cur), after, sizeof after);
    }
    for (char *p = before; *p; p++) if (*p == '\n') *p = ' ';   // one line: a newline shows as a space
    for (char *p = after; *p; p++) if (*p == '\n') *p = ' ';
    float wb = uit_ov_text_width(d->a, font, before), wp = uit_ov_text_width(d->a, font, k->pre);
    float caret = wb + wp, scroll = 0;
    if (caret > cx1 - cx0 - 2 * s) scroll = caret - (cx1 - cx0 - 2 * s);   // keep the caret in sight
    float px = cx0 - scroll;
    if (!k->n && !k->pre[0]) {
        if (k->req.guide[0]) {
            kb_u8(k->req.guide, 0x100, tmp, sizeof tmp);
            text(d, font, cx0, ty, C_FAINT, tmp, cx1 - cx0);
        }
    } else {
        px += text_clip(d, font, px, ty, C_TEXT, before, cx0, cx1);
        if (k->pre[0]) {
            float w = text_clip(d, font, px, ty, C_TEAL, k->pre, cx0, cx1);
            float ux0 = px < cx0 ? cx0 : px, ux1 = px + w > cx1 ? cx1 : px + w;
            if (ux1 > ux0) rect(d, ux0, ty + text_h(d, font) + 1 * s, ux1, ty + text_h(d, font) + 2.5f * s, C_TEAL, 0);
            px += w;
        }
        text_clip(d, font, px, ty, C_TEXT, after, cx0, cx1);
    }
    if (((o->now - k->t0) / 530) % 2 == 0) {
        float cx = cx0 - scroll + caret;
        rect(d, cx, ty - 2 * s, cx + 2 * s, ty + text_h(d, font) + 2 * s, C_TEAL, 0);
    }
    y = fy1 + 8 * s;
    // under it: why the last key did nothing, or the length rule; the count
    snprintf(tmp, sizeof tmp, "%d / %d", k->n, k->req.max_len);
    text_r(d, UIT_OVF_SMALL, fx1, y, k->n >= k->req.max_len ? C_GOLD : C_DIM, tmp);
    if (k->msg[0] && o->now - k->msg_ms < KB_MSG_MS) text(d, UIT_OVF_SMALL, fx0, y, C_GOLD, k->msg, pw * 0.6f);
    else if (k->req.min_len > 1) {
        snprintf(tmp, sizeof tmp, "At least %d characters", k->req.min_len);
        text(d, UIT_OVF_SMALL, fx0, y, C_FAINT, tmp, 0);
    }
    y += text_h(d, UIT_OVF_SMALL) + 14 * s;
    // the keys
    float unit = (pw - 2 * pad - gap * (UIT_KB_COLS - 1)) / UIT_KB_COLS;
    memset(k->key, 0, sizeof k->key);
    for (int r = 0; r < UIT_KB_ROWS; r++, y += keyh + gap) {
        float kx = x0 + pad;
        for (int c = 0; c < kb_cols(r); c++) {
            float wu = r < 4 ? 1.0f : kKbSpecialW[c], kw = unit * wu + gap * (wu - 1);
            if (r == 4 && c == UIT_KBK_COUNT - 1) kw = x1 - pad - kx;   // the last key ends at the edge
            UitOvBox b = {kx, y, kx + kw, y + keyh};
            k->key[r][c] = b;
            kx += kw + gap;
            int focus = k->row == r && k->col == c;
            const char *label;
            int usable = 1, accent = 0;
            char lb[64];
            if (r < 4) {
                uint32_t cp = uit_ov_kb_char(k, r, c);
                if (!cp) continue;
                char *p = lb;
                if (cp < 0x80) *p++ = (char)cp;
                else if (cp < 0x800) { *p++ = (char)(0xC0 | cp >> 6); *p++ = (char)(0x80 | (cp & 63)); }
                else { *p++ = (char)(0xE0 | cp >> 12); *p++ = (char)(0x80 | (cp >> 6 & 63)); *p++ = (char)(0x80 | (cp & 63)); }
                *p = 0;
                label = lb;
                usable = uit_swkbd_allowed(&k->req, cp) && k->n < k->req.max_len;
            } else {
                label = kb_special_label(k, c, lb, sizeof lb);
                if (c == UIT_KBK_SPACE) usable = uit_swkbd_allowed(&k->req, ' ') && k->n < k->req.max_len;
                if (c == UIT_KBK_SHIFT) accent = k->shift != 0;
                if (c == UIT_KBK_OK) accent = 1, usable = k->n >= (k->req.min_len > 0 ? k->req.min_len : 1);
            }
            uint32_t bg = focus ? P_FOCUS : P_RAISED;
            rect(d, b.x0, b.y0, b.x1, b.y1, bg, 8 * s);
            if (accent && usable) rect(d, b.x0, b.y0, b.x1, b.y1, c == UIT_KBK_OK ? C_TEAL_SOFT : C_GOLD_SOFT, 8 * s);
            if (focus) outline(d, b.x0, b.y0, b.x1, b.y1, C_TEAL, 8 * s, 2 * s);
            int lf = r < 4 ? UIT_OVF_SEMI : UIT_OVF_BODY;
            uint32_t col = !usable ? C_FAINT : focus ? C_TEXT : accent ? (c == UIT_KBK_OK ? C_TEAL : C_GOLD) : C_BODY;
            text_c(d, lf, (b.x0 + b.x1) * 0.5f, b.y0 + (keyh - text_h(d, lf)) * 0.5f, col, label, kw - 8 * s);
        }
    }
    y += 16 * s - gap;
    // the hints
    {   // [UIT 0270] the confirm button (type) and the back one (delete) follow Game > Controller > Confirm button
        char kh[160];
        int okp = o->confirm_game ? 1 : 0, fam = o->pad_present ? o->pad_family : UIT_PADFAM_GENERIC;
        snprintf(kh, sizeof kh, "%s type \xc2\xb7 %s delete \xc2\xb7 %s space \xc2\xb7 %s symbols \xc2\xb7 L3 shift \xc2\xb7 L / R move \xc2\xb7 + OK \xc2\xb7 \xe2\x80\x93 cancel",
                 uit_ov_face_label(fam, okp), uit_ov_face_label(fam, 1 - okp), uit_ov_face_label(fam, 2), uit_ov_face_label(fam, 3));
        text(d, UIT_OVF_SMALL, x0 + pad, y, C_DIM, kh, pw - 2 * pad);
    }
    y += text_h(d, UIT_OVF_SMALL) + 6 * s;
    text(d, UIT_OVF_SMALL, x0 + pad, y, C_FAINT,
         "Or type on the keyboard \xc2\xb7 Enter OK \xc2\xb7 Esc cancel \xc2\xb7 Ctrl+V paste \xc2\xb7 click a key", pw - 2 * pad);
}

// [UIT 0292] the Restart / Quit question, over the panel: a title, what is lost, [Cancel] [Quit] right aligned (as the
// launcher's dialogs), the focused one outlined
static void ask_draw(UitOv *o, Dl *d, float px0, float py0, float px1, float py1) {
    float s = d->s, pad = 26 * s, bh = 44 * s, gap = 10 * s;
    int restart = o->ask == OV_RESTART;
    const char *title = restart ? "Restart the game?" : "Quit to desktop?", *body = "Progress since your last save will be lost.";
    const char *lab[2] = {"Cancel", restart ? "Restart" : "Quit"};
    int okp = o->confirm_game ? 1 : 0, fam = o->pad_present ? o->pad_family : UIT_PADFAM_GENERIC;
    char hint[160];
    snprintf(hint, sizeof hint, "\xe2\x86\x90\xe2\x86\x92 choose  \xc2\xb7  Enter / %s  press  \xc2\xb7  Esc / %s  cancel",
             uit_ov_face_label(fam, okp), uit_ov_face_label(fam, 1 - okp));
    float pw = 560 * s;
    if (pw > px1 - px0 - 16 * s) pw = px1 - px0 - 16 * s;
    float ph = pad + text_h(d, UIT_OVF_TITLE) + 12 * s + text_h(d, UIT_OVF_BODY) + 26 * s + bh + 14 * s + text_h(d, UIT_OVF_SMALL) + pad;
    float x0 = floorf((px0 + px1 - pw) * 0.5f), y0 = floorf((py0 + py1 - ph) * 0.5f), x1 = x0 + pw, y1 = y0 + ph;
    o->ask_panel = (UitOvBox){x0, y0, x1, y1};
    rect(d, px0, py0, px1, py1, C_SCRIM, 14 * s);
    rect(d, x0, y0, x1, y1, P_PANEL, 14 * s);
    outline(d, x0, y0, x1, y1, P_LINE, 14 * s, 1);
    float y = y0 + pad;
    text(d, UIT_OVF_TITLE, x0 + pad, y, C_TEXT, title, pw - 2 * pad);
    y += text_h(d, UIT_OVF_TITLE) + 12 * s;
    text(d, UIT_OVF_BODY, x0 + pad, y, C_BODY, body, pw - 2 * pad);
    y += text_h(d, UIT_OVF_BODY) + 26 * s;
    float w[2], total = gap;
    for (int b = 0; b < 2; b++) {
        w[b] = uit_ov_text_width(d->a, UIT_OVF_SEMI, lab[b]) + 48 * s;
        if (w[b] < 132 * s) w[b] = 132 * s;
        total += w[b];
    }
    float bx = x1 - pad - total;
    for (int b = 0; b < 2; b++) {
        int foc = o->ask_sel == b;
        UitOvBox r = {bx, y, bx + w[b], y + bh};
        o->ask_btn[b] = r;
        rect(d, r.x0, r.y0, r.x1, r.y1, foc ? P_FOCUS : P_RAISED, 8 * s);
        if (b == 1) rect(d, r.x0, r.y0, r.x1, r.y1, C_GOLD_SOFT, 8 * s);   // the one that ends the game
        if (foc) outline(d, r.x0, r.y0, r.x1, r.y1, C_TEAL, 8 * s, 2 * s);
        text_c(d, UIT_OVF_SEMI, (r.x0 + r.x1) * 0.5f, r.y0 + (bh - text_h(d, UIT_OVF_SEMI)) * 0.5f,
               foc ? C_TEXT : b == 1 ? C_GOLD : C_BODY, lab[b], 0);
        bx += w[b] + gap;
    }
    y += bh + 14 * s;
    text(d, UIT_OVF_SMALL, x0 + pad, y, C_FAINT, hint, pw - 2 * pad);
}

int uit_ov_build(UitOv *o, const UitOvAtlas *a, const UitOvFrame *f, UitOvQuad *q, int cap) {
    o->now = f->now_ms;
    o->dirty = 0;
    if (!a || !a->pix) return 0;
    Dl dl = {q, 0, cap, a, a->scale}, *d = &dl;
    float s = a->scale;
    o->fw = f->w;
    o->fh = f->h;
    o->s = s;
    if (o->kb.active) { kb_draw(o, d, f); return d->n; }   // [UIT 0177] only the dialog
    if (!o->open) { hud(o, d, f); return d->n; }
    rows_for_tab(o);
    // the panel: as tall as the longest tab, so switching tabs does not move it
    int most = 0;
    for (int t = 0; t < UIT_OV_TABS; t++) {
        int n = 0;
        for (int i = 0; i < OV_COUNT; i++) n += S(i)->tab == t && shown(o, i);
        if (n > most) most = n;
    }
    float pad = 28 * s, rowh = 64 * s, pw = 860 * s, head = 86 * s, tabs = 46 * s, foot = 58 * s;
    // [UIT 0321] at most UIT_OV_PANEL_ROWS rows, and never taller than the screen: longer tabs scroll
    float fixed = pad + head + tabs + 10 * s + foot;
    int fit = (int)((f->h - 16 - fixed) / rowh);
    if (most > UIT_OV_PANEL_ROWS) most = UIT_OV_PANEL_ROWS;
    if (most > fit) most = fit;
    if (most < 1) most = 1;
    o->rvis = most;
    float ph = fixed + rowh * (float)most;
    if (pw > f->w - 16) pw = f->w - 16;
    float x0 = floorf((f->w - pw) * 0.5f), y0 = floorf((f->h - ph) * 0.5f), x1 = x0 + pw, y1 = y0 + ph;
    if (y0 < 8) y0 = 8;
    o->panel = (UitOvBox){x0, y0, x1, y1};
    rect(d, 0, 0, f->w, f->h, C_SCRIM, 0);
    rect(d, x0, y0, x1, y1, P_PANEL, 14 * s);
    outline(d, x0, y0, x1, y1, P_LINE, 14 * s, 1);
    // header
    float y = y0 + pad;
    text(d, UIT_OVF_SMALL, x0 + pad, y, C_FAINT, "ULTRA INSTINCT TEARS", 0);
    y += text_h(d, UIT_OVF_SMALL) + 8 * s;
    text(d, UIT_OVF_TITLE, x0 + pad, y, C_TEXT, "In-game settings", 0);
    {
        float cs = 34 * s, cx = x1 - pad - cs, cy = y0 + pad;
        int hov = in_box(&o->closebox, o->mx, o->my);
        if (hov) rect(d, cx, cy, cx + cs, cy + cs, P_RAISED, 8 * s);
        text_c(d, UIT_OVF_TITLE, cx + cs * 0.5f, cy + (cs - text_h(d, UIT_OVF_TITLE)) * 0.5f, hov ? C_TEXT : C_DIM, "\xc3\x97", 0);
        o->closebox = (UitOvBox){cx, cy, cx + cs, cy + cs};
        char hint[96];
        snprintf(hint, sizeof hint, "%s or hold %s to close", f->hotkey ? f->hotkey : "F1", f->pad_combo ? f->pad_combo : "L3 + R3");
        text_r(d, UIT_OVF_SMALL, cx - 14 * s, cy + (cs - text_h(d, UIT_OVF_SMALL)) * 0.5f, C_FAINT, hint);
    }
    // tabs (LB / RB, Q / E)
    y = y0 + pad + head;
    float tx = x0 + pad;
    for (int t = 0; t < UIT_OV_TABS; t++) {
        float w = uit_ov_text_width(a, UIT_OVF_SEMI, kTabNames[t]) + 28 * s, h = 34 * s;
        int on = t == o->tab, hov = in_box(&o->tabbox[t], o->mx, o->my);
        if (on) rect(d, tx, y, tx + w, y + h, C_TEAL_SOFT, h * 0.5f);
        else if (hov) rect(d, tx, y, tx + w, y + h, P_RAISED, h * 0.5f);
        text_c(d, UIT_OVF_SEMI, tx + w * 0.5f, y + (h - text_h(d, UIT_OVF_SEMI)) * 0.5f, on ? C_TEAL : hov ? C_TEXT : C_DIM, kTabNames[t], 0);
        o->tabbox[t] = (UitOvBox){tx, y, tx + w, y + h};
        tx += w + 6 * s;
    }
    {   // the hint, when the tabs leave room for it (7 tabs on a narrow window do not)
        const char *th = "LB / RB  or  Q / E: tabs";
        if (tx + 12 * s <= x1 - pad - uit_ov_text_width(a, UIT_OVF_SMALL, th))
            text_r(d, UIT_OVF_SMALL, x1 - pad, y + (34 * s - text_h(d, UIT_OVF_SMALL)) * 0.5f, C_FAINT, th);
    }
    y += tabs;
    rect(d, x0 + pad, y, x1 - pad, y + 1, P_LINE, 0);
    y += 10 * s;
    // rows
    for (int i = 0; i < OV_COUNT; i++) o->rowbox[i] = (UitOvBox){0};
    float cw = 340 * s;
    int *rs = &o->rscroll[o->tab], vis = most;   // [UIT 0321] keep the selected row in sight
    if (o->sel[o->tab] < *rs) *rs = o->sel[o->tab];
    if (o->sel[o->tab] >= *rs + vis) *rs = o->sel[o->tab] - vis + 1;
    if (*rs > o->nrows - vis) *rs = o->nrows - vis;
    if (*rs < 0) *rs = 0;
    for (int r = *rs; r < o->nrows && r < *rs + vis; r++) {
        int id = o->rows[r], on = r == o->sel[o->tab];
        const UitOvSetting *st = S(id);
        const char *w = why(o, id);
        float ry = y + rowh * (float)(r - *rs), rx0 = x0 + pad * 0.5f, rx1 = x1 - pad * 0.5f;
        o->rowbox[id] = (UitOvBox){rx0, ry, rx1, ry + rowh};
        if (on) {
            rect(d, rx0, ry + 2 * s, rx1, ry + rowh - 2 * s, P_FOCUS, 10 * s);
            rect(d, rx0, ry + 14 * s, rx0 + 3 * s, ry + rowh - 14 * s, C_TEAL, 1.5f * s);
        }
        float lx = rx0 + pad * 0.5f + 8 * s, nh = text_h(d, UIT_OVF_SEMI), hh = text_h(d, UIT_OVF_SMALL);
        float ty = ry + (rowh - nh - hh - 5 * s) * 0.5f;
        char help[256], shot[48] = "";
        const char *ht = st->help;
        if (id == OV_KEYS) {
            if (f->shot) snprintf(shot, sizeof shot, "  \xc2\xb7  %s screenshot", f->shot);   // [UIT 0346]
            snprintf(help, sizeof help, "Overlay: %s, or hold %s%s  \xc2\xb7  F7 mark  \xc2\xb7  Shift+F7 capture  \xc2\xb7  F8 FSR  \xc2\xb7  Shift+F8 benchmark  \xc2\xb7  F9 sharpness  \xc2\xb7  F10 DLSS  \xc2\xb7  F11 fullscreen",
                     f->hotkey ? f->hotkey : "F1", f->pad_combo ? f->pad_combo : "L3 + R3", shot);
            ht = help;
        }
        float maxw = st->kind == UIT_OVS_INFO ? rx1 - lx - 16 * s : rx1 - lx - cw - 40 * s;
        text(d, UIT_OVF_SEMI, lx, ty, w ? C_DIM : on ? C_TEXT : C_BODY, st->name, maxw);
        if (w) text(d, UIT_OVF_SMALL, lx, ty + nh + 5 * s, C_GOLD, w, maxw);
        else if (ht) text(d, UIT_OVF_SMALL, lx, ty + nh + 5 * s, on ? C_DIM : C_FAINT, ht, maxw);
        if (st->kind != UIT_OVS_INFO) control(o, d, id, rx1 - 16 * s - cw, rx1 - 16 * s, ry + rowh * 0.5f, on, w != NULL);
    }
    if (o->nrows > vis && o->tab != UIT_OV_TAB_AMIIBO && o->tab != UIT_OV_TAB_CONTROLS) {   // [UIT 0321] the scroll bar
        float tx = x1 - pad * 0.5f + 6 * s, th = rowh * (float)vis, k = th / (float)o->nrows;
        rect(d, tx, y, tx + 4 * s, y + th, P_LINE, 2 * s);
        rect(d, tx, y + k * (float)*rs, tx + 4 * s, y + k * (float)(*rs + vis), C_DIM, 2 * s);
    }
    if (o->tab == UIT_OV_TAB_AMIIBO) amiibo_draw(o, d, x0, x1, y, y1 - foot - 4 * s, pad);
    else o->am_nbox = 0;
    if (o->tab == UIT_OV_TAB_CONTROLS) controls_draw(o, d, x0, x1, y, y1 - foot - 4 * s, pad);
    else
        for (int r = 0; r < UIT_OV_BIND_ROWS; r++) o->brow[r] = (UitOvBox){0};
    // footer: the keys, and what happened to the last change
    float fy = y1 - foot;
    rect(d, x0 + pad, fy, x1 - pad, fy + 1, P_LINE, 0);
    float ly = fy + (foot - text_h(d, UIT_OVF_SMALL)) * 0.5f;
    // [UIT 0270] the controller's confirm / back / reset buttons as Game > Controller > Confirm button sets them
    int okp = o->confirm_game ? 1 : 0, fam = o->pad_present ? o->pad_family : UIT_PADFAM_GENERIC;
    const char *fa = uit_ov_face_label(fam, okp), *fb = uit_ov_face_label(fam, 1 - okp), *fx = uit_ov_face_label(fam, 2);
    char keys[200];
    if (o->tab == UIT_OV_TAB_AMIIBO)
        snprintf(keys, sizeof keys, "Enter / %s  scan  \xc2\xb7  Del / %s  read the folder again  \xc2\xb7  Esc / %s  close", fa, fx, fb);
    else if (o->tab == UIT_OV_TAB_CONTROLS && !o->cap)
        snprintf(keys, sizeof keys, "Enter / %s  change  \xc2\xb7  Del / %s  default  \xc2\xb7  Esc / %s  close", fa, fx, fb);
    else if (o->tab == UIT_OV_TAB_CONTROLS && o->cap_kind == UIT_BIND_PAD) snprintf(keys, sizeof keys, "Esc  cancel  \xc2\xb7  Delete  unbind");
    else if (o->tab == UIT_OV_TAB_CONTROLS) snprintf(keys, sizeof keys, "Esc / %s  cancel  \xc2\xb7  Delete / %s  unbind", fb, fx);
    else
        snprintf(keys, sizeof keys, "\xe2\x86\x91\xe2\x86\x93 select  \xc2\xb7  \xe2\x86\x90\xe2\x86\x92 change  \xc2\xb7  Enter / %s  toggle  \xc2\xb7  Esc / %s  close", fa, fb);
    float hw = text(d, UIT_OVF_SMALL, x0 + pad, ly, o->cap ? C_GOLD : C_FAINT, keys, 0);
    if (o->status[0] && f->now_ms - o->status_ms > 5000) o->status[0] = 0;   // a message shows for 5 s
    if (o->status[0]) {
        float smax = pw - pad * 2 - hw - 24 * s, sw = uit_ov_text_width(a, UIT_OVF_SMALL, o->status) + 20 * s;
        if (sw > smax) sw = smax;
        float sh = text_h(d, UIT_OVF_SMALL) + 10 * s;
        float sx = x1 - pad - sw, sy = fy + (foot - sh) * 0.5f;
        rect(d, sx, sy, sx + sw, sy + sh, o->status_bad ? C_GOLD_SOFT : C_TEAL_SOFT, sh * 0.5f);
        text(d, UIT_OVF_SMALL, sx + 10 * s, sy + 5 * s, o->status_bad ? C_GOLD : C_TEAL, o->status, sw - 20 * s);
    } else if (!o->save)
        text_r(d, UIT_OVF_SMALL, x1 - pad, ly, C_GOLD, "This session only");
    if (o->ask) ask_draw(o, d, x0, y0, x1, y1);   // [UIT 0292]
    hud(o, d, f);
    return d->n;
}
