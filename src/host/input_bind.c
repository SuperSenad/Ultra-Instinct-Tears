// [UIT 0174] The bindings as data (see input_bind.h).
#include "input_bind.h"

#include <stdio.h>
#include <string.h>

#include "input_map.h"

_Static_assert(UIT_BUTTON_COUNT == UIT_BIND_BUTTONS, "input_bind.h UIT_BIND_BUTTONS");
_Static_assert(UIT_PAD_INPUT_COUNT == UIT_BIND_PAD_INPUTS, "input_bind.h UIT_BIND_PAD_INPUTS");
_Static_assert(UIT_KEY_COUNT < 256, "UitBindList holds key indices in a byte");
_Static_assert(UIT_PAD_INPUT_COUNT <= UIT_BIND_MAX, "a hand-written list of every controller input still fits");

// What each Switch button does, as launcher/settings.ini says it (TOTK_PAD_* summary; tools/tests/test_overlay.cpp)
static const char *const kDoes[UIT_BIND_BUTTONS] = {
    "talk, pick up, open and confirm", "sprint (hold), cancel and back", "jump", "attack with your weapon",
    "open the abilities wheel (hold) and use the chosen ability", "throw (hold) and the fuse/throw menu",
    "lock on to a target and raise your shield", "draw and fire the bow", "open the inventory menu", "open the map",
    "crouch and sneak", "look through the scope", "the quick menu for materials", "whistle for your horse",
    "the quick menu for shields", "the quick menu for weapons",
};

// launcher/model.cpp pad_label: the label printed on each family's controller, per position token
static const struct { const char *tok, *l[4]; } kPadLabels[UIT_BIND_PAD_INPUTS] = {
    {"south", {"Bottom button", "A", "Cross", "B"}},           {"east", {"Right button", "B", "Circle", "A"}},
    {"west", {"Left button", "X", "Square", "Y"}},             {"north", {"Top button", "Y", "Triangle", "X"}},
    {"leftshoulder", {"L shoulder", "LB", "L1", "L"}},         {"rightshoulder", {"R shoulder", "RB", "R1", "R"}},
    {"lefttrigger", {"L trigger", "LT", "L2", "ZL"}},          {"righttrigger", {"R trigger", "RT", "R2", "ZR"}},
    {"start", {"Start", "Menu", "Options", "+"}},              {"back", {"Back", "View", "Create", "-"}},
    {"guide", {"Guide", "Xbox", "PS", "Home"}},                {"leftstick", {"L stick press", "LS", "L3", "L stick"}},
    {"rightstick", {"R stick press", "RS", "R3", "R stick"}},  {"dpup", {"D-pad up", "D-pad up", "D-pad up", "D-pad up"}},
    {"dpdown", {"D-pad down", "D-pad down", "D-pad down", "D-pad down"}},
    {"dpleft", {"D-pad left", "D-pad left", "D-pad left", "D-pad left"}},
    {"dpright", {"D-pad right", "D-pad right", "D-pad right", "D-pad right"}},
    {"misc1", {"Share", "Share", "Mic", "Capture"}},           {"touchpad", {"Touchpad", "Touchpad", "Touchpad", "Touchpad"}},
    {"rightpaddle1", {"Paddle P1", "P1", "Paddle R4", "Paddle"}}, {"leftpaddle1", {"Paddle P3", "P3", "Paddle L4", "Paddle"}},
    {"rightpaddle2", {"Paddle P2", "P2", "Paddle R5", "Paddle"}}, {"leftpaddle2", {"Paddle P4", "P4", "Paddle L5", "Paddle"}},
};

static int ok_button(int b) { return b >= 0 && b < UIT_BIND_BUTTONS; }
static int ok_kind(int k) { return k == UIT_BIND_PAD || k == UIT_BIND_KEY; }

const char *uit_bind_cfg_key(int kind, int b) {
    if (!ok_button(b) || !ok_kind(kind)) return NULL;
    return kind == UIT_BIND_PAD ? kUitButtons[b].pad_key : kUitButtons[b].key_key;
}
const char *uit_bind_button_name(int b) { return ok_button(b) ? kUitButtons[b].name : ""; }
const char *uit_bind_button_does(int b) { return ok_button(b) ? kDoes[b] : ""; }
uint64_t uit_bind_button_bit(int b) { return ok_button(b) ? kUitButtons[b].bit : 0; }

