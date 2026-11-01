// [UIT 0171] The in-game overlay's core (src/host/uit_overlay.c) against the launcher (launcher/model.cpp, settings.ini):
//  1. every overlay setting matches its launcher/settings.ini entry (type, default, tokens, choices, range) and the
//     launcher path it quotes (Tab > section > group > name) is the one the launcher really shows;
//  2. cfg round trip: what the overlay writes for each value is byte for byte what the launcher writes (CfgFile::set +
//     write_value), on files with CRLF / LF / BOM / duplicates / no "# Your settings" header, and the launcher reads
//     the value back;
//  3. the input state machine: the controller combo (hold, cancel, latch, suppress), navigation repeat, tabs, mouse;
//  4. the font atlas and the layout: built once, crisp sizes at 4K, quad counts and how long it takes;
//  5. [UIT 0172] with a third argument, a CPU reference of the Vulkan pass's shaders draws previews (PPM) there;
//  6. [UIT 0174] the Controls tab: input_bind.c's names, tokens, labels and cfg lines against the launcher's, and the
//     capture (swap, timeout, cancel, clear, reset, refused keys and buttons, nothing reaching the game, the saved file);
//  7. [UIT 0175] the Amiibo tab (the folder's list, choose a file, scan, scan again, re-read, the reader's line, the mouse,
//     scrolling, no hooks / an empty folder) and the Cheats tab (two live switches saved like any setting);
//  8. [UIT 0177] the software keyboard dialog (typing, limits, filters, the controller grid, the mouse, OK / cancel, drawing).
// build (Linux SDL3 for model.cpp's file helpers):
//   cc -O2 -c -I<SDL3>/include src/host/uit_overlay.c -o /tmp/uit_overlay.o
//   cc -O2 -c -I<SDL3>/include src/host/input_bind.c -o /tmp/input_bind.o
//   cc -O2 -c src/host/swkbd.c -o /tmp/swkbd.o
//   c++ -O2 -std=c++17 -fno-exceptions -fno-rtti -nostdlib++ -Ilauncher -Isrc/host -I<SDL3>/include tools/tests/test_overlay.cpp \
//       launcher/model.cpp /tmp/uit_overlay.o /tmp/input_bind.o /tmp/swkbd.o -L<SDL3>/lib -lSDL3 -lm -o /tmp/test_overlay
//   (-nostdlib++: launcher/mini.h needs no C++ library)
//   /tmp/test_overlay launcher/settings.ini third_party/inter [/tmp/preview]
#include "model.h"
#include "uit_overlay.h"

#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <SDL3/SDL_keyboard.h>
#include <SDL3/SDL_scancode.h>

using namespace ut;
static int fails, checks;
#define CHECK(c) do { checks++; if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)
#define CHECKM(c, ...) do { checks++; if (!(c)) { printf("FAIL %s:%d %s: ", __FILE__, __LINE__, #c); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

static double now_ms() {
    timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}
static mini::string read_file(const char *p) {
    FILE *f = fopen(p, "rb");
    if (!f) return mini::string();
    mini::string s;
    char buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) s += mini::string(buf, n);
    fclose(f);
    return s;
}
static void write_file(const char *p, const mini::string &s) {
    FILE *f = fopen(p, "wb");
    fwrite(s.data(), 1, s.size(), f);
    fclose(f);
}
static const Setting *find(const mini::vector<Setting> &v, const char *k) {
    for (const auto &s : v) if (s.key == k) return &s;
    return nullptr;
}

// ---- 1. the launcher path, as launcher/main.cpp builds its tabs (kTabNames, section, folded "More x settings")
static const char *kTabKey[] = {"", "graphics", "performance", "game", "mods", "advanced", ""};
static const char *kTabName[] = {"Play", "Graphics", "Performance", "Game", "Mods", "Advanced", "About"};
static mini::string launcher_path(const mini::vector<Setting> &S, const Setting &s) {
    mini::string p;
    for (int t = 0; t < 7; t++) if (s.tab == kTabKey[t] && *kTabKey[t]) p = kTabName[t];
    if (!s.section.empty()) p += " > " + s.section;
    mini::string g = s.group;
    if (s.groupFolded) {
        bool sibling = false;   // a visible "X" in the same tab and section: the folded part reads "More x settings"
        for (const auto &o : S) if (o.tab == s.tab && o.section == s.section && !o.groupFolded && o.group == s.group) sibling = true;
        if (sibling) {
            mini::string low = g;
            if (!low.empty() && low[0] >= 'A' && low[0] <= 'Z') low[0] = (char)(low[0] - 'A' + 'a');
            if (low.size() > 2 && low.back() == 's' && low[low.size() - 2] != 's') low.pop_back();
            g = "More " + low + " settings";
        }
    }
    return p + " > " + g + " > " + s.name;
}

static Type type_of(UitOvKind k) {
    return k == UIT_OVS_BOOL ? Type::Bool : k == UIT_OVS_CHOICE ? Type::Choice : k == UIT_OVS_INT ? Type::Int : Type::Float;
}
static mini::string str(const char *s) { return s ? mini::string(s) : mini::string(); }

static void test_table(const mini::vector<Setting> &S) {
    int keyed = 0;
    for (int i = 0; i < OV_COUNT; i++) {
        const UitOvSetting *o = &kUitOvSettings[i];
        CHECK(o->id == i);
        CHECKM(o->name && *o->name, "id %d", i);
        if (o->kind != UIT_OVS_INFO) CHECKM(o->help && *o->help && !strchr(o->help, '\n'), "%s: one line of help", o->name);
        if (o->help) CHECKM(strlen(o->help) <= 100, "%s: help is %zu chars", o->name, strlen(o->help));
        if (!o->key) continue;
        keyed++;
        const Setting *s = find(S, o->key);
        CHECKM(s, "%s is not in settings.ini", o->key);
        if (!s) continue;
        if (i == OV_FSR) {   // the launcher's Upscaler choice; the overlay's on / off is that choice's "fsr" position
            CHECK(s->type == Type::Choice);
            bool has_fsr = false;
            for (const auto &c : s->choices) if (c.value == "fsr") { has_fsr = true; CHECKM(launcher_path(S, *s) + ": " + c.label == o->launcher, "%s", (launcher_path(S, *s) + ": " + c.label).c_str()); }
            CHECK(has_fsr);
            continue;
        }
        CHECKM(s->type == type_of(o->kind), "%s type", o->key);
        CHECKM(s->def == o->def, "%s default %s vs %s", o->key, s->def.c_str(), o->def);
        CHECKM(launcher_path(S, *s) == o->launcher, "%s: \"%s\" vs the launcher's \"%s\"", o->key, o->launcher, launcher_path(S, *s).c_str());
        if (o->kind == UIT_OVS_BOOL) CHECKM(s->on == o->on && s->off == o->off, "%s on/off", o->key);
        if (o->kind == UIT_OVS_CHOICE) {   // every overlay choice is one of the launcher's (values; labels may be shorter)
            char v[64], l[96];
            for (int c = 0; uit_ov_choice(o, c, v, sizeof v, l, sizeof l); c++) {
                bool found = false;
                for (const auto &lc : s->choices) if (lc.value == v) found = true;
                CHECKM(found, "%s: choice %s not in the launcher", o->key, v);
            }
            CHECKM(uit_ov_choice_count(o) == (int)s->choices.size(), "%s: %d choices vs the launcher's %d", o->key,
                   uit_ov_choice_count(o), (int)s->choices.size());
        }
        if (o->kind == UIT_OVS_INT || o->kind == UIT_OVS_FLOAT) {
            CHECKM(s->min == o->min && s->max == o->max && fabs(s->step - o->step) < 1e-9, "%s range %g..%g/%g vs %g..%g/%g",
                   o->key, o->min, o->max, o->step, s->min, s->max, s->step);
            CHECKM(s->autoLabel == str(o->autolabel), "%s auto label", o->key);
            CHECKM(s->zero == str(o->zero), "%s zero \"%s\" vs \"%s\"", o->key, str(o->zero).c_str(), s->zero.c_str());
            CHECKM(s->unit == str(o->unit), "%s unit \"%s\" vs \"%s\"", o->key, str(o->unit).c_str(), s->unit.c_str());
        }
    }
    printf("table: %d overlay rows, %d saved to keys that the launcher shows\n", OV_COUNT, keyed);
}

// ---- 2. cfg round trip
static Value launcher_value(const Setting &s, const UitOvSetting *o, double v) {
    Value r;
    switch (s.type) {
    case Type::Bool: r.b = v >= 0.5; break;
    case Type::Choice: {
        char val[64];
        uit_ov_choice(o, (int)v, val, sizeof val, NULL, 0);
        for (int i = 0; i < (int)s.choices.size(); i++) if (s.choices[i].value == val) r.index = i;
        break;
    }
    default:
        if (v < s.min && !s.autoLabel.empty()) r.kind = Value::Auto, r.num = s.min;
        else r.num = v;
    }
    return r;
}

static const char *kFiles[] = {
    "",
    "# made by tools/setup.py\r\nexefs=C:\\TotK\\exefs\r\n\r\n# Your settings (kept when tools/setup.py runs again):\r\nTOTK_FPS=60\r\n",
    "\xEF\xBB\xBFromfs=D:/game/romfs\n# Your settings (kept when tools/setup.py runs again):\nTOTK_VOLUME=40\nTOTK_RUMBLE=0\n",
    "romfs=x\nTOTK_TEX_BUDGET_MB=64\nfoo bar\nTOTK_TEX_BUDGET_MB=128\nTOTK_UPSCALER=fsr\nTOTK_FSR=1",   // dupes, no final newline
    "TOTK_GYRO_DEADZONE=1.5\r\nTOTK_GYRO=0\r\nTOTK_UPSCALER=dlss\r\n",
};

static void test_roundtrip(const mini::vector<Setting> &S) {
    const char *pa = "/tmp/uit_test_overlay_a.cfg", *pb = "/tmp/uit_test_overlay_b.cfg";
    int cases = 0;
    for (const char *file : kFiles) {
        for (int i = 0; i < OV_COUNT; i++) {
            const UitOvSetting *o = &kUitOvSettings[i];
            if (!o->key) continue;
            const Setting *s = find(S, o->key);
            if (!s) continue;
            // the values a player can pick in the overlay
            mini::vector<double> vals;
            if (o->kind == UIT_OVS_BOOL) vals = {0, 1};
            else if (o->kind == UIT_OVS_CHOICE) for (int c = 0; c < uit_ov_choice_count(o); c++) vals.push_back(c);
            else {
                if (o->autolabel) vals.push_back(o->min - 1);
                for (double v = o->min; v <= o->max + 1e-9; v += o->step * (o->kind == UIT_OVS_INT && o->max > 200 ? 37 : 1)) vals.push_back(v);
                vals.push_back(atof(o->def));
            }
            for (double v : vals) for (int dlss = 0; dlss < 2; dlss++) {
                UitCfgEdit e[4];
                int n = uit_ov_cfg_edits(i, v, dlss, e, 4);
                mini::string in(file);
                // the overlay: in memory, then through the real file path (tmp + rename)
                char *out = nullptr;
                size_t outn = 0;
                CHECK(uit_cfg_edit_text(in.data(), in.size(), e, n, &out, &outn));
                write_file(pa, in);
                char err[256] = "";
                CHECKM(uit_cfg_apply(pa, e, n, err, sizeof err), "%s", err);
                mini::string ov = read_file(pa);
                CHECK(ov == mini::string(out, outn));
                free(out);
                // the launcher: CfgFile::set(key, write_value) (FSR: main.cpp's Upscaler and TOTK_FSR in step)
                write_file(pb, in);
                CfgFile c;
                c.load(pb);
                if (i == OV_FSR) {
                    bool on = v >= 0.5;
                    if (dlss) c.set("TOTK_FSR", on ? mini::optional<mini::string>() : mini::optional<mini::string>(mini::string("0")));
                    else {
                        c.set("TOTK_UPSCALER", on ? mini::optional<mini::string>(mini::string("fsr")) : mini::optional<mini::string>());
                        c.set("TOTK_FSR", on ? mini::optional<mini::string>(mini::string("1")) : mini::optional<mini::string>());
                    }
                } else
                    c.set(o->key, write_value(*s, launcher_value(*s, o, v)));
                mini::string lerr;
                CHECK(c.save(&lerr));
                mini::string la = read_file(pb);
                CHECKM(ov == la, "%s=%g on file %d: overlay\n[%s]\nlauncher\n[%s]", o->key, v, (int)(&file - kFiles), ov.c_str(), la.c_str());
                // the launcher reads the overlay's file back as the value picked
                CfgFile back;
                back.load(pa);
                if (i != OV_FSR) {
                    Value got = read_value(*s, back.get(o->key)), want = launcher_value(*s, o, v);
                    CHECKM(same(*s, got, want), "%s=%g reads back as %s", o->key, v, display(*s, got).c_str());
                } else if (!dlss) {
                    mini::optional<mini::string> up = back.get("TOTK_UPSCALER");
                    CHECK((v >= 0.5) == (up && *up == "fsr"));
                }
                cases++;
            }
        }
    }
    remove(pa);
    remove(pb);
    printf("cfg round trip: %d cases, byte for byte the launcher's file\n", cases);
}

