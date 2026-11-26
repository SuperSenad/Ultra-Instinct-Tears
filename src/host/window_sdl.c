// The window (milestone 3.2): SDL3 on the process's main thread — window, keyboard, game controllers with rumble —
// while the game runs on its own thread (boot.c). The picture goes through src/vk/present.c.
//  - keyboard: SDL scancodes become the Mac virtual key codes the controller emulation of overrides_input.c (from
//    MacNativeGames) expects, so the key layout is theirs (J/K/I/U = A/B/X/Y, WASD + arrows = sticks, ...);
//  - game controllers: buttons by position as on a Switch controller (the right face button is A), sticks with Y up;
//  - rumble: the game's vibration values (hid_feedback.c) drive the controller's low / high frequency motors.
//  - motion: the controller's gyro and accelerometer feed motion_fusion.c (gyro aiming); F11 / Alt+Enter: fullscreen.
//  - [UIT 0173] the in-game overlay (overlay_live.c): its hotkey (F1) / controller hold combo (L3 + R3 for 1 s) open
//    it; while it is open it takes the keyboard, the mouse and the controller, and the game sees no input at all.
//  - [UIT 0451] the mouse camera (mouse_aim.h): while the overlay is closed the mouse is captured (relative mode: raw
//    counts, no pointer acceleration, no cursor) and turns the camera; its buttons press Switch buttons. [UIT 0454] With
//    the bow drawn it turns the motion sensor instead, so the game's motion aiming follows it exactly.
#include <SDL3/SDL.h>
#define SDL_MAIN_HANDLED   // main() is boot.c's; SDL only gets SDL_SetMainReady
#include <SDL3/SDL_main.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "host.h"
#include "input_map.h"
#include "input_bind.h"   // [UIT 0174] the bindings, changed live by the overlay
#include "../vk/present.h"
#include "../vk/present_fsr.h"
#include "../vk/dlss.h"
#include "uit_icon.h"   // build/: assets/uit-256.png (tools/build_game.py)
#include "save_txn.h"   // [UIT 0080] uit_save_drain
#include "screenshot.h"   // [UIT 0346]
#include "overlay_live.h"   // [UIT 0173] the in-game overlay
#include "stick_dz.h"   // [UIT 0290] stick deadzones, [UIT 0291] trigger press point
#include "mouse_aim.h"  // [UIT 0451] the mouse camera's limits
#include "uit_live.h"
#include "../gfx/vkb_capture.h"   // [UIT 0157] Shift+F7: a frame capture for diagnosis
#include "../runtime/platform.h"   // [UIT 0293] plat_terminate_code, plat_restart_self
#include "game_exit.h"   // [UIT 0294] the restart exit status
#include "../gfx/uit_frametime.h"   // [UIT 0303] the frame rate cap in the background
#include "bench_live.h"   // [UIT 0302] Shift+F8: the benchmark
#include "prompts.h"   // [UIT 0452] Button prompts = Auto asks which controller is connected

static SDL_Window *window;
static SDL_Gamepad *pad;
static SDL_Mutex *pad_lock;

// The Mac virtual key code the game's keyboard input uses for an SDL scancode (input_map.h), or -1.
static int mac_key(SDL_Scancode s) {
    int i = uit_key_from_scancode(s);
    return i < 0 ? -1 : kUitKeys[i].mac;
}

// 0029: the controller and keyboard bindings, from TOTK_PAD_<button> / TOTK_KEY_<button> (input_map.h). Each controller
// input or key can press several Switch buttons only if the cfg says so by hand; the launcher keeps them one-to-one.
// [UIT 0174] input_bind.c reads them, and the overlay's Controls tab changes them while the game runs: the new tables
// are built in the spare slot and published with one release store; pad_update reads the pointer with one acquire
// load per update (no lock). The key table goes through runtime_input_set_keymap (a pointer swap, overrides_input.c).
static UitBinds binds;
static uint64_t pad_slots[2][UIT_PAD_INPUT_COUNT], key_bits[128];
static const uint64_t *_Atomic pad_bits_live = pad_slots[0];
static int pad_slot;

const UitBinds *uit_input_binds(void) { return &binds; }
void uit_input_apply_binds(const UitBinds *b) {   // window thread (the overlay's hooks run there, as pad_update)
    if (b != &binds) binds = *b;
    pad_slot ^= 1;
    uit_binds_compile(&binds, pad_slots[pad_slot], key_bits);
    atomic_store_explicit(&pad_bits_live, pad_slots[pad_slot], memory_order_release);
    runtime_input_set_keymap(key_bits);   // copied there into its spare buffer
}
static const char *env_get(const char *k) { return getenv(k); }
static void input_map_init(void) {
    uit_binds_load(&binds, env_get, 1);
    uit_input_apply_binds(&binds);
}

