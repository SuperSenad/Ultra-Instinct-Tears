// Ultra Instinct Tears launcher: edit every ultrainstincttears.cfg setting and start the game, with a controller or mouse and keyboard.
//
// SDL3 + Dear ImGui (draw lists only: the widgets, focus and navigation are this file's own, so left/right can change
// the focused value and every screen works the same with a pad, arrows, or the mouse). The settings come from
// launcher/settings.ini (embedded at build time); ultrainstincttears.cfg is edited line by line so comments, paths and keys
// the launcher does not know are kept. Build: py -3 tools/build_launcher.py (tools/setup.py runs it).
#define SDL_MAIN_HANDLED   // main() below hands over to SDL_RunApp itself: no SDL_main_impl.h needed
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include "mini.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "imgui.h"
#include "backends/imgui_impl_sdl3.h"
#include "backends/imgui_impl_sdlrenderer3.h"
#include "launcher_data.h"
#include "../src/host/input_map.h"
#include "../src/host/modfs.h"
#include "../src/host/game_exit.h"   // [UIT 0294] Restart game
#include "model.h"
#include "platform.h"
#include "save_backup.h"
#include "single_instance.h"
#include "ui.h"

using namespace ut;

// ---------------------------------------------------------------- input

enum Act {
    A_Up, A_Down, A_Left, A_Right, A_Accept, A_Back, A_Reset, A_TabPrev, A_TabNext, A_PagePrev, A_PageNext,
    A_Play, A_PlayLog, A_Home, A_End, A_JumpTab, A_Shift, A_Char, A_Backspace, A_Done, A_Paste,
    A_Grab, A_ModUp, A_ModDown,   // 0059: Mods tab: pick a mod up (pad Y) / move it (Ctrl+Up / Ctrl+Down)
    A_Search,                     // [UIT 0251] Ctrl+F, / or the pad's View button: the tab's search
};
struct Action { Act a; int n = 0; int repeat = 0; bool fast = false; mini::string ch; };

enum class Device { Keyboard, Gamepad };

enum Glyph { G_Move, G_Change, G_Accept, G_Back, G_Reset, G_Tabs, G_Page, G_Play, G_Shift, G_Done, G_Grab, G_Search };

// ---------------------------------------------------------------- screens

// 0032: the Stutter tab is the "Stutter" section of Performance (settings.ini: section = Stutter)
static const char *kTabNames[] = {"Play", "Graphics", "Performance", "Game", "Mods", "Advanced", "About"};
static const char *kTabKeys[] = {"", "graphics", "performance", "game", "mods", "advanced", ""};
static const char *kTabBlurb[] = {
    "",
    "How the picture is scaled and shown.",
    "Frame rate, how the game paces itself, and stutter (shader warm-up and texture streaming).",
    "Language, controller and sound.",
    "RomFS mods: install them, turn each on or off and set which one wins a file both change. Changes apply the next time you press Play.",
    "Folders, troubleshooting and developer switches. Most players never need these.",
    "",
};
enum { TAB_PLAY = 0, TAB_MODS = 4, TAB_ADVANCED = 5, TAB_ABOUT = 6, TAB_COUNT = 7 };

enum Btn { B_Play, B_PlayLog, B_Recommend, B_ResetAll, B_OpenFolder, B_OpenCfg, B_OpenLogs, B_Quit, B_ResetGroup, B_Update, B_Report,
           B_ModFile, B_ModFolder, B_ModInbox, B_ModAllOn, B_ModAllOff, B_ModOpen, B_Speed,
           B_Backups,    // [UIT 0347] About > Open save backups
           B_SearchIn };   // [UIT 0251] "Show the matches in <tab>" (gid = the tab number)

struct Item {
    enum Kind { Row, Group, Heading, Button, Note, Mod } kind;
    int idx = -1;              // setting index (Row), button id (Button; B_ResetGroup resets the group gid), mod (Mod)
    mini::string label;         // group / heading name
    mini::string gid;           // group id: tab/group (+ when folded)
    float h = 0;               // design units
    bool focusable() const { return kind == Row || kind == Group || kind == Button || kind == Mod; }
};

struct Modal {
    enum Kind { None, Confirm, Edit, Message, Bind } kind = None;
    mini::string title, body, ok = "OK", cancel;
    mini::vector<mini::pair<mini::string, mini::string>> rows;   // name, change
    mini::function<void()> on_ok, on_alt;
    mini::string alt;                                         // a third button (e.g. "Open log")
    int focus = 0;                                           // button index
    bool danger = false;
    // Edit
    int setting = -1;
    mini::string buf, error;
    int kr = 0, kc = 0;                                      // on-screen keyboard cursor
    bool shift = false;
    float open_t = 0;
    // Bind: waiting for the controller button (bind_pad) or key that setting `setting` should use
    bool bind_pad = false;
    double bind_until = 0;
    // [UIT 0036] a helper is running (update, report): no buttons, the dialog stays; pct >= 0 draws a bar
    bool busy = false;
    float pct = -1;
    // [UIT 0270] the first-run question (which button confirms): alt (2) = Standard, ok (0) = Follow the game
    bool ask_confirm = false;
};

static const double kBindSeconds = 8.0;   // a bind dialog with no press closes by itself

static const char *kOskRows[] = {"1234567890-", "qwertyuiop\\", "asdfghjkl:_", "zxcvbnm,./="};
static const char *kOskRowsShift[] = {"!@#$%^&*()+", "QWERTYUIOP|", "ASDFGHJKL;~", "ZXCVBNM<>?'"};
enum { OSK_SPECIAL_ROW = 4 };
static const char *kOskSpecial[] = {"Shift", "Space", "Delete", "Clear", "Paste", "Done"};
// [UIT 0251] the controller's search keyboard: letters, digits and the punctuation setting names use; then a key row
static const char *kSearchRows[] = {"1234567890", "qwertyuiop", "asdfghjkl+", "zxcvbnm-_."};
enum { SOSK_SPECIAL_ROW = 4 };
static const char *kSearchSpecial[] = {"Space", "Delete", "Clear", "Done"};

struct App {
    SDL_Window *win = nullptr;
    SDL_Renderer *ren = nullptr;
    Draw d;
    mini::vector<Setting> S;
    mini::map<mini::string, int> by_key;
    CfgFile cfg;
    mini::string dir, exe;
    bool dll = false, exe_found = false;
    LiftFeatures lift;   // [UIT 0070] data/generated/lift-features.ini: what the build-time scan found
    GpuInfo gpu;
    SDL_Time cfg_mtime = 0;
    SDL_Texture *logo = nullptr;   // the icon (assets/uit-256.png) in the top bar
    mini::string start_toast;      // shown on the first frame (the settings file moved to its new name)

    int tab = TAB_PLAY;
    // [UIT 0251] search: each settings tab keeps its own query (switching tabs keeps them; the field itself lets go)
    mini::string query[TAB_COUNT];
    bool sfield = false;               // the search field has the focus (typing goes to it); not an item
    bool sosk = false;                 // the controller's search keyboard is open (in the detail panel; implies sfield)
    int skr = 1, skc = 0;              // its cursor
    int eat_slash = 0;                 // the '/' that opened the field must not be typed into it
    int smatch = 0, sother[TAB_COUNT] = {};   // matches on this tab / on the others
    int last_stab = 1;                 // View on Play / About opens the last settings tab's search
    mini::vector<SearchText> stext;    // per setting, built once
    mini::vector<mini::string> swords; // this tab's query words (empty: not searching)
    int focus[TAB_COUNT] = {};
    float scroll[TAB_COUNT] = {}, scroll_to[TAB_COUNT] = {};
    mini::set<mini::string> expanded;
    mini::vector<Item> items;
    Device dev = Device::Keyboard;
    Modal modal;
    mini::vector<Action> actions;

    // feedback
    double now = 0, dt = 0;
    double save_at = -1, saved_at = -100;
    mini::string save_error, toast;
    double toast_at = -100;
    bool snap = false;                 // scripted screenshots: no easing
    ImVec4 frect = {0, 0, 0, 0};       // the animated focus highlight (x, y, w, h)
    bool frect_valid = false;
    float tab_x = 0, tab_w = 0;        // animated tab underline
    mini::map<int, float> knob;         // toggle switch positions
    int drag = -1;                     // setting being dragged with the mouse
    bool mouse_moved = false;
    bool quit = false;

    // the game
    Game game;
    bool running = false;
    double game_started = 0;   // [UIT 0294] when Play started it (now): a restart asked at once is not followed
    bool fake_running = false;   // [UIT 0304] script gamerunning:1: the launcher as while the game runs (no game started)
    mini::string last_log, last_run;

    // held directions on the pad (repeat)
    int held = -1;
    double held_next = 0;
    int held_count = 0;
    int stick_dir = -1;
    bool trig_down[2] = {};
    bool pad_connected = false;        // a controller is open (bindings, names on the key caps)

    // ---------------------------------------------------------------- values
    int idx(const char *key) const { auto it = by_key.find(key); return it == by_key.end() ? -1 : it->second; }

    mini::optional<mini::string> raw(int i) const {
        const Setting &s = S[i];
        auto r = cfg.get(s.key);
        if (!r && !s.alias.empty()) r = cfg.get(s.alias);
        if (!r && s.key == "TOTK_UPSCALER") {          // older files only have TOTK_FSR=1
            int f = idx("TOTK_FSR");
            if (f >= 0 && read_value(S[f], cfg.get("TOTK_FSR")).b) r = mini::string("fsr");
        }
        return r;
    }
    Value val(int i) const { return read_value(S[i], raw(i)); }
    bool modified(int i) const { return !same(S[i], val(i), default_value(S[i])); }
    mini::string tok(int i) const { return token(S[i], val(i)); }
    mini::string shown(int i) const { return display(S[i], val(i)); }

    // [UIT 0270] Game > Controller > Confirm button (TOTK_UI_CONFIRM): which face button confirms here and in the
    // overlay. By position, as SDL reports them: standard = the bottom (south) button confirms and the right (east) one
    // goes back; game = the right one confirms and the bottom one goes back, as in the game. Names follow the controller.
    bool confirm_game() const { int i = idx("TOTK_UI_CONFIRM"); return i >= 0 && tok(i) == "game"; }
    int ok_button() const { return confirm_game() ? SDL_GAMEPAD_BUTTON_EAST : SDL_GAMEPAD_BUTTON_SOUTH; }
    int back_button() const { return confirm_game() ? SDL_GAMEPAD_BUTTON_SOUTH : SDL_GAMEPAD_BUTTON_EAST; }
    static mini::string face_name(const char *pos) {   // a generic controller is named like an Xbox one (A B X Y)
        if (pad_family != PadFamily::Generic) return pad_label(pos);
        return !strcmp(pos, "south") ? "A" : !strcmp(pos, "east") ? "B" : !strcmp(pos, "west") ? "X" : "Y";
    }
    mini::string ok_name() const { return face_name(confirm_game() ? "east" : "south"); }
    mini::string back_name() const { return face_name(confirm_game() ? "south" : "east"); }

    // [UIT 0304] when a change takes effect: the row tag ("restart": not changeable in the in-game overlay) and what a
    // change made while the game runs does (the game reads ultrainstincttears.cfg only when it starts)
    static mini::string applies_tag(const Setting &s) { return s.applies == Applies::Restart ? "restart" : s.applies == Applies::Setup ? "setup" : ""; }
    static mini::string running_note(const Setting &s) {
        if (s.applies == Applies::Live) return "Takes effect after restarting the game (the in-game overlay changes it now)";
        if (s.applies == Applies::Setup) return "Takes effect at the next setup run";
        return "Takes effect after restarting the game";
    }

    void set(int i, const Value &v) {
        const Setting &s = S[i];
        if (running && s.applies != Applies::Launcher) flash(running_note(s));
        cfg.set(s.key, write_value(s, v));
        if (!s.alias.empty()) cfg.set(s.alias, mini::nullopt);
        if (s.key == "TOTK_UPSCALER") {                 // keep the FSR 1 switch in step for builds without DLSS
            mini::string u = token(s, v);
            cfg.set("TOTK_FSR", u == "fsr" ? mini::optional<mini::string>("1") : mini::nullopt);
        }
        if (s.key == "TOTK_FSR" && !v.b && cfg.get("TOTK_UPSCALER") == mini::optional<mini::string>("fsr"))
            cfg.set("TOTK_UPSCALER", mini::nullopt);
        if (s.key == "TOTK_MODS" || s.key == "TOTK_MODS_DIR") mods_dirty = true;   // the Mods list shows it: rebuilt next frame
        if (s.key.find("TOTK_CHEAT_") == 0 && tab == TAB_MODS) build_items();   // [UIT 0175] Disable Mods counts the cheats
        save_at = now + 0.25;
    }

    // [UIT 0070] the build-time scan did not find this setting's game code: shown greyed out with the reason and not
    // changeable (reset to the default still works, so a value from an older build can be cleared)
    bool locked(int i) const { return !lift_unavailable(S[i], lift).empty(); }

    bool active(int i) const {
        const Setting &s = S[i];
        if (s.key == "TOTK_DLSS_MODE" && !dll) return false;
        if (!lift_unavailable(s, lift).empty()) return false;
        if (s.needsKey.empty()) return true;
        int j = idx(s.needsKey.c_str());
        if (j < 0) return true;
        mini::string t = tok(j);
        return mini::find(s.needsVals.begin(), s.needsVals.end(), t) != s.needsVals.end();
    }

    mini::string needs_note(int i) const {
        const Setting &s = S[i];
        if (s.key == "TOTK_DLSS_MODE" && !dll) return "Unavailable: nvngx_dlss.dll or sl.interposer.dll is not next to UltraInstinctTears.exe";
        mini::string why = lift_unavailable(s, lift);
        if (!why.empty()) return why;
        int j = idx(s.needsKey.c_str());
        if (j < 0) return {};
        mini::string labels;
        for (size_t k = 0; k < s.needsVals.size(); k++) {
            if (k) labels += k + 1 == s.needsVals.size() ? " or " : ", ";
            labels += display_token(S[j], s.needsVals[k]);
        }
        return "Applies when " + S[j].name + " is " + labels;
    }

    bool choice_ok(int i, int c) const {
        return !(S[i].key == "TOTK_UPSCALER" && S[i].choices[c].value == "dlss" && !dll);
    }

    bool want_dlss() const { return dll && (gpu.rtx || gpu.name.empty()); }

    Value recommended(int i) const {
        const Setting &s = S[i];
        if (s.key == "TOTK_UPSCALER") return read_value(s, mini::string(want_dlss() ? "dlss" : "fsr"));
        if (s.recommended == "-") return default_value(s);
        return read_value(s, s.recommended);
    }

    // Left/right on a value; dir -1 or +1. Returns true when it changed.
    bool step(int i, int dir, int repeat, bool fast) {
        const Setting &s = S[i];
        if (s.key == "TOTK_DLSS_MODE" && !dll) return false;
        if (locked(i)) return false;
        Value v = val(i), before = v;
        switch (s.type) {
        case Type::Bool: v.b = dir > 0; break;
        case Type::Choice: {
            if (v.kind == Value::Custom) { v = default_value(s); break; }
            int n = (int)s.choices.size(), c = v.index;
            do c += dir; while (c >= 0 && c < n && !choice_ok(i, c));
            if (c < 0 || c >= n) return false;
            v.index = c;
            break;
        }
        case Type::Int:
        case Type::Float: {
            if (v.kind == Value::Custom) { v = default_value(s); break; }
            if (v.kind == Value::Auto) {
                if (dir < 0) return false;
                v.kind = Value::Normal, v.num = s.min;
                break;
            }
            double mult = fast ? 10 : repeat > 30 ? 10 : repeat > 12 ? 4 : 1;
            double range = s.max - s.min;
            if (range / s.step > 400 && repeat > 30) mult = mini::max(mult, mini::floor(range / s.step / 40));
            double nv = v.num + dir * s.step * mult;
            if (nv < s.min - 1e-9) {
                if (!s.autoLabel.empty() || s.def == "-") { v.kind = Value::Auto; break; }
                nv = s.min;
            }
            nv = mini::min(nv, s.max);
            nv = s.min + mini::round((nv - s.min) / s.step) * s.step;
            if (nv == v.num) return false;
            v.num = nv;
            break;
        }
        case Type::Text: case Type::Pad: case Type::Key: return false;
        }
        if (same(s, v, before) && v.kind == before.kind) return false;
        set(i, v);
        return true;
    }

    // ---------------------------------------------------------------- layout model
    void build_items() {
        items.clear();
        swords = search_tab(tab) ? search_words(query[tab]) : mini::vector<mini::string>();
        if (!swords.empty()) { build_search_items(); return; }
        if (tab == TAB_PLAY) {
            for (int b : {B_Play, B_PlayLog, B_Recommend, B_ResetAll, B_OpenFolder, B_Quit}) items.push_back({Item::Button, b, "", "", 0});
            return;
        }
        if (tab == TAB_ABOUT) {
            for (int b : {B_OpenFolder, B_OpenCfg, B_OpenLogs, B_Backups, B_Update, B_Report, B_Speed}) items.push_back({Item::Button, b, "", "", 0});
            return;
        }
        if (tab == TAB_MODS) {   // 0059: install, the load order (highest first), then the Mods settings
            if (mods.nmods || !cheats_on_now().empty() || !cheats_before_disable().empty()) {   // [UIT 0062] first and obvious: the whole game unmodified in one press, and back ([UIT 0175] cheats too)
                items.push_back({Item::Heading, -1, "Play the unmodified game", "mods/vanilla", 46});
                items.push_back({Item::Button, B_ModAllOff, "", "mods/vanilla", 62});
            }
            items.push_back({Item::Heading, -1, "Install", "mods/install", 46});
            for (int b : {B_ModFile, B_ModFolder, B_ModInbox}) items.push_back({Item::Button, b, "", "mods/install", 62});
            items.push_back({Item::Heading, -1, "Load order: a higher mod wins a file both change", "mods/order", 46});
            if (!mods_on()) items.push_back({Item::Note, 1, "", "mods/order", 56});
            if (!mods.nmods) items.push_back({Item::Note, 0, "", "mods/order", 84});
            for (int k = 0; k < mods.nmods; k++) items.push_back({Item::Mod, k, "", "mods/order", 76});
            if (mods.nmods) items.push_back({Item::Button, B_ModAllOn, "", "mods/order", 62});
            items.push_back({Item::Button, B_ModOpen, "", "mods/order", 62});
            emit_groups("");
            return;
        }
        // Groups in the order the schema first names them (folded or not), except that a folded "X+" always sits right
        // after its visible "X"; a group's settings stay together even when the schema interleaves them.
        // Settings with a section= come after the tab's own groups, under one collapsible heading per section.
        mini::vector<mini::string> secs;
        mini::map<mini::string, const Setting *> sec_first;
        for (int i = 0; i < (int)S.size(); i++)
            if (S[i].tab == kTabKeys[tab] && !S[i].section.empty() && !sec_first.count(sec_gid(S[i])))
                secs.push_back(sec_gid(S[i])), sec_first[sec_gid(S[i])] = &S[i];
        emit_groups("");
        for (const mini::string &sg : secs) {
            items.push_back({Item::Group, -1, section_label(sg), sg, 58});
            if (expanded.count(sg)) emit_groups(sec_first[sg]->section);
        }
    }

    void emit_groups(const mini::string &section) {
        mini::vector<mini::string> order;
        mini::map<mini::string, mini::vector<int>> members;
        mini::map<mini::string, const Setting *> first;
        for (int i = 0; i < (int)S.size(); i++) {
            if (S[i].tab != kTabKeys[tab] || S[i].section != section) continue;
            mini::string g = gid(S[i]);
            if (!members.count(g)) order.push_back(g), first[g] = &S[i];
            members[g].push_back(i);
        }
        mini::vector<mini::string> sorted;   // schema order; a folded "X+" moves up to sit right after its visible "X"
        for (const mini::string &g : order)
            if (!first[g]->groupFolded) {
                sorted.push_back(g);
                if (members.count(g + "+")) sorted.push_back(g + "+");
            } else if (!members.count(g.substr(0, g.size() - 1)))
                sorted.push_back(g);
        for (const mini::string &g : sorted) {
            const Setting &s = *first[g];
            bool open = true;
            if (s.groupFolded) {
                open = expanded.count(g) > 0;
                // "Controller+" after "Controller": the folded part reads "More controller settings"
                mini::string label = s.group;
                if (members.count(g.substr(0, g.size() - 1))) {
                    mini::string low = lc_first(s.group);
                    if (low.size() > 2 && low.back() == 's' && low[low.size() - 2] != 's') low.pop_back();   // "Shaders": "More shader settings"
                    label = "More " + low + " settings";
                }
                items.push_back({Item::Group, -1, label, g, 58});
            } else
                items.push_back({Item::Heading, -1, s.group, g, 46});
            if (open) {
                for (int i : members[g]) items.push_back({Item::Row, i, "", g, 72});
                if (is_bind(members[g][0])) items.push_back({Item::Button, B_ResetGroup, "", g, 58});
            }
        }
    }

    bool is_bind(int i) const { return S[i].type == Type::Pad || S[i].type == Type::Key; }

    static mini::string gid(const Setting &s) {
        return s.tab + "/" + (s.section.empty() ? "" : s.section + "/") + s.group + (s.groupFolded ? "+" : "");
    }
    static mini::string sec_gid(const Setting &s) { return "sec:" + s.tab + "/" + s.section; }
    static bool is_sec(const mini::string &g) { return g.size() > 4 && g.compare(0, 4, "sec:") == 0; }
    mini::string section_label(const mini::string &sg) const {
        mini::string n = sg.substr(sg.rfind('/') + 1);
        return n == "Stutter" ? "Stutter: shader warm-up and texture streaming" : n;
    }

    int group_count(const mini::string &g, int *changed, mini::vector<int> *members = nullptr) const {
        int n = 0;
        *changed = 0;
        for (int i = 0; i < (int)S.size(); i++)
            if (gid(S[i]) == g || (is_sec(g) && !S[i].section.empty() && sec_gid(S[i]) == g)) {
                n++;
                *changed += modified(i);
                if (members) members->push_back(i);
            }
        return n;
    }

    void clamp_focus() {
        int &f = focus[tab];
        if (items.empty()) { f = 0; return; }
        f = mini::clamp(f, 0, (int)items.size() - 1);
        if (!items[f].focusable()) move_focus(1, false);
    }

    void move_focus(int dir, bool wrap) {
        int &f = focus[tab], n = (int)items.size();
        if (!n) return;
        int g = f;
        for (int k = 0; k < n; k++) {
            g += dir;
            if (g < 0 || g >= n) {
                if (!wrap) return;
                g = (g + n) % n;
            }
            if (items[g].focusable()) { f = g; return; }
        }
    }

    void page(int dir) {   // to the first row of the next/previous group
        int &f = focus[tab], n = (int)items.size();
        if (!n) return;
        auto starts = [&](int g) { return items[g].kind == Item::Group || items[g].kind == Item::Heading; };
        int cur = f;                                   // the group the focus is in
        while (cur > 0 && !starts(cur)) cur--;
        int g = cur + dir;
        while (g >= 0 && g < n && !starts(g)) g += dir;
        if (g < 0) g = 0;
        if (g >= n) g = n - 1;
        f = g;
        if (!items[f].focusable()) move_focus(1, false);
    }

    // ---------------------------------------------------------------- [UIT 0251] search
    static bool search_tab(int t) { return t != TAB_PLAY && t != TAB_ABOUT; }
    bool searching() const { return !swords.empty(); }
    int first_focusable() const {
        for (int k = 0; k < (int)items.size(); k++) if (items[k].focusable()) return k;
        return -1;
    }
    int last_focusable() const {
        for (int k = (int)items.size() - 1; k >= 0; k--) if (items[k].focusable()) return k;
        return -1;
    }
    // Only the matching rows, under one heading per group ("CPU" and its folded "CPU+" read as one), the group with
    // the best match first, the best match first inside it. Folding (`expanded`) is not touched, so clearing the
    // search brings every group back as it was.
    void build_search_items() {
        if (stext.size() != S.size()) { stext.clear(); for (auto &st : S) stext.push_back(search_text(st)); }
        smatch = 0;
        for (int t = 0; t < TAB_COUNT; t++) sother[t] = 0;
        mini::vector<mini::pair<int, int>> found;   // setting, score
        for (int i = 0; i < (int)S.size(); i++) {
            int sc = search_score(stext[i], swords);
            if (!sc) continue;
            for (int t = 1; t < TAB_ABOUT; t++)
                if (S[i].tab == kTabKeys[t]) { if (t == tab) found.push_back({i, sc}); else sother[t]++; }
        }
        if (tab == TAB_MODS) {   // installed mods by name, then the Mods settings
            bool head = false;
            for (int k = 0; k < mods.nmods; k++)
                if (search_score(search_text_name(mod_title(k)), swords)) {
                    if (!head) items.push_back({Item::Heading, -1, "Installed mods", "mods/order", 46}), head = true;
                    items.push_back({Item::Mod, k, "", "mods/order", 76});
                    smatch++;
                }
        }
        struct G { mini::string key, label; int best = 0; mini::vector<mini::pair<int, int>> rows; };
        mini::vector<G> gs;
        for (auto &h : found) {
            const Setting &st = S[h.first];
            mini::string k = st.section + "/" + st.group;
            size_t g = 0;
            while (g < gs.size() && gs[g].key != k) g++;
            if (g == gs.size()) {
                G n;
                n.key = k, n.label = st.section.empty() ? st.group : st.section + "  \xC2\xB7  " + st.group;
                gs.push_back(n);
            }
            gs[g].best = mini::max(gs[g].best, h.second);
            auto &r = gs[g].rows;
            size_t p = 0;
            while (p < r.size() && r[p].second >= h.second) p++;   // by score, schema order among equals
            r.insert(r.begin() + p, h);
        }
        for (size_t a = 1; a < gs.size(); a++)   // groups by their best match (stable)
            for (size_t b = a; b > 0 && gs[b].best > gs[b - 1].best; b--) { G t = gs[b]; gs[b] = gs[b - 1]; gs[b - 1] = t; }
        for (auto &g : gs) {
            items.push_back({Item::Heading, -1, g.label, "search", 46});
            for (auto &r : g.rows) items.push_back({Item::Row, r.first, "", gid(S[r.first]), 72});
            smatch += (int)g.rows.size();
        }
        if (!smatch) {
            items.push_back({Item::Note, 2, "", "search", 84});
            for (int t = 1; t < TAB_ABOUT; t++)
                if (sother[t]) items.push_back({Item::Button, B_SearchIn, "Show " + plural(sother[t], "match", "matches") + " in " + kTabNames[t], mini::to_string(t), 62});
        }
    }
    void set_sfield(bool on) {
        if (!on) sosk = false;
        if (on == sfield) return;
        sfield = on;
        if (win) {
            if (on) SDL_StartTextInput(win);
            else if (modal.kind != Modal::Edit) SDL_StopTextInput(win);
        }
    }
    // after the query changed: rebuild, first result focused, list from the top. Clearing it puts the focus back on
    // the setting that was focused (or its folded group) so the full list opens where the search left off.
    void search_changed() {
        int keep = -1;
        if (!items.empty() && items[focus[tab]].kind == Item::Row) keep = items[focus[tab]].idx;
        build_items();
        if (!searching() && keep >= 0) focus_setting(keep);
        else {
            int f0 = first_focusable();
            focus[tab] = f0 < 0 ? 0 : f0;
            scroll_to[tab] = 0;
        }
        follow = true;
        frect_valid = false;
    }
    void focus_setting(int i) {
        const Setting &st = S[i];
        int k = -1;
        for (int j = 0; j < (int)items.size() && k < 0; j++) if (items[j].kind == Item::Row && items[j].idx == i) k = j;
        for (int j = 0; j < (int)items.size() && k < 0; j++) if (items[j].kind == Item::Group && items[j].gid == gid(st)) k = j;
        for (int j = 0; j < (int)items.size() && k < 0; j++) if (items[j].kind == Item::Group && !st.section.empty() && items[j].gid == sec_gid(st)) k = j;
        if (k >= 0) focus[tab] = k;
        clamp_focus();
    }
    void search_set(const mini::string &q) {
        if (q == query[tab]) return;
        query[tab] = q;
        search_changed();
    }
    void search_type(const mini::string &c) {
        mini::string q = query[tab];
        for (char ch : c) if ((unsigned char)ch >= 0x20 && ch != 0x7f && q.size() < 64) q += ch;
        search_set(q);
    }
    void search_backspace(bool word = false) {
        mini::string q = query[tab];
        if (word) {   // Ctrl+Backspace: the last word
            while (!q.empty() && q.back() == ' ') q.pop_back();
            while (!q.empty() && q.back() != ' ') q.pop_back();
        } else if (!q.empty()) {
            size_t n = q.size() - 1;
            while (n > 0 && ((unsigned char)q[n] & 0xC0) == 0x80) n--;
            q.erase(n);
        }
        search_set(q);
    }
    void search_paste() {
        char *t = SDL_GetClipboardText();
        if (!t) return;
        mini::string c = t;
        SDL_free(t);
        for (auto &ch : c) if (ch == '\r' || ch == '\n' || ch == '\t') ch = ' ';
        search_type(c);
    }
    void open_search() {   // Ctrl+F, / or View: Play and About have no settings, so their search is the last settings tab's
        if (!search_tab(tab)) switch_tab(last_stab);
        set_sfield(true);
        if (dev == Device::Gamepad) sosk = true;
        follow = true;
    }
    void search_results() {   // from the field to the first result (stays in the field when nothing matches)
        int f0 = first_focusable();
        if (f0 < 0) return;
        set_sfield(false);
        focus[tab] = f0;
        follow = true;
    }
    void search_go(int t) {   // "Show N matches in <tab>": the same words there
        mini::string q = query[tab];
        set_sfield(false);
        query[t] = q;
        switch_tab(t);
        search_changed();
    }
    void handle_search_field(const Action &a) {
        switch (a.a) {
        case A_Char: search_type(a.ch); break;
        case A_Backspace: search_backspace(a.n == 1); break;
        case A_Reset: search_backspace(); break;
        case A_Paste: search_paste(); break;
        case A_Back:   // Esc / B: clear, then leave
            if (!query[tab].empty()) search_set("");
            else set_sfield(false);
            break;
        case A_Accept: case A_Search:
            if (dev == Device::Gamepad) sosk = true;
            else if (a.a == A_Accept) search_results();
            break;
        case A_Down: case A_Done: search_results(); break;
        case A_Up:   // wraps, as Up on the first row comes here
            if (a.repeat == 0) { int l = last_focusable(); if (l >= 0) { set_sfield(false); focus[tab] = l; follow = true; } }
            break;
        case A_TabPrev: case A_TabNext: case A_JumpTab: case A_PagePrev: case A_PageNext: case A_Home: case A_End:
        case A_Play: case A_PlayLog:
            set_sfield(false);
            handle(a);
            break;
        default: break;
        }
    }
    void search_key() {   // A on the search keyboard
        if (skr < SOSK_SPECIAL_ROW) { search_type(mini::string(1, kSearchRows[skr][skc])); return; }
        switch (skc) {
        case 0: search_type(" "); break;
        case 1: search_backspace(); break;
        case 2: search_set(""); break;
        case 3: search_results(); if (sfield) sosk = false; break;
        }
    }
    void handle_search_osk(const Action &a) {
        int cols = skr == SOSK_SPECIAL_ROW ? 4 : 10;
        switch (a.a) {
        case A_Char: search_type(a.ch); break;
        case A_Backspace: case A_Reset: search_backspace(); break;   // X
        case A_Shift: case A_Grab: search_type(" "); break;           // Y
        case A_Paste: search_paste(); break;
        case A_Accept: if (dev == Device::Gamepad) search_key(); else { search_results(); sosk = false; } break;
        case A_Done: case A_Play: case A_Search: search_results(); sosk = false; break;   // Start / View: done
        case A_Back: sosk = false; break;   // B: the keyboard closes, the field keeps the focus (B again clears)
        case A_Left: skc = (skc + cols - 1) % cols; break;
        case A_Right: skc = (skc + 1) % cols; break;
        case A_Up: case A_Down: {
            int r = (skr + (a.a == A_Down ? 1 : 4)) % 5;
            if (r == SOSK_SPECIAL_ROW && skr != SOSK_SPECIAL_ROW) skc = mini::min(3, skc * 4 / 10);
            else if (r != SOSK_SPECIAL_ROW && skr == SOSK_SPECIAL_ROW) skc = mini::min(9, skc * 10 / 4 + 1);
            skr = r;
            break;
        }
        default: break;
        }
    }

    void switch_tab(int t) {
        t = (t + TAB_COUNT) % TAB_COUNT;
        if (t == tab) return;
        set_sfield(false);
        tab = t;
        if (search_tab(t)) last_stab = t;
        grab = -1;
        if (t == TAB_MODS && task_kind == T_None) mods_reload();   // rebuilds the items
        build_items();
        clamp_focus();
        frect_valid = false;
    }

    void jump_to(int i) {     // show a setting: its tab, its group opened, focused
        const Setting &s = S[i];
        for (int t = 0; t < TAB_COUNT; t++)
            if (s.tab == kTabKeys[t]) { tab = t; break; }
        if (s.groupFolded) expanded.insert(gid(s));
        if (!s.section.empty()) expanded.insert(sec_gid(s));
        set_sfield(false);
        if (search_tab(tab)) last_stab = tab;
        build_items();
        bool shown = false;
        for (int k = 0; k < (int)items.size(); k++)
            if (items[k].kind == Item::Row && items[k].idx == i) focus[tab] = k, shown = true;
        if (!shown && searching()) {   // [UIT 0251] the tab's search hides it: clear the search
            query[tab].clear();
            build_items();
            for (int k = 0; k < (int)items.size(); k++)
                if (items[k].kind == Item::Row && items[k].idx == i) focus[tab] = k;
        }
        frect_valid = false;
    }

    // ---------------------------------------------------------------- actions
    void flash(const mini::string &t) { toast = t; toast_at = now; }

    void save_now() {
        if (save_at < 0) return;
        save_at = -1;
        save_error.clear();
        if (cfg.save(&save_error)) {
            saved_at = now;
            cfg.exists = true;
            SDL_PathInfo info;
            if (SDL_GetPathInfo(cfg.path.c_str(), &info)) cfg_mtime = info.modify_time;
        }
    }

    void reload_if_changed() {   // someone edited ultrainstincttears.cfg (or setup.py ran) while the launcher was open
        SDL_PathInfo info;
        if (save_at >= 0 || !SDL_GetPathInfo(cfg.path.c_str(), &info) || info.modify_time == cfg_mtime) return;
        cfg.load(cfg.path);
        cfg_mtime = info.modify_time;
        flash("ultrainstincttears.cfg changed on disk: reloaded");
    }