// ---- 3. the input state machine
static double g_val[OV_COUNT];
static int g_sets;
static double h_get(int id) { return g_val[id]; }
static void h_set(int id, double v) { g_val[id] = v; g_sets++; }
static const char *h_why(int id) { return id == OV_TEXBUD ? "not live in this run" : NULL; }
static int h_shown(int id) { return id != OV_DLSS; }
static int h_choice_ok(int id, int i) { return !(id == OV_FPS && i == 0); }
static const char *h_cfg(void) { return ""; }
static const UitOvHooks kHooks = {h_get, h_set, h_why, h_shown, NULL, NULL, h_choice_ok, h_cfg, NULL};

static const uint32_t L3 = 1u << 7, R3 = 1u << 8, A = 1u << 0, B = 1u << 1;   // SDL_GAMEPAD_BUTTON_LEFT_STICK = 7

static int sel_id(UitOv *o) { return o->rows[o->sel[o->tab]]; }

static void test_state() {
    UitOv o;
    uit_ov_init(&o, &kHooks, L3 | R3, 1000);
    uint64_t t = 1000;
    // a lone stick click is the game's (crouch / scope): passes, nothing shown
    CHECK(uit_ov_pad(&o, L3, 0, t) == 0 && o.cstate == OVC_IDLE);
    CHECK(uit_ov_pad(&o, 0, 0, t += 16) == 0);
    // both held: hidden from the game, ring after 150 ms, opens at 1 s
    CHECK((uit_ov_pad(&o, L3 | R3, 0, t += 16) & (L3 | R3)) == (L3 | R3) && o.cstate == OVC_HOLD);
    uint64_t t0 = t;
    CHECK(uit_ov_combo_progress(&o, t0 + 100) == 0);
    CHECK(fabsf(uit_ov_combo_progress(&o, t0 + 500) - 0.5f) < 0.01f);
    uit_ov_pad(&o, L3 | R3, 0, t0 + 999);
    CHECK(!o.open);
    CHECK(uit_ov_pad(&o, L3 | R3, 0, t0 + 1000) == 0xFFFFFFFFu && o.open);
    t = t0 + 1000;
    // still held after opening: no toggle back, everything masked
    for (int k = 0; k < 200; k++) CHECK(uit_ov_pad(&o, L3 | R3, 0, t += 16) == 0xFFFFFFFFu && o.open);
    CHECK(uit_ov_pad(&o, 0, 0, t += 16) == 0xFFFFFFFFu && o.cstate == OVC_IDLE);
    // navigation: down once on press, repeat after 380 ms then every 90 ms
    int s0 = o.sel[o.tab];
    uit_ov_pad(&o, 0, OVNAV_DOWN, t += 16);
    CHECK(o.sel[o.tab] == s0 + 1);
    uit_ov_pad(&o, 0, OVNAV_DOWN, t += 300);
    CHECK(o.sel[o.tab] == s0 + 1);
    uit_ov_pad(&o, 0, OVNAV_DOWN, t += 100);   // 400 ms: first repeat
    CHECK(o.sel[o.tab] == s0 + 2);
    uit_ov_pad(&o, 0, OVNAV_DOWN, t += 90);
    CHECK(o.sel[o.tab] == s0 + 3);
    uit_ov_pad(&o, 0, 0, t += 16);
    // tabs wrap: LB from Display goes to Overlay, RB back
    CHECK(o.tab == UIT_OV_TAB_DISPLAY);
    uit_ov_pad(&o, 0, OVNAV_TABL, t += 16);
    CHECK(o.tab == UIT_OV_TAB_OVERLAY);
    uit_ov_pad(&o, 0, 0, t += 16);
    uit_ov_pad(&o, 0, OVNAV_TABR, t += 16);
    CHECK(o.tab == UIT_OV_TAB_DISPLAY);
    uit_ov_pad(&o, 0, 0, t += 16);
    // the DLSS row is hidden (shown() = 0)
    for (int r = 0; r < o.nrows; r++) CHECK(o.rows[r] != OV_DLSS);
    // A toggles FSR (row 0), right on a bool sets on, left off
    o.sel[o.tab] = 0;
    CHECK(sel_id(&o) == OV_FSR);
    g_val[OV_FSR] = 0;
    uit_ov_pad(&o, A, OVNAV_CONFIRM, t += 16);
    CHECK(g_val[OV_FSR] == 1 && o.pending[OV_FSR]);
    uit_ov_pad(&o, 0, 0, t += 16);
    uit_ov_input(&o, OVIN_RIGHT);
    CHECK(g_val[OV_FSR] == 1);
    uit_ov_input(&o, OVIN_LEFT);
    CHECK(g_val[OV_FSR] == 0);
    // frame rate: choice 0 (30) is skipped while playing; the arrows stop at the ends
    uit_ov_set_open(&o, 1, 0);
    o.tab = UIT_OV_TAB_PERF;
    uit_ov_input(&o, OVIN_UP);
    o.sel[o.tab] = 0;
    CHECK(sel_id(&o) == OV_FPS);
    g_val[OV_FPS] = 1;
    uit_ov_input(&o, OVIN_LEFT);
    CHECK(g_val[OV_FPS] == 1);
    uit_ov_input(&o, OVIN_RIGHT);
    CHECK(g_val[OV_FPS] == 2);
    uit_ov_input(&o, OVIN_RIGHT);
    CHECK(g_val[OV_FPS] == 2);
    uit_ov_input(&o, OVIN_CONFIRM);   // wraps, skipping 30
    CHECK(g_val[OV_FPS] == 1);
    // a greyed-out row does not change and says why
    for (int r = 0; r < o.nrows; r++) if (o.rows[r] == OV_TEXBUD) o.sel[o.tab] = r;
    g_val[OV_TEXBUD] = 256;
    int sets = g_sets;
    uit_ov_input(&o, OVIN_RIGHT);
    CHECK(g_sets == sets && strstr(o.status, "not live"));
    // numbers: steps, clamps, fast repeat
    o.tab = UIT_OV_TAB_GAME;
    uit_ov_input(&o, OVIN_UP);
    o.sel[o.tab] = 0;
    CHECK(sel_id(&o) == OV_VOLUME);
    g_val[OV_VOLUME] = 95;
    uit_ov_input(&o, OVIN_RIGHT);
    CHECK(g_val[OV_VOLUME] == 100);
    uit_ov_input(&o, OVIN_RIGHT);
    CHECK(g_val[OV_VOLUME] == 100);
    // the gyro deadzone: Auto below min, right goes to 0, left from 0 back to Auto
    for (int r = 0; r < o.nrows; r++) if (o.rows[r] == OV_GYRODZ) o.sel[o.tab] = r;
    g_val[OV_GYRODZ] = -1;
    uit_ov_input(&o, OVIN_RIGHT);
    CHECK(g_val[OV_GYRODZ] == 0);
    uit_ov_input(&o, OVIN_RIGHT);
    CHECK(g_val[OV_GYRODZ] == 0.5);
    uit_ov_input(&o, OVIN_LEFT);
    uit_ov_input(&o, OVIN_LEFT);
    CHECK(g_val[OV_GYRODZ] < 0);
    // B closes; B stays hidden from the game until it is let go
    uit_ov_pad(&o, 0, 0, t += 16);
    CHECK(uit_ov_pad(&o, B, OVNAV_BACK, t += 16) == 0xFFFFFFFFu && !o.open);
    CHECK(uit_ov_pad(&o, B, 0, t += 16) == B);
    CHECK(uit_ov_pad(&o, B | A, 0, t += 16) == B);   // A passes
    CHECK(uit_ov_pad(&o, A, 0, t += 16) == 0);
    CHECK(uit_ov_pad(&o, B, 0, t += 16) == 0);       // a new B press is the game's
    uit_ov_pad(&o, 0, 0, t += 16);
    // the combo cancels when another button joins, and does not start while one is held (crouch + jump + R3 ...)
    uit_ov_pad(&o, L3 | R3, 0, t += 16);
    CHECK(o.cstate == OVC_HOLD);
    CHECK(uit_ov_pad(&o, L3 | R3 | A, 0, t += 16) == (L3 | R3) && o.cstate == OVC_DISARMED);
    uit_ov_pad(&o, L3 | R3, 0, t += 2000);
    CHECK(!o.open && o.cstate == OVC_DISARMED);
    uit_ov_pad(&o, 0, 0, t += 16);
    CHECK(o.cstate == OVC_IDLE);
    uit_ov_pad(&o, A | L3, 0, t += 16);
    uit_ov_pad(&o, A | L3 | R3, 0, t += 16);
    uit_ov_pad(&o, L3 | R3, 0, t += 16);
    uit_ov_pad(&o, L3 | R3, 0, t += 2000);
    CHECK(!o.open);
    uit_ov_pad(&o, 0, 0, t += 16);
    // released early: no open, the stick clicks stay hidden until let go (no stray crouch)
    uit_ov_pad(&o, L3 | R3, 0, t += 16);
    CHECK(uit_ov_pad(&o, L3, 0, t += 500) == L3 && !o.open);
    CHECK(uit_ov_pad(&o, 0, 0, t += 16) == 0);
    // closing with the combo: hidden until both are let go, then the game has them again
    uit_ov_pad(&o, L3 | R3, 0, t += 16);
    uit_ov_pad(&o, L3 | R3, 0, t += 1000);
    CHECK(o.open);
    uit_ov_pad(&o, 0, 0, t += 16);
    uit_ov_pad(&o, L3 | R3, 0, t += 16);
    uit_ov_pad(&o, L3 | R3, 0, t += 1000);
    CHECK(!o.open);
    CHECK(uit_ov_pad(&o, L3 | R3, 0, t += 16) == (L3 | R3));
    CHECK((uit_ov_pad(&o, R3, 0, t += 16) & R3) == R3);
    CHECK(uit_ov_pad(&o, 0, 0, t += 16) == 0);
    CHECK(uit_ov_pad(&o, R3, 0, t += 16) == 0);
    uit_ov_pad(&o, 0, 0, t += 16);
    // remapped / no combo
    UitOv k;
    uit_ov_init(&k, &kHooks, 0, 1000);
    CHECK(uit_ov_pad(&k, L3 | R3, 0, t) == 0);
    CHECK(uit_ov_pad(&k, L3 | R3, 0, t + 5000) == 0 && !k.open);
    uit_ov_init(&k, &kHooks, 1u << 15 /* misc1 */, 500);
    uit_ov_pad(&k, 1u << 15, 0, t);
    uit_ov_pad(&k, 1u << 15, 0, t + 500);
    CHECK(k.open);
    // session only: nothing pending is written, the status says so
    uit_ov_set_open(&k, 1, 0);
    k.save = 0;
    k.tab = UIT_OV_TAB_GAME;
    uit_ov_input(&k, OVIN_UP);
    g_val[OV_VOLUME] = 50;
    uit_ov_input(&k, OVIN_RIGHT);
    CHECK(k.pending[OV_VOLUME] && strstr(k.status, "session"));
    CHECK(uit_ov_tick(&k, t + 10000) == 0);
    // keyboard Esc (OVIN_BACK) closes; input is ignored while closed
    uit_ov_input(&k, OVIN_BACK);
    CHECK(!k.open);
    sets = g_sets;
    uit_ov_input(&k, OVIN_RIGHT);
    CHECK(g_sets == sets);
    printf("state machine: combo, cancel, latch, suppress, navigation, tabs, greyed rows, session-only\n");
}

