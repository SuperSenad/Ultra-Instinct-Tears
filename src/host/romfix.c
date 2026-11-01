/* [UIT 0065 / 0068 / 0069] Game data edits made in memory when the game opens a file: see romfix.h.
 *
 * The edits are MacNativeGames' (MIT; scripts/make_romfs_patches.py, make_output_res_overlay.py, ultrawide_layout.py),
 * written again here in C so they run at start-up on the player's own files instead of in a build step:
 *  - quick menus: the weapon / shield / material menus (ShortCutBG_00_In) and Autobuild (AutoBuilder_00_In) capture
 *    the live picture for their background when the capture panes become visible at animation frame 1.0. UI
 *    animations advance 0.5 frames per game frame at 60 fps, so the capture lands after the world stopped drawing and
 *    the background is black above 30 fps. Their visibility keys move to frame 0.0: the capture is taken at once.
 *  - HeapDef: DynamicTextureAllocatorMem (the fixed heap the canvas-sized render targets come from) and its parent
 *    Graphics grow by uit_heap_extra_kib(); svc.c adds the same to the application pool, so no other heap shrinks.
 *  - layouts wider than 16:9: each layout's root pane is narrowed to 16:9 (scale x = 16/9 over the aspect), full-screen
 *    backgrounds widen back, HUD parts move to the corners. Pane names and distances: the community TOTK-Ultrawide
 *    findings as MacNativeGames checked them against 1.4.3's Common.Product.140 (no mod files are used).
 * Each field must hold the value the game ships before it is written; otherwise the whole edit is left out. */
#include "romfix.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "uit_str.h"   /* [UIT 0104] uit_stricmp: no <strings.h> on Windows */
#include <zstd.h>

#include "uit_display.h"

#define LOG(...) (fprintf(stderr, "[totk] romfix: " __VA_ARGS__), fputc('\n', stderr))

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static float rdf(const uint8_t *p) { float f; memcpy(&f, p, 4); return f; }
static void wrf(uint8_t *p, float f) { memcpy(p, &f, 4); }
static void wr32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v, p[1] = (uint8_t)(v >> 8), p[2] = (uint8_t)(v >> 16), p[3] = (uint8_t)(v >> 24); }

/* ======================================== SARC ======================================== */
int romfix_sarc(const uint8_t *d, size_t n, RomfixMember **out) {
    *out = NULL;
    if (n < 0x14 || memcmp(d, "SARC", 4) || d[6] != 0xff || d[7] != 0xfe) return -1;
    size_t head = rd16(d + 4), start = rd32(d + 12);
    if (rd32(d + 8) != n || start > n || head + 12 > n || memcmp(d + head, "SFAT", 4)) return -1;
    size_t count = rd16(d + head + 6), names = head + 12 + count * 16;
    if (names + 8 > n || memcmp(d + names, "SFNT", 4)) return -1;
    names += 8;
    RomfixMember *m = calloc(count + 1, sizeof *m);
    if (!m) return -1;
    for (size_t i = 0; i < count; i++) {
        const uint8_t *e = d + head + 12 + i * 16;
        size_t at = names + (size_t)(rd32(e + 4) & 0xffffff) * 4, lo = rd32(e + 8), hi = rd32(e + 12);
        if (at >= start || !memchr(d + at, 0, start - at) || lo > hi || start + hi > n) { free(m); return -1; }
        m[i].name = (const char *)d + at, m[i].lo = start + lo, m[i].hi = start + hi;
    }
    *out = m;
    return (int)count;
}
static const RomfixMember *member(const RomfixMember *m, int nm, const char *name) {
    for (int i = 0; i < nm; i++) if (!strcmp(m[i].name, name)) return &m[i];
    return NULL;
}

