// [UIT 0170] Settings that can change while the game runs: the setters and getters the in-game overlay
// (src/host/uit_overlay.c, src/host/overlay_live.c) uses. Each is implemented next to the code that reads the setting,
// and each is safe to call from the window (SDL main) thread while the game, replay and present threads run:
// an atomic store that the reading thread picks up at its next use, never a setenv (getenv on other threads).
// Settings read once at start (resolution, upscaler choice, lift features, ...) have no entry here on purpose.
#pragma once

// src/vk/present_fsr.c: FSR 1 at the present (F8) and its sharpness in stops, 0..2 (F9 steps through presets)
int vkp_fsr_wanted(void);
void vkp_fsr_set(int on);
float vkp_fsr_sharpness(void);
void vkp_fsr_set_sharpness(float stops);

// src/vk/present.c: the swapchain is rebuilt (before the next present) with the new choice.
// mode: 0 automatic, 1 mailbox, 2 fifo, 3 immediate (launcher choice order); triple: 0 automatic, 1 on, 2 off
int vkp_present_mode_choice(void);
void vkp_set_present_mode(int mode);
int vkp_triple_buffer_choice(void);
void vkp_set_triple_buffer(int triple);
const char *vkp_present_mode_name(void);   // the mode the current swapchain has ("mailbox", "fifo", "immediate")

// src/host/fps_unlock.c + src/gfx/nvn/hle_nvn.c: the frame rate target between 60 and 120 (only when the run started
// above 30: the 60 FPS game patches are chosen once at start). totk_fps_retarget returns 0 when it cannot.
int totk_fps_start(void);      // the target the run started with (30, 60, 120)
int totk_fps_target(void);     // the target now
int totk_fps_retarget(int fps);
void nvn_fps_retarget(int fps);   // the present clock follows at the game thread's next present

// src/gfx/nvn/hle_nvn.c: TOTK_INTERVAL_HOLD (0..30) and TOTK_MAX_INTERVAL (2 = half rate, 1, 3, 0)
int nvn_interval_hold(void);
void nvn_set_interval_hold(int frames);
int nvn_max_interval(void);
void nvn_set_max_interval(int v);

// src/gfx/vkb_tex.c: TOTK_TEX_BUDGET_MB (8..1536) and TOTK_TEX_GRACE_MS (0..50); 1 = streaming is on (they apply)
int vkb_tex_live(int *budget_mb, double *grace_ms);
void vkb_tex_set_budget_mb(int mb);
void vkb_tex_set_grace_ms(double ms);

// src/host/hid_feedback.c: rumble / gyro on (1) or muted (0); *started = the device was enabled at start (muting
// works either way, turning back on only then)
int runtime_hid_feedback_on(const char *name, int *started);
int runtime_hid_feedback_flag(const char *name, int v);

// src/host/motion_fusion.c: the gyro dead zone in deg/s, 0..20; < 0 = automatic (the controller's default)
double runtime_motion_deadzone(int *automatic);
void runtime_motion_set_deadzone(double deg);

// src/host/audio_mixer.c: TOTK_VOLUME, 0..100 % (100 = untouched samples)
int audio_volume(void);
void audio_set_volume(int percent);
void audio_set_away(int away);   // [UIT 0348] the window lost (1) or got back (0) focus: TOTK_BG_MUTE
const char *uit_cfg_path(void);   // src/host/boot.c: the settings file this run read ("" = none; Windows)

// src/host/window_sdl.c [UIT 0290]: stick deadzones in %, which 0 = left inner (0..50), 1 = right inner (0..50),
// 2 = outer, both sticks (0..30); [UIT 0291] the trigger press point in % (5..95, 50 = as before) and its axis value
int uit_stick_deadzone(int which);
void uit_set_stick_deadzone(int which, int pct);
int uit_trigger_point(void);
void uit_set_trigger_point(int pct);
int uit_trigger_threshold_now(void);