// ---- 6. [UIT 0174] the Controls tab: input_bind.c against the launcher's bindings (model.cpp bind_list / write_value /
// pad_label / key_label, settings.ini TOTK_PAD_* / TOTK_KEY_*) and the capture's state machine
static UitBinds g_applied;
static int g_applies;
static char g_cfg_path[256];
static void h_binds(const UitBinds *b) { g_applied = *b; g_applies++; }
static const char *h_cfg_tmp(void) { return g_cfg_path; }
static const UitOvHooks kBindHooks = {h_get, h_set, h_why, h_shown, NULL, NULL, h_choice_ok, h_cfg_tmp, NULL, h_binds};
// [UIT 0270] the face buttons by position, for both Game > Controller > Confirm button layouts, and their labels;
// [UIT 0271] a second combo (the Guide button alone) next to L3 + R3
static void test_face_guide() {
    const uint32_t S = 1u << 0, E = 1u << 1, W = 1u << 2, N = 1u << 3, G = 1u << 5;
    CHECK(uit_ov_face_nav(S, 0) == OVNAV_CONFIRM && uit_ov_face_nav(E, 0) == OVNAV_BACK);
    CHECK(uit_ov_face_nav(E, 1) == OVNAV_CONFIRM && uit_ov_face_nav(S, 1) == OVNAV_BACK);
    CHECK(uit_ov_face_nav(W, 0) == OVNAV_RESET && uit_ov_face_nav(W, 1) == OVNAV_RESET);
    CHECK(uit_ov_face_nav(N, 0) == OVNAV_Y && uit_ov_face_nav(N, 1) == OVNAV_Y);
    CHECK(uit_ov_face_nav(S | E, 1) == (OVNAV_CONFIRM | OVNAV_BACK) && uit_ov_face_nav(G | L3, 0) == 0);
    CHECK(!strcmp(uit_ov_face_label(UIT_PADFAM_XBOX, 0), "A") && !strcmp(uit_ov_face_label(UIT_PADFAM_XBOX, 1), "B"));
    CHECK(!strcmp(uit_ov_face_label(UIT_PADFAM_NINTENDO, 0), "B") && !strcmp(uit_ov_face_label(UIT_PADFAM_NINTENDO, 1), "A"));
    CHECK(!strcmp(uit_ov_face_label(UIT_PADFAM_PS, 0), "Cross") && !strcmp(uit_ov_face_label(UIT_PADFAM_PS, 1), "Circle"));
    CHECK(!strcmp(uit_ov_face_label(UIT_PADFAM_GENERIC, 2), "X") && !strcmp(uit_ov_face_label(99, 3), "Y"));
    // the overlay's own confirm / back: with the game's layout the right button confirms, the bottom one closes
    UitOv o;
    uit_ov_init(&o, &kHooks, L3 | R3, 1000);
    o.confirm_game = 1;
    uint64_t t = 1000;
    uit_ov_set_open(&o, 1, 0);
    uit_ov_pad(&o, 0, 0, t += 16);
    uit_ov_pad(&o, S, uit_ov_face_nav(S, o.confirm_game), t += 16);
    CHECK(!o.open);
    // Guide alone, held 1 s: opens; the game never sees it; L3 + R3 still works next to it
    uit_ov_init(&o, &kHooks, L3 | R3, 1000);
    uit_ov_set_combo2(&o, G);
    CHECK(o.combo2 == G);
    t = 5000;
    CHECK(uit_ov_pad(&o, G, 0, t) == G && o.cstate == OVC_HOLD && o.ccur == G);
    CHECK(uit_ov_pad(&o, G, 0, t + 999) == G && !o.open);
    uit_ov_pad(&o, G, 0, t + 1000);
    CHECK(o.open && o.cstate == OVC_LATCHED);
    CHECK(uit_ov_pad(&o, 0, 0, t += 1100) == 0xFFFFFFFFu && o.cstate == OVC_IDLE);
    uit_ov_pad(&o, G, 0, t += 16);   // and closes the same way, the Guide press hidden until let go
    uit_ov_pad(&o, G, 0, t += 1000);
    CHECK(!o.open && uit_ov_pad(&o, G, 0, t += 16) == G);
    CHECK(uit_ov_pad(&o, 0, 0, t += 16) == 0 && o.cstate == OVC_IDLE);
    // released early: nothing opens
    uit_ov_pad(&o, G, 0, t += 16);
    CHECK(uit_ov_pad(&o, 0, 0, t += 400) == 0 && !o.open && o.cstate == OVC_IDLE);
    // L3 + R3 still opens
    uit_ov_pad(&o, L3 | R3, 0, t += 16);
    uit_ov_pad(&o, L3 | R3, 0, t += 1000);
    CHECK(o.open && o.ccur == (L3 | R3));
    uit_ov_pad(&o, 0, 0, t += 16);
    uit_ov_set_open(&o, 0, 0);
    uit_ov_pad(&o, 0, 0, t += 16);
    // Guide with another button: not a hold (Guide + A in a game's own chord)
    uit_ov_pad(&o, G | S, 0, t += 16);
    CHECK(o.cstate == OVC_DISARMED);
    uit_ov_pad(&o, G, 0, t += 2000);
    CHECK(!o.open && o.cstate == OVC_DISARMED);
    uit_ov_pad(&o, 0, 0, t += 16);
    CHECK(o.cstate == OVC_IDLE);
    // L3 held (crouching) then Guide: L3 is the game's, Guide cannot start (another button is down)
    uit_ov_pad(&o, L3, 0, t += 16);
    CHECK(o.cstate == OVC_IDLE);
    uit_ov_pad(&o, L3 | G, 0, t += 16);
    CHECK(o.cstate == OVC_DISARMED);
    uit_ov_pad(&o, 0, 0, t += 16);
    // Guide being held, then another button: cancelled, Guide hidden until let go
    uit_ov_pad(&o, G, 0, t += 16);
    CHECK(o.cstate == OVC_HOLD);
    CHECK((uit_ov_pad(&o, G | S, 0, t += 300) & G) == G && o.cstate == OVC_DISARMED);
    uit_ov_pad(&o, G | S, 0, t += 2000);
    CHECK(!o.open);
    uit_ov_pad(&o, 0, 0, t += 16);
    // the same combo twice is one
    uit_ov_set_combo2(&o, L3 | R3);
    CHECK(o.combo2 == 0);
}

static const uint32_t PAD_SOUTH = 1u << 0, PAD_EAST = 1u << 1, PAD_WEST = 1u << 2, PAD_MISC1 = 1u << 15;

static mini::string list_tokens(const UitBindList &l, int kind) {
    mini::vector<mini::string> v;
    for (int k = 0; k < l.n; k++) v.push_back(uit_bind_token(kind, l.in[k]));
    return bind_join(v);
}
static const char *g_env_key, *g_env_val;
static const char *env_one(const char *k) { return g_env_key && !strcmp(k, g_env_key) ? g_env_val : NULL; }