/* ======================================== quick-menu backgrounds ======================================== */
/* every FLVI (visibility) step track of the BFLAN at [lo, hi): calls f(pane, key offset, key count) */
typedef int (*FlviFn)(void *ctx, const char *pane, size_t ko, unsigned nk);
static int flvi_walk(const uint8_t *d, size_t lo, size_t hi, FlviFn f, void *ctx) {
    if (hi - lo < 0x14 || memcmp(d + lo, "FLAN", 4) || d[lo + 4] != 0xff || d[lo + 5] != 0xfe) return -1;
    size_t pos = lo + 0x14;
    for (unsigned s = 0, ns = rd16(d + lo + 0x10); s < ns; s++) {
        if (pos + 8 > hi) return -1;
        size_t size = rd32(d + pos + 4);
        if (size < 8 || pos + size > hi) return -1;
        if (!memcmp(d + pos, "pai1", 4)) {
            if (size < 20) return -1;
            unsigned count = rd16(d + pos + 14);
            size_t table = pos + rd32(d + pos + 16);
            if (table + 4ull * count > hi) return -1;
            for (unsigned e = 0; e < count; e++) {
                size_t eo = pos + rd32(d + table + 4 * e);
                if (eo + 32 > hi) return -1;
                char name[29];
                memcpy(name, d + eo, 28), name[28] = 0;
                for (unsigned t = 0, nt = d[eo + 28]; t < nt; t++) {
                    if (eo + 36 + 4 * t > hi) return -1;
                    size_t to = eo + rd32(d + eo + 32 + 4 * t);
                    if (to + 8 > hi) return -1;
                    if (memcmp(d + to, "FLVI", 4)) continue;
                    for (unsigned g = 0, ng = d[to + 4]; g < ng; g++) {
                        if (to + 12 + 4 * g > hi) return -1;
                        size_t go = to + rd32(d + to + 8 + 4 * g);
                        if (go + 12 > hi || d[go + 2] != 1) return -1;   /* visibility tracks are step curves */
                        unsigned nk = rd16(d + go + 4);
                        size_t ko = go + rd32(d + go + 8);
                        if (ko + 8ull * nk > hi) return -1;
                        int r = f(ctx, name, ko, nk);
                        if (r) return r;
                    }
                }
            }
        }
        pos += size;
    }
    return 0;
}
typedef struct { const uint8_t *d; const char *const *panes; int np; size_t at[4]; int found[4], shipped, moved; } Capture;
static int capture_fn(void *ctx, const char *pane, size_t ko, unsigned nk) {
    Capture *c = ctx;
    for (int i = 0; i < c->np; i++) {
        if (strcmp(pane, c->panes[i])) continue;
        if (c->found[i]++) return -2;                        /* the same pane twice: not the analysed layout */
        if (nk != 2 || rdf(c->d + ko) != 0.0f || rd16(c->d + ko + 4) != 0 || rd16(c->d + ko + 12) != 1) return -3;
        float f = rdf(c->d + ko + 8);
        if (f == 1.0f) c->shipped++;
        else if (f >= 0.0f && f < 1.0f) c->moved++;          /* a mod's fix already moved it */
        else return -3;
        c->at[i] = ko + 8;
    }
    return 0;
}
int romfix_menu_bg(uint8_t *d, size_t n, const RomfixMember *m, int nm) {
    static const char *const SHORTCUT[] = {"C_CaptureBG_00", "C_CaptureClearBG_00", "P_CaptureClearBG_00"};
    static const char *const AUTOBUILD[] = {"C_CaptureBG_00"};
    static const struct { const char *member; const char *const *panes; int np; } MEMBERS[] = {
        {"anim/ShortCutBG_00_In.bflan", SHORTCUT, 3}, {"anim/AutoBuilder_00_In.bflan", AUTOBUILD, 1}};
    Capture c[2];
    int shipped = 0, moved = 0;
    (void)n;
    for (int k = 0; k < 2; k++) {
        const RomfixMember *mm = member(m, nm, MEMBERS[k].member);
        if (!mm) return -1;
        memset(&c[k], 0, sizeof c[k]);
        c[k].d = d, c[k].panes = MEMBERS[k].panes, c[k].np = MEMBERS[k].np;
        int r = flvi_walk(d, mm->lo, mm->hi, capture_fn, &c[k]);
        if (r) return r < 0 ? r : -1;
        for (int i = 0; i < c[k].np; i++) if (c[k].found[i] != 1) return -4;   /* a pane is missing */
        shipped += c[k].shipped, moved += c[k].moved;
    }
    if (!shipped) return 0;          /* every key already earlier than 1.0: a mod's fix, left as it is */
    if (moved) return -5;            /* some moved and some not: not data this knows */
    for (int k = 0; k < 2; k++) for (int i = 0; i < c[k].np; i++) wrf(d + c[k].at[i], 0.0f);
    return 1;
}

