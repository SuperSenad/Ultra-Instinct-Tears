// [UIT 0171] The in-game overlay's glue (window / SDL main thread): the settings hooks over src/host/uit_live.h, the
// hotkey, mouse and controller input, saving, and the hand-over to the present thread (overlay_share.h).
// [UIT 0174] the Controls tab's bindings come from window_sdl.c (uit_input_binds) and go back to it on each change
// (uit_input_apply_binds: the game uses them at once; input_bind.h).
//   TOTK_OVERLAY=0            no menu, key or combo (nothing is built unless the game asks for text: the keyboard dialog)
//   TOTK_OVERLAY_KEY=F1       the keyboard key that opens / closes it (F1..F6, F12, Insert, Home, End, ...)
//   TOTK_OVERLAY_PAD=leftstick+rightstick   the controller combo (SDL button names joined by '+'; none = keyboard only)
//                             [UIT 0271] two combos split by ',' (either opens it): leftstick+rightstick,guide; guide alone
//   TOTK_UI_CONFIRM=game      [UIT 0270] the right face button confirms and the bottom one closes (default standard: the bottom)
//   TOTK_OVERLAY_HOLD_MS=1000 how long the combo is held (300..3000)
//   TOTK_OVERLAY_FPS=1        the frame rate counter from the start
// [UIT 0175] the Amiibo tab (a file -> src/host/amiibo.c's reader -> nfp.c, the game's nn::nfp) and the Cheats tab
// (src/host/cheats.c):
//   TOTK_AMIIBO_DIR=amiibo    the folder the Amiibo tab lists (relative to the game folder); any file: Choose a file
// [UIT 0177] the keyboard dialog (nn::swkbd, src/host/swkbd_host.c): the game's thread posts a request here and waits;
// the window thread shows it (uit_overlay.c), takes the keys / text / clicks / controller, and answers. Also with
// TOTK_OVERLAY=0 (the menu is off, the dialog is not: a game asking for a name must get one). Unused, it costs one
// relaxed atomic load a window loop.
#include "overlay_live.h"

#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "amiibo.h"
#include "cheats.h"
#include "host.h"
#include "overlay_share.h"
#include "uit_live.h"
#include "uit_overlay.h"
#include "../vk/dlss.h"
#include "../vk/present.h"
#include "../gfx/vkb_capture.h"   // [UIT 0157] Overlay > Capture a frame for diagnosis
#include "uit_font.h"   // build/: third_party/inter (tools/build_game.py font_header)

// ---------------------------------------------------------------- the hand-over to the present thread
_Atomic int g_uit_ov_visible;
static pthread_mutex_t share_lock = PTHREAD_MUTEX_INITIALIZER;
static UitOvQuad share_q[UIT_OV_MAX_QUADS];
static UitOvShared share = {share_q, 0, 0, NULL, 0, 0, 0};
const UitOvShared *uit_ov_share_lock(void) { pthread_mutex_lock(&share_lock); return &share; }
void uit_ov_share_unlock(void) { pthread_mutex_unlock(&share_lock); }

// ---------------------------------------------------------------- state (main thread)
static int enabled;     // the glue runs (the window is up, the font atlas did not fail)
static int menu_off;    // [UIT 0177] TOTK_OVERLAY=0: no menu, no hotkey, no combo; the keyboard dialog still works
static SDL_Window *win;
static void (*fullscreen_cb)(void), (*mark_cb)(void);
static UitOv ov;
static UitOvAtlas atlas;
static SDL_Scancode hotkey = SDL_SCANCODE_F1;
static char hotkey_name[32] = "F1", combo_name[64] = "L3 + R3";
static UitOvQuad build_q[UIT_OV_MAX_QUADS];
static int last_n = -1;
static uint32_t last_phys;
static double fps = -1;

int runtime_nfp_present(const UitAmiibo *a);   // nfp.c: the figure on the reader (signals the game)
void runtime_nfp_tick(void);
void runtime_amiibo_log(const char *fmt, ...);   // nfp.c [UIT 0180]: the log and diag/amiibo.txt

static int is_fullscreen(void) { return (SDL_GetWindowFlags(win) & SDL_WINDOW_FULLSCREEN) != 0; }

// ---------------------------------------------------------------- the settings hooks
static const int kFps[3] = {30, 60, 120};
static const int kMaxIv[4] = {2, 1, 3, 0};   // TOTK_MAX_INTERVAL choices "-", 1, 3, 0 (hle_nvn.c: 2 = half rate)

static int win_size_choice(int i, int *w, int *h) {
    char v[32];
    if (!uit_ov_choice(&kUitOvSettings[OV_WINSIZE], i, v, sizeof v, NULL, 0)) return 0;
    if (!strcmp(v, "-")) { *w = 1280; *h = 720; return 1; }
    return sscanf(v, "%dx%d", w, h) == 2;
}

static double hk_get(int id) {
    int b, started, a;
    double g;
    switch (id) {
    case OV_FSR: return vkp_fsr_wanted();
    case OV_DLSS: return dlss_active();
    case OV_SHARP: return vkp_fsr_sharpness();
    case OV_FULLSCREEN: return is_fullscreen();
    case OV_WINSIZE: {
        int w = 0, h = 0, cw, ch;
        SDL_GetWindowSize(win, &w, &h);
        for (int i = 0; win_size_choice(i, &cw, &ch); i++)
            if (cw == w && ch == h) return i;
        return -1;
    }
    case OV_PRESENT: return vkp_present_mode_choice();
    case OV_TRIPLE: return vkp_triple_buffer_choice();
    case OV_FPS:
        for (int i = 0; i < 3; i++)
            if (kFps[i] == totk_fps_target()) return i;
        return -1;
    case OV_HOLD: return nvn_interval_hold();
    case OV_MAXIV:
        for (int i = 0; i < 4; i++)
            if (kMaxIv[i] == nvn_max_interval()) return i;
        return -1;
    case OV_TEXBUD: vkb_tex_live(&b, &g); return b;
    case OV_TEXGRACE: vkb_tex_live(&b, &g); return g;
    case OV_VOLUME: return audio_volume();
    case OV_RUMBLE: b = runtime_hid_feedback_on("rumble", &started); return b && started;
    case OV_GYRO: b = runtime_hid_feedback_on("gyro", &started); return b && started;
    case OV_GYRODZ: g = runtime_motion_deadzone(&a); return a ? kUitOvSettings[OV_GYRODZ].min - 1 : g;
    case OV_LSDZ: return uit_stick_deadzone(0);
    case OV_RSDZ: return uit_stick_deadzone(1);
    case OV_STICKOUT: return uit_stick_deadzone(2);
    case OV_TRIGPT: return uit_trigger_point();
    case OV_CHEAT_DUR: return uit_cheat_durability();
    case OV_CHEAT_AMIIBO: return uit_cheat_amiibo();
    default: return 0;
    }
}