static void test_binds(const mini::vector<Setting> &S) {
    // the table: each Switch button's two settings.ini entries (name, type, default, what it does), its launcher path
    for (int kind = 0; kind < 2; kind++)
        for (int b = 0; b < UIT_BIND_BUTTONS; b++) {
            const Setting *s = find(S, uit_bind_cfg_key(kind, b));
            CHECKM(s, "%s", uit_bind_cfg_key(kind, b));
            if (!s) continue;
            CHECK(s->type == (kind == UIT_BIND_PAD ? Type::Pad : Type::Key));
            CHECKM(s->name == uit_bind_button_name(b), "%s: %s", s->key.c_str(), uit_bind_button_name(b));
            mini::string does = mini::string("(") + uit_bind_button_does(b) + ").";
            CHECKM(s->summary.size() > does.size() && s->summary.substr(s->summary.size() - does.size()) == does, "%s: %s", s->key.c_str(), does.c_str());
            mini::string want = mini::string(kind == UIT_BIND_PAD ? "Game > Controller buttons > " : "Game > Keyboard keys > ") + s->name;
            CHECKM(launcher_path(S, *s) == want, "%s", launcher_path(S, *s).c_str());
            UitBinds d;
            uit_binds_default(&d);
            CHECK(list_tokens(d.b[kind][b], kind) == bind_join(bind_list(s->def, kind == UIT_BIND_PAD)));
            char v[128];
            CHECK(uit_binds_cfg_value(&d, kind, b, v, sizeof v) == 0 && !write_value(*s, default_value(*s)));
        }
    // the labels: as the launcher names each input, for every controller family
    PadFamily fams[4] = {PadFamily::Generic, PadFamily::Xbox, PadFamily::PlayStation, PadFamily::Nintendo};
    for (int fam = 0; fam < 4; fam++) {
        pad_family = fams[fam];
        for (int i = 0; i < uit_bind_input_count(UIT_BIND_PAD); i++)
            CHECKM(pad_label(uit_bind_token(UIT_BIND_PAD, i)) == uit_bind_label(UIT_BIND_PAD, i, fam), "%s", uit_bind_token(UIT_BIND_PAD, i));
    }
    pad_family = PadFamily::Generic;
    for (int i = 0; i < uit_bind_input_count(UIT_BIND_KEY); i++)
        CHECKM(key_label(uit_bind_token(UIT_BIND_KEY, i)) == uit_bind_label(UIT_BIND_KEY, i, 0), "%s", uit_bind_token(UIT_BIND_KEY, i));
    // parse and the saved line: byte for byte what the launcher reads (bind_list) and writes (write_value)
    const char *pads[] = {"east", "East", "south,east", "south, south ,dpup", "none", "", "bogus", "bogus,north", "lefttrigger,righttrigger,misc1",
                          "a", "touchpad,guide,back,start", "leftstick,rightstick,leftshoulder,rightshoulder,dpdown,dpleft,dpright,west,north"};
    const char *keys[] = {"J", "j", "LShift,Space", "W", "W,K", "none", "Up", "Tab,Esc", "Semicolon", "bogus", "1,2,3", "K,K", "F,H,G,Z,X,C,V,B,Q,E,R,Y"};
    for (int kind = 0; kind < 2; kind++)
        for (int b = 0; b < UIT_BIND_BUTTONS; b++) {
            const Setting *s = find(S, uit_bind_cfg_key(kind, b));
            if (!s) continue;
            bool pad = kind == UIT_BIND_PAD;
            size_t nraw = pad ? sizeof pads / sizeof *pads : sizeof keys / sizeof *keys;
            for (size_t r = 0; r < nraw; r++) {
                const char *raw = pad ? pads[r] : keys[r];
                UitBindList l;
                int bad = uit_bind_parse(kind, b, raw, &l);
                mini::vector<mini::string> ll = bind_list(raw, pad);
                bool none = !strcmp(raw, "none");
                if (!ll.empty() || none) {   // the launcher's tokens, unless nothing usable was there (the game: the default)
                    UitBinds t;
                    uit_binds_default(&t);
                    t.b[kind][b] = l;
                    CHECKM(list_tokens(l, kind) == bind_join(ll) || (!pad && bad), "%s %s: %s / %s", s->key.c_str(), raw,
                           list_tokens(l, kind).c_str(), bind_join(ll).c_str());
                    Value v = read_value(*s, mini::string(raw));
                    mini::optional<mini::string> w = write_value(*s, v);
                    char out[128];
                    int has = uit_binds_cfg_value(&t, kind, b, out, sizeof out);
                    if (!bad) CHECKM(has == (bool)w && (!has || *w == out), "%s %s: %s / %s", s->key.c_str(), raw, has ? out : "(none)", w ? w->c_str() : "(none)");
                } else {
                    UitBinds d;
                    uit_binds_default(&d);
                    CHECK(uit_bind_list_eq(&l, &d.b[kind][b]));
                }
            }
        }
    // load: the game's start reads the same keys (and counts what it changed)
    {
        UitBinds b;
        g_env_key = "TOTK_PAD_A"; g_env_val = "south";
        CHECK(uit_binds_load(&b, env_one, 0) == 1 && b.b[UIT_BIND_PAD][0].n == 1 && !strcmp(uit_bind_token(UIT_BIND_PAD, b.b[UIT_BIND_PAD][0].in[0]), "south"));
        g_env_key = "TOTK_KEY_A"; g_env_val = "W";   // a stick key: unusable, the default stays
        CHECK(uit_binds_load(&b, env_one, 0) == 0 && uit_bind_is_default(&b, UIT_BIND_KEY, 0));
        g_env_key = NULL;
    }
    // swap semantics (launcher main.cpp bind_apply): A takes B's button, B takes A's old one
    {
        UitBinds b;
        uit_binds_default(&b);
        int south = -1, moved = -2;
        for (int i = 0; i < uit_bind_input_count(UIT_BIND_PAD); i++) if (!strcmp(uit_bind_token(UIT_BIND_PAD, i), "south")) south = i;
        CHECK(uit_binds_assign(&b, UIT_BIND_PAD, 0, south, &moved) == 1 && moved == 1);
        CHECK(list_tokens(b.b[UIT_BIND_PAD][0], 0) == "south" && list_tokens(b.b[UIT_BIND_PAD][1], 0) == "east");
        CHECK(uit_binds_assign(&b, UIT_BIND_PAD, 0, south, &moved) == 0 && moved == -1);   // already
        uint64_t pt[UIT_BIND_PAD_INPUTS], kt[128];
        uit_binds_compile(&b, pt, kt);
        CHECK(pt[south] == uit_bind_button_bit(0));
        uit_binds_reset(&b, UIT_BIND_PAD, -1);
        UitBinds d;
        uit_binds_default(&d);
        CHECK(!memcmp(&b, &d, sizeof b));
    }
    // the overlay hotkeys the launcher offers are never game keys (pressing one during a capture closes the overlay)
    {
        const Setting *hk = find(S, "TOTK_OVERLAY_KEY");
        CHECK(hk);
        if (hk)
            for (const auto &c : hk->choices) {
                mini::string n = c.value == "-" ? mini::string("F1") : c.value;
                SDL_Scancode sc = SDL_GetScancodeFromName(n.c_str());
                CHECKM(sc != SDL_SCANCODE_UNKNOWN && uit_bind_key_from_scancode(sc) < 0, "%s", n.c_str());
            }
        for (int f = SDL_SCANCODE_F7; f <= SDL_SCANCODE_F11; f++) CHECK(uit_bind_key_from_scancode(f) < 0);
    }

    // the capture, through the overlay
    snprintf(g_cfg_path, sizeof g_cfg_path, "/tmp/uit_test_binds_%d.cfg", (int)getpid());
    write_file(g_cfg_path, "# test\r\nTOTK_FPS=60\r\n");
    UitBinds d;
    uit_binds_default(&d);
    UitOv o;
    uit_ov_init(&o, &kBindHooks, L3 | R3, 1000);
    uit_ov_set_binds(&o, &d);
    o.pad_family = UIT_PADFAM_XBOX;
    uint64_t t = 1000;
    uit_ov_pad(&o, 0, 0, t);
    uit_ov_set_open(&o, 1, 0);
    o.tab = UIT_OV_TAB_GAME;
    uit_ov_input(&o, OVIN_TAB_NEXT);
    CHECK(o.tab == UIT_OV_TAB_CONTROLS && o.nrows == 0 && o.sel[o.tab] == 0 && o.bcol == UIT_BIND_PAD);
    // no controller: refused
    uit_ov_pad(&o, PAD_SOUTH, OVNAV_CONFIRM, t += 16);
    CHECK(!uit_ov_capturing(&o) && o.status_bad && strstr(o.status, "Connect a controller"));
    uit_ov_pad(&o, 0, 0, t += 16);
    o.pad_present = 1;
    // A (confirm) starts it; A still held does not bind; let go, press the bottom button: A = south, B swaps to east
    g_applies = 0;
    uit_ov_pad(&o, PAD_SOUTH, OVNAV_CONFIRM, t += 16);
    CHECK(uit_ov_capturing(&o) == 1 + UIT_BIND_PAD && o.cap_btn == 0);
    CHECK(uit_ov_pad(&o, PAD_SOUTH, OVNAV_CONFIRM, t += 16) == 0xFFFFFFFFu && uit_ov_capturing(&o));   // the game sees nothing
    uit_ov_pad(&o, 0, 0, t += 16);
    CHECK(uit_ov_capturing(&o));
    CHECK(uit_ov_pad(&o, PAD_SOUTH, OVNAV_CONFIRM, t += 16) == 0xFFFFFFFFu);
    CHECK(!uit_ov_capturing(&o) && g_applies == 1);
    CHECKM(!strcmp(o.status, "Switch A: A  \xc2\xb7  Switch B now uses B"), "%s", o.status);
    CHECK(list_tokens(g_applied.b[UIT_BIND_PAD][0], 0) == "south" && list_tokens(g_applied.b[UIT_BIND_PAD][1], 0) == "east");
    CHECK(o.bpending[UIT_BIND_PAD][0] && o.bpending[UIT_BIND_PAD][1]);
    uit_ov_pad(&o, PAD_SOUTH, OVNAV_CONFIRM, t += 16);   // still held after binding: does not start another capture
    CHECK(!uit_ov_capturing(&o));
    uit_ov_pad(&o, 0, 0, t += 16);
    // saved as the launcher writes it, then the launcher reads it back
    uit_ov_tick(&o, t += 500);
    {
        CfgFile c;
        CHECK(c.load(g_cfg_path));
        auto a = c.get("TOTK_PAD_A"), bb = c.get("TOTK_PAD_B");
        CHECK(a && *a == "south" && bb && *bb == "east" && c.get("TOTK_FPS"));
        CHECK(c.crlf);
    }
    // a timeout leaves it unchanged
    uit_ov_input(&o, OVIN_DOWN);   // B
    uit_ov_input(&o, OVIN_CONFIRM);
    CHECK(uit_ov_capturing(&o) && o.cap_btn == 1);
    uit_ov_tick(&o, t += UIT_OV_BIND_MS - 10);
    CHECK(uit_ov_capturing(&o));
    uit_ov_tick(&o, t += 20);
    CHECKM(!uit_ov_capturing(&o) && !strcmp(o.status, "No press in time: Switch B is unchanged"), "%s", o.status);
    // cancel (Esc / right click / a click) and clear (Delete)
    uit_ov_input(&o, OVIN_CONFIRM);
    uit_ov_capture_cancel(&o, "Cancelled");
    CHECK(!uit_ov_capturing(&o) && !strcmp(o.status, "Cancelled: Switch B is unchanged"));
    uit_ov_input(&o, OVIN_CONFIRM);
    uit_ov_capture_clear(&o);
    CHECKM(!uit_ov_capturing(&o) && o.binds.b[UIT_BIND_PAD][1].n == 0 &&
           !strcmp(o.status, "Switch B is not bound on the controller now (Del / X on the row puts the default back)"), "%s", o.status);
    // Delete / X: the default again; with a warning when another Switch button took it in a swap
    uit_ov_pad(&o, PAD_WEST, OVNAV_RESET, t += 16);
    uit_ov_pad(&o, 0, 0, t += 16);
    CHECKM(list_tokens(o.binds.b[UIT_BIND_PAD][1], 0) == "south" && o.status_bad && strstr(o.status, "Switch A uses it too"), "%s", o.status);
    // the overlay's own combo, when it is one button, cannot be bound (the game never sees it)
    o.combo = PAD_MISC1;
    uit_ov_input(&o, OVIN_CONFIRM);
    uit_ov_pad(&o, PAD_MISC1, 0, t += 16);
    CHECKM(uit_ov_capturing(&o) && o.status_bad && strstr(o.status, "Overlay controller combo"), "%s", o.status);
    uit_ov_pad(&o, 0, 0, t += 16);
    uit_ov_pad(&o, PAD_EAST, OVNAV_BACK, t += 16);   // B binds here (every button does): no close
    CHECK(o.open && !uit_ov_capturing(&o) && list_tokens(o.binds.b[UIT_BIND_PAD][1], 0) == "east");
    uit_ov_pad(&o, 0, 0, t += 16);
    o.combo = L3 | R3;
    // [UIT 0271] the Guide button as the second combo (L3 + R3 or Guide): cannot be bound either
    uit_ov_set_combo2(&o, 1u << 5);
    uit_ov_input(&o, OVIN_CONFIRM);
    uit_ov_pad(&o, 1u << 5, 0, t += 16);
    CHECKM(uit_ov_capturing(&o) && o.status_bad && !strncmp(o.status, "Xbox opens the overlay", 22), "%s", o.status);
    uit_ov_capture_cancel(&o, "Cancelled");
    uit_ov_pad(&o, 0, 0, t += 16);
    uit_ov_set_combo2(&o, 0);
    // the keyboard column: stick keys and unknown keys are refused, a game key binds; B on the controller cancels
    uit_ov_input(&o, OVIN_RIGHT);
    CHECK(o.bcol == UIT_BIND_KEY);
    uit_ov_input(&o, OVIN_CONFIRM);
    CHECK(uit_ov_capturing(&o) == 1 + UIT_BIND_KEY);
    uit_ov_capture_key(&o, SDL_SCANCODE_W, "W");
    CHECKM(uit_ov_capturing(&o) && !strcmp(o.status, "W moves Link (the left stick), so it cannot press a button. Pick another key."), "%s", o.status);
    uit_ov_capture_key(&o, SDL_SCANCODE_UP, "Up");
    CHECKM(strstr(o.status, "moves the camera (the right stick), so it cannot press a button"), "%s", o.status);
    uit_ov_capture_key(&o, SDL_SCANCODE_F7, "F7");
    CHECKM(!strcmp(o.status, "F7 is not a key the game can read. Pick another one."), "%s", o.status);
    int kL = uit_bind_key_from_scancode(SDL_SCANCODE_L);
    CHECK(kL >= 0);
    uit_ov_capture_key(&o, SDL_SCANCODE_L, "L");
    CHECK(!uit_ov_capturing(&o) && o.binds.b[UIT_BIND_KEY][1].n == 1 && o.binds.b[UIT_BIND_KEY][1].in[0] == kL);
    uit_ov_input(&o, OVIN_CONFIRM);
    uit_ov_pad(&o, PAD_EAST, OVNAV_BACK, t += 16);
    CHECK(o.open && !uit_ov_capturing(&o) && strstr(o.status, "Cancelled"));
    uit_ov_pad(&o, 0, 0, t += 16);
    uit_ov_input(&o, OVIN_CONFIRM);
    uit_ov_pad(&o, PAD_WEST, OVNAV_RESET, t += 16);
    CHECK(!uit_ov_capturing(&o) && o.binds.b[UIT_BIND_KEY][1].n == 0);
    uit_ov_pad(&o, 0, 0, t += 16);
    // a controller press while waiting for a key does not bind
    uit_ov_input(&o, OVIN_CONFIRM);
    uit_ov_pad(&o, PAD_SOUTH, OVNAV_CONFIRM, t += 16);
    CHECK(uit_ov_capturing(&o) == 1 + UIT_BIND_KEY && o.binds.b[UIT_BIND_KEY][1].n == 0);
    uit_ov_pad(&o, 0, 0, t += 16);
    // closing ends the capture with nothing changed
    uit_ov_set_open(&o, 0, 0);
    CHECK(!uit_ov_capturing(&o));
    uit_ov_set_open(&o, 1, 0);
    CHECK(o.tab == UIT_OV_TAB_CONTROLS);
    // "All buttons": Reset controller / Reset keyboard
    o.sel[o.tab] = UIT_BIND_BUTTONS;
    o.bcol = UIT_BIND_KEY;
    uit_ov_input(&o, OVIN_CONFIRM);
    CHECK(uit_bind_is_default(&o.binds, UIT_BIND_KEY, 1) && !uit_ov_capturing(&o) && strstr(o.status, "on the keyboard is back to its default"));
    o.bcol = UIT_BIND_PAD;
    uit_ov_input(&o, OVIN_RESET);
    for (int b = 0; b < UIT_BIND_BUTTONS; b++) CHECK(uit_bind_is_default(&o.binds, 0, b) && uit_bind_is_default(&o.binds, 1, b));
    CHECK(!memcmp(&g_applied, &o.binds, sizeof o.binds));
    uit_ov_flush(&o, t += 16);
    {
        CfgFile c;
        CHECK(c.load(g_cfg_path));
        CHECK(!c.get("TOTK_PAD_A") && !c.get("TOTK_PAD_B") && !c.get("TOTK_KEY_B") && c.get("TOTK_FPS"));
    }
    // saving off: live only
    o.save = 0;
    o.sel[o.tab] = 0;
    uit_ov_input(&o, OVIN_CONFIRM);
    uit_ov_pad(&o, 0, 0, t += 16);
    uit_ov_pad(&o, PAD_WEST, OVNAV_RESET, t += 16);
    CHECK(strstr(o.status, "this session only") && list_tokens(g_applied.b[0][0], 0) == "west");
    uit_ov_tick(&o, t += 1000);
    {
        CfgFile c;
        CHECK(c.load(g_cfg_path) && !c.get("TOTK_PAD_A"));
    }
    remove(g_cfg_path);
    printf("controls: 16 buttons x 2, labels for 4 controller families, capture / swap / timeout / cancel / clear / reset\n");
}