/* ======================================== UI layouts wider than 16:9 ======================================== */
enum { FX = 44, FY = 48, FSX = 68, FSY = 72 };
enum { E_K, E_WIDE, E_OFF, E_LIN, E_LINK };   /* k; 1/k; out of sight; a + b*w + c*(1-k); (a + b*w)*k */
typedef struct { const char *layout, *pane; uint8_t field, kind; float a, b, c; } Edit;
const char *const romfix_keep_root[10] = {"AppMenuBG_00", "PauseMenuBG_00", "ShopMenuParasailBG_00", "ShortCutBG_00", "TitleMenuBG_00",
                                        "EventFade_00", "EventFadeWipe_00", "CameraShutter_00", "PaAppPictureBookDetail_00",
                                        "AppMenuOverlay_00"};
#define K(l, p) {l, p, FSX, E_K, 0, 0, 0}
#define W(l, p) {l, p, FSX, E_WIDE, 0, 0, 0}
#define OFF(l, p) {l, p, FX, E_OFF, 0, 0, 0}
#define X(l, p, a, b, c) {l, p, FX, E_LIN, a, b, c}
#define XK(l, p, a, b) {l, p, FX, E_LINK, a, b, 0}
#define DECO(l, L, R) K(l, L), XK(l, L, -675, -1), K(l, R), XK(l, R, 737, 1)
static const Edit EDITS[] = {
    K("AppAlbum_00", "N_FadeInOut_00"), W("AppAlbum_00", "N_Open_01"),
    W("AppMap_00", "N_BlurInOut_00"), W("AppMap_00", "N_BlurInOut_01"), W("AppMap_00", "N_BlurInOut_02"), W("AppMap_00", "N_BlurInOut_03"),
    X("AppMap_00", "N_MiniMap_00", 0, 0, -270),
    K("AppMenuOverlay_00", "N_FadeInOut_00"), K("AppMenuOverlay_00", "N_FadeInOut_01"),
    K("AppMenuOverlay_00", "N_Icon_00"), K("AppMenuOverlay_00", "N_TitleOne_01"),
    W("AppOpenDemoWindow_00", "P_BG_00"), W("AutoBuilder_00", "N_BG_00"),
    W("CameraPointer_00", "N_InOutScope_00"), {"CameraPointer_00", "N_InOutScope_00", FSY, E_WIDE, 0, 0, 0},
    K("CameraPointer_00", "N_Type_01"), W("CameraPointer_00", "Pa_CameraFocus_00"),
    W("CameraPointer_00", "S_Scissor_00"), W("CameraPointer_00", "W_Shadow_01"),
    K("CameraSystemWindow_00", "N_AlbumCon_00"), W("CameraSystemWindow_00", "N_InOut_00"),
    K("CameraSystemWindow_00", "Pa_BtnL_00"), K("CameraSystemWindow_00", "Pa_BtnR_00"), K("CameraSystemWindow_00", "T_Text_00"),
    OFF("ChallengeLog_00", "C_CaptureUp_00"), OFF("ChallengeLog_00", "C_CaptureUp_01"),
    W("GameOver_00", "P_BG_00"),
    W("LoadingFade_00", "N_Base_00"), X("LoadingFade_00", "N_Logo_00", 0, 1, 0),
    X("LoadingFade_00", "N_Status_00", 0, 1, 0), X("LoadingFade_00", "N_Tips_00", 0, -1, 0),
    W("LoadingFade_00", "P_BG_00"), K("LoadingFade_00", "P_SideBGDeco_00"),
    K("LoadingFade_00", "Pa_IconPlayer_00"), K("LoadingFade_00", "Pa_LargeDungeonMap_00"),
    K("LoadingFade_00", "Pa_LargeDungeonMap_01"), K("LoadingFade_00", "Pa_Map_00"), K("LoadingFade_00", "Pa_Map_01"),
    W("MessageLog_01", "P_Capture_00"), W("OwnedHorseList_00", "N_BG_00"),
    OFF("PaCommonBtnTitle_00", "B_Hit_00"), OFF("PaCommonBtnTitle_00", "N_Cursor_00"),
    W("PaLargeDungeonMap_00", "P_BG_00"), OFF("PaMessageBtn_00", "B_Hit_00"),
    W("PaRecipeCardList_00", "P_BG_00"),
    X("SaveLoadIcon_00", "RootPane", 0, 0, -960),
    W("ShopMenu_00", "N_BG_00"), W("StaffRoll_00", "N_BG_00"),
    DECO("PauseMenuOverlay_00", "P_DecoL_00", "P_DecoR_00"),
    W("PauseMenuOverlay_00", "P_FooterBase_00"), W("PauseMenuOverlay_00", "S_DecoScissor_00"),
    DECO("SystemLoadList_00", "P_DecoL_00", "P_DecoR_00"),
    K("SystemLoadList_00", "A_Title_00"), W("SystemLoadList_00", "N_Header_00"), W("SystemLoadList_00", "N_SystemBG_00"),
    DECO("SystemOption_00", "P_DecoL_00", "P_DecoR_00"),
    K("SystemOption_00", "A_Title_00"), W("SystemOption_00", "N_Header_00"), W("SystemOption_00", "N_SystemBG_00"),
    DECO("SystemSaveLoad_00", "P_DecoL_00", "P_DecoR_00"),
    K("SystemSaveLoad_00", "A_Title_00"), W("SystemSaveLoad_00", "N_Header_00"), W("SystemSaveLoad_00", "N_SystemBG_00"),
    K("SystemActionGuide_00", "A_Title_00"), W("SystemActionGuide_00", "N_Header_00"), W("SystemActionGuide_00", "N_SystemBG_00"),
    K("SystemActionGuide_00", "P_DecoL_01"), XK("SystemActionGuide_00", "P_DecoL_01", -675, -1),
    K("SystemActionGuide_00", "P_DecoL_02"), XK("SystemActionGuide_00", "P_DecoL_02", 675, 1),
};
/* the HUD in the screen's corners instead of the middle 16:9 */
static const Edit CORNER[] = {
    X("EnvironmentInfo_00", "RootPane", 0, 0, 960), X("EventSkip_00", "RootPane", 0, 0, 960),
    X("InstantTipsAppMenu_00", "RootPane", 0, 0, 960), X("InstantTipsWithPict_00", "RootPane", 0, 0, 960),
    X("InstantTips_00", "RootPane", 0, 0, 960), X("NumDisplaySecond_00", "RootPane", 0, 0, 960),
    X("NumDisplay_00", "RootPane", 0, 0, 960), X("PickUpWin_00", "RootPane", 0, 0, 960),
    X("Location_00", "N_Small_00", -388, -1, 0), X("PlayerStatus3D_00", "N_ExtraStaminaTarget_01", -745, -1, 0),
    X("PlayerStatus_00", "N_DevilDemoTargetC_00", -284, -1, 0), X("PlayerStatus_00", "N_DevilDemoTargetLT_00", -576, -1, 0),
    X("PlayerStatus_00", "N_HeartGaugeTargetLT_00", -576, -1, 0), X("PlayerStatus_00", "N_HeartPos_00", 0, -1, 0),
    X("PlayerStatus_00", "Pa_CompanionLife_00", -840, -1, 0),
};
static double edit_value(const Edit *e, double k) {
    double w = 960 * (1 / k - 1);
    switch (e->kind) {
    case E_K: return k;
    case E_WIDE: return 1 / k;
    case E_OFF: return 10000.0;
    case E_LIN: return e->a + e->b * w + e->c * (1 - k);
    default: return (e->a + e->b * w) * k;
    }
}
/* the first pane of that exact name in the BFLYT at [lo, hi): its section offset, 0 if none, (size_t)-1 if unreadable */
static size_t find_pane(const uint8_t *d, size_t lo, size_t hi, const char *name) {
    static const char MAGICS[][5] = {"pan1", "pic1", "txt1", "wnd1", "bnd1", "prt1", "ali1", "scr1"};
    if (hi - lo < 0x14 || memcmp(d + lo, "FLYT", 4)) return (size_t)-1;
    size_t pos = lo + rd16(d + lo + 6);
    for (unsigned s = 0, ns = rd16(d + lo + 0x10); s < ns; s++) {
        if (pos + 8 > hi) return (size_t)-1;
        size_t size = rd32(d + pos + 4);
        if (size < 8 || pos + size > hi) return (size_t)-1;
        for (unsigned i = 0; i < 8; i++)
            if (!memcmp(d + pos, MAGICS[i], 4)) {
                if (size < FSY + 4) return (size_t)-1;
                char nm[25];
                memcpy(nm, d + pos + 12, 24), nm[24] = 0;
                if (!strcmp(nm, name)) return pos;
                break;
            }
        pos += size;
    }
    return 0;
}
static size_t pane_at(const uint8_t *d, const RomfixMember *m, int nm, const char *layout, const char *pane) {
    char mn[128];
    snprintf(mn, sizeof mn, "blyt/%s.bflyt", layout);
    const RomfixMember *mm = member(m, nm, mn);
    if (!mm) return 0;
    size_t at = find_pane(d, mm->lo, mm->hi, pane);
    return at == (size_t)-1 ? 0 : at;
}
static int apply_edits(uint8_t *d, const RomfixMember *m, int nm, const Edit *e, size_t ne, double k) {
    for (size_t i = 0; i < ne; i++) {
        size_t at = pane_at(d, m, nm, e[i].layout, e[i].pane);
        if (!at) { LOG("layouts: %s has no pane %s", e[i].layout, e[i].pane); return -2; }
        double shipped = edit_value(&e[i], 1.0), have = rdf(d + at + e[i].field);
        if (e[i].kind != E_OFF && fabs(have - shipped) > 1e-3) {
            LOG("layouts: %s %s +%u is %g, expected %g", e[i].layout, e[i].pane, e[i].field, have, shipped);
            return -3;
        }
        wrf(d + at + e[i].field, (float)edit_value(&e[i], k));
    }
    return 0;
}
int romfix_layout_field(unsigned i, int corner, const char **layout, const char **pane, unsigned *field, float *shipped) {
    const unsigned ne = sizeof EDITS / sizeof *EDITS, nc = corner ? sizeof CORNER / sizeof *CORNER : 0;
    static const unsigned MINI[] = {872, 880, 1628, 1636, 3264, 3272, 3468, 3476};
    const Edit *e = i < ne ? &EDITS[i] : i < ne + nc ? &CORNER[i - ne] : NULL;
    if (e) { *layout = e->layout, *pane = e->pane, *field = e->field, *shipped = (float)edit_value(e, 1.0); return 1; }
    i -= ne + nc;
    *pane = NULL;
    if (i < 2) { *layout = "PaMapIconDragonTears_00_Zoom", *field = i ? 300 : 288, *shipped = i ? 1.28f : 1.08f; return 2; }
    if (corner && i - 2 < 8) { *layout = "AppMap_00_MiniMap", *field = MINI[i - 2], *shipped = 765.0f; return 2; }
    return 0;
}
int romfix_layouts(uint8_t *d, size_t n, const RomfixMember *m, int nm, double aspect, int corner) {
    (void)n;
    double k = (16.0 / 9.0) / aspect, w = 960 * (1 / k - 1);
    if (!(k < 1) || k < 0.25) return -1;
    for (size_t i = 0; i < sizeof romfix_keep_root / sizeof *romfix_keep_root; i++) {
        char mn[128];
        snprintf(mn, sizeof mn, "blyt/%s.bflyt", romfix_keep_root[i]);
        if (!member(m, nm, mn)) { LOG("layouts: no layout %s", romfix_keep_root[i]); return -2; }
    }
    int roots = 0;
    for (int i = 0; i < nm; i++) {   /* every layout's root pane narrowed to 16:9, except the full-screen ones */
        size_t ln = strlen(m[i].name);
        if (strncmp(m[i].name, "blyt/", 5) || ln < 12 || strcmp(m[i].name + ln - 6, ".bflyt")) continue;
        char layout[128];
        if (ln - 11 >= sizeof layout) return -2;
        memcpy(layout, m[i].name + 5, ln - 11), layout[ln - 11] = 0;
        int keep = 0;
        for (size_t j = 0; j < sizeof romfix_keep_root / sizeof *romfix_keep_root; j++) keep |= !strcmp(layout, romfix_keep_root[j]);
        if (keep) continue;
        size_t at = find_pane(d, m[i].lo, m[i].hi, "RootPane");
        if (!at || at == (size_t)-1) { LOG("layouts: %s has no RootPane", layout); return -2; }
        wrf(d + at + FSX, (float)k);
        roots++;
    }
    int r = apply_edits(d, m, nm, EDITS, sizeof EDITS / sizeof *EDITS, k);
    if (!r && corner) r = apply_edits(d, m, nm, CORNER, sizeof CORNER / sizeof *CORNER, k);
    if (r) return r;
    /* floats inside animation key tables: (animation, byte offset, shipped, new) */
    struct { const char *anim; unsigned off; float shipped; double value; } A[10] = {
        {"PaMapIconDragonTears_00_Zoom", 288, 1.08f, 1.08 / k}, {"PaMapIconDragonTears_00_Zoom", 300, 1.28f, 1.28 / k}};
    int na = 2;
    if (corner) {   /* the minimap's slide-in keys end at x 765, the right edge of a 16:9 layout */
        static const unsigned OFFS[] = {872, 880, 1628, 1636, 3264, 3272, 3468, 3476};
        for (unsigned i = 0; i < 8; i++) A[na].anim = "AppMap_00_MiniMap", A[na].off = OFFS[i], A[na].shipped = 765.0f, A[na++].value = 765 + w;
    }
    for (int i = 0; i < na; i++) {
        char mn[128];
        snprintf(mn, sizeof mn, "anim/%s.bflan", A[i].anim);
        const RomfixMember *mm = member(m, nm, mn);
        if (!mm || A[i].off + 4 > mm->hi - mm->lo) { LOG("layouts: no animation %s +%u", A[i].anim, A[i].off); return -2; }
        float have = rdf(d + mm->lo + A[i].off);
        if (fabsf(have - A[i].shipped) >= 1e-4f) { LOG("layouts: %s +%u is %g, expected %g", A[i].anim, A[i].off, have, A[i].shipped); return -3; }
        wrf(d + mm->lo + A[i].off, (float)A[i].value);
    }
    LOG("layouts for aspect %.4f: %d root panes narrowed, HUD %s", aspect, roots, corner ? "in the corners" : "in the middle 16:9");
    return 1;
}

