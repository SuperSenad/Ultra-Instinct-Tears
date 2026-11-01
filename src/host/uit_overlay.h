// [UIT 0171] The in-game overlay's portable core: no SDL, no Vulkan, so tools/tests can drive it on its own.
//  - the settings it offers (kUitOvSettings: only settings that change while the game runs, src/host/uit_live.h),
//  - its input state machine (keyboard / mouse / controller navigation, the controller's hold-to-open combo),
//  - its layout as a list of quads (UitOvQuad: rounded rects, text, outlines, the hold ring) for src/vk/overlay_vk.c,
//  - its font atlas (Inter, stb_truetype; built once per size, freed on shutdown),
//  - saving to ultrainstincttears.cfg exactly as the launcher writes it (launcher/model.cpp CfgFile / write_value).
//  - [UIT 0174] the Controls tab: the controller and keyboard bindings (input_bind.h), changed while the game runs.
//  - [UIT 0175] the Amiibo tab (scan an amiibo file: src/host/amiibo.h) and the Cheats tab (src/host/cheats.h).
// Threads: all of it runs on the window (SDL main) thread; the present thread only reads what uit_ov_publish hands
// over (src/host/overlay_live.c), under that function's lock.
#pragma once
#include <stddef.h>
#include <stdint.h>

#include "input_bind.h"
#include "swkbd.h"   // [UIT 0177] the keyboard dialog's request