// ---- 4. atlas + layout + mouse (the layout's hit boxes)
// ---- 7. [UIT 0175] the Amiibo and Cheats tabs
static int g_am_files = 40, g_am_lists, g_am_picks, g_am_last_i = -99, g_am_scans, g_am_have_last, g_am_line_bad;
static char g_am_name[64][64];
static int h_am_list(int refresh) { (void)refresh; g_am_lists++; return g_am_files; }
static const char *h_am_file(int i) {
    if (i < 0 || i >= g_am_files || i >= 64) return NULL;
    snprintf(g_am_name[i], sizeof g_am_name[i], "Link (Archer)  \xc2\xb7  amiibo_%02d.bin", i);
    return g_am_name[i];
}
static void h_am_pick(void) { g_am_picks++; }
static int h_am_scan(int i, char *msg, size_t n) {
    g_am_scans++;
    g_am_last_i = i;
    if (i == 3) { snprintf(msg, n, "Not scanned, wrong size (amiibo_03.bin)"); return 1; }
    g_am_have_last = 1;
    snprintf(msg, n, "Scanned amiibo %d", i);
    return 0;
}
static const char *h_am_line(char *buf, size_t n, int *bad) {
    *bad = g_am_line_bad;
    snprintf(buf, n, "%s", g_am_line_bad ? "The game did not ask for an amiibo in 20 s" : "The game is asking for an amiibo: scan one now");
    return buf;
}
static const char *h_am_last(void) { return g_am_have_last ? "Link (Archer)" : NULL; }
static const char *h_am_dir(void) { return "amiibo"; }
static int h_am_nfiles(void) { return 3; }
static const UitOvHooks kAmHooks = {h_get, h_set, h_why, h_shown, NULL, NULL, h_choice_ok, h_cfg, NULL, NULL,
                                    h_am_list, h_am_file, h_am_pick, h_am_scan, h_am_line, h_am_last, h_am_dir};

static void test_amiibo_tab(const char *fontdir) {
    uint64_t t = 50000;
    UitOv o;
    uit_ov_init(&o, &kAmHooks, L3 | R3, 1000);
    uit_ov_set_open(&o, 1, 0);
    CHECK(g_am_lists == 0);   // nothing read until the tab is shown
    int lists = g_am_lists;
    while (o.tab != UIT_OV_TAB_AMIIBO) uit_ov_input(&o, OVIN_TAB_NEXT);
    CHECK(g_am_lists == lists + 1 && o.am_n == 40 && UIT_OV_AM_ROWS(&o) == 42);
    int *sel = &o.sel[UIT_OV_TAB_AMIIBO];
    *sel = 0;
    // row 0: the file dialog
    uit_ov_input(&o, OVIN_CONFIRM);
    CHECK(g_am_picks == 1 && g_am_scans == 0);
    // the glue's answer lands in the footer
    uit_ov_note(&o, 1, "No file chosen");
    CHECK(!strcmp(o.status, "No file chosen") && o.status_bad);
    // row 1: scan again, with nothing scanned yet: says so, asks nothing
    uit_ov_input(&o, OVIN_DOWN);
    CHECK(*sel == 1);
    uit_ov_input(&o, OVIN_CONFIRM);
    CHECK(g_am_scans == 0 && strstr(o.status, "Nothing scanned") && o.status_bad);
    // a file: its index, its message, not an error
    uit_ov_input(&o, OVIN_DOWN);
    uit_ov_input(&o, OVIN_DOWN);
    CHECK(*sel == 3);
    uit_ov_input(&o, OVIN_CONFIRM);
    CHECK(g_am_scans == 1 && g_am_last_i == 1 && !strcmp(o.status, "Scanned amiibo 1") && !o.status_bad);
    // a bad file: the reason, as an error
    *sel = 5;
    uit_ov_input(&o, OVIN_CONFIRM);
    CHECK(g_am_last_i == 3 && strstr(o.status, "wrong size") && o.status_bad);
    // scan again: index -1
    *sel = 1;
    uit_ov_input(&o, OVIN_CONFIRM);
    CHECK(g_am_last_i == -1 && g_am_scans == 3 && !o.status_bad);
    // the list's ends: no wrap
    *sel = 0;
    uit_ov_input(&o, OVIN_UP);
    CHECK(*sel == 0);
    for (int k = 0; k < 100; k++) uit_ov_input(&o, OVIN_DOWN);
    CHECK(*sel == 41);
    // Del / X reads the folder again: fewer files, the selection follows
    g_am_files = 5;
    uit_ov_input(&o, OVIN_RESET);
    CHECK(o.am_n == 5 && *sel == 6 && strstr(o.status, "5 amiibo files"));
    g_am_files = 1;
    uit_ov_input(&o, OVIN_RESET);
    CHECK(strstr(o.status, "1 amiibo file in"));
    g_am_files = 40;
    uit_ov_input(&o, OVIN_RESET);
    // [UIT 0181] with built-in figures after the files: the count says which are which
    {
        UitOvHooks hb = kAmHooks;
        hb.amiibo_files = h_am_nfiles;
        UitOv b;
        uit_ov_init(&b, &hb, L3 | R3, 1000);
        uit_ov_set_open(&b, 1, 0);
        while (b.tab != UIT_OV_TAB_AMIIBO) uit_ov_input(&b, OVIN_TAB_NEXT);
        uit_ov_input(&b, OVIN_RESET);
        CHECK(b.am_n == 40 && strstr(b.status, "3 amiibo files in the folder, 37 built-in figures"));
    }
    // other keys still work: tabs, close
    uit_ov_input(&o, OVIN_TAB_NEXT);
    CHECK(o.tab == UIT_OV_TAB_CHEATS);
    uit_ov_input(&o, OVIN_TAB_PREV);
    CHECK(o.tab == UIT_OV_TAB_AMIIBO);
    // reopening on the tab reads the folder again
    lists = g_am_lists;
    uit_ov_input(&o, OVIN_BACK);
    CHECK(!o.open);
    uit_ov_set_open(&o, 1, 0);
    CHECK(o.open && o.tab == UIT_OV_TAB_AMIIBO && g_am_lists == lists + 1);
    // a build without the reader's hooks: says so, nothing to call
    {
        UitOv n;
        uit_ov_init(&n, &kHooks, L3 | R3, 1000);
        uit_ov_set_open(&n, 1, 0);
        while (n.tab != UIT_OV_TAB_AMIIBO) uit_ov_input(&n, OVIN_TAB_NEXT);
        CHECK(n.am_n == 0 && UIT_OV_AM_ROWS(&n) == 2);
        uit_ov_input(&n, OVIN_CONFIRM);
        CHECK(strstr(n.status, "not available") && n.status_bad);
    }
    // the Cheats tab: two switches, off, live, saved like any setting (the launcher's Game > Cheats)
    {
        UitOv c;
        uit_ov_init(&c, &kHooks, L3 | R3, 1000);
        uit_ov_set_open(&c, 1, 0);
        while (c.tab != UIT_OV_TAB_CHEATS) uit_ov_input(&c, OVIN_TAB_NEXT);
        CHECK(c.nrows == 2 && c.rows[0] == OV_CHEAT_DUR && c.rows[1] == OV_CHEAT_AMIIBO);
        CHECK(!strcmp(kUitOvSettings[OV_CHEAT_DUR].key, "TOTK_CHEAT_DURABILITY") && !strcmp(kUitOvSettings[OV_CHEAT_AMIIBO].key, "TOTK_CHEAT_AMIIBO"));
        g_val[OV_CHEAT_DUR] = g_val[OV_CHEAT_AMIIBO] = 0;
        c.sel[c.tab] = 0;
        uit_ov_input(&c, OVIN_CONFIRM);
        CHECK(g_val[OV_CHEAT_DUR] == 1 && c.pending[OV_CHEAT_DUR]);
        uit_ov_input(&c, OVIN_DOWN);
        uit_ov_input(&c, OVIN_RIGHT);
        CHECK(g_val[OV_CHEAT_AMIIBO] == 1 && c.pending[OV_CHEAT_AMIIBO]);
        UitCfgEdit e[8];
        int ne = uit_ov_cfg_edits(OV_CHEAT_DUR, 1, 0, e, 8);
        CHECK(ne == 1 && !strcmp(e[0].key, "TOTK_CHEAT_DURABILITY") && !strcmp(e[0].value, "1"));
        ne = uit_ov_cfg_edits(OV_CHEAT_AMIIBO, 0, 0, e, 8);
        CHECK(ne == 1 && !strcmp(e[0].key, "TOTK_CHEAT_AMIIBO") && e[0].remove);   // off = the line goes (the launcher's "-")
        g_val[OV_CHEAT_DUR] = g_val[OV_CHEAT_AMIIBO] = 0;
    }
    // drawing and the mouse, at each size
    mini::string r = read_file((mini::string(fontdir) + "/Inter-Regular.ttf").c_str()),
                 b = read_file((mini::string(fontdir) + "/Inter-SemiBold.ttf").c_str());
    if (r.empty() || b.empty()) return;
    struct { float w, h; } sizes[] = {{1280, 720}, {1920, 1080}, {3840, 2160}};
    static UitOvQuad q[8192];
    for (auto sz : sizes) {
        UitOvAtlas a;
        CHECK(uit_ov_atlas_build(&a, (const unsigned char *)r.data(), (const unsigned char *)b.data(), uit_ov_scale(sz.w, sz.h)));
        UitOvFrame f = {sz.w, sz.h, t, 59.9, "F1", "L3 + R3"};
        UitOv m;
        uit_ov_init(&m, &kAmHooks, L3 | R3, 1000);
        uit_ov_set_open(&m, 1, 0);
        while (m.tab != UIT_OV_TAB_AMIIBO) uit_ov_input(&m, OVIN_TAB_NEXT);
        int *ms = &m.sel[UIT_OV_TAB_AMIIBO];
        for (int s0 = 0; s0 < UIT_OV_AM_ROWS(&m); s0 += 7) {   // every part of the list: the selected row is drawn, in the panel
            *ms = s0;
            g_am_line_bad = s0 & 1;
            int n = uit_ov_build(&m, &a, &f, q, 8192);
            CHECK(n > 0 && n <= 3072);
            for (int k = 0; k < n; k++) CHECK(q[k].x1 >= q[k].x0 && q[k].y1 >= q[k].y0 && q[k].x0 >= -1 && q[k].x1 <= sz.w + 1 && q[k].y1 <= sz.h + 1);
            int found = -1;
            for (int k = 0; k < m.am_nbox; k++)
                if (m.am_boxrow[k] == s0) found = k;
            CHECKM(found >= 0, "row %d", s0);
            if (found >= 0) CHECK(m.am_box[found].y0 >= m.panel.y0 && m.am_box[found].y1 <= m.panel.y1 + 1);
            CHECK(m.am_nbox > 3 && m.am_nbox <= UIT_OV_AM_VIS);
        }
        g_am_line_bad = 0;
        // the mouse: hover selects, the wheel moves, a click scans that row's file
        *ms = 0;
        uit_ov_build(&m, &a, &f, q, 8192);
        UitOvBox bx = m.am_box[4];
        int row = m.am_boxrow[4];
        uit_ov_mouse(&m, (bx.x0 + bx.x1) / 2, (bx.y0 + bx.y1) / 2, 0, 0);
        CHECK(*ms == row);
        uit_ov_mouse(&m, (bx.x0 + bx.x1) / 2, (bx.y0 + bx.y1) / 2, 0, -1);
        CHECK(*ms == row + 1);
        int scans = g_am_scans;
        uit_ov_build(&m, &a, &f, q, 8192);
        uit_ov_mouse(&m, (bx.x0 + bx.x1) / 2, (bx.y0 + bx.y1) / 2, 1, 0);
        CHECK(g_am_scans == scans + 1 && g_am_last_i == row - 2);
        // a click on the first row opens the dialog
        int picks = g_am_picks;
        *ms = 0;
        uit_ov_build(&m, &a, &f, q, 8192);
        uit_ov_mouse(&m, (m.am_box[0].x0 + m.am_box[0].x1) / 2, (m.am_box[0].y0 + m.am_box[0].y1) / 2, 1, 0);
        CHECK(m.am_boxrow[0] == 0 && g_am_picks == picks + 1);
        // an empty folder: the two rows and the folder's name
        g_am_files = 0;
        uit_ov_input(&m, OVIN_RESET);
        int n = uit_ov_build(&m, &a, &f, q, 8192);
        CHECK(n > 0 && m.am_nbox == 2 && m.am_n == 0);
        g_am_files = 40;
        // the Cheats tab draws as rows
        uit_ov_input(&m, OVIN_TAB_NEXT);
        n = uit_ov_build(&m, &a, &f, q, 8192);
        CHECK(n > 0 && m.tab == UIT_OV_TAB_CHEATS && m.rowbox[OV_CHEAT_DUR].y1 > m.rowbox[OV_CHEAT_DUR].y0);
        uit_ov_atlas_free(&a);
    }
    printf("amiibo tab: folder list, choose a file, scan, scan again, re-read, mouse, scrolling, empty / no reader; cheats tab\n");
}