/* ======================================== HeapDef (BYML v7) ======================================== */
typedef struct { const uint8_t *d; size_t n; size_t keys; uint32_t nkeys; } Byml;
static int byml_key(const Byml *b, uint32_t i, const char *want) {
    if (i >= b->nkeys) return 0;
    size_t o = b->keys + rd32(b->d + b->keys + 4 + 4 * (size_t)i);
    size_t l = strlen(want);
    return o + l < b->n && !memcmp(b->d + o, want, l) && b->d[o + l] == 0;
}
/* the value field of hash node `node`'s entry `name` (type `type`): 0 if none */
static size_t byml_field(const Byml *b, size_t node, const char *name, uint8_t type) {
    if (node + 4 > b->n || b->d[node] != 0xc1) return 0;
    uint32_t cnt = rd32(b->d + node) >> 8;
    if (node + 4 + 8ull * cnt > b->n) return 0;
    for (uint32_t i = 0; i < cnt; i++) {
        size_t e = node + 4 + 8 * (size_t)i;
        if (byml_key(b, rd32(b->d + e) & 0xffffff, name)) return b->d[e + 3] == type ? e + 4 : 0;
    }
    return 0;
}
static size_t byml_child(const Byml *b, size_t node, const char *name) {
    size_t f = byml_field(b, node, name, 0xc1);
    return f ? rd32(b->d + f) : 0;
}
int romfix_heapdef(uint8_t *d, size_t n, uint32_t extra_kib, int shipped) {
    if (n < 16 || d[0] != 'Y' || d[1] != 'B' || rd16(d + 2) != 7) return -1;
    Byml b = {d, n, rd32(d + 4), 0};
    size_t root = rd32(d + 12);
    if (b.keys + 8 > n || d[b.keys] != 0xc2) return -1;
    b.nkeys = rd32(d + b.keys) >> 8;
    if (b.keys + 4 + 4ull * b.nkeys > n) return -1;
    size_t heaps = byml_child(&b, root, "Heaps");
    size_t dta = heaps ? byml_child(&b, heaps, "DynamicTextureAllocatorMem") : 0, gfx = heaps ? byml_child(&b, heaps, "Graphics") : 0;
    size_t da = dta ? byml_field(&b, dta, "Size", 0xd1) : 0, ga = gfx ? byml_field(&b, gfx, "Size", 0xd1) : 0;
    if (!da || !ga || da + 4 > n || ga + 4 > n) return -2;
    int32_t dv = (int32_t)rd32(d + da), gv = (int32_t)rd32(d + ga);
    if (shipped && (dv != 113664 || gv != 201728)) return -3;
    if (dv <= 0 || gv <= 0 || (uint64_t)gv + extra_kib > 0x7fffffff) return -3;
    wr32(d + da, (uint32_t)dv + extra_kib), wr32(d + ga, (uint32_t)gv + extra_kib);
    LOG("HeapDef: DynamicTextureAllocatorMem %d -> %u KiB, Graphics %d -> %u KiB", dv, (unsigned)dv + extra_kib, gv, (unsigned)gv + extra_kib);
    return 1;
}