    // ---------------------------------------------------------------- [UIT 0036] Install update, Report a problem
    // Both run a Python helper from tools\ (the same Python that builds the game): tools/update.py finds and installs a
    // source package (local zips, and [UIT 0281] the newest GitHub release when the player presses Install update),
    // tools/report.py zips the logs and settings. The launcher shows their progress and results; every dialog works with
    // a controller. The network is only ever used by that helper process, only after the press, never while the game
    // runs (run_helper refuses then, and Play waits for a check or a download): the game is never slowed by an update.
    enum TaskKind { T_None, T_Scan, T_Install, T_Report, T_Mods, T_Guide, T_Build, T_Download };   // [UIT 0149] T_Guide/T_Build: PGO
    mini::string build_args;   // [UIT 0149] the setup.py options of the running T_Build
    Game task;
    TaskKind task_kind = T_None;
    mini::string task_out, task_prog;
    double task_poll = 0;
    SDL_Mutex *pick_mu = nullptr;
    mini::vector<mini::string> picked;   // from the file dialog (its callback may run on another thread)
    bool picked_ready = false;
    enum PickKind { P_Update, P_Mod } pick_kind = P_Update;   // what the dialog was opened for
    bool scan_file = false;
    struct Pkg { mini::string ref, label, version, commit, when; };
    // [UIT 0281] scan --online's "release" line: the newest GitHub release (tools/update.py online_line)
    struct Rel { mini::string tag, version, asset, when, sha, notes, url, mine; long long size = 0; bool newer = false, pre = false; };
    bool scan_online = false;
    // [UIT 0320] Advanced > Updates and reports > Check for updates when the launcher starts: once per launcher start,
    // a few seconds in, tools/update.py check in its own background process (not run_helper: no dialog, no busy task,
    // Play and every button stay free). Never while the game runs (Play stops it; it asks again once the game has
    // closed), never after --relaunch, a short time limit, errors only logged. A newer release: a dot on About, one line
    // at the bottom ("Update available: v1.0.x - About > Install update"), never a dialog.
    enum { UC_Off, UC_Armed, UC_Asking, UC_Done } ucheck_state = UC_Off;
    Game ucheck;
    double ucheck_at = 0, ucheck_t0 = 0;
    bool ucheck_said_wait = false, relaunched = false;
    mini::string ucheck_out;
    bool update_avail = false;   // the dot on About and the line at the bottom
    mini::string avail_tag;
    static constexpr double kUcheckDelay = 3.0, kUcheckKill = 20.0;   // seconds after the start; the launcher's own limit

    mini::string setting_text(const char *key) const {
        int i = idx(key);
        auto r = i >= 0 ? raw(i) : cfg.get(key);
        return r && !r->empty() && *r != "-" ? *r : mini::string();
    }
    static mini::string quote(const mini::string &x) { return "\"" + x + "\""; }
    mini::string python_cmd() const {
        mini::string py = setting_text("TOTK_LAUNCHER_PYTHON");
#ifdef _WIN32
        mini::string bundled = join(join(join(dir, "deps"), "python"), "python.exe");   // the package's own (setup.py uses it)
        if (py.empty() && file_exists(bundled)) return quote(bundled);
        return py.empty() ? mini::string("py -3") : py;
#else
        return py.empty() ? mini::string("python3") : py;
#endif
    }
    static mini::string read_text(const mini::string &path) {   // [UIT 0046] shared: never blocks a helper's rename
        return read_shared(path);
    }
    static mini::vector<mini::string> split(const mini::string &t, char c) {
        mini::vector<mini::string> out;
        size_t a = 0;
        for (size_t k = 0; k <= t.size(); k++)
            if (k == t.size() || t[k] == c) {
                mini::string x = t.substr(a, k - a);
                if (!x.empty() && x.back() == '\r') x.pop_back();
                out.push_back(x);
                a = k + 1;
            }
        return out;
    }
    static mini::string json_get(const mini::string &t, const char *key) {   // one field of update.py's progress file
        mini::string k = mini::string("\"") + key + "\": ";
        size_t p = t.find(k);
        if (p == mini::string::npos) return {};
        p += k.size();
        mini::string v;
        if (p < t.size() && t[p] == '"') {
            for (p++; p < t.size() && t[p] != '"'; p++) {
                if (t[p] == '\\' && p + 1 < t.size()) {
                    char e = t[++p];
                    if (e == 'u') { v += '?'; p += 4; }
                    else v += e == 'n' ? ' ' : e == 't' ? ' ' : e;
                } else v += t[p];
            }
        } else for (; p < t.size() && t[p] != ',' && t[p] != '}'; p++) v += t[p];
        return v;
    }
    mini::string last_lines(const mini::string &path, int n) const {
        auto ls = split(read_text(path), '\n');
        mini::string out;
        int have = 0;
        for (int k = (int)ls.size() - 1; k >= 0 && have < n; k--)
            if (!ls[k].empty()) { out = ls[k] + (out.empty() ? "" : "\n") + out; have++; }
        return out;
    }

    bool run_helper(TaskKind kind, const char *script, const mini::string &args, const mini::string &title, const mini::string &body) {
        if (task_kind != T_None) { flash("Still busy with the last one"); return false; }
        if (running) { flash("Close the game first"); return false; }
        mini::string tools = join(dir, "tools"), helper = join(tools, script);
        if (!file_exists(helper)) {
            message(title, mini::string("tools\\") + script + " is not in the game folder. It comes with the source package: this works in a "
                    "folder that tools/setup.py built in place.");
            return false;
        }
        save_now();
        mini::string logs = join(dir, "logs");
        SDL_CreateDirectory(logs.c_str());
        task_out = join(logs, mini::string(kind == T_Report ? "report" : kind == T_Scan ? "update-scan" : kind == T_Mods ? "mods-run" :
                                           kind == T_Guide ? "pgo-guide" : kind == T_Build ? "pgo-build" : kind == T_Download ? "update-fetch" :
                                           "update-run") + ".txt");
        mini::string err;
        if (!start_process(python_cmd() + " " + quote(helper) + args, dir, task_out, &task, &err)) {
            message("Could not start " + mini::string(script), err);
            return false;
        }
        task_kind = kind;
        modal = Modal();
        modal.kind = Modal::Message;
        modal.title = title, modal.body = body, modal.busy = true, modal.pct = kind == T_Install || kind == T_Mods || kind == T_Download ? 0.0f : -1.0f;
        modal.open_t = (float)now;
        return true;
    }

    void update_start(const mini::string &file = {}) {
        mini::string args = " scan --tsv";
        if (!file.empty()) args += " --file " + quote(file);
        scan_file = !file.empty();
        // [UIT 0281] Advanced > Updates and reports > Check GitHub for updates (on unless 0): a picked zip is never checked online
        scan_online = file.empty() && setting_text("TOTK_LAUNCHER_UPDATE_ONLINE") != "0";
        if (scan_online) args += " --online";
        run_helper(T_Scan, "update.py", args, "Looking for an update",
                   !file.empty() ? "Reading " + file : scan_online ? "Asking GitHub for the newest release and looking for UltraInstinctTears-src*.zip packages..."
                                                                   : "Looking for UltraInstinctTears-src*.zip packages...");
    }
    // [UIT 0320] arms the start-up check (startup and the startupcheck script step); delay in seconds
    void startup_check(double delay) {
        if (relaunched) { SDL_Log("update check: skipped (the launcher restarted itself after an update)"); return; }
        if (setting_text("TOTK_LAUNCHER_UPDATE_ONLINE") == "0") { SDL_Log("update check: off (Check GitHub for updates is off)"); return; }
        if (setting_text("TOTK_LAUNCHER_UPDATE_ON_START") == "0") { SDL_Log("update check: off (Check for updates when the launcher starts is off)"); return; }
        if (!file_exists(join(join(dir, "tools"), "update.py"))) { SDL_Log("update check: skipped (no tools/update.py in the game folder)"); return; }
        ucheck_state = UC_Armed, ucheck_at = now + delay, ucheck_said_wait = false;
    }
    void ucheck_stop(const char *why) {   // Play pressed, or the launcher closes: the check never runs alongside the game
        if (ucheck_state != UC_Asking) return;
        stop_process(&ucheck);
        SDL_Log("update check: stopped (%s)", why);
        note_ucheck(mini::string("stopped: ") + why + ", checks again after the game closes");
        ucheck_state = UC_Armed, ucheck_at = now + kUcheckDelay, ucheck_said_wait = false;   // once more, after the game
    }
    void note_ucheck(const mini::string &line) {   // one line for logs\update-check.txt (players see no launcher log)
        if (SDL_IOStream *f = SDL_IOFromFile(ucheck_out.c_str(), "ab")) {
            mini::string l = "launcher\t" + line + "\n";
            SDL_WriteIO(f, l.c_str(), l.size());
            SDL_CloseIO(f);
        }
    }
    void ucheck_poll() {   // every frame: cheap (a flag, or one non-blocking look at the helper process)
        if (ucheck_state == UC_Armed) {
            if (running) {
                if (!ucheck_said_wait) SDL_Log("update check: waiting until the game has closed"), ucheck_said_wait = true;
                ucheck_at = mini::max(ucheck_at, now + kUcheckDelay);   // a few seconds after it closes, not at once
                return;
            }
            if (now < ucheck_at) return;
            mini::string logs = join(dir, "logs"), err;
            SDL_CreateDirectory(logs.c_str());
            ucheck_out = join(logs, "update-check.txt");
            if (!start_process(python_cmd() + " " + quote(join(join(dir, "tools"), "update.py")) + " check", dir, ucheck_out, &ucheck, &err)) {
                SDL_Log("update check: could not start: %s", err.c_str());
                ucheck_state = UC_Done;
                return;
            }
            ucheck_state = UC_Asking, ucheck_t0 = now;
            return;
        }
        if (ucheck_state != UC_Asking) return;
        if (running) { ucheck_stop("the game started"); return; }
        if (ucheck.running()) {
            if (now - ucheck_t0 > kUcheckKill) {
                stop_process(&ucheck);
                ucheck_state = UC_Done;
                SDL_Log("update check: no answer in %.0f s: given up", kUcheckKill);
                note_ucheck("given up: no answer in 20 s");
            }
            return;
        }
        ucheck_state = UC_Done;
        mini::string error;
        for (auto &l : split(read_text(ucheck_out), '\n')) {
            auto f = split(l, '\t');
            Rel r;
            if (f.size() >= 3 && f[0] == "online") error = tsv_unescape(f[2]);
            if (!parse_rel(f, &r)) continue;
            if (!r.newer) { SDL_Log("update check: up to date (%s is the newest release)", r.tag.c_str()); update_avail = false; return; }
            update_avail = true, avail_tag = r.tag;
            SDL_Log("update check: %s is available (installed: %s)", r.tag.c_str(), r.mine.empty() ? "unknown" : r.mine.c_str());
            flash(avail_line());
            return;
        }
        SDL_Log("update check: could not check: %s", error.empty() ? last_lines(ucheck_out, 3).c_str() : error.c_str());
    }
    mini::string avail_line() const { return "Update available: " + avail_tag + " - About > Install update"; }
    static bool parse_rel(const mini::vector<mini::string> &f, Rel *rel) {   // update.py online_line's "release" line
        if (f.size() < 12 || f[0] != "release") return false;
        rel->tag = tsv_unescape(f[1]), rel->version = tsv_unescape(f[2]), rel->newer = f[3] == "1", rel->asset = tsv_unescape(f[4]);
        rel->size = atoll(f[5].c_str()), rel->when = tsv_unescape(f[6]), rel->pre = f[7] == "1", rel->sha = f[8];
        rel->notes = tsv_unescape(f[9]), rel->url = tsv_unescape(f[10]), rel->mine = tsv_unescape(f[11]);
        return true;
    }
    static mini::string tsv_unescape(const mini::string &x) {   // update.py tsv_escape: \\ \n \t
        mini::string o;
        for (size_t k = 0; k < x.size(); k++) {
            if (x[k] == '\\' && k + 1 < x.size()) { char e = x[++k]; o += e == 'n' ? '\n' : e == 't' ? '\t' : e; }
            else o += x[k];
        }
        return o;
    }
    static mini::string mb(long long n) {
        char b[32];
        if (n >= (1 << 20)) snprintf(b, sizeof b, "%.1f MB", n / 1048576.0); else snprintf(b, sizeof b, "%lld KB", (n + 1023) / 1024);
        return b;
    }
    // [UIT 0281] A newer GitHub release: what's new, Install downloads and checks it (update.py download), then the same
    // install as a local zip. "Use a zip instead" is the local path, always there.
    void offer_release(const Rel &r, const mini::vector<Pkg> &pk, const Pkg &cur) {
        mini::string notes = r.notes.empty() ? mini::string("(The release has no notes.)") : r.notes;
        confirm("Update available: " + r.tag,
                "What's new:\n" + notes + "\n\nInstall downloads it from GitHub, checks it (" +
                (r.sha.empty() ? mini::string("its size; this release publishes no SHA-256") : mini::string("its size and SHA-256")) +
                ") and installs it like a zip: your saves, settings and game data stay as they are, the files it replaces are backed up "
                "and put back if the rebuild fails. The rebuild takes a few minutes; keep the launcher open.", "Install", false,
                [this, r] { release_download(r.tag); });
        modal.cancel = "Later";
        modal.rows.push_back({"Installed now", cur.version.empty() ? (r.mine.empty() ? mini::string("unknown") : r.mine)
                                                                   : cur.version + " " + cur.commit + "  (" + cur.when + ")"});
        modal.rows.push_back({"New version", r.version + (r.pre ? "  (pre-release)" : "") + (r.when.empty() ? mini::string() : "  (" + r.when + ")")});
        modal.rows.push_back({"Download", r.asset + ", " + mb(r.size)});
        modal.alt = "Use a zip instead";
        mini::vector<Pkg> keep = pk;
        Pkg c = cur;
        modal.on_alt = [this, keep, c] { if (keep.empty()) pick_zip(); else offer_update(keep, c, false, {}); };
    }
    void release_download(const mini::string &tag) {
        task_prog = join(join(dir, "logs"), "update-progress.json");
        SDL_RemovePath(task_prog.c_str());
        run_helper(T_Download, "update.py", " download --tag " + quote(tag) + " --progress " + quote(task_prog),
                   "Downloading " + tag, "Asking GitHub for the release...");
    }

    static void SDLCALL picked_cb(void *ud, const char *const *files, int) {
        App *a = (App *)ud;
        SDL_LockMutex(a->pick_mu);
        a->picked.clear();
        for (int k = 0; files && files[k]; k++) a->picked.push_back(mini::string(files[k]));
        a->picked_ready = true;
        SDL_UnlockMutex(a->pick_mu);
    }
    void pick_zip() {
        static const SDL_DialogFileFilter f[] = {{"Ultra Instinct Tears source package (zip)", "zip"}};
        if (!pick_mu) pick_mu = SDL_CreateMutex();
        pick_kind = P_Update;
        SDL_ShowOpenFileDialog(picked_cb, this, win, f, 1, nullptr, false);
    }

    // online: what the GitHub check found when it offers nothing newer ("" = not checked), shown with the local result
    void offer_update(const mini::vector<Pkg> &pk, const Pkg &cur, bool picked_one, const mini::string &online) {
        if (pk.empty()) {
            modal = Modal();
            modal.kind = Modal::Confirm;
            modal.title = "No update package found";
            modal.body = (online.empty() ? mini::string() : "GitHub: " + online + "\n\n") +
                         "Looked for UltraInstinctTears-src*.zip in the game folder, the folder above it and the folders set in Advanced > "
                         "Updates and reports. Choose a zip yourself, or add the folder where you keep them there.";
            modal.ok = "Choose a zip...", modal.cancel = "Close";
            modal.on_ok = [this] { pick_zip(); };
            modal.open_t = (float)now;
            return;
        }
        const Pkg &p = pk[0];
        bool same = !p.commit.empty() && p.commit == cur.commit;
        mini::string zip = p.ref;
        confirm(same ? "This version is already installed" : "Install this update?",
                mini::string(same ? "Reinstalling puts its files back as they came. " : "") +
                "Your saves, settings and game data stay as they are. The files it replaces are backed up in updates\\ and put back "
                "if the rebuild fails. The rebuild runs tools/setup.py and takes a few minutes, longer when the game's code must "
                "be translated again; keep the launcher open.", same ? "Install anyway" : "Install", false,
                [this, zip, same] { update_install(zip, same); });
        modal.rows.push_back({"Installed now", cur.version.empty() ? mini::string("unknown") : cur.version + " " + cur.commit + "  (" + cur.when + ")"});
        modal.rows.push_back({"Package", p.version + " " + p.commit + "  (" + p.when + ")"});
        modal.rows.push_back({"File", p.label});
        if (!picked_one && pk.size() > 1) modal.rows.push_back({"Other packages found", mini::to_string(pk.size() - 1) + " (older)"});
        if (!online.empty()) modal.rows.push_back({"GitHub", online});
        modal.alt = picked_one ? "Choose another..." : "Choose a zip...";
        modal.on_alt = [this] { pick_zip(); };
    }

    void update_install(const mini::string &zip, bool force) {
        task_prog = join(join(dir, "logs"), "update-progress.json");
        SDL_RemovePath(task_prog.c_str());
        run_helper(T_Install, "update.py", " install " + quote(zip) + " --progress " + quote(task_prog) + (force ? " --force" : ""),
                   "Installing the update", "Starting...");
    }

    // ---------------------------------------------------------------- [UIT 0149] About > Faster game code (PGO)
    // The three PGO steps of tools/setup.py (--pgo-guide says which is next) as buttons: step 1 builds the profiling
    // exe (--pgo-profile), step 2 is playing, step 3 builds the fast exe from the counts (--pgo); --no-pgo goes back.
    void pgo_start() {
        run_helper(T_Guide, "setup.py", " --pgo-guide --pgo-tsv", "Faster game code (PGO)", "Reading where you are in the steps...");
    }
    void pgo_build(const mini::string &opt) {
        build_args = opt;
        run_helper(T_Build, "setup.py", " " + opt, opt == "--no-pgo" ? "Building the normal version" :
                   opt == "--pgo" ? "Step 3: building the fast version" : "Step 1: building the profiling version",
                   "Starting tools\\setup.py " + opt + " (this compiles all of the game's C again: often 10-30 minutes; keep the launcher open)...");
    }
    void pgo_guide_done(int code) {
        auto ls = split(read_text(task_out), '\n');
        mini::string state, body;
        int raws = 0;
        for (auto &l : ls) {
            auto f = split(l, '\t');
            if (f.size() >= 3 && f[0] == "pgo-state") { state = f[1]; raws = atoi(f[2].c_str()); continue; }
            if (!l.empty()) body += (body.empty() ? "" : "\n") + l;
        }
        if (code != 0 || state.empty()) {
            message("Could not read the PGO steps", last_lines(task_out, 6) + "\n\nThe Python command is in Advanced > Updates and reports.");
            return;
        }
        modal = Modal();
        modal.kind = Modal::Confirm;
        modal.title = "Faster game code (PGO)";
        modal.body = body;
        modal.cancel = "Close";
        modal.open_t = (float)now;
        if (state == "normal") {
            modal.ok = "Step 1: build the profiling version";
            modal.on_ok = [this] { pgo_build("--pgo-profile"); };
            if (raws > 0) { modal.alt = "Step 3: build the fast version"; modal.on_alt = [this] { pgo_build("--pgo"); }; }
        } else if (state == "profiling") {
            if (raws > 0) { modal.ok = "Step 3: build the fast version"; modal.on_ok = [this] { pgo_build("--pgo"); }; }
            else modal.ok = "OK";
            modal.alt = "Back to the normal build";
            modal.on_alt = [this] { pgo_build("--no-pgo"); };
        } else {   // fast
            modal.ok = "OK";
            modal.alt = "Back to the normal build";
            modal.on_alt = [this] { pgo_build("--no-pgo"); };
        }
    }
    void pgo_build_done(int code) {
        if (code != 0) {
            message("The build failed", last_lines(task_out, 8) + "\n\nThe game folder keeps the exe it had before when the build stops early. "
                    "Full output: " + task_out);
            mini::string l = task_out;
            modal.alt = "Open log";
            modal.on_alt = [l] { open_path(l); };
            return;
        }
        if (build_args == "--pgo-profile")
            message("Profiling version built", "Step 2: press Play and play 10 minutes or more the way you normally do (explore, a fight, "
                    "Ultrahand, the sky, a cave, a town). It runs slower while it counts. Close the game normally; the counts are "
                    "saved every 30 s and at exit. Then come back to About > Faster game code (PGO) for step 3.");
        else if (build_args == "--pgo")
            message("Fast version built", "The game now uses the PGO build made from your play. Later updates keep using it; after a big "
                    "update you can redo the steps.");
        else message("Back on the normal build", "The normal build (no PGO) is installed again.");
    }

    void report_start() {
        run_helper(T_Report, "report.py", "", "Making a problem report", "Collecting the newest logs, your settings and the version...");
    }

    void poll_task() {
        if (pick_mu) {
            SDL_LockMutex(pick_mu);
            bool ready = picked_ready;
            mini::vector<mini::string> f = picked;
            picked_ready = false;
            SDL_UnlockMutex(pick_mu);
            if (ready && !f.empty()) {
                if (pick_kind == P_Mod) mods_install(f, "install");
                else update_start(f[0]);
            }
        }
        if (task_kind == T_None) return;
        if (task_kind == T_Build && modal.busy && now - task_poll > 0.5) {   // [UIT 0149] setup.py's newest line
            task_poll = now;
            mini::string l = last_lines(task_out, 1);
            if (!l.empty()) modal.body = l.substr(0, 300);
        }
        if ((task_kind == T_Install || task_kind == T_Mods || task_kind == T_Download) && (now - task_poll > 0.25 || snap)) {
            task_poll = now;
            mini::string t = read_text(task_prog);
            if (!t.empty() && modal.busy) {
                mini::string step = json_get(t, "step"), msg = json_get(t, "msg");
                modal.pct = (float)atoi(json_get(t, "pct").c_str());
                modal.body = msg.empty() || msg[0] == '[' ? step : msg;
            }
        }
        if (task.running()) return;
        TaskKind k = task_kind;
        task_kind = T_None;
        int code = task.exit_code;
        close_modal(true);
        if (k == T_Mods) { mods_done(code); return; }
        if (k == T_Guide) { pgo_guide_done(code); return; }
        if (k == T_Build) { pgo_build_done(code); return; }
        if (k == T_Scan) {
            auto ls = split(read_text(task_out), '\n');
            mini::vector<Pkg> pk;
            Pkg cur;
            mini::string error, online;
            Rel rel;
            bool have_rel = false;
            for (auto &l : ls) {
                auto f = split(l, '\t');
                if (f.size() >= 2 && f[0] == "error") error = f[1];
                if (f.size() >= 3 && f[0] == "online" && f[1] == "error") online = "could not check: " + tsv_unescape(f[2]);
                if (parse_rel(f, &rel)) {   // [UIT 0281]
                    have_rel = true;
                    update_avail = rel.newer, avail_tag = rel.tag;   // [UIT 0320] the notice follows the newest answer
                    if (!rel.newer) online = "up to date (" + rel.tag + " is the newest release)";
                    continue;
                }
                if (f.size() < 6) continue;
                Pkg p{f[1], f[2], f[3], f[4], f[5]};
                if (f[0] == "current") cur = p;
                else if (f[0] == "package") pk.push_back(p);
            }
            if (code != 0 || (!error.empty() && pk.empty())) {
                message("Could not read the update package", error.empty() ? last_lines(task_out, 6) : error);
                return;
            }
            if (scan_online && online.empty() && !have_rel) online = "could not check (no answer from tools\\update.py: an older one?)";
            if (have_rel && rel.newer) offer_release(rel, pk, cur);
            else offer_update(pk, cur, scan_file, online);
        } else if (k == T_Download) {   // [UIT 0281] downloaded and checked: the same install as a local zip
            mini::string t = read_text(task_prog), st = json_get(t, "state"), msg = json_get(t, "msg"), zip = json_get(t, "zip");
            if (code == 0 && st == "done" && !zip.empty() && file_exists(zip)) { update_install(zip, false); return; }
            message("The download failed", (msg.empty() ? last_lines(task_out, 6) : msg) +
                    "\n\nNothing was installed; the game folder is as it was. You can choose a zip yourself instead.");
            modal.alt = "Choose a zip...";
            modal.on_alt = [this] { pick_zip(); };
        } else if (k == T_Install) {
            mini::string t = read_text(task_prog), st = json_get(t, "state"), msg = json_get(t, "msg"), lg = json_get(t, "log");
            if (code == 0 && st == "done") {
                update_avail = false;   // [UIT 0320]
                modal = Modal();
                modal.kind = Modal::Confirm;
                modal.title = "Update installed";
                modal.body = msg + ". Your settings are kept. Restart the launcher to use its new version too.";
#ifdef _WIN32
                modal.ok = "Restart launcher", modal.cancel = "Later";
                modal.on_ok = [this] {
                    mini::string err, l = join(join(dir, "logs"), "launcher-restart.txt");
                    si_release();   // [UIT 0272] the new copy takes the folder's mutex (--relaunch: it waits for it)
                    if (start_process(quote(join(dir, "UltraInstinctTearsLauncher.exe")) + " --relaunch", dir, l, &task, &err)) quit = true;
                    else si_claim(dir, true, 0), message("Could not restart the launcher", err);
                };
#else
                modal.ok = "OK";
#endif
                modal.open_t = (float)now;
            } else {
                message(st == "rolled_back" ? "The update failed and was undone" : "The update failed",
                        (msg.empty() ? last_lines(task_out, 6) : msg) +
                        (st == "rolled_back" ? "\n\nThe previous version is back in place and plays as before." : "") +
                        (lg.empty() ? mini::string() : "\n\nLog: " + lg));
                mini::string l = lg.empty() ? task_out : lg;
                modal.alt = "Open log";
                modal.on_alt = [l] { open_path(l); };
            }
        } else if (k == T_Report) {
            mini::string zip = last_lines(task_out, 1);
            if (code != 0 || zip.find(".zip") == mini::string::npos) {
                message("Could not make the report", last_lines(task_out, 6) + "\n\nThe Python command is in Advanced > Updates and reports.");
                return;
            }
            mini::string notes = last_lines(task_out, 3);
            size_t zp = notes.find(zip);
            if (zp != mini::string::npos) notes = notes.substr(0, zp);
            message("Problem report saved", zip + "\n\n" + notes.substr(0, 400) +
                    "\n\nAttach this zip to your bug report. It holds logs and settings only: no game files or saves. "
                    "Your settings file shows your folder paths.");
            size_t sl = zip.rfind('\\'), sl2 = zip.rfind('/');
            size_t cut = sl == mini::string::npos ? sl2 : sl2 == mini::string::npos ? sl : mini::max(sl, sl2);
            mini::string folder = cut == mini::string::npos ? dir : zip.substr(0, cut);
            modal.alt = "Show in folder";
            modal.on_alt = [folder] { open_path(folder); };
        }
    }

    // ---------------------------------------------------------------- [UIT 0059] Mods tab
    // The list is src/host/modfs.c itself (built into the launcher), so it shows exactly the order, the on/off states
    // and the winner of each file the game will use. Order and on/off go to <mods>/load-order.txt; installing and
    // uninstalling run tools/mods.py (zip / 7z / rar / folder, layout detection, broken archives) like Install update.
    // Nothing here runs while the game runs, and nothing is read by the game when there are no mods.
    Modfs mods{};
    mini::string mods_loaded;            // the folder mods was read from
    SDL_Time mods_mtime = 0;
    double mods_check = 0;
    mini::vector<mini::string> inbox;    // waiting in mods-inbox
    int grab = -1;                       // the mod being moved (pad: Y, then up / down; A, B or Y drops it)
    mini::string mods_focus;             // focus this mod (folder name) after the next list rebuild
    mini::string mods_op;                // what the running T_Mods task does: install, inbox, remove, undo
    mini::vector<mini::string> mods_placed;   // per mod (same index): game paths it was placed at by file name (uit-placed.txt)

    // "UI/LayoutArchive/x.zs" -> "UI > LayoutArchive" (where) and "x.zs" (the file)
    static mini::string place_dir(const mini::string &g) {
        size_t sl = g.rfind('/');
        if (sl == mini::string::npos) return "the top of the game's files";
        mini::string o;
        for (char ch : g.substr(0, sl)) { if (ch == '/') o += " > "; else o += ch; }
        return o;
    }
    static mini::string place_file(const mini::string &g) {
        size_t sl = g.rfind('/');
        return sl == mini::string::npos ? g : g.substr(sl + 1);
    }
    // plain words for files placed by name: "Placed in UI > LayoutArchive, replacing the game's Common...blarc.zs."
    static mini::string placed_text(const mini::vector<mini::string> &ps, size_t show = 4) {
        if (ps.size() == 1) return "Placed in " + place_dir(ps[0]) + ", replacing the game's " + place_file(ps[0]) + ".";
        mini::string t = "Placed " + num((long long)ps.size()) + " files, each replacing the game's file of that name:";
        for (size_t i = 0; i < ps.size() && i < show; i++) t += "\n\xE2\x80\xA2 " + place_file(ps[i]) + " in " + place_dir(ps[i]);
        if (ps.size() > show) t += "\n\xE2\x80\xA2 and " + num((long long)(ps.size() - show)) + " more";
        return t;
    }
    mini::vector<mini::string> dropped;  // files dropped on the window (SDL_EVENT_DROP_FILE)
    bool drop_open = false;
    struct Canon {               // the game's RomFS files a mod also ships, cached across reloads (a hash set: big mods
        mini::string root, buf;  // have tens of thousands of files, and moving a mod reloads the list)
        mini::vector<mini::string> keys;
        mini::vector<signed char> vals;   // 0 empty, 1 the game has it, -1 it does not
        size_t used = 0;
        static uint64_t hash(const char *p) { uint64_t h = 1469598103934665603ull; for (; *p; p++) h = (h ^ (unsigned char)*p) * 1099511628211ull; return h; }
        signed char *slot(const char *p) {
            if ((used + 1) * 2 > keys.size()) {
                mini::vector<mini::string> ok = mini::move(keys);
                mini::vector<signed char> ov = mini::move(vals);
                size_t n = ok.size() ? ok.size() * 2 : 1024;
                keys = mini::vector<mini::string>(), vals = mini::vector<signed char>();
                keys.resize(n), vals.resize(n);
                for (size_t k = 0; k < n; k++) vals[k] = 0;
                used = 0;
                for (size_t k = 0; k < ok.size(); k++) if (ov[k]) *slot(ok[k].c_str()) = ov[k], keys[last] = mini::move(ok[k]);
            }
            size_t m = keys.size() - 1, i = (size_t)hash(p) & m;
            while (vals[i] && keys[i] != p) i = (i + 1) & m;
            if (!vals[i]) keys[i] = p, used++;
            last = i;
            return &vals[i];
        }
        size_t last = 0;
        bool indexed = false;    // root holds setup.py's romfs.bin + runtime-index.bin: every game path, lower case, is in the set
        void clear() { keys = mini::vector<mini::string>(), vals = mini::vector<signed char>(), used = 0, indexed = false; }
        static mini::string lower(const char *p) { mini::string l; for (; *p; p++) l += (char)SDL_tolower((unsigned char)*p); return l; }
        bool load_index(const mini::string &file) {   // "TOTKRFS1", u64 count, u64 strings; {u64 off, u64 size, u32 name, u32 len}
            size_t n = 0;
            unsigned char *b = (unsigned char *)SDL_LoadFile(file.c_str(), &n);
            if (!b) return false;
            auto u64 = [&](size_t o) { uint64_t v = 0; for (int k = 7; k >= 0; k--) v = v << 8 | b[o + k]; return v; };
            auto u32 = [&](size_t o) { uint32_t v = 0; for (int k = 3; k >= 0; k--) v = v << 8 | b[o + k]; return v; };
            bool ok = n >= 24 && !SDL_memcmp(b, "TOTKRFS1", 8);
            uint64_t cnt = ok ? u64(8) : 0, so = ok ? u64(16) : 0;
            if (ok && (cnt > (n - 24) / 24 || so > n)) ok = false;
            for (uint64_t i = 0; ok && i < cnt; i++) {
                size_t e = 24 + (size_t)i * 24;
                uint64_t no = so + u32(e + 16), nl = u32(e + 20);
                if (nl == 0 || no + nl > n) { ok = false; break; }
                mini::string l = lower(mini::string((const char *)b + no, (size_t)nl - 1).c_str());
                *slot(l.c_str()) = 1;
            }
            SDL_free(b);
            if (!ok) clear();
            return indexed = ok;
        }
    };
    Canon canon;                         // RomFS lookups for "files the game does not have" (cached across reloads)

    mini::string abs_path(const mini::string &p) const {
        bool abs = !p.empty() && (p[0] == '/' || p[0] == '\\' || (p.size() > 1 && p[1] == ':'));
        return abs ? p : join(dir, p);
    }
    mini::string mods_dir() const {
        mini::string m = setting_text("TOTK_MODS_DIR");
        return abs_path(m.empty() ? mini::string("mods") : m);
    }
    static mini::string trim_slash(mini::string p) {
        while (p.size() > 1 && (p.back() == '/' || p.back() == '\\') && !(p.size() == 3 && p[1] == ':')) p.pop_back();
        return p;
    }
    static mini::string parent_of(const mini::string &p) {
        size_t a = p.rfind('/'), b = p.rfind('\\'), n = mini::string::npos;
        size_t cut = a == n ? b : b == n ? a : mini::max(a, b);
        return cut == n ? mini::string() : p.substr(0, cut);
    }
    static mini::string base_of(const mini::string &p) {
        mini::string q = trim_slash(p), up = parent_of(q);
        return up.empty() ? q : q.substr(up.size() + 1);
    }
    mini::string inbox_dir() const {   // next to the mods folder (tools/mods.py: md.parent / mods-inbox)
        mini::string up = parent_of(trim_slash(mods_dir()));
        return join(up.empty() ? dir : up, "mods-inbox");
    }
    bool mods_on() const { return setting_text("TOTK_MODS") != "0"; }
    static mini::string fmt_bytes(uint64_t b) {
        char t[32];
        if (b >= 1000ull * 1000 * 1000) SDL_snprintf(t, sizeof t, "%.1f GB", b / 1e9);
        else if (b >= 1000ull * 1000) SDL_snprintf(t, sizeof t, "%.1f MB", b / 1e6);
        else if (b >= 1000) SDL_snprintf(t, sizeof t, "%.0f KB", b / 1e3);
        else SDL_snprintf(t, sizeof t, "%u bytes", (unsigned)b);
        return t;
    }
    static mini::string num(long long v) { char t[32]; SDL_snprintf(t, sizeof t, "%lld", v); return t; }
    static mini::string plural(long long v, const char *one, const char *many) { return num(v) + " " + (v == 1 ? one : many); }
    mini::string mod_title(int k) const {
        const ModfsMod &m = mods.mods[k];
        return m.title[0] ? mini::string(m.title) : mini::string(m.name);
    }