#ifdef __cplusplus
extern "C" {
#endif

// ---- what the renderer draws: one instance per quad (48 bytes; src/vk/shaders/overlay.vert reads this layout)
typedef struct {
    float x0, y0, x1, y1;   // pixels, top left origin
    float u0, v0, u1, v1;   // atlas coordinates (text); the ring's filled fraction in u0
    uint32_t rgba;          // R | G << 8 | B << 16 | A << 24, sRGB (the swapchain is UNORM: written as is)
    float radius, kind, stroke;
} UitOvQuad;
enum { UIT_OVQ_RECT = 0, UIT_OVQ_TEXT = 1, UIT_OVQ_OUTLINE = 2, UIT_OVQ_RING = 3 };

// ---- the settings
enum { UIT_OV_TAB_DISPLAY, UIT_OV_TAB_PERF, UIT_OV_TAB_GAME, UIT_OV_TAB_CONTROLS, UIT_OV_TAB_AMIIBO, UIT_OV_TAB_CHEATS,
       UIT_OV_TAB_OVERLAY, UIT_OV_TABS };
#define UIT_OV_PANEL_ROWS 8   // [UIT 0321] at most this many setting rows at once (the Game tab's 8)
typedef enum { UIT_OVS_BOOL, UIT_OVS_CHOICE, UIT_OVS_INT, UIT_OVS_FLOAT, UIT_OVS_ACTION, UIT_OVS_INFO } UitOvKind;
enum {
    OV_FSR, OV_DLSS, OV_SHARP, OV_FULLSCREEN, OV_WINSIZE, OV_PRESENT, OV_TRIPLE,
    OV_FPS, OV_HOLD, OV_MAXIV, OV_BGFPS, OV_TEXBUD, OV_TEXGRACE,
    OV_VOLUME, OV_RUMBLE, OV_GYRO, OV_GYRODZ, OV_LSDZ, OV_RSDZ, OV_STICKOUT, OV_TRIGPT,
    OV_CHEAT_DUR, OV_CHEAT_AMIIBO,
    OV_SHOWFPS, OV_FTGRAPH, OV_SAVE, OV_MARK, OV_CAPTURE, OV_BENCHLEN, OV_BENCH, OV_KEYS,
    OV_RESTART, OV_QUIT,   // [UIT 0292] the Overlay tab's last rows (each asks first)
    OV_COUNT
};
typedef struct {
    int id, tab;
    UitOvKind kind;
    const char *name, *help;
    const char *key;                 // the cfg key it is saved as; NULL = this session only
    const char *def, *on, *off;      // as in launcher/settings.ini (bool tokens: "-" = no line)
    const char *choices;             // "value:label|value:label" (values as in settings.ini)
    double min, max, step;
    const char *unit, *zero, *autolabel;   // zero: the text for 0; autolabel: the text below min (no line)
    const char *launcher;            // where the launcher shows it (Tab > section > setting)
} UitOvSetting;
extern const UitOvSetting kUitOvSettings[OV_COUNT];
int uit_ov_choice_count(const UitOvSetting *s);
// The i-th choice's value / label (copied, NUL-terminated); 0 when i is out of range.
int uit_ov_choice(const UitOvSetting *s, int i, char *value, size_t vn, char *label, size_t ln);

// What the overlay calls to read and change the game (src/host/overlay_live.c; tests pass fakes).
typedef struct {
    double (*get)(int id);           // bool 0/1, choice index (-1 = none of them), number (below min = automatic)
    void (*set)(int id, double v);
    const char *(*why)(int id);      // NULL: can change now; otherwise why not (the row is shown greyed out)
    int (*shown)(int id);            // 0: no row (e.g. DLSS on a PC without it)
    const char *(*text)(int id, double v, char *buf, size_t n);   // NULL result: the usual value text
    void (*action)(int id);
    int (*choice_ok)(int id, int index);   // 0: that choice is skipped now (NULL hook: every choice is fine)
    const char *(*cfg_path)(void);   // "" / NULL: no settings file (changes are this session only)
    int (*dlss_run)(void);           // the run's upscaler is DLSS (how FSR is saved, uit_ov_cfg_edits)
    void (*binds_set)(const UitBinds *b);   // [UIT 0174] the Controls tab changed a binding: use these now
    // [UIT 0175] the Amiibo tab (NULL amiibo_scan: the tab says scanning is not available)
    int (*amiibo_list)(int refresh);                  // how many files the amiibo folder has (refresh: read it again)
    const char *(*amiibo_file)(int i);                // the i-th one's name as shown
    void (*amiibo_pick)(void);                        // the system's file dialog (its answer: uit_ov_note, later)
    int (*amiibo_scan)(int i, char *msg, size_t n);   // file i (-1: the last one again): 0 scanned, 1 not; msg: what happened
    const char *(*amiibo_line)(char *buf, size_t n, int *bad);   // the status line (does the game ask, what was scanned)
    const char *(*amiibo_last)(void);                 // the last amiibo scanned (NULL: none yet)
    const char *(*amiibo_dir)(void);                  // the folder listed (as shown)
    int (*amiibo_files)(void);   // [UIT 0181] how many of amiibo_list's rows are files (the rest: built-in figures); NULL: all
    // [UIT 0292] Restart / Quit game, once confirmed: end the game as closing the window does (restart 1: start it again).
    // NULL: no Restart / Quit rows.
    int (*quit)(int restart);   // 1: the game is ending; 0: it could not (the status line says so, the game goes on)
} UitOvHooks;

// ---- ultrainstincttears.cfg: the lines one change writes (launcher/model.cpp write_value, main.cpp's FSR keep-in-step)
typedef struct { const char *key; char value[128]; int remove; } UitCfgEdit;   // 128: a binding list (input_bind.h)
int uit_ov_cfg_edits(int id, double v, int dlss_run, UitCfgEdit *out, int max);   // the number of edits
// The file's text with the edits made as CfgFile::set does them (BOM dropped, line ending kept, a new key after the
// "# Your settings" header). *out is malloc'd. Returns 0 on allocation failure.
int uit_cfg_edit_text(const char *in, size_t n, const UitCfgEdit *e, int ne, char **out, size_t *outn);
// Read, edit, write a .tmp file and rename it over the file. 1 = saved; 0 = err says why.
int uit_cfg_apply(const char *path, const UitCfgEdit *e, int ne, char *err, size_t errn);

// ---- fonts
enum { UIT_OVF_BODY, UIT_OVF_SEMI, UIT_OVF_TITLE, UIT_OVF_SMALL, UIT_OVF_COUNT };
// ASCII 32..126, [UIT 0177] Latin-1, Latin Extended-A, Greek and Cyrillic (names typed in the keyboard dialog), and
// marks (arrows, chevrons, dashes, the keyboard's keys) (see uit_overlay.c kRanges / kExtra). Anything else draws '?'.
#define UIT_OV_GLYPHS (95 + 96 + 128 + 80 + 96 + 16)
typedef struct { float x0, y0, x1, y1, u0, v0, u1, v1, adv; } UitOvGlyph;   // x/y relative to the pen on the baseline
typedef struct {
    UitOvGlyph g[UIT_OVF_COUNT][UIT_OV_GLYPHS];
    float ascent[UIT_OVF_COUNT], descent[UIT_OVF_COUNT], size[UIT_OVF_COUNT];
    uint8_t *pix;                    // R8, w x h
    int w, h;
    float scale;
} UitOvAtlas;
int uit_ov_atlas_build(UitOvAtlas *a, const unsigned char *regular_ttf, const unsigned char *semibold_ttf, float scale);
void uit_ov_atlas_free(UitOvAtlas *a);
float uit_ov_text_width(const UitOvAtlas *a, int font, const char *utf8);

// ---- the overlay
typedef enum { OVIN_UP, OVIN_DOWN, OVIN_LEFT, OVIN_RIGHT, OVIN_CONFIRM, OVIN_BACK, OVIN_TAB_PREV, OVIN_TAB_NEXT,
               OVIN_RESET /* Delete: a binding back to its default (Controls tab) */ } UitOvInput;
// Controller navigation bits for uit_ov_pad. [UIT 0270] overlay_live.c maps the face buttons by position
// (uit_ov_face_nav): Game > Controller > Confirm button (TOTK_UI_CONFIRM) standard = the bottom button confirms and the
// right one closes, game = the right one confirms and the bottom one closes; the left button puts a binding back to its
// default, the top one is the keyboard dialog's Y. (Before 0270 they went by the printed letters A / B / X / Y.)
enum { OVNAV_UP = 1, OVNAV_DOWN = 2, OVNAV_LEFT = 4, OVNAV_RIGHT = 8, OVNAV_CONFIRM = 16, OVNAV_BACK = 32, OVNAV_TABL = 64, OVNAV_TABR = 128,
       OVNAV_RESET = 256,
       // [UIT 0177] the keyboard dialog's own: Y (letters / symbols), + / Start (OK), - / Back (cancel), L3 (shift)
       OVNAV_Y = 512, OVNAV_PLUS = 1024, OVNAV_MINUS = 2048, OVNAV_L3 = 4096 };
#define UIT_OV_BIND_ROWS (UIT_BIND_BUTTONS + 1)   // Controls tab: a row per Switch button, then "All buttons" (the resets)
#define UIT_OV_AM_VIS 24                          // Amiibo tab: at most this many rows laid out at once
#define UIT_OV_BIND_MS 8000                       // a capture with no press ends by itself (launcher: kBindSeconds)
enum { OVC_IDLE, OVC_HOLD, OVC_DISARMED, OVC_LATCHED };
typedef struct { float x0, y0, x1, y1; } UitOvBox;
// [UIT 0177] the software keyboard dialog (nn::swkbd's ShowKeyboard, answered by src/host/swkbd_host.c). A grid of
// keys for the controller and the mouse, the physical keyboard's text (and an IME's) through the glue.
#define UIT_KB_ROWS 5                              // 4 rows of 10 characters, then the special keys
#define UIT_KB_COLS 10
enum { UIT_KB_PAGE_LETTERS, UIT_KB_PAGE_SYMBOLS, UIT_KB_PAGE_ACCENTS, UIT_KB_PAGES };
enum { UIT_KBK_SHIFT, UIT_KBK_PAGE, UIT_KBK_SPACE, UIT_KBK_BACK, UIT_KBK_CANCEL, UIT_KBK_OK, UIT_KBK_COUNT };   // row 4
enum { UIT_KBE_BACKSPACE, UIT_KBE_DELETE, UIT_KBE_LEFT, UIT_KBE_RIGHT, UIT_KBE_HOME, UIT_KBE_END, UIT_KBE_OK,
       UIT_KBE_CANCEL, UIT_KBE_NEWLINE };          // the physical keyboard's editing keys (uit_ov_kb_edit)
typedef struct {
    int active;                       // shown, and taking every input
    int done, ok;                     // answered: the glue takes the text (uit_ov_kb_result) and ends it (uit_ov_kb_end)
    UitSwkbdReq req;
    uint16_t t[UIT_SWKBD_LIMIT + 2];  // the text (UTF-16, as the game gets it), t[n] = 0
    int n, cur;                       // its length and the cursor (units)
    char pre[256];                    // an IME's composition, shown at the cursor (not part of the text yet)
    int page, shift, row, col;        // the grid: page, shift (1 one letter, 2 locked), the focused key
    uint32_t nav_prev;                // ~0 at the start: what is held then acts only once let go
    uint64_t nav_t, t0;
    int nav_rep;
    char msg[96];                     // why a key did nothing (too long, not allowed here, ...), for 3 s
    uint64_t msg_ms;
    UitOvBox panel, field, key[UIT_KB_ROWS][UIT_KB_COLS];   // the last layout: the mouse's targets
} UitOvKb;
typedef struct {
    const UitOvHooks *h;
    int open, tab, sel[UIT_OV_TABS];
    int save;                         // 1: changes go to the settings file; 0: this session only
    int show_fps;                     // the frame rate counter (drawn while the overlay is closed)
    int show_graph;                   // [UIT 0301] the frame-time graph (under the counter, while the overlay is closed)
    int bench_on;                     // [UIT 0302] the glue: a benchmark is running (the row's button says Stop)
    // the controller combo (physical button bits: window_sdl.c, 1 << SDL_GamepadButton, 30/31 = triggers)
    uint32_t combo, hold_ms;
    uint32_t combo2;                  // [UIT 0271] a second combo that also opens it (e.g. Guide alone), 0 = none
    uint32_t ccur;                    // the combo being held (HOLD / LATCHED): combo or combo2
    int cstate;
    uint64_t ct0;
    uint32_t suppress;                // buttons held when the overlay closed (or a cancelled combo): hidden until released
    uint32_t nav_prev;
    uint64_t nav_t;
    int nav_rep;
    // the mouse
    float mx, my;
    int drag;                         // setting id being dragged + 1
    // the last layout (hit boxes, window size)
    float fw, fh, s;
    int nrows, rows[OV_COUNT];
    UitOvBox panel, rowbox[OV_COUNT], dec[OV_COUNT], inc[OV_COUNT], bar[OV_COUNT], tabbox[UIT_OV_TABS], closebox;
    // saving
    unsigned char pending[OV_COUNT];
    uint64_t change_ms;
    char status[160];
    int status_bad;
    uint64_t status_ms;
    uint64_t now;                     // the last time given to uit_ov_pad / uit_ov_tick / uit_ov_build
    int dirty;                        // the picture changed: build the quads again
    uint32_t phys_now;                // the controller buttons down at the last uit_ov_pad
    // [UIT 0174] the Controls tab: sel[UIT_OV_TAB_CONTROLS] is the row, bcol the column (UIT_BIND_PAD / UIT_BIND_KEY)
    UitBinds binds;                   // what the game uses now (uit_ov_set_binds; changes go out through binds_set)
    int have_binds;                   // 0: no bindings given (the tab says so and changes nothing)
    unsigned char bpending[2][UIT_BIND_BUTTONS];   // saved with the settings (same delay, same "save" switch)
    int bcol, bscroll;                // the column; the first row shown (the list scrolls)
    int pad_present, pad_family;      // the glue: a controller is connected, and its kind (labels)
    int confirm_game;                 // [UIT 0270] TOTK_UI_CONFIRM=game: the right face button confirms (the hints say so)
    int cap, cap_kind, cap_btn;       // capturing: "press the button you want" for Switch button cap_btn
    uint64_t cap_t0;
    uint32_t cap_held;                // buttons down when it started: they bind only after being let go
    UitOvBox brow[UIT_OV_BIND_ROWS], cell[UIT_OV_BIND_ROWS][2], cellreset[UIT_BIND_BUTTONS][2];   // the mouse's targets
    // [UIT 0175] the Amiibo tab: sel[UIT_OV_TAB_AMIIBO] is the row (0 choose a file, 1 scan again, 2.. the folder's files)
    int am_n, am_scroll, am_nbox;     // the files listed; the first file row shown; the rows laid out
    int am_boxrow[UIT_OV_AM_VIS];
    UitOvBox am_box[UIT_OV_AM_VIS];
    UitOvKb kb;                       // [UIT 0177]
    // [UIT 0292] "Quit to desktop?" / "Restart the game?": ask = OV_QUIT / OV_RESTART while it is shown (0: not), it takes
    // every input; ask_sel 0 Cancel (where it starts), 1 Quit / Restart. exit_req: 1 quit, 2 restart once confirmed.
    int ask, ask_sel, exit_req;
    UitOvBox ask_panel, ask_btn[2];
    // [UIT 0321] a tab with more rows than the panel shows (the Overlay tab: 10 with Restart and Quit) scrolls with the
    // selection; the panel keeps the height of UIT_OV_PANEL_ROWS rows, so it fits a 1080p screen and never moves
    int rscroll[UIT_OV_TABS];         // the first row shown on each tab
    int rvis;                         // rows the panel shows (the last build)
} UitOv;

void uit_ov_init(UitOv *o, const UitOvHooks *h, uint32_t combo, uint32_t hold_ms);
void uit_ov_set_open(UitOv *o, int open, uint32_t phys_held);   // phys_held: buttons down now (hidden until released)
void uit_ov_input(UitOv *o, UitOvInput in);
void uit_ov_mouse(UitOv *o, float x, float y, int button /* 0 move, 1 down, 2 up */, float wheel);
// Controller: phys = every button down now, nav = OVNAV_* now. Returns the buttons the game must not see (all of them
// while the overlay is open). Opens / closes the overlay when the combo has been held for hold_ms.
uint32_t uit_ov_pad(UitOv *o, uint32_t phys, uint32_t nav, uint64_t now_ms);
// [UIT 0271] a second combo (0 = none): either one held alone for hold_ms opens / closes the overlay
void uit_ov_set_combo2(UitOv *o, uint32_t combo2);
// [UIT 0270] the face buttons in phys (1 << SDL_GamepadButton: south 0, east 1, west 2, north 3) as OVNAV_CONFIRM /
// BACK / RESET / Y; confirm_game: the right (east) button confirms and the bottom (south) one goes back
uint32_t uit_ov_face_nav(uint32_t phys, int confirm_game);
// [UIT 0270] what a face button is called on a controller family (UIT_PADFAM_*): pos 0 bottom, 1 right, 2 left, 3 top
const char *uit_ov_face_label(int family, int pos);
float uit_ov_combo_progress(const UitOv *o, uint64_t now_ms);   // 0 = no hint shown; else 0..1
// Saves changes a short while after the last one (and at once when the overlay closes). 1 = wrote the file.
int uit_ov_tick(UitOv *o, uint64_t now_ms);
int uit_ov_flush(UitOv *o, uint64_t now_ms);

// ---- [UIT 0174] the Controls tab. The overlay's own keys and buttons are physical (arrows / WASD, Enter, Esc, the
// D-pad, the labelled A / B / X, LB / RB, the combo and the hotkey), so no binding can lock you out of it.
void uit_ov_set_binds(UitOv *o, const UitBinds *b);   // the bindings in use (at start; the tab edits its copy)
// Capturing: 0, or 1 + UIT_BIND_PAD / UIT_BIND_KEY. While a keyboard capture runs every key goes to
// uit_ov_capture_key except Esc (uit_ov_capture_cancel), Delete (uit_ov_capture_clear) and the hotkey (closes).
int uit_ov_capturing(const UitOv *o);
void uit_ov_capture_start(UitOv *o, int kind, int button);
void uit_ov_capture_key(UitOv *o, int sdl_scancode, const char *key_name);
void uit_ov_capture_cancel(UitOv *o, const char *why);   // "Cancelled", "No press in time"
void uit_ov_capture_clear(UitOv *o);                      // nothing presses that Switch button now ("none")
void uit_ov_capture_refuse(UitOv *o, const char *why);   // keeps waiting, says why that press does not bind
// The Switch button's binding back to the default (button < 0: all of that kind)
void uit_ov_bind_reset(UitOv *o, int kind, int button);

// ---- [UIT 0177] the keyboard dialog. While kb.active: uit_ov_pad / uit_ov_mouse / uit_ov_build go to it (the
// settings stay as they were underneath), uit_ov_pad returns ~0 (the game sees a controller at rest).
void uit_ov_kb_begin(UitOv *o, const UitSwkbdReq *r, uint64_t now_ms);   // what is held then acts once let go
void uit_ov_kb_text(UitOv *o, const char *utf8);      // typed (a key, an IME's commit, a paste): at the cursor, filtered
void uit_ov_kb_compose(UitOv *o, const char *utf8);   // an IME's composition ("" or NULL: none)
void uit_ov_kb_edit(UitOv *o, int kbe);               // UIT_KBE_*
void uit_ov_kb_key(UitOv *o, int row, int col);       // a grid key pressed (the controller's A, a click)
int uit_ov_kb_result(const UitOv *o, uint16_t *out);  // the text (UIT_SWKBD_LIMIT + 1 units); returns its length
void uit_ov_kb_end(UitOv *o, uint32_t phys_held);     // closed: what is held stays hidden from the game until let go
uint32_t uit_ov_kb_char(const UitOvKb *k, int row, int col);   // the character on a grid key (0: none / a special key)

// ---- [UIT 0175] a message in the footer from the glue (the file dialog's answer arrives after the click)
void uit_ov_note(UitOv *o, int bad, const char *msg);
#define UIT_OV_AM_ROWS(o) (2 + (o)->am_n)   // the Amiibo tab's rows

typedef struct {
    float w, h;                       // the swapchain / window size in pixels
    uint64_t now_ms;
    double fps;                       // the game's frame rate (the counter), < 0 = not known yet
    const char *hotkey, *pad_combo;   // e.g. "F1", "L3 + R3"
    const float *ft;                  // [UIT 0301] the graph: frame times in ms, oldest first (show_graph)
    int ftn;
    const char *bench, *bench2;       // [UIT 0302] "Benchmarking 42 s" / the result, and a second line (NULL: none)
} UitOvFrame;
// The quads to draw now (0 when nothing is shown). The atlas must be built for uit_ov_scale(w, h).
int uit_ov_build(UitOv *o, const UitOvAtlas *a, const UitOvFrame *f, UitOvQuad *q, int cap);
float uit_ov_scale(float w, float h);

#ifdef __cplusplus
}
#endif