static void hk_set(int id, double v) {
    int on = v >= 0.5, i = (int)lround(v);
    switch (id) {
    case OV_FSR: vkp_fsr_set(on); break;
    case OV_DLSS: if (dlss_active() != on) dlss_toggle(); break;
    case OV_SHARP: vkp_fsr_set_sharpness((float)v); break;
    case OV_FULLSCREEN: if (is_fullscreen() != on && fullscreen_cb) fullscreen_cb(); break;
    case OV_WINSIZE: {
        int w, h;
        if (!win_size_choice(i, &w, &h)) break;
        if (SDL_GetWindowFlags(win) & SDL_WINDOW_MAXIMIZED) SDL_RestoreWindow(win);
        SDL_SetWindowSize(win, w, h);
        SDL_SetWindowPosition(win, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
        break;
    }
    case OV_PRESENT: vkp_set_present_mode(i); break;
    case OV_TRIPLE: vkp_set_triple_buffer(i); break;
    case OV_FPS: if (i >= 0 && i < 3 && totk_fps_retarget(kFps[i])) nvn_fps_retarget(kFps[i]); break;
    case OV_HOLD: nvn_set_interval_hold(i); break;
    case OV_MAXIV: if (i >= 0 && i < 4) nvn_set_max_interval(kMaxIv[i]); break;
    case OV_TEXBUD: vkb_tex_set_budget_mb(i); break;
    case OV_TEXGRACE: vkb_tex_set_grace_ms(v); break;
    case OV_VOLUME: audio_set_volume(i); break;
    case OV_RUMBLE: runtime_hid_feedback_flag("rumble", on); break;
    case OV_GYRO: runtime_hid_feedback_flag("gyro", on); break;
    case OV_GYRODZ: runtime_motion_set_deadzone(v < kUitOvSettings[OV_GYRODZ].min ? -1 : v); break;
    case OV_LSDZ: uit_set_stick_deadzone(0, i); break;
    case OV_RSDZ: uit_set_stick_deadzone(1, i); break;
    case OV_STICKOUT: uit_set_stick_deadzone(2, i); break;
    case OV_TRIGPT: uit_set_trigger_point(i); break;
    case OV_CHEAT_DUR: uit_cheat_set_durability(on); break;
    case OV_CHEAT_AMIIBO: uit_cheat_set_amiibo(on); break;
    default: break;
    }
}

// NULL: the setting can change now; otherwise why not (shown in gold under its name)
static const char *hk_why(int id) {
    int b, started;
    double g;
    switch (id) {
    case OV_SHARP: return vkp_fsr_wanted() ? NULL : "Applies with AMD FSR 1 on";
    case OV_WINSIZE: return is_fullscreen() ? "Leave fullscreen to change the window size" : NULL;
    case OV_FPS: return totk_fps_start() > 30 ? NULL : "This run started at 30 FPS: pick 60 or 120 in the launcher (Performance > Frame rate)";
    case OV_HOLD: case OV_MAXIV: return totk_fps_start() > 30 ? NULL : "Applies at 60 or 120 FPS";
    case OV_TEXBUD: case OV_TEXGRACE: return vkb_tex_live(&b, &g) ? NULL : "Texture streaming was off at start (Performance > Stutter > Textures > Texture streaming)";
    case OV_RUMBLE: runtime_hid_feedback_on("rumble", &started);
        return started ? NULL : "Off when the game started: turn it on in the launcher (Game > Controller)";
    case OV_GYRO: runtime_hid_feedback_on("gyro", &started);
        return started ? NULL : "Off when the game started: turn it on in the launcher (Game > Controller)";
    case OV_GYRODZ: runtime_hid_feedback_on("gyro", &started);
        return started && runtime_hid_feedback_on("gyro", NULL) ? NULL : "Applies with motion controls on";
    case OV_CHEAT_DUR: return uit_cheat_durability_available() ? NULL : "Not in this build: the game code it patches was not found";
    case OV_CHEAT_AMIIBO: return uit_cheat_amiibo_available() ? NULL : "Not in this build: the game code it patches was not found";
    default: return NULL;
    }
}

static int hk_shown(int id) { return id != OV_DLSS || dlss_available(); }

static const char *hk_text(int id, double v, char *buf, size_t n) {
    if (id == OV_PRESENT && (int)v == 0) { snprintf(buf, n, "Automatic (now %s)", vkp_present_mode_name()); return buf; }
    if (id == OV_WINSIZE && is_fullscreen()) { snprintf(buf, n, "Fullscreen"); return buf; }
    if (id == OV_WINSIZE && v < 0) {
        int w = 0, h = 0;
        SDL_GetWindowSize(win, &w, &h);
        snprintf(buf, n, "%d x %d", w, h);
        return buf;
    }
    return NULL;
}

static int hk_choice_ok(int id, int i) {
    if (id == OV_FPS) return i > 0;   // 30 <-> 60 is not live: the 60 FPS game patches are chosen at start
    if (id == OV_WINSIZE) {           // not larger than the screen
        int w, h;
        SDL_Rect r;
        if (!win_size_choice(i, &w, &h)) return 0;
        if (SDL_GetDisplayUsableBounds(SDL_GetDisplayForWindow(win), &r)) return w <= r.w && h <= r.h;
    }
    return 1;
}

static void hk_action(int id) {
    if (id == OV_MARK && mark_cb) mark_cb();
    if (id == OV_CAPTURE) vkb_capture_request();   // [UIT 0157] as Shift+F7
}
static int hk_dlss_run(void) { return dlss_upscaler() == UPSCALER_DLSS; }

// ---------------------------------------------------------------- [UIT 0175] the Amiibo tab
// A file (the folder's list or the system's dialog) is read and parsed here (amiibo.c: plaintext ID and serial only, no
// keys), then put on the reader (nfp.c). The dialog answers on its own thread: the path waits under a lock for the next
// frame. Nothing here runs until the tab is used: no folder read, no timers (the reader's tick starts with a scan).
typedef struct { char file[192], shown[200]; } AmFile;
static AmFile *am_files;
static int am_n, am_cap, am_used, am_nb;   // am_nb: built-in figures listed after the files [UIT 0181]
static char am_dir[512] = "amiibo";
static UitAmiibo am_last;
static int am_have_last;
static pthread_mutex_t am_lock = PTHREAD_MUTEX_INITIALIZER;
static char am_picked[1024];
static int am_pick_state;   // 0 nothing, 1 the dialog is open, 2 a path is waiting, 3 cancelled, 4 the dialog failed
static char am_pick_err[160];

static int am_ext_ok(const char *n) {
    const char *e = strrchr(n, '.');
    return e && (!SDL_strcasecmp(e, ".bin") || !SDL_strcasecmp(e, ".json") || !SDL_strcasecmp(e, ".nfc"));   // .nfc: [UIT 0181]
}
static int am_cmp(const void *a, const void *b) { return SDL_strcasecmp(((const AmFile *)a)->file, ((const AmFile *)b)->file); }

// Read and parse one file: 0 = ok (*out filled), else UIT_AMIIBO_* (and *why: the words)
static int am_load(const char *path, const char *shown_name, UitAmiibo *out, const char **why) {
    SDL_PathInfo pi;
    if (!SDL_GetPathInfo(path, &pi) || pi.type != SDL_PATHTYPE_FILE) { *why = "the file can't be found or read"; return UIT_AMIIBO_EMPTY; }
    if (pi.size > 65536) { *why = uit_amiibo_error_text(UIT_AMIIBO_SIZE); return UIT_AMIIBO_SIZE; }
    size_t n = 0;
    void *d = SDL_LoadFile(path, &n);
    if (!d) { *why = "the file can't be read"; return UIT_AMIIBO_EMPTY; }
    int r = uit_amiibo_parse((const uint8_t *)d, n, shown_name, out);
    SDL_free(d);
    *why = uit_amiibo_error_text(r);
    return r;
}
static const char *base_name(const char *p) {
    const char *b = p;
    for (const char *c = p; *c; c++)
        if (*c == '/' || *c == '\\') b = c + 1;
    return b;
}

static int hk_am_list(int refresh) {
    if (!refresh && am_used) return am_n + am_nb;
    am_used = 1;
    const char *e = getenv("TOTK_AMIIBO_DIR");
    snprintf(am_dir, sizeof am_dir, "%s", e && *e ? e : "amiibo");
    am_n = 0;
    int count = 0;
    char **names = SDL_GlobDirectory(am_dir, NULL, 0, &count);
    for (int i = 0; names && i < count && am_n < 1000; i++) {
        if (!am_ext_ok(names[i]) || strchr(names[i], '/') || strchr(names[i], '\\') || strlen(names[i]) >= sizeof am_files->file) continue;
        if (am_n == am_cap) {
            int nc = am_cap ? am_cap * 2 : 32;
            AmFile *nf = (AmFile *)realloc(am_files, (size_t)nc * sizeof *nf);
            if (!nf) break;
            am_files = nf, am_cap = nc;
        }
        AmFile *f = &am_files[am_n++];
        snprintf(f->file, sizeof f->file, "%s", names[i]);
    }
    int listed = names != NULL;
    SDL_free(names);
    qsort(am_files, (size_t)am_n, sizeof *am_files, am_cmp);
    runtime_amiibo_log("amiibo folder \"%s\": %s, %d amiibo file(s) (.bin / .json / .nfc)", am_dir,   // [UIT 0180]
                       listed ? "read" : "can't be read (not there?)", am_n);
    for (int i = 0; i < am_n; i++) {   // the character's name next to each file (and why one would not scan)
        char path[800];
        snprintf(path, sizeof path, "%s/%s", am_dir, am_files[i].file);
        UitAmiibo a;
        const char *why;
        if (!am_load(path, am_files[i].file, &a, &why))
            snprintf(am_files[i].shown, sizeof am_files[i].shown, "%s  \xc2\xb7  %s", a.name, am_files[i].file);
        else
            snprintf(am_files[i].shown, sizeof am_files[i].shown, "%s  \xc2\xb7  %s", am_files[i].file, why);
    }
    am_nb = uit_amiibo_builtin_count();
    return am_n + am_nb;
}
// [UIT 0181] After the folder's files: the built-in figures (amiibo.c uit_amiibo_builtin), so no file is needed at all
static char am_bshown[96];
static int hk_am_files(void) { return am_n; }
static const char *hk_am_file(int i) {
    if (i >= 0 && i < am_n) return am_files[i].shown;
    UitAmiibo a;
    if (i < am_n || uit_amiibo_builtin(i - am_n, &a)) return NULL;
    snprintf(am_bshown, sizeof am_bshown, "%s  \xc2\xb7  built in (no file)", a.name);
    return am_bshown;
}
static const char *hk_am_last(void) { return am_have_last ? am_last.name : NULL; }
static const char *hk_am_dir(void) { return am_dir; }

static int am_present(const UitAmiibo *a, char *msg, size_t n) {
    am_last = *a;
    am_have_last = 1;
    int r = runtime_nfp_present(a);
    if (r == 1) { snprintf(msg, n, "Scanned %s", a->name); return 0; }
    snprintf(msg, n, "The game isn't asking for an amiibo right now: %s waits on the reader for %d s", a->name, UIT_NFP_HOLD_MS / 1000);
    return 1;
}
static int am_scan_path(const char *path, const char *shown, char *msg, size_t n) {
    UitAmiibo a;
    const char *why;
    int r = am_load(path, shown, &a, &why);
    if (r) {
        snprintf(msg, n, "Not scanned, %s (%s)", why, shown);   // the reason first: a long file name is what gets cut
        runtime_amiibo_log("not scanned: %s: %s", shown, why);   // [UIT 0180] the file's name only, not its folder
        return 1;
    }
    return am_present(&a, msg, n);
}
static int hk_am_scan(int i, char *msg, size_t n) {
    if (i < 0) {
        if (!am_have_last) { snprintf(msg, n, "Nothing scanned yet"); return 1; }
        UitAmiibo a = am_last;
        return am_present(&a, msg, n);
    }
    if (i >= am_n) {   // [UIT 0181] a built-in figure
        UitAmiibo a;
        if (uit_amiibo_builtin(i - am_n, &a)) return 1;
        return am_present(&a, msg, n);
    }
    char path[800];
    snprintf(path, sizeof path, "%s/%s", am_dir, am_files[i].file);
    return am_scan_path(path, am_files[i].file, msg, n);
}

static void SDLCALL am_dialog_done(void *ud, const char *const *list, int filter) {
    (void)ud, (void)filter;
    pthread_mutex_lock(&am_lock);
    if (!list) {
        am_pick_state = 4;
        snprintf(am_pick_err, sizeof am_pick_err, "%s", SDL_GetError());
    } else if (!list[0]) am_pick_state = 3;
    else {
        snprintf(am_picked, sizeof am_picked, "%s", list[0]);
        am_pick_state = 2;
    }
    pthread_mutex_unlock(&am_lock);
}
static void hk_am_pick(void) {
    static const SDL_DialogFileFilter kFilters[] = {{"amiibo files (*.bin, *.json, *.nfc)", "bin;json;nfc"}, {"All files", "*"}};
    pthread_mutex_lock(&am_lock);
    int busy = am_pick_state == 1;
    if (!busy) am_pick_state = 1;
    pthread_mutex_unlock(&am_lock);
    if (busy) { uit_ov_note(&ov, 1, "The file dialog is already open"); return; }
    if (!am_used) hk_am_list(1);
    static char where[600];   // the dialog keeps the pointer until it answers
    SDL_PathInfo pi;
    const char *base = SDL_GetBasePath();
    if (SDL_GetPathInfo(am_dir, &pi) && pi.type == SDL_PATHTYPE_DIRECTORY) {
        if (am_dir[0] == '/' || am_dir[0] == '\\' || (am_dir[0] && am_dir[1] == ':') || !base) snprintf(where, sizeof where, "%s/", am_dir);
        else snprintf(where, sizeof where, "%s%s/", base, am_dir);
    } else where[0] = 0;
    uit_ov_note(&ov, 0, is_fullscreen() ? "Choose the file in the window that opened (Alt+Tab if it is behind the game)"
                                        : "Choose the file in the window that opened");
    SDL_ShowOpenFileDialog(am_dialog_done, NULL, win, kFilters, 2, where[0] ? where : NULL, false);
}
static void am_pick_poll(void) {
    if (!am_pick_state) return;   // one plain load per frame while nothing was asked
    char path[sizeof am_picked], err[sizeof am_pick_err];
    pthread_mutex_lock(&am_lock);
    int st = am_pick_state;
    if (st >= 2) {
        memcpy(path, am_picked, sizeof path);
        memcpy(err, am_pick_err, sizeof err);
        am_pick_state = 0;
    }
    pthread_mutex_unlock(&am_lock);
    if (st < 2) return;
    if (st == 3) { uit_ov_note(&ov, 0, "No file chosen"); return; }
    if (st == 4) {
        char m[200];
        snprintf(m, sizeof m, "The file dialog did not open (%s): put the file in the amiibo folder", err[0] ? err : "no reason given");
        runtime_amiibo_log("Choose a file...: %s", m);   // [UIT 0180]
        uit_ov_note(&ov, 1, m);
        return;
    }
    char msg[200];
    int bad = am_scan_path(path, base_name(path), msg, sizeof msg);
    uit_ov_note(&ov, bad, msg);
}
static const char *hk_am_line(char *buf, size_t n, int *bad) {
    *bad = 0;
    int ds = uit_nfp_state() ? uit_nfp_device_state(UIT_NFP_HANDLE) : -1;
    if (ds == UIT_NFP_DS_SEARCHING) { snprintf(buf, n, "The game is asking for an amiibo: scan one now"); return buf; }
    switch (am_have_last ? uit_nfp_scan_result() : 0) {
    case 1: snprintf(buf, n, "%s is on the reader, waiting for the game to ask", am_last.name); return buf;
    case 2: snprintf(buf, n, "The game sees %s", am_last.name); return buf;
    case 3:
        *bad = 1;
        snprintf(buf, n, "The game did not ask for an amiibo in %d s: %s was taken off the reader", UIT_NFP_HOLD_MS / 1000, am_last.name);
        return buf;
    case 4: snprintf(buf, n, "The game read %s", am_last.name); return buf;
    default: snprintf(buf, n, "The game isn't asking for an amiibo right now"); return buf;
    }
}

static const UitOvHooks kHooks = {hk_get, hk_set, hk_why, hk_shown, hk_text, hk_action, hk_choice_ok, uit_cfg_path, hk_dlss_run,
                                  uit_input_apply_binds, hk_am_list, hk_am_file, hk_am_pick, hk_am_scan, hk_am_line, hk_am_last,
                                  hk_am_dir, hk_am_files};

// ---------------------------------------------------------------- start
static const struct { const char *sdl, *shown; } kPadNames[] = {
    {"leftstick", "L3"}, {"rightstick", "R3"}, {"back", "View"}, {"start", "Menu"}, {"guide", "Guide"},
    {"misc1", "Share"}, {"touchpad", "Touchpad"}, {"leftshoulder", "LB"}, {"rightshoulder", "RB"},
    {"a", "A"}, {"b", "B"}, {"x", "X"}, {"y", "Y"}, {"dpup", "D-pad up"}, {"dpdown", "D-pad down"},
    {"dpleft", "D-pad left"}, {"dpright", "D-pad right"},
};

static uint32_t parse_combo(const char *e, char *name, size_t n) {
    uint32_t bits = 0;
    name[0] = 0;
    char buf[128];
    snprintf(buf, sizeof buf, "%s", e);
    for (char *t = strtok(buf, "+ "); t; t = strtok(NULL, "+ ")) {
        SDL_GamepadButton b = SDL_GetGamepadButtonFromString(t);
        if (b == SDL_GAMEPAD_BUTTON_INVALID || b >= 30) {
            fprintf(stderr, "[overlay] TOTK_OVERLAY_PAD: unknown button \"%s\" (ignored)\n", t);
            continue;
        }
        bits |= 1u << b;
        const char *shown = t;
        for (size_t i = 0; i < sizeof kPadNames / sizeof *kPadNames; i++)
            if (!SDL_strcasecmp(kPadNames[i].sdl, t)) shown = kPadNames[i].shown;
        size_t l = strlen(name);
        snprintf(name + l, n - l, "%s%s", l ? " + " : "", shown);
    }
    return bits;
}

// ---------------------------------------------------------------- [UIT 0177] the keyboard dialog's hand-over
// The game's thread (uit_overlay_kb_run) posts the request and waits on kb_cv; the window loop wakes on kb_wake (an SDL
// user event: SDL_PushEvent is thread-safe), shows the dialog in uit_overlay_frame and answers there once it is done.
static pthread_mutex_t kb_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t kb_cv = PTHREAD_COND_INITIALIZER;
static int kb_state;                // under kb_lock: 0 idle, 1 posted, 2 shown, 3 answered
static int kb_alive;                // under kb_lock: a window thread will answer (cleared at shutdown / atlas failure)
static _Atomic int kb_posted;       // the window loop's cheap check
static const UitSwkbdReq *kb_req;   // the game thread's, while it waits
static uint16_t kb_out[UIT_SWKBD_LIMIT + 1];
static int kb_n, kb_ok;
static Uint32 kb_wake;
static int kb_drawn;                // the dialog is (or was, until hidden) in the published quads
static UitOvBox kb_area;            // the IME's area last given to SDL

static void kb_init(void) {
    kb_wake = SDL_RegisterEvents(1);
    pthread_mutex_lock(&kb_lock);
    kb_alive = 1;
    pthread_mutex_unlock(&kb_lock);
}

int uit_overlay_kb_run(const UitSwkbdReq *r, uint16_t *out, int *n) {   // the game's thread
    pthread_mutex_lock(&kb_lock);
    if (!kb_alive || kb_state) { pthread_mutex_unlock(&kb_lock); return -1; }
    kb_req = r;
    kb_state = 1;
    atomic_store(&kb_posted, 1);
    pthread_mutex_unlock(&kb_lock);
    if (kb_wake) {
        SDL_Event e;
        SDL_zero(e);
        e.type = kb_wake;
        SDL_PushEvent(&e);
    }
    pthread_mutex_lock(&kb_lock);
    while (kb_state != 3 && kb_alive) pthread_cond_wait(&kb_cv, &kb_lock);
    int ok = 0;
    *n = 0;
    if (kb_state == 3) {
        ok = kb_ok;
        *n = kb_n;
        memcpy(out, kb_out, (size_t)(kb_n + 1) * 2);
    }
    int answered = kb_state == 3;
    kb_state = 0;
    kb_req = NULL;
    atomic_store(&kb_posted, 0);
    pthread_mutex_unlock(&kb_lock);
    return answered ? ok : -1;
}

static void kb_answer(int ok) {   // window thread: the dialog is done (or cannot be shown)
    pthread_mutex_lock(&kb_lock);
    kb_n = ok ? uit_ov_kb_result(&ov, kb_out) : 0;
    kb_out[kb_n] = 0;
    kb_ok = ok;
    if (kb_state == 1 || kb_state == 2) kb_state = 3;
    pthread_cond_broadcast(&kb_cv);
    pthread_mutex_unlock(&kb_lock);
    if (ov.kb.active) {
        uit_ov_kb_end(&ov, last_phys);
        SDL_StopTextInput(win);
        runtime_input_clear();   // nothing typed reaches the game; window_sdl.c sends the controller again
    }
}

static void kb_abort(void) {   // no window thread to answer any more: the game's thread is let go (and falls back)
    pthread_mutex_lock(&kb_lock);
    kb_alive = 0;
    pthread_cond_broadcast(&kb_cv);
    pthread_mutex_unlock(&kb_lock);
    if (ov.kb.active) {
        uit_ov_kb_end(&ov, last_phys);
        SDL_StopTextInput(win);
    }
}

static void kb_take(uint64_t now) {   // window thread: a request was posted
    pthread_mutex_lock(&kb_lock);
    int take = kb_state == 1 && kb_req;
    if (take) {
        uit_ov_kb_begin(&ov, kb_req, now);
        kb_state = 2;
    }
    atomic_store(&kb_posted, 0);
    pthread_mutex_unlock(&kb_lock);
    if (!take) return;
    kb_area = (UitOvBox){0, 0, 0, 0};
    SDL_StartTextInput(win);   // only now: the IME / on-screen keyboard is off the rest of the time
    runtime_input_clear();     // what the game had held is let go
    fprintf(stderr, "[swkbd] dialog shown\n");
}

static void kb_ime_area(void) {   // where an IME puts its candidate window: under the field (window coordinates)
    if (!memcmp(&kb_area, &ov.kb.field, sizeof kb_area) || ov.kb.field.x1 <= ov.kb.field.x0) return;
    kb_area = ov.kb.field;
    float d = SDL_GetWindowPixelDensity(win);
    if (d <= 0) d = 1;
    SDL_Rect r = {(int)(kb_area.x0 / d), (int)(kb_area.y0 / d), (int)((kb_area.x1 - kb_area.x0) / d),
                  (int)((kb_area.y1 - kb_area.y0) / d)};
    SDL_SetTextInputArea(win, &r, 0);
}

// The game sends no frames while its thread waits in ShowKeyboard: the dialog's changes are shown on the last picture.
static void kb_repaint(uint64_t now) {
    static uint64_t frames, at;
    uint64_t f = vkp_frames();
    if (f != frames) { frames = f; at = now; return; }
    if (now - at >= 50) vkp_repaint();
}

static int kb_key(const SDL_KeyboardEvent *k) {   // the dialog is up: every key is its own but F7-F11
    if (k->scancode >= SDL_SCANCODE_F7 && k->scancode <= SDL_SCANCODE_F11) return 0;
    if (!k->down || ov.kb.pre[0]) return 1;   // an IME composing gets Enter / Backspace / the arrows itself
    switch (k->scancode) {
    case SDL_SCANCODE_RETURN: case SDL_SCANCODE_KP_ENTER:
        if (!k->repeat) uit_ov_kb_edit(&ov, (k->mod & SDL_KMOD_SHIFT) && ov.kb.req.newline ? UIT_KBE_NEWLINE : UIT_KBE_OK);
        break;
    case SDL_SCANCODE_ESCAPE: if (!k->repeat) uit_ov_kb_edit(&ov, UIT_KBE_CANCEL); break;
    case SDL_SCANCODE_BACKSPACE: uit_ov_kb_edit(&ov, UIT_KBE_BACKSPACE); break;
    case SDL_SCANCODE_DELETE: uit_ov_kb_edit(&ov, UIT_KBE_DELETE); break;
    case SDL_SCANCODE_LEFT: uit_ov_kb_edit(&ov, UIT_KBE_LEFT); break;
    case SDL_SCANCODE_RIGHT: uit_ov_kb_edit(&ov, UIT_KBE_RIGHT); break;
    case SDL_SCANCODE_HOME: case SDL_SCANCODE_UP: uit_ov_kb_edit(&ov, UIT_KBE_HOME); break;
    case SDL_SCANCODE_END: case SDL_SCANCODE_DOWN: uit_ov_kb_edit(&ov, UIT_KBE_END); break;
    case SDL_SCANCODE_V:
        if (k->mod & (SDL_KMOD_CTRL | SDL_KMOD_GUI)) {
            char *c = SDL_GetClipboardText();
            if (c) uit_ov_kb_text(&ov, c);   // one line: a newline in it is refused unless the game allows them
            SDL_free(c);
        }
        break;
    default: break;
    }
    return 1;
}

int uit_overlay_text(const SDL_Event *e) {
    if (!enabled || !ov.kb.active) return 0;
    if (e->type == SDL_EVENT_TEXT_INPUT) uit_ov_kb_text(&ov, e->text.text);
    else if (e->type == SDL_EVENT_TEXT_EDITING) uit_ov_kb_compose(&ov, e->edit.text);
    else return 0;
    return 1;
}

void uit_overlay_init(SDL_Window *w, void (*fullscreen)(void), void (*mark)(void)) {
    win = w;
    fullscreen_cb = fullscreen;
    mark_cb = mark;
    const char *e = getenv("TOTK_OVERLAY");
    enabled = 1;
    kb_init();   // [UIT 0177]
    if (e && !strcmp(e, "0")) {
        menu_off = 1;
        uit_ov_init(&ov, &kHooks, 0, 1000);
        fprintf(stderr, "[overlay] off (TOTK_OVERLAY=0): only the game's keyboard dialog\n");
        return;
    }
    e = getenv("TOTK_OVERLAY_KEY");
    if (e && *e) {
        SDL_Scancode s = SDL_GetScancodeFromName(e);
        if (s == SDL_SCANCODE_UNKNOWN || (s >= SDL_SCANCODE_F7 && s <= SDL_SCANCODE_F11))
            fprintf(stderr, "[overlay] TOTK_OVERLAY_KEY=%s is not a free key: F1\n", e);
        else {
            hotkey = s;
            snprintf(hotkey_name, sizeof hotkey_name, "%s", SDL_GetScancodeName(s));
        }
    }
    uint32_t combo = 1u << SDL_GAMEPAD_BUTTON_LEFT_STICK | 1u << SDL_GAMEPAD_BUTTON_RIGHT_STICK, combo2 = 0;
    e = getenv("TOTK_OVERLAY_PAD");
    if (e && !strcmp(e, "none")) {
        combo = 0;
        snprintf(combo_name, sizeof combo_name, "(no controller combo)");
    } else if (e && *e) {
        // [UIT 0271] "a+b,guide": up to two combos, either one opens it (Game > In-game overlay > Overlay controller combo)
        char buf[128], nm[64];
        uint32_t cs[2] = {0, 0};
        int n = 0;
        snprintf(buf, sizeof buf, "%s", e);
        combo_name[0] = 0;
        for (char *part = buf, *next; part; part = next) {   // (strtok is parse_combo's)
            next = strchr(part, ',');
            if (next) *next++ = 0;
            uint32_t c = parse_combo(part, nm, sizeof nm);
            if (!c) continue;
            if (n == 2) { fprintf(stderr, "[overlay] TOTK_OVERLAY_PAD: only two combos are read (\"%s\" ignored)\n", part); continue; }
            cs[n++] = c;
            size_t l = strlen(combo_name);
            snprintf(combo_name + l, sizeof combo_name - l, "%s%s", l ? " or " : "", nm);
        }
        if (n) { combo = cs[0]; combo2 = n > 1 ? cs[1] : 0; }
        else {
            snprintf(combo_name, sizeof combo_name, "L3 + R3");
            fprintf(stderr, "[overlay] TOTK_OVERLAY_PAD=%s has no button: L3 + R3\n", e);
        }
    }
    uint32_t hold = 1000;
    e = getenv("TOTK_OVERLAY_HOLD_MS");
    if (e && *e) {
        long v = strtol(e, NULL, 10);
        hold = v < 300 ? 300 : v > 3000 ? 3000 : (uint32_t)v;
    }
    uit_ov_init(&ov, &kHooks, combo, hold);
    uit_ov_set_combo2(&ov, combo2);
    // [UIT 0270] Game > Controller > Confirm button: game = the right face button confirms in the overlay too
    e = getenv("TOTK_UI_CONFIRM");
    ov.confirm_game = e && !strcmp(e, "game");
    uit_ov_set_binds(&ov, uit_input_binds());   // window_sdl.c's input_map_init ran first
    e = getenv("TOTK_OVERLAY_FPS");
    ov.show_fps = e && *e && strcmp(e, "0");
    fprintf(stderr, "[overlay] %s, or hold %s for %.1f s\n", hotkey_name, combo ? combo_name : "nothing", hold / 1000.0);
}

// ---------------------------------------------------------------- input
static void opened_or_closed(int was_open) {
    // opening: the keys and buttons held are let go for the game; closing: nothing pressed while it was open (or the
    // key / button that closed it) reaches the game -- window_sdl.c then sends the controller's state again
    if (ov.open != was_open) runtime_input_clear();
}

static void set_open(int open) {
    int was = ov.open;
    uit_ov_set_open(&ov, open, last_phys);
    opened_or_closed(was);
}

// [UIT 0174] a key while the Controls tab waits for a press: nothing reaches the game or the window's own keys
static void capture_key(const SDL_KeyboardEvent *k) {
    static const char *const kWindowKeys[5] = {"marks the log", "turns FSR 1 on or off", "changes the sharpening",
                                               "turns DLSS on or off", "switches fullscreen"};
    if (!k->down || k->repeat) return;   // the launcher's dialog: a held key does not bind again
    if (k->scancode == SDL_SCANCODE_ESCAPE) { uit_ov_capture_cancel(&ov, "Cancelled"); return; }
    if (k->scancode == SDL_SCANCODE_DELETE) { uit_ov_capture_clear(&ov); return; }
    if (uit_ov_capturing(&ov) != 1 + UIT_BIND_KEY) return;   // waiting for the controller
    if (k->scancode >= SDL_SCANCODE_F7 && k->scancode <= SDL_SCANCODE_F11) {
        char m[160];
        snprintf(m, sizeof m, "%s %s, so it cannot press a button. Pick another key.", SDL_GetScancodeName(k->scancode),
                 kWindowKeys[k->scancode - SDL_SCANCODE_F7]);
        uit_ov_capture_refuse(&ov, m);
        return;
    }
    uit_ov_capture_key(&ov, (int)k->scancode, SDL_GetScancodeName(k->scancode));
}

int uit_overlay_key(const SDL_KeyboardEvent *k) {
    if (!enabled) return 0;
    if (ov.kb.active) return kb_key(k);   // [UIT 0177]
    if (menu_off) return 0;
    if (k->scancode == hotkey) {   // also while capturing: closing ends the capture with nothing changed
        if (k->down && !k->repeat) set_open(!ov.open);
        return 1;
    }
    if (!ov.open) return 0;
    if (uit_ov_capturing(&ov)) { capture_key(k); return 1; }
    if (k->scancode >= SDL_SCANCODE_F7 && k->scancode <= SDL_SCANCODE_F11) return 0;   // window_sdl.c's keys still work
    if (!k->down) return 1;
    static int rep;
    rep = k->repeat ? rep + 1 : 0;
    int was = ov.open;
    ov.nav_rep = rep;   // held arrows step faster after a while, as on the controller
    switch (k->scancode) {
    case SDL_SCANCODE_UP: case SDL_SCANCODE_W: uit_ov_input(&ov, OVIN_UP); break;
    case SDL_SCANCODE_DOWN: case SDL_SCANCODE_S: uit_ov_input(&ov, OVIN_DOWN); break;
    case SDL_SCANCODE_LEFT: case SDL_SCANCODE_A: uit_ov_input(&ov, OVIN_LEFT); break;
    case SDL_SCANCODE_RIGHT: case SDL_SCANCODE_D: uit_ov_input(&ov, OVIN_RIGHT); break;
    case SDL_SCANCODE_RETURN: case SDL_SCANCODE_KP_ENTER: case SDL_SCANCODE_SPACE:
        if (!k->repeat) uit_ov_input(&ov, OVIN_CONFIRM);
        break;
    case SDL_SCANCODE_ESCAPE: case SDL_SCANCODE_BACKSPACE: if (!k->repeat) set_open(0); break;
    case SDL_SCANCODE_Q: case SDL_SCANCODE_PAGEUP: if (!k->repeat) uit_ov_input(&ov, OVIN_TAB_PREV); break;
    case SDL_SCANCODE_E: case SDL_SCANCODE_PAGEDOWN: if (!k->repeat) uit_ov_input(&ov, OVIN_TAB_NEXT); break;
    case SDL_SCANCODE_TAB: if (!k->repeat) uit_ov_input(&ov, (k->mod & SDL_KMOD_SHIFT) ? OVIN_TAB_PREV : OVIN_TAB_NEXT); break;
    case SDL_SCANCODE_DELETE: if (!k->repeat) uit_ov_input(&ov, OVIN_RESET); break;   // Controls: the default again
    default: break;
    }
    opened_or_closed(was);
    return 1;   // every other key too: the game sees no input while the overlay is open
}

int uit_overlay_mouse(const SDL_Event *e) {
    if (!enabled || (!ov.open && !ov.kb.active)) return 0;
    float d = SDL_GetWindowPixelDensity(win);
    if (d <= 0) d = 1;
    if (ov.kb.active) {   // [UIT 0177] a key is clicked (pointing at one focuses it)
        if (e->type == SDL_EVENT_MOUSE_MOTION) uit_ov_mouse(&ov, e->motion.x * d, e->motion.y * d, 0, 0);
        else if (e->type == SDL_EVENT_MOUSE_BUTTON_DOWN && e->button.button == SDL_BUTTON_LEFT)
            uit_ov_mouse(&ov, e->button.x * d, e->button.y * d, 1, 0);
        return 1;
    }
    switch (e->type) {
    case SDL_EVENT_MOUSE_MOTION: uit_ov_mouse(&ov, e->motion.x * d, e->motion.y * d, 0, 0); break;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
        if (e->button.button == SDL_BUTTON_LEFT) uit_ov_mouse(&ov, e->button.x * d, e->button.y * d, 1, 0);
        else if (e->button.button == SDL_BUTTON_RIGHT) {
            if (uit_ov_capturing(&ov)) uit_ov_capture_cancel(&ov, "Cancelled");   // ends the capture, not the overlay
            else set_open(0);
        }
        break;
    case SDL_EVENT_MOUSE_BUTTON_UP:
        if (e->button.button == SDL_BUTTON_LEFT) uit_ov_mouse(&ov, e->button.x * d, e->button.y * d, 2, 0);
        break;
    case SDL_EVENT_MOUSE_WHEEL:
        if (e->wheel.y != 0) uit_ov_mouse(&ov, e->wheel.mouse_x * d, e->wheel.mouse_y * d, 0, e->wheel.y);
        break;
    default: return 0;
    }
    return 1;
}

uint32_t uit_overlay_pad(SDL_Gamepad *pad) {
    if (!enabled || (menu_off && !ov.kb.active)) return 0;
    uint32_t phys = 0, nav = 0;
    if (pad) {
        for (int b = 0; b < SDL_GAMEPAD_BUTTON_COUNT && b < 30; b++)
            if (SDL_GetGamepadButton(pad, (SDL_GamepadButton)b)) phys |= 1u << b;
        int thr = uit_trigger_threshold_now();   // [UIT 0291] the same press point the game gets
        if (SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) > thr) phys |= 1u << 30;
        if (SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) > thr) phys |= 1u << 31;
        if (ov.open || ov.kb.active) {
            int lx = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTX), ly = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTY);
            int ax = abs(lx), ay = abs(ly);
            if (phys & 1u << SDL_GAMEPAD_BUTTON_DPAD_UP || (ly < -20000 && ay > ax)) nav |= OVNAV_UP;
            if (phys & 1u << SDL_GAMEPAD_BUTTON_DPAD_DOWN || (ly > 20000 && ay > ax)) nav |= OVNAV_DOWN;
            if (phys & 1u << SDL_GAMEPAD_BUTTON_DPAD_LEFT || (lx < -20000 && ax >= ay)) nav |= OVNAV_LEFT;
            if (phys & 1u << SDL_GAMEPAD_BUTTON_DPAD_RIGHT || (lx > 20000 && ax >= ay)) nav |= OVNAV_RIGHT;
            // [UIT 0270] by position, as Game > Controller > Confirm button (TOTK_UI_CONFIRM) says (the launcher's own)
            nav |= uit_ov_face_nav(phys, ov.confirm_game);
            if (phys & 1u << SDL_GAMEPAD_BUTTON_LEFT_SHOULDER) nav |= OVNAV_TABL;
            if (phys & 1u << SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER) nav |= OVNAV_TABR;
            // [UIT 0177] the keyboard dialog's own (Y: uit_ov_face_nav, the top button)
            if (phys & 1u << SDL_GAMEPAD_BUTTON_START) nav |= OVNAV_PLUS;
            if (phys & 1u << SDL_GAMEPAD_BUTTON_BACK) nav |= OVNAV_MINUS;
            if (phys & 1u << SDL_GAMEPAD_BUTTON_LEFT_STICK) nav |= OVNAV_L3;
        }
    }
    last_phys = phys;
    if (ov.open) {   // the Controls tab: is there a controller to bind, and whose labels to show (launcher main.cpp)
        int fam = ov.pad_family;
        if (pad) switch (SDL_GetGamepadType(pad)) {
            case SDL_GAMEPAD_TYPE_XBOX360: case SDL_GAMEPAD_TYPE_XBOXONE: fam = UIT_PADFAM_XBOX; break;
            case SDL_GAMEPAD_TYPE_PS3: case SDL_GAMEPAD_TYPE_PS4: case SDL_GAMEPAD_TYPE_PS5: fam = UIT_PADFAM_PS; break;
            case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO: case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_PAIR: fam = UIT_PADFAM_NINTENDO; break;
            default: fam = UIT_PADFAM_GENERIC; break;
            }
        if (fam != ov.pad_family || ov.pad_present != (pad != NULL)) {
            ov.pad_family = fam;
            ov.pad_present = pad != NULL;
            ov.dirty = 1;
            if (!pad && uit_ov_capturing(&ov) == 1 + UIT_BIND_PAD) uit_ov_capture_cancel(&ov, "The controller was disconnected");
        }
    }
    int was = ov.open;
    uint32_t mask = uit_ov_pad(&ov, phys, nav, SDL_GetTicks());
    opened_or_closed(was);
    return mask;
}

