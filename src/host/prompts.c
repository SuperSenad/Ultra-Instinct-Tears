/* [UIT 0452] Button prompts that match the controller: see prompts.h.
 *
 * The file formats (the button font's code points, the bfotf wrapper, the texture names, R8 block-linear images and
 * the one-letter panes) follow valentderah/totk-pc-recomp tools/make_romfs_patches.py and tools/button_icons.py
 * (reference commit on main, November 2026; MIT, Copyright (c) 2026 Valiantsin Dzerakh, licenses/valentderah-totk-recomp.txt),
 * themselves adapted from MacNativeGames (MIT). That tool rebuilds the files once with fontTools at setup; this is
 * our own C, run in memory on the player's files at start-up, and it never changes a file's size: a font glyph that
 * grows takes its room from the glyphs it replaces (the CFF CharStrings data between them is laid out again), so the
 * resource size table needs no change. The icons are our own (prompt_icons.h). */
#include "prompts.h"

#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "prompt_icons.h"
#include "uit_str.h"
#include "../runtime/logline.h"
#define LOG(...) uit_logline("[totk] button prompts: " __VA_ARGS__)

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint64_t rd64(const uint8_t *p) { return rd32(p) | (uint64_t)rd32(p + 4) << 32; }
static uint16_t be16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }
static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static void wbe16(uint8_t *p, unsigned v) { p[0] = (uint8_t)(v >> 8), p[1] = (uint8_t)v; }
static void wbe32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 24), p[1] = (uint8_t)(v >> 16), p[2] = (uint8_t)(v >> 8), p[3] = (uint8_t)v; }

/* ======================================== the style ======================================== */
static int (*detector)(void);
void uit_prompts_set_detector(int (*detect)(void)) { detector = detect; }
static const char *const kNames[] = {"switch", "xbox", "ps4", "ps5"};
const char *uit_prompts_name(int s) { return s >= 0 && s < 4 ? kNames[s] : "switch"; }
static int parse_style(const char *v) {
    for (int i = 0; v && i < 4; i++) if (!uit_stricmp(v, kNames[i])) return i;
    return -1;
}
static void remember_path(char *out, size_t cap) {
    const char *dir = getenv("TOTK_PROFILE_DIR");   /* as runtime_profile_dir (account.c) */
    snprintf(out, cap, "%s/button-prompts-auto.txt", dir && *dir ? dir : "userdata");
}
static pthread_once_t style_once = PTHREAD_ONCE_INIT;
static int style_v;
static void style_init(void) {
    const char *v = getenv("TOTK_BUTTON_PROMPTS");
    style_v = UIT_PROMPTS_SWITCH;
    if (v && !uit_stricmp(v, "switch")) return;
    if (!v || !*v || !strcmp(v, "-")) v = "auto";   /* [UIT 0453] Auto is the default */
    int s = parse_style(v);
    if (s >= 0) { style_v = s; LOG("%s (Game > Controller > Button prompts)", kNames[s]); return; }
    if (uit_stricmp(v, "auto")) { LOG("TOTK_BUTTON_PROMPTS=%s is not a style this build knows: the Switch buttons stay", v); return; }
    char path[1100];
    remember_path(path, sizeof path);
    int d = detector ? detector() : -1;
    if (d >= 0 && d < 4) {
        style_v = d;
        FILE *f = fopen(path, "wb");
        if (f) { fprintf(f, "%s\n", kNames[d]); fclose(f); }
        LOG("Auto: %s, from the controller connected at start (restart the game after changing controllers)", kNames[d]);
        return;
    }
    FILE *f = fopen(path, "rb");
    char buf[32] = "";
    if (f) {
        if (!fgets(buf, sizeof buf, f)) buf[0] = 0;
        fclose(f);
    }
    buf[strcspn(buf, "\r\n ")] = 0;
    s = parse_style(buf);
    if (s >= 0) { style_v = s; LOG("Auto: no controller connected at start; %s, the last one Auto saw", kNames[s]); return; }
    LOG("Auto: no controller connected at start and none seen before: the Switch buttons stay");
}
int uit_prompts_style(void) { pthread_once(&style_once, style_init); return style_v; }

/* ======================================== which picture each Switch button gets ======================================== */
static const char *const kPadKeys[PB_COUNT] = {"TOTK_PAD_A", "TOTK_PAD_B", "TOTK_PAD_X", "TOTK_PAD_Y", "TOTK_PAD_L",
                                               "TOTK_PAD_R", "TOTK_PAD_ZL", "TOTK_PAD_ZR", "TOTK_PAD_PLUS", "TOTK_PAD_MINUS"};
/* the defaults of input_map.h kUitButtons (tools/tests/test_prompts.c checks they agree) */
static const char *const kPadDefs[PB_COUNT] = {"east", "south", "north", "west", "leftshoulder", "rightshoulder",
                                               "lefttrigger", "righttrigger", "start", "back"};
static const char *env_get(const char *k) { return getenv(k); }

int prompts_map(int style, const char *(*get)(const char *), PromptMap *m) {
    memset(m, -1, sizeof m->game + sizeof m->icon);
    memset(m->letter, 0, sizeof m->letter);
    m->style = style;
    if (style <= UIT_PROMPTS_SWITCH || style > UIT_PROMPTS_PS5) return 0;
    if (!get) get = env_get;
    int ps = style != UIT_PROMPTS_XBOX, changed = 0;
    for (int b = 0; b < PB_COUNT; b++) {
        const char *v = get(kPadKeys[b]);
        if (!v || !*v) v = kPadDefs[b];
        char tok[32];
        size_t k = 0;
        while (*v == ' ') v++;
        while (*v && *v != ',' && *v != ' ' && k + 1 < sizeof tok) tok[k++] = *v++;
        tok[k] = 0;
        int game = -1, icon = -1;
        if (!uit_stricmp(tok, "south")) game = PB_A, icon = PI_PS_CROSS;
        else if (!uit_stricmp(tok, "east")) game = PB_B, icon = PI_PS_CIRCLE;
        else if (!uit_stricmp(tok, "west")) game = PB_X, icon = PI_PS_SQUARE;
        else if (!uit_stricmp(tok, "north")) game = PB_Y, icon = PI_PS_TRIANGLE;
        else if (!uit_stricmp(tok, "leftshoulder")) icon = ps ? PI_PS_L1 : PI_XB_LB;
        else if (!uit_stricmp(tok, "rightshoulder")) icon = ps ? PI_PS_R1 : PI_XB_RB;
        else if (!uit_stricmp(tok, "lefttrigger")) icon = ps ? PI_PS_L2 : PI_XB_LT;
        else if (!uit_stricmp(tok, "righttrigger")) icon = ps ? PI_PS_R2 : PI_XB_RT;
        else if (!uit_stricmp(tok, "start")) icon = ps ? PI_PS_OPTIONS : PI_XB_MENU;
        else if (!uit_stricmp(tok, "back")) icon = style == UIT_PROMPTS_PS5 ? PI_PS5_CREATE : ps ? PI_PS4_SHARE : PI_XB_VIEW;
        /* else: unbound or a button with no picture of its own (a paddle, the touchpad): the game's picture stays */
        if (ps) game = -1;           /* PlayStation: our symbols */
        else if (game >= 0) icon = -1;   /* Xbox face buttons: the game's own letters */
        if (game == b) game = -1;    /* already that picture */
        m->game[b] = (signed char)game, m->icon[b] = (signed char)icon;
        if (b <= PB_Y && game >= 0) m->letter[b] = "ABXY"[game];
        changed += game >= 0 || icon >= 0;
    }
    return changed;
}