    static const char *canon_cb(const char *path, void *ud) {   // the game has this RomFS file (any case on Windows)
        Canon *c = (Canon *)ud;
        if (c->indexed) {
            c->buf = Canon::lower(path);
            signed char *v = c->slot(c->buf.c_str());
            if (!*v) *v = -1;
            return *v > 0 ? path : nullptr;
        }
        signed char *v = c->slot(path);
        if (!*v) {
            c->buf = join(c->root, path);
            SDL_PathInfo pi;
            *v = SDL_GetPathInfo(c->buf.c_str(), &pi) && pi.type == SDL_PATHTYPE_FILE ? 1 : -1;
        }
        return *v > 0 ? path : nullptr;
    }
    static SDL_Time mtime(const mini::string &p) {
        SDL_PathInfo pi;
        return SDL_GetPathInfo(p.c_str(), &pi) ? pi.modify_time : 0;
    }
    void inbox_scan() {
        inbox.clear();
        mini::string ib = inbox_dir();
        mini::vector<mini::string> names;   // the top level only (SDL_GlobDirectory would walk every file of a mod folder)
        SDL_EnumerateDirectory(ib.c_str(), [](void *ud, const char *, const char *f) {
            ((mini::vector<mini::string> *)ud)->push_back(f);
            return SDL_ENUM_CONTINUE;
        }, &names);
        for (auto &nm : names) {
            mini::string low;
            for (char ch : nm) low += (char)SDL_tolower((unsigned char)ch);
            if (low == "installed" || low == "failed" || low == "readme.txt" || low.empty() || low[0] == '.') continue;
            SDL_PathInfo pi;
            bool folder = SDL_GetPathInfo(join(ib, nm).c_str(), &pi) && pi.type == SDL_PATHTYPE_DIRECTORY;
            auto ends = [&](const char *e) { size_t l = SDL_strlen(e); return low.size() > l && low.compare(low.size() - l, l, e) == 0; };
            if (folder || ends(".zip") || ends(".7z") || ends(".rar") || ends(".tar") || ends(".tgz") || ends(".tar.gz") ||
                ends(".zs") || ends(".ips") || ends(".pchtxt"))   // a bare game file (placed by name); code patches: told why not
                inbox.push_back(nm);
        }
        for (size_t a = 1; a < inbox.size(); a++)
            for (size_t b = a; b > 0 && inbox[b] < inbox[b - 1]; b--) mini::swap(inbox[b], inbox[b - 1]);
    }
    void mods_reload() {
        mini::string keep = mods_focus;
        if (keep.empty() && tab == TAB_MODS && focus[tab] < (int)items.size() && items[focus[tab]].kind == Item::Mod)
            keep = mods.mods[items[focus[tab]].idx].name;
        mini::string g = grab >= 0 && grab < mods.nmods ? mini::string(mods.mods[grab].name) : mini::string();
        modfs_free(&mods);
        mini::string md = mods_dir(), rp = romfs_path();
        SDL_PathInfo pi;
        bool have = !rp.empty() && SDL_GetPathInfo(rp.c_str(), &pi) && pi.type == SDL_PATHTYPE_DIRECTORY;
        if (canon.root != rp) {
            canon.clear(), canon.root = rp;
            mini::string idx = join(rp, "runtime-index.bin");
            if (have && SDL_GetPathInfo(idx.c_str(), &pi) && pi.type == SDL_PATHTYPE_FILE) canon.load_index(idx);
        }
        modfs_load(&mods, md.c_str(), 1, have ? canon_cb : nullptr, nullptr, &canon);
        mods_placed.clear();
        for (int k = 0; k < mods.nmods; k++) {
            mini::string t;
            if (SDL_GetPathInfo(join(join(md, mods.mods[k].name), "uit-placed.txt").c_str(), &pi)) t = read_text(join(join(md, mods.mods[k].name), "uit-placed.txt"));
            mods_placed.push_back(t);
        }
        mods_loaded = md;
        mods_mtime = mtime(md);
        mods_check = now;
        inbox_scan();
        grab = -1;
        for (int k = 0; k < mods.nmods; k++) if (!g.empty() && g == mods.mods[k].name) grab = k;
        if (tab == TAB_MODS) {
            build_items();
            if (!keep.empty())
                for (int i = 0; i < (int)items.size(); i++)
                    if (items[i].kind == Item::Mod && keep == mods.mods[items[i].idx].name) { focus[tab] = i; follow = true; }
            clamp_focus();
        }
        mods_focus.clear();
    }
    bool mods_dirty = false;
    void mods_poll() {
        if (mods_dirty && task_kind == T_None) {   // Load mods or the folder changed: list again, focus stays on that setting
            mods_dirty = false;
            const Item *it = focus[tab] < (int)items.size() ? &items[focus[tab]] : nullptr;
            int row = it && it->kind == Item::Row ? it->idx : -1;
            mods_reload();
            if (row >= 0 && tab == TAB_MODS)
                for (int k = 0; k < (int)items.size(); k++)
                    if (items[k].kind == Item::Row && items[k].idx == row) focus[tab] = k;
        }   // Explorer added or removed a mod, the folder setting changed, or a download landed in the inbox
        if (running || task_kind != T_None || (tab != TAB_MODS && tab != TAB_PLAY) || now - mods_check < 1.5) return;
        mods_check = now;
        mini::string md = mods_dir();
        size_t nin = inbox.size();
        if (md != mods_loaded || mtime(md) != mods_mtime) { mods_reload(); return; }
        inbox_scan();
        if (inbox.size() != nin && tab == TAB_MODS) build_items(), clamp_focus();
    }
    bool mods_write(const mini::vector<int> &order, const mini::vector<int> &on) {   // order[0] = highest
        mini::vector<const char *> names;
        mini::vector<int> en;
        for (size_t k = 0; k < order.size(); k++) names.push_back(mods.mods[order[k]].name), en.push_back(on[k]);
        if (modfs_write_order(mods_loaded.c_str(), names.data(), en.data(), (int)names.size()) != 0) {
            message("Could not save the load order", "load-order.txt could not be written in\n" + mods_loaded +
                    "\n\nIs the folder read-only, or is the file open in another program?");
            return false;
        }
        mods_reload();
        return true;
    }
    mini::vector<int> mods_ids() const { mini::vector<int> v; for (int k = 0; k < mods.nmods; k++) v.push_back(k); return v; }
    mini::vector<int> mods_states() const { mini::vector<int> v; for (int k = 0; k < mods.nmods; k++) v.push_back(mods.mods[k].enabled); return v; }
    void mod_set(int k, bool on) {
        if (k < 0 || k >= mods.nmods || (bool)mods.mods[k].enabled == on) return;
        auto en = mods_states();
        en[k] = on;
        mini::string t = mod_title(k);
        if (mods_write(mods_ids(), en)) flash(t + (on ? ": on" : ": off") + " (applies on the next Play)");
    }
    void mods_all(bool on) {
        if (!mods.nmods) return;
        mini::vector<int> en;
        for (int k = 0; k < mods.nmods; k++) en.push_back(on);
        if (mods_write(mods_ids(), en)) {
            forget_mods_before();
            flash(on ? "All mods on" : "All mods off: the game starts with its own files");
        }
    }
    // [UIT 0062] Disable Mods remembers which were on (mods/.uit-mods-before-disable.txt, a name per line; a dot name, so
    // no mod and no game file); Enable Mods turns those back on (plus any turned on since) and forgets the list.
    mini::string mods_before_path() const { return join(trim_slash(mods_dir()), ".uit-mods-before-disable.txt"); }
    bool mods_any_on() const { for (int k = 0; k < mods.nmods; k++) if (mods.mods[k].enabled) return true; return false; }
    // [UIT 0175] the built-in cheats (Game > Cheats) count as mods here: Disable Mods turns them off too and remembers
    // them in the same file as "::TOTK_CHEAT_..." lines (no folder name has a colon), Enable Mods turns them back on.
    static constexpr const char *kCheatKeys[] = {"TOTK_CHEAT_DURABILITY", "TOTK_CHEAT_AMIIBO", "TOTK_CHEAT_STAMINA", "TOTK_CHEAT_ARROWS"};   // [UIT 0344, 0345]
    mini::vector<mini::string> cheats_on_now() const {
        mini::vector<mini::string> o;
        for (const char *k : kCheatKeys) { int i = idx(k); if (i >= 0 && val(i).b) o.push_back(k); }
        return o;
    }
    mini::vector<mini::string> before_lines() const {
        mini::vector<mini::string> o;
        SDL_PathInfo pi;
        if (!SDL_GetPathInfo(mods_before_path().c_str(), &pi)) return o;
        for (auto &l : split(read_text(mods_before_path()), '\n')) {
            mini::string q = l;
            while (!q.empty() && (q.back() == '\r' || q.back() == ' ')) q.pop_back();
            if (!q.empty()) o.push_back(q);
        }
        return o;
    }
    mini::vector<mini::string> cheats_before_disable() const {   // the remembered cheats this launcher knows
        mini::vector<mini::string> o;
        for (auto &q : before_lines())
            for (const char *k : kCheatKeys) if (q == mini::string("::") + k && idx(k) >= 0) { o.push_back(k); break; }
        return o;
    }
    void forget_mods_before() {   // the mods' part goes; remembered cheats stay until Enable Mods
        mini::string keep;
        for (auto &c : cheats_before_disable()) keep += "::" + c + "\n";
        if (keep.empty()) SDL_RemovePath(mods_before_path().c_str());
        else SDL_SaveFile(mods_before_path().c_str(), keep.data(), keep.size());
    }
    void cheat_set(const mini::string &k, bool on) {
        int i = idx(k.c_str());
        if (i < 0) return;
        Value v = val(i);
        v.kind = Value::Normal;
        v.b = on;
        set(i, v);
    }
    mini::vector<mini::string> mods_before_disable() const {   // the remembered names that are still installed
        mini::vector<mini::string> o;
        for (auto &q : before_lines()) {
            if ((q.size() > 1 && q[0] == ':' && q[1] == ':')) continue;   // a cheat
            for (int k = 0; k < mods.nmods; k++) if (q == mods.mods[k].name) { o.push_back(q); break; }
        }
        return o;
    }
    bool mods_before_has_mods() const {   // the file names any mod (installed or not): else Enable Mods leaves mods alone
        for (auto &q : before_lines()) if (!(q.size() > 1 && q[0] == ':' && q[1] == ':')) return true;
        return false;
    }
    void mods_vanilla() {
        auto cheats = cheats_on_now();
        if (!mods.nmods && cheats.empty() && cheats_before_disable().empty()) return;
        mini::vector<int> en;
        if (mods_any_on() || !cheats.empty()) {
            mini::string keep;
            int n = 0;
            for (int k = 0; k < mods.nmods; k++) if (mods.mods[k].enabled) keep += mini::string(mods.mods[k].name) + "\n", n++;
            auto remember = cheats;   // plus cheats remembered by an earlier Disable and not turned back on since (All on keeps them)
            for (auto &c : cheats_before_disable()) {
                bool have = false;
                for (auto &q : remember) have = have || q == c;
                if (!have) remember.push_back(c);
            }
            for (auto &c : remember) keep += "::" + c + "\n";
            SDL_CreateDirectory(trim_slash(mods_dir()).c_str());   // no mods yet: the folder may not exist
            if (!SDL_SaveFile(mods_before_path().c_str(), keep.data(), keep.size())) {
                message("Could not disable mods", "Could not write " + mods_before_path() + " (" + SDL_GetError() + "), so the mods that are on "
                        "could not be remembered. Nothing was changed. Is the mods folder read-only?");
                return;
            }
            for (auto &c : cheats) cheat_set(c, false);
            for (int k = 0; k < mods.nmods; k++) en.push_back(0);
            mini::string what = n && !remember.empty() ? plural(n, "mod", "mods") + " and " + plural((long long)remember.size(), "cheat", "cheats")
                              : n ? plural(n, "mod", "mods") : plural((long long)remember.size(), "cheat", "cheats");
            if (!n || mods_write(mods_ids(), en))
                flash("Mods disabled: Play starts the unmodified game. Enable Mods turns the " + what + " back on");
            if (!n) build_items();
            return;
        }
        auto back = mods_before_disable();
        auto cback = cheats_before_disable();
        bool touch_mods = mods.nmods && (mods_before_has_mods() || cback.empty());   // only cheats remembered: the mods stay as they are
        for (int k = 0; k < mods.nmods; k++) {
            bool b = back.empty();
            for (auto &q : back) b = b || q == mods.mods[k].name;
            en.push_back(b);
        }
        for (auto &c : cback) cheat_set(c, true);
        if (!touch_mods || mods_write(mods_ids(), en)) {
            SDL_RemovePath(mods_before_path().c_str());
            mini::string m = !touch_mods ? mini::string("Mods enabled: ")
                           : back.empty() ? "Mods enabled: all " + num(mods.nmods) + " on"
                                          : "Mods enabled: the " + plural((long long)back.size(), "mod", "mods") + " from before are on again";
            if (!cback.empty()) m += (touch_mods ? mini::string(", ") : mini::string()) + "the " + plural((long long)cback.size(), "cheat", "cheats") + " from before " + (cback.size() == 1 ? "is" : "are") + " on again";
            flash(m);
            build_items();
        }
    }
    void mod_move(int k, int dir_) {   // dir_ -1: up (higher priority), +1: down
        int to = k + dir_;
        if (k < 0 || k >= mods.nmods) return;
        if (to < 0 || to >= mods.nmods) { flash(dir_ < 0 ? "Already the top mod: it wins every file it shares" : "Already the bottom mod"); return; }
        auto ord = mods_ids(), en = mods_states();
        mini::swap(ord[k], ord[to]);
        mini::swap(en[k], en[to]);
        mods_focus = mods.mods[k].name;
        mods_write(ord, en);      // mods_reload keeps grab on the same mod by name (grab still holds its old place here)
    }
    void mods_install(const mini::vector<mini::string> &src, const char *op) {
        if (tab != TAB_MODS) switch_tab(TAB_MODS);
        mini::string args = mini::string(" ") + op;
        for (const mini::string &s : src) args += " " + quote(trim_slash(s));   // "C:\x\" would escape the quote
        mini::string logs = join(dir, "logs");
        SDL_CreateDirectory(logs.c_str());
        task_prog = join(logs, "mods-progress.json");
        SDL_RemovePath(task_prog.c_str());
        mini::string md = trim_slash(mods_dir());
        args += " --mods " + quote(md) + " --progress " + quote(task_prog);
        if (mini::string(op) == "inbox") args += " --inbox " + quote(trim_slash(inbox_dir()));
        mods_op = op;
        mini::string what = src.size() == 1 ? base_of(src[0]) : plural((long long)src.size(), "item", "items");
        run_helper(T_Mods, "mods.py", args, mods_op == "inbox" ? "Installing from the inbox" : "Installing " + what,
                   "Reading " + what + "...");
    }
    void mod_remove(int k) {
        if (k < 0 || k >= mods.nmods) return;
        mini::string name = mods.mods[k].name, md = trim_slash(mods_loaded);
        confirm("Uninstall " + mod_title(k) + "?",
                "Deletes its " + plural(mods.mods[k].nfiles, "file", "files") + " (" + fmt_bytes(mods.mods[k].bytes) + ") and the folder\n" +
                join(md, name) + "\n\nThe game's own files are not touched. To keep it but not load it, turn it off instead.",
                "Uninstall", true, [this, name, md] {
                    mods_op = "remove";
                    mods_focus.clear();
                    run_helper(T_Mods, "mods.py", " remove " + quote(name) + " --mods " + quote(md), "Uninstalling " + name, "Removing files...");
                });
    }
    void mods_done(int code) {
        auto ls = split(read_text(task_out), '\n');
        mini::vector<mini::pair<mini::string, mini::string>> rows;
        mini::string notes, errors, first, removed, undone;
        mini::vector<mini::string> placed;
        int ok = 0, bad = 0, nundone = 0;
        bool empty = false;
        for (auto &l : ls) {
            auto f = split(l, '\t');
            if (f[0] == "placed" && f.size() >= 3) placed.push_back(f[2]);
            else if (f[0] == "undone" && f.size() >= 3) { nundone++; undone += (undone.empty() ? "" : ", ") + f[1] + (f[2] == "restored" ? " (the copy from before)" : ""); }
            else if (f[0] == "installed" && f.size() >= 6) {
                ok++;
                if (first.empty()) first = f[1];
                mini::string st = f[4] == "updated" ? "updated, same place" : f[4] == "off" ? "off (a variant)" : ok == 1 ? "on, top of the list" : "on";
                rows.push_back({f[1], st + " \xC2\xB7 " + plural(atoll(f[2].c_str()), "file", "files") + " \xC2\xB7 " + fmt_bytes((uint64_t)atoll(f[3].c_str()))});
                mini::string nt;   // the "placed by file name" part is said in plain words below
                for (auto &part : split(f[5], ';')) {
                    mini::string q = part;
                    while (!q.empty() && q[0] == ' ') q = q.substr(1);
                    if (!q.empty() && q.compare(0, 19, "placed by file name") != 0) nt += (nt.empty() ? "" : "; ") + q;
                }
                if (!nt.empty()) notes += f[1] + ": " + nt + "\n";
            } else if (f[0] == "error" && f.size() >= 3) {
                bad++;
                rows.push_back({f[1], "not installed"});
                errors += f[2] + "\n\n";
            } else if (f[0] == "removed" && f.size() >= 2) removed = f[1];
            else if (f[0] == "empty") empty = true;
        }
        mods_reload();
        inbox_scan();
        if (mods_op == "undo") {
            if (nundone && errors.empty()) {
                flash("Undone: " + undone + (nundone == 1 && undone.find("(the copy") == mini::string::npos ? " removed" : "") + ", load order as before");
                return;
            }
            message(nundone ? "Undone, with a problem" : "Could not undo", errors.empty() ? last_lines(task_out, 6) : errors);
            return;
        }
        if (mods_op == "remove") {
            if (!removed.empty()) { flash(removed + " uninstalled"); return; }
            message("Could not uninstall", errors.empty() ? last_lines(task_out, 6) : errors);
            return;
        }
        if (empty) { message("The inbox is empty", "Put mod downloads (.zip, .7z, .rar or a mod folder) in\n" + inbox_dir() + "\n\nthen press Install from mods-inbox again."); return; }
        if (!ok && !bad) {
            message("Could not install", (code ? last_lines(task_out, 8) : mini::string("tools/mods.py gave no result.")) +
                    "\n\nThe Python command is in Advanced > Updates and reports.");
            mini::string l = task_out;
            modal.alt = "Open log";
            modal.on_alt = [l] { open_path(l); };
            return;
        }
        if (!first.empty()) {
            for (int i = 0; i < (int)items.size(); i++)
                if (items[i].kind == Item::Mod && first == mods.mods[items[i].idx].name) focus[tab] = i, follow = true;
        }
        mini::string title = !bad ? (ok == 1 ? "Mod installed" : plural(ok, "mod", "mods") + " installed")
                           : !ok ? "Could not install" : num(ok) + " installed, " + num(bad) + " not";
        mini::string body;
        if (ok) body = ok == 1 ? first + " is on top of the load order, so it wins any file it shares with another mod. It applies the next time you press Play."
                               : "The new mods are on top of the load order. They apply the next time you press Play.";
        if (!placed.empty()) {           // placed by file name: say where, in plain words, and offer Undo right here
            if (!bad && ok == 1) title = placed.size() == 1 ? "Game file placed" : "Game files placed";
            body = placed_text(placed) + "\n\nThe game has exactly one file with " + (placed.size() == 1 ? "that name" : "each name") +
                   ", so that is the one it replaces. " + (ok == 1 ? "Installed as the mod " + first + ", on top of the load order. " : mini::string()) +
                   "It applies the next time you press Play.\n\nNot what you meant? Undo removes " + (ok == 1 ? mini::string("it") : mini::string("this install")) +
                   " and puts the mods folder and load order back as they were.";
        }
        if (!notes.empty()) body += "\n\n" + notes;
        while (!errors.empty() && errors.back() == '\n') errors.pop_back();
        if (!errors.empty()) body += (body.empty() ? "" : "\n\n") + errors;
        if (ok && !mods_on()) body += "\n\nLoad mods is off (Mod settings, below the list): turn it on to load them.";
        if (mods_op == "inbox") body += "\n\nInstalled items moved to mods-inbox\\installed, the others to mods-inbox\\failed with a note.";
        while (!body.empty() && (body.back() == '\n' || body.back() == ' ')) body.pop_back();
        message(title, body);
        modal.rows = rows;
        if (!placed.empty()) {
            mini::string md = trim_slash(mods_loaded);
            modal.alt = "Undo";
            modal.on_alt = [this, md] {
                mods_op = "undo";
                mods_focus.clear();
                run_helper(T_Mods, "mods.py", " undo --mods " + quote(md), "Undoing the install", "Putting the mods folder back...");
            };
        }
    }
    void drop_done() {   // files dropped on the window: a source package installs an update, anything else is a mod
        mini::vector<mini::string> f = dropped;
        dropped.clear();
        if (f.empty()) return;
        if (running) { flash("Close the game first"); return; }
        if (task_kind != T_None || modal.kind != Modal::None) { flash("Finish the open dialog first, then drop it again"); return; }
        mini::string b = base_of(f[0]), low;
        for (char ch : b) low += (char)SDL_tolower((unsigned char)ch);
        if (f.size() == 1 && low.compare(0, 18, "ultrainstincttears") == 0 && low.size() > 4 && low.compare(low.size() - 4, 4, ".zip") == 0 &&
            low.find("-src") != mini::string::npos) { update_start(f[0]); return; }
        mods_install(f, "install");
    }
    void pick_mod(bool folder) {
        static const SDL_DialogFileFilter f[] = {{"Mods (zip, 7z, rar, or a game file)", "zip;7z;rar;tar;tgz;gz;zs"},
                                                     {"All files", "*"}};
        if (!pick_mu) pick_mu = SDL_CreateMutex();
        pick_kind = P_Mod;
        if (folder) SDL_ShowOpenFolderDialog(picked_cb, this, win, nullptr, false);
        else SDL_ShowOpenFileDialog(picked_cb, this, win, f, 2, nullptr, true);
    }

    void play(bool log) {
        if (running) { flash("Ultra Instinct Tears is already running"); return; }
        if (task_kind == T_Install) { flash("An update is being installed: play when it is done"); return; }
        if (task_kind == T_Download) { flash("An update is being downloaded: play when it is done"); return; }   // [UIT 0281]
        if (task_kind == T_Scan) { flash("Looking for an update: play when it is done"); return; }
        if (task_kind == T_Mods) { flash("A mod is being installed: play when it is done"); return; }
        save_now();
        exe_found = file_exists(exe);
        if (!exe_found) {
            message("UltraInstinctTears.exe not found", "The launcher looks for UltraInstinctTears.exe in its own folder:\n" + dir +
                    "\n\nRun tools/setup.py to build the game, then start the launcher from the game folder.");
            return;
        }
        mini::string logp, err;
        if (log) {
            mini::string logs = join(dir, "logs");
            SDL_CreateDirectory(logs.c_str());
            logp = join(logs, "ultrainstincttears-" + timestamp() + ".log");
        }
        ucheck_stop("Play was pressed");   // [UIT 0320] before the game starts: the network stays quiet while it runs
        mini::string backed = backup_saves();
        if (!start_game(exe, dir, logp, &game, &err)) { message("Could not start the game", err); return; }
        running = true;
        game_started = now;
        last_log = logp;
        last_run = (log ? "Running, logging to " + logp : mini::string("Running")) + backed;
        SDL_MinimizeWindow(win);
    }

    // [UIT 0347] Game > Saves > Save backups (TOTK_SAVE_BACKUPS, 10 by default, 0 = off): the save folder zipped into
    // backups\ in the game folder before the game starts, the newest N kept. Never more than a few seconds (it gives up
    // and the game starts anyway); the saves themselves are only read. Returns a note for the status line.
    mini::string backup_saves() {
        int i = idx("TOTK_SAVE_BACKUPS");
        int keep = i >= 0 ? (int)val(i).num : 10;
        if (keep <= 0) return "";
        int si = idx("saves");
        mini::optional<mini::string> r = si >= 0 ? raw(si) : mini::optional<mini::string>();
        mini::string saves = r && !r->empty() && *r != "-" ? *r : mini::string("userdata");   // the game's default
        bool abs = saves[0] == '/' || saves[0] == '\\' || (saves.size() > 1 && saves[1] == ':');
        if (!abs) saves = join(dir, saves);
        Uint64 t0 = SDL_GetTicks();
        SaveBackup b = save_backup(saves, join(dir, "backups"), keep, 5.0);
        unsigned ms = (unsigned)(SDL_GetTicks() - t0);
        switch (b.result) {
        case SaveBackup::Made:
            SDL_Log("save backup: %s in %u ms (%d older removed)", b.file.c_str(), ms, b.removed);
            return " \xC2\xB7 saves backed up";
        case SaveBackup::Unchanged:
            SDL_Log("save backup: saves unchanged since %s", b.file.c_str());
            return " \xC2\xB7 saves unchanged since the last backup";
        case SaveBackup::NoSaves: SDL_Log("save backup: no saves in %s yet", saves.c_str()); return "";
        case SaveBackup::Skipped:
            SDL_Log("save backup: skipped after %u ms: %s", ms, b.why.c_str());
            flash("Saves not backed up: " + b.why);
            return " \xC2\xB7 saves not backed up";
        default: return "";
        }
    }

    void poll_game() {
        if (!running || fake_running || game.running()) return;
        running = false;
        int code = game.exit_code;
        int end = uit_game_end(code, now - game_started);
        if (end == UIT_END_RESTART) {
            // [UIT 0294] Overlay > Restart game: the same way again (Play, or Play with log: a new log), with the settings
            // the game saved (ultrainstincttears.cfg read again first, so the launcher does not write its older copy)
            bool log = !last_log.empty();
            reload_if_changed();
            play(log);
            if (running) { flash("Restarting the game"); return; }
            last_run = "Restart from the in-game overlay: the game could not be started again";   // play said why
        }
        SDL_RestoreWindow(win);
        SDL_RaiseWindow(win);
        if (end == UIT_END_RESTART) {   // not started again: play() has said why
        } else if (end == UIT_END_RESTART_SKIPPED) {
            last_run = "Last run ended normally";
            flash("The game asked to restart right after starting: not restarted");
        } else if (code == 0) {
            last_run = "Last run ended normally";
            flash("The game closed normally");
        } else {
            char hex[32];
            snprintf(hex, sizeof hex, "0x%08X", (unsigned)code);
            last_run = mini::string("Last run ended with code ") + hex;
            mini::string body = mini::string("UltraInstinctTears.exe exited with code ") + hex + ".";
            if (!last_log.empty()) body += "\n\nThe log is in\n" + last_log;
            else body += "\n\nUse Play with log next time to keep a log file for a bug report.";
            message("The game closed with an error", body);
            if (!last_log.empty()) {
                mini::string lp = last_log;
                modal.alt = "Open log";
                modal.on_alt = [lp] { open_path(lp); };
            }
        }
        if (!last_log.empty()) last_run += " \xC2\xB7 log saved";
    }

    void message(const mini::string &title, const mini::string &body) {
        modal = Modal();
        modal.kind = Modal::Message;
        modal.title = title, modal.body = body, modal.ok = "OK";
        modal.open_t = (float)now;
    }

    // [UIT 0270] the first open (TOTK_UI_CONFIRM not in the cfg yet): which button confirms. Mouse: click a picture or
    // a button; keyboard: Left / Right and Enter; controller: press the button that should confirm (the bottom one picks
    // Standard, the right one Follow the game), or move with the d-pad. Esc (or closing the window) asks again next time.
    bool confirm_unasked() const {
        int i = idx("TOTK_UI_CONFIRM");
        return i >= 0 && S[i].ask && !raw(i);
    }
    void ask_confirm_layout() {
        int i = idx("TOTK_UI_CONFIRM");
        if (i < 0) return;
        modal = Modal();
        modal.kind = Modal::Confirm;
        modal.ask_confirm = true;
        modal.title = "Which button should confirm?";
        modal.alt = "Standard", modal.ok = "Follow the game";      // buttons left to right: alt, ok
        modal.on_alt = [this, i] { pick_confirm(i, "standard"); };
        modal.on_ok = [this, i] { pick_confirm(i, "game"); };
        modal.focus = 2;
        modal.open_t = (float)now;
    }
    void pick_confirm(int i, const char *v) {
        Value nv;
        for (int k = 0; k < (int)S[i].choices.size(); k++) if (S[i].choices[k].value == v) nv.index = k;
        set(i, nv);
        save_at = 0;
        flash(mini::string(confirm_game() ? "Follow the game" : "Standard") + ": " + ok_name() + " confirms, " + back_name() +
              " goes back. Change it in Game > Controller > Confirm button (menus).");
    }
    mini::string ask_confirm_body() const {
        return "Pick the controller button that confirms in this launcher and in the in-game overlay. Standard: the bottom "
               "button (" + face_name("south") + ") confirms and the right one (" + face_name("east") + ") goes back. Follow the game: "
               "the right button (" + face_name("east") + ") confirms and the bottom one goes back, as in the game and on the Switch. "
               "The game's own buttons do not change.\n\nOn the controller, press the button you want to confirm with. "
               "Change it any time: Game > Controller > Confirm button (menus).";
    }

    void confirm(const mini::string &title, const mini::string &body, const mini::string &ok, bool danger,
                 mini::function<void()> fn) {
        modal = Modal();
        modal.kind = Modal::Confirm;
        modal.title = title, modal.body = body, modal.ok = ok, modal.cancel = "Cancel";
        modal.danger = danger;
        modal.on_ok = mini::move(fn);
        modal.focus = danger ? 1 : 0;      // button ids: 0 ok, 1 cancel; destructive ones start on Cancel
        modal.open_t = (float)now;
    }

    void edit(int i) {
        modal = Modal();
        modal.kind = Modal::Edit;
        modal.setting = i;
        modal.title = S[i].name;
        Value v = val(i);
        if (v.kind == Value::Custom || S[i].type == Type::Text) modal.buf = v.text;
        else if (v.kind == Value::Normal && (S[i].type == Type::Int || S[i].type == Type::Float)) modal.buf = fmt_num(S[i], v.num);
        modal.open_t = (float)now;
        SDL_StartTextInput(win);
    }

    void edit_done() {
        int i = modal.setting;
        const Setting &s = S[i];
        mini::string t = modal.buf;
        while (!t.empty() && t.back() == ' ') t.pop_back();
        while (!t.empty() && t.front() == ' ') t.erase(0, 1);
        if (s.type == Type::Int || s.type == Type::Float) {
            if (t.empty()) { set(i, default_value(s)); close_modal(); return; }
            char *end = nullptr;
            double v = strtod(t.c_str(), &end);
            if (end == t.c_str() || *end || v < s.min - 1e-9 || v > s.max + 1e-9 || (s.type == Type::Int && v != mini::floor(v))) {
                modal.error = "Enter a " + mini::string(s.type == Type::Int ? "whole number" : "number") + " from " +
                              fmt_num(s, s.min) + " to " + fmt_num(s, s.max) + ", or leave it empty for the default.";
                return;
            }
            Value nv;
            nv.num = v;
            set(i, nv);
        } else {
            Value nv;
            nv.text = t;
            set(i, nv);
        }
        close_modal();
    }

    void close_modal(bool force = false) {
        if (modal.busy && !force) return;   // a helper is still running: its dialog stays until it is done
        if (modal.kind == Modal::Edit) SDL_StopTextInput(win);
        if (modal.kind == Modal::Bind) trig_down[0] = trig_down[1] = true;   // a held trigger does not page on release
        modal = Modal();
    }

    void reset_all() {
        mini::vector<mini::pair<mini::string, mini::string>> rows;
        for (int i = 0; i < (int)S.size(); i++)
            if (S[i].key.rfind("TOTK_", 0) == 0 && modified(i))
                rows.push_back({S[i].name, shown(i) + "  \xE2\x86\x92  " + display(S[i], default_value(S[i]))});
        if (rows.empty()) { flash("Every setting is already at its default"); return; }
        confirm("Reset all settings?", mini::to_string(rows.size()) + (rows.size() == 1 ? " setting goes" : " settings go") +
                " back to the game's default. Data folders are kept.", "Reset all", true, [this] {
            for (int i = 0; i < (int)S.size(); i++)
                if (S[i].key.rfind("TOTK_", 0) == 0 && modified(i)) set(i, default_value(S[i]));
            flash("All settings reset to defaults");
        });
        modal.rows = rows;
    }

    void apply_recommended() {
        mini::vector<int> change;
        mini::vector<mini::pair<mini::string, mini::string>> rows;
        for (int i = 0; i < (int)S.size(); i++) {
            if (!S[i].hasRecommended) continue;
            if (S[i].key == "TOTK_DLSS_MODE" && !want_dlss()) continue;
            Value r = recommended(i);
            if (same(S[i], val(i), r)) continue;
            change.push_back(i);
            rows.push_back({S[i].name, shown(i) + "  \xE2\x86\x92  " + display(S[i], r)});
        }
        if (change.empty()) { flash("Already using the recommended settings"); return; }
        mini::string why = gpu.name.empty() ? "" : gpu.name + ". ";
        why += want_dlss() ? "DLSS Quality (DLSS files found)" : dll ? "FSR 1 (DLSS needs an RTX card)" : "FSR 1 (no DLSS files in the game folder)";
        why += ", 60 FPS, shader warm-up (the shaders you have seen), texture streaming and controller vibration on, fullscreen, and the troubleshooting checks off. Nothing else changes.";
        confirm("Use the recommended settings?", why, "Apply", false, [this, change] {
            for (int i : change) set(i, recommended(i));
            flash("Recommended settings applied");
        });
        modal.rows = rows;
    }

    void press_button(int b) {
        switch (b) {
        case B_Play: play(false); break;
        case B_PlayLog: play(true); break;
        case B_Recommend: apply_recommended(); break;
        case B_ResetAll: reset_all(); break;
        case B_OpenFolder: open_path(dir); break;
        case B_OpenCfg:
            if (!cfg.exists) { save_at = 0; save_now(); }
            open_path(cfg.path);
            break;
        case B_OpenLogs: {
            mini::string logs = join(dir, "logs");
            SDL_CreateDirectory(logs.c_str());
            open_path(logs);
            break;
        }
        case B_Update: update_start(); break;
        case B_Report: report_start(); break;
        case B_Speed: pgo_start(); break;
        case B_Backups: {
            mini::string b = join(dir, "backups");
            SDL_CreateDirectory(b.c_str());
            open_path(b);
            break;
        }
        case B_ModFile: if (!running) pick_mod(false); break;
        case B_ModFolder: if (!running) pick_mod(true); break;
        case B_ModInbox: mods_install({}, "inbox"); break;
        case B_ModAllOn: mods_all(true); break;
        case B_ModAllOff: mods_vanilla(); break;
        case B_ModOpen: {
            mini::string md = mods_dir();
            SDL_CreateDirectory(md.c_str());
            open_path(md);
            break;
        }
        case B_Quit: confirm("Quit the launcher?", "Your settings are already saved.", "Quit", false, [this] { quit = true; }); break;
        }
    }

    void toggle_group(const mini::string &k) {
        if (expanded.count(k)) expanded.erase(k); else expanded.insert(k);
        build_items();
    }

    void accept_item() {
        if (items.empty()) return;
        Item &it = items[focus[tab]];
        if (it.kind == Item::Button && it.idx == B_ResetGroup) { reset_group(it.gid); return; }
        if (it.kind == Item::Button && it.idx == B_SearchIn) { search_go(atoi(it.gid.c_str())); return; }
        if (it.kind == Item::Button) { press_button(it.idx); return; }
        if (it.kind == Item::Group) { toggle_group(it.gid); return; }
        if (it.kind == Item::Mod) { mod_set(it.idx, !mods.mods[it.idx].enabled); return; }
        if (it.kind != Item::Row) return;
        int i = it.idx;
        const Setting &s = S[i];
        if (locked(i)) return;
        Value v = val(i);
        switch (s.type) {
        case Type::Bool: v.b = !v.b; set(i, v); break;
        case Type::Choice: {
            if (s.key == "TOTK_DLSS_MODE" && !dll) break;
            if (v.kind == Value::Custom) { set(i, default_value(s)); break; }
            int n = (int)s.choices.size(), c = v.index;
            do c = (c + 1) % n; while (!choice_ok(i, c));
            v.index = c;
            set(i, v);
            break;
        }
        case Type::Pad: case Type::Key: open_bind(i); break;
        default: edit(i); break;
        }
    }

    void reset_item() {
        if (items.empty()) return;
        Item &it = items[focus[tab]];
        if ((it.kind == Item::Group || (it.kind == Item::Button && it.idx == B_ResetGroup))) { reset_group(it.gid); return; }
        if (it.kind == Item::Mod) { mod_remove(it.idx); return; }   // X / Del on a mod: uninstall (after a confirmation)
        if (it.kind != Item::Row || !modified(it.idx)) return;
        set(it.idx, default_value(S[it.idx]));
        flash(S[it.idx].name + " reset to default");
    }