// [UIT 0290] Game > Controller > Left / Right stick deadzone (TOTK_STICK_DEADZONE_L / _R, 0..50 %) and Game > More
// controller settings > Stick outer deadzone (TOTK_STICK_OUTER, 0..30 %); [UIT 0291] Game > More controller settings >
// Trigger press point (TOTK_TRIGGER_POINT, 5..95 %, 50 = the old fixed half way). Read at start, changed live by the
// overlay (its hooks run on this thread, as pad_update; atomics anyway, relaxed: one load each per poll).
static _Atomic int dz_pct[3];                 // left inner, right inner, outer
static _Atomic int trig_pct = 50, trig_thr = 16384;
static int env_pct(const char *k, int def, int lo, int hi) {
    const char *e = getenv(k);
    char *end;
    if (!e || !*e) return def;
    long v = strtol(e, &end, 10);
    if (end == e || *end || v < lo || v > hi) { fprintf(stderr, "[totk] %s=%s is not %d..%d: %d\n", k, e, lo, hi, def); return def; }
    return (int)v;
}
static void stick_dz_init(void) {
    atomic_store(&dz_pct[0], env_pct("TOTK_STICK_DEADZONE_L", 0, 0, UIT_STICK_DZ_MAX));
    atomic_store(&dz_pct[1], env_pct("TOTK_STICK_DEADZONE_R", 0, 0, UIT_STICK_DZ_MAX));
    atomic_store(&dz_pct[2], env_pct("TOTK_STICK_OUTER", 0, 0, UIT_STICK_OUTER_MAX));
    uit_set_trigger_point(env_pct("TOTK_TRIGGER_POINT", 50, 5, 95));
    fprintf(stderr, "[totk] [UIT 0290] sticks: deadzone left %d%%, right %d%%, outer %d%%; triggers press at %d%%\n",
            atomic_load(&dz_pct[0]), atomic_load(&dz_pct[1]), atomic_load(&dz_pct[2]), atomic_load(&trig_pct));
}
int uit_stick_deadzone(int which) { return which >= 0 && which < 3 ? atomic_load_explicit(&dz_pct[which], memory_order_relaxed) : 0; }
void uit_set_stick_deadzone(int which, int pct) {
    int hi = which == 2 ? UIT_STICK_OUTER_MAX : UIT_STICK_DZ_MAX;
    if (which < 0 || which > 2) return;
    atomic_store_explicit(&dz_pct[which], pct < 0 ? 0 : pct > hi ? hi : pct, memory_order_relaxed);
}
int uit_trigger_point(void) { return atomic_load_explicit(&trig_pct, memory_order_relaxed); }
int uit_trigger_threshold_now(void) { return atomic_load_explicit(&trig_thr, memory_order_relaxed); }
void uit_set_trigger_point(int pct) {
    pct = pct < 5 ? 5 : pct > 95 ? 95 : pct;
    atomic_store_explicit(&trig_pct, pct, memory_order_relaxed);
    atomic_store_explicit(&trig_thr, uit_trigger_threshold(pct), memory_order_relaxed);
}