/* ======================================== zstd with the game's dictionaries ======================================== */
static uint8_t *unzstd(const uint8_t *src, size_t n, const void *dict, size_t dn, size_t *on) {
    unsigned long long c = ZSTD_getFrameContentSize(src, n);
    if (c == ZSTD_CONTENTSIZE_ERROR || c == ZSTD_CONTENTSIZE_UNKNOWN || c > (512ull << 20)) return NULL;
    uint8_t *d = malloc((size_t)c + 1);
    ZSTD_DCtx *z = ZSTD_createDCtx();
    size_t r = d && z ? ZSTD_decompress_usingDict(z, d, (size_t)c, src, n, dict, dn) : (size_t)-1;
    ZSTD_freeDCtx(z);
    if (ZSTD_isError(r) || r != c) { free(d); return NULL; }
    *on = r;
    return d;
}
/* the dictionary with that id from Pack/ZsDic.pack.zs (a zstd SARC of dictionaries): malloc'd */
static uint8_t *dictionary(unsigned id, RomfixRead rd, size_t *dn) {
    size_t pn = 0, sn = 0;
    uint8_t *pz = rd("Pack/ZsDic.pack.zs", &pn), *pack = pz ? unzstd(pz, pn, NULL, 0, &sn) : NULL, *out = NULL;
    free(pz);
    RomfixMember *m = NULL;
    int nm = pack ? romfix_sarc(pack, sn, &m) : -1;
    for (int i = 0; i < nm && !out; i++)
        if (ZSTD_getDictID_fromDict(pack + m[i].lo, m[i].hi - m[i].lo) == id && (out = malloc(m[i].hi - m[i].lo + 1)))
            memcpy(out, pack + m[i].lo, *dn = m[i].hi - m[i].lo);
    free(m), free(pack);
    return out;
}
static uint64_t fnv(const uint8_t *p, size_t n, uint64_t h) {
    for (size_t i = 0; i < n; i++) h = (h ^ p[i]) * 0x100000001b3ull;
    return h;
}
/* compressed like the game's own (its dictionary id, the content size); a copy saved in cache_dir is used when it
   decodes to exactly the same bytes */