    // Every setting of one group back to its default, after a confirmation listing them.
    void reset_group(const mini::string &g) {
        mini::vector<int> members, change;
        int changed = 0;
        group_count(g, &changed, &members);
        mini::vector<mini::pair<mini::string, mini::string>> rows;
        for (int i : members)
            if (modified(i)) {
                change.push_back(i);
                rows.push_back({S[i].name, shown(i) + "  \xE2\x86\x92  " + display(S[i], default_value(S[i]))});
            }
        mini::string name = group_label(g);
        if (change.empty()) { flash(name + ": already at the defaults"); return; }
        confirm("Reset " + lc_first(name) + "?", mini::to_string(change.size()) + (change.size() == 1 ? " setting goes" : " settings go") +
                " back to the default. Nothing else changes.", "Reset", true, [this, change, name] {
            for (int i : change) set(i, default_value(S[i]));
            flash(name + " reset to defaults");
        });
        modal.rows = rows;
    }

    static mini::string lc_first(mini::string n) {   // a group name inside a sentence: "Reset keyboard keys"
        if (n.size() > 1 && islower((unsigned char)n[1])) n[0] = (char)tolower((unsigned char)n[0]);   // "DLSS internals" stays
        return n;
    }
    mini::string group_label(const mini::string &g) const {
        size_t slash = g.rfind('/');
        mini::string n = slash == mini::string::npos ? g : g.substr(slash + 1);
        if (!n.empty() && n.back() == '+') n.pop_back();
        return n;
    }

    // ---------------------------------------------------------------- button bindings (Type::Pad / Type::Key)
    // Accept on a binding row opens a dialog that takes the next controller button or key. A button or key that
    // another Switch button already uses moves over; the other one takes this one's old binding (a swap), so every
    // Switch button keeps something to press unless you clear it on purpose.
    void open_bind(int i) {
        modal = Modal();
        modal.kind = Modal::Bind;
        modal.setting = i;
        modal.bind_pad = S[i].type == Type::Pad;
        modal.title = mini::string(modal.bind_pad ? "Controller" : "Keyboard") + ": Switch " + S[i].name;
        modal.open_t = (float)now;
        modal.bind_until = now + kBindSeconds;
    }

    mini::string bind_label(bool pad, const mini::string &t) const { return pad ? pad_label(t) : key_label(t); }

    mini::string bind_labels(int i) const {
        mini::vector<mini::string> l = bind_list(val(i).text, S[i].type == Type::Pad);
        if (l.empty()) return "nothing";
        mini::string out;
        for (size_t k = 0; k < l.size(); k++) out += (k ? " or " : "") + bind_label(S[i].type == Type::Pad, l[k]);
        return out;
    }

    void bind_set(int i, const mini::vector<mini::string> &l) {
        Value v;
        v.text = bind_join(l);
        set(i, v);
    }

    void bind_apply(const mini::string &token) {
        int i = modal.setting;
        if (i < 0 || modal.kind != Modal::Bind) return;
        bool pad = modal.bind_pad;
        mini::vector<mini::string> old = bind_list(val(i).text, pad);
        close_modal();
        mini::string what = bind_label(pad, token);
        if (old.size() == 1 && old[0] == token) { flash("Switch " + S[i].name + " already uses " + what); return; }
        mini::string moved;
        for (int j = 0; j < (int)S.size(); j++) {
            if (j == i || S[j].type != S[i].type) continue;
            mini::vector<mini::string> l = bind_list(val(j).text, pad);
            auto it = mini::find(l.begin(), l.end(), token);
            if (it == l.end()) continue;
            l.erase(it);
            if (l.empty()) l = old;                       // a swap: the other button takes this one's old binding
            bind_set(j, l);
            moved = "Switch " + S[j].name + " now uses " + bind_labels(j);
        }
        bind_set(i, {token});
        flash("Switch " + S[i].name + ": " + what + (moved.empty() ? "" : "  \xC2\xB7  " + moved));
    }

    void bind_clear() {
        int i = modal.setting;
        if (i < 0 || modal.kind != Modal::Bind) return;
        close_modal();
        bind_set(i, {});
        flash("Switch " + S[i].name + " is not bound " + (S[i].type == Type::Pad ? "on the controller" : "on the keyboard") +
              " now (Del / X on the row puts the default back)");
    }

    void bind_cancel(const char *why) {
        int i = modal.setting;
        close_modal();
        if (i >= 0) flash(mini::string(why) + ": Switch " + S[i].name + " is unchanged");
    }

    void bind_key(SDL_Scancode sc) {
        int k = uit_key_from_scancode(sc);
        if (k < 0) { modal.error = mini::string(SDL_GetScancodeName(sc)) + " is not a key the game can read. Pick another one."; return; }
        if (kUitKeys[k].stick) {
            modal.error = mini::string(kUitKeys[k].label) + (kUitKeys[k].sc == SDL_SCANCODE_W || kUitKeys[k].sc == SDL_SCANCODE_A || kUitKeys[k].sc == SDL_SCANCODE_S ||
                                                              kUitKeys[k].sc == SDL_SCANCODE_D ? " moves Link (the left stick)" : " moves the camera (the right stick)") +
                          ", so it cannot press a button. Pick another key.";
            return;
        }
        bind_apply(kUitKeys[k].token);
    }

    void bind_pad_button(int b) {
        for (int k = 0; k < UIT_PAD_INPUT_COUNT; k++)
            if (kUitPadInputs[k].button == b) { bind_apply(kUitPadInputs[k].token); return; }
    }

    void back() {
        if (tab == TAB_PLAY) { press_button(B_Quit); return; }
        if (tab == TAB_MODS && grab >= 0) { grab = -1; return; }
        if (searching()) { set_sfield(true); follow = true; return; }   // [UIT 0251] B / Esc on a result: back to the field
        if (!items.empty() && (items[focus[tab]].kind == Item::Row || items[focus[tab]].kind == Item::Button)) {   // inside an opened group: fold it
            mini::string k = items[focus[tab]].gid;
            if (!k.empty() && k.back() == '+' && expanded.count(k)) {
                expanded.erase(k);
                build_items();
                for (int g = 0; g < (int)items.size(); g++)
                    if (items[g].kind == Item::Group && items[g].gid == k) focus[tab] = g;
                return;
            }
            const Item &r = items[focus[tab]];
            if (r.kind == Item::Row && !S[r.idx].section.empty() && expanded.count(sec_gid(S[r.idx]))) {   // inside a section: fold it
                mini::string sg = sec_gid(S[r.idx]);
                expanded.erase(sg);
                build_items();
                for (int g = 0; g < (int)items.size(); g++)
                    if (items[g].kind == Item::Group && items[g].gid == sg) focus[tab] = g;
                return;
            }
        }
        switch_tab(TAB_PLAY);
    }

    void handle(const Action &a) {
        if (modal.kind != Modal::None) { handle_modal(a); return; }
        if (sosk) { handle_search_osk(a); return; }
        if (sfield) { handle_search_field(a); return; }
        if (a.a == A_Search) { open_search(); return; }
        if (searching() && (a.a == A_Grab || a.a == A_ModUp || a.a == A_ModDown)) return;   // the load order moves in the full list only
        Item *it = items.empty() ? nullptr : &items[focus[tab]];
        if (grab >= 0 && tab == TAB_MODS) {   // moving a mod: up / down move it, A / B / Y drop it
            switch (a.a) {
            case A_Up: case A_ModUp: mod_move(grab, -1); return;
            case A_Down: case A_ModDown: mod_move(grab, 1); return;
            case A_Accept: case A_Back: case A_Grab: grab = -1; flash("Load order saved"); return;
            case A_TabPrev: case A_TabNext: case A_JumpTab: case A_Play: case A_PlayLog: grab = -1; break;
            default: return;
            }
        }
        bool on_mod = it && it->kind == Item::Mod;
        switch (a.a) {
        case A_Grab:
            if (on_mod && mods.nmods > 1) { grab = it->idx; flash("Moving " + mod_title(grab) + ": up / down, then A to drop"); }
            break;
        case A_ModUp: if (on_mod) mod_move(it->idx, -1); break;
        case A_ModDown: if (on_mod) mod_move(it->idx, 1); break;
        default: break;
        }
        if (on_mod && (a.a == A_Left || a.a == A_Right)) { if (!a.repeat) mod_set(it->idx, a.a == A_Right); return; }
        switch (a.a) {
        case A_Up:
            if (search_tab(tab) && a.repeat == 0 && focus[tab] == first_focusable()) { set_sfield(true); follow = true; break; }   // [UIT 0251] up from the top: the search
            move_focus(-1, a.repeat == 0);
            break;
        case A_Down: move_focus(1, a.repeat == 0); break;
        case A_Left:
        case A_Right:
            if (it && it->kind == Item::Row) step(it->idx, a.a == A_Left ? -1 : 1, a.repeat, a.fast);
            else if (it && it->kind == Item::Group) {
                bool open = expanded.count(it->gid) > 0;
                if ((a.a == A_Right) != open) toggle_group(it->gid);
            }
            break;
        case A_Accept: accept_item(); break;
        case A_Back: back(); break;
        case A_Reset: reset_item(); break;
        case A_TabPrev: switch_tab(tab - 1); break;
        case A_TabNext: switch_tab(tab + 1); break;
        case A_JumpTab: switch_tab(a.n); break;
        case A_PagePrev: page(-1); break;
        case A_PageNext: page(1); break;
        case A_Home: focus[tab] = 0; clamp_focus(); break;
        case A_End:
            focus[tab] = (int)items.size() - 1;
            if (!items.empty() && !items.back().focusable()) move_focus(-1, false);
            break;
        case A_Play: play(false); break;
        case A_PlayLog: play(true); break;
        default: break;
        }
    }

    // modal buttons, left to right: [alt] [cancel] [ok]; ids 0 ok, 1 cancel, 2 alt
    mini::vector<int> modal_order() const {
        mini::vector<int> o;
        if (!modal.alt.empty()) o.push_back(2);
        if (!modal.cancel.empty()) o.push_back(1);
        o.push_back(0);
        return o;
    }

    void modal_press(int id) {
        if (modal.busy) return;
        mini::function<void()> ok = mini::move(modal.on_ok), alt = mini::move(modal.on_alt);   // the modal is cleared next
        close_modal();
        if (id == 0 && ok) ok();
        if (id == 2 && alt) alt();
    }

    void osk_type(const mini::string &c) {
        if (modal.buf.size() + c.size() <= 260) modal.buf += c;
        modal.error.clear();
        if (modal.shift && c.size() == 1 && isalpha((unsigned char)c[0]) && dev == Device::Gamepad) modal.shift = false;
    }

    void osk_press() {
        if (modal.kr < OSK_SPECIAL_ROW) {
            const char *row = (modal.shift ? kOskRowsShift : kOskRows)[modal.kr];
            osk_type(mini::string(1, row[modal.kc]));
            return;
        }
        switch (modal.kc) {
        case 0: modal.shift = !modal.shift; break;
        case 1: osk_type(" "); break;
        case 2: backspace(); break;
        case 3: modal.buf.clear(); break;
        case 4: paste(); break;
        case 5: edit_done(); break;
        }
    }

    void backspace() {
        if (modal.buf.empty()) return;
        size_t n = modal.buf.size() - 1;
        while (n > 0 && ((unsigned char)modal.buf[n] & 0xC0) == 0x80) n--;
        modal.buf.erase(n);
        modal.error.clear();
    }

    void paste() {
        char *t = SDL_GetClipboardText();
        if (t) {
            mini::string s = t;
            s.erase(mini::remove_if(s.begin(), s.end(), [](char c) { return c == '\r' || c == '\n'; }), s.end());
            osk_type(s);
            SDL_free(t);
        }
    }

    void handle_modal(const Action &a) {
        if (modal.kind == Modal::Bind) {       // the presses themselves come in through key_down / pad_button
            if (a.a == A_Back) bind_cancel("Cancelled");
            else if (a.a == A_Reset) bind_clear();
            return;
        }
        if (modal.kind == Modal::Edit) {
            int cols = modal.kr == OSK_SPECIAL_ROW ? 6 : 11;
            switch (a.a) {
            case A_Char: osk_type(a.ch); break;
            case A_Backspace: case A_Reset: backspace(); break;
            case A_Shift: modal.shift = !modal.shift; break;
            case A_Paste: paste(); break;
            case A_Done: case A_Play: edit_done(); break;
            case A_Back: close_modal(); break;
            case A_Accept: if (dev == Device::Gamepad) osk_press(); else edit_done(); break;
            case A_Left: modal.kc = (modal.kc + cols - 1) % cols; break;
            case A_Right: modal.kc = (modal.kc + 1) % cols; break;
            case A_Up:
            case A_Down: {
                int r = (modal.kr + (a.a == A_Down ? 1 : 4)) % 5;
                if (r == OSK_SPECIAL_ROW && modal.kr != OSK_SPECIAL_ROW) modal.kc = mini::min(5, modal.kc * 6 / 11);
                else if (r != OSK_SPECIAL_ROW && modal.kr == OSK_SPECIAL_ROW) modal.kc = mini::min(10, modal.kc * 11 / 6 + 1);
                modal.kr = r;
                break;
            }
            default: break;
            }
            return;
        }
        mini::vector<int> o = modal_order();
        int pos = (int)(mini::find(o.begin(), o.end(), modal.focus) - o.begin());
        switch (a.a) {
        case A_Left: case A_Up: modal.focus = o[mini::max(0, pos - 1)]; break;
        case A_Right: case A_Down: modal.focus = o[mini::min((int)o.size() - 1, pos + 1)]; break;
        case A_Accept: modal_press(modal.focus); break;
        case A_Back: close_modal(); break;
        default: break;
        }
    }

    // ---------------------------------------------------------------- drawing: shared pieces
    struct Hit {
        ImVec2 a, b;
        int item = -1;                    // hover focuses this item
        mini::function<void()> click;
        int slider = -1;                  // click/drag sets this setting from x
        float x0 = 0, x1 = 0;
    };
    mini::vector<Hit> hits;
    float mx = -1, my = -1;
    bool follow = true;                   // keep the focused row scrolled into view (off while using the wheel)
    mini::vector<mini::pair<int, mini::string>> script;

    float ease(float cur, float to, float rate) const {
        if (snap) return to;
        float k = 1.0f - mini::exp(-rate * (float)dt);
        float v = cur + (to - cur) * k;
        return mini::fabs(v - to) < 0.25f ? to : v;
    }

    void hit(ImVec2 a, ImVec2 b, int item, mini::function<void()> fn = nullptr) {
        Hit h;
        h.a = a, h.b = b, h.item = item, h.click = mini::move(fn);
        hits.push_back(mini::move(h));
    }

    bool hovered(ImVec2 a, ImVec2 b) const { return mx >= a.x && mx < b.x && my >= a.y && my < b.y; }

    // [UIT 0270] a face button by position (0 bottom, 1 right, 2 left, 3 top), drawn as the connected controller labels
    // it: A B X Y (Xbox and generic), B A Y X (Nintendo), or the PlayStation shapes. Returns the width.
    float face_glyph(float x0, float cy, int pos, bool draw = true, ImU32 fill = 0, ImU32 ink = 0) {
        float rr = d.px(12);
        if (!draw) return rr * 2;
        if (!fill) fill = pal::raised;
        if (!ink) ink = pal::text;
        ImVec2 c = {x0 + rr, cy};
        d.dl->AddCircleFilled(c, rr, fill, 24);
        d.dl->AddCircle(c, rr, alpha(pal::faint, 0.9f), 24, 1);
        if (pad_family == PadFamily::PlayStation) {
            float q = d.px(5.5f), th = d.px(1.6f);
            if (pos == 0) {
                d.dl->AddLine({c.x - q, c.y - q}, {c.x + q, c.y + q}, ink, th);
                d.dl->AddLine({c.x - q, c.y + q}, {c.x + q, c.y - q}, ink, th);
            } else if (pos == 1) d.dl->AddCircle(c, q + d.px(0.5f), ink, 20, th);
            else if (pos == 2) d.dl->AddRect({c.x - q, c.y - q}, {c.x + q, c.y + q}, ink, 0, 0, th);
            else d.dl->AddTriangle({c.x, c.y - q - d.px(1)}, {c.x + q + d.px(0.5f), c.y + q - d.px(1)}, {c.x - q - d.px(0.5f), c.y + q - d.px(1)}, ink, th);
            return rr * 2;
        }
        static const char *kPos[] = {"south", "east", "west", "north"};
        mini::string t = face_name(kPos[pos & 3]);
        ImVec2 ts = d.measure(d.semi, d.fs(13), t);
        d.text(d.semi, d.fs(13), {c.x - ts.x / 2, cy - ts.y / 2}, ink, t);
        return rr * 2;
    }

    // A controller button or a key cap, vertically centred on cy. Returns the width.
    float glyph(Glyph g, float x, float cy, bool draw = true) {
        float h = d.px(24), r = d.px(5), pad = d.px(7), fsz = d.fs(12.5f);
        ImU32 fill = pal::raised, edge = alpha(pal::faint, 0.9f), ink = pal::body;
        auto cap = [&](float x0, const mini::string &t, float minw) {
            float w = mini::max(minw, d.measure(d.semi, fsz, t).x + pad * 2);
            if (draw) {
                d.rect({x0, cy - h / 2}, {x0 + w, cy + h / 2}, fill, r);
                d.outline({x0, cy - h / 2}, {x0 + w, cy + h / 2}, edge, r);
                ImVec2 ts = d.measure(d.semi, fsz, t);
                d.text(d.semi, fsz, {x0 + (w - ts.x) / 2, cy - ts.y / 2}, ink, t);
            }
            return w;
        };
        auto arrows = [&](float x0, bool vertical) {
            float w = d.px(34);
            if (draw) {
                d.rect({x0, cy - h / 2}, {x0 + w, cy + h / 2}, fill, r);
                d.outline({x0, cy - h / 2}, {x0 + w, cy + h / 2}, edge, r);
                float c = x0 + w / 2, sz = d.px(4.5f);
                if (vertical) { d.chevron({c, cy - d.px(4)}, sz, ink, 3); d.chevron({c, cy + d.px(4)}, sz, ink, 1); }
                else { d.chevron({c - d.px(6), cy}, sz, ink, 2); d.chevron({c + d.px(6), cy}, sz, ink, 0); }
            }
            return w;
        };
        auto face = [&](float x0, const char *t) {
            float rr = d.px(12);
            if (draw) {
                d.dl->AddCircleFilled({x0 + rr, cy}, rr, fill, 24);
                d.dl->AddCircle({x0 + rr, cy}, rr, edge, 24, 1);
                ImVec2 ts = d.measure(d.semi, d.fs(13), t);
                d.text(d.semi, d.fs(13), {x0 + rr - ts.x / 2, cy - ts.y / 2}, pal::text, t);
            }
            return rr * 2;
        };
        auto dpad = [&](float x0, bool vertical) {
            float sz = d.px(22), arm = d.px(7.5f), c = x0 + sz / 2;
            if (draw) {
                ImU32 lo = alpha(pal::faint, 0.8f), hi = pal::body;
                d.rect({c - arm / 2, cy - sz / 2}, {c + arm / 2, cy + sz / 2}, vertical ? hi : lo, d.px(2));
                d.rect({c - sz / 2, cy - arm / 2}, {c + sz / 2, cy + arm / 2}, vertical ? lo : hi, d.px(2));
                d.rect({c - arm / 2, cy - arm / 2}, {c + arm / 2, cy + arm / 2}, lo);
            }
            return sz;
        };
        auto menu = [&](float x0) {     // the Start / Menu button
            float w = d.px(30);
            if (draw) {
                d.rect({x0, cy - h / 2}, {x0 + w, cy + h / 2}, fill, h / 2);
                d.outline({x0, cy - h / 2}, {x0 + w, cy + h / 2}, edge, h / 2);
                for (int k = -1; k <= 1; k++)
                    d.rect({x0 + w / 2 - d.px(6), cy + k * d.px(4) - d.px(0.75f)}, {x0 + w / 2 + d.px(6), cy + k * d.px(4) + d.px(0.75f)}, ink);
            }
            return w;
        };
        auto view = [&](float x0) {     // [UIT 0251] the View / Back button: two overlapping windows
            float w = d.px(30);
            if (draw) {
                d.rect({x0, cy - h / 2}, {x0 + w, cy + h / 2}, fill, h / 2);
                d.outline({x0, cy - h / 2}, {x0 + w, cy + h / 2}, edge, h / 2);
                float c = x0 + w / 2, q = d.px(4.5f), o = d.px(2);
                d.outline({c - q - o, cy - q - o}, {c + q - o, cy + q - o}, ink, d.px(1.5f), d.px(1.25f));
                d.rect({c - q + o, cy - q + o}, {c + q + o, cy + q + o}, fill, d.px(1.5f));
                d.outline({c - q + o, cy - q + o}, {c + q + o, cy + q + o}, ink, d.px(1.5f), d.px(1.25f));
            }
            return w;
        };
        float gap = d.px(4);
        bool pad_dev = dev == Device::Gamepad;
        switch (g) {
        case G_Search: return pad_dev ? view(x) : cap(x, "Ctrl F", 0);
        case G_Move: return pad_dev ? dpad(x, true) : arrows(x, true);
        case G_Change: return pad_dev ? dpad(x, false) : arrows(x, false);
        case G_Accept: return pad_dev ? face_glyph(x, cy, confirm_game() ? 1 : 0, draw) : cap(x, "Enter", 0);
        case G_Back: return pad_dev ? face_glyph(x, cy, confirm_game() ? 0 : 1, draw) : cap(x, "Esc", 0);
        case G_Reset: return pad_dev ? face_glyph(x, cy, 2, draw) : cap(x, "Del", 0);
        case G_Shift: return pad_dev ? face_glyph(x, cy, 3, draw) : cap(x, "Shift", 0);
        case G_Grab: return pad_dev ? face_glyph(x, cy, 3, draw) : cap(x, "M", 0);
        case G_Play: return pad_dev ? menu(x) : cap(x, "F5", 0);
        case G_Done: return pad_dev ? menu(x) : cap(x, "Enter", 0);
        case G_Tabs: {
            float w = cap(x, pad_dev ? "LB" : "Q", h);
            return w + gap + cap(x + w + gap, pad_dev ? "RB" : "E", h);
        }
        case G_Page: {
            float w = cap(x, pad_dev ? "LT" : "PgUp", h);
            return w + gap + cap(x + w + gap, pad_dev ? "RT" : "PgDn", h);
        }
        }
        return 0;
    }

    void top_bar(float W) {
        float h = d.px(76), cy = h / 2;
        d.rect({0, 0}, {W, h}, pal::bg);
        d.rect({0, h - 1}, {W, h}, pal::line);
        float x = d.px(40);
        float fs = d.fs(15), sp = d.px(3.2f);
        ImVec2 ws = d.measure(d.semi, fs, "U");
        bool pad_dev = dev == Device::Gamepad;
        // the layout first: tabs centred unless that would run them (and the LB / Q pill on their left) into the title;
        // then shifted right, and on narrow windows the "Launcher" label is left out
        float tfs = d.fs(16), tpad = d.px(17);
        float total = 0;
        float widths[TAB_COUNT];
        for (int t = 0; t < TAB_COUNT; t++) total += widths[t] = d.measure(d.semi, tfs, kTabNames[t]).x + tpad * 2;
        float gw = glyph(G_Tabs, 0, 0, false);   // LB / RB on either side
        float lbw = gw / 2;
        auto pill_w = [&](const char *t) { return mini::max(d.measure(d.semi, d.fs(12.5f), t).x + d.px(14), d.px(24)); };
        float pl = pill_w(pad_dev ? "LB" : "Q"), pr = pill_w(pad_dev ? "RB" : "E");
        float title_end = x + (logo ? d.px(34) + d.px(14) : 0) + d.spaced_width(d.semi, fs, "ULTRA INSTINCT TEARS", sp);
        float label_end = title_end + d.px(24) + d.measure(d.reg, fs, "Launcher").x;
        float right = W - (running ? d.px(40) + d.measure(d.reg, d.fs(14), "Game running").x + d.px(28) : d.px(24));
        auto left_ok = [&](float end) { return end + d.px(20) + pl + d.px(14); };   // the first tab's left edge allowed
        float tx = (W - total) / 2;
        bool label = true;
        if (tx < left_ok(label_end)) {
            tx = left_ok(label_end);
            if (tx + total + d.px(14) + pr > right) { label = false; tx = mini::max((W - total) / 2, left_ok(title_end)); }
        }
        if (logo) {
            float ls = d.px(34);
            d.dl->AddImage((ImTextureID)(intptr_t)logo, {x, cy - ls / 2}, {x + ls, cy + ls / 2});
            x += ls + d.px(14);
        }
        d.text_spaced(d.semi, fs, {x, cy - ws.y / 2}, pal::text, "ULTRA INSTINCT TEARS", sp);
        x += d.spaced_width(d.semi, fs, "ULTRA INSTINCT TEARS", sp) + d.px(12);
        if (label) {
            d.rect({x, cy - d.px(8)}, {x + 1, cy + d.px(8)}, pal::line);
            d.text(d.reg, fs, {x + d.px(12), cy - ws.y / 2}, pal::dim, "Launcher");
        }

        // tabs
        float mid = tx;
        ImVec2 ts = d.measure(d.semi, tfs, "Ag");
        for (int t = 0; t < TAB_COUNT; t++) {
            ImVec2 a = {tx, 0}, b = {tx + widths[t], h};
            bool hov = hovered(a, b) && modal.kind == Modal::None;
            ImU32 c = t == tab ? pal::text : hov ? pal::body : pal::dim;
            d.text(d.semi, tfs, {tx + tpad, cy - ts.y / 2}, c, kTabNames[t]);
            if (t == TAB_ABOUT && update_avail)   // [UIT 0320] a newer release: a small dot after About, in the tab's padding
                d.dl->AddCircleFilled({tx + widths[t] - tpad + d.px(7), cy - ts.y / 2 + d.px(3)}, d.px(3.5f), pal::gold);
            if (t == tab) {
                float to_x = tx + tpad, to_w = widths[t] - tpad * 2;
                if (tab_w == 0 || snap) tab_x = to_x, tab_w = to_w;
                tab_x = ease(tab_x, to_x, 16), tab_w = ease(tab_w, to_w, 16);
            }
            if (modal.kind == Modal::None) { int tt = t; hit(a, b, -1, [this, tt] { switch_tab(tt); }); }
            tx += widths[t];
        }
        d.rect({tab_x, h - d.px(3)}, {tab_x + tab_w, h - 1}, pal::teal, d.px(1));
        // the tab switch glyphs, clickable
        float gx0 = mid - d.px(14) - lbw, gx1 = tx + d.px(14);
        auto one = [&](float gx, const char *t, int dir) {
            float w = d.measure(d.semi, d.fs(12.5f), t).x + d.px(14);
            w = mini::max(w, d.px(24));
            if (dir < 0) gx -= w - lbw;
            d.rect({gx, cy - d.px(12)}, {gx + w, cy + d.px(12)}, pal::raised, d.px(5));
            d.outline({gx, cy - d.px(12)}, {gx + w, cy + d.px(12)}, alpha(pal::faint, 0.9f), d.px(5));
            ImVec2 s2 = d.measure(d.semi, d.fs(12.5f), t);
            d.text(d.semi, d.fs(12.5f), {gx + (w - s2.x) / 2, cy - s2.y / 2}, pal::body, t);
            if (modal.kind == Modal::None) hit({gx, cy - d.px(14)}, {gx + w, cy + d.px(14)}, -1, [this, dir] { switch_tab(tab + dir); });
        };
        one(gx0, pad_dev ? "LB" : "Q", -1);
        one(gx1, pad_dev ? "RB" : "E", 1);

        // right: the running state
        if (running) {
            ImVec2 rs = d.measure(d.reg, d.fs(14), "Game running");
            float rx = W - d.px(40) - rs.x;
            d.dl->AddCircleFilled({rx - d.px(10), cy}, d.px(3.5f), pal::teal);
            d.text(d.reg, d.fs(14), {rx, cy - rs.y / 2}, pal::dim, "Game running");
        }
    }

    struct Hint { Glyph g; mini::string label; Act act; };

    mini::vector<Hint> hints() {
        mini::vector<Hint> h;
        bool pad_dev = dev == Device::Gamepad;
        if (modal.kind == Modal::Edit) {
            if (pad_dev) {
                h = {{G_Accept, "Type", A_Accept}, {G_Reset, "Delete", A_Backspace}, {G_Shift, "Shift", A_Shift},
                     {G_Done, "Done", A_Done}, {G_Back, "Cancel", A_Back}};
            } else {
                h = {{G_Done, "Save", A_Done}, {G_Back, "Cancel", A_Back}};
            }
            return h;
        }
        if (modal.kind == Modal::Bind) {
            // with the controller dialog every pad button binds; the keyboard cancels and clears
            if (modal.bind_pad && pad_dev) return {};
            return {{G_Reset, "Clear", A_Reset}, {G_Back, "Cancel", A_Back}};
        }
        if (modal.busy) return {};
        if (modal.kind != Modal::None) return {{G_Change, "Choose", A_Right}, {G_Accept, "Select", A_Accept}, {G_Back, "Cancel", A_Back}};
        if (sosk) {   // [UIT 0251] the controller's search keyboard
            if (pad_dev) return {{G_Accept, "Type", A_Accept}, {G_Reset, "Delete", A_Backspace}, {G_Shift, "Space", A_Shift},
                                 {G_Done, "Done", A_Done}, {G_Back, "Close", A_Back}};
            return {{G_Accept, "Done", A_Accept}, {G_Back, "Close", A_Back}};
        }
        if (sfield) {   // the search field
            if (pad_dev) h.push_back({G_Accept, "Keyboard", A_Accept});
            if (smatch > 0 && searching()) h.push_back({pad_dev ? G_Move : G_Accept, "Results", A_Down});
            if (pad_dev && !query[tab].empty()) h.push_back({G_Reset, "Delete", A_Backspace});
            h.push_back({G_Back, query[tab].empty() ? "Leave search" : "Clear", A_Back});
            h.push_back({G_Play, "Play", A_Play});
            return h;
        }
        Item *it = items.empty() ? nullptr : &items[focus[tab]];
        if (it && it->kind == Item::Row) {
            const Setting &s = S[it->idx];
            bool on = active(it->idx);
            bool bind = is_bind(it->idx);
            if (on && s.type != Type::Text && !bind) h.push_back({G_Change, "Change", A_Right});
            if (on) h.push_back({G_Accept, bind ? "Bind" : s.type == Type::Bool ? "Toggle" : s.type == Type::Choice ? "Next" : "Edit", A_Accept});
            if (modified(it->idx)) h.push_back({G_Reset, "Default", A_Reset});
            h.push_back({G_Back, searching() ? "To search" : s.groupFolded && expanded.count(gid(s)) ? "Close group" : "Back", A_Back});
        } else if (it && it->kind == Item::Group) {
            h.push_back({G_Accept, expanded.count(it->gid) ? "Close" : "Open", A_Accept});
            int changed = 0;
            group_count(it->gid, &changed);
            if (changed) h.push_back({G_Reset, "Reset group", A_Reset});
            h.push_back({G_Back, "Back", A_Back});
        } else if (it && it->kind == Item::Mod && grab >= 0) {
            h.push_back({G_Move, "Move mod", A_Down});
            h.push_back({G_Accept, "Drop", A_Accept});
        } else if (it && it->kind == Item::Mod) {
            h.push_back({G_Accept, mods.mods[it->idx].enabled ? "Turn off" : "Turn on", A_Accept});
            if (mods.nmods > 1) h.push_back({G_Grab, "Move", A_Grab});
            h.push_back({G_Reset, "Uninstall", A_Reset});
            h.push_back({G_Back, searching() ? "To search" : "Back", A_Back});
        } else if (it && it->kind == Item::Button && it->idx == B_ResetGroup) {
            h.push_back({G_Accept, "Reset", A_Accept});
            h.push_back({G_Back, "Close group", A_Back});
        } else {
            h.push_back({G_Move, "Move", A_Down});
            h.push_back({G_Accept, "Select", A_Accept});
            if (tab != TAB_PLAY) h.push_back({G_Back, searching() ? "To search" : "Back", A_Back});
        }
        if (tab != TAB_PLAY && tab != TAB_ABOUT && !pad_dev) h.push_back({G_Page, "Section", A_PageNext});
        if (search_tab(tab)) h.push_back({G_Search, "Search", A_Search});
        h.push_back({G_Play, "Play", A_Play});
        return h;
    }

    void footer(float W, float H) {
        float h = d.px(58), y = H - h, cy = y + h / 2;
        d.rect({0, y}, {W, H}, pal::bg);
        d.rect({0, y}, {W, y + 1}, pal::line);
        // left: what just happened
        float x = d.px(40);
        float fs = d.fs(14);
        ImVec2 ts = d.measure(d.reg, fs, "Ag");
        mini::string msg;
        ImU32 col = pal::dim;
        bool dot = false;
        if (!save_error.empty()) msg = "Could not save ultrainstincttears.cfg: " + save_error, col = pal::gold;
        else if (now - toast_at < 3.0) msg = toast, col = pal::body, dot = true;
        else if (save_at >= 0) msg = "Saving\xE2\x80\xA6";
        else if (now - saved_at < 2.5) msg = "Saved to ultrainstincttears.cfg", dot = true;
        else if (update_avail && !running) msg = avail_line(), col = pal::gold;   // [UIT 0320] the one-line notice
        else msg = cfg.exists ? "Changes save automatically" : "No ultrainstincttears.cfg yet: the first change creates it";
        if (dot) {
            d.check({x + d.px(6), cy}, d.px(9), pal::teal);
            x += d.px(20);
        }
        float hint_w = 0;
        auto hs = hints();
        for (auto &k : hs) hint_w += glyph(k.g, 0, 0, false) + d.px(8) + d.measure(d.reg, fs, k.label).x + d.px(24);
        d.text_fit(d.reg, fs, {x, cy - ts.y / 2}, col, msg, W - hint_w - x - d.px(60));
        // right: what the buttons do here
        float hx = W - d.px(40) - hint_w + d.px(24);
        for (auto &k : hs) {
            float x0 = hx;
            hx += glyph(k.g, hx, cy) + d.px(8);
            float lw = d.measure(d.reg, fs, k.label).x;
            bool hov = hovered({x0, y}, {hx + lw, H});
            d.text(d.reg, fs, {hx, cy - ts.y / 2}, hov ? pal::text : pal::dim, k.label);
            Act act = k.act;
            hit({x0 - d.px(6), y}, {hx + lw + d.px(6), H}, -1, [this, act] {
                if (modal.kind == Modal::None && (act == A_Right || act == A_Down)) return;   // "Change" / "Move" are not buttons
                Action a{act};
                handle(a);
            });
            hx += lw + d.px(24);
        }
    }

    // ---------------------------------------------------------------- drawing: settings tabs
    float ctrl_w() const { return d.px(250); }