// [UIT 0451] The mouse camera. Game > Mouse: TOTK_MOUSE (0 = off), TOTK_MOUSE_SENS (multiplier, 0.1..5),
// TOTK_MOUSE_AIM_SENS (while ZR is held, i.e. drawing a bow: a multiplier of the camera's; unset = the same),
// TOTK_MOUSE_INVERT_Y (1), TOTK_MOUSE_MIN (0..30 %: the smallest push a moving mouse gives, for a game deadzone),
// TOTK_MOUSE_CURVE (1..3: undoes a game response curve; 1 = off) and [UIT 0454] TOTK_MOUSE_POSE (- automatic: upright
// in handheld mode, else flat; flat; upright: how the controller the mouse turns for the bow is held). The overlay
// changes the first four while the game runs (window thread, as these). The counts go to overrides_input.c
// (runtime_mouse_motion), which makes the stick.
// [UIT 0454] - the bow: while ZR is held (keys, mouse buttons or the controller) and the game has its motion sensor,
//   the counts are an exact angle for the sensor instead (motion_fusion.c runtime_motion_mouse), at the camera's
//   sensitivity times the bow's; the stick gets none of them. Without the sensor the bow aims through the stick.
// - the mouse buttons are keys of the bindings (input_map.h MouseLeft ...; by default left = Y, right = ZR): pressed
//   for the game only while the mouse is captured; whatever is down when it is let go (the overlay, the focus) is let
//   go for the game too, and the click that brings the window back is not a press.
#define NPAD_ZR 512u
int runtime_handheld_mode(void);
static int m_on = 1, m_invert, m_min_pct, m_captured, m_pose;
static double m_sens = 1.0, m_aim = -1, m_curve = 1.0;   // m_aim < 0: the camera's
static unsigned m_held;      // bit b - 1: SDL mouse button b (1 left, 2 middle, 3 right, 4 back, 5 forward) is down for the game
static Uint64 m_focus_ms;    // focus came back at (a click that brings the window back is not a press)
static double env_num(const char *k, double def, double lo, double hi) {
    const char *e = getenv(k);
    char *end;
    if (!e || !*e || !strcmp(e, "-")) return def;
    double v = strtod(e, &end);
    if (end == e || *end || !(v >= lo && v <= hi)) { fprintf(stderr, "[totk] %s=%s is not %g..%g: %g\n", k, e, lo, hi, def); return def; }
    return v;
}
static void mouse_push(void) {
    runtime_mouse_setup(m_on, m_sens, m_aim, m_invert, m_min_pct / 100.0, m_curve);
    runtime_motion_mouse_enable(m_on && runtime_gyro_enabled(), m_pose);   // [UIT 0454] the bow's sensor
}
static void mouse_button(int b, int down) {   // [UIT 0454] a mouse button as the key it is bound as
    int k = uit_key_from_mouse(b);
    if (k < 0) return;
    unsigned bit = 1u << (b - 1);
    if (down == !!(m_held & bit)) return;
    m_held ^= bit;
    runtime_input_key((unsigned)kUitKeys[k].mac, down);
}
static void mouse_release_all(void) {
    for (int b = 1; b <= 5; b++) mouse_button(b, 0);
}
static void mouse_capture_sync(void) {   // captured while on and the overlay is closed (SDL lets go while unfocused)
    int want = m_on && window && !uit_overlay_open();
    if (want == m_captured) return;
    if (!SDL_SetWindowRelativeMouseMode(window, want)) {
        fprintf(stderr, "[totk] [UIT 0451] mouse capture: %s\n", SDL_GetError());
        want = 0;
    }
    m_captured = want;
    if (!want) mouse_release_all();
}
static void mouse_init(void) {
    const char *e = getenv("TOTK_MOUSE");
    m_on = !(e && !strcmp(e, "0"));
    m_sens = env_num("TOTK_MOUSE_SENS", 1.0, 0.1, 5.0);
    m_aim = env_num("TOTK_MOUSE_AIM_SENS", -1, 0.1, 2.0);
    e = getenv("TOTK_MOUSE_INVERT_Y");
    m_invert = e && !strcmp(e, "1");
    m_min_pct = (int)env_num("TOTK_MOUSE_MIN", 0, 0, UIT_MOUSE_MIN_PUSH_MAX);
    m_curve = env_num("TOTK_MOUSE_CURVE", 1.0, 1.0, UIT_MOUSE_CURVE_MAX);
    e = getenv("TOTK_MOUSE_POSE");
    m_pose = e && !strcmp(e, "upright") ? 1 : e && !strcmp(e, "flat") ? 0 : runtime_handheld_mode() ? 1 : 0;
    mouse_push();
    char a[32] = "the camera's";
    if (m_aim > 0) snprintf(a, sizeof a, "%.2fx", m_aim);
    fprintf(stderr, "[totk] [UIT 0451] mouse camera %s: sensitivity %.2f, bow aiming %s, invert %s, slow movement boost %d%%, "
            "fine aim correction %.2f; [UIT 0454] the bow %s\n", m_on ? "on" : "off", m_sens, a, m_invert ? "on" : "off",
            m_min_pct, m_curve, !runtime_gyro_enabled() ? "through the stick (motion controls off)" : m_pose ? "through the motion sensor, upright" : "through the motion sensor, flat");
}
static void mouse_event(const SDL_Event *e) {   // the overlay did not take it
    if (!m_captured) return;
    if (e->type == SDL_EVENT_MOUSE_MOTION) {
        if (e->motion.which == SDL_TOUCH_MOUSEID || (e->motion.xrel == 0 && e->motion.yrel == 0)) return;
        // [UIT 0454] the bow drawn: an exact angle for the game's motion aiming (the sensor's own setting can be off live)
        if ((runtime_input_held() & NPAD_ZR) && runtime_motion_mouse_ready() && runtime_hid_feedback_on("gyro", NULL)) {
            double yaw, pitch, sens = m_sens * (m_aim > 0 ? m_aim : 1.0);
            uit_mouse_aim_angles(e->motion.xrel, e->motion.yrel, sens, m_invert, &yaw, &pitch);
            runtime_motion_mouse(yaw, pitch);
        } else runtime_mouse_motion(e->motion.xrel, e->motion.yrel);
        return;
    }
    if (e->type != SDL_EVENT_MOUSE_BUTTON_DOWN && e->type != SDL_EVENT_MOUSE_BUTTON_UP) return;
    if (e->button.which == SDL_TOUCH_MOUSEID) return;
    if (e->type == SDL_EVENT_MOUSE_BUTTON_DOWN && SDL_GetTicks() - m_focus_ms < 300) return;   // the click that brought the window back
    mouse_button(e->button.button, e->type == SDL_EVENT_MOUSE_BUTTON_DOWN);
}
int uit_mouse_on(void) { return m_on; }
void uit_set_mouse_on(int on) { m_on = !!on; mouse_push(); mouse_capture_sync(); }
double uit_mouse_sens(void) { return m_sens; }
void uit_set_mouse_sens(double v) { m_sens = v < 0.1 ? 0.1 : v > 5.0 ? 5.0 : v; mouse_push(); }
double uit_mouse_aim_sens(void) { return m_aim; }
void uit_set_mouse_aim_sens(double v) { m_aim = v < 0.1 ? -1 : v > 2.0 ? 2.0 : v; mouse_push(); }
int uit_mouse_invert(void) { return m_invert; }
void uit_set_mouse_invert(int on) { m_invert = !!on; mouse_push(); }

