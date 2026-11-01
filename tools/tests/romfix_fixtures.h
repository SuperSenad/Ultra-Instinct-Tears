/* [UIT 0065] Made-up files for the romfix tests (tools/tests/test_romfix.c, test_fs_romfix.c): BFLAN visibility tracks,
 * BFLYT panes, SARC archives, a BYML HeapDef and a zstd dictionary pack like the game's. No game data. */
#include "../../src/host/romfix.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zstd.h>

size_t ZDICT_trainFromBuffer(void *dict, size_t cap, const void *samples, const size_t *sizes, unsigned n);
unsigned ZDICT_isError(size_t r);

typedef struct { uint8_t *d; size_t n, cap; } Buf;
static void put(Buf *b, const void *p, size_t n) {
    if (b->n + n > b->cap) { b->cap = (b->n + n) * 2 + 64; b->d = realloc(b->d, b->cap); }
    memcpy(b->d + b->n, p, n), b->n += n;
}
static void zero(Buf *b, size_t n) { while (n--) put(b, "", 1); }
static void align(Buf *b, size_t a) { while (b->n % a) put(b, "", 1); }
static void w16(uint8_t *p, unsigned v) { p[0] = (uint8_t)v, p[1] = (uint8_t)(v >> 8); }
static void w32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v, p[1] = (uint8_t)(v >> 8), p[2] = (uint8_t)(v >> 16), p[3] = (uint8_t)(v >> 24); }
static uint32_t r32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static void wf(uint8_t *p, float f) { memcpy(p, &f, 4); }
static float rf(const uint8_t *p) { float f; memcpy(&f, p, 4); return f; }