int prompts_wants(const char *path) {
    if (!path) return 0;
    size_t n = strlen(path);
    if (!strncmp(path, "Font/Font", 9) && n > 20 && !strcmp(path + n - 20, ".Nin_NX_NVN.bfarc.zs"))
        return uit_prompts_style() != UIT_PROMPTS_SWITCH;
    if (!strncmp(path, "Mals/", 5) && n > 8 && !strcmp(path + n - 8, ".sarc.zs") && !strchr(path + 5, '/'))
        return uit_prompts_style() == UIT_PROMPTS_XBOX;
    return 0;
}

/* ======================================== icon outlines ======================================== */
#define ICON_CY 302.5
typedef struct { double a, b, c, d; } Xf;   /* x' = x * a + b, y' = y * c + d */
static void icon_bounds(int icon, double *x0, double *y0, double *x1, double *y1) {
    const int16_t *o = kPromptIcons[icon].ops;
    *x0 = *y0 = 1e9, *x1 = *y1 = -1e9;
    for (size_t i = 0; o[i];) {
        int op = o[i++], np = op == 1 || op == 2 ? 1 : op == 3 ? 3 : 0;
        for (int k = 0; k < np; k++, i += 2) {
            if (o[i] < *x0) *x0 = o[i];
            if (o[i] > *x1) *x1 = o[i];
            if (o[i + 1] < *y0) *y0 = o[i + 1];
            if (o[i + 1] > *y1) *y1 = o[i + 1];
        }
    }
}

/* ---- Type 2 charstrings ---- */
typedef struct { int32_t v; uint8_t op, esc, size; } Tok;   /* op 0: a number of v (size: its encoded bytes) */
typedef struct { Tok *t; size_t n, cap; } Toks;
static int tk_push(Toks *s, int32_t v, uint8_t op) {
    if (s->n == s->cap) {
        Tok *g = realloc(s->t, (s->cap = s->cap ? s->cap * 2 : 256) * sizeof *g);
        if (!g) return -1;
        s->t = g;
    }
    uint8_t size = 0;
    if (!op) size = v >= -107 && v <= 107 ? 1 : (v >= -1131 && v <= 1131) ? 2 : 3;
    s->t[s->n++] = (Tok){v, op, 0, size};
    return 0;
}
static size_t tk_encode(const Toks *s, uint8_t *out, size_t cap) {
    size_t at = 0;
    for (size_t i = 0; i < s->n; i++) {
        const Tok *t = &s->t[i];
        if (at + 3 > cap) return 0;
        if (t->op) { out[at++] = t->op; continue; }
        int32_t v = t->v;
        if (t->size == 1) out[at++] = (uint8_t)(v + 139);
        else if (t->size == 2 && v > 0) out[at++] = (uint8_t)(247 + ((v - 108) >> 8)), out[at++] = (uint8_t)((v - 108) & 255);
        else if (t->size == 2) out[at++] = (uint8_t)(251 + ((-v - 108) >> 8)), out[at++] = (uint8_t)((-v - 108) & 255);
        else out[at++] = 28, out[at++] = (uint8_t)((uint16_t)v >> 8), out[at++] = (uint8_t)v;
    }
    return at;
}
enum { T2_RMOVETO = 21, T2_RLINETO = 5, T2_HLINETO = 6, T2_VLINETO = 7, T2_RRCURVETO = 8, T2_ENDCHAR = 14,
       T2_VHCURVETO = 30, T2_HVCURVETO = 31, T2_HHCURVETO = 27, T2_VVCURVETO = 26 };
/* the outline of icon through xf as Type 2 tokens (width first); xmin: the outline's least x */
static int tk_outline(Toks *s, int icon, const Xf *xf, int width, int *xmin) {
    const int16_t *o = kPromptIcons[icon].ops;
    int cx = 0, cy = 0, first = 1, mn = 1 << 30;
    for (size_t i = 0; o[i];) {
        int op = o[i++];
        if (op == 4) continue;   /* contours close by themselves */
        int np = op == 3 ? 3 : 1, px[3], py[3];
        for (int k = 0; k < np; k++, i += 2) {
            px[k] = (int)lround(o[i] * xf->a + xf->b), py[k] = (int)lround(o[i + 1] * xf->c + xf->d);
            if (px[k] < mn) mn = px[k];
        }
        int r = 0;
        if (op == 1) {
            if (first) r |= tk_push(s, width, 0), first = 0;
            r |= tk_push(s, px[0] - cx, 0) | tk_push(s, py[0] - cy, 0) | tk_push(s, 0, T2_RMOVETO);
        } else if (op == 2) {
            int dx = px[0] - cx, dy = py[0] - cy;
            if (!dx && !dy) continue;
            if (!dy) r |= tk_push(s, dx, 0) | tk_push(s, 0, T2_HLINETO);
            else if (!dx) r |= tk_push(s, dy, 0) | tk_push(s, 0, T2_VLINETO);
            else r |= tk_push(s, dx, 0) | tk_push(s, dy, 0) | tk_push(s, 0, T2_RLINETO);
        } else {
            int a = px[0] - cx, b = py[0] - cy, c = px[1] - px[0], d = py[1] - py[0], e = px[2] - px[1], f = py[2] - py[1];
            if (!b && !e) r |= tk_push(s, a, 0) | tk_push(s, c, 0) | tk_push(s, d, 0) | tk_push(s, f, 0) | tk_push(s, 0, T2_HVCURVETO);
            else if (!a && !f) r |= tk_push(s, b, 0) | tk_push(s, c, 0) | tk_push(s, d, 0) | tk_push(s, e, 0) | tk_push(s, 0, T2_VHCURVETO);
            else if (!b && !f) r |= tk_push(s, a, 0) | tk_push(s, c, 0) | tk_push(s, d, 0) | tk_push(s, e, 0) | tk_push(s, 0, T2_HHCURVETO);
            else if (!a && !e) r |= tk_push(s, b, 0) | tk_push(s, c, 0) | tk_push(s, d, 0) | tk_push(s, f, 0) | tk_push(s, 0, T2_VVCURVETO);
            else r |= tk_push(s, a, 0) | tk_push(s, b, 0) | tk_push(s, c, 0) | tk_push(s, d, 0) | tk_push(s, e, 0) | tk_push(s, f, 0) |
                      tk_push(s, 0, T2_RRCURVETO);
        }
        if (r) return -1;
        cx = px[np - 1], cy = py[np - 1];
    }
    if (first) return -1;
    *xmin = mn;
    return 0;
}
/* exactly pad more bytes, same outline: numbers written in a longer form (1 -> 3 bytes: +2, 2 -> 3 bytes: +1) and,
   when that is not enough, moves of 0 0 before endchar (a move that draws nothing) */