int uit_bind_input_count(int kind) { return kind == UIT_BIND_PAD ? UIT_PAD_INPUT_COUNT : kind == UIT_BIND_KEY ? UIT_KEY_COUNT : 0; }
const char *uit_bind_token(int kind, int in) {
    if (in < 0 || in >= uit_bind_input_count(kind)) return "";
    return kind == UIT_BIND_PAD ? kUitPadInputs[in].token : kUitKeys[in].token;
}
const char *uit_bind_label(int kind, int in, int fam) {
    if (in < 0 || in >= uit_bind_input_count(kind)) return "?";
    if (kind == UIT_BIND_KEY) return kUitKeys[in].label;
    if (fam < 0 || fam > 3) fam = UIT_PADFAM_GENERIC;
    return strcmp(kPadLabels[in].tok, kUitPadInputs[in].token) ? kUitPadInputs[in].token : kPadLabels[in].l[fam];
}
int uit_bind_key_is_stick(int in) {
    if (in < 0 || in >= UIT_KEY_COUNT || !kUitKeys[in].stick) return 0;
    SDL_Scancode sc = kUitKeys[in].sc;
    return sc == SDL_SCANCODE_W || sc == SDL_SCANCODE_A || sc == SDL_SCANCODE_S || sc == SDL_SCANCODE_D ? 1 : 2;
}

int uit_bind_pad_from_phys(int bit) {
    for (int i = 0; i < UIT_PAD_INPUT_COUNT; i++) {
        const UitPadInput *p = &kUitPadInputs[i];
        if (p->button >= 0 ? p->button == bit
                           : (bit == 30 && p->axis == SDL_GAMEPAD_AXIS_LEFT_TRIGGER) || (bit == 31 && p->axis == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER))
            return i;
    }
    return -1;
}
uint32_t uit_bind_pad_phys(int in) {
    if (in < 0 || in >= UIT_PAD_INPUT_COUNT) return 0;
    const UitPadInput *p = &kUitPadInputs[in];
    if (p->button >= 0) return p->button < 30 ? 1u << p->button : 0;
    return p->axis == SDL_GAMEPAD_AXIS_LEFT_TRIGGER ? 1u << 30 : p->axis == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER ? 1u << 31 : 0;
}
int uit_bind_key_from_scancode(int sc) { return uit_key_from_scancode((SDL_Scancode)sc); }

// ---------------------------------------------------------------- lists
typedef struct { int kind; UitBindList *l; int bad; } Parse;

static void add(UitBindList *l, int in) {
    for (int i = 0; i < l->n; i++)
        if (l->in[i] == in) return;   // the launcher keeps each token once (bind_list)
    if (l->n < UIT_BIND_MAX) l->in[l->n++] = (uint8_t)in;
}

static void parse_token(const char *t, size_t n, void *u) {
    Parse *p = u;
    if (uit_token_eq(t, n, "none")) return;
    int i = p->kind == UIT_BIND_PAD ? uit_pad_find(t, n) : uit_key_find(t, n);
    if (i < 0 || (p->kind == UIT_BIND_KEY && kUitKeys[i].stick)) { p->bad++; return; }
    add(p->l, i);
}

static const char *def_value(int kind, int b) { return kind == UIT_BIND_PAD ? kUitButtons[b].pad_def : kUitButtons[b].key_def; }

int uit_bind_parse(int kind, int b, const char *v, UitBindList *out) {
    memset(out, 0, sizeof *out);
    if (!ok_button(b) || !ok_kind(kind)) return 0;
    Parse p = {kind, out, 0};
    if (!v || !*v) v = def_value(kind, b);
    uit_tokens(v, parse_token, &p);
    if (!out->n && p.bad) {   // nothing usable: the default rather than lose the button (window_sdl.c, model.cpp read_value)
        int bad = p.bad;
        uit_tokens(def_value(kind, b), parse_token, &p);
        p.bad = bad;
    }
    return p.bad;
}

void uit_binds_default(UitBinds *b) {
    for (int k = 0; k < 2; k++)
        for (int i = 0; i < UIT_BIND_BUTTONS; i++) uit_bind_parse(k, i, NULL, &b->b[k][i]);
}

int uit_binds_load(UitBinds *b, const char *(*get)(const char *), int log) {
    int custom = 0;
    for (int i = 0; i < UIT_BIND_BUTTONS; i++)
        for (int k = 0; k < 2; k++) {
            const char *key = uit_bind_cfg_key(k, i), *v = get ? get(key) : NULL;
            int bad = uit_bind_parse(k, i, v, &b->b[k][i]);
            if (log && bad)
                fprintf(stderr, "[totk] %s=%s: %d unusable %s (%s)\n", key, v, bad, k ? "key(s)" : "controller input(s)",
                        b->b[k][i].n ? "ignored" : "using the default");
            if (uit_bind_is_default(b, k, i)) continue;
            custom++;
            if (log) {
                char t[160];
                uit_binds_cfg_value(b, k, i, t, sizeof t);
                fprintf(stderr, "[totk] controls: Switch %s = %s %s\n", kUitButtons[i].name, k ? "key" : "controller", t);
            }
        }
    if (log && custom) fprintf(stderr, "[totk] controls: %d binding(s) changed from the defaults\n", custom);
    return custom;
}