static void test_draw(const char *fontdir) {
    mini::string r = read_file((mini::string(fontdir) + "/Inter-Regular.ttf").c_str()),
                 b = read_file((mini::string(fontdir) + "/Inter-SemiBold.ttf").c_str());
    CHECK(!r.empty() && !b.empty());
    if (r.empty() || b.empty()) return;
    struct { float w, h; } sizes[] = {{1280, 720}, {1920, 1080}, {2560, 1440}, {3840, 2160}};
    static UitOvQuad q[8192];
    for (auto sz : sizes) {
        float sc = uit_ov_scale(sz.w, sz.h);
        UitOvAtlas a;
        double t0 = now_ms();
        CHECK(uit_ov_atlas_build(&a, (const unsigned char *)r.data(), (const unsigned char *)b.data(), sc));
        double ta = now_ms() - t0;
        // every glyph the overlay draws is in the atlas (arrows, chevrons ... have a width)
        CHECK(uit_ov_text_width(&a, UIT_OVF_BODY, "\xE2\x86\x90\xE2\x86\x92\xE2\x80\xB9\xE2\x80\xBA") > 4 * sc);
        UitOv o;
        uit_ov_init(&o, &kHooks, L3 | R3, 1000);
        UitOvFrame f = {sz.w, sz.h, 0, 59.9, "F1", "L3 + R3"};
        CHECK(uit_ov_build(&o, &a, &f, q, 8192) == 0);   // closed, no counter: nothing at all
        o.show_fps = 1;
        int nf = uit_ov_build(&o, &a, &f, q, 8192);
        CHECK(nf > 0 && nf < 40);
        uit_ov_set_open(&o, 1, 0);
        int most = 0;
        double tb = 0;
        for (int t = 0; t < UIT_OV_TABS; t++) {
            o.tab = t;
            uit_ov_input(&o, OVIN_DOWN);
            double t1 = now_ms();
            int n = 0;
            for (int k = 0; k < 100; k++) n = uit_ov_build(&o, &a, &f, q, 8192);
            tb += (now_ms() - t1) / 100;
            CHECK(n > 0 && n <= 3072);   // UIT_OV_MAX_QUADS (src/host/overlay_share.h)
            if (n > most) most = n;
            for (int k = 0; k < n; k++) CHECK(q[k].x1 >= q[k].x0 && q[k].y1 >= q[k].y0 && q[k].x0 >= -1 && q[k].x1 <= sz.w + 1);
        }
        // the mouse: hovering a row selects it, a click on a tab switches to it, wheel moves
        o.tab = UIT_OV_TAB_DISPLAY;
        uit_ov_build(&o, &a, &f, q, 8192);
        int id = o.rows[2];
        UitOvBox bx = o.rowbox[id];
        uit_ov_mouse(&o, (bx.x0 + bx.x1) / 2, (bx.y0 + bx.y1) / 2, 0, 0);
        CHECK(o.sel[o.tab] == 2);
        uit_ov_mouse(&o, 0, 0, 0, -1);
        CHECK(o.sel[o.tab] == 3);
        UitOvBox tb2 = o.tabbox[UIT_OV_TAB_GAME];
        uit_ov_mouse(&o, (tb2.x0 + tb2.x1) / 2, (tb2.y0 + tb2.y1) / 2, 1, 0);
        CHECK(o.tab == UIT_OV_TAB_GAME);
        // [UIT 0174] the Controls tab with bindings and a controller: in the budget and the panel, the mouse's targets
        {
            UitBinds bd;
            uit_binds_default(&bd);
            UitOv c;
            uit_ov_init(&c, &kBindHooks, L3 | R3, 1000);
            uit_ov_set_binds(&c, &bd);
            c.pad_present = 1;
            c.pad_family = UIT_PADFAM_PS;
            uit_ov_set_open(&c, 1, 0);
            c.tab = UIT_OV_TAB_CONTROLS;
            for (int r = 0; r <= UIT_BIND_BUTTONS; r += 4) {
                c.sel[c.tab] = r;
                if (r == 4) uit_ov_capture_start(&c, UIT_BIND_KEY, 4);   // the waiting cell, its countdown
                if (r == 8) { uit_ov_capture_cancel(&c, "Cancelled"); uit_binds_clear(&c.binds, UIT_BIND_PAD, 8); }   // a gold dot, a Reset chip
                int n = uit_ov_build(&c, &a, &f, q, 8192);
                CHECK(n > 0 && n <= 3072);
                for (int k = 0; k < n; k++) CHECK(q[k].x1 >= q[k].x0 && q[k].y1 >= q[k].y0 && q[k].x0 >= -1 && q[k].x1 <= sz.w + 1 && q[k].y1 <= sz.h + 1);
                if (n > most) most = n;
                // the selected row is shown (the list scrolls to it), and inside the panel
                UitOvBox rb = c.brow[r];
                CHECKM(rb.y1 > rb.y0 && rb.y0 >= c.panel.y0 && rb.y1 <= c.panel.y1 + 1, "row %d", r);
            }
            CHECK(c.bscroll > 0);
            uit_ov_capture_cancel(&c, "Cancelled");
            c.sel[c.tab] = 0;
            uit_ov_build(&c, &a, &f, q, 8192);
            CHECK(c.bscroll == 0);
            // hover the second row's keyboard cell: that row and column; a click waits for a key; a click cancels
            UitOvBox kc = c.cell[1][UIT_BIND_KEY];
            uit_ov_mouse(&c, (kc.x0 + kc.x1) / 2, (kc.y0 + kc.y1) / 2, 0, 0);
            CHECK(c.sel[c.tab] == 1 && c.bcol == UIT_BIND_KEY);
            uit_ov_mouse(&c, (kc.x0 + kc.x1) / 2, (kc.y0 + kc.y1) / 2, 1, 0);
            CHECK(uit_ov_capturing(&c) == 1 + UIT_BIND_KEY && c.cap_btn == 1);
            uit_ov_build(&c, &a, &f, q, 8192);
            uit_ov_mouse(&c, (kc.x0 + kc.x1) / 2, (kc.y0 + kc.y1) / 2, 1, 0);
            CHECK(!uit_ov_capturing(&c) && c.open);
            // the wheel moves the selection; the Reset chip on a changed cell puts the default back
            uit_ov_mouse(&c, (kc.x0 + kc.x1) / 2, (kc.y0 + kc.y1) / 2, 0, -1);
            CHECK(c.sel[c.tab] == 2);
            c.sel[c.tab] = 8;
            c.bcol = UIT_BIND_PAD;
            uit_ov_build(&c, &a, &f, q, 8192);
            UitOvBox rc = c.cellreset[8][UIT_BIND_PAD];
            CHECK(rc.x1 > rc.x0);
            uit_ov_mouse(&c, (rc.x0 + rc.x1) / 2, (rc.y0 + rc.y1) / 2, 1, 0);
            CHECK(uit_bind_is_default(&c.binds, UIT_BIND_PAD, 8) && !uit_ov_capturing(&c));
            uit_ov_set_open(&c, 0, 0);
        }
        uit_ov_build(&o, &a, &f, q, 8192);
        UitOvBox cb = o.closebox;
        uit_ov_mouse(&o, (cb.x0 + cb.x1) / 2, (cb.y0 + cb.y1) / 2, 1, 0);
        CHECK(!o.open);
        printf("%4.0fx%-4.0f scale %.2f: atlas %dx%d (%d KiB) in %.1f ms; open %d quads (%d KiB), layout %.3f ms; counter %d quads\n",
               sz.w, sz.h, sc, a.w, a.h, a.w * a.h / 1024, ta, most, (int)(most * sizeof(UitOvQuad) / 1024),
               tb / UIT_OV_TABS, nf);
        uit_ov_atlas_free(&a);
        CHECK(a.pix == nullptr);
    }
}