static int tk_pad(Toks *s, size_t pad) {
    if (!pad) return 0;
    size_t n1 = 0, n2 = 0;
    for (size_t i = 0; i < s->n; i++) if (!s->t[i].op) n1 += s->t[i].size == 1, n2 += s->t[i].size == 2;
    for (size_t k = 0; k * 3 <= pad; k++) {
        size_t rest = pad - k * 3, m1 = n1 + 2 * k;   /* each move brings two one-byte zeros */
        size_t a = rest / 2 < m1 ? rest / 2 : m1, b = rest - 2 * a;
        if (b > n2) continue;
        for (size_t j = 0; j < k; j++)
            if (tk_push(s, 0, 0) || tk_push(s, 0, 0) || tk_push(s, 0, T2_RMOVETO)) return -1;
        for (size_t i = 0; i < s->n && (a || b); i++) {
            Tok *t = &s->t[i];
            if (t->op) continue;
            if (t->size == 1 && a) t->size = 3, a--;
            else if (t->size == 2 && b) t->size = 3, b--;
        }
        return a || b ? -1 : 0;
    }
    return -1;
}
static int cs_build(int icon, int advance, int nominal, size_t pad, uint8_t *out, size_t cap, size_t *len, int *xmin) {
    double x0, y0, x1, y1;
    icon_bounds(icon, &x0, &y0, &x1, &y1);
    double w = x1 - x0, room = advance - 40.0, k = w > room && room > 0 ? room / w : 1.0;
    Xf xf = {k, advance / 2.0 - (x0 + x1) / 2.0 * k, k, ICON_CY - ICON_CY * k};
    Toks s = {0};
    int ok = !tk_outline(&s, icon, &xf, advance - nominal, xmin);
    if (ok) ok = !tk_pad(&s, pad) && !tk_push(&s, 0, T2_ENDCHAR);   /* the padding moves, then endchar */
    size_t n = ok ? tk_encode(&s, out, cap) : 0;
    free(s.t);
    *len = n;
    return n ? 0 : -1;
}
size_t prompts_charstring(int icon, int advance, int nominal_width, uint8_t *out, size_t cap, int *xmin) {
    size_t n = 0;
    if (icon < 0 || icon >= PI_COUNT || cs_build(icon, advance, nominal_width, 0, out, cap, &n, xmin)) return 0;
    return n;
}

/* ---- rasterizing (the HUD's button images) ---- */
typedef struct { float x0, y0, x1, y1; } Edge;
void prompts_raster(int icon, int size, int margin, uint8_t *out) {
    memset(out, 0, (size_t)size * size);
    if (icon < 0 || icon >= PI_COUNT || size <= 2 * margin) return;
    double x0, y0, x1, y1;
    icon_bounds(icon, &x0, &y0, &x1, &y1);
    double inner = size - 2.0 * margin, bw = x1 - x0, bh = y1 - y0, k = inner / (bw > bh ? bw : bh);
    double ox = margin + (inner - bw * k) / 2 - x0 * k, oy = margin + (inner - bh * k) / 2 - y0 * k;
    size_t cap = 4096, ne = 0;
    Edge *e = malloc(cap * sizeof *e);
    if (!e) return;
    const int16_t *o = kPromptIcons[icon].ops;
    double cx = 0, cy = 0, sx = 0, sy = 0;
#define PX(X) ((X) * k + ox)
#define PY(Y) (size - ((Y) * k + oy))
#define EDGE(AX, AY, BX, BY) do { if (ne == cap) { Edge *g = realloc(e, (cap *= 2) * sizeof *e); if (!g) { free(e); return; } e = g; } \
        e[ne++] = (Edge){(float)(AX), (float)(AY), (float)(BX), (float)(BY)}; } while (0)
    for (size_t i = 0;;) {
        int op = o[i++];
        if (op == 1 || op == 4 || op == 0) {
            if (cx != sx || cy != sy) EDGE(cx, cy, sx, sy);   /* close the contour */
            if (!op) break;
            if (op == 1) cx = sx = PX(o[i]), cy = sy = PY(o[i + 1]), i += 2;
            else cx = sx, cy = sy;
        } else if (op == 2) {
            double nx = PX(o[i]), ny = PY(o[i + 1]);
            i += 2;
            EDGE(cx, cy, nx, ny);
            cx = nx, cy = ny;
        } else {
            double ax = PX(o[i]), ay = PY(o[i + 1]), bx = PX(o[i + 2]), by = PY(o[i + 3]), qx = PX(o[i + 4]), qy = PY(o[i + 5]);
            i += 6;
            double px = cx, py = cy;
            for (int t = 1; t <= 16; t++) {
                double u = t / 16.0, v = 1 - u;
                double nx = v * v * v * cx + 3 * v * v * u * ax + 3 * v * u * u * bx + u * u * u * qx;
                double ny = v * v * v * cy + 3 * v * v * u * ay + 3 * v * u * u * by + u * u * u * qy;
                EDGE(px, py, nx, ny);
                px = nx, py = ny;
            }
            cx = qx, cy = qy;
        }
    }