// Switch npad button bits (libnx HidNpadButton): A B X Y StickL StickR L R ZL ZR Plus Minus Left Up Right Down
static int32_t stick(Sint16 v, int invert) {
    int32_t x = invert ? -(int32_t)v : v;
    return x > 32767 ? 32767 : x < -32767 ? -32767 : x;
}
// [UIT 0173] the overlay's mask (uit_overlay_pad): ~0 while it is open (the game gets a connected controller at rest),
// else the buttons of the hold combo while they are held or latched (bit = SDL button, 30 / 31 = left / right trigger)
static void pad_update(void) {
    RuntimeGamepadState s = {0};
    uint32_t mask = uit_overlay_pad(pad);
    if (pad && mask == ~0u) {
        s.connected = 1;
    } else if (pad) {
        const uint64_t *pad_bits = atomic_load_explicit(&pad_bits_live, memory_order_acquire);
        int thr = atomic_load_explicit(&trig_thr, memory_order_relaxed);
        for (int i = 0; i < UIT_PAD_INPUT_COUNT; i++) {
            if (!pad_bits[i]) continue;
            const UitPadInput *in = &kUitPadInputs[i];
            if (mask & uit_bind_pad_phys(i)) continue;
            int down = in->button >= 0 ? SDL_GetGamepadButton(pad, (SDL_GamepadButton)in->button)
                                       : SDL_GetGamepadAxis(pad, (SDL_GamepadAxis)in->axis) > thr;
            if (down) s.buttons |= pad_bits[i];
        }
        s.lx = stick(SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTX), 0);
        s.ly = stick(SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTY), 1);
        s.rx = stick(SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHTX), 0);
        s.ry = stick(SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHTY), 1);
        int dl = atomic_load_explicit(&dz_pct[0], memory_order_relaxed), dr = atomic_load_explicit(&dz_pct[1], memory_order_relaxed),
            dout = atomic_load_explicit(&dz_pct[2], memory_order_relaxed);
        if (dl | dout) uit_stick_dz(&s.lx, &s.ly, dl / 100.f, dout / 100.f);   // [UIT 0290] all 0: untouched
        if (dr | dout) uit_stick_dz(&s.rx, &s.ry, dr / 100.f, dout / 100.f);
        s.connected = 1;
    }
    runtime_gamepad_set(&s);
}

// hid_feedback.c: the game's vibration value for one side (0 left, 1 right). A Switch controller has a low and a high
// frequency actuator per side; an SDL controller has a low and a high frequency motor, so each takes the stronger side.
// The game sends values every frame; the 250 ms duration stops the motors if it stops sending.
void runtime_rumble_set(unsigned side, float amp_low, float freq_low, float amp_high, float freq_high) {
    (void)freq_low; (void)freq_high;
    static float low[2], high[2];
    if (side > 1 || !pad_lock) return;
    SDL_LockMutex(pad_lock);
    low[side] = amp_low < 0 ? 0 : amp_low > 1 ? 1 : amp_low;
    high[side] = amp_high < 0 ? 0 : amp_high > 1 ? 1 : amp_high;
    float l = low[0] > low[1] ? low[0] : low[1], h = high[0] > high[1] ? high[0] : high[1];
    if (pad) SDL_RumbleGamepad(pad, (Uint16)(l * 65535), (Uint16)(h * 65535), 250);
    SDL_UnlockMutex(pad_lock);
}