    // The value control at the right of a row; [x0, x1] is its box, cy its centre line.
    void control(int i, float x0, float x1, float cy, bool foc, float fade, bool live) {
        if (locked(i)) live = false;   // [UIT 0070] greyed out: no click targets
        const Setting &s = S[i];
        Value v = val(i);
        auto A = [&](ImU32 c) { return alpha(c, fade); };
        float fs = d.fs(16);
        ImVec2 ts = d.measure(d.semi, fs, "Ag");
        switch (s.type) {
        case Type::Bool: {
            float tw = d.px(46), th = d.px(26);
            ImVec2 a = {x1 - tw, cy - th / 2}, b = {x1, cy + th / 2};
            float &k = knob[i];
            float to = v.b ? 1.0f : 0.0f;
            if (!knob.count(-1 - i) || snap) k = to, knob[-1 - i] = 1;
            k = ease(k * 100, to * 100, 18) / 100;
            d.rect(a, b, A(mix(pal::line, pal::teal, k)), th / 2);
            float r = th / 2 - d.px(3.5f);
            ImVec2 c = {a.x + th / 2 + (tw - th) * k, cy};
            d.dl->AddCircleFilled(c, r, A(mix(pal::dim, pal::ink, k)), 24);
            mini::string lbl = v.b ? "On" : "Off";
            ImVec2 ls = d.measure(d.semi, fs, lbl);
            d.text(d.semi, fs, {a.x - d.px(14) - ls.x, cy - ts.y / 2}, A(v.b ? pal::text : pal::dim), lbl);
            if (live) hit({a.x - d.px(14) - ls.x - d.px(6), cy - d.px(20)}, {x1 + d.px(4), cy + d.px(20)}, -1, [this, i] {
                Value nv = val(i);
                nv.b = !nv.b;
                set(i, nv);
            });
            break;
        }
        case Type::Choice: {
            mini::string lbl = display(s, v);
            float cw = x1 - x0;
            int n = (int)s.choices.size();
            bool custom = v.kind == Value::Custom;
            bool can_l = live && (custom || v.index > 0), can_r = live && (custom || v.index < n - 1);
            if (s.key == "TOTK_UPSCALER" && !dll && !custom && v.index == n - 2) can_r = false;
            float ch = d.px(6);
            ImU32 on = foc ? pal::teal : pal::body;
            d.chevron({x0 + d.px(10), cy - d.px(4)}, ch, A(can_l ? on : alpha(pal::faint, 0.5f)), 2);
            d.chevron({x1 - d.px(10), cy - d.px(4)}, ch, A(can_r ? on : alpha(pal::faint, 0.5f)), 0);
            ImVec2 ls = d.measure(d.semi, fs, lbl);
            float maxw = cw - d.px(48);
            float lx = x0 + (cw - mini::min(ls.x, maxw)) / 2;
            d.text_fit(d.semi, fs, {lx, cy - d.px(4) - ts.y / 2}, A(foc ? pal::text : pal::body), lbl, maxw);
            if (n <= 12) {            // position pips under the label
                float pw = d.px(5), pg = d.px(5);
                float total = n * pw + (n - 1) * pg, px0 = x0 + (cw - total) / 2, py = cy + d.px(13);
                for (int c = 0; c < n; c++) {
                    bool cur = !custom && c == v.index;
                    bool ok = choice_ok(i, c);
                    ImU32 pc = cur ? (foc ? pal::teal : pal::body) : alpha(pal::faint, ok ? 0.8f : 0.3f);
                    d.rect({px0 + c * (pw + pg), py - d.px(1.5f)}, {px0 + c * (pw + pg) + pw, py + d.px(1.5f)}, A(pc), d.px(1.5f));
                }
            }
            if (live) {
                hit({x0 - d.px(4), cy - d.px(22)}, {x0 + d.px(28), cy + d.px(22)}, -1, [this, i] { step(i, -1, 0, false); });
                hit({x1 - d.px(28), cy - d.px(22)}, {x1 + d.px(4), cy + d.px(22)}, -1, [this, i] { step(i, 1, 0, false); });
            }
            break;
        }
        case Type::Int:
        case Type::Float: {
            mini::string lbl = display(s, v);
            ImVec2 ls = d.measure(d.semi, fs, lbl);
            d.text_fit(d.semi, fs, {x1 - mini::min(ls.x, x1 - x0), cy - d.px(7) - ts.y / 2}, A(foc ? pal::text : pal::body), lbl, x1 - x0, false);
            float ty = cy + d.px(13), tx0 = x0 + d.px(40), tx1 = x1;
            d.rect({tx0, ty - d.px(1.5f)}, {tx1, ty + d.px(1.5f)}, A(pal::line), d.px(1.5f));
            auto pos = [&](double n) { return tx0 + (float)((n - s.min) / (s.max - s.min)) * (tx1 - tx0); };
            Value dv = default_value(s);
            if (dv.kind == Value::Normal) d.rect({pos(dv.num) - d.px(0.75f), ty - d.px(5)}, {pos(dv.num) + d.px(0.75f), ty + d.px(5)}, A(pal::faint));
            if (v.kind == Value::Normal) {
                float p = pos(mini::clamp(v.num, s.min, s.max));
                d.rect({tx0, ty - d.px(1.5f)}, {p, ty + d.px(1.5f)}, A(foc ? pal::teal : pal::dim), d.px(1.5f));
                d.dl->AddCircleFilled({p, ty}, d.px(foc ? 6 : 5), A(foc ? pal::teal : pal::body), 20);
            }
            if (live) {
                Hit h;
                h.a = {tx0 - d.px(8), ty - d.px(12)}, h.b = {tx1 + d.px(8), ty + d.px(12)};
                h.slider = i, h.x0 = tx0, h.x1 = tx1;
                hits.push_back(h);
                hit({x0, cy - d.px(24)}, {x1, ty - d.px(12)}, -1, [this, i] { edit(i); });
            }
            break;
        }
        case Type::Text: {
            mini::string lbl = display(s, v);
            ImVec2 ls = d.measure(d.reg, fs, lbl);
            float w = mini::min(ls.x, x1 - x0);
            d.text_fit(d.reg, fs, {x1 - w, cy - d.px(7) - ts.y / 2}, A(v.kind == Value::Auto ? pal::dim : foc ? pal::text : pal::body), lbl, x1 - x0, true);
            mini::string sub = dev == Device::Gamepad ? "Press " + ok_name() + " to edit" : mini::string("Click or Enter to edit");
            ImVec2 ss = d.measure(d.reg, d.fs(13), sub);
            d.text(d.reg, d.fs(13), {x1 - ss.x, cy + d.px(13) - ss.y / 2}, A(foc ? pal::dim : pal::faint), sub);
            if (live) hit({x0, cy - d.px(24)}, {x1, cy + d.px(24)}, -1, [this, i] { edit(i); });
            break;
        }
        case Type::Pad:
        case Type::Key: {
            // key caps, right aligned: what this Switch button is on now
            bool pad = s.type == Type::Pad;
            mini::vector<mini::string> l = bind_list(v.text, pad);
            float cfs = d.fs(14.5f), chh = d.px(28), gap = d.px(6);
            float x = x1, ccy = cy - d.px(7);
            if (l.empty()) {
                ImVec2 ls = d.measure(d.reg, fs, "Not bound");
                d.text(d.reg, fs, {x1 - ls.x, ccy - ls.y / 2}, A(pal::gold), "Not bound");
            }
            for (int k = (int)l.size() - 1; k >= 0; k--) {
                mini::string lbl = bind_label(pad, l[k]);
                ImVec2 ls = d.measure(d.semi, cfs, lbl);
                float cw = mini::max(chh, ls.x + d.px(20));
                if (x - cw < x0 && k < (int)l.size() - 1) break;
                ImVec2 a = {x - cw, ccy - chh / 2}, b = {x, ccy + chh / 2};
                d.rect(a, b, A(foc ? pal::raised : pal::bg), pad ? chh / 2 : d.px(6));
                d.outline(a, b, A(foc ? pal::teal : pal::line), pad ? chh / 2 : d.px(6), d.px(1.25f));
                d.text_fit(d.semi, cfs, {a.x + (cw - mini::min(ls.x, cw - d.px(12))) / 2, ccy - ls.y / 2}, A(foc ? pal::text : pal::body), lbl, cw - d.px(12));
                x -= cw + gap;
                if (k > 0) {
                    ImVec2 os = d.measure(d.reg, d.fs(13), "or");
                    if (x - os.x - gap < x0) break;
                    d.text(d.reg, d.fs(13), {x - os.x, ccy - os.y / 2}, A(pal::faint), "or");
                    x -= os.x + gap;
                }
            }
            mini::string sub = dev == Device::Gamepad ? "Press " + ok_name() + " to change" : mini::string("Click or Enter to change");
            ImVec2 ss = d.measure(d.reg, d.fs(13), sub);
            d.text(d.reg, d.fs(13), {x1 - ss.x, cy + d.px(17) - ss.y / 2}, A(foc ? pal::dim : pal::faint), sub);
            if (live) hit({x0, cy - d.px(24)}, {x1, cy + d.px(24)}, -1, [this, i] { open_bind(i); });
            break;
        }
        }
    }

    void slider_set(const Hit &h, float x) {
        const Setting &s = S[h.slider];
        double t = mini::clamp((x - h.x0) / (h.x1 - h.x0), 0.0f, 1.0f);
        double v = s.min + t * (s.max - s.min);
        v = s.min + mini::round((v - s.min) / s.step) * s.step;
        v = mini::clamp(v, s.min, s.max);
        Value cur = val(h.slider);
        if (cur.kind == Value::Normal && cur.num == v) return;
        Value nv;
        nv.num = v;
        set(h.slider, nv);
    }
    Hit drag_hit;

    void mod_button(int b, mini::string *title, mini::string *meta, ImU32 *mc) const {   // 0059: Mods tab buttons
        int on = 0;
        for (int k = 0; k < mods.nmods; k++) on += mods.mods[k].enabled;
        switch (b) {
        case B_ModFile: *title = "Install from a file\xE2\x80\xA6"; *meta = ".zip, .7z, .rar or a game file; or drop it here"; break;
        case B_ModFolder: *title = "Install a mod folder\xE2\x80\xA6"; *meta = "Copied, the original stays"; break;
        case B_ModInbox:
            *title = "Install from mods-inbox";
            *meta = inbox.empty() ? mini::string("Empty") : plural((long long)inbox.size(), "waiting", "waiting");
            if (!inbox.empty()) *mc = pal::teal;
            break;
        case B_ModAllOn: *title = "Turn all mods on"; *meta = num(on) + " of " + num(mods.nmods) + " on"; break;
        case B_ModAllOff:   // [UIT 0062] Disable Mods / Enable Mods
            if (int ch = (int)cheats_on_now().size(); on || ch) {   // [UIT 0175] the cheats count too
                *title = "Disable Mods";
                *meta = mods.nmods ? num(on) + " of " + num(mods.nmods) + " on" : mini::string();
                if (ch) *meta += (meta->empty() ? "" : " \xC2\xB7 ") + plural(ch, "cheat", "cheats") + " on";
            } else {
                int back = (int)mods_before_disable().size(), cback = (int)cheats_before_disable().size();
                bool all = mods.nmods && (mods_before_has_mods() || !cback);
                *title = "Enable Mods";
                *meta = "All off now \xC2\xB7 turns " + (back ? "the " + num(back) + " from before" : all ? "all " + num(mods.nmods) : mini::string()) +
                        (cback ? mini::string(back || all ? " and " : "") + "the " + plural(cback, "cheat", "cheats") : mini::string()) + " back on";
                *mc = pal::teal;
            }
            break;
        case B_ModOpen: *title = "Open the mods folder"; *meta = mods_dir(); *mc = pal::faint; break;
        default: break;
        }
    }

    void settings_tab(float W, float H) {
        float top = d.px(76), bottom = H - d.px(58);
        float lx0 = d.px(64), lx1 = mini::floor(W * 0.585f);
        bool live = modal.kind == Modal::None;
        // heading
        float y = top + d.px(34), ty = y;
        ImVec2 tsz = d.measure(d.semi, d.fs(30), kTabNames[tab]);
        y += d.text(d.semi, d.fs(30), {lx0, y}, pal::text, kTabNames[tab]) + d.px(4);
        // [UIT 0251] the search field, on the title's line at the right of the list. [UIT 0300] centred on the title
        // row, and its width (+ a gap) is kept free on the right of the title and of the description below it: the
        // description wraps in what is left (more lines on a narrow window; the list then starts lower)
        float fh = d.px(40), gap = d.px(24);
        float fx0 = mini::max(lx0 + tsz.x + d.px(28), lx1 - d.px(340)), fy0 = mini::floor(ty + tsz.y / 2 - fh / 2);
        search_field({fx0, fy0}, {lx1, fy0 + fh});
        float hx1 = mini::max(lx0 + d.px(120), fx0 - gap), hy1 = y;
        if (searching()) { search_status(lx0, y, hx1); hy1 = y + d.measure(d.semi, d.fs(15.5f), "Ag").y; }
        else hy1 = y + d.text(d.reg, d.fs(15.5f), {lx0, y}, pal::dim, kTabBlurb[tab], hx1 - lx0);
        hy1 = mini::max(hy1, fy0 + fh);
        float ly0 = mini::max(top + d.px(122), mini::floor(hy1 + d.px(29))), ly1 = bottom - d.px(16);
        float vh = ly1 - ly0;

        // positions
        mini::vector<float> ys(items.size());
        float total = 0;
        for (size_t k = 0; k < items.size(); k++) {
            if (items[k].kind == Item::Heading && k > 0) total += d.px(14);
            if (items[k].kind == Item::Group && k > 0 && items[k - 1].kind != Item::Group) total += d.px(14);
            ys[k] = total;
            total += d.px(items[k].h) + (items[k].kind == Item::Group ? d.px(8) : 0);
        }
        total += d.px(24);
        int f = focus[tab];
        float &sc = scroll[tab], &st = scroll_to[tab];
        float maxs = mini::max(0.0f, total - vh);
        if (follow && sfield) st = 0;   // [UIT 0251] the field: the results from the top
        else if (follow && !items.empty()) {
            float fy0 = ys[f], fy1 = ys[f] + d.px(items[f].h), margin = d.px(96);
            // a heading right above the focus scrolls in with it
            if (f > 0 && items[f - 1].kind == Item::Heading) fy0 = ys[f - 1];
            if (fy0 - margin < st) st = fy0 - margin;
            if (fy1 + margin > st + vh) st = fy1 + margin - vh;
        }
        st = mini::clamp(st, 0.0f, maxs);
        sc = ease(sc, st, 14);
        sc = mini::clamp(sc, 0.0f, maxs);

        d.dl->PushClipRect({lx0 - d.px(24), ly0}, {lx1 + d.px(8), ly1}, true);
        float rx0 = lx0 - d.px(16), rx1 = lx1;
        // focus highlight eases between rows
        if (!items.empty()) {
            ImVec4 to = {rx0, ly0 + ys[f] - sc, rx1 - rx0, d.px(items[f].h)};
            if (!frect_valid || snap) frect = to, frect_valid = true;
            frect.x = ease(frect.x, to.x, 22), frect.y = ease(frect.y, to.y, 22);
            frect.z = ease(frect.z, to.z, 22), frect.w = ease(frect.w, to.w, 22);
        }
        float cx1 = rx1 - d.px(20), cx0 = cx1 - ctrl_w();
        for (size_t k = 0; k < items.size(); k++) {
            Item &it = items[k];
            float y0 = ly0 + ys[k] - sc, h = d.px(it.h), y1 = y0 + h;
            if (y1 < ly0 - d.px(40) || y0 > ly1 + d.px(40)) continue;
            bool foc = (int)k == f && !sfield;   // [UIT 0251] the search field has the focus instead
            if (it.kind == Item::Heading) {
                ImVec2 ts = d.measure(d.semi, d.fs(12.5f), "A");
                mini::string up = it.label;
                for (auto &c : up) c = (char)toupper((unsigned char)c);
                d.text_spaced(d.semi, d.fs(12.5f), {lx0, y1 - d.px(14) - ts.y}, pal::faint, up, d.px(1.6f));
                continue;
            }
            if (it.kind == Item::Group) {
                bool open = expanded.count(it.gid) > 0;
                ImVec2 a = {rx0, y0}, b = {rx1, y1};
                d.rect(a, b, foc ? pal::focus : pal::panel, d.px(8));
                if (foc) d.rect({a.x, a.y + d.px(10)}, {a.x + d.px(3), b.y - d.px(10)}, pal::teal, d.px(1.5f));
                float cy = (y0 + y1) / 2;
                d.chevron({lx0 + d.px(6), cy}, d.px(5.5f), foc ? pal::teal : pal::dim, open ? 1 : 0);
                ImVec2 ts = d.measure(d.semi, d.fs(17), "Ag");
                d.text(d.semi, d.fs(17), {lx0 + d.px(26), cy - ts.y / 2}, foc ? pal::text : pal::body, it.label);
                int changed = 0, n = group_count(it.gid, &changed);
                mini::string meta = mini::to_string(n) + (n == 1 ? " setting" : " settings");
                if (changed) meta += " \xC2\xB7 " + mini::to_string(changed) + " changed";
                ImVec2 ms = d.measure(d.reg, d.fs(14), meta);
                d.text(d.reg, d.fs(14), {cx1 - ms.x, cy - ms.y / 2}, changed ? pal::teal : pal::dim, meta);
                if (live) {
                    mini::string g = it.gid;
                    int kk = (int)k;
                    hit(a, b, kk, [this, g, kk] { focus[tab] = kk; toggle_group(g); });
                }
                continue;
            }
            if (it.kind == Item::Note && it.idx == 2) {   // [UIT 0251] the search found nothing on this tab
                bool other = false;
                for (int t = 1; t < TAB_ABOUT; t++) other |= sother[t] > 0;
                d.text_fit(d.semi, d.fs(17.5f), {lx0 + d.px(6), y0 + d.px(10)}, pal::body,
                           "No settings match \xE2\x80\x9C" + query[tab] + "\xE2\x80\x9D", rx1 - lx0 - d.px(36));
                d.text(d.reg, d.fs(14.5f), {lx0 + d.px(6), y0 + d.px(38)}, pal::dim,
                       other ? mini::string("Other tabs have some:") : mini::string("Try fewer words or part of a word. Names, descriptions and cfg keys (TOTK_\xE2\x80\xA6) are searched."),
                       rx1 - lx0 - d.px(36));
                continue;
            }
            if (it.kind == Item::Note) {          // 0059: Mods tab notes (no mods yet; Load mods is off)
                float cy = (y0 + y1) / 2;
                mini::string t = it.idx == 1 ? mini::string("Load mods is off (Mod settings, below): the game starts with its own files. The list still saves.")
                                             : "No mods yet. Install one above, drop a .zip, .7z, .rar or mod folder on this window, or put mod "
                                               "folders (with their romfs folder) in " + mods_dir();
                ImU32 c = it.idx == 1 ? pal::gold : pal::dim;
                if (it.idx == 1) d.dl->AddCircleFilled({lx0 + d.px(6), y0 + d.px(18)}, d.px(3.5f), pal::gold, 12);
                d.text(d.reg, d.fs(15), {lx0 + d.px(it.idx == 1 ? 22 : 6), y0 + d.px(it.idx == 1 ? 8 : 12)}, c, t, rx1 - lx0 - d.px(36));
                continue;
            }
            if (it.kind == Item::Mod) {           // 0059: one mod: priority, name, what it changes, the on/off switch
                int mk = it.idx;
                const ModfsMod &m = mods.mods[mk];
                bool en = m.enabled, grabbed = grab == mk;
                float fade = en ? 1.0f : 0.5f;
                ImVec2 a = {rx0, y0}, b = {rx1, y1};
                bool hov = live && hovered(a, b);
                if (foc) {
                    ImVec2 fa = {frect.x, frect.y}, fb = {frect.x + frect.z, frect.y + frect.w};
                    d.rect(fa, fb, pal::focus, d.px(8));
                    d.rect({fa.x, fa.y + d.px(14)}, {fa.x + d.px(3), fb.y - d.px(14)}, pal::teal, d.px(1.5f));
                }
                if (grabbed) d.outline({a.x + 1, a.y + 1}, {b.x - 1, b.y - 1}, pal::teal, d.px(8), d.px(2));
                if (live) { int kk = (int)k; hit({rx0, mini::max(y0, ly0)}, {rx1, mini::min(y1, ly1)}, kk, [this, kk] { focus[tab] = kk; }); }
                float cy = (y0 + y1) / 2;
                mini::string pos = num(mk + 1);
                ImVec2 ps = d.measure(d.semi, d.fs(15), pos);
                d.text(d.semi, d.fs(15), {lx0 + d.px(16) - ps.x / 2, cy - ps.y / 2}, grabbed ? pal::teal : pal::faint, pos);
                float tx = lx0 + d.px(38);
                // the switch
                float tw = d.px(46), th = d.px(26);
                ImVec2 sa = {cx1 - tw, cy - th / 2}, sb = {cx1, cy + th / 2};
                float &kn = knob[1000000 + mk];
                float to = en ? 1.0f : 0.0f;
                if (!knob.count(-1000001 - mk) || snap) kn = to, knob[-1000001 - mk] = 1;
                kn = ease(kn * 100, to * 100, 18) / 100;
                d.rect(sa, sb, mix(pal::line, pal::teal, kn), th / 2);
                d.dl->AddCircleFilled({sa.x + th / 2 + (tw - th) * kn, cy}, th / 2 - d.px(3.5f), mix(pal::dim, pal::ink, kn), 24);
                if (live) hit({sa.x - d.px(8), cy - d.px(20)}, {sb.x + d.px(4), cy + d.px(20)}, -1, [this, mk] { mod_set(mk, !mods.mods[mk].enabled); });
                float right = sa.x - d.px(16);
                // move arrows (mouse): on the row under the mouse or the focused one
                if (live && dev == Device::Keyboard && (hov || foc) && mods.nmods > 1) {
                    for (int dir_ = 1; dir_ >= -1; dir_ -= 2) {   // placed right to left: reads up, down
                        bool can = dir_ < 0 ? mk > 0 : mk < mods.nmods - 1;
                        ImVec2 c = {right - d.px(14), cy};
                        ImVec2 ha = {c.x - d.px(14), c.y - d.px(16)}, hb = {c.x + d.px(14), c.y + d.px(16)};
                        bool hv = can && hovered(ha, hb);
                        if (hv) d.rect(ha, hb, pal::raised, d.px(6));
                        d.chevron(c, d.px(5.5f), !can ? alpha(pal::faint, 0.4f) : hv ? pal::teal : pal::dim, dir_ < 0 ? 3 : 1);
                        if (can) hit(ha, hb, -1, [this, mk, dir_] { mod_move(mk, dir_); });
                        right -= d.px(32);
                    }
                    right -= d.px(6);
                }
                // name, version
                float nameY = y0 + d.px(14), tw2 = right - tx - d.px(8);
                mini::string title = mod_title(mk);
                ImVec2 ns = d.measure(d.semi, d.fs(17.5f), title);
                search_marks(d.semi, d.fs(17.5f), {tx, nameY}, title, tw2);
                d.text_fit(d.semi, d.fs(17.5f), {tx, nameY}, alpha(foc ? pal::text : pal::body, fade), title, tw2);
                if (m.version[0] && ns.x + d.px(90) < tw2)
                    d.text_fit(d.reg, d.fs(14), {tx + ns.x + d.px(10), nameY + d.px(3)}, pal::faint, m.version, tw2 - ns.x - d.px(10));
                // what it does, the most important thing first
                mini::string sub;
                ImU32 sc2 = pal::dim;
                unsigned code = m.unsupported & (MODFS_CODE | MODFS_U_CHEATS | MODFS_U_FPSRES | MODFS_U_DLL);
                if (grabbed) sub = "Moving: up / down, then drop it", sc2 = pal::teal;
                else if (m.unsupported & MODFS_U_EMPTY) sub = code ? "Code patches only: nothing here can load" : "No game files in it: nothing to load", sc2 = pal::gold;
                else {
                    sub = plural(m.nfiles, "file", "files") + " \xC2\xB7 " + fmt_bytes(m.bytes);
                    if (!en) sub = "Off \xC2\xB7 " + sub;
                    else {
                        if (m.nlost) sub += " \xC2\xB7 " + num(m.nlost) + " replaced by a higher mod";
                        else if (m.nwon) sub += " \xC2\xB7 wins " + num(m.nwon) + " over lower mods";
                    }
                    if (code) sub += " \xC2\xB7 its code patches cannot load", sc2 = pal::gold;
                }
                if (!en && !grabbed && sc2 == pal::dim) sc2 = pal::faint;
                d.text_fit(d.reg, d.fs(14.5f), {tx, nameY + d.px(26)}, sc2, sub, tw2);
                continue;
            }
            if (it.kind == Item::Button && it.idx != B_ResetGroup) {   // 0059: the Mods tab's actions
                ImVec2 a = {rx0 + d.px(6), y0 + d.px(8)}, b = {rx1, y1 - d.px(8)};
                bool hov = live && hovered(a, b);
                d.rect(a, b, foc ? pal::focus : hov ? mix(pal::bg, pal::focus, 0.5f) : pal::bg, d.px(8));
                d.outline(a, b, foc ? pal::teal : pal::line, d.px(8), d.px(1.25f));
                float cy = (y0 + y1) / 2;
                mini::string title, meta;
                ImU32 mc = pal::dim;
                if (it.idx == B_SearchIn) title = it.label, meta = "The same words there";
                else mod_button(it.idx, &title, &meta, &mc);
                ImVec2 ic = {lx0 + d.px(12), cy};
                ImU32 icol = foc ? pal::teal : pal::body;
                if (it.idx == B_ModFile || it.idx == B_ModFolder || it.idx == B_ModInbox) {
                    d.rect({ic.x - d.px(6), ic.y - d.px(1)}, {ic.x + d.px(6), ic.y + d.px(1)}, icol);
                    d.rect({ic.x - d.px(1), ic.y - d.px(6)}, {ic.x + d.px(1), ic.y + d.px(6)}, icol);
                } else if (it.idx == B_ModAllOn) d.check(ic, d.px(9), icol);
                else if (it.idx == B_ModAllOff) { if (mods_any_on()) d.cross(ic, d.px(7), icol); else d.check(ic, d.px(9), icol); }
                else d.chevron(ic, d.px(5), icol, 0);
                ImVec2 ts = d.measure(d.semi, d.fs(16), "Ag");
                float tx = lx0 + d.px(32);
                d.text(d.semi, d.fs(16), {tx, cy - ts.y / 2}, foc ? pal::text : pal::body, title);
                float tw = d.measure(d.semi, d.fs(16), title).x;
                float mw = mini::max(0.0f, cx1 - (tx + tw + d.px(24)));
                ImVec2 ms = d.measure(d.reg, d.fs(14), meta);
                float w2 = mini::min(ms.x, mw);
                d.text_fit(d.reg, d.fs(14), {cx1 - w2, cy - ms.y / 2}, mc, meta, w2, true);
                if (live) {
                    int kk = (int)k, bb = it.idx, tt = atoi(it.gid.c_str());
                    hit(a, b, kk, [this, kk, bb, tt] { focus[tab] = kk; if (bb == B_SearchIn) search_go(tt); else press_button(bb); });
                }
                continue;
            }
            if (it.kind == Item::Button) {        // "Reset all to defaults" at the end of a binding group
                int changed = 0;
                group_count(it.gid, &changed);
                ImVec2 a = {rx0 + d.px(6), y0 + d.px(8)}, b = {rx1, y1 - d.px(8)};
                bool hov = live && hovered(a, b);
                d.rect(a, b, foc ? pal::focus : hov ? mix(pal::bg, pal::focus, 0.5f) : pal::bg, d.px(8));
                d.outline(a, b, foc ? pal::teal : pal::line, d.px(8), d.px(1.25f));
                float cy = (y0 + y1) / 2;
                d.reset_icon({lx0 + d.px(12), cy}, d.px(6.5f), changed ? (foc ? pal::teal : pal::body) : pal::faint);
                ImVec2 ts = d.measure(d.semi, d.fs(16), "Ag");
                d.text(d.semi, d.fs(16), {lx0 + d.px(32), cy - ts.y / 2}, changed ? (foc ? pal::text : pal::body) : pal::dim,
                       "Reset " + lc_first(group_label(it.gid)) + " to defaults");
                mini::string meta = changed ? mini::to_string(changed) + " changed" : mini::string("All at the defaults");
                ImVec2 ms = d.measure(d.reg, d.fs(14), meta);
                d.text(d.reg, d.fs(14), {cx1 - ms.x, cy - ms.y / 2}, changed ? pal::teal : pal::faint, meta);
                if (live) {
                    mini::string g = it.gid;
                    int kk = (int)k;
                    hit(a, b, kk, [this, g, kk] { focus[tab] = kk; reset_group(g); });
                }
                continue;
            }
            // a setting row
            int i = it.idx;
            const Setting &s = S[i];
            bool on = active(i);
            float fade = on ? 1.0f : 0.45f;
            if (foc) {
                ImVec2 a = {frect.x, frect.y}, b = {frect.x + frect.z, frect.y + frect.w};
                d.rect(a, b, pal::focus, d.px(8));
                d.rect({a.x, a.y + d.px(14)}, {a.x + d.px(3), b.y - d.px(14)}, pal::teal, d.px(1.5f));
            }
            if (live) {
                int kk = (int)k;
                hit({rx0, mini::max(y0, ly0)}, {rx1, mini::min(y1, ly1)}, kk, [this, kk, i] {
                    focus[tab] = kk;
                    if (S[i].type == Type::Bool && active(i)) { Value v = val(i); v.b = !v.b; set(i, v); }
                    if (is_bind(i) && active(i)) open_bind(i);
                });
            }
            float tx = lx0 + d.px(6);
            float nameY = y0 + d.px(15);
            bool mod = modified(i);
            if (mod) d.dl->AddCircleFilled({lx0 - d.px(6), nameY + d.px(10)}, d.px(3), pal::teal, 12);
            float tw = cx0 - tx - d.px(24);
            // [UIT 0304] "restart" after the name: the game reads it when it starts (the in-game overlay cannot change it)
            mini::string tag = applies_tag(s);
            float tagw = tag.empty() ? 0 : d.measure(d.reg, d.fs(12), tag).x + d.px(14), nw = tw - (tag.empty() ? 0 : tagw + d.px(10));
            search_marks(d.semi, d.fs(17.5f), {tx, nameY}, s.name, nw);
            d.text_fit(d.semi, d.fs(17.5f), {tx, nameY}, alpha(foc ? pal::text : pal::body, fade), s.name, nw);
            if (!tag.empty()) {
                float gx = tx + mini::min(d.measure(d.semi, d.fs(17.5f), s.name).x, nw) + d.px(10), gh = d.px(18);
                ImVec2 ts = d.measure(d.reg, d.fs(12), tag);
                float gy = nameY + (d.measure(d.semi, d.fs(17.5f), "Ag").y - gh) / 2;
                d.outline({gx, gy}, {gx + tagw, gy + gh}, alpha(pal::faint, fade), gh / 2, d.px(1));
                d.text(d.reg, d.fs(12), {gx + d.px(7), gy + (gh - ts.y) / 2}, alpha(pal::dim, fade), tag);
            }
            mini::string sub = on ? s.summary : needs_note(i);
            d.text_fit(d.reg, d.fs(14.5f), {tx, nameY + d.px(26)}, alpha(on ? pal::dim : pal::faint, on ? 1.0f : 0.9f), sub, tw);
            control(i, cx0, cx1, (y0 + y1) / 2, foc, fade, live && on);
            // reset icon for a changed row under the mouse (the keyboard/pad use X / Del)
            if (mod && live && dev == Device::Keyboard && hovered({rx0, y0}, {rx1, y1})) {
                ImVec2 c = {cx0 - d.px(22), (y0 + y1) / 2};
                bool hv = hovered({c.x - d.px(12), c.y - d.px(12)}, {c.x + d.px(12), c.y + d.px(12)});
                d.reset_icon(c, d.px(7), hv ? pal::teal : pal::dim);
                hit({c.x - d.px(12), c.y - d.px(12)}, {c.x + d.px(12), c.y + d.px(12)}, -1, [this, i] {
                    set(i, default_value(S[i]));
                    flash(S[i].name + " reset to default");
                });
            }
        }
        d.dl->PopClipRect();
        if (sc > 1) d.rect({lx0 - d.px(16), ly0}, {lx1, ly0 + 1}, pal::line);
        if (sc < maxs - 1) d.rect({lx0 - d.px(16), ly1 - 1}, {lx1, ly1}, pal::line);
        if (maxs > 0) {   // scroll position
            float bx = lx1 + d.px(10), th = mini::max(d.px(30), vh * vh / total);
            float by = ly0 + (vh - th) * (sc / maxs);
            d.rect({bx, by}, {bx + d.px(3), by + th}, alpha(pal::faint, 0.6f), d.px(1.5f));
        }
        if (sosk || (sfield && !searching()) || (searching() && !smatch)) search_panel(lx1 + d.px(40), top + d.px(34), W - d.px(56), bottom - d.px(24));
        else detail(lx1 + d.px(40), top + d.px(34), W - d.px(56), bottom - d.px(24));
    }

    // ---------------------------------------------------------------- [UIT 0251] search: drawing
    void search_marks(ImFont *f, float size, ImVec2 p, const mini::string &text, float maxw) {   // behind the matched letters
        if (!searching()) return;
        float h = d.measure(f, size, "Ag").y;
        for (auto &r : search_spans(text, swords)) {
            float a = d.measure(f, size, text.substr(0, r.first)).x, b = d.measure(f, size, text.substr(0, r.second)).x;
            if (a >= maxw) break;
            b = mini::min(b, maxw);
            d.rect({p.x + a - d.px(1.5f), p.y - d.px(1)}, {p.x + b + d.px(1.5f), p.y + h + d.px(1)}, alpha(pal::teal, 0.26f), d.px(3));
        }
    }

    void search_field(ImVec2 a, ImVec2 b) {
        bool live = modal.kind == Modal::None, foc = sfield, hov = live && hovered(a, b);
        if (live) hit(a, b, -1, [this] { set_sfield(true); });   // first: the (x) below wins over it
        float r = d.px(8), cy = (a.y + b.y) / 2;
        d.rect(a, b, foc ? pal::focus : hov ? mix(pal::bg, pal::focus, 0.5f) : pal::bg, r);
        d.outline(a, b, foc ? pal::teal : hov ? alpha(pal::teal, 0.6f) : pal::line, r, d.px(foc ? 1.5f : 1.25f));
        ImU32 ic = foc ? pal::teal : pal::dim;   // magnifier
        ImVec2 c = {a.x + d.px(19), cy - d.px(1.5f)};
        d.dl->AddCircle(c, d.px(6), ic, 20, d.px(1.6f));
        d.dl->AddLine({c.x + d.px(4.4f), c.y + d.px(4.4f)}, {c.x + d.px(9), c.y + d.px(9)}, ic, d.px(2));
        float tx = a.x + d.px(36), right = b.x - d.px(12);
        const mini::string &q = query[tab];
        if (!q.empty()) {   // (x): clears, the field keeps (or takes) the focus
            ImVec2 xc = {b.x - d.px(20), cy};
            ImVec2 ha = {xc.x - d.px(15), a.y}, hb = b;
            bool xh = live && hovered(ha, hb);
            d.dl->AddCircleFilled(xc, d.px(10), xh ? pal::teal : pal::raised, 20);
            d.cross(xc, d.px(13), xh ? pal::ink : pal::body);
            if (live) hit(ha, hb, -1, [this] { search_set(""); set_sfield(true); });
            right = xc.x - d.px(16);
        } else if (!foc) {   // the key / button that opens it
            float gw = glyph(G_Search, 0, 0, false);
            glyph(G_Search, right - gw, cy);
            right -= gw + d.px(10);
        }
        float th = d.measure(d.reg, d.fs(16), "Ag").y, tw = 0;
        if (q.empty()) d.text_fit(d.reg, d.fs(16), {tx, cy - th / 2}, pal::faint, "Search " + mini::string(kTabNames[tab]), right - tx);
        else {
            tw = mini::min(d.measure(d.semi, d.fs(16), q).x, right - tx);
            d.text_fit(d.semi, d.fs(16), {tx, cy - th / 2}, pal::text, q, right - tx, true);
        }
        if (foc && (snap || mini::fmod(now, 1.0) < 0.55)) d.rect({tx + tw + d.px(1), cy - th / 2 + d.px(2)}, {tx + tw + d.px(3), cy + th / 2 - d.px(2)}, pal::teal);
    }