#undef PX
#undef PY
#undef EDGE
    enum { SS = 4 };
    int n = size * SS;
    uint32_t *cover = calloc((size_t)size * size, sizeof *cover);
    float *xs = malloc(ne * sizeof *xs);
    int *ds = malloc(ne * sizeof *ds);
    for (int row = 0; cover && xs && ds && row < n; row++) {
        float y = (row + 0.5f) / SS;
        int m = 0;
        for (size_t j = 0; j < ne; j++) {
            float ay = e[j].y0, by = e[j].y1;
            if (ay == by || y < (ay < by ? ay : by) || y >= (ay < by ? by : ay)) continue;
            float x = e[j].x0 + (y - ay) * (e[j].x1 - e[j].x0) / (by - ay);
            int d = by > ay ? 1 : -1, at = m++;
            while (at > 0 && xs[at - 1] > x) xs[at] = xs[at - 1], ds[at] = ds[at - 1], at--;
            xs[at] = x, ds[at] = d;
        }
        int wind = 0;
        float start = 0;
        for (int j = 0; j < m; j++) {
            if (!wind) start = xs[j];
            wind += ds[j];
            if (!wind) {
                int c0 = (int)lroundf(start * SS), c1 = (int)lroundf(xs[j] * SS);
                if (c0 < 0) c0 = 0;
                if (c1 > n) c1 = n;
                for (int c = c0; c < c1; c++) cover[(row / SS) * size + c / SS]++;
            }
        }
    }
    for (int i = 0; cover && i < size * size; i++) out[i] = (uint8_t)(cover[i] * 255 / (SS * SS) > 255 ? 255 : cover[i] * 255 / (SS * SS));
    free(cover), free(xs), free(ds), free(e);
}

/* ======================================== OpenType CFF: the button font ======================================== */
int prompts_bfotf_xor(uint8_t *b, size_t n) {
    if (n < 16) return 0;
    uint8_t size[4], key[4];
    wbe32(size, (uint32_t)(n - 8));
    for (int i = 0; i < 4; i++) key[i] = b[4 + i] ^ size[i];
    for (size_t i = 8; i < n; i++) b[i] ^= key[(i - 8) & 3];
    return 1;
}

typedef struct { size_t base; unsigned count, offsize; size_t end; } CffIndex;   /* base: the byte before the data */
static int cff_index(const uint8_t *d, size_t n, size_t at, CffIndex *ix) {
    if (at + 2 > n) return -1;
    ix->count = be16(d + at);
    if (!ix->count) { ix->offsize = 0, ix->base = at + 2, ix->end = at + 2; return 0; }
    if (at + 3 > n) return -1;
    ix->offsize = d[at + 2];
    if (ix->offsize < 1 || ix->offsize > 4 || at + 3 + (size_t)(ix->count + 1) * ix->offsize > n) return -1;
    ix->base = at + 3 + (size_t)(ix->count + 1) * ix->offsize - 1;
    ix->end = 0;
    size_t prev = 0;
    for (unsigned i = 0; i <= ix->count; i++) {   /* offsets start at 1 and never go down */
        size_t o = 0;
        for (unsigned k = 0; k < ix->offsize; k++) o = o << 8 | d[at + 3 + (size_t)i * ix->offsize + k];
        if (o < 1 || o < prev || ix->base + o > n) return -1;
        prev = o;
    }
    ix->end = ix->base + prev;
    return 0;
}
static size_t cff_off(const uint8_t *d, const CffIndex *ix, unsigned i) {
    size_t at = ix->base + 1 - (size_t)(ix->count + 1) * ix->offsize + (size_t)i * ix->offsize, o = 0;
    for (unsigned k = 0; k < ix->offsize; k++) o = o << 8 | d[at + k];
    return ix->base + o;
}
static void cff_set_off(uint8_t *d, const CffIndex *ix, unsigned i, size_t abs) {
    size_t at = ix->base + 1 - (size_t)(ix->count + 1) * ix->offsize + (size_t)i * ix->offsize, o = abs - ix->base;
    for (unsigned k = ix->offsize; k-- > 0;) d[at + k] = (uint8_t)o, o >>= 8;
}
/* a DICT: calls f(op, operands, count) for each operator (12 x as 1200 + x); reals become their integer part */
typedef int (*DictFn)(void *ctx, int op, const double *v, int nv);
static int cff_dict(const uint8_t *d, size_t lo, size_t hi, DictFn f, void *ctx) {
    double v[48];
    int nv = 0;
    for (size_t i = lo; i < hi;) {
        uint8_t b = d[i];
        double x;
        if (b <= 21) {
            int op = b;
            i++;
            if (b == 12) { if (i >= hi) return -1; op = 1200 + d[i++]; }
            if (f(ctx, op, v, nv)) return 1;
            nv = 0;
            continue;
        }
        if (b == 28) { if (i + 3 > hi) return -1; x = (int16_t)be16(d + i + 1), i += 3; }
        else if (b == 29) { if (i + 5 > hi) return -1; x = (int32_t)be32(d + i + 1), i += 5; }
        else if (b == 30) {   /* real: nibbles to 0xf */
            char s[64];
            size_t k = 0;
            int done = 0;
            for (i++; i < hi && !done; i++)
                for (int h = 0; h < 2 && !done; h++) {
                    int nib = h ? d[i] & 15 : d[i] >> 4;
                    const char *map[] = {"0", "1", "2", "3", "4", "5", "6", "7", "8", "9", ".", "E", "E-", "", "-", ""};
                    if (nib == 15) { done = 1; break; }
                    for (const char *p = map[nib]; *p && k + 1 < sizeof s; p++) s[k++] = *p;
                }
            s[k] = 0;
            x = atof(s);
        } else if (b >= 32 && b <= 246) x = b - 139, i++;
        else if (b >= 247 && b <= 250) { if (i + 2 > hi) return -1; x = (b - 247) * 256 + d[i + 1] + 108, i += 2; }
        else if (b >= 251 && b <= 254) { if (i + 2 > hi) return -1; x = -(b - 251) * 256 - d[i + 1] - 108, i += 2; }
        else return -1;
        if (nv < 48) v[nv++] = x;
    }
    return 0;
}
typedef struct { long charstrings, priv_size, priv_off; int cid, type; } TopDict;
static int top_fn(void *ctx, int op, const double *v, int nv) {
    TopDict *t = ctx;
    if (op == 17 && nv >= 1) t->charstrings = (long)v[nv - 1];
    else if (op == 18 && nv >= 2) t->priv_size = (long)v[nv - 2], t->priv_off = (long)v[nv - 1];
    else if (op == 1230) t->cid = 1;
    else if (op == 1206 && nv >= 1) t->type = (int)v[nv - 1];
    return 0;
}
static int priv_fn(void *ctx, int op, const double *v, int nv) {
    if (op == 21 && nv >= 1) *(long *)ctx = lround(v[nv - 1]);
    return 0;
}