int uit_overlay_open(void) { return enabled && (ov.open || ov.kb.active); }
int uit_overlay_busy(void) { return enabled && (ov.open || ov.kb.active || ov.cstate == OVC_HOLD); }
int uit_overlay_wait_ms(int idle_ms) { return uit_overlay_busy() ? 16 : idle_ms; }

// ---------------------------------------------------------------- drawing
static void fps_update(uint64_t now) {
    static uint64_t t0, f0;
    uint64_t f = vkp_frames();
    if (!t0) { t0 = now; f0 = f; return; }
    if (now - t0 < 500) return;
    fps = (double)(f - f0) * 1000.0 / (double)(now - t0);
    t0 = now;
    f0 = f;
}

static void hide(void) {
    if (atomic_load_explicit(&g_uit_ov_visible, memory_order_relaxed)) atomic_store(&g_uit_ov_visible, 0);
    last_n = -1;
    if (kb_drawn) {   // [UIT 0177] the dialog's last picture goes too, also if the game is not sending frames yet
        kb_drawn = 0;
        vkp_repaint();
    }
}

void uit_overlay_frame(void) {
    if (!enabled) return;
    uint64_t now = SDL_GetTicks();
    // [UIT 0177] the keyboard dialog: a request from the game's thread, an answer once it is done
    if (atomic_load_explicit(&kb_posted, memory_order_relaxed)) kb_take(now);
    if (ov.kb.active && ov.kb.done) kb_answer(ov.kb.ok);
    if (menu_off && !ov.kb.active) { if (kb_drawn) hide(); return; }
    if (am_have_last) runtime_nfp_tick();   // [UIT 0175] the reader's timers (removal, the wait running out) once used
    am_pick_poll();
    uit_ov_tick(&ov, now);
    if (ov.show_fps) fps_update(now);
    if (!ov.open && !ov.kb.active && !ov.show_fps && uit_ov_combo_progress(&ov, now) <= 0) { hide(); return; }
    int pw = 0, ph = 0;
    SDL_GetWindowSizeInPixels(win, &pw, &ph);
    if (pw <= 0 || ph <= 0) { hide(); return; }
    float s = uit_ov_scale((float)pw, (float)ph);
    uint8_t *old = NULL;
    if (!atlas.pix || atlas.scale != s) {   // the first time, and when the window changes size a lot
        uint64_t t0 = runtime_now_ns();
        UitOvAtlas na;
        if (!uit_ov_atlas_build(&na, kInterRegular, kInterSemiBold, s)) {
            fprintf(stderr, "[overlay] font atlas failed: overlay off\n");
            kb_abort();   // [UIT 0177] a waiting keyboard request is let go (swkbd_host.c falls back)
            enabled = 0;
            hide();
            return;
        }
        old = atlas.pix;   // freed once the present thread can no longer see it (below, after the hand-over)
        atlas = na;
        fprintf(stderr, "[overlay] font atlas %dx%d at %.3gx in %.1f ms\n", atlas.w, atlas.h, s, (runtime_now_ns() - t0) / 1e6);
        last_n = -1;
    }
    UitOvFrame f = {(float)pw, (float)ph, now, fps, hotkey_name, ov.combo ? combo_name : NULL};
    int n = uit_ov_build(&ov, &atlas, &f, build_q, UIT_OV_MAX_QUADS);
    if (ov.kb.active) kb_ime_area();
    if (!n && !old) { hide(); return; }
    if (!old && n == last_n && !memcmp(build_q, share_q, (size_t)n * sizeof *build_q) &&
        atomic_load_explicit(&g_uit_ov_visible, memory_order_relaxed)) {
        if (ov.kb.active) kb_repaint(now);   // [UIT 0177] counts the game's frames
        return;   // nothing changed (the FPS counter between its updates)
    }
    // the quads and the atlas they were laid out with change together (one lock), so no frame mixes the two
    uit_ov_share_lock();
    if (old || share.pix != atlas.pix) {
        share.pix = atlas.pix;
        share.aw = atlas.w;
        share.ah = atlas.h;
        share.atlas_gen++;
    }
    memcpy(share_q, build_q, (size_t)n * sizeof *build_q);
    share.n = n;
    share.seq++;
    uit_ov_share_unlock();
    free(old);   // the present thread reads the atlas only under the lock (overlay_vk.c copies it out)
    if (!n) { hide(); return; }
    last_n = n;
    atomic_store_explicit(&g_uit_ov_visible, 1, memory_order_release);
    if (ov.kb.active) { kb_drawn = 1; kb_repaint(now); }
}

void uit_overlay_shutdown(void) {
    if (!enabled) return;
    kb_abort();   // [UIT 0177]
    if (!menu_off) uit_ov_flush(&ov, SDL_GetTicks());
    atomic_store(&g_uit_ov_visible, 0);
    uit_ov_share_lock();
    share.pix = NULL;
    share.n = 0;
    share.atlas_gen++;
    uit_ov_share_unlock();
    uit_ov_atlas_free(&atlas);
    enabled = 0;
}