static uint8_t *rezstd(const char *tag, const uint8_t *data, size_t n, const void *dict, size_t dn, const char *cache_dir, size_t *on) {
    char cp[4096] = "";
    if (cache_dir && *cache_dir) {
        uint64_t h = fnv(data, n, 0xcbf29ce484222325ull);
        snprintf(cp, sizeof cp, "%s/%s-%016llx.zs", cache_dir, tag, (unsigned long long)h);
        FILE *f = fopen(cp, "rb");
        if (f) {
            uint8_t *z = NULL;
            long len = fseek(f, 0, SEEK_END) == 0 ? ftell(f) : -1;
            if (len > 0 && len < (512l << 20) && !fseek(f, 0, SEEK_SET) && (z = malloc((size_t)len)) && fread(z, 1, (size_t)len, f) != (size_t)len) free(z), z = NULL;
            fclose(f);
            size_t cn = 0;
            uint8_t *chk = z ? unzstd(z, (size_t)len, dict, dn, &cn) : NULL;
            int same = chk && cn == n && !memcmp(chk, data, n);
            free(chk);
            if (same) { *on = (size_t)len; return z; }
            free(z);
        }
    }
    size_t cap = ZSTD_compressBound(n);
    uint8_t *z = malloc(cap);
    ZSTD_CCtx *c = ZSTD_createCCtx();
    size_t r = z && c ? ZSTD_compress_usingDict(c, z, cap, data, n, dict, dn, 19) : (size_t)-1;
    ZSTD_freeCCtx(c);
    size_t cn = 0;
    uint8_t *chk = ZSTD_isError(r) ? NULL : unzstd(z, r, dict, dn, &cn);
    int ok = chk && cn == n && !memcmp(chk, data, n) && ZSTD_getDictID_fromFrame(z, r) == (dict ? ZSTD_getDictID_fromDict(dict, dn) : 0);
    free(chk);
    if (!ok) { free(z); return NULL; }
    if (*cp) {
        char tmp[4200];
        snprintf(tmp, sizeof tmp, "%s.tmp", cp);
        FILE *f = fopen(tmp, "wb");
        int w = f && fwrite(z, 1, r, f) == r;
        if (f) w &= fclose(f) == 0;
        remove(cp);
        if (!w || rename(tmp, cp)) remove(tmp);
    }
    *on = r;
    return z;
}