typedef struct { size_t at, len; } Table;
static int sfnt_table(const uint8_t *f, size_t n, const char *tag, Table *t, size_t *rec) {
    unsigned nt = be16(f + 4);
    if (12 + (size_t)nt * 16 > n) return -1;
    for (unsigned i = 0; i < nt; i++) {
        const uint8_t *r = f + 12 + (size_t)i * 16;
        if (memcmp(r, tag, 4)) continue;
        t->at = be32(r + 8), t->len = be32(r + 12);
        if (t->at > n || t->len > n - t->at) return -1;
        if (rec) *rec = 12 + (size_t)i * 16;
        return 0;
    }
    return -1;
}
static uint32_t sfnt_sum(const uint8_t *p, size_t len) {
    uint32_t s = 0;
    for (size_t i = 0; i < len; i += 4) {
        uint8_t w[4] = {0, 0, 0, 0};
        for (size_t k = 0; k < 4 && i + k < len; k++) w[k] = p[i + k];
        s += be32(w);
    }
    return s;
}
/* the glyph of code point cp (cmap format 4 or 12 of a Unicode subtable), 0 when none */
static unsigned cmap_glyph(const uint8_t *f, const Table *t, uint32_t cp) {
    const uint8_t *c = f + t->at;
    if (t->len < 4) return 0;
    unsigned nsub = be16(c + 2);
    unsigned best = 0;
    for (unsigned pass = 0; pass < 2 && !best; pass++)
        for (unsigned i = 0; i < nsub && !best; i++) {
            if (4 + (size_t)i * 8 + 8 > t->len) return 0;
            unsigned pid = be16(c + 4 + i * 8), eid = be16(c + 6 + i * 8);
            size_t off = be32(c + 8 + i * 8);
            if (!(pid == 0 || (pid == 3 && (eid == 1 || eid == 10)))) continue;
            if (off + 8 > t->len) continue;
            const uint8_t *s = c + off;
            unsigned fmt = be16(s);
            if (fmt == 12 && pass == 0) {
                uint32_t ng = be32(s + 12);
                if (off + 16 + (size_t)ng * 12 > t->len) continue;
                for (uint32_t g = 0; g < ng; g++) {
                    uint32_t a = be32(s + 16 + g * 12), b = be32(s + 20 + g * 12), id = be32(s + 24 + g * 12);
                    if (cp >= a && cp <= b) { best = id + (cp - a); break; }
                }
            } else if (fmt == 4 && pass == 1 && cp <= 0xffff) {
                unsigned segx2 = be16(s + 6);
                size_t len = be16(s + 2);
                if (off + len > t->len || 16 + (size_t)segx2 * 4 > len) continue;
                const uint8_t *ends = s + 14, *starts = ends + segx2 + 2, *deltas = starts + segx2, *ranges = deltas + segx2;
                for (unsigned g = 0; g < segx2 / 2; g++) {
                    unsigned e = be16(ends + g * 2), st = be16(starts + g * 2);
                    if (cp > e) continue;
                    if (cp < st) break;
                    unsigned ro = be16(ranges + g * 2), d = be16(deltas + g * 2), gid;
                    if (!ro) gid = (cp + d) & 0xffff;
                    else {
                        size_t at = (size_t)(ranges + g * 2 - s) + ro + (cp - st) * 2;
                        if (at + 2 > len) break;
                        gid = be16(s + at);
                        if (gid) gid = (gid + d) & 0xffff;
                    }
                    best = gid;
                    break;
                }
            }
        }
    return best;
}

static const uint32_t kCodepoints[PB_COUNT] = {0xE040, 0xE041, 0xE042, 0xE043, 0xE044, 0xE045, 0xE046, 0xE047, 0xE04B, 0xE04C};
/* the order glyphs are taken in when the room runs short: the face buttons together first, then these */
static const int kOrder[] = {PB_ZL, PB_ZR, PB_L, PB_R, PB_PLUS, PB_MINUS};

typedef struct {
    int on;             /* will change */
    unsigned gid, src;  /* the glyph; src: the glyph copied (game pictures) */
    int icon;           /* or our icon */
    int advance, lsb;
    size_t olen;        /* its charstring now */
    uint8_t *nb;        /* the new one */
    size_t nlen;
} GlyphJob;