int uit_bind_list_eq(const UitBindList *a, const UitBindList *b) {
    return a->n == b->n && !memcmp(a->in, b->in, a->n);
}

int uit_bind_is_default(const UitBinds *b, int kind, int i) {
    UitBindList d;
    uit_bind_parse(kind, i, NULL, &d);
    return uit_bind_list_eq(&b->b[kind][i], &d);
}

int uit_binds_cfg_value(const UitBinds *b, int kind, int i, char *out, size_t n) {
    if (n) out[0] = 0;
    if (!ok_button(i) || !ok_kind(kind) || uit_bind_is_default(b, kind, i)) return 0;
    const UitBindList *l = &b->b[kind][i];
    if (!l->n) { snprintf(out, n, "none"); return 1; }
    size_t used = 0;
    for (int k = 0; k < l->n && used < n; k++) {
        int w = snprintf(out + used, n - used, "%s%s", k ? "," : "", uit_bind_token(kind, l->in[k]));
        if (w < 0) break;
        used += (size_t)w;
    }
    return 1;
}

void uit_binds_text(const UitBinds *b, int kind, int i, int fam, char *out, size_t n) {
    if (n) out[0] = 0;
    if (!ok_button(i) || !ok_kind(kind)) return;
    const UitBindList *l = &b->b[kind][i];
    if (!l->n) { snprintf(out, n, "nothing"); return; }
    size_t used = 0;
    for (int k = 0; k < l->n && used < n; k++) {
        int w = snprintf(out + used, n - used, "%s%s", k ? " or " : "", uit_bind_label(kind, l->in[k], fam));
        if (w < 0) break;
        used += (size_t)w;
    }
}

int uit_binds_assign(UitBinds *b, int kind, int i, int in, int *moved) {
    if (moved) *moved = -1;
    if (!ok_button(i) || !ok_kind(kind) || in < 0 || in >= uit_bind_input_count(kind)) return 0;
    if (kind == UIT_BIND_KEY && kUitKeys[in].stick) return 0;
    UitBindList old = b->b[kind][i];
    if (old.n == 1 && old.in[0] == in) return 0;
    for (int j = 0; j < UIT_BIND_BUTTONS; j++) {
        if (j == i) continue;
        UitBindList *l = &b->b[kind][j];
        int at = -1;
        for (int k = 0; k < l->n; k++)
            if (l->in[k] == in) at = k;
        if (at < 0) continue;
        memmove(&l->in[at], &l->in[at + 1], (size_t)(l->n - at - 1));
        l->n--;
        if (!l->n) *l = old;   // a swap: the other button takes this one's old binding
        if (moved) *moved = j;
    }
    UitBindList one = {1, {(uint8_t)in}};
    b->b[kind][i] = one;
    return 1;
}

void uit_binds_clear(UitBinds *b, int kind, int i) {
    if (ok_button(i) && ok_kind(kind)) memset(&b->b[kind][i], 0, sizeof b->b[kind][i]);
}

void uit_binds_reset(UitBinds *b, int kind, int i) {
    if (!ok_kind(kind)) return;
    if (i < 0) {
        for (int k = 0; k < UIT_BIND_BUTTONS; k++) uit_bind_parse(kind, k, NULL, &b->b[kind][k]);
    } else if (ok_button(i))
        uit_bind_parse(kind, i, NULL, &b->b[kind][i]);
}

void uit_binds_compile(const UitBinds *b, uint64_t pad[UIT_BIND_PAD_INPUTS], uint64_t key[128]) {
    memset(pad, 0, UIT_BIND_PAD_INPUTS * sizeof *pad);
    memset(key, 0, 128 * sizeof *key);
    for (int i = 0; i < UIT_BIND_BUTTONS; i++) {
        for (int k = 0; k < b->b[UIT_BIND_PAD][i].n; k++) pad[b->b[UIT_BIND_PAD][i].in[k]] |= kUitButtons[i].bit;
        for (int k = 0; k < b->b[UIT_BIND_KEY][i].n; k++) {
            int m = kUitKeys[b->b[UIT_BIND_KEY][i].in[k]].mac;
            if (m >= 0 && m < 128) key[m] |= kUitButtons[i].bit;
        }
    }
}