static void toggle_fullscreen(void);
static void log_mark(void);

// [UIT 0452] Game > Controller > Button prompts = Auto: the kind of controller connected now (prompts.c asks once, the
// first time the game opens a file the prompts change, from the game's thread). The open one if there is one, else the
// first SDL lists (it may not be opened yet: the window thread opens it on its "added" event). Xbox for every pad that
// is not a PlayStation or Nintendo one (Xbox, XInput pads, Steam Deck / Steam Controller and unknown pads use Xbox
// letters). -1: none connected.
static int prompt_family(void) {
    SDL_GamepadType t = SDL_GAMEPAD_TYPE_UNKNOWN;
    const char *name = NULL;
    int have = 0;
    if (pad_lock) {
        SDL_LockMutex(pad_lock);
        if (pad) t = SDL_GetGamepadType(pad), name = SDL_GetGamepadName(pad), have = 1;
        SDL_UnlockMutex(pad_lock);
    }
    if (!have) {
        int n = 0;
        SDL_JoystickID *ids = SDL_GetGamepads(&n);
        if (ids && n > 0) t = SDL_GetGamepadTypeForID(ids[0]), name = SDL_GetGamepadNameForID(ids[0]), have = 1;
        SDL_free(ids);
    }
    if (!have) return -1;
    int f;
    switch (t) {
    case SDL_GAMEPAD_TYPE_PS3: case SDL_GAMEPAD_TYPE_PS4: f = UIT_PROMPTS_PS4; break;
    case SDL_GAMEPAD_TYPE_PS5: f = UIT_PROMPTS_PS5; break;
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO: case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_LEFT:
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_RIGHT: case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_PAIR:
    case SDL_GAMEPAD_TYPE_GAMECUBE: f = UIT_PROMPTS_SWITCH; break;
    default: f = UIT_PROMPTS_XBOX; break;
    }
    fprintf(stderr, "[totk] button prompts: Auto sees %s (SDL type %d)\n", name ? name : "a controller", (int)t);
    return f;
}

int window_init(void) {
    SDL_SetMainReady();
    SDL_SetHint(SDL_HINT_AUDIO_DEVICE_SAMPLE_FRAMES, "480");   // 10 ms device buffer at 48 kHz
    SDL_SetHint(SDL_HINT_WINDOWS_INTRESOURCE_ICON, "1");        // the exe's icon resource (assets/uit.ico via build/uit.rc) for the window
    SDL_SetHint(SDL_HINT_WINDOWS_INTRESOURCE_ICON_SMALL, "1");
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD | SDL_INIT_AUDIO)) {
        fprintf(stderr, "[totk] SDL: %s\n", SDL_GetError());
        return 0;
    }
    pad_lock = SDL_CreateMutex();
    uit_prompts_set_detector(prompt_family);   // [UIT 0452]
    input_map_init();
    stick_dz_init();
    // TOTK_WINDOW_SIZE=<w>x<h> (default 1280x720) and TOTK_FULLSCREEN=1 (borderless, as F11): set by the launcher.
    int ww = 1280, wh = 720;
    const char *ws = getenv("TOTK_WINDOW_SIZE");
    if (ws && (sscanf(ws, "%dx%d", &ww, &wh) != 2 || ww < 320 || wh < 180 || ww > 16384 || wh > 16384)) {
        fprintf(stderr, "[totk] TOTK_WINDOW_SIZE=%s is not like 1920x1080: 1280x720\n", ws);
        ww = 1280, wh = 720;
    }
    window = SDL_CreateWindow("Ultra Instinct Tears", ww, wh, SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    if (!window) { fprintf(stderr, "[totk] window: %s\n", SDL_GetError()); return 0; }
    SDL_Surface *icon = SDL_LoadPNG_IO(SDL_IOFromConstMem(kUitIconPng, sizeof kUitIconPng), true);
    if (icon) {                                                 // also when the exe has no icon resource
        SDL_SetWindowIcon(window, icon);
        SDL_DestroySurface(icon);
    }
    const char *fs = getenv("TOTK_FULLSCREEN");
    if (fs && *fs && strcmp(fs, "0")) SDL_SetWindowFullscreen(window, true);   // borderless desktop fullscreen
    if (!vkp_init(window))
        runtime_fail("Vulkan initialization failed (Vulkan 1.3 with VK_EXT_shader_object is required)");
    uit_overlay_init(window, toggle_fullscreen, log_mark);   // [UIT 0173] TOTK_OVERLAY=0: off
    uit_bench_init(window);   // [UIT 0302] TOTK_BENCH_SECONDS
    uit_bg_set_fps(uit_bg_parse(getenv("TOTK_BG_FPS")));   // [UIT 0303] applies once the window loses the focus
    mouse_init();            // [UIT 0451] the mouse camera
    mouse_capture_sync();
    return 1;
}