    void search_status(float x, float y, float x1) {   // replaces the blurb: how many match here, and on the other tabs
        float fs = d.fs(15.5f);
        mini::string m = smatch ? plural(smatch, "match", "matches") : mini::string("No match here");
        d.text(d.semi, fs, {x, y}, smatch ? pal::teal : pal::gold, m);
        x += d.measure(d.semi, fs, m).x;
        bool first = true;
        for (int t = 1; t < TAB_ABOUT; t++) {
            if (!sother[t]) continue;
            mini::string lead = first ? "   \xC2\xB7   Also: " : "   ";
            mini::string link = mini::string(kTabNames[t]) + " " + num(sother[t]);
            float lw = d.measure(d.reg, fs, lead).x, kw = d.measure(d.semi, fs, link).x;
            if (x + lw + kw > x1) break;
            d.text(d.reg, fs, {x, y}, pal::dim, lead);
            x += lw;
            ImVec2 la = {x, y}, lb = {x + kw, y + d.measure(d.semi, fs, "Ag").y};
            bool hv = modal.kind == Modal::None && hovered(la, lb);
            d.text(d.semi, fs, la, hv ? pal::teal : pal::body, link);
            if (hv) d.rect({la.x, lb.y}, {lb.x, lb.y + d.px(1)}, pal::teal);
            if (modal.kind == Modal::None) hit(la, lb, -1, [this, t] { search_go(t); });
            x += kw;
            first = false;
        }
    }

    // The right panel while searching with nothing to show there: how search works; with a controller, its keyboard
    // (docked here so the results on the left stay visible and filter as it types).
    void search_panel(float x0, float y0, float x1, float y1) {
        d.rect({x0, y0}, {x1, y1}, pal::panel, d.px(12));
        float pad = d.px(32), x = x0 + pad, w = x1 - x0 - pad * 2, y = y0 + pad;
        bool pad_dev = dev == Device::Gamepad;
        mini::string up = mini::string("SEARCH  \xC2\xB7  ") + kTabNames[tab];
        for (auto &c : up) c = (char)toupper((unsigned char)c);
        d.text_spaced(d.semi, d.fs(12), {x, y}, pal::faint, up, d.px(1.5f));
        y += d.px(28);
        if (!sosk) {
            y += d.text(d.semi, d.fs(27), {x, y}, pal::text, searching() ? "Nothing here" : "Find a setting", w) + d.px(14);
            mini::string body = "Type part of a setting's name, what it does, or its cfg key (TOTK_FPS). Every word has to match, in any order; "
                                "capitals and punctuation do not matter. Name matches come first, and the matching letters are marked.";
            y += d.text(d.reg, d.fs(15.5f), {x, y}, pal::body, body, w) + d.px(24);
            mini::string keys = pad_dev ? ok_name() + " opens the keyboard. Down goes to the results; Up on the first result comes back here. " +
                                              back_name() + " clears the words, " + back_name() + " again leaves. View opens the search from any tab."
                                        : mini::string("Enter or Down goes to the results; Up on the first result comes back here. Esc clears the words, Esc again "
                                          "leaves. Ctrl+F or / opens the search from any tab.");
            y = section(x, y, "KEYS", keys + " Each tab keeps its own search; folded groups open for the matches and fold back when it is cleared.", w, pal::dim);
            return;
        }
        // the typed words
        float bh = d.px(52), th = d.measure(d.semi, d.fs(20), "Ag").y;
        ImVec2 a = {x, y}, b = {x + w, y + bh};
        d.rect(a, b, pal::bg, d.px(8));
        d.outline(a, b, pal::teal, d.px(8), d.px(1.5f));
        const mini::string &q = query[tab];
        float ty = y + (bh - th) / 2, tw = 0;
        if (q.empty()) d.text_fit(d.reg, d.fs(18), {x + d.px(18), ty + d.px(1)}, pal::faint, "Search " + mini::string(kTabNames[tab]), w - d.px(40));
        else {
            tw = mini::min(d.measure(d.semi, d.fs(20), q).x, w - d.px(40));
            d.text_fit(d.semi, d.fs(20), {x + d.px(18), ty}, pal::text, q, w - d.px(40), true);
        }
        if (snap || mini::fmod(now, 1.0) < 0.55) d.rect({x + d.px(19) + tw, ty + d.px(2)}, {x + d.px(21) + tw, ty + th - d.px(2)}, pal::teal);
        y += bh + d.px(10);
        mini::string m = !searching() ? mini::string("Results show on the left as you type") : smatch ? plural(smatch, "match", "matches") + " on this tab" : mini::string("No match on this tab");
        int others = 0;
        for (int t = 1; t < TAB_ABOUT; t++) others += sother[t];
        if (searching() && others) m += " \xC2\xB7 " + num(others) + " on other tabs";
        d.text_fit(d.reg, d.fs(14.5f), {x, y}, searching() && !smatch ? pal::gold : pal::faint, m, w);
        y += d.px(30);
        // the keys, at the bottom of the panel
        float kg = d.px(6), kh = d.px(46), kw = mini::min(d.px(56), (w - 9 * kg) / 10);
        float gw = 10 * kw + 9 * kg, gx = x + (w - gw) / 2;
        float ky = mini::max(y + d.px(8), y1 - pad - 5 * kh - 4 * kg);
        for (int r = 0; r < 5; r++) {
            int cols = r == SOSK_SPECIAL_ROW ? 4 : 10;
            float w2 = r == SOSK_SPECIAL_ROW ? (gw - 3 * kg) / 4 : kw;
            for (int c = 0; c < cols; c++) {
                ImVec2 ka = {gx + c * (w2 + kg), ky}, kb = {ka.x + w2, ky + kh};
                bool foc = skr == r && skc == c;
                d.rect(ka, kb, foc ? pal::teal : pal::raised, d.px(7));
                mini::string lbl = r == SOSK_SPECIAL_ROW ? kSearchSpecial[c] : mini::string(1, kSearchRows[r][c]);
                ImFont *f = r == SOSK_SPECIAL_ROW ? d.semi : d.reg;
                float fs = r == SOSK_SPECIAL_ROW ? d.fs(14.5f) : d.fs(18);
                ImVec2 ls = d.measure(f, fs, lbl);
                d.text(f, fs, {ka.x + (w2 - ls.x) / 2, ka.y + (kh - ls.y) / 2}, foc ? pal::ink : pal::body, lbl);
                int rr = r, cc = c;
                hit(ka, kb, -1, [this, rr, cc] { skr = rr, skc = cc; search_key(); });
            }
            ky += kh + kg;
        }
    }

    float section(float x, float y, const char *label, const mini::string &body, float w, ImU32 col = pal::body) {
        if (body.empty()) return y;
        d.text_spaced(d.semi, d.fs(12), {x, y}, pal::faint, label, d.px(1.5f));
        y += d.px(22);
        y += d.text(d.reg, d.fs(15.5f), {x, y}, col, body, w);
        return y + d.px(20);
    }

    float badge(float x, float y, const mini::string &t, ImU32 fg, ImU32 bg) {
        ImVec2 ts = d.measure(d.semi, d.fs(12.5f), t);
        float w = ts.x + d.px(20), h = d.px(24);
        d.rect({x, y}, {x + w, y + h}, bg, h / 2);
        d.text(d.semi, d.fs(12.5f), {x + d.px(10), y + (h - ts.y) / 2}, fg, t);
        return w;
    }

    // 0059: the Mods tab's right panel: one mod, or what a Mods button does
    void mod_detail(int k, float x, float y, float w, float y1) {
        const ModfsMod &m = mods.mods[k];
        bool pad_dev = dev == Device::Gamepad;
        int n = mods.nmods;
        d.text_fit(d.semi, d.fs(12), {x, y}, pal::faint, "MODS  \xC2\xB7  LOAD ORDER  \xC2\xB7  " + num(k + 1) + " OF " + num(n), w);
        y += d.px(28);
        y += d.text(d.semi, d.fs(27), {x, y}, pal::text, mod_title(k), w) + d.px(6);
        mini::string by;
        if (m.version[0]) by = mini::string("Version ") + m.version;
        if (m.author[0]) by += (by.empty() ? "By " : " \xC2\xB7 by ") + mini::string(m.author);
        if (m.title[0] && SDL_strcmp(m.title, m.name)) by += (by.empty() ? "" : " \xC2\xB7 ") + mini::string("folder ") + m.name;
        if (!by.empty()) y += d.text(d.reg, d.fs(14.5f), {x, y}, pal::dim, by, w) + d.px(8);
        float bx = x;
        bx += badge(bx, y, m.enabled ? "On" : "Off", m.enabled ? pal::ink : pal::dim, m.enabled ? pal::teal : pal::raised) + d.px(8);
        bx += badge(bx, y, k == 0 ? "Top priority" : "Priority " + num(k + 1), pal::body, pal::raised) + d.px(8);
        if (m.unsupported & (MODFS_CODE | MODFS_U_CHEATS | MODFS_U_DLL)) bx += badge(bx, y, "Code patches: not loaded", pal::gold, pal::goldSoft) + d.px(8);
        if (m.nrestbl && bx + d.px(120) < x + w) bx += badge(bx, y, "Size table", pal::teal, pal::tealSoft) + d.px(8);
        y += d.px(24) + d.px(18);
        auto fits = [&] { return y < y1 - d.px(70); };
        if (!mods_on()) {
            y += d.text(d.reg, d.fs(15), {x, y}, pal::gold, "Load mods is off (Mod settings, below the list): no mod loads until it is on.", w) + d.px(14);
        }
        if (m.description[0] && fits()) {
            mini::string ds = m.description;
            if (ds.size() > 320) ds = ds.substr(0, 317) + "\xE2\x80\xA6";
            y = section(x, y, "ABOUT", ds, w);
        }
        if (m.unsupported && fits()) y = section(x, y, "CANNOT LOAD", modfs_unsupported_text(m.unsupported), w, pal::gold);
        if (k < (int)mods_placed.size() && !mods_placed[k].empty() && fits()) {
            mini::vector<mini::string> ps;
            for (auto &l : split(mods_placed[k], '\n')) { mini::string q = l; while (!q.empty() && (q.back() == '\r' || q.back() == ' ')) q.pop_back(); if (!q.empty()) ps.push_back(q); }
            if (!ps.empty()) y = section(x, y, "PLACED BY FILE NAME", placed_text(ps, 3) + " It came as a loose file, so it went to the one game path with that name.", w);
        }
        mini::string files = m.nfiles ? plural(m.nfiles, "file", "files") + ", " + fmt_bytes(m.bytes) + " (its romfs folder)."
                                       : mini::string("No RomFS files, so nothing in it loads.");
        if (m.nnew > 0) {
            files += " " + num(m.nnew) + (m.nnew == 1 ? " is a file" : " are files") + " the game does not have: normal for added content, but a mod that "
                     "only replaces files and shows this may be for another game version (this is 1.4.3).";
            int shown_n = 0;
            for (size_t f = 0; f < mods.nfiles && shown_n < 2; f++)
                if (mods.files[f].mod == k && mods.files[f].vanilla == 0) files += mini::string(shown_n++ ? ", " : " For example ") + mods.files[f].path;
            if (shown_n) files += ".";
        } else if (m.nnew == 0 && m.nfiles) files += " The game has every one of them: it replaces files, adds none.";
        if (m.nrestbl) files += setting_text("TOTK_MODS_RESTBL") == "0" ? " It ships a resource size table: used only when it is the top mod that ships one."
                                                                         : " It ships a resource size table: merged at start with the game's and other mods' tables.";
        if (fits()) y = section(x, y, "FILES", files, w);
        mini::string cf;
        if (!m.enabled) cf = "Off: none of its files load, so it takes no files from other mods.";
        else {
            int lines = 0;
            for (int j = 0; j < n && lines < 4; j++)
                if (j != k && mods.pairs && mods.pairs[k * n + j]) cf += (lines++ ? "\n" : "") + mini::string("Wins ") + plural(mods.pairs[k * n + j], "file", "files") + " over " + mod_title(j);
            for (int j = 0; j < n && lines < 6; j++)
                if (j != k && mods.pairs && mods.pairs[j * n + k]) cf += (lines++ ? "\n" : "") + mini::string("Loses ") + plural(mods.pairs[j * n + k], "file", "files") + " to " + mod_title(j) + " (higher)";
            if (cf.empty()) cf = "Shares no files with the other mods that are on.";
        }
        if (fits()) y = section(x, y, "CONFLICTS", cf, w);
        if (fits()) y = section(x, y, "CHANGE", pad_dev ? ok_name() + " turns it on or off. " + face_name("north") + " picks it up: up / down move it, " + ok_name() +
                                                              " drops it. " + face_name("west") + " uninstalls it."
                                                        : mini::string("Enter or the switch turns it on or off. Ctrl+Up / Ctrl+Down or the arrows on the row move it (M picks it up). Del uninstalls it."), w, pal::dim);
        if (y < y1 - d.px(24)) d.text_fit(d.reg, d.fs(13.5f), {x, y1 - d.px(20)}, pal::faint, join(mods_loaded, m.name), w, true);
    }
    void mods_button_detail(const Item &it, float x, float y, float w, float y1) {
        mini::string title, meta, body, more, more_t;
        ImU32 mc = pal::dim;
        mod_button(it.idx, &title, &meta, &mc);
        switch (it.idx) {
        case B_ModFile:
            body = "Choose one or more mod downloads: .zip, .7z or .rar (or .tar). The launcher finds the mod's files wherever the archive keeps "
                   "them (a romfs folder at any depth, or the game's own folders such as Actor and UI), installs each mod into the mods folder "
                   "and puts it on top of the load order, turned on. Updating a mod already installed keeps its place. A damaged or "
                   "incomplete archive changes nothing and says so. A mod that is a single game file with no folders (some UI and "
                   "screen fixes) goes where the game keeps the file of that name.";
            more_t = "ALSO", more = "Drop files or folders from Explorer onto this window to install them the same way. A .7z or .rar is read by "
                                    "Windows' own tar, or by 7-Zip when that is installed. Nothing is ever downloaded.\n"
                                    "Code patches (exefs, .ips, .pchtxt) and cheats cannot load: they change the Switch program, which the "
                                    "recompiled PC game does not run. The launcher says so instead of installing them. Frame rate and "
                                    "resolution are settings here instead (Performance > Frame rate, Graphics > Image quality).";
            break;
        case B_ModFolder:
            body = "Choose an unpacked mod folder: it is copied into the mods folder (the original stays where it is) and goes on top of the "
                   "load order, turned on. Any of the usual layouts works: the mod's own folder, its romfs folder, an emulator layout "
                   "(atmosphere\\contents\\<title id>\\romfs, or Yuzu and Ryujinx folders), or a folder of mods.";
            break;
        case B_ModInbox:
            body = "For a controller: copy mod downloads (.zip, .7z, .rar or folders) into\n" + inbox_dir() +
                   "\nthen press " + (dev == Device::Gamepad ? ok_name() : mini::string("Enter")) + " here. They install oldest first, so the newest ends on "
                   "top, then move to installed\\ (or failed\\ with a note that says why).";
            if (!inbox.empty()) {
                more_t = "WAITING";
                for (size_t k = 0; k < inbox.size() && k < 6; k++) more += (k ? "\n" : "") + inbox[k];
                if (inbox.size() > 6) more += "\n\xE2\x80\xA6";
            }
            break;
        case B_ModAllOn: body = "Turns every mod on. Their order stays as it is."; break;
        case B_ModAllOff:
            if (mods_any_on() || !cheats_on_now().empty()) {
                title = "Disable Mods";
                body = "Turns every mod off in one press, without uninstalling any, and the built-in cheats (Game > Cheats) too. The next "
                       "Play starts the unmodified game: it reads only "
                       "its own files and its own resource size table, exactly as with no mods installed. Use it to rule mods out when "
                       "something looks wrong.\n\nEnable Mods (the same button afterwards) turns back on exactly the mods and cheats that "
                       "are on now, in the same order.";
            } else {
                auto back = mods_before_disable();
                auto cback = cheats_before_disable();
                if (!cback.empty()) {
                    more_t = "TURNS BACK ON";
                    for (auto &c : cback) {   // [UIT 0344] its setting's name
                        int ci = idx(c.c_str());
                        more += mini::string(more.empty() ? "" : "\n") + "Cheat: " + (ci >= 0 ? S[ci].name : c);
                    }
                }
                title = "Enable Mods";
                body = "No mod is on, so Play starts the unmodified game.\n\n" +
                       (back.empty() ? "Enable Mods turns every installed mod on. Their order stays as it is."
                                     : mini::string("Enable Mods turns back on the mods that were on when you pressed Disable Mods. Their order stays as it is.")) ;
                if (!back.empty()) {
                    more_t = "TURNS BACK ON";
                    for (size_t k = 0; k < back.size() && k < 8; k++) more += (more.empty() ? "" : "\n") + back[k];
                    if (back.size() > 8) more += "\n\xE2\x80\xA6";
                }
            }
            body += mini::string("\n\n") + (dev == Device::Gamepad ? "Press " + ok_name() + " here." : mini::string("Press Enter here, or click it."));
            if (!mods_on()) body += "\n\nLoad mods (Mod settings, below) is Off, so the game is unmodified either way.";
            break;
        case B_ModOpen:
            body = "Opens\n" + mods_dir() + "\nEach mod is a folder there: <Name>\\romfs\\... with the game's own paths. load-order.txt "
                   "keeps the order and which mods are on; a folder it does not list is on and goes on top. README.txt there (written at the "
                   "first install) explains it all. The folder is set in Mod settings below.";
            break;
        default: return;
        }
        d.text_fit(d.semi, d.fs(12), {x, y}, pal::faint, it.idx == B_ModAllOn ? "MODS  \xC2\xB7  LOAD ORDER" : it.idx == B_ModAllOff ? "MODS  \xC2\xB7  UNMODIFIED GAME" : "MODS  \xC2\xB7  INSTALL", w);
        y += d.px(28);
        y += d.text(d.semi, d.fs(27), {x, y}, pal::text, title, w) + d.px(14);
        y += d.text(d.reg, d.fs(15.5f), {x, y}, pal::body, body, w) + d.px(24);
        if (!more.empty() && y < y1 - d.px(60)) y = section(x, y, more_t.c_str(), more, w, pal::dim);
    }

    void detail(float x0, float y0, float x1, float y1) {
        d.rect({x0, y0}, {x1, y1}, pal::panel, d.px(12));
        float pad = d.px(32), x = x0 + pad, w = x1 - x0 - pad * 2, y = y0 + pad;
        if (items.empty()) return;
        Item &it = items[focus[tab]];
        mini::string crumb = mini::string(kTabNames[tab]);
        if (it.kind == Item::Group || (it.kind == Item::Button && it.idx == B_ResetGroup)) {
            mini::vector<int> members;
            int changed = 0, n = group_count(it.gid, &changed, &members);
            mini::string up = crumb;
            for (auto &c : up) c = (char)toupper((unsigned char)c);
            d.text_spaced(d.semi, d.fs(12), {x, y}, pal::faint, up, d.px(1.5f));
            y += d.px(28);
            mini::string title = it.kind == Item::Group ? it.label : "Reset " + lc_first(group_label(it.gid));
            y += d.text(d.semi, d.fs(27), {x, y}, pal::text, title, w) + d.px(10);
            bool open = expanded.count(it.gid) > 0;
            bool binds = !members.empty() && is_bind(members[0]);
            bool pad_dev = dev == Device::Gamepad;
            mini::string body;
            if (it.kind == Item::Button)
            {
                const char *what = !members.empty() && S[members[0]].type == Type::Key ? "key" : "button";
                body = changed ? mini::string("Puts every ") + what + " in this group back to the game's default (it asks first and lists the changes)."
                               : mini::string("Every ") + what + " in this group is at the game's default.";
            }
            else if (binds) {
                bool pad = S[members[0]].type == Type::Pad;
                body = mini::string(pad ? "Which controller button presses each Switch button in the game." : "Which key presses each Switch button in the game (keyboard play).") +
                       " Pick a Switch button, press " + (pad_dev ? ok_name() : mini::string("Enter or click")) + ", then press the new " + (pad ? "button" : "key") +
                       ". A " + (pad ? "button" : "key") + " that another Switch button uses swaps with it. ";
                body += open ? (pad_dev ? face_name("west") : mini::string("Del")) + " on this heading resets the whole group." : (pad_dev ? "Press " + ok_name() : mini::string("Press Enter or click")) + " to show them.";
            } else if (is_sec(it.gid)) {
                body = mini::string(group_label(it.gid) == "Stutter" ? "Shader warm-up and texture streaming, the two causes of hitches (this was the Stutter tab). " : "") +
                       mini::to_string(n) + " settings in this section. ";
                body += open ? "They are shown below this heading." : (pad_dev ? "Press " + ok_name() : mini::string("Press Enter or click")) + " to show them.";
            } else {
                body = mini::to_string(n) + " less common settings, folded away to keep this page short. ";
                body += open ? "They are shown below this heading." : (pad_dev ? "Press " + ok_name() : mini::string("Press Enter or click")) + " to show them.";
            }
            y += d.text(d.reg, d.fs(15.5f), {x, y}, pal::body, body, w) + d.px(24);
            d.text_spaced(d.semi, d.fs(12), {x, y}, pal::faint, "IN THIS GROUP", d.px(1.5f));
            y += d.px(26);
            for (int i : members) {
                if (y > y1 - pad - d.px(24)) {
                    d.text(d.reg, d.fs(14.5f), {x, y}, pal::dim, "\xE2\x80\xA6");
                    break;
                }
                mini::string v = shown(i);
                ImVec2 vs = d.measure(d.reg, d.fs(14.5f), v);
                float vw = mini::min(vs.x, w * 0.45f);
                d.text_fit(d.reg, d.fs(14.5f), {x, y}, pal::body, S[i].name, w - vw - d.px(16));
                d.text_fit(d.reg, d.fs(14.5f), {x + w - vw, y}, modified(i) ? pal::teal : pal::dim, v, vw);
                y += d.px(25);
            }
            return;
        }
        if (it.kind == Item::Mod) { mod_detail(it.idx, x, y, w, y1 - pad); return; }
        if (it.kind == Item::Button) { mods_button_detail(it, x, y, w, y1 - pad); return; }
        if (it.kind != Item::Row) return;
        int i = it.idx;
        const Setting &s = S[i];
        Value v = val(i);
        bool on = active(i);
        if (!s.section.empty()) crumb += "  \xC2\xB7  " + s.section;
        crumb += "  \xC2\xB7  " + s.group;
        mini::string up = crumb;
        for (auto &c : up) c = (char)toupper((unsigned char)c);
        d.text_fit(d.semi, d.fs(12), {x, y}, pal::faint, up, w);
        y += d.px(28);
        y += d.text(d.semi, d.fs(27), {x, y}, pal::text, s.name, w) + d.px(6);
        y += d.text(d.semi, d.fs(19), {x, y}, on ? pal::teal : pal::dim, display(s, v), w) + d.px(12);

        // badges
        float bx = x;
        bool mod = modified(i);
        if (s.hasRecommended) {
            Value r = recommended(i);
            if (same(s, v, r)) bx += badge(bx, y, "Recommended", pal::gold, pal::goldSoft) + d.px(8);
            else bx += badge(bx, y, "Recommended: " + display(s, r), pal::gold, pal::goldSoft) + d.px(8);
        }
        if (mod) bx += badge(bx, y, "Changed", pal::teal, pal::tealSoft) + d.px(8);
        // [UIT 0304] when a change takes effect
        mini::string when = s.applies == Applies::Live ? "Also in the in-game overlay" : s.applies == Applies::Restart ? "Takes effect when the game starts"
                          : s.applies == Applies::Setup ? "Takes effect at the next setup run" : "";
        if (!when.empty()) {
            if (bx > x && bx + d.measure(d.semi, d.fs(12.5f), when).x + d.px(20) > x + w) bx = x, y += d.px(32);   // its own line
            bx += badge(bx, y, when, pal::body, pal::raised) + d.px(8);
        }
        if (bx > x) y += d.px(24) + d.px(18);
        else y += d.px(6);

        // warnings
        auto warn = [&](const mini::string &t) {
            float h = d.measure(d.reg, d.fs(14.5f), t, w - d.px(24)).y;
            d.rect({x, y}, {x + d.px(3), y + h}, pal::gold, d.px(1.5f));
            d.text(d.reg, d.fs(14.5f), {x + d.px(14), y}, pal::gold, t, w - d.px(24));
            y += h + d.px(16);
        };
        if (!on) warn(needs_note(i) + ".");
        if (running && s.applies != Applies::Launcher) warn(running_note(s));   // [UIT 0304]
        if (s.key == "TOTK_UPSCALER" && !dll)
            warn("DLSS is unavailable (FSR runs instead): nvngx_dlss.dll and sl.interposer.dll are not in the game folder. Run "
                 "tools\\setup.py --get-dlss once (it fetches NVIDIA's Streamline SDK 2.14.16), or extract that SDK to "
                 "deps\\streamline and run tools\\setup.py again (the launcher itself never downloads anything).");
        if (v.kind == Value::Custom) warn("ultrainstincttears.cfg has \"" + v.text + "\", which this list does not offer. The game still uses it; change it here to pick a listed value.");
        if (const char *env = SDL_getenv(s.key.c_str()))
            warn(mini::string("Your environment sets ") + s.key + "=" + env + ", which wins over ultrainstincttears.cfg.");

        if (is_bind(i)) {
            bool pad = s.type == Type::Pad;
            if (pad && !pad_connected) warn("No controller is connected. Connect one to bind by pressing; until then the names below are generic.");
            if (!pad) {
                y = section(x, y, "CHANGE", (dev == Device::Gamepad ? "Press " + ok_name() : mini::string("Press Enter or click")) +
                            ", then press the key. Esc cancels, Del clears. W A S D and the arrow keys move Link and the camera, so they cannot be bound.", w);
            } else
                y = section(x, y, "CHANGE", (dev == Device::Gamepad ? "Press " + ok_name() : mini::string("Press Enter or click")) +
                            ", then press the button or trigger. Esc cancels, Del clears. The sticks always move Link and the camera.", w);
        }
        float foot = d.px(40);
        float limit = y1 - pad - foot;
        auto fits = [&](const mini::string &t) { return y + d.px(22) + d.measure(d.reg, d.fs(15.5f), t, w).y <= limit; };
        y += d.text(d.reg, d.fs(16), {x, y}, pal::body, s.detail.empty() ? s.summary : s.detail, w) + d.px(22);
        if (fits(s.tradeoff)) y = section(x, y, "TRADE-OFF", s.tradeoff, w);
        mini::string def = display(s, default_value(s));
        if (s.type == Type::Int || s.type == Type::Float) def += "   (range " + fmt_num(s, s.min) + "\xE2\x80\x93" + fmt_num(s, s.max) + (s.unit.empty() ? "" : " " + s.unit) + ")";
        if (fits(def)) y = section(x, y, "DEFAULT", def, w, pal::dim);
        if (fits(s.tip)) y = section(x, y, "TIP", s.tip, w, pal::dim);
        if (fits(s.hint)) y = section(x, y, "FORMAT", s.hint, w, pal::dim);
        // the raw line, for people who read the file
        auto r = raw(i);
        mini::string line = r ? s.key + "=" + *r : s.key + "  (not in ultrainstincttears.cfg: the game default applies)";
        d.rect({x, y1 - pad - d.px(30)}, {x + w, y1 - pad - d.px(29)}, pal::line);
        d.text_fit(d.reg, d.fs(13), {x, y1 - pad - d.px(18)}, pal::faint, line, w);
    }

    // ---------------------------------------------------------------- drawing: Play and About
    void button(int k, ImVec2 a, ImVec2 b, const mini::string &title, const mini::string &sub, Glyph g = G_Move, const char *keys = nullptr) {
        bool foc = focus[tab] == k && modal.kind == Modal::None;
        bool hov = hovered(a, b) && modal.kind == Modal::None;
        float cy = (a.y + b.y) / 2;
        d.rect(a, b, foc ? pal::focus : hov ? mix(pal::panel, pal::focus, 0.5f) : pal::panel, d.px(10));
        if (foc) d.rect({a.x, a.y + d.px(14)}, {a.x + d.px(3), b.y - d.px(14)}, pal::teal, d.px(1.5f));
        float x = a.x + d.px(24), w = b.x - x - d.px(24);
        float right = 0;
        if (g != G_Move && dev == Device::Gamepad) right = glyph(g, b.x - d.px(20) - glyph(g, 0, 0, false), cy) + d.px(16);
        else if (keys) {
            ImVec2 ks = d.measure(d.reg, d.fs(13.5f), keys);
            d.text(d.reg, d.fs(13.5f), {b.x - d.px(22) - ks.x, cy - ks.y / 2}, pal::faint, keys);
            right = ks.x + d.px(22);
        }
        float th = d.measure(d.semi, d.fs(17), "Ag").y, sh = sub.empty() ? 0 : d.measure(d.reg, d.fs(14), "Ag").y + d.px(3);
        float ty = cy - (th + sh) / 2;
        d.text_fit(d.semi, d.fs(17), {x, ty}, foc ? pal::text : pal::body, title, w - right);
        if (!sub.empty()) d.text_fit(d.reg, d.fs(14), {x, ty + th + d.px(3)}, pal::dim, sub, w - right, sub.find('/') != mini::string::npos || sub.find('\\') != mini::string::npos);
        if (modal.kind == Modal::None) {
            int id = items[k].idx;
            hit(a, b, k, [this, id] { press_button(id); });
        }
    }

    void play_button(int k, ImVec2 a, ImVec2 b) {
        bool foc = focus[tab] == k && modal.kind == Modal::None;
        bool hov = hovered(a, b) && modal.kind == Modal::None;
        ImU32 fill = hov ? mix(pal::gold, pal::text, 0.12f) : pal::gold;
        if (running) fill = mix(pal::gold, pal::panel, 0.55f);
        if (foc) d.outline({a.x - d.px(5), a.y - d.px(5)}, {b.x + d.px(5), b.y + d.px(5)}, pal::teal, d.px(15), d.px(2));
        d.rect(a, b, fill, d.px(11));
        float cy = (a.y + b.y) / 2;
        mini::string t = running ? "Running" : "Play";
        ImVec2 ts = d.measure(d.semi, d.fs(26), t);
        float tri = d.px(17), gap = d.px(16);
        float tx = (a.x + b.x - (tri + gap + ts.x)) / 2;
        d.dl->AddTriangleFilled({tx, cy - tri * 0.62f}, {tx, cy + tri * 0.62f}, {tx + tri, cy}, pal::ink);
        d.text(d.semi, d.fs(26), {tx + tri + gap, cy - ts.y / 2}, pal::ink, t);
        // the shortcut, small, inside the button
        const char *key = dev == Device::Gamepad ? nullptr : "F5";
        if (key) {
            ImVec2 ks = d.measure(d.semi, d.fs(13), key);
            d.text(d.semi, d.fs(13), {b.x - d.px(22) - ks.x, cy - ks.y / 2}, alpha(pal::ink, 0.55f), key);
        } else {
            float gw = glyph(G_Play, 0, 0, false);
            glyph(G_Play, b.x - d.px(22) - gw, cy);
        }
        if (modal.kind == Modal::None) hit(a, b, k, [this] { play(false); });
    }

    mini::string romfs_path() const {
        for (int i = 0; i < (int)S.size(); i++)
            if (S[i].alias == "TOTK_ROMFS") {
                auto r = raw(i);
                if (!r || r->empty()) return {};
                mini::string p = *r;
                bool abs = p[0] == '/' || p[0] == '\\' || (p.size() > 1 && p[1] == ':');
                return abs ? p : join(dir, p);
            }
        return {};
    }

    void play_tab(float W, float H) {
        float top = d.px(76), bottom = H - d.px(58);
        float x0 = d.px(96), x1 = x0 + d.px(470);
        float y = top + d.px(64);
        y += d.text(d.semi, d.fs(46), {x0 - d.px(2), y}, pal::text, "Ultra Instinct Tears") + d.px(4);
        y += d.text(d.reg, d.fs(18), {x0, y}, pal::dim, "Tears of the Kingdom 1.4.3, rebuilt for PC") + d.px(40);
        play_button(0, {x0, y}, {x1, y + d.px(84)});
        y += d.px(84) + d.px(24);
        mini::string rec = want_dlss() ? "DLSS Quality" : "FSR 1";
        rec += ", 60 FPS, shader warm-up, fullscreen";
        struct B { const char *t; mini::string sub; Glyph g; const char *keys; };
        B bs[] = {
            {"Play with log", "Keeps the game's output in logs\\ for bug reports", G_Move, "Ctrl+L"},
            {"Recommended for this PC", rec, G_Move, nullptr},
            {"Reset all settings", "Back to the game's defaults (asks first)", G_Move, nullptr},
            {"Open game folder", dir, G_Move, nullptr},
            {"Quit", "Settings are saved as you change them", G_Back, "Esc"},
        };
        for (int k = 0; k < 5; k++) {
            float h = d.px(62);
            button(k + 1, {x0, y}, {x1, y + h}, bs[k].t, bs[k].sub, bs[k].g, bs[k].keys);
            y += h + d.px(10);
        }

        // right: a summary that jumps to the setting, and the checks
        float rx0 = mini::max(x1 + d.px(80), W * 0.47f), rx1 = W - d.px(96);
        float ry = top + d.px(64);
        float rw = rx1 - rx0;
        d.text_spaced(d.semi, d.fs(12), {rx0, ry}, pal::faint, "YOUR SETUP", d.px(1.6f));
        ry += d.px(30);
        mini::vector<int> rows;
        auto add = [&](const char *k) { int i = idx(k); if (i >= 0) rows.push_back(i); };
        add("TOTK_UPSCALER");
        mini::string up = idx("TOTK_UPSCALER") >= 0 ? tok(idx("TOTK_UPSCALER")) : "";
        if (up == "dlss") add("TOTK_DLSS_MODE");
        else if (up == "fsr") add("TOTK_FSR_SHARPNESS");
        add("TOTK_FULLSCREEN");
        add("TOTK_WINDOW_SIZE");
        add("TOTK_FPS");
        add("TOTK_SHADER_PREWARM");
        float rh = d.px(44);
        for (int i : rows) {
            ImVec2 a = {rx0 - d.px(14), ry}, b = {rx1 + d.px(14), ry + rh};
            bool hov = hovered(a, b) && modal.kind == Modal::None;
            if (hov) d.rect(a, b, pal::panel, d.px(8));
            float cy = ry + rh / 2;
            ImVec2 ts = d.measure(d.reg, d.fs(16), "Ag");
            d.text(d.reg, d.fs(16), {rx0, cy - ts.y / 2}, pal::dim, S[i].name);
            mini::string v = active(i) ? shown(i) : "Not used";
            ImVec2 vs = d.measure(d.semi, d.fs(16), v);
            float vx = rx1 - vs.x - (hov ? d.px(18) : 0);
            d.text(d.semi, d.fs(16), {vx, cy - ts.y / 2}, active(i) ? pal::text : pal::faint, v);
            if (hov) d.chevron({rx1 - d.px(3), cy}, d.px(5), pal::teal, 0);
            d.rect({rx0, ry + rh - 1}, {rx1, ry + rh}, alpha(pal::line, 0.7f));
            if (modal.kind == Modal::None) hit(a, b, -1, [this, i] { jump_to(i); });
            ry += rh;
        }
        ry += d.px(40);
        d.text_spaced(d.semi, d.fs(12), {rx0, ry}, pal::faint, "CHECKS", d.px(1.6f));
        ry += d.px(30);
        auto check = [&](int state, const mini::string &name, const mini::string &v) {   // 1 ok, 0 missing, -1 neutral
            float cy = ry + rh / 2;
            ImVec2 ts = d.measure(d.reg, d.fs(16), "Ag");
            if (state > 0) d.check({rx0 + d.px(7), cy}, d.px(11), pal::teal);
            else if (state == 0) d.cross({rx0 + d.px(7), cy}, d.px(9), pal::gold);
            else d.dl->AddCircle({rx0 + d.px(7), cy}, d.px(4.5f), pal::faint, 16, d.px(1.5f));
            d.text(d.reg, d.fs(16), {rx0 + d.px(28), cy - ts.y / 2}, pal::body, name);
            float nw = d.measure(d.reg, d.fs(16), name).x + d.px(52);
            ImVec2 vs = d.measure(d.reg, d.fs(15), v);
            float vw = mini::min(vs.x, rw - nw);
            d.text_fit(d.reg, d.fs(15), {rx1 - vw, cy - vs.y / 2}, state == 0 ? pal::gold : pal::dim, v, vw, true);
            d.rect({rx0, ry + rh - 1}, {rx1, ry + rh}, alpha(pal::line, 0.7f));
            ry += rh;
        };
        check(exe_found, "UltraInstinctTears.exe", exe_found ? "Found" : "Not found: run tools/setup.py");
        mini::string rp = romfs_path();
        SDL_PathInfo pi;
        bool rok = !rp.empty() && SDL_GetPathInfo(rp.c_str(), &pi) && pi.type == SDL_PATHTYPE_DIRECTORY;
        check(rok, "Game data (RomFS)", rp.empty() ? "Not set: run tools/setup.py" : rok ? rp : "Missing: " + rp);
        check(dll ? 1 : -1, "DLSS", dll ? "nvngx_dlss.dll and sl.interposer.dll found" : "Not available (nvngx_dlss.dll or sl.interposer.dll missing)");
        check(cfg.exists ? 1 : -1, "ultrainstincttears.cfg", cfg.exists ? "Found" : "Created on the first change");
        check(gpu.name.empty() ? -1 : 1, "Graphics card", gpu.name.empty() ? "Not detected" : gpu.name);
        {
            int on = 0, bad = 0;
            for (int k = 0; k < mods.nmods; k++) on += mods.mods[k].enabled, bad += mods.mods[k].enabled && (mods.mods[k].unsupported & (MODFS_CODE | MODFS_U_EMPTY)) != 0;
            mini::string v = mods.nmods == 0 ? mini::string("None installed") : num(on) + " of " + num(mods.nmods) + " on";
            if (mods.nmods && !mods_on()) v = "Load mods is off (" + plural(mods.nmods, "mod", "mods") + " installed)";
            else if (bad) v += " \xC2\xB7 " + num(bad) + (bad == 1 ? " has" : " have") + " parts that can't load";
            if (!inbox.empty()) v += " \xC2\xB7 " + num((long long)inbox.size()) + " in mods-inbox";
            float a0 = ry;
            check(mods.nmods && mods_on() && on ? 1 : -1, "Mods", v);
            if (modal.kind == Modal::None) hit({rx0, a0}, {rx1, ry}, -1, [this] { switch_tab(TAB_MODS); });
        }
        if (!last_run.empty()) {
            ry += d.px(18);
            d.text_fit(d.reg, d.fs(14.5f), {rx0, ry}, pal::dim, last_run, rw, true);
        }
    }