// ---- 8. [UIT 0177] the software keyboard dialog (nn::swkbd's ShowKeyboard): typing, the limits and filters, the
// controller grid, the mouse, OK / cancel, the game seeing nothing, the drawing in the budget at each size.
static UitSwkbdReq kb_req(const char *initial, int max_len) {   // like TotK naming a horse
    UitSwkbdReq r;
    memset(&r, 0, sizeof r);
    r.mode = 8;
    r.invalid = UIT_KBINV_AT | UIT_KBINV_PERCENT | UIT_KBINV_BACKSLASH;
    r.max_len = max_len;
    const char *h = "Name your horse";
    for (int i = 0; h[i]; i++) r.header[i] = (uint16_t)h[i];
    const char *g = "Horse name";
    for (int i = 0; g[i]; i++) r.guide[i] = (uint16_t)g[i];
    for (int i = 0; initial && initial[i]; i++) r.initial[r.initial_len++] = (uint16_t)initial[i];
    return r;
}
static bool kb_is(const UitOv &o, const char *s) {
    int i = 0;
    for (; s[i]; i++) if (i >= o.kb.n || o.kb.t[i] != (uint8_t)s[i]) return false;
    return i == o.kb.n && o.kb.t[i] == 0;
}
static void test_kb(const char *fontdir) {
    UitOv o;
    uit_ov_init(&o, &kHooks, L3 | R3, 1000);
    UitSwkbdReq r = kb_req("Epona", 9);
    uint16_t out[UIT_SWKBD_LIMIT + 1];
    // typing at the cursor; refused characters say why; at most max
    uit_ov_kb_begin(&o, &r, 100);
    CHECK(o.kb.active && !o.kb.done && o.kb.n == 5 && o.kb.cur == 5 && o.kb.row == 1 && kb_is(o, "Epona"));
    uit_ov_kb_text(&o, "x@y");
    CHECK(kb_is(o, "Eponaxy") && o.kb.msg[0]);
    uit_ov_kb_text(&o, "z");
    CHECK(kb_is(o, "Eponaxyz") && !o.kb.msg[0]);
    uit_ov_kb_text(&o, "ABCD");
    CHECK(kb_is(o, "EponaxyzA") && strstr(o.kb.msg, "9"));
    uit_ov_kb_text(&o, "\r\t");   // nothing fits; a CR is dropped, a tab would be a space
    CHECK(o.kb.n == 9);
    // editing
    uit_ov_kb_edit(&o, UIT_KBE_BACKSPACE);
    uit_ov_kb_edit(&o, UIT_KBE_HOME);
    uit_ov_kb_edit(&o, UIT_KBE_DELETE);
    CHECK(kb_is(o, "ponaxyz") && o.kb.cur == 0);
    uit_ov_kb_edit(&o, UIT_KBE_RIGHT);
    uit_ov_kb_text(&o, "\tq");
    CHECK(kb_is(o, "p qonaxyz") && o.kb.cur == 3);
    uit_ov_kb_edit(&o, UIT_KBE_END);
    CHECK(o.kb.cur == 9);
    uit_ov_kb_edit(&o, UIT_KBE_LEFT);
    uit_ov_kb_edit(&o, UIT_KBE_BACKSPACE);
    CHECK(kb_is(o, "p qonaxz") && o.kb.cur == 7);
    // an IME's composition shows, the commit replaces it
    uit_ov_kb_compose(&o, "ka");
    CHECK(!strcmp(o.kb.pre, "ka") && o.kb.n == 8);
    uit_ov_kb_text(&o, "\xC3\xA9");
    CHECK(!o.kb.pre[0] && o.kb.n == 9 && o.kb.t[7] == 0xE9);
    // the menu's inputs do nothing while it is up; the controller is hidden from the game
    uit_ov_set_open(&o, 1, 0);   // the menu open underneath keeps its place
    int sel0 = o.sel[o.tab], tab0 = o.tab;
    uit_ov_input(&o, OVIN_DOWN);
    uit_ov_input(&o, OVIN_TAB_NEXT);
    CHECK(o.sel[o.tab] == sel0 && o.tab == tab0);
    uit_ov_set_open(&o, 0, 0);
    CHECK(uit_ov_pad(&o, L3 | R3, 0, 200) == 0xFFFFFFFFu);
    CHECK(uit_ov_pad(&o, L3 | R3, 0, 5000) == 0xFFFFFFFFu && !o.open);   // the combo held for long: not the menu
    // OK: the text; Enter without newlines allowed is OK
    uit_ov_kb_edit(&o, UIT_KBE_NEWLINE);
    CHECK(o.kb.done && o.kb.ok);
    int n = uit_ov_kb_result(&o, out);
    CHECK(n == 9 && out[0] == 'p' && out[7] == 0xE9 && out[9] == 0);
    uit_ov_kb_text(&o, "late");   // answered: nothing more
    CHECK(o.kb.n == 9);
    uit_ov_kb_end(&o, 1u << 0);
    CHECK(!o.kb.active && (o.suppress & 1u));
    // an empty answer is refused (the applet greys OK out); min_len too; cancel gives no text
    r = kb_req(nullptr, 9);
    r.min_len = 3;
    uit_ov_kb_begin(&o, &r, 100);
    uit_ov_kb_edit(&o, UIT_KBE_OK);
    CHECK(!o.kb.done && strstr(o.kb.msg, "3"));
    uit_ov_kb_text(&o, "ab");
    uit_ov_kb_edit(&o, UIT_KBE_OK);
    CHECK(!o.kb.done);
    uit_ov_kb_edit(&o, UIT_KBE_CANCEL);
    CHECK(o.kb.done && !o.kb.ok && uit_ov_kb_result(&o, out) == 0 && out[0] == 0);
    uit_ov_kb_end(&o, 0);
    r.min_len = 0;
    uit_ov_kb_begin(&o, &r, 100);
    uit_ov_kb_edit(&o, UIT_KBE_OK);
    CHECK(!o.kb.done && o.kb.msg[0]);
    uit_ov_kb_end(&o, 0);
    // a character outside the BMP is one pair: typed whole, deleted whole, never half over max
    r = kb_req("1234567", 9);
    uit_ov_kb_begin(&o, &r, 100);
    uit_ov_kb_text(&o, "\xF0\x9F\x90\xB4");
    CHECK(o.kb.n == 9 && o.kb.t[7] == 0xD83D && o.kb.t[8] == 0xDC34);
    uit_ov_kb_edit(&o, UIT_KBE_LEFT);
    CHECK(o.kb.cur == 7);
    uit_ov_kb_edit(&o, UIT_KBE_DELETE);
    CHECK(o.kb.n == 7);
    uit_ov_kb_text(&o, "8\xF0\x9F\x90\xB4");
    CHECK(o.kb.n == 8 && o.kb.msg[0]);
    uit_ov_kb_edit(&o, UIT_KBE_BACKSPACE);
    uit_ov_kb_edit(&o, UIT_KBE_BACKSPACE);
    CHECK(kb_is(o, "123456"));
    uit_ov_kb_end(&o, 0);
    // numeric: the digits' row first, letters refused; password and the cursor at the start
    r = kb_req("42", 4);
    r.mode = UIT_KBMODE_NUMERIC;
    r.cursor_start = 1;
    uit_ov_kb_begin(&o, &r, 100);
    CHECK(o.kb.row == 0 && o.kb.cur == 0);
    uit_ov_kb_text(&o, "a7");
    CHECK(kb_is(o, "742") && strstr(o.kb.msg, "number"));
    uit_ov_kb_end(&o, 0);
    // the controller: A held when it opens does nothing until let go (nor repeats); then the grid
    r = kb_req(nullptr, 9);
    uit_ov_kb_begin(&o, &r, 1000);
    uint64_t t = 1000;
    CHECK(uit_ov_pad(&o, 1, OVNAV_CONFIRM | OVNAV_DOWN, t) == 0xFFFFFFFFu);
    CHECK(uit_ov_pad(&o, 1, OVNAV_CONFIRM | OVNAV_DOWN, t += 1000) == 0xFFFFFFFFu);
    CHECK(o.kb.n == 0 && o.kb.row == 1);
    uit_ov_pad(&o, 0, 0, t += 16);
    uit_ov_pad(&o, 1, OVNAV_CONFIRM, t += 16);   // A on the first letter
    CHECK(kb_is(o, "q"));
    uit_ov_pad(&o, 0, 0, t += 16);
    uit_ov_pad(&o, 0, OVNAV_RIGHT, t += 16);
    uit_ov_pad(&o, 0, 0, t += 16);
    uit_ov_pad(&o, 0, OVNAV_L3, t += 16);         // shift once
    uit_ov_pad(&o, 1, OVNAV_CONFIRM, t += 16);
    uit_ov_pad(&o, 0, 0, t += 16);
    uit_ov_pad(&o, 1, OVNAV_CONFIRM, t += 16);
    uit_ov_pad(&o, 0, 0, t += 16);
    CHECK(kb_is(o, "qWw") && o.kb.col == 1);
    uit_ov_pad(&o, 0, OVNAV_RESET, t += 16);      // X: a space
    uit_ov_pad(&o, 0, OVNAV_BACK, t += 16);       // B: backspace
    uit_ov_pad(&o, 0, 0, t += 16);
    CHECK(kb_is(o, "qWw"));
    uit_ov_pad(&o, 0, OVNAV_RIGHT, t += 16);       // a held direction repeats after 380 ms, then every 70
    uit_ov_pad(&o, 0, OVNAV_RIGHT, t += 300);
    CHECK(o.kb.col == 2);
    uit_ov_pad(&o, 0, OVNAV_RIGHT, t += 100);
    CHECK(o.kb.col == 3);
    uit_ov_pad(&o, 0, OVNAV_RIGHT, t += 70);
    CHECK(o.kb.col == 4);
    uit_ov_pad(&o, 0, 0, t += 16);
    uit_ov_pad(&o, 0, OVNAV_LEFT, t += 16);        // wraps
    uit_ov_pad(&o, 0, 0, t += 16);
    CHECK(o.kb.col == 3);
    for (int i = 0; i < 3; i++) { uit_ov_pad(&o, 0, OVNAV_DOWN, t += 16); uit_ov_pad(&o, 0, 0, t += 16); }
    CHECK(o.kb.row == 4 && o.kb.col == UIT_KBK_SPACE);   // the special row: the key under the one above
    uit_ov_pad(&o, 0, OVNAV_DOWN, t += 16); uit_ov_pad(&o, 0, 0, t += 16);
    CHECK(o.kb.row == 0);                                 // wraps to the digits
    uit_ov_pad(&o, 0, OVNAV_Y, t += 16); uit_ov_pad(&o, 0, 0, t += 16);   // Y: the next page
    CHECK(o.kb.page == UIT_KB_PAGE_SYMBOLS && uit_ov_kb_char(&o.kb, 0, 0) == '!');
    CHECK(uit_ov_kb_char(&o.kb, 3, 9) == 0xF7 && uit_ov_kb_char(&o.kb, 4, 0) == 0 && uit_ov_kb_char(&o.kb, 0, 10) == 0);
    uit_ov_pad(&o, 0, OVNAV_TABL, t += 16); uit_ov_pad(&o, 0, 0, t += 16);   // L / R: the cursor
    CHECK(o.kb.cur == 2);
    uit_ov_pad(&o, 0, OVNAV_PLUS, t += 16);       // +: OK
    CHECK(o.kb.done && o.kb.ok);
    uit_ov_kb_end(&o, 0);
    uit_ov_kb_begin(&o, &r, t);
    uit_ov_pad(&o, 0, 0, t += 16);
    uit_ov_pad(&o, 0, OVNAV_MINUS, t += 16);      // -: cancel
    CHECK(o.kb.done && !o.kb.ok);
    uit_ov_kb_end(&o, 0);
    // the grid's special keys
    uit_ov_kb_begin(&o, &r, t);
    uit_ov_kb_key(&o, 4, UIT_KBK_SHIFT);
    uit_ov_kb_key(&o, 4, UIT_KBK_SHIFT);   // locked
    uit_ov_kb_key(&o, 1, 0);
    uit_ov_kb_key(&o, 1, 1);
    uit_ov_kb_key(&o, 4, UIT_KBK_SHIFT);   // off
    uit_ov_kb_key(&o, 4, UIT_KBK_SPACE);
    uit_ov_kb_key(&o, 4, UIT_KBK_PAGE);
    uit_ov_kb_key(&o, 4, UIT_KBK_PAGE);
    uit_ov_kb_key(&o, 0, 9);               // an accent
    uit_ov_kb_key(&o, 4, UIT_KBK_PAGE);    // round to the letters
    uit_ov_kb_key(&o, 2, 0);
    uit_ov_kb_key(&o, 4, UIT_KBK_PAGE);
    uit_ov_kb_key(&o, 0, 1);               // '@': not allowed for a horse
    CHECK(o.kb.n == 5 && o.kb.t[0] == 'Q' && o.kb.t[1] == 'W' && o.kb.t[2] == ' ' && o.kb.t[3] == 0xE9 && o.kb.t[4] == 'a' && o.kb.msg[0]);
    uit_ov_kb_key(&o, 4, UIT_KBK_BACK);
    CHECK(o.kb.n == 4);
    uit_ov_kb_key(&o, 4, UIT_KBK_OK);
    CHECK(o.kb.done && o.kb.ok && uit_ov_kb_result(&o, out) == 4);
    uit_ov_kb_end(&o, 0);

    // drawing and the mouse, at each size
    mini::string fr = read_file((mini::string(fontdir) + "/Inter-Regular.ttf").c_str()),
                 fb = read_file((mini::string(fontdir) + "/Inter-SemiBold.ttf").c_str());
    if (fr.empty() || fb.empty()) return;
    struct { float w, h; } sizes[] = {{1280, 720}, {1920, 1080}, {3840, 2160}};
    static UitOvQuad q[8192];
    int most = 0;
    for (auto sz : sizes) {
        UitOvAtlas a;
        CHECK(uit_ov_atlas_build(&a, (const unsigned char *)fr.data(), (const unsigned char *)fb.data(), uit_ov_scale(sz.w, sz.h)));
        // the glyphs the dialog draws: Latin-1, Latin Extended-A, the key symbols
        CHECK(uit_ov_text_width(&a, UIT_OVF_BODY, "\xE2\x8C\xAB\xE2\x87\xA7\xE2\x90\xA3\xE2\x86\xB5\xC5\x93\xC5\x82") > 0);
        UitOvFrame f = {sz.w, sz.h, 2000, 59.9, "F1", "L3 + R3"};
        UitOv m;
        uit_ov_init(&m, &kHooks, L3 | R3, 1000);
        m.show_fps = 1;
        UitSwkbdReq rq = kb_req("Epona", 9);
        const char *sub = "Up to 9 characters";
        for (int i = 0; sub[i]; i++) rq.sub[i] = (uint16_t)sub[i];
        uit_ov_kb_begin(&m, &rq, 2000);
        for (int v = 0; v < 6; v++) {   // letters, shift, symbols, accents, a message, password, a long text (scrolled)
            if (v == 1) m.kb.shift = 2;
            if (v == 2) m.kb.page = UIT_KB_PAGE_SYMBOLS;
            if (v == 3) { m.kb.page = UIT_KB_PAGE_ACCENTS; uit_ov_kb_text(&m, "@"); uit_ov_kb_compose(&m, "kana"); }
            if (v == 4) { m.kb.req.password = 1; uit_ov_kb_compose(&m, ""); }
            if (v == 5) {
                UitSwkbdReq big = kb_req(nullptr, 500);
                big.newline = 1;
                uit_ov_kb_begin(&m, &big, 2000);
                for (int i = 0; i < 25; i++) uit_ov_kb_text(&m, "The quick brown fox jumps\n");
            }
            int nq = uit_ov_build(&m, &a, &f, q, 8192);
            CHECKM(nq > 0 && nq <= 3072, "variant %d: %d quads", v, nq);   // UIT_OV_MAX_QUADS
            if (nq > most) most = nq;
            for (int k = 0; k < nq; k++) CHECK(q[k].x1 >= q[k].x0 && q[k].y1 >= q[k].y0 && q[k].x0 >= -1 && q[k].x1 <= sz.w + 1 && q[k].y1 <= sz.h + 1);
            CHECK(m.kb.panel.x1 > m.kb.panel.x0 && m.kb.panel.x0 >= 0 && m.kb.panel.x1 <= sz.w && m.kb.panel.y1 <= sz.h);
            for (int rr = 0; rr < UIT_KB_ROWS; rr++)
                for (int c = 0; c < (rr < 4 ? UIT_KB_COLS : UIT_KBK_COUNT); c++) {
                    const UitOvBox &kb = m.kb.key[rr][c];
                    CHECK(kb.x1 > kb.x0 && kb.y1 > kb.y0 && kb.x0 >= m.kb.panel.x0 && kb.x1 <= m.kb.panel.x1 && kb.y1 <= m.kb.panel.y1);
                }
        }
        uit_ov_kb_end(&m, 0);
        // the mouse: hover focuses a key, a click presses it; a click on OK answers
        uit_ov_kb_begin(&m, &rq, 2000);
        uit_ov_build(&m, &a, &f, q, 8192);
        UitOvBox kq = m.kb.key[1][0];
        uit_ov_mouse(&m, (kq.x0 + kq.x1) / 2, (kq.y0 + kq.y1) / 2, 0, 0);
        CHECK(m.kb.row == 1 && m.kb.col == 0 && m.kb.n == 5);
        uit_ov_mouse(&m, (kq.x0 + kq.x1) / 2, (kq.y0 + kq.y1) / 2, 1, 0);
        CHECK(kb_is(m, "Eponaq"));
        uit_ov_kb_edit(&m, UIT_KBE_HOME);
        uit_ov_mouse(&m, (m.kb.field.x0 + m.kb.field.x1) / 2, (m.kb.field.y0 + m.kb.field.y1) / 2, 1, 0);
        CHECK(m.kb.cur == 6);
        uit_ov_mouse(&m, 1, 1, 1, 0);   // outside: nothing
        CHECK(!m.kb.done && m.kb.n == 6 && !m.open);
        UitOvBox ko = m.kb.key[4][UIT_KBK_OK];
        uit_ov_mouse(&m, (ko.x0 + ko.x1) / 2, (ko.y0 + ko.y1) / 2, 1, 0);
        CHECK(m.kb.done && m.kb.ok);
        uit_ov_kb_end(&m, 0);
        CHECK(uit_ov_build(&m, &a, &f, q, 8192) < 40);   // closed again: the counter only
        uit_ov_atlas_free(&a);
    }
    printf("keyboard dialog: typing, limits, filters, pairs, IME, pad grid / repeat / held keys, mouse, OK / cancel; at most %d quads\n", most);
}