int prompts_otf(uint8_t *f, size_t n, const PromptMap *map) {
    if (n < 12 || memcmp(f, "OTTO", 4)) { LOG("button font: not an OpenType CFF font"); return -1; }
    Table cff, hmtx, hhea, cmap, head, maxp;
    size_t cff_rec = 0, hmtx_rec = 0, head_rec = 0;
    if (sfnt_table(f, n, "CFF ", &cff, &cff_rec) || sfnt_table(f, n, "hmtx", &hmtx, &hmtx_rec) || sfnt_table(f, n, "hhea", &hhea, NULL) ||
        sfnt_table(f, n, "cmap", &cmap, NULL) || sfnt_table(f, n, "head", &head, &head_rec) || sfnt_table(f, n, "maxp", &maxp, NULL) ||
        hhea.len < 36 || head.len < 54 || maxp.len < 6) { LOG("button font: a table is missing"); return -2; }
    if (be16(f + head.at + 18) != 1000) { LOG("button font: %u units to the em, not 1000", be16(f + head.at + 18)); return -3; }
    unsigned nglyphs = be16(f + maxp.at + 4), nhm = be16(f + hhea.at + 34);
    if (!nhm || hmtx.len < (size_t)nhm * 4 + (size_t)(nglyphs > nhm ? nglyphs - nhm : 0) * 2) { LOG("button font: short hmtx"); return -4; }
    const uint8_t *c = f + cff.at;
    size_t cn = cff.len;
    if (cn < 4 || c[0] != 1) { LOG("button font: CFF version %u", cn ? c[0] : 0); return -5; }
    CffIndex names, tops;
    if (cff_index(c, cn, c[2], &names) || cff_index(c, cn, names.end, &tops) || tops.count < 1) return -6;
    TopDict td = {0, 0, 0, 0, 2};
    if (cff_dict(c, cff_off(c, &tops, 0), cff_off(c, &tops, 1), top_fn, &td) < 0) return -7;
    if (td.cid || td.type != 2 || td.charstrings <= 0) { LOG("button font: a CID-keyed or Type %d CFF", td.type); return -8; }
    long nominal = 0;
    if (td.priv_size > 0 && (td.priv_off <= 0 || (size_t)td.priv_off + (size_t)td.priv_size > cn)) return -9;
    if (td.priv_size > 0 && cff_dict(c, (size_t)td.priv_off, (size_t)(td.priv_off + td.priv_size), priv_fn, &nominal) < 0) return -9;
    CffIndex cs;
    if (cff_index(c, cn, (size_t)td.charstrings, &cs) || cs.count != nglyphs) { LOG("button font: unexpected CharStrings"); return -10; }
    uint8_t *cc = f + cff.at;

    GlyphJob job[PB_COUNT];
    memset(job, 0, sizeof job);
    unsigned gids[PB_COUNT];
    for (int b = 0; b < PB_COUNT; b++) gids[b] = cmap_glyph(f, &cmap, kCodepoints[b]);
    int wanted = 0, missing = 0;
    for (int b = 0; b < PB_COUNT; b++) {
        if (map->game[b] < 0 && map->icon[b] < 0) continue;
        wanted++;
        unsigned g = gids[b];
        if (!g || g >= nglyphs || (map->game[b] >= 0 && (!gids[(int)map->game[b]] || gids[(int)map->game[b]] >= nglyphs))) { missing++; continue; }
        for (int o = 0; o < b; o++) if (job[o].on && job[o].gid == g) g = 0;   /* two code points on one glyph */
        if (!g) { missing++; continue; }
        GlyphJob *j = &job[b];
        j->gid = g, j->icon = map->icon[b], j->src = map->game[b] >= 0 ? gids[(int)map->game[b]] : 0;
        j->olen = cff_off(cc, &cs, g + 1) - cff_off(cc, &cs, g);
        unsigned hg = g < nhm ? g : nhm - 1;
        j->advance = be16(f + hmtx.at + hg * 4);
        j->lsb = g < nhm ? (int16_t)be16(f + hmtx.at + g * 4 + 2) : (int16_t)be16(f + hmtx.at + nhm * 4 + (g - nhm) * 2);
        if (j->icon >= 0) {
            size_t len = 0;
            int xmin = 0;
            j->nb = malloc(4096);
            if (!j->nb || cs_build(j->icon, j->advance, (int)nominal, 0, j->nb, 4096, &len, &xmin)) { free(j->nb), j->nb = NULL; missing++; continue; }
            j->nlen = len, j->lsb = xmin;
        } else {
            size_t so = cff_off(cc, &cs, j->src), se = cff_off(cc, &cs, j->src + 1);
            j->nb = malloc(se - so + 1);
            if (!j->nb) { missing++; continue; }
            memcpy(j->nb, cc + so, se - so), j->nlen = se - so;
            unsigned sg = j->src < nhm ? j->src : nhm - 1;
            j->advance = be16(f + hmtx.at + sg * 4);
            j->lsb = j->src < nhm ? (int16_t)be16(f + hmtx.at + j->src * 4 + 2) : (int16_t)be16(f + hmtx.at + nhm * 4 + (j->src - nhm) * 2);
        }
        j->on = -1;   /* a candidate */
    }
    if (!wanted) return 0;
    /* choose: the face buttons as one, then one by one while the room lasts */
    long budget = 0, need = 0;
    int gen = 0, faces = 0, faces_ok = 1;
    for (int b = PB_A; b <= PB_Y; b++) if (job[b].on) { faces++; budget += (long)job[b].olen, need += (long)job[b].nlen, gen += job[b].icon >= 0; }
    if (faces && (need > budget || (need < budget && !gen))) {
        faces_ok = 0;
        LOG("button font: the face buttons' new glyphs need %ld bytes where %ld are free: they stay as they are", need, budget);
        budget = need = gen = 0;
    }
    for (int b = PB_A; b <= PB_Y; b++) if (job[b].on) job[b].on = faces_ok;
    for (size_t i = 0; i < sizeof kOrder / sizeof *kOrder; i++) {
        GlyphJob *j = &job[kOrder[i]];
        if (!j->on) continue;
        long nb2 = budget + (long)j->olen, nn2 = need + (long)j->nlen;
        int g2 = gen + (j->icon >= 0);
        if (nn2 <= nb2) j->on = 1, budget = nb2, need = nn2, gen = g2;
        else { j->on = 0; LOG("button font: no room for the new %s glyph (%zu bytes for %zu): it stays", kPadKeys[kOrder[i]] + 9, j->nlen, j->olen); }
    }
    int count = 0;
    for (int b = 0; b < PB_COUNT; b++) count += job[b].on == 1;
    if (count && need < budget && !gen) {   /* only copies left and room over: nothing to pad with */
        LOG("button font: the copied glyphs do not fill their room exactly: the font stays as it is");
        count = 0;
    }
    int result = 0;
    if (count) {
        /* the slack goes into the first glyph we draw */
        size_t slack = (size_t)(budget - need);
        for (int b = 0; slack && b < PB_COUNT; b++) {
            GlyphJob *j = &job[b];
            if (j->on != 1 || j->icon < 0) continue;
            size_t len = 0;
            int xmin = 0;
            uint8_t *nb = malloc(j->nlen + slack + 4096);
            if (nb && !cs_build(j->icon, j->advance, (int)nominal, slack, nb, j->nlen + slack + 4096, &len, &xmin) && len == j->nlen + slack) {
                free(j->nb), j->nb = nb, j->nlen = len, slack = 0;
            } else free(nb);
        }
        if (slack) { LOG("button font: could not lay the glyphs out in their room: the font stays as it is"); count = 0; }
    }
    if (count) {
        unsigned lo = nglyphs, hi = 0;
        for (int b = 0; b < PB_COUNT; b++) if (job[b].on == 1) { if (job[b].gid < lo) lo = job[b].gid; if (job[b].gid > hi) hi = job[b].gid; }
        size_t rlo = cff_off(cc, &cs, lo), rhi = cff_off(cc, &cs, hi + 1);
        uint8_t *region = malloc(rhi - rlo + 1);
        size_t *lens = malloc(sizeof *lens * (hi - lo + 1));
        if (region && lens) {
            size_t at = 0;
            int bad = 0;
            for (unsigned g = lo; g <= hi && !bad; g++) {
                const GlyphJob *j = NULL;
                for (int b = 0; b < PB_COUNT; b++) if (job[b].on == 1 && job[b].gid == g) j = &job[b];
                const uint8_t *src = j ? j->nb : cc + cff_off(cc, &cs, g);
                size_t len = j ? j->nlen : cff_off(cc, &cs, g + 1) - cff_off(cc, &cs, g);
                if (at + len > rhi - rlo) { bad = 1; break; }
                memcpy(region + at, src, len), lens[g - lo] = len, at += len;
            }
            if (!bad && at == rhi - rlo) {
                memcpy(cc + rlo, region, at);
                size_t pos = rlo;
                for (unsigned g = lo; g < hi; g++) pos += lens[g - lo], cff_set_off(cc, &cs, g + 1, pos);
                for (int b = 0; b < PB_COUNT; b++) {
                    const GlyphJob *j = &job[b];
                    if (j->on != 1) continue;
                    if (j->gid < nhm) wbe16(f + hmtx.at + j->gid * 4, (unsigned)j->advance), wbe16(f + hmtx.at + j->gid * 4 + 2, (unsigned)(j->lsb & 0xffff));
                    else wbe16(f + hmtx.at + nhm * 4 + (j->gid - nhm) * 2, (unsigned)(j->lsb & 0xffff));
                }
                /* table checksums and the font's checksum adjustment */
                wbe32(f + cff_rec + 4, sfnt_sum(f + cff.at, cff.len));
                wbe32(f + hmtx_rec + 4, sfnt_sum(f + hmtx.at, hmtx.len));
                wbe32(f + head.at + 8, 0);
                wbe32(f + head_rec + 4, sfnt_sum(f + head.at, head.len));
                wbe32(f + head.at + 8, 0xB1B0AFBAu - sfnt_sum(f, n));
                result = count;
            } else LOG("button font: the glyph room did not add up: the font stays as it is");
        }
        free(region), free(lens);
    }
    for (int b = 0; b < PB_COUNT; b++) free(job[b].nb);
    if (missing) LOG("button font: %d of the glyphs to change are not in the font", missing);
    return result;
}