/* ---- BFLAN with visibility tracks: panes[i] gets keys (0, 0) (frame[i], 1) ---- */
static Buf flan(const char *const *panes, const float *frame, int np) {
    Buf b = {0};
    zero(&b, 0x14);
    memcpy(b.d, "FLAN", 4), b.d[4] = 0xff, b.d[5] = 0xfe, w16(b.d + 0x10, 1);
    size_t pai = b.n;
    zero(&b, 20);
    memcpy(b.d + pai, "pai1", 4), w16(b.d + pai + 14, (unsigned)np), w32(b.d + pai + 16, 20);
    size_t table = b.n;
    zero(&b, 4 * (size_t)np);
    for (int i = 0; i < np; i++) {
        size_t eo = b.n;
        w32(b.d + table + 4 * i, (uint32_t)(eo - pai));
        zero(&b, 36);
        strncpy((char *)b.d + eo, panes[i], 28), b.d[eo + 28] = 1, w32(b.d + eo + 32, 36);
        size_t to = b.n;
        zero(&b, 12);
        memcpy(b.d + to, "FLVI", 4), b.d[to + 4] = 1, w32(b.d + to + 8, 12);
        size_t go = b.n;
        zero(&b, 12);
        b.d[go + 2] = 1, w16(b.d + go + 4, 2), w32(b.d + go + 8, 12);
        size_t ko = b.n;
        zero(&b, 16);
        wf(b.d + ko, 0), w16(b.d + ko + 4, 0), wf(b.d + ko + 8, frame[i]), w16(b.d + ko + 12, 1);
    }
    w32(b.d + pai + 4, (uint32_t)(b.n - pai));
    return b;
}
/* ---- BFLYT: RootPane and the named panes, every field at its shipped value ---- */
typedef struct { const char *pane; unsigned field; float v; } PF;
static Buf flyt(const char *layout, PF *pf, int npf, float root_x) {
    const char *names[64];
    int nn = 0;
    names[nn++] = "RootPane";
    for (int i = 0; i < npf; i++) {
        int have = 0;
        for (int j = 0; j < nn; j++) have |= !strcmp(names[j], pf[i].pane);
        if (!have) names[nn++] = pf[i].pane;
    }
    Buf b = {0};
    zero(&b, 0x14);
    memcpy(b.d, "FLYT", 4), w16(b.d + 6, 0x14), w16(b.d + 0x10, (unsigned)nn + 1);
    size_t lyt = b.n;   /* a non-pane section first */
    zero(&b, 16);
    memcpy(b.d + lyt, "lyt1", 4), w32(b.d + lyt + 4, 16);
    for (int j = 0; j < nn; j++) {
        size_t at = b.n;
        zero(&b, 0x54);
        memcpy(b.d + at, j % 2 ? "pic1" : "pan1", 4), w32(b.d + at + 4, 0x54);
        strncpy((char *)b.d + at + 12, names[j], 24);
        wf(b.d + at + 68, 1), wf(b.d + at + 72, 1);
        if (!j) wf(b.d + at + 44, root_x);
        for (int i = 0; i < npf; i++) if (!strcmp(pf[i].pane, names[j])) wf(b.d + at + pf[i].field, pf[i].v);
    }
    (void)layout;
    return b;
}
static size_t pane_off(const Buf *b, const char *name) {
    for (size_t at = 0x14; at + 8 <= b->n; at += r32(b->d + at + 4))
        if (memcmp(b->d + at, "lyt1", 4) && !strcmp((const char *)b->d + at + 12, name)) return at;
    return 0;
}
/* ---- SARC ---- */
typedef struct { char name[128]; Buf data; } Mem;
static Buf sarc(Mem *m, int nm) {
    Buf names = {0}, data = {0}, b = {0};
    uint32_t *noff = calloc((size_t)nm, 4), *lo = calloc((size_t)nm, 4), *hi = calloc((size_t)nm, 4);
    for (int i = 0; i < nm; i++) {
        noff[i] = (uint32_t)(names.n / 4);
        put(&names, m[i].name, strlen(m[i].name) + 1), align(&names, 4);
        align(&data, 8), lo[i] = (uint32_t)data.n, put(&data, m[i].data.d, m[i].data.n), hi[i] = (uint32_t)data.n;
    }
    zero(&b, 0x14 + 12 + 16 * (size_t)nm + 8);
    memcpy(b.d, "SARC", 4), w16(b.d + 4, 0x14), b.d[6] = 0xff, b.d[7] = 0xfe;
    memcpy(b.d + 0x14, "SFAT", 4), w16(b.d + 0x18, 12), w16(b.d + 0x1a, (unsigned)nm), w32(b.d + 0x1c, 0x65);
    for (int i = 0; i < nm; i++) {
        uint8_t *e = b.d + 0x20 + 16 * i;
        w32(e, (uint32_t)i), w32(e + 4, 0x01000000 | noff[i]), w32(e + 8, lo[i]), w32(e + 12, hi[i]);
    }
    size_t sfnt = 0x20 + 16 * (size_t)nm;
    memcpy(b.d + sfnt, "SFNT", 4), w16(b.d + sfnt + 4, 8);
    put(&b, names.d, names.n), align(&b, 64);
    size_t start = b.n;
    put(&b, data.d, data.n);
    w32(b.d + 8, (uint32_t)b.n), w32(b.d + 12, (uint32_t)start);
    free(names.d), free(data.d), free(noff), free(lo), free(hi);
    return b;
}
static const char *SC[] = {"C_CaptureBG_00", "C_CaptureClearBG_00", "P_CaptureClearBG_00", "N_Other_00"};
static const char *AB[] = {"C_CaptureBG_00"};
/* the made-up Common archive: capture keys at `frame`; layouts with every field the edits check ("Foo_00" extra) */
static Buf common(float frame, int bad_field) {
    static Mem m[128];
    int nm = 0;
    float f4[4] = {frame, frame, frame, 1.0f}, f1[1] = {frame};
    snprintf(m[nm].name, 128, "anim/ShortCutBG_00_In.bflan"), m[nm++].data = flan(SC, f4, 4);
    snprintf(m[nm].name, 128, "anim/AutoBuilder_00_In.bflan"), m[nm++].data = flan(AB, f1, 1);
    const char *layouts[96];
    int nl = 0;
    for (int i = 0; i < 10; i++) layouts[nl++] = romfix_keep_root[i];
    layouts[nl++] = "Foo_00";
    const char *L, *P; unsigned F; float S;
    for (unsigned i = 0; romfix_layout_field(i, 1, &L, &P, &F, &S); i++) {
        if (!P) continue;
        int have = 0;
        for (int j = 0; j < nl; j++) have |= !strcmp(layouts[j], L);
        if (!have) layouts[nl++] = L;
    }
    int nfield = 0;
    for (int j = 0; j < nl; j++) {
        PF pf[64];
        int np = 0;
        float root_x = 0;
        for (unsigned i = 0; romfix_layout_field(i, 1, &L, &P, &F, &S); i++) {
            if (!P || strcmp(L, layouts[j])) continue;
            if (!strcmp(P, "RootPane")) { if (F == 44) root_x = S; continue; }
            if (S != 10000.0f && nfield++ == bad_field) S += 5;   /* out-of-sight fields take any value */
            pf[np++] = (PF){P, F, S};
        }
        snprintf(m[nm].name, 128, "blyt/%s.bflyt", layouts[j]), m[nm++].data = flyt(layouts[j], pf, np, root_x);
    }
    Buf zoom = {0}, mini = {0};
    zero(&zoom, 304), zero(&mini, 3480);
    for (unsigned i = 0; romfix_layout_field(i, 1, &L, &P, &F, &S); i++)
        if (!P) wf((!strcmp(L, "AppMap_00_MiniMap") ? mini : zoom).d + F, S);
    snprintf(m[nm].name, 128, "anim/PaMapIconDragonTears_00_Zoom.bflan"), m[nm++].data = zoom;
    snprintf(m[nm].name, 128, "anim/AppMap_00_MiniMap.bflan"), m[nm++].data = mini;
    Buf out = sarc(m, nm);
    for (int i = 0; i < nm; i++) free(m[i].data.d);
    return out;
}
static const RomfixMember *find(const RomfixMember *m, int nm, const char *name) {
    for (int i = 0; i < nm; i++) if (!strcmp(m[i].name, name)) return &m[i];
    return NULL;
}
/* the visibility key frame of pane in the member (-1: none) */
static float key_of(const uint8_t *d, const RomfixMember *mm, const char *pane) {
    Buf b = {(uint8_t *)d + mm->lo, mm->hi - mm->lo, 0};
    size_t pai = 0x14, table = pai + 20;
    unsigned n = b.d[pai + 14] | b.d[pai + 15] << 8;
    for (unsigned e = 0; e < n; e++) {
        size_t eo = pai + r32(b.d + table + 4 * e);
        if (!strcmp((const char *)b.d + eo, pane)) return rf(b.d + eo + 36 + 12 + 12 + 8);
    }
    return -1;
}
__attribute__((unused)) static float field(const uint8_t *d, const RomfixMember *m, int nm, const char *layout, const char *pane, unsigned f) {
    char mn[128];
    snprintf(mn, sizeof mn, "blyt/%s.bflyt", layout);
    const RomfixMember *mm = find(m, nm, mn);
    if (!mm) return NAN;
    Buf b = {(uint8_t *)d + mm->lo, mm->hi - mm->lo, 0};
    size_t at = pane_off(&b, pane);
    return at ? rf(b.d + at + f) : NAN;
}
/* ---- BYML HeapDef: Heaps -> {DynamicTextureAllocatorMem, Graphics, Other} -> Size ---- */
static Buf heapdef(int32_t dta, int32_t gfx) {
    static const char *K[] = {"DynamicTextureAllocatorMem", "Graphics", "Heaps", "Other", "Parent", "Size"};
    Buf b = {0};
    zero(&b, 16);
    memcpy(b.d, "YB", 2), w16(b.d + 2, 7);
    size_t kt = b.n;
    zero(&b, 4 + 4 * 7);
    b.d[kt] = 0xc2, b.d[kt + 1] = 6;
    for (int i = 0; i < 6; i++) { w32(b.d + kt + 4 + 4 * i, (uint32_t)(b.n - kt)); put(&b, K[i], strlen(K[i]) + 1); }
    w32(b.d + kt + 4 + 24, (uint32_t)(b.n - kt));
    align(&b, 4);
    w32(b.d + 4, (uint32_t)kt);
    size_t h[3];
    int32_t sizes[3] = {dta, gfx, 4096};
    for (int i = 0; i < 3; i++) {   /* {Parent: 7 (an int here), Size: n} */
        h[i] = b.n;
        zero(&b, 4 + 16);
        b.d[h[i]] = 0xc1, b.d[h[i] + 1] = 2;
        uint8_t *e = b.d + h[i] + 4;
        e[0] = 4, e[3] = 0xd1, w32(e + 4, 7);
        e[8] = 5, e[11] = 0xd1, w32(e + 12, (uint32_t)sizes[i]);
    }
    size_t heaps = b.n;
    zero(&b, 4 + 24);
    b.d[heaps] = 0xc1, b.d[heaps + 1] = 3;
    int ki[3] = {0, 1, 3};
    for (int i = 0; i < 3; i++) { uint8_t *e = b.d + heaps + 4 + 8 * i; e[0] = (uint8_t)ki[i], e[3] = 0xc1, w32(e + 4, (uint32_t)h[i]); }
    size_t root = b.n;
    zero(&b, 12);
    b.d[root] = 0xc1, b.d[root + 1] = 1, b.d[root + 4] = 2, b.d[root + 7] = 0xc1, w32(b.d + root + 8, (uint32_t)heaps);
    w32(b.d + 12, (uint32_t)root);
    return b;
}
static int32_t heap_size(const uint8_t *d, int which) {   /* which: 0 DTA, 1 Graphics, 2 Other */
    size_t root = r32(d + 12), heaps = r32(d + root + 8), h = r32(d + heaps + 4 + 8 * which + 4);
    return (int32_t)r32(d + h + 4 + 12);
}