// ---- 5. a CPU reference of the pass (src/vk/shaders/overlay.vert / .frag, line for line) for a look at the layout
// without a GPU: test_overlay ... <dir> writes <dir>/overlay_<tab>.ppm at 1920x1080 over a stand-in picture.
static float clampf(float v, float a, float b) { return v < a ? a : v > b ? b : v; }
static float round_box(float px, float py, float bx, float by, float r) {
    r = fminf(r, fminf(bx, by));
    float qx = fabsf(px) - bx + r, qy = fabsf(py) - by + r;
    float ox = fmaxf(qx, 0), oy = fmaxf(qy, 0);
    return sqrtf(ox * ox + oy * oy) + fminf(fmaxf(qx, qy), 0.0f) - r;
}
static float atlas_bilinear(const UitOvAtlas *a, float u, float v) {   // VK_FILTER_LINEAR, clamp to edge
    float x = u * a->w - 0.5f, y = v * a->h - 0.5f;
    int x0 = (int)floorf(x), y0 = (int)floorf(y);
    float fx = x - x0, fy = y - y0;
    auto at = [&](int xx, int yy) {
        xx = xx < 0 ? 0 : xx >= a->w ? a->w - 1 : xx;
        yy = yy < 0 ? 0 : yy >= a->h ? a->h - 1 : yy;
        return a->pix[(size_t)yy * a->w + xx] / 255.0f;
    };
    return (at(x0, y0) * (1 - fx) + at(x0 + 1, y0) * fx) * (1 - fy) + (at(x0, y0 + 1) * (1 - fx) + at(x0 + 1, y0 + 1) * fx) * fy;
}
static void raster(float *img, int W, int H, const UitOvAtlas *a, const UitOvQuad *q, int n) {
    for (int i = 0; i < n; i++) {
        const UitOvQuad &k = q[i];
        float cr = (k.rgba & 255) / 255.0f, cg = (k.rgba >> 8 & 255) / 255.0f, cb = (k.rgba >> 16 & 255) / 255.0f,
              ca = (k.rgba >> 24) / 255.0f;
        float hx = (k.x1 - k.x0) * 0.5f, hy = (k.y1 - k.y0) * 0.5f;
        int kind = (int)(k.kind + 0.5f);
        for (int y = (int)floorf(k.y0); y < (int)ceilf(k.y1); y++)
            for (int x = (int)floorf(k.x0); x < (int)ceilf(k.x1); x++) {
                float px = x + 0.5f, py = y + 0.5f;
                if (x < 0 || y < 0 || x >= W || y >= H || px < k.x0 || px >= k.x1 || py < k.y0 || py >= k.y1) continue;
                float tx = (px - k.x0) / (k.x1 - k.x0), ty = (py - k.y0) / (k.y1 - k.y0);
                float lx = (tx - 0.5f) * 2 * hx, ly = (ty - 0.5f) * 2 * hy, al;
                float r = cr, g = cg, b = cb, aa = ca;
                if (kind == UIT_OVQ_TEXT) {
                    al = atlas_bilinear(a, k.u0 + (k.u1 - k.u0) * tx, k.v0 + (k.v1 - k.v0) * ty);
                } else if (kind == UIT_OVQ_RING) {
                    float rr = hx - k.stroke * 0.5f - 0.5f, d = fabsf(sqrtf(lx * lx + ly * ly) - rr) - k.stroke * 0.5f;
                    al = clampf(0.5f - d, 0, 1);
                    float ang = atan2f(lx, -ly) / 6.2831853f;
                    if (ang < 0) ang += 1;
                    if (ang > k.u0) { r = r * 0.35f + 0.12f; g = g * 0.35f + 0.12f; b = b * 0.35f + 0.12f; aa *= 0.55f; }
                } else {
                    float d = round_box(lx, ly, hx, hy, k.radius);
                    al = clampf(0.5f - d, 0, 1);
                    if (kind == UIT_OVQ_OUTLINE) al *= clampf(d + k.stroke + 0.5f, 0, 1);
                }
                float s = aa * al;
                float *o = img + ((size_t)y * W + x) * 3;
                o[0] = r * s + o[0] * (1 - s);
                o[1] = g * s + o[1] * (1 - s);
                o[2] = b * s + o[2] * (1 - s);
            }
    }
}
static void write_ppm(const char *path, const float *img, int W, int H) {
    FILE *f = fopen(path, "wb");
    if (!f) { CHECKM(0, "cannot write %s", path); return; }
    fprintf(f, "P6\n%d %d\n255\n", W, H);
    for (size_t i = 0; i < (size_t)W * H * 3; i++) fputc((int)(clampf(img[i], 0, 1) * 255 + 0.5f), f);
    fclose(f);
}
static void test_preview(const char *fontdir, const char *outdir) {
    mini::string r = read_file((mini::string(fontdir) + "/Inter-Regular.ttf").c_str()),
                 b = read_file((mini::string(fontdir) + "/Inter-SemiBold.ttf").c_str());
    if (r.empty() || b.empty()) return;
    const int W = 1920, H = 1080;
    UitOvAtlas a;
    CHECK(uit_ov_atlas_build(&a, (const unsigned char *)r.data(), (const unsigned char *)b.data(), uit_ov_scale(W, H)));
    static UitOvQuad q[8192];
    float *img = (float *)malloc((size_t)W * H * 3 * sizeof(float));
    const char *names[UIT_OV_TABS + 3] = {"display", "performance", "game", "controls", "amiibo", "cheats", "overlay", "fps", "ring", "keyboard"};
    for (int t = 0; t < UIT_OV_TABS + 3; t++) {
        for (int y = 0; y < H; y++)   // a stand-in for the game picture: sky to grass
            for (int x = 0; x < W; x++) {
                float k = (float)y / H, *o = img + ((size_t)y * W + x) * 3;
                o[0] = 0.35f + 0.2f * k; o[1] = 0.55f + 0.1f * k; o[2] = 0.85f - 0.55f * k;
            }
        UitOv o;
        uit_ov_init(&o, &kHooks, L3 | R3, 1000);
        UitOvFrame f = {(float)W, (float)H, 10000, 59.9, "F1", "L3 + R3"};
        UitBinds bd;
        uit_binds_default(&bd);
        if (t == UIT_OV_TAB_AMIIBO) {   // [UIT 0175] a folder of files, one scanned
            uit_ov_init(&o, &kAmHooks, L3 | R3, 1000);
            g_am_have_last = 1;
        }
        if (t == UIT_OV_TAB_CONTROLS) {   // a controller, one changed binding, the keyboard cell of B waiting
            uit_ov_init(&o, &kBindHooks, L3 | R3, 1000);
            uit_ov_set_binds(&o, &bd);
            o.pad_present = 1;
            o.pad_family = UIT_PADFAM_XBOX;
            o.now = 10000 - 3000;
            uit_binds_clear(&o.binds, UIT_BIND_PAD, 3);
        }
        if (t < UIT_OV_TABS) {
            uit_ov_set_open(&o, 1, 0);
            while (o.tab != t) uit_ov_input(&o, OVIN_TAB_NEXT);   // as a player gets there (the Amiibo tab reads its folder)
            uit_ov_input(&o, OVIN_DOWN);
        }
        if (t == UIT_OV_TAB_CONTROLS) { o.bcol = UIT_BIND_KEY; uit_ov_capture_start(&o, UIT_BIND_KEY, 1); }
        else if (t == UIT_OV_TABS) o.show_fps = 1;
        else if (t == UIT_OV_TABS + 2) {   // [UIT 0177] the keyboard dialog naming a horse, Shift once, a refused '@'
            UitSwkbdReq rq = kb_req("Epon", 9);
            uit_ov_kb_begin(&o, &rq, 10000 - 400);
            o.kb.col = 3;
            o.kb.shift = 1;
            uit_ov_kb_text(&o, "@");
        }
        else { uit_ov_pad(&o, L3 | R3, 0, 10000 - 600); f.now_ms = 10000; }   // the combo held for 0.6 s of 1
        int n = uit_ov_build(&o, &a, &f, q, 8192);
        CHECK(n > 0);
        raster(img, W, H, &a, q, n);
        write_ppm((mini::string(outdir) + "/overlay_" + names[t] + ".ppm").c_str(), img, W, H);
    }
    free(img);
    uit_ov_atlas_free(&a);
    printf("previews: %s/overlay_*.ppm\n", outdir);
}

int main(int argc, char **argv) {
    if (argc < 3) { printf("usage: test_overlay launcher/settings.ini third_party/inter [preview dir]\n"); return 2; }
    mini::string err;
    auto S = parse_schema(read_file(argv[1]), &err);
    CHECKM(err.empty(), "%s", err.c_str());
    CHECK(sizeof(UitOvQuad) == 48);   // the pass's vertex attributes (src/vk/overlay_vk.c, overlay.vert)
    CHECK(offsetof(UitOvQuad, u0) == 16 && offsetof(UitOvQuad, rgba) == 32 && offsetof(UitOvQuad, radius) == 36);
    test_table(S);
    test_roundtrip(S);
    test_state();
    test_face_guide();
    test_binds(S);
    test_draw(argv[2]);
    test_amiibo_tab(argv[2]);
    test_kb(argv[2]);
    if (argc > 3) test_preview(argv[2], argv[3]);
    printf("%s: %d checks, %d failed\n", fails ? "FAIL" : "ok", checks, fails);
    return fails != 0;
}