// Motion: the controller's accelerometer and gyro (SDL_EVENT_GAMEPAD_SENSOR_UPDATE) go to motion_fusion.c.
static void pad_attached(void) {   // pad_lock held; pad may be NULL
    if (!pad) { runtime_motion_live(0); return; }
    fprintf(stderr, "[totk] game controller: %s\n", SDL_GetGamepadName(pad));
    if (SDL_GamepadHasSensor(pad, SDL_SENSOR_GYRO) && SDL_GamepadHasSensor(pad, SDL_SENSOR_ACCEL) &&
        SDL_SetGamepadSensorEnabled(pad, SDL_SENSOR_GYRO, true) && SDL_SetGamepadSensorEnabled(pad, SDL_SENSOR_ACCEL, true)) {
        const char *axes = getenv("TOTK_GYRO_AXES");
        if (runtime_motion_axes(axes) < 0) fprintf(stderr, "[totk] TOTK_GYRO_AXES=%s is not like \"-x,y,-z\": ignored\n", axes);
        runtime_motion_live(1);
        fprintf(stderr, "[totk] controller motion sensors on (%.0f Hz gyro)\n", SDL_GetGamepadSensorDataRate(pad, SDL_SENSOR_GYRO));
    } else {
        runtime_motion_live(0);
        fprintf(stderr, "[totk] controller has no motion sensors: no gyro aiming\n");
    }
}
// SDL's controller frame: x right, y out of the face, z toward the player (the bottom edge); acceleration in m/s^2 as
// the force holding it up (face up reads y = +9.8). The Switch frame (runtime_motion_feed): x right, y toward the top
// edge, z out of the face, acceleration as gravity (face up reads z = -1 G). (x, -z, y) is a rotation, so the gyro
// keeps its sense; the acceleration also changes sign.
static void pad_sensor(const SDL_GamepadSensorEvent *e) {
    static double acc[3] = {0, 0, -1};
    const float *d = e->data;
    if (e->sensor == SDL_SENSOR_ACCEL) {
        acc[0] = -d[0] / SDL_STANDARD_GRAVITY;
        acc[1] = d[2] / SDL_STANDARD_GRAVITY;
        acc[2] = -d[1] / SDL_STANDARD_GRAVITY;
    } else if (e->sensor == SDL_SENSOR_GYRO) {
        double gyro[3] = {d[0], -d[2], d[1]};
        if (uit_overlay_open()) gyro[0] = gyro[1] = gyro[2] = 0;   // [UIT 0173] the camera holds still under the overlay
        runtime_motion_feed(acc, gyro, runtime_now_ns());
    }
}

static void open_pad(SDL_JoystickID id) {
    SDL_LockMutex(pad_lock);
    if (!pad) {
        pad = SDL_OpenGamepad(id);
        pad_attached();
    }
    SDL_UnlockMutex(pad_lock);
    pad_update();
}
static void close_pad(SDL_JoystickID id) {
    SDL_LockMutex(pad_lock);
    if (pad && SDL_GetGamepadID(pad) == id) {
        SDL_CloseGamepad(pad);
        pad = NULL;
        int n = 0;   // another one still connected takes over
        SDL_JoystickID *ids = SDL_GetGamepads(&n);
        if (ids && n) pad = SDL_OpenGamepad(ids[0]);
        SDL_free(ids);
        pad_attached();
    }
    SDL_UnlockMutex(pad_lock);
    pad_update();
}

static void toggle_fullscreen(void) {
    bool on = (SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN) != 0;
    SDL_SetWindowFullscreen(window, !on);   // borderless desktop fullscreen (no display mode change)
}

// [UIT 0053] F7: a mark in the log (also the overlay's Overlay > Mark in log)
static void log_mark(void) {
    static int marks;
    time_t now = time(NULL);
    const struct tm *lt = localtime(&now);
    fprintf(stderr, "[mark] %d: F7 at %02d:%02d:%02d local, %.2f s after start\n", ++marks, lt ? lt->tm_hour : 0,
            lt ? lt->tm_min : 0, lt ? lt->tm_sec : 0, (double)SDL_GetTicks() / 1000.0);
}