int prompts_font_archive(uint8_t *d, size_t n, const PromptMap *map) {
    RomfixMember *m = NULL;
    int nm = romfix_sarc(d, n, &m), r = -1, found = 0;
    for (int i = 0; i < nm; i++) {
        if (strcmp(m[i].name, "scft/External.bfotf")) continue;
        found = 1;
        uint8_t *b = d + m[i].lo;
        size_t bn = m[i].hi - m[i].lo;
        if (!prompts_bfotf_xor(b, bn)) break;
        if (memcmp(b + 8, "OTTO", 4)) { prompts_bfotf_xor(b, bn); LOG("scft/External.bfotf: not the button font this reads"); r = -2; break; }
        r = prompts_otf(b + 8, bn - 8, map);
        prompts_bfotf_xor(b, bn);
        break;
    }
    if (nm < 0) LOG("font archive: not a SARC archive this reads");
    else if (!found) LOG("font archive: no scft/External.bfotf");
    free(m);
    return r;
}

/* ======================================== the Common archive: images and letters ======================================== */
uint32_t prompts_gob_offset(uint32_t x, uint32_t y, uint32_t wg, uint32_t bh) {
    return (y / (8 * bh)) * 512 * bh * wg + (x / 64) * 512 * bh + ((y % (8 * bh)) / 8) * 512 + ((x % 64) / 32) * 256 +
           ((y % 8) / 2) * 64 + ((x % 32) / 16) * 32 + (y % 2) * 16 + (x % 16);
}
typedef struct { size_t info, data; uint32_t size, fmt, w, h, mips, bh; int found; } Tex;
static const char *const kTexNames[PB_COUNT] = {"Nt_KeyTexA_00", "Nt_KeyTexB_00", "Nt_KeyTexX_00", "Nt_KeyTexY_00", "Nt_KeyTexL_00",
                                               NULL, "Nt_KeyTexZL_00", NULL, NULL, NULL};
static int bntx_find(const uint8_t *d, size_t lo, size_t hi, Tex *tex) {
    if (hi - lo < 0x30 || memcmp(d + lo, "BNTX", 4) || memcmp(d + lo + 0x20, "NX  ", 4)) return -1;
    uint32_t count = rd32(d + lo + 0x24);
    uint64_t table = rd64(d + lo + 0x28);
    if (table > hi - lo || (uint64_t)count * 8 > hi - lo - table) return -1;
    for (uint32_t k = 0; k < count; k++) {
        uint64_t info = rd64(d + lo + table + 8ull * k);
        if (info > hi - lo || hi - lo - info < 0x78 || memcmp(d + lo + info, "BRTI", 4)) return -1;
        size_t ia = lo + (size_t)info;
        uint64_t name = rd64(d + ia + 0x60);
        if (name > hi - lo - 2) return -1;
        size_t nl = rd16(d + lo + name);
        if (name + 2 + nl > hi - lo) return -1;
        const char *s = (const char *)d + lo + name + 2;
        size_t base = 0;
        while (base < nl && s[base] != '^') base++;
        for (int b = 0; b < PB_COUNT; b++) {
            if (!kTexNames[b] || strlen(kTexNames[b]) != base || memcmp(kTexNames[b], s, base)) continue;
            uint64_t mt = rd64(d + ia + 0x70);
            if (mt > hi - lo - 8) return -1;
            uint64_t m0 = rd64(d + lo + mt);
            Tex *t = &tex[b];
            t->info = ia, t->size = rd32(d + ia + 0x50), t->fmt = rd32(d + ia + 0x1c), t->mips = rd16(d + ia + 0x16);
            t->w = rd32(d + ia + 0x24), t->h = rd32(d + ia + 0x28), t->bh = 1u << (rd32(d + ia + 0x34) & 7);
            if (m0 > hi - lo || t->size > hi - lo - m0) return -1;
            t->data = lo + (size_t)m0, t->found = 1;
        }
    }
    return 0;
}
static int same_shape(const uint8_t *d, const Tex *a, const Tex *b) {
    return !memcmp(d + a->info + 0x10, d + b->info + 0x10, 0x2c) && a->size == b->size;
}
#define R8_UNORM 0x201u
static int tex_paint(uint8_t *d, const Tex *t, int icon) {
    if (t->fmt != R8_UNORM || t->w != t->h || t->w < 16 || t->w > 1024 || t->mips != 1) return -1;
    uint32_t wg = (t->w + 63) / 64;
    if (prompts_gob_offset(t->w - 1, t->h - 1, wg, t->bh) >= t->size) return -1;
    uint8_t *mask = malloc((size_t)t->w * t->h);
    if (!mask) return -1;
    prompts_raster(icon, (int)t->w, 4, mask);
    for (uint32_t y = 0; y < t->h; y++)
        for (uint32_t x = 0; x < t->w; x++) d[t->data + prompts_gob_offset(x, y, wg, t->bh)] = mask[y * t->w + x];
    free(mask);
    return 0;
}
/* the one-letter text panes T_{A,B,X,Y}_<n> of a layout: their letter -> map->letter */
static int letter_of(const PromptMap *map, uint16_t ch) {
    for (int b = 0; b < 4; b++) if (ch == (uint16_t)"ABXY"[b]) return map->letter[b] ? map->letter[b] : 0;
    return 0;
}
static int pane_name_ok(const char *s, size_t cap) {
    if (cap < 5 || s[0] != 'T' || s[1] != '_' || !s[2] || !strchr("ABXY", s[2]) || s[3] != '_') return 0;
    size_t i = 4;
    if (!(s[i] >= '0' && s[i] <= '9')) return 0;
    while (i < cap && s[i] >= '0' && s[i] <= '9') i++;
    return i == cap || !s[i];
}
static int bflyt_letters(uint8_t *d, size_t lo, size_t hi, const PromptMap *map, int write) {
    if (hi - lo < 0x14 || memcmp(d + lo, "FLYT", 4)) return 0;
    size_t pos = lo + rd16(d + lo + 6);
    int changed = 0;
    while (pos + 8 <= hi) {
        uint32_t size = rd32(d + pos + 4);
        if (!memcmp(d + pos, "txt1", 4) && size >= 0x68 && pos + size <= hi) {
            char name[25];
            memcpy(name, d + pos + 12, 24), name[24] = 0;
            uint16_t len = rd16(d + pos + 0x56);
            uint32_t at = rd32(d + pos + 0x64);
            if (pane_name_ok(name, 24) && len == 4 && at && pos + at + 4 <= hi) {
                int nl = letter_of(map, rd16(d + pos + at));
                if (nl) {
                    if (write) d[pos + at] = (uint8_t)nl, d[pos + at + 1] = 0;
                    changed++;
                }
            }
        }
        if (!size) break;
        pos += size;
    }
    return changed;
}