    void about_tab(float W, float H) {
        float top = d.px(76);
        float x0 = d.px(96), x1 = x0 + d.px(470);
        float y = top + d.px(64);
        y += d.text(d.semi, d.fs(30), {x0, y}, pal::text, "About") + d.px(14);
        y += d.text(d.reg, d.fs(16), {x0, y}, pal::body,
                    "Ultra Instinct Tears runs The Legend of Zelda: Tears of the Kingdom 1.4.3 natively on Windows, recompiled from your own "
                    "dumped copy of the game. This launcher edits every setting the game reads in ultrainstincttears.cfg, in the game folder, "
                    "with your comments and folder paths kept as they are.", x1 - x0) + d.px(28);
        const char *t[] = {"Open game folder", "Open ultrainstincttears.cfg", "Open logs folder", "Open save backups", "Install update",
                           "Report a problem", "Faster game code (PGO)"};
        mini::string sub[] = {dir, "In your default text editor", "Logs from Play with log",
                              "Your saves as they were before each Play (Game > Saves)",
                              update_avail ? avail_tag + " is out on GitHub: see what is new and install it"   // [UIT 0320]
                                           : mini::string("From a downloaded source package; backs up, rebuilds, undoes a failure"),
                              "Zips the newest log, settings and version",
                              "Teaches the compiler from your own play: build, play, rebuild"};
        for (int k = 0; k < 7; k++) {
            button(k, {x0, y}, {x1, y + d.px(60)}, t[k], sub[k]);
            y += d.px(68);
        }
        y += d.px(14);
        d.text_fit(d.reg, d.fs(13.5f), {x0, y}, pal::faint, cfg.path, x1 - x0, true);

        float rx0 = mini::max(x1 + d.px(80), W * 0.47f), rx1 = W - d.px(96);
        float ry = top + d.px(70);
        d.text_spaced(d.semi, d.fs(12), {rx0, ry}, pal::faint, "CONTROLS", d.px(1.6f));
        ry += d.px(30);
        float c1 = rx0 + (rx1 - rx0) * 0.32f, c2 = rx0 + (rx1 - rx0) * 0.58f;
        const char *rows[][3] = {
            {"", "Controller", "Keyboard and mouse"},
            {"Move", "D-pad, left stick", "Arrow keys, hover"},
            {"Change a value", "D-pad left / right", "Left / right (Shift: faster), click, drag"},
            {"Select, toggle", "A", "Enter, Space, click"},
            {"Back", "B", "Esc"},
            {"Reset to default", "X", "Del, Backspace, \xE2\x86\xBA icon"},
            {"Switch tab", "LB / RB", "Q / E, Tab, 1\xE2\x80\x93" "7, click"},
            {"Next section", "LT / RT", "PgUp / PgDn"},
            {"Play", "Start", "F5 (Ctrl+L: with log)"},
        };
        for (auto &r : rows) {
            bool head = r[0][0] == 0;
            ImU32 c = head ? pal::faint : pal::body;
            ImFont *f = head ? d.semi : d.reg;
            float fs = head ? d.fs(13.5f) : d.fs(15);
            d.text(d.reg, fs, {rx0, ry}, pal::dim, r[0]);
            d.text_fit(f, fs, {c1, ry}, c, r[1], c2 - c1 - d.px(12));
            d.text_fit(f, fs, {c2, ry}, c, r[2], rx1 - c2);
            ry += d.px(head ? 30 : 29);
        }
        ry += d.px(26);
        d.text_spaced(d.semi, d.fs(12), {rx0, ry}, pal::faint, "IN GAME", d.px(1.6f));
        ry += d.px(28);
        mini::string keys_line = "F7  mark in log     Shift+F7  frame capture     F8  FSR on / off     F9  sharpness     F10  DLSS on / off     F11, Alt+Enter  fullscreen";
        if (setting_text("TOTK_OVERLAY") != "0") {   // [UIT 0173] the in-game overlay's key and controller combo, as set
            mini::string k = setting_text("TOTK_OVERLAY_KEY"), p = setting_text("TOTK_OVERLAY_PAD");
            const char *pad = p.empty() ? "hold L3 + R3" : p == "misc1" ? "hold Share" : p == "touchpad" ? "hold the touchpad"
                              : p == "guide" ? "hold Guide / PS / Home"   // [UIT 0271]
                              : p == "leftstick+rightstick,guide" ? "hold L3 + R3 or Guide" : nullptr;
            keys_line = (k.empty() ? mini::string("F1") : k) + (pad ? mini::string(" (") + pad + ")" : mini::string()) +
                        "  overlay     " + keys_line;
        }
        ry += d.text(d.reg, d.fs(15), {rx0, ry}, pal::body, keys_line, rx1 - rx0) + d.px(30);
        d.text_spaced(d.semi, d.fs(12), {rx0, ry}, pal::faint, "LICENCES", d.px(1.6f));
        ry += d.px(28);
        d.text(d.reg, d.fs(14), {rx0, ry}, pal::dim,
               "Based on UltraTears by kuraeow. With code from MacNativeGames, Ryujinx and SkyboundTOTK (MIT).\n"
               "Ultra Instinct Tears and this launcher: GPL-3.0.  SDL 3: zlib.  Dear ImGui: MIT.  Inter: SIL OFL 1.1.\n"
               "Not affiliated with or endorsed by Nintendo. You need your own legally dumped copy of the game.", rx1 - rx0);
    }

    // ---------------------------------------------------------------- drawing: dialogs
    float modal_button(ImVec2 a, float h, const mini::string &t, bool foc, bool danger, int id) {
        ImVec2 ts = d.measure(d.semi, d.fs(16), t);
        float w = mini::max(d.px(132), ts.x + d.px(48));
        ImVec2 b = {a.x + w, a.y + h};
        bool hov = hovered(a, b);
        d.rect(a, b, foc ? pal::teal : hov ? pal::focus : pal::raised, d.px(8));
        if (hov && !foc) d.outline(a, b, alpha(pal::teal, 0.6f), d.px(8));
        ImU32 c = foc ? pal::ink : danger ? pal::gold : pal::body;
        d.text(d.semi, d.fs(16), {a.x + (w - ts.x) / 2, a.y + (h - ts.y) / 2}, c, t);
        hit(a, b, -1, [this, id] {
            if (modal.kind == Modal::Edit) { if (id == 0) edit_done(); else close_modal(); }
            else if (modal.kind == Modal::Bind) { if (id == 2) bind_clear(); else bind_cancel("Cancelled"); }
            else modal_press(id);
        });
        return w;
    }

    void draw_modal(float W, float H) {
        float t = snap ? 1.0f : mini::min(1.0f, (float)(now - modal.open_t) / 0.14f);
        float e = 1 - (1 - t) * (1 - t);
        d.rect({0, 0}, {W, H}, alpha(IM_COL32(6, 9, 10, 255), 0.72f * e));
        bool edit_m = modal.kind == Modal::Edit;
        bool osk = edit_m && dev == Device::Gamepad;
        float w = d.px(edit_m ? 720 : 620), pad = d.px(36), iw = w - pad * 2;
        // measure
        float h = pad;
        float title_h = d.measure(d.semi, d.fs(24), modal.title, iw).y;
        h += title_h + d.px(14);
        mini::string body = modal.body;
        const Setting *s = edit_m ? &S[modal.setting] : nullptr;
        if (edit_m) {
            if (s->type == Type::Int || s->type == Type::Float)
                body = mini::string(s->type == Type::Int ? "A whole number" : "A number") + " from " + fmt_num(*s, s->min) + " to " + fmt_num(*s, s->max) +
                       (s->unit.empty() ? "" : " (" + s->unit + ")") + ". Leave it empty for the default.";
            else body = s->hint.empty() ? "Leave it empty to remove the line and use the default." : s->hint + ". Leave it empty for the default.";
        }
        bool bind_m = modal.kind == Modal::Bind;
        mini::string now_line;
        if (bind_m) {
            const Setting &bs = S[modal.setting];
            bool pad = modal.bind_pad;
            body = pad ? "Press the controller button or trigger that should press Switch " + bs.name + " in the game."
                       : "Press the key that should press Switch " + bs.name + " in the game.";
            body += pad ? " Every controller button binds here, so cancel with the mouse, Esc, or by waiting."
                        : mini::string(" Esc cancels") + (dev == Device::Gamepad ? " (" + back_name() + " on the controller)" : mini::string()) + ", Delete clears it.";
            body += " A " + mini::string(pad ? "button" : "key") + " another Switch button uses swaps with it.";
            now_line = "Now: " + bind_labels(modal.setting) + "     Default: " + display(bs, default_value(bs));
        }
        if (modal.ask_confirm) body = ask_confirm_body();
        float body_h = body.empty() ? 0 : d.measure(d.reg, d.fs(16), body, iw).y;
        h += body_h + d.px(20);
        if (bind_m) h += d.px(124);
        float pic_h = d.px(168);
        if (modal.ask_confirm) h += pic_h + d.px(18);     // [UIT 0270] the two layouts as pictures                       // the "press now" box, the current binding, the error line
        int nrows = mini::min((int)modal.rows.size(), 9);
        float row_h = d.px(30);
        if (nrows) h += nrows * row_h + (modal.rows.size() > 9 ? row_h : 0) + d.px(16);
        float key_w = d.px(52), key_h = d.px(46), kg = d.px(6);
        if (edit_m) {
            h += d.px(58) + d.px(10);                   // input
            h += d.px(26);                              // error line
            if (osk) h += 5 * (key_h + kg) + d.px(14);
        }
        if (modal.pct >= 0) h += d.px(34);               // [UIT 0036] the progress bar
        if (!modal.busy) h += d.px(50);                 // buttons
        h += pad;
        float x0 = (W - w) / 2, y0 = (H - h) / 2 + (1 - e) * d.px(14);
        d.rect({x0, y0}, {x0 + w, y0 + h}, pal::panel, d.px(14));
        d.outline({x0, y0}, {x0 + w, y0 + h}, pal::line, d.px(14));
        float x = x0 + pad, y = y0 + pad;
        y += d.text(d.semi, d.fs(24), {x, y}, pal::text, modal.title, iw) + d.px(14);
        if (!body.empty()) y += d.text(d.reg, d.fs(16), {x, y}, pal::body, body, iw);
        y += d.px(20);
        for (int r = 0; r < nrows; r++) {
            auto &row = modal.rows[r];
            ImVec2 cs = d.measure(d.reg, d.fs(15), row.second);
            float cw = mini::min(cs.x, iw * 0.55f);
            d.text_fit(d.reg, d.fs(15), {x, y + d.px(4)}, pal::dim, row.first, iw - cw - d.px(16));
            d.text_fit(d.reg, d.fs(15), {x + iw - cw, y + d.px(4)}, row.second == "not installed" ? pal::gold : pal::teal, row.second, cw);   // a mod install that failed
            d.rect({x, y + row_h - 1}, {x + iw, y + row_h}, alpha(pal::line, 0.6f));
            y += row_h;
        }
        if (modal.rows.size() > 9) {
            d.text(d.reg, d.fs(15), {x, y + d.px(4)}, pal::faint, "and " + mini::to_string(modal.rows.size() - 9) + " more");
            y += row_h;
        }
        if (nrows) y += d.px(16);
        if (modal.pct >= 0) {
            ImVec2 a = {x, y}, b = {x + iw, y + d.px(14)};
            d.rect(a, b, pal::raised, d.px(7));
            d.rect(a, {x + iw * mini::min(1.0f, modal.pct / 100.0f), b.y}, pal::teal, d.px(7));
            y += d.px(34);
        }
        if (modal.busy) return;
        if (modal.ask_confirm) {   // [UIT 0270] a panel per layout: the four face buttons, the confirm one lit
            float gap = d.px(14), pw = (iw - gap) / 2;
            for (int k = 0; k < 2; k++) {
                int id = k == 0 ? 2 : 0;
                bool game = k == 1, foc = modal.focus == id;
                ImVec2 a = {x + k * (pw + gap), y}, b = {a.x + pw, y + pic_h};
                d.rect(a, b, foc ? alpha(pal::teal, 0.10f) : pal::bg, d.px(10));
                d.outline(a, b, foc ? pal::teal : pal::line, d.px(10), foc ? d.px(2) : d.px(1));
                d.text(d.semi, d.fs(17), {a.x + d.px(18), a.y + d.px(14)}, foc ? pal::text : pal::body, game ? "Follow the game" : "Standard");
                float cx = a.x + d.px(70), cy = a.y + d.px(102), o = d.px(25), rr = d.px(12);
                int okp = game ? 1 : 0, backp = game ? 0 : 1;
                const ImVec2 at[4] = {{cx, cy + o}, {cx + o, cy}, {cx - o, cy}, {cx, cy - o}};
                for (int pos = 0; pos < 4; pos++) {
                    ImU32 f = pos == okp ? pal::teal : pos == backp ? alpha(pal::gold, 0.30f) : pal::raised;
                    face_glyph(at[pos].x - rr, at[pos].y, pos, true, f, pos == okp ? pal::ink : pal::text);
                }
                static const char *kPos[] = {"south", "east", "west", "north"};
                float tx = cx + o + rr + d.px(20), tw = b.x - tx - d.px(12);
                d.text_fit(d.semi, d.fs(15), {tx, cy - d.px(30)}, pal::teal, "Confirm: " + face_name(kPos[okp]), tw);
                d.text_fit(d.reg, d.fs(14), {tx, cy - d.px(10)}, pal::dim, game ? "the right button" : "the bottom button", tw);
                d.text_fit(d.semi, d.fs(15), {tx, cy + d.px(12)}, pal::gold, "Back: " + face_name(kPos[backp]), tw);
                hit(a, b, -1, [this, id] { modal_press(id); });
            }
            y += pic_h + d.px(18);
        }
        if (bind_m) {
            double left = mini::max(0.0, modal.bind_until - now);
            float k = snap ? 0.62f : (float)(left / kBindSeconds);
            ImVec2 a = {x, y}, b = {x + iw, y + d.px(58)};
            d.rect(a, b, pal::bg, d.px(8));
            d.outline(a, b, modal.error.empty() ? pal::teal : pal::gold, d.px(8), d.px(1.5f));
            d.rect({a.x + d.px(2), b.y - d.px(5)}, {a.x + d.px(2) + (iw - d.px(4)) * k, b.y - d.px(2)}, alpha(pal::teal, 0.55f), d.px(2));
            mini::string wait = modal.bind_pad ? "Waiting for a controller button..." : "Waiting for a key...";
            float pulse = snap ? 1.0f : 0.65f + 0.35f * (float)mini::fabs(mini::fmod(now * 1.6, 2.0) - 1.0);
            ImVec2 ws = d.measure(d.semi, d.fs(20), wait);
            d.text(d.semi, d.fs(20), {x + d.px(18), y + (d.px(58) - ws.y) / 2 - d.px(1)}, alpha(pal::text, pulse), wait);
            mini::string secs = mini::to_string((int)(left + 0.999)) + " s";
            ImVec2 ss = d.measure(d.reg, d.fs(15), secs);
            d.text(d.reg, d.fs(15), {b.x - ss.x - d.px(18), y + (d.px(58) - ss.y) / 2}, pal::faint, secs);
            y += d.px(58) + d.px(12);
            d.text_fit(d.reg, d.fs(15), {x, y}, pal::dim, now_line, iw);
            y += d.px(28);
            if (!modal.error.empty()) d.text_fit(d.reg, d.fs(14.5f), {x, y}, pal::gold, modal.error, iw);
            y += d.px(26);
        }
        if (edit_m) {
            ImVec2 a = {x, y}, b = {x + iw, y + d.px(58)};
            d.rect(a, b, pal::bg, d.px(8));
            d.outline(a, b, modal.error.empty() ? pal::teal : pal::gold, d.px(8), d.px(1.5f));
            ImVec2 ts = d.measure(d.semi, d.fs(20), modal.buf.empty() ? "Ag" : modal.buf);
            float tw = mini::min(ts.x, iw - d.px(40));
            float ty = y + (d.px(58) - d.measure(d.semi, d.fs(20), "Ag").y) / 2;
            if (modal.buf.empty()) {
                mini::string ph = "Default: " + display(*s, default_value(*s));
                d.text_fit(d.reg, d.fs(18), {x + d.px(18), ty + d.px(1)}, pal::faint, ph, iw - d.px(40));
                tw = 0;
            } else d.text_fit(d.semi, d.fs(20), {x + d.px(18), ty}, pal::text, modal.buf, iw - d.px(40), true);
            if (snap || mini::fmod(now, 1.0) < 0.55)
                d.rect({x + d.px(19) + tw, ty + d.px(2)}, {x + d.px(21) + tw, ty + d.measure(d.semi, d.fs(20), "Ag").y - d.px(2)}, pal::teal);
            y += d.px(58) + d.px(10);
            d.text_fit(d.reg, d.fs(14.5f), {x, y}, modal.error.empty() ? pal::faint : pal::gold,
                       modal.error.empty() ? s->key : modal.error, iw);
            y += d.px(26);
            if (osk) {
                y += d.px(4);
                float gw = 11 * key_w + 10 * kg, gx = x + (iw - gw) / 2;
                for (int r = 0; r < 5; r++) {
                    int cols = r == OSK_SPECIAL_ROW ? 6 : 11;
                    float kw = r == OSK_SPECIAL_ROW ? (gw - 5 * kg) / 6 : key_w;
                    for (int c = 0; c < cols; c++) {
                        ImVec2 ka = {gx + c * (kw + kg), y}, kb = {ka.x + kw, y + key_h};
                        bool foc = modal.kr == r && modal.kc == c;
                        bool lit = r == OSK_SPECIAL_ROW && c == 0 && modal.shift;
                        d.rect(ka, kb, foc ? pal::teal : lit ? pal::tealSoft : pal::raised, d.px(7));
                        mini::string lbl = r == OSK_SPECIAL_ROW ? kOskSpecial[c] : mini::string(1, (modal.shift ? kOskRowsShift : kOskRows)[r][c]);
                        ImFont *f = r == OSK_SPECIAL_ROW ? d.semi : d.reg;
                        float fs = r == OSK_SPECIAL_ROW ? d.fs(14.5f) : d.fs(18);
                        ImVec2 ls = d.measure(f, fs, lbl);
                        d.text(f, fs, {ka.x + (kw - ls.x) / 2, ka.y + (key_h - ls.y) / 2}, foc ? pal::ink : pal::body, lbl);
                        int rr = r, cc = c;
                        hit(ka, kb, -1, [this, rr, cc] { modal.kr = rr, modal.kc = cc; osk_press(); });
                    }
                    y += key_h + kg;
                }
                y += d.px(14);
            }
        }
        // buttons, right aligned
        float bh = d.px(50), by = y0 + h - pad - bh;
        mini::vector<mini::pair<int, mini::string>> bs;
        if (edit_m) bs = {{1, "Cancel"}, {0, "Save"}};
        else if (bind_m) bs = {{2, "Clear"}, {1, "Cancel"}};
        else for (int id : modal_order()) bs.push_back({id, id == 0 ? modal.ok : id == 1 ? modal.cancel : modal.alt});
        float total = 0;
        for (auto &b : bs) total += mini::max(d.px(132), d.measure(d.semi, d.fs(16), b.second).x + d.px(48)) + d.px(10);
        float bx = x0 + w - pad - total + d.px(10);
        for (auto &b : bs) {
            bool foc = edit_m ? (!osk && b.first == 0) : bind_m ? false : modal.focus == b.first;
            bx += modal_button({bx, by}, bh, b.second, foc, modal.danger && b.first == 0, b.first) + d.px(10);
        }
    }

    // ---------------------------------------------------------------- frame
    void frame() {
        ImGuiIO &io = ImGui::GetIO();
        float W = io.DisplaySize.x, H = io.DisplaySize.y;
        d.s = mini::min(W / 1440.0f, H / 900.0f);
        d.dl = ImGui::GetBackgroundDrawList();
        hits.clear();
        d.rect({0, 0}, {W, H}, pal::bg);
        if (tab == TAB_PLAY) play_tab(W, H);
        else if (tab == TAB_ABOUT) about_tab(W, H);
        else settings_tab(W, H);
        top_bar(W);
        footer(W, H);
        if (modal.kind != Modal::None) draw_modal(W, H);
        if (mouse_moved && modal.kind == Modal::None && drag < 0) {
            for (int k = (int)hits.size() - 1; k >= 0; k--)
                if (hovered(hits[k].a, hits[k].b)) {
                    if (hits[k].item >= 0 && hits[k].item != focus[tab]) { focus[tab] = hits[k].item; follow = false; }
                    if (hits[k].item >= 0) break;
                }
        }
        mouse_moved = false;
    }

    void click(float x, float y) {
        mx = x, my = y;
        for (int k = (int)hits.size() - 1; k >= 0; k--) {
            Hit &h = hits[k];
            if (!hovered(h.a, h.b)) continue;
            if (h.slider >= 0) {
                drag = h.slider, drag_hit = h;
                slider_set(h, x);
                return;
            }
            if (h.item >= 0 && modal.kind == Modal::None) focus[tab] = h.item, set_sfield(false);   // [UIT 0251]
            if (h.click) {
                auto fn = h.click;     // the handler may rebuild hits
                fn();
                return;
            }
            if (h.item >= 0) return;
        }
    }

    // ---------------------------------------------------------------- input
    void push(Act a, int repeat = 0, bool fast = false) {
        Action x{a};
        x.repeat = repeat, x.fast = fast;
        actions.push_back(x);
    }

    void on_dir(int dirn, int repeat) { push((Act)(A_Up + dirn), repeat); }

    void key_down(const SDL_KeyboardEvent &k) {
        dev = Device::Keyboard;
        bool ctrl = k.mod & SDL_KMOD_CTRL, shift = k.mod & SDL_KMOD_SHIFT;
        static int rep = 0;
        rep = k.repeat ? rep + 1 : 0;
        if (modal.kind == Modal::Bind) {      // Esc cancels, Delete clears; any other key binds (keyboard dialog)
            if (k.repeat) return;
            if (k.key == SDLK_ESCAPE) push(A_Back);
            else if (k.key == SDLK_DELETE) push(A_Reset);
            else if (!modal.bind_pad) bind_key(k.scancode);
            return;
        }
        if (modal.kind == Modal::Edit) {
            switch (k.key) {
            case SDLK_RETURN: case SDLK_KP_ENTER: push(A_Done); break;
            case SDLK_ESCAPE: push(A_Back); break;
            case SDLK_BACKSPACE: push(A_Backspace); break;
            case SDLK_V: if (ctrl) push(A_Paste); break;
            default: break;
            }
            return;
        }
        if (sfield && modal.kind == Modal::None) {   // [UIT 0251] typing in the search field: letters arrive as text
            switch (k.key) {
            case SDLK_ESCAPE: if (!k.repeat) push(A_Back); return;
            case SDLK_RETURN: case SDLK_KP_ENTER: if (!k.repeat) push(A_Accept); return;
            case SDLK_DOWN: push(A_Down, rep); return;
            case SDLK_UP: push(A_Up, rep); return;
            case SDLK_LEFT: if (sosk) push(A_Left, rep); return;
            case SDLK_RIGHT: if (sosk) push(A_Right, rep); return;
            case SDLK_BACKSPACE: { Action a{A_Backspace}; a.n = ctrl ? 1 : 0; actions.push_back(a); return; }
            case SDLK_TAB: push(shift ? A_TabPrev : A_TabNext); return;
            case SDLK_PAGEUP: push(A_PagePrev); return;
            case SDLK_PAGEDOWN: push(A_PageNext); return;
            case SDLK_F5: if (!k.repeat) push(shift ? A_PlayLog : A_Play); return;
            case SDLK_V: if (ctrl) push(A_Paste); return;
            case SDLK_F: if (ctrl && !k.repeat) push(A_Search); return;
            default: if (!ctrl) return; break;   // Ctrl+S / Ctrl+Q / Ctrl+L as anywhere
            }
        }
        if (ctrl && k.key == SDLK_F && !k.repeat) { push(A_Search); return; }   // [UIT 0251]
        if (ctrl) {
            if (k.key == SDLK_L && !k.repeat) push(A_PlayLog);
            if (k.key == SDLK_S) { save_at = 0; save_now(); flash("Saved"); }
            if (k.key == SDLK_Q) quit = true;
            if (k.key == SDLK_UP) push(A_ModUp, rep);      // Mods tab: move the focused mod up / down the load order
            if (k.key == SDLK_DOWN) push(A_ModDown, rep);
            return;
        }
        switch (k.key) {
        case SDLK_UP: push(A_Up, rep, shift); break;
        case SDLK_DOWN: push(A_Down, rep, shift); break;
        case SDLK_LEFT: push(A_Left, rep, shift); break;
        case SDLK_RIGHT: push(A_Right, rep, shift); break;
        case SDLK_RETURN: case SDLK_KP_ENTER: case SDLK_SPACE: if (!k.repeat) push(A_Accept); break;
        case SDLK_ESCAPE: if (!k.repeat) push(A_Back); break;
        case SDLK_BACKSPACE: case SDLK_DELETE: if (!k.repeat) push(A_Reset); break;
        case SDLK_TAB: push(shift ? A_TabPrev : A_TabNext); break;
        case SDLK_Q: push(A_TabPrev); break;
        case SDLK_E: push(A_TabNext); break;
        case SDLK_PAGEUP: push(A_PagePrev); break;
        case SDLK_PAGEDOWN: push(A_PageNext); break;
        case SDLK_HOME: push(A_Home); break;
        case SDLK_END: push(A_End); break;
        case SDLK_F5: if (!k.repeat) push(shift ? A_PlayLog : A_Play); break;
        case SDLK_M: if (!k.repeat) push(A_Grab); break;   // Mods tab: pick the focused mod up, as Y on a controller
        case SDLK_SLASH: case SDLK_KP_DIVIDE: if (!k.repeat) { eat_slash = 3; push(A_Search); } break;   // [UIT 0251]
        default:
            if (k.key >= SDLK_1 && k.key <= SDLK_7 && !k.repeat) { Action a{A_JumpTab}; a.n = (int)(k.key - SDLK_1); actions.push_back(a); }
            break;
        }
    }

    void text_input(const char *t) {
        if (modal.kind == Modal::Edit) { Action a{A_Char}; a.ch = t; actions.push_back(a); }
        else if (modal.kind == Modal::None && sfield) {   // [UIT 0251] the search field
            if (eat_slash && !strcmp(t, "/")) eat_slash = 0;   // the '/' that opened it
            else { Action a{A_Char}; a.ch = t; actions.push_back(a); }
        }
    }

    void pad_button(int b, bool down) {
        int dirn = b == SDL_GAMEPAD_BUTTON_DPAD_UP ? 0 : b == SDL_GAMEPAD_BUTTON_DPAD_DOWN ? 1 : b == SDL_GAMEPAD_BUTTON_DPAD_LEFT ? 2
                 : b == SDL_GAMEPAD_BUTTON_DPAD_RIGHT ? 3 : -1;
        if (!down) {
            if (dirn >= 0 && held == dirn) held = -1;
            return;
        }
        dev = Device::Gamepad;
        if (modal.kind == Modal::Bind) {      // controller dialog: every button binds; keyboard dialog: B cancels, X clears
            if (modal.bind_pad) bind_pad_button(b);
            else if (b == back_button()) push(A_Back);
            else if (b == SDL_GAMEPAD_BUTTON_WEST) push(A_Reset);
            return;
        }
        if (dirn >= 0) {
            held = dirn, held_count = 0, held_next = now + 0.38;
            on_dir(dirn, 0);
            return;
        }
        if (modal.ask_confirm && (b == SDL_GAMEPAD_BUTTON_SOUTH || b == SDL_GAMEPAD_BUTTON_EAST)) {
            modal_press(b == SDL_GAMEPAD_BUTTON_SOUTH ? 2 : 0);   // [UIT 0270] the first-run question: the button that should confirm
            return;
        }
        bool edit_m = modal.kind == Modal::Edit;
        if (b == ok_button()) { push(A_Accept); return; }   // [UIT 0270] south or east, by Game > Controller > Confirm button
        if (b == back_button()) { push(A_Back); return; }
        switch (b) {
        case SDL_GAMEPAD_BUTTON_WEST: push(A_Reset); break;
        case SDL_GAMEPAD_BUTTON_NORTH: push(edit_m || sosk ? A_Shift : A_Grab); break;
        case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER: if (!edit_m && !sosk) push(A_TabPrev); break;
        case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: if (!edit_m && !sosk) push(A_TabNext); break;
        case SDL_GAMEPAD_BUTTON_START: push(edit_m || sosk ? A_Done : A_Play); break;
        case SDL_GAMEPAD_BUTTON_BACK: if (modal.kind == Modal::None) push(A_Search); break;   // [UIT 0251] View: search
        default: break;
        }
    }

    // The connected controller names the bindings ("B" on Xbox, "Circle" on PlayStation); a script's padtype: wins.
    bool pad_type_forced = false;
    void pad_seen(mini::vector<SDL_Gamepad *> &pads) {
        if (!pad_type_forced) pad_connected = !pads.empty();
        if (pad_type_forced || pads.empty()) return;
        switch (SDL_GetGamepadType(pads[0])) {
        case SDL_GAMEPAD_TYPE_XBOX360: case SDL_GAMEPAD_TYPE_XBOXONE: pad_family = PadFamily::Xbox; break;
        case SDL_GAMEPAD_TYPE_PS3: case SDL_GAMEPAD_TYPE_PS4: case SDL_GAMEPAD_TYPE_PS5: pad_family = PadFamily::PlayStation; break;
        case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO: case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_PAIR: pad_family = PadFamily::Nintendo; break;
        default: pad_family = PadFamily::Generic; break;
        }
    }

    void pad_repeat(mini::vector<SDL_Gamepad *> &pads) {
        // left stick as a d-pad
        int sd = -1;
        for (auto *p : pads) {
            float x = SDL_GetGamepadAxis(p, SDL_GAMEPAD_AXIS_LEFTX) / 32767.0f, y = SDL_GetGamepadAxis(p, SDL_GAMEPAD_AXIS_LEFTY) / 32767.0f;
            float th = stick_dir >= 0 ? 0.42f : 0.55f;     // a little hysteresis
            if (mini::fabs(y) > mini::fabs(x) && mini::fabs(y) > th) sd = y < 0 ? 0 : 1;
            else if (mini::fabs(x) > th) sd = x < 0 ? 2 : 3;
            if (sd >= 0) break;
        }
        if (sd != stick_dir) {
            if (stick_dir >= 0 && held == stick_dir + 10) held = -1;
            stick_dir = sd;
            if (sd >= 0) {
                dev = Device::Gamepad;
                held = sd + 10, held_count = 0, held_next = now + 0.38;
                on_dir(sd, 0);
            }
        }
        if (held >= 0 && now >= held_next) {
            held_count++;
            on_dir(held % 10, held_count);
            held_next = now + (held_count < 8 ? 0.085 : held_count < 20 ? 0.055 : 0.035);
        }
    }

    void pad_axis(int axis, float v) {
        if (axis != SDL_GAMEPAD_AXIS_LEFT_TRIGGER && axis != SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) return;
        int k = axis == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER;
        if (!trig_down[k] && v > 0.5f) {
            trig_down[k] = true;
            dev = Device::Gamepad;
            if (modal.kind == Modal::Bind && modal.bind_pad) bind_apply(k ? "righttrigger" : "lefttrigger");
            else if (modal.kind == Modal::None && !sosk) push(k ? A_PageNext : A_PagePrev);
        } else if (trig_down[k] && v < 0.25f) trig_down[k] = false;
    }

    void run_actions() {
        mini::vector<Action> todo;
        todo.swap(actions);
        for (auto &a : todo) {
            switch (a.a) {
            case A_Up: case A_Down: case A_Home: case A_End: case A_PagePrev: case A_PageNext:
            case A_TabPrev: case A_TabNext: case A_JumpTab: follow = true; break;
            default: break;
            }
            handle(a);
            if (quit) break;
        }
    }