/* ======================================== settings ======================================== */
static int off_value(const char *e) { return e && (!strcmp(e, "0") || !uit_stricmp(e, "off") || !uit_stricmp(e, "false")); }
static int menu_fix_on(void) { return !off_value(getenv("TOTK_MENU_BG_FIX")); }
/* the layouts' aspect (0: none): only wider than 16:9, only when the camera patch shows the wider view */
static double layouts_aspect(int *corner) {
    const char *h = getenv("TOTK_ASPECT_HUD");
    *corner = !(h && !uit_stricmp(h, "center"));
    if (off_value(h) || (h && !uit_stricmp(h, "stretch"))) return 0;
    if (!totk_aspect_on()) return 0;
    double a = uit_present_aspect();
    return a > 16.0 / 9.0 * 1.01 ? a : 0;
}
int romfix_wants(const char *path) {
    if (!path) return 0;
    if (!strcmp(path, ROMFIX_COMMON)) { int c; return menu_fix_on() || layouts_aspect(&c) > 0; }
    if (!strcmp(path, ROMFIX_HEAPDEF)) return uit_heap_extra_kib() > 0;
    return 0;
}

int romfix_apply(const char *path, const uint8_t *in, size_t n, int from_mod, RomfixRead rd, const char *cache_dir, uint8_t **out, size_t *on) {
    *out = NULL, *on = 0;
    if (!romfix_wants(path) || !in || n < 8) return 0;
    int common = !strcmp(path, ROMFIX_COMMON);
    unsigned id = ZSTD_getDictID_fromFrame(in, n);
    size_t dn = 0, len = 0;
    uint8_t *dict = id ? dictionary(id, rd, &dn) : NULL;
    if (id && !dict) { LOG("%s: its zstd dictionary %u is not in Pack/ZsDic.pack.zs: left as it is", path, id); return 0; }
    uint8_t *d = unzstd(in, n, dict, dn, &len);
    if (!d) { LOG("%s: not a zstd file this reads: left as it is", path); free(dict); return 0; }
    int changed = 0;
    if (common) {
        RomfixMember *m = NULL;
        int nm = romfix_sarc(d, len, &m);
        if (nm < 0) LOG("%s: not a SARC archive this reads: left as it is", path);
        if (nm >= 0 && menu_fix_on()) {
            int r = romfix_menu_bg(d, len, m, nm);
            if (r > 0) { changed = 1; LOG("quick-menu background: capture keys moved from frame 1.0 to 0.0 (TOTK_MENU_BG_FIX)"); }
            else if (!r) LOG("quick-menu background: the archive already captures early (a mod's fix): left as the mod has it");
            else LOG("quick-menu background: the capture keys are not the ones 1.4.3 ships (%d): not changed", r);
        }
        int corner;
        double a = nm >= 0 ? layouts_aspect(&corner) : 0;
        if (a > 0) {
            uint8_t *copy = malloc(len);
            if (copy) {
                memcpy(copy, d, len);
                if (romfix_layouts(copy, len, m, nm, a, corner) > 0) free(d), d = copy, changed = 1;
                else { free(copy); LOG("layouts: not the ones 1.4.3 ships: the UI stays 16:9 (stretched to the picture)"); }
            }
        }
        free(m);
    } else {
        uint64_t extra = uit_heap_extra_kib();
        /* the shipped sizes are checked unless a mod changed the table (then its own sizes get the same addition) */
        int r = romfix_heapdef(d, len, (uint32_t)extra, !from_mod);
        if (r > 0) changed = 1;
        else {
            LOG("%s: not the HeapDef 1.4.3 ships (%d): the texture heaps stay as they are, so the canvas stays 1080p and the "
                "scene within the game's own budget", path, r);
            uit_heap_refused();
        }
    }
    if (changed) {
        const char *tag = common ? "Common.Product.140.blarc" : "HeapDef.byml";
        *out = rezstd(tag, d, len, dict, dn, cache_dir, on);
        if (!*out) {
            LOG("%s: could not compress the edited file: left as it is", path);
            changed = 0;
            if (!common) uit_heap_refused();
        }
    }
    free(d), free(dict);
    return changed;
}