/* ---- the game's dictionary pack ---- */
static uint8_t *dict_buf;
static size_t dict_n;
static Buf pack_zs;
__attribute__((unused)) static uint8_t *rd(const char *path, size_t *n) {
    if (strcmp(path, "Pack/ZsDic.pack.zs")) return NULL;
    uint8_t *d = malloc(pack_zs.n);
    memcpy(d, pack_zs.d, pack_zs.n), *n = pack_zs.n;
    return d;
}
static Buf zs(const Buf *raw, const void *dict, size_t dn) {
    Buf o = {0};
    o.cap = ZSTD_compressBound(raw->n), o.d = malloc(o.cap);
    ZSTD_CCtx *c = ZSTD_createCCtx();
    o.n = ZSTD_compress_usingDict(c, o.d, o.cap, raw->d, raw->n, dict, dn, 3);
    ZSTD_freeCCtx(c);
    return o;
}
static uint8_t *unzs(const uint8_t *z, size_t n, size_t *on) {
    unsigned long long c = ZSTD_getFrameContentSize(z, n);
    uint8_t *d = malloc((size_t)c + 1);
    ZSTD_DCtx *x = ZSTD_createDCtx();
    *on = ZSTD_decompress_usingDict(x, d, (size_t)c, z, n, dict_buf, dict_n);
    ZSTD_freeDCtx(x);
    return d;
}

/* a dictionary trained on made-up layouts, and Pack/ZsDic.pack.zs holding it */
static void make_dict(void) {
    Buf samples = {0};
    size_t sizes[200];
    for (int i = 0; i < 200; i++) {
        Buf s = common(1.0f, -1);
        size_t at = (size_t)(i * 997) % (s.n - 2048);
        put(&samples, s.d + at, 2048), sizes[i] = 2048;
        free(s.d);
    }
    dict_buf = malloc(16384);
    dict_n = ZDICT_trainFromBuffer(dict_buf, 16384, samples.d, sizes, 200);
    if (ZDICT_isError(dict_n) || !ZSTD_getDictID_fromDict(dict_buf, dict_n)) { printf("no dictionary\n"); exit(2); }
    free(samples.d);
    Mem pm[2];
    snprintf(pm[0].name, 128, "other.zsdic"), pm[0].data = (Buf){(uint8_t *)"not a dictionary", 16, 0};
    snprintf(pm[1].name, 128, "zs.zsdic"), pm[1].data = (Buf){dict_buf, dict_n, 0};
    Buf pack = sarc(pm, 2);
    pack_zs = zs(&pack, NULL, 0);
    free(pack.d);
}