    // ---------------------------------------------------------------- scripted runs (screenshots, smoke tests)
    // --script "pad;tab:1;down:2;right;accept;back;reset;page;type:abc;jump:TOTK_FPS;mouse:x,y;click;wait:3;shot:a.png;quit"
    // Test steps (tools/test_launcher.py): press:recommend|reset|play|playlog|folder (a Play-tab button), ok / cancel /
    // alt (a dialog button), each (every setting: right, right, left, reset), save (write ultrainstincttears.cfg now), reload
    // (read it again), check:KEY=VALUE (log "script: check ... FAILED" unless the file's value is VALUE, "-" = absent).
    mini::vector<mini::string> cmds;
    size_t cmd_i = 0;
    int cmd_wait = 0;
    mini::string shot_path;

    void script_step() {
        if (cmd_wait > 0) { cmd_wait--; return; }
        if (cmd_i >= cmds.size()) return;
        mini::string c = cmds[cmd_i++], arg;
        size_t colon = c.find(':');
        if (colon != mini::string::npos) arg = c.substr(colon + 1), c = c.substr(0, colon);
        int n = arg.empty() ? 1 : atoi(arg.c_str());
        auto rep = [&](Act a) { for (int k = 0; k < n; k++) { push(a); run_actions(); } };
        if (c == "pad") dev = Device::Gamepad;
        else if (c == "kbm") dev = Device::Keyboard;
        else if (c == "tab") switch_tab(n);
        else if (c == "up") rep(A_Up);
        else if (c == "down") rep(A_Down);
        else if (c == "left") rep(A_Left);
        else if (c == "right") rep(A_Right);
        else if (c == "accept") rep(A_Accept);
        else if (c == "back") rep(A_Back);
        else if (c == "reset") rep(A_Reset);
        else if (c == "page") rep(A_PageNext);
        else if (c == "shift") rep(A_Shift);
        else if (c == "play") rep(A_Play);
        else if (c == "type") { for (char ch : arg) { Action a{A_Char}; a.ch = mini::string(1, ch); actions.push_back(a); } run_actions(); }
        else if (c == "done") rep(A_Done);
        else if (c == "jump") { int i = idx(arg.c_str()); if (i >= 0) jump_to(i); follow = true; }
        else if (c == "mouse") {
            float x = 0, y = 0;
            sscanf(arg.c_str(), "%f,%f", &x, &y);
            mx = x, my = y, mouse_moved = true, dev = Device::Keyboard;
        } else if (c == "click") click(mx, my);
        else if (c == "wait") cmd_wait = n;
        else if (c == "shot") shot_path = arg;
        // bindings: bind:KEY opens the dialog for setting KEY; presskey:J / presspad:south / presstrig:right send a press
        // (as from the keyboard / controller); padtype:xbox|ps|nintendo|generic names the controller
        else if (c == "bind") { int i = idx(arg.c_str()); if (i >= 0) { jump_to(i); open_bind(i); } else SDL_Log("script: check bind %s FAILED (no such setting)", arg.c_str()); }
        else if (c == "presskey") {
            SDL_Scancode sc = SDL_GetScancodeFromName(arg.c_str());
            int k = uit_key_find(arg.c_str(), arg.size());
            if (k >= 0) sc = kUitKeys[k].sc;
            if (modal.kind != Modal::Bind) SDL_Log("script: check presskey %s FAILED (no bind dialog)", arg.c_str());
            else if (sc == SDL_SCANCODE_ESCAPE) bind_cancel("Cancelled");
            else if (sc == SDL_SCANCODE_DELETE) bind_clear();
            else if (!modal.bind_pad) bind_key(sc);
        } else if (c == "presspad") {
            int p = uit_pad_find(arg.c_str(), arg.size());
            if (p < 0 || modal.kind != Modal::Bind) SDL_Log("script: check presspad %s FAILED", arg.c_str());
            else if (kUitPadInputs[p].button >= 0) pad_button(kUitPadInputs[p].button, true), pad_button(kUitPadInputs[p].button, false);
            else pad_axis(kUitPadInputs[p].axis, 0.0f), pad_axis(kUitPadInputs[p].axis, 1.0f), pad_axis(kUitPadInputs[p].axis, 0.0f);
            run_actions();
        } else if (c == "padtype") {
            pad_type_forced = true;
            pad_connected = arg != "none";
            pad_family = arg == "xbox" ? PadFamily::Xbox : arg == "ps" ? PadFamily::PlayStation : arg == "nintendo" ? PadFamily::Nintendo : PadFamily::Generic;
        }
        else if (c == "quit") quit = true;
        // [UIT 0304] gamerunning:1|0 (as if the game were running, nothing is started); checkapplies:KEY=live|restart|setup|launcher;
        // checkflash:TEXT / checknoflash:TEXT (the last message contains TEXT / does not)
        else if (c == "gamerunning") { running = fake_running = n != 0; if (!running) last_run.clear(); else last_run = "Running"; }
        else if (c == "checkapplies") {
            size_t eq = arg.find('=');
            int i = idx(arg.substr(0, eq).c_str());
            mini::string have = i < 0 ? mini::string("(no such setting)") : S[i].applies == Applies::Live ? "live" : S[i].applies == Applies::Setup ? "setup"
                              : S[i].applies == Applies::Launcher ? "launcher" : "restart";
            mini::string want = eq == mini::string::npos ? mini::string() : arg.substr(eq + 1);
            SDL_Log("script: check applies %s %s (have %s)", arg.c_str(), have == want ? "ok" : "FAILED", have.c_str());
        } else if (c == "checkflash" || c == "checknoflash") {   // checknoflash:TEXT: the last message does not contain TEXT
            bool has = toast.find(arg) != mini::string::npos;
            SDL_Log("script: check %s %s %s (have %s)", c.c_str() + 5, arg.c_str(), has == (c == "checkflash") ? "ok" : "FAILED", toast.c_str());
        }
        // [UIT 0270] firstrun (open the which-button-confirms question if TOTK_UI_CONFIRM is not set, as on the first open);
        // padbtn:south|east|west|north (by position; a / b / x / y are the same positions)
        else if (c == "firstrun") { if (confirm_unasked()) ask_confirm_layout(); else SDL_Log("script: firstrun: already chosen"); }
        // [UIT 0270] checkbuttons:OK/BACK, the confirm and back buttons' names as the hints show them (e.g. A/B, B/A, Cross/Circle);
        // nodialog: no dialog is open
        else if (c == "checkbuttons") {
            mini::string have = ok_name() + "/" + back_name();
            SDL_Log("script: check buttons %s %s (have %s)", arg.c_str(), have == arg ? "ok" : "FAILED", have.c_str());
        } else if (c == "nodialog") {
            if (modal.kind != Modal::None) SDL_Log("script: check nodialog FAILED (dialog: %s)", modal.title.c_str());
        }
        // [UIT 0251] search: search:WORDS (set this tab's search), key:NAME (a key press as from the keyboard, e.g. ctrl+f,
        // /, Escape, Return, Down, ctrl+Backspace; "/" also sends its text), text:abc (typed text), padbtn:view|a|b|x|y|start|
        // up|down|left|right|lb|rb (a controller button), checkrows:K1,K2 (the listed settings in order, mod:NAME for mods),
        // checkcount:N (matches here), checkfocus:KEY|search|osk|group:GID|button:LABEL|mod:NAME|note, checkopen:GID=0|1,
        // checkquery:TEXT ("-" = empty), checktab:N
        else if (c == "search") { search_set(arg); run_actions(); }
        else if (c == "key") {
            SDL_KeyboardEvent k{};
            k.type = SDL_EVENT_KEY_DOWN, k.down = true;
            mini::string nm = arg;
            for (;;) {
                if (nm.substr(0, 5) == "ctrl+") k.mod |= SDL_KMOD_LCTRL, nm = nm.substr(5);
                else if (nm.substr(0, 6) == "shift+") k.mod |= SDL_KMOD_LSHIFT, nm = nm.substr(6);
                else break;
            }
            k.key = SDL_GetKeyFromName(nm.c_str());
            if (k.key == SDLK_UNKNOWN) SDL_Log("script: check key %s FAILED (unknown key)", arg.c_str());
            else {
                key_down(k);
                if (nm == "/" && !k.mod) text_input("/");
                run_actions();
            }
        } else if (c == "text") { for (char ch : arg) { char t[2] = {ch, 0}; text_input(t); } run_actions(); }
        else if (c == "padbtn") {
            static const char *names[] = {"a", "b", "x", "y", "view", "start", "lb", "rb", "up", "down", "left", "right",
                                          "south", "east", "west", "north"};
            static const int btns[] = {SDL_GAMEPAD_BUTTON_SOUTH, SDL_GAMEPAD_BUTTON_EAST, SDL_GAMEPAD_BUTTON_WEST, SDL_GAMEPAD_BUTTON_NORTH,
                                       SDL_GAMEPAD_BUTTON_BACK, SDL_GAMEPAD_BUTTON_START, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER,
                                       SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, SDL_GAMEPAD_BUTTON_DPAD_UP, SDL_GAMEPAD_BUTTON_DPAD_DOWN,
                                       SDL_GAMEPAD_BUTTON_DPAD_LEFT, SDL_GAMEPAD_BUTTON_DPAD_RIGHT, SDL_GAMEPAD_BUTTON_SOUTH,
                                       SDL_GAMEPAD_BUTTON_EAST, SDL_GAMEPAD_BUTTON_WEST, SDL_GAMEPAD_BUTTON_NORTH};
            int b = -1;
            for (int k = 0; k < (int)(sizeof names / sizeof *names); k++) if (arg == names[k]) b = btns[k];
            if (b < 0) SDL_Log("script: check padbtn %s FAILED (unknown button)", arg.c_str());
            else { pad_button(b, true); pad_button(b, false); run_actions(); }
        } else if (c == "checkrows") {
            mini::string have;
            for (auto &it : items) {
                if (it.kind == Item::Row) have += (have.empty() ? "" : ",") + S[it.idx].key;
                else if (it.kind == Item::Mod) have += mini::string(have.empty() ? "mod:" : ",mod:") + mods.mods[it.idx].name;
            }
            if (have.empty()) have = "-";
            SDL_Log("script: check rows %s %s (have %s)", arg.c_str(), have == arg ? "ok" : "FAILED", have.c_str());
        } else if (c == "checkcount") {
            SDL_Log("script: check count %d %s (have %d)", n, smatch == n ? "ok" : "FAILED", smatch);
        } else if (c == "checkquery") {
            mini::string have = query[tab].empty() ? mini::string("-") : query[tab];
            SDL_Log("script: check query %s %s (have %s)", arg.c_str(), have == arg ? "ok" : "FAILED", have.c_str());
        } else if (c == "checktab") {
            SDL_Log("script: check tab %d %s (have %d)", n, tab == n ? "ok" : "FAILED", tab);
        } else if (c == "checkopen") {
            size_t eq = arg.find('=');
            mini::string g = arg.substr(0, eq);
            int want = eq == mini::string::npos ? 1 : atoi(arg.c_str() + eq + 1), have = expanded.count(g) ? 1 : 0;
            SDL_Log("script: check open %s %s (have %d)", arg.c_str(), have == want ? "ok" : "FAILED", have);
        } else if (c == "checkfocus") {
            mini::string have = "-";
            if (sosk) have = "osk";
            else if (sfield) have = "search";
            else if (!items.empty()) {
                Item &it = items[focus[tab]];
                if (it.kind == Item::Row) have = S[it.idx].key;
                else if (it.kind == Item::Group) have = "group:" + it.gid;
                else if (it.kind == Item::Mod) have = mini::string("mod:") + mods.mods[it.idx].name;
                else if (it.kind == Item::Button) have = "button:" + (it.idx == B_SearchIn ? it.label : num(it.idx));
                else if (it.kind == Item::Note) have = "note";
                else if (it.kind == Item::Heading) have = "heading";
            }
            SDL_Log("script: check focus %s %s (have %s)", arg.c_str(), have == arg ? "ok" : "FAILED", have.c_str());
        }
        else if (c == "pickzip") { if (!pick_mu) pick_mu = SDL_CreateMutex(); SDL_LockMutex(pick_mu); picked.clear(); picked.push_back(arg); pick_kind = P_Update; picked_ready = true; SDL_UnlockMutex(pick_mu); poll_task(); }
        // mods (0059): mods (scan again), pickmod:a|b (as if picked in the dialog), drop:path (one dropped file or folder),
        // checkmods:A+,B- (the load order, + on / - off), focusmod:NAME, grab / modup / moddown, checkinbox:N, checknew:NAME=N,
        // checkbefore:A,B (what Disable Mods remembers, "-" none), checkcheats:ON/REMEMBERED (the cheats, "-" none), checkplaced:NAME=game/path (what uit-placed.txt says; "" = none, "-" = no such mod)
        else if (c == "mods") mods_reload();
        else if (c == "pickmod" || c == "drop") {
            mini::vector<mini::string> l;
            mini::string cur;
            for (char ch : arg + "|") { if (ch == '|') { if (!cur.empty()) l.push_back(cur); cur.clear(); } else cur += ch; }
            if (c == "drop") { for (auto &x : l) dropped.push_back(x); drop_done(); }
            else {
                if (!pick_mu) pick_mu = SDL_CreateMutex();
                SDL_LockMutex(pick_mu); picked = l; pick_kind = P_Mod; picked_ready = true; SDL_UnlockMutex(pick_mu);
                poll_task();
            }
        } else if (c == "checkmods") {
            mini::string got;
            for (int k = 0; k < mods.nmods; k++) got += mini::string(k ? "," : "") + mods.mods[k].name + (mods.mods[k].enabled ? "+" : "-");
            SDL_Log("script: check mods %s %s (have %s)", arg.c_str(), got == arg ? "ok" : "FAILED", got.c_str());
        } else if (c == "checknew") {   // checknew:NAME=N: how many of that mod's files the game does not have
            size_t eq = arg.find('=');
            mini::string nm = eq == mini::string::npos ? arg : arg.substr(0, eq);
            int want = eq == mini::string::npos ? 0 : SDL_atoi(arg.c_str() + eq + 1), have = -1;
            for (int k = 0; k < mods.nmods; k++) if (nm == mods.mods[k].name) have = mods.mods[k].nnew;
            SDL_Log("script: check new %s %s (have %d)", arg.c_str(), have == want ? "ok" : "FAILED", have);
        } else if (c == "checkbefore") {   // checkbefore:A,B: the mods Disable Mods remembers ("-" = none)
            mini::string have;
            for (auto &q : mods_before_disable()) have += (have.empty() ? "" : ",") + q;
            if (have.empty()) have = "-";
            SDL_Log("script: check before %s %s (have %s)", arg.c_str(), have == arg ? "ok" : "FAILED", have.c_str());
        } else if (c == "checkcheats") {   // [UIT 0175] checkcheats:ON/REMEMBERED, e.g. TOTK_CHEAT_AMIIBO/- ("-" = none)
            mini::string on, rem;
            for (auto &q : cheats_on_now()) on += (on.empty() ? "" : ",") + q;
            for (auto &q : cheats_before_disable()) rem += (rem.empty() ? "" : ",") + q;
            mini::string have = (on.empty() ? mini::string("-") : on) + "/" + (rem.empty() ? mini::string("-") : rem);
            SDL_Log("script: check cheats %s %s (have %s)", arg.c_str(), have == arg ? "ok" : "FAILED", have.c_str());
        } else if (c == "checkplaced") {   // checkplaced:NAME=game/path: the mod's details show it placed there
            size_t eq = arg.find('=');
            mini::string nm = eq == mini::string::npos ? arg : arg.substr(0, eq), want = eq == mini::string::npos ? mini::string() : arg.substr(eq + 1), have = "-";
            for (int k = 0; k < mods.nmods && k < (int)mods_placed.size(); k++) if (nm == mods.mods[k].name) have = mods_placed[k];
            while (!have.empty() && (have.back() == '\n' || have.back() == '\r')) have.pop_back();
            SDL_Log("script: check placed %s %s (have %s)", arg.c_str(), have == want ? "ok" : "FAILED", have.c_str());
        } else if (c == "checkinbox") {
            SDL_Log("script: check inbox %d %s (have %d)", n, (int)inbox.size() == n ? "ok" : "FAILED", (int)inbox.size());
        } else if (c == "focusmod") {
            bool f = false;
            for (size_t k = 0; k < items.size(); k++)
                if (items[k].kind == Item::Mod && arg == mods.mods[items[k].idx].name) focus[tab] = (int)k, follow = true, f = true;
            if (!f) SDL_Log("script: check focusmod %s FAILED (not listed)", arg.c_str());
        }
        else if (c == "grab") rep(A_Grab);
        else if (c == "modup") rep(A_ModUp);
        else if (c == "moddown") rep(A_ModDown);
        // [UIT 0320] startupcheck[:SECONDS] (arm the start-up update check as the launcher does at start, default at once);
        // relaunch (as after --relaunch); waitcheck (until it has answered, 30 s at most, or is waiting for the game);
        // checkbadge:1|0 (the dot on About / the line at the bottom); checkucheck:off|armed|asking|done
        else if (c == "startupcheck") startup_check(arg.empty() ? 0.0 : atof(arg.c_str()));
        else if (c == "relaunch") relaunched = true;
        else if (c == "waitcheck") {
            for (int k = 0; k < 1500 && (ucheck_state == UC_Asking || (ucheck_state == UC_Armed && !running)); k++) {
                SDL_Delay(20); now = SDL_GetTicksNS() / 1e9; ucheck_poll();
            }
            if (ucheck_state == UC_Asking) SDL_Log("script: check waitcheck FAILED (still asking)");
        } else if (c == "checkbadge") {
            SDL_Log("script: check badge %d %s (have %d, %s)", n, update_avail == (n != 0) ? "ok" : "FAILED", update_avail ? 1 : 0, avail_tag.c_str());
        } else if (c == "checkucheck") {
            static const char *st[] = {"off", "armed", "asking", "done"};
            SDL_Log("script: check ucheck %s %s (have %s)", arg.c_str(), arg == st[ucheck_state] ? "ok" : "FAILED", st[ucheck_state]);
        }
        else if (c == "waittask") {   // until the helper is done (a minute at most)
            for (int k = 0; k < 3000 && task_kind != T_None; k++) { SDL_Delay(20); now = SDL_GetTicksNS() / 1e9; poll_task(); }
            if (task_kind != T_None) SDL_Log("script: check waittask FAILED (still running)");
        } else if (c == "title") {
            if (modal.title != arg) SDL_Log("script: check title %s FAILED (dialog: %s)", arg.c_str(), modal.kind == Modal::None ? "none" : modal.title.c_str());
        }
        else if (c == "bodyhas") {   // [UIT 0281] bodyhas:TEXT: the open dialog's text contains TEXT
            if (modal.body.find(arg) == mini::string::npos) SDL_Log("script: check bodyhas %s FAILED (body: %s)", arg.c_str(), modal.body.substr(0, 300).c_str());
        } else if (c == "rowhas") {   // rowhas:NAME=TEXT: the dialog has a row NAME whose value contains TEXT
            size_t eq = arg.find('=');
            mini::string nm = arg.substr(0, eq), want = eq == mini::string::npos ? mini::string() : arg.substr(eq + 1), have = "(no such row)";
            for (auto &r : modal.rows) if (r.first == nm) have = r.second;
            if (have.find(want) == mini::string::npos || have == "(no such row)") SDL_Log("script: check rowhas %s FAILED (have %s)", arg.c_str(), have.c_str());
        }
        else if (c == "press") {
            static const char *names[] = {"play", "playlog", "recommend", "reset", "folder", "cfg", "logs", "quit", "-", "update", "report",
                                           "modfile", "modfolder", "inbox", "allon", "alloff", "modsfolder", "pgo", "backups"};
            for (int b = 0; b < (int)(sizeof names / sizeof *names); b++)
                if (arg == names[b]) press_button(b);
        } else if (c == "ok" || c == "cancel" || c == "alt") {
            if (modal.kind == Modal::None) SDL_Log("script: check %s FAILED (no dialog open)", c.c_str());
            else modal_press(c == "ok" ? 0 : c == "cancel" ? 1 : 2);
        } else if (c == "each") {
            for (int i = 0; i < (int)S.size(); i++) {
                if (S[i].type == Type::Text) continue;
                jump_to(i);
                for (Act a : {A_Right, A_Right, A_Left, A_Reset}) {
                    push(a);
                    run_actions();
                    if (modal.kind != Modal::None) close_modal();
                }
            }
        } else if (c == "save") {
            save_at = 0;
            save_now();
        } else if (c == "reload") {
            if (save_at >= 0) { save_at = 0; save_now(); }
            cfg.load(cfg.path);
        } else if (c == "check") {
            size_t eq = arg.find('=');
            mini::string k = arg.substr(0, eq), want = eq == mini::string::npos ? "-" : arg.substr(eq + 1);
            CfgFile f;
            if (save_at >= 0) { save_at = 0; save_now(); }
            f.load(cfg.path);
            auto got = f.get(k);
            mini::string g = got ? *got : mini::string("-");
            SDL_Log("script: check %s=%s %s (file has %s)", k.c_str(), want.c_str(), g == want ? "ok" : "FAILED", g.c_str());
        }
        else SDL_Log("script: unknown command %s", c.c_str());
        if (c != "wait" && c != "shot") cmd_wait = 1;   // let one frame settle after each step
    }
};

static mini::vector<mini::string> split(const mini::string &s, char sep) {
    mini::vector<mini::string> out;
    mini::string cur;
    for (char c : s) {
        if (c == sep) { if (!cur.empty()) out.push_back(cur); cur.clear(); }
        else cur += c;
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

static void save_shot(SDL_Renderer *ren, const mini::string &path) {
    SDL_Surface *s = SDL_RenderReadPixels(ren, nullptr);
    if (!s) { SDL_Log("screenshot failed: %s", SDL_GetError()); return; }
#if SDL_VERSION_ATLEAST(3, 4, 0)
    bool ok = SDL_SavePNG(s, path.c_str());
#else
    bool ok = SDL_SaveBMP(s, path.c_str());
#endif
    if (!ok) SDL_Log("screenshot %s: %s", path.c_str(), SDL_GetError());
    SDL_DestroySurface(s);
}

static int app_main(int argc, char **argv) {
    for (int k = 1; k < argc; k++)          // --crash-test: check that launcher-crash.txt is written
        if (!strcmp(argv[k], "--crash-test")) { volatile int *volatile p = nullptr; *p = 1; }
    App app;
    mini::string script;
    int want_w = 0, want_h = 0;
    bool relaunch = false;
    for (int k = 1; k < argc; k++) {
        mini::string a = argv[k];
        if (a == "--dir" && k + 1 < argc) app.dir = argv[++k];
        else if (a == "--size" && k + 1 < argc) sscanf(argv[++k], "%dx%d", &want_w, &want_h);
        else if (a == "--script" && k + 1 < argc) script = argv[++k];
        else if (a == "--relaunch") relaunch = true;   // [UIT 0272] About > Install update > Restart launcher
    }
    app.relaunched = relaunch;   // [UIT 0320] just updated: no start-up check
    if (app.dir.empty()) {
        const char *base = SDL_GetBasePath();
        app.dir = base ? base : ".";
    }
    while (app.dir.size() > 1 && (app.dir.back() == '/' || app.dir.back() == '\\')) app.dir.pop_back();
    // [UIT 0272] one launcher per folder (single_instance.h): a second copy brings the first forward and exits.
    // Scripts (the tests) run alongside one that is open.
    if (script.empty() && si_claim(app.dir, relaunch, 10000) == SI_HANDED_OVER) {
        SDL_Log("launcher: already open for %s: brought that one forward", app.dir.c_str());
        return 0;
    }
    app.exe = join(app.dir, "UltraInstinctTears.exe");
    app.exe_found = file_exists(app.exe);
    // DLSS goes through NVIDIA Streamline: setup.py copies these next to the exe from deps\streamline
    app.dll = file_exists(join(app.dir, "nvngx_dlss.dll")) && file_exists(join(app.dir, "sl.interposer.dll"));
    // [UIT 0070] the settings for game code the scan did not find are greyed out with its reason
    app.lift = parse_lift_features(read_shared(join(join(app.dir, "data"), "generated/lift-features.ini")));

    mini::string err;
    app.S = parse_schema(mini::string((const char *)kSettingsIni, kSettingsIniSize), &err);
    if (!err.empty() || app.S.empty()) {
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Ultra Instinct Tears Launcher", ("The built-in settings list is broken: " + err).c_str(), nullptr);
        return 1;
    }
    for (int i = 0; i < (int)app.S.size(); i++) app.by_key[app.S[i].key] = i;
    for (const Setting &st : app.S)
        if (st.section == "Stutter") app.expanded.insert(App::sec_gid(st));   // the old Stutter tab starts open
    {   // the settings file before the rename: copied to the new name once, the old file stays as a backup
        mini::string cfg = join(app.dir, "ultrainstincttears.cfg"), old = join(app.dir, "ultratears.cfg");
        if (!file_exists(cfg) && file_exists(old)) {
            if (SDL_CopyFile(old.c_str(), cfg.c_str())) {
                app.start_toast = "Settings moved to ultrainstincttears.cfg (ultratears.cfg kept as a backup)";
                SDL_Log("settings: copied %s to %s (the old file is kept as a backup)", old.c_str(), cfg.c_str());
            } else {
                SDL_Log("settings: could not copy %s to %s: %s (reading the old file)", old.c_str(), cfg.c_str(), SDL_GetError());
                cfg = old;
            }
        }
        app.cfg.load(cfg);
    }
    {
        SDL_PathInfo info;
        if (SDL_GetPathInfo(app.cfg.path.c_str(), &info)) app.cfg_mtime = info.modify_time;
    }
    app.gpu = detect_gpu();

    // SDL3 is per-monitor DPI aware on Windows by itself; the window asks for high pixel density below
    SDL_SetHint(SDL_HINT_WINDOWS_INTRESOURCE_ICON, "1");
    SDL_SetHint(SDL_HINT_WINDOWS_INTRESOURCE_ICON_SMALL, "1");
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "0");
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Ultra Instinct Tears Launcher", SDL_GetError(), nullptr);
        return 1;
    }
    int w = want_w, h = want_h;
    if (w <= 0 || h <= 0) {
        SDL_Rect r{0, 0, 1600, 1000};
        SDL_GetDisplayUsableBounds(SDL_GetPrimaryDisplay(), &r);
        h = (int)(r.h * 0.74f);
        w = (int)(h * 1.6f);
        if (w > r.w * 0.92f) w = (int)(r.w * 0.92f), h = (int)(w / 1.6f);
        w = mini::max(w, 960), h = mini::max(h, 600);
    }
    app.win = SDL_CreateWindow("Ultra Instinct Tears Launcher", w, h, SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIDDEN);
    if (!app.win) { SDL_Log("window: %s", SDL_GetError()); return 1; }
    SDL_SetWindowMinimumSize(app.win, 960, 600);
    app.ren = SDL_CreateRenderer(app.win, nullptr);
    if (!app.ren) { SDL_Log("renderer: %s", SDL_GetError()); return 1; }
    if (SDL_Surface *icon = SDL_LoadPNG_IO(SDL_IOFromConstMem(kUitIconPng, kUitIconPngSize), true)) {
        SDL_SetWindowIcon(app.win, icon);   // also when the exe has no icon resource
        app.logo = SDL_CreateTextureFromSurface(app.ren, icon);
        SDL_DestroySurface(icon);
    }
    SDL_SetRenderVSync(app.ren, 1);
    SDL_SetWindowPosition(app.win, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    ImFontConfig fc;
    fc.FontDataOwnedByAtlas = false;
    app.d.reg = io.Fonts->AddFontFromMemoryTTF((void *)kInterRegular, (int)kInterRegularSize, 18.0f, &fc);
    app.d.semi = io.Fonts->AddFontFromMemoryTTF((void *)kInterSemiBold, (int)kInterSemiBoldSize, 18.0f, &fc);
    ImGui_ImplSDL3_InitForSDLRenderer(app.win, app.ren);
    ImGui_ImplSDLRenderer3_Init(app.ren);

    app.cmds = split(script, ';');
    app.snap = !app.cmds.empty();
    app.mods_reload();
    if (!app.inbox.empty() && app.start_toast.empty())
        app.start_toast = app.plural((long long)app.inbox.size(), "mod is", "mods are") + " waiting in mods-inbox: Mods tab, Install from mods-inbox";
    app.build_items();
    app.clamp_focus();
    if (app.cmds.empty() && app.confirm_unasked()) app.ask_confirm_layout();   // [UIT 0270] scripts: the firstrun command
    SDL_ShowWindow(app.win);
    app.now = SDL_GetTicksNS() / 1e9;
    if (app.cmds.empty()) app.startup_check(App::kUcheckDelay);   // [UIT 0320] scripts: the startupcheck command

    mini::vector<SDL_Gamepad *> pads;
    Uint64 last = SDL_GetTicksNS();
    while (!app.quit) {
        SDL_Event e;
        bool minimized = SDL_GetWindowFlags(app.win) & SDL_WINDOW_MINIMIZED;
        if (si_raise_requested()) {   // [UIT 0272] a second copy was started: this one comes forward (also while the game runs)
            SDL_ShowWindow(app.win);
            if (minimized) SDL_RestoreWindow(app.win);
            SDL_RaiseWindow(app.win);
            minimized = false;
            SDL_Log("launcher: started again: window brought forward");
        }
        // [UIT 0304] while the game runs the launcher works only when it is the front window (settings can be changed;
        // they take effect at the next start), at no more than ~30 frames a second; otherwise it only waits for the game
        bool front = !minimized && (!app.cmds.empty() || (SDL_GetWindowFlags(app.win) & SDL_WINDOW_INPUT_FOCUS));
        if ((app.running && !front) || (minimized && app.cmds.empty())) SDL_WaitEventTimeout(nullptr, 250);
        else if (app.running && app.cmds.empty()) SDL_WaitEventTimeout(nullptr, 30);
        while (SDL_PollEvent(&e)) {
            switch (e.type) {
            case SDL_EVENT_QUIT: app.quit = true; break;
            case SDL_EVENT_WINDOW_FOCUS_GAINED:
                app.reload_if_changed();   // also while the game runs: its overlay may have saved a setting
                if (!app.running) app.mods_check = 0;   // a mod copied in by hand shows at once
                front = !minimized;
                break;
            case SDL_EVENT_WINDOW_FOCUS_LOST:   // [UIT 0304] saved before the game (its overlay) can write the file again
                if (app.save_at >= 0) { app.save_at = 0; app.save_now(); }
                if (app.cmds.empty()) front = false;
                break;
            case SDL_EVENT_DROP_BEGIN: app.drop_open = true; break;
            case SDL_EVENT_DROP_FILE:
                if (e.drop.data) app.dropped.push_back(e.drop.data);
                break;
            case SDL_EVENT_DROP_COMPLETE: app.drop_open = false; break;
            case SDL_EVENT_GAMEPAD_ADDED:
                if (SDL_Gamepad *p = SDL_OpenGamepad(e.gdevice.which)) pads.push_back(p);
                break;
            case SDL_EVENT_GAMEPAD_REMOVED:
                for (size_t k = 0; k < pads.size(); k++)
                    if (SDL_GetGamepadID(pads[k]) == e.gdevice.which) { SDL_CloseGamepad(pads[k]); pads.erase(pads.begin() + k); break; }
                break;
            default: break;
            }
            if (app.running && !front) continue;      // the game has the controller and keyboard now
            switch (e.type) {
            case SDL_EVENT_KEY_DOWN: app.key_down(e.key); break;
            case SDL_EVENT_TEXT_INPUT:
                app.text_input(e.text.text);
                break;
            case SDL_EVENT_GAMEPAD_BUTTON_DOWN: app.pad_button(e.gbutton.button, true); break;
            case SDL_EVENT_GAMEPAD_BUTTON_UP: app.pad_button(e.gbutton.button, false); break;
            case SDL_EVENT_GAMEPAD_AXIS_MOTION: app.pad_axis(e.gaxis.axis, e.gaxis.value / 32767.0f); break;
            case SDL_EVENT_MOUSE_MOTION:
                if (mini::fabs(e.motion.xrel) + mini::fabs(e.motion.yrel) > 1.5f || app.dev == Device::Keyboard) {
                    app.mx = e.motion.x, app.my = e.motion.y;
                    app.mouse_moved = true;
                    app.dev = Device::Keyboard;
                    if (app.drag >= 0) app.slider_set(app.drag_hit, app.mx);
                }
                break;
            case SDL_EVENT_MOUSE_BUTTON_DOWN:
                app.dev = Device::Keyboard;
                if (e.button.button == SDL_BUTTON_LEFT) app.click(e.button.x, e.button.y);
                else if (e.button.button == SDL_BUTTON_X1) app.push(A_Back);
                break;
            case SDL_EVENT_MOUSE_BUTTON_UP:
                if (e.button.button == SDL_BUTTON_LEFT) app.drag = -1;
                break;
            case SDL_EVENT_MOUSE_WHEEL:
                if (app.modal.kind == Modal::None && app.tab != TAB_PLAY && app.tab != TAB_ABOUT) {
                    app.scroll_to[app.tab] -= e.wheel.y * app.d.px(90);
                    app.follow = false;
                    app.mouse_moved = true;
                }
                break;
            default: break;
            }
        }
        if (app.eat_slash) app.eat_slash--;
        Uint64 t = SDL_GetTicksNS();
        app.dt = mini::min(0.1, (t - last) / 1e9);
        last = t;
        app.now = t / 1e9;
        if (!app.start_toast.empty()) app.flash(app.start_toast), app.start_toast.clear();
        if (app.running) app.poll_game();
        app.ucheck_poll();   // [UIT 0320] also in the background: stops the check if the game is running
        if (app.running && !front) continue;
        app.poll_task();
        if (!app.dropped.empty() && !app.drop_open) app.drop_done();
        app.mods_poll();
        if (!app.cmds.empty()) app.script_step();
        app.pad_seen(pads);
        if (app.modal.kind == Modal::Bind && !app.snap && app.now >= app.modal.bind_until) app.bind_cancel("No press in time");
        app.pad_repeat(pads);
        app.run_actions();
        if (app.save_at >= 0 && app.now >= app.save_at) app.save_now();
        if (SDL_GetWindowFlags(app.win) & SDL_WINDOW_MINIMIZED) continue;

        ImGui_ImplSDLRenderer3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();
        app.frame();
        ImGui::Render();
        SDL_SetRenderDrawColor(app.ren, 14, 19, 21, 255);
        SDL_RenderClear(app.ren);
        ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), app.ren);
        if (!app.shot_path.empty()) { save_shot(app.ren, app.shot_path); app.shot_path.clear(); }
        SDL_RenderPresent(app.ren);
        if (!app.cmds.empty() && app.cmd_i >= app.cmds.size() && app.cmd_wait == 0) app.quit = true;
    }
    if (app.save_at >= 0) { app.save_at = 0; app.save_now(); }
    app.ucheck_stop("the launcher closed");   // [UIT 0320]

    ImGui_ImplSDLRenderer3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    if (app.logo) SDL_DestroyTexture(app.logo);
    ImGui::DestroyContext();
    for (auto *p : pads) SDL_CloseGamepad(p);
    SDL_DestroyRenderer(app.ren);
    SDL_DestroyWindow(app.win);
    SDL_Quit();
    return 0;
}

// What SDL_main_impl.h would add: SDL_RunApp gives app_main UTF-8 arguments on Windows (from the wide command line)
// and marks SDL's main as ready. The GUI subsystem (WinMainCRTStartup in crt2.o) still starts at main().
int main(int argc, char **argv) {
    install_crash_handler();
#ifdef _WIN32
    (void)argc, (void)argv;
    return SDL_RunApp(0, nullptr, app_main, nullptr);     // NULL: SDL reads the wide command line
#else
    return SDL_RunApp(argc, argv, app_main, nullptr);
#endif
}