// Game frame rate and shown frame rate in the title, twice a second.
static void title_update(void) {
    static uint64_t t0, game0, shown0;
    uint64_t now = runtime_now_ns(), game = vkp_frames(), shown = vkp_presented();
    if (!t0) { t0 = now; game0 = game; shown0 = shown; return; }
    if (now - t0 < 500000000) return;
    double s = (now - t0) / 1e9;
    char title[128];
    snprintf(title, sizeof title, "Ultra Instinct Tears  |  game %.1f fps  |  shown %.1f fps", (game - game0) / s, (shown - shown0) / s);
    SDL_SetWindowTitle(window, title);
    t0 = now; game0 = game; shown0 = shown;
}

// [UIT 0293] The one way the window ends the game: its close button, Alt+F4, the system, and [UIT 0292] the overlay's
// Quit game (how 1) / Restart game (how 2), which send the same SDL_EVENT_QUIT. A pending overlay change and a save
// commit still being written finish, the log is flushed, then the process ends (the system releases the GPU, the
// window and the game's threads with it). A watchdog makes sure it cannot hang: if that takes longer than
// EXIT_WATCHDOG_MS (past uit_save_drain's own 10 s), the log says so and the process ends anyway with the same status.
// Restart: started by the launcher (UIT_ENV_LAUNCHER), exit status UIT_EXIT_RESTART and the launcher starts it again
// the same way; started by hand, plat_restart_self starts it again and this one exits 0.
#define EXIT_WATCHDOG_MS 12000
static int exit_code;
static int SDLCALL exit_watchdog(void *arg) {
    (void)arg;
    SDL_Delay(EXIT_WATCHDOG_MS);
    fprintf(stderr, "[totk] closing took over %d s: ending the game now\n", EXIT_WATCHDOG_MS / 1000);
    plat_terminate_code(exit_code);   // flushes the log
    return 0;
}
static _Noreturn void window_exit(int how) {
    const char *e = getenv(UIT_ENV_LAUNCHER);
    int by_launcher = e && *e == '1';
    exit_code = how == 2 && by_launcher ? UIT_EXIT_RESTART : 0;
    fprintf(stderr, "%s\n", how == 1 ? "[totk] quit from the overlay"
                           : how == 2 ? (by_launcher ? "[totk] restart from the overlay: the launcher starts the game again"
                                                     : "[totk] restart from the overlay: starting the game again")
                                      : "[totk] window closed");
    uint64_t exit_t0 = SDL_GetTicks();   // [UIT 0444]
    SDL_Thread *wd = SDL_CreateThread(exit_watchdog, "uit-exit-watchdog", NULL);
    if (wd) SDL_DetachThread(wd);
    runtime_profiler_stop();   // [UIT 0363] TOTK_PROF(_GAME): the final CPU profile is written while the exit runs
    uit_overlay_shutdown();   // [UIT 0173] a pending overlay change is saved
    uit_lift_exit();   // [UIT 0070] the shadow probe's notes (diag/lift-probe.txt)
    uit_save_drain();  // [UIT 0080] a save commit still being flushed finishes first (at most 10 s)
    // [UIT 0363] the final CPU report: [UIT 0444] until 1 s before the watchdog (at least 4 s), not 4 s flat
    uint64_t exit_ms = SDL_GetTicks() - exit_t0;
    runtime_profiler_finish(exit_ms + 5000 < EXIT_WATCHDOG_MS ? (unsigned)(EXIT_WATCHDOG_MS - 1000 - exit_ms) : 4000);
    if (how == 2 && !by_launcher && !plat_restart_self())
        fprintf(stderr, "[totk] restart: could not start the game again; start it from the launcher\n");
    fflush(stderr);    // _Exit skips the CRT's flush: the log's last lines (NUL / lasrecomp.log are buffered)
    _Exit(exit_code);
}