int prompts_common(uint8_t *d, size_t n, const RomfixMember *m, int nm, const PromptMap *map) {
    (void)n;   /* the members are checked against it by romfix_sarc */
    const RomfixMember *bn = NULL;
    for (int i = 0; i < nm; i++) if (!strcmp(m[i].name, "timg/__Combined.bntx")) bn = &m[i];
    if (!bn) { LOG("Common archive: no timg/__Combined.bntx"); return -1; }
    Tex tex[PB_COUNT];
    memset(tex, 0, sizeof tex);
    if (bntx_find(d, bn->lo, bn->hi, tex)) { LOG("Common archive: timg/__Combined.bntx is not a texture list this reads"); return -2; }
    /* the game's pictures first, from a copy (Xbox A and B trade places) */
    uint8_t *save[4] = {0};
    for (int b = PB_A; b <= PB_Y; b++)
        if (tex[b].found && (save[b] = malloc(tex[b].size))) memcpy(save[b], d + tex[b].data, tex[b].size);
    int changed = 0, skipped = 0;
    for (int b = 0; b < PB_COUNT; b++) {
        if (!kTexNames[b] || (map->game[b] < 0 && map->icon[b] < 0)) continue;
        Tex *t = &tex[b];
        if (!t->found) { skipped++; continue; }
        if (map->game[b] >= 0) {
            int s = map->game[b];
            if (s > PB_Y || !tex[s].found || !save[s] || !same_shape(d, t, &tex[s])) { skipped++; continue; }
            memcpy(d + t->data, save[s], t->size);
            changed++;
        } else if (!tex_paint(d, t, map->icon[b])) changed++;
        else skipped++;
    }
    for (int b = 0; b < 4; b++) free(save[b]);
    if (skipped) LOG("Common archive: %d button images are not the kind this draws: they stay", skipped);
    int letters = 0;
    if (map->letter[0] || map->letter[1] || map->letter[2] || map->letter[3])
        for (int i = 0; i < nm; i++) {
            size_t l = strlen(m[i].name);
            if (l > 6 && !strcmp(m[i].name + l - 6, ".bflyt")) letters += bflyt_letters(d, m[i].lo, m[i].hi, map, 1);
        }
    LOG("Common archive: %d button images changed, %d letter panes", changed, letters);
    return changed + letters;
}

/* ======================================== Mals: the letters of the action prompt's panes ======================================== */
static int msbt_letters(uint8_t *d, size_t n, const PromptMap *map) {
    if (n < 0x20 || memcmp(d, "MsgStdBn", 8) || d[8] != 0xff || d[9] != 0xfe || d[0x0c] != 1) return -1;
    size_t lbl = 0, lbl_n = 0, txt = 0, txt_n = 0, pos = 0x20;
    for (unsigned s = 0, ns = rd16(d + 0x0e); s < ns; s++) {
        if (pos + 16 > n) return -1;
        size_t size = rd32(d + pos + 4);
        if (size > n - pos - 16) return -1;
        if (!memcmp(d + pos, "LBL1", 4)) lbl = pos + 16, lbl_n = size;
        if (!memcmp(d + pos, "TXT2", 4)) txt = pos + 16, txt_n = size;
        pos = (pos + 16 + size + 15) & ~(size_t)15;
    }
    if (!lbl || !txt || lbl_n < 4 || txt_n < 4) return -1;
    uint32_t count = rd32(d + txt);
    if ((uint64_t)count * 4 + 4 > txt_n) return -1;
    int changed = 0;
    uint32_t groups = rd32(d + lbl);
    if ((uint64_t)groups * 8 + 4 > lbl_n) return -1;
    for (uint32_t g = 0; g < groups; g++) {
        uint32_t k = rd32(d + lbl + 4 + g * 8), at = rd32(d + lbl + 8 + g * 8);
        size_t p = lbl + at;
        for (uint32_t e = 0; e < k; e++) {
            if (p + 1 > lbl + lbl_n) return -1;
            size_t l = d[p];
            if (p + 1 + l + 4 > lbl + lbl_n) return -1;
            char name[64];
            size_t cl = l < sizeof name - 1 ? l : sizeof name - 1;
            memcpy(name, d + p + 1, cl), name[cl] = 0;
            uint32_t idx = rd32(d + p + 1 + l);
            p += 1 + l + 4;
            if (!pane_name_ok(name, cl) || idx >= count) continue;
            size_t so = rd32(d + txt + 4 + idx * 4), se = idx + 1 < count ? rd32(d + txt + 4 + (idx + 1) * 4) : txt_n;
            if (so > se || se > txt_n || se - so != 4) continue;
            int nl = letter_of(map, rd16(d + txt + so));
            if (nl && rd16(d + txt + so + 2) == 0) d[txt + so] = (uint8_t)nl, d[txt + so + 1] = 0, changed++;
        }
    }
    return changed;
}
int prompts_mals(uint8_t *d, size_t n, const PromptMap *map) {
    if (!(map->letter[0] || map->letter[1] || map->letter[2] || map->letter[3])) return 0;
    RomfixMember *m = NULL;
    int nm = romfix_sarc(d, n, &m), r = -1;
    for (int i = 0; i < nm; i++)
        if (!strcmp(m[i].name, "LayoutMsg/DoCommand_00.msbt")) r = msbt_letters(d + m[i].lo, m[i].hi - m[i].lo, map);
    free(m);
    return r;
}

/* tools/tests/test_prompts.c: an icon's charstring with exactly pad bytes more */
int prompts_test_build(int icon, int advance, int nominal, size_t pad, uint8_t *out, size_t cap, size_t *len, int *xmin);
int prompts_test_build(int icon, int advance, int nominal, size_t pad, uint8_t *out, size_t cap, size_t *len, int *xmin) {
    return icon < 0 || icon >= PI_COUNT ? -1 : cs_build(icon, advance, nominal, pad, out, cap, len, xmin);
}