_Noreturn void window_loop(void) {
    int was_open = 0;
    for (;;) {
        SDL_Event e;
        // [UIT 0173] while the overlay is open or its combo held: ~60 Hz (hold timing, repeat, redraw); else as before
        if (SDL_WaitEventTimeout(&e, uit_overlay_wait_ms(100))) {
            do {
                switch (e.type) {
                case SDL_EVENT_QUIT: window_exit(uit_overlay_exit_how());
                case SDL_EVENT_KEY_DOWN: case SDL_EVENT_KEY_UP: {
                    if (e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat &&
                        (e.key.scancode == SDL_SCANCODE_F11 ||
                         (e.key.scancode == SDL_SCANCODE_RETURN && (e.key.mod & SDL_KMOD_ALT)))) {
                        toggle_fullscreen();
                        break;
                    }
                    // [UIT 0346] the screenshot key (F12): not a game key; the overlay's own first (its hotkey, typing)
                    if (uit_shot_key() && e.key.scancode == (SDL_Scancode)uit_shot_key() &&
                        !uit_overlay_wants_key(e.key.scancode)) {
                        if (e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat) uit_shot_take("key");
                        break;
                    }
                    if (uit_overlay_key(&e.key)) break;   // [UIT 0173] its hotkey; while open every key but F7-F11
                    if (e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat && e.key.scancode == SDL_SCANCODE_F8 && (e.key.mod & SDL_KMOD_SHIFT)) {
                        uit_bench_toggle();   // [UIT 0302] Shift+F8: start / stop the benchmark
                        break;
                    }
                    if (e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat && e.key.scancode == SDL_SCANCODE_F8) { vkp_fsr_toggle(); break; }
                    if (e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat && e.key.scancode == SDL_SCANCODE_F9) { vkp_fsr_sharpness_next(); break; }
                    if (e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat && e.key.scancode == SDL_SCANCODE_F10) { dlss_toggle(); break; }   // DLSS on / off
                    if (e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat && e.key.scancode == SDL_SCANCODE_F7) {
                        if (e.key.mod & SDL_KMOD_SHIFT) {   // [UIT 0157] Shift+F7: what the next frame's draws read (diag/frame-capture-*.txt)
                            log_mark();
                            vkb_capture_request();
                        } else log_mark();
                        break;
                    }
                    int k = mac_key(e.key.scancode);
                    if (k >= 0 && !e.key.repeat) runtime_input_key((unsigned)k, e.type == SDL_EVENT_KEY_DOWN);
                    break;
                }
                // [UIT 0348] audio_set_away: TOTK_BG_MUTE (it does nothing when that is off)
                case SDL_EVENT_WINDOW_FOCUS_LOST: runtime_input_clear(); m_held = 0; uit_bg_set_away(1); audio_set_away(1); break;
                // [UIT 0303] back in front: the cap is off before the next frame; minimised counts as away
                case SDL_EVENT_WINDOW_FOCUS_GAINED: m_focus_ms = SDL_GetTicks(); uit_bg_set_away(0); audio_set_away(0); break;
                case SDL_EVENT_WINDOW_MINIMIZED: uit_bg_set_away(1); audio_set_away(1); break;
                case SDL_EVENT_WINDOW_RESTORED:
                    if (SDL_GetWindowFlags(window) & SDL_WINDOW_INPUT_FOCUS) uit_bg_set_away(0), audio_set_away(0);
                    break;
                case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED: vkp_resized(); break;
                case SDL_EVENT_GAMEPAD_ADDED: open_pad(e.gdevice.which); break;
                case SDL_EVENT_GAMEPAD_REMOVED: close_pad(e.gdevice.which); break;
                case SDL_EVENT_GAMEPAD_AXIS_MOTION: case SDL_EVENT_GAMEPAD_BUTTON_DOWN: case SDL_EVENT_GAMEPAD_BUTTON_UP:
                    pad_update();
                    break;
                case SDL_EVENT_GAMEPAD_SENSOR_UPDATE: pad_sensor(&e.gsensor); break;
                case SDL_EVENT_MOUSE_MOTION: case SDL_EVENT_MOUSE_BUTTON_DOWN: case SDL_EVENT_MOUSE_BUTTON_UP:
                case SDL_EVENT_MOUSE_WHEEL:
                    // [UIT 0173] the overlay takes the mouse while open; [UIT 0451] else it is the mouse camera's
                    if (!uit_overlay_mouse(&e)) mouse_event(&e);
                    break;
                case SDL_EVENT_TEXT_INPUT: case SDL_EVENT_TEXT_EDITING:
                    uit_overlay_text(&e);    // [UIT 0177] the keyboard dialog (text input is on only while it is up)
                    break;
                default: break;   // [UIT 0177] also the keyboard dialog's wake-up (overlay_live.c kb_wake): the frame below
                }
                if (uit_overlay_open() != was_open) {   // opened / closed by a key or the mouse: the game's controller
                    was_open = uit_overlay_open();      // state follows (at rest while open, the real one after)
                    pad_update();
                }
            } while (SDL_PollEvent(&e));
        }
        if (uit_overlay_busy()) pad_update();   // the combo's hold time and the stick's repeat run without events
        was_open = uit_overlay_open();
        uit_overlay_frame();
        mouse_capture_sync();   // [UIT 0451] let the mouse go while the overlay is open, take it back after
        title_update();
    }
}
