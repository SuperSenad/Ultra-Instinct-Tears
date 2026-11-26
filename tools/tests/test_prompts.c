/* [UIT 0452] Box test of src/host/prompts.c on made-up files (no game data): the button map for each style and
 * binding, the button images of a made-up texture list (R8 block-linear, the Xbox swap and the PlayStation drawings),
 * the one-letter panes of a layout and a message file, and the charstrings. tools/tests/test_prompts.py drives the
 * font part (made-up OpenType CFF fonts from fontTools, checked again with fontTools) through the "font" mode below.
 *   clang -O1 -Isrc/host -I<zstd.h folder> -I<SDL3 include> tools/tests/test_prompts.c src/host/prompts.c \
 *       src/host/romfix.c src/host/uit_display.c -lzstd -lm -lpthread -o /tmp/t/prompts && /tmp/t/prompts
 *   /tmp/t/prompts font <xbox|ps4|ps5> in.sarc out.sarc       the font archive edit, for test_prompts.py
 *   /tmp/t/prompts glyphs <dir> <advance> <nominal>           each icon's charstring (unpadded and padded by 1..9)
 *   /tmp/t/prompts raster <dir>                              every icon as a 108x108 PGM, to look at */
#include "../../src/host/prompts.h"
#include "../../src/host/prompt_icons.h"
#include "../../src/host/input_map.h"
#include "romfix_fixtures.h"

const int main_output_res_patch = 1, main_aspect_patch = 1, main_shadow_patch = 1;   /* uit_display.c (romfix.c links it) */
static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

static const char *cfg[2][2];
static const char *get_cfg(const char *k) {
    for (int i = 0; i < 2; i++) if (cfg[i][0] && !strcmp(cfg[i][0], k)) return cfg[i][1];
    return NULL;
}

static void test_map(void) {
    PromptMap m;
    CHECK(prompts_map(UIT_PROMPTS_SWITCH, get_cfg, &m) == 0);
    for (int b = 0; b < PB_COUNT; b++) CHECK(m.game[b] == -1 && m.icon[b] == -1);
    /* Xbox, default bindings: Switch A is the right button = Xbox B, and so on; the letters follow */
    CHECK(prompts_map(UIT_PROMPTS_XBOX, get_cfg, &m) == PB_COUNT);
    CHECK(m.game[PB_A] == PB_B && m.game[PB_B] == PB_A && m.game[PB_X] == PB_Y && m.game[PB_Y] == PB_X);
    CHECK(m.icon[PB_A] == -1 && m.icon[PB_L] == PI_XB_LB && m.icon[PB_ZR] == PI_XB_RT && m.icon[PB_PLUS] == PI_XB_MENU && m.icon[PB_MINUS] == PI_XB_VIEW);
    CHECK(!memcmp(m.letter, "BAYX", 4));
    /* PlayStation: our symbols, by position */
    CHECK(prompts_map(UIT_PROMPTS_PS5, get_cfg, &m) == PB_COUNT);
    CHECK(m.icon[PB_A] == PI_PS_CIRCLE && m.icon[PB_B] == PI_PS_CROSS && m.icon[PB_X] == PI_PS_TRIANGLE && m.icon[PB_Y] == PI_PS_SQUARE);
    CHECK(m.game[PB_A] == -1 && m.icon[PB_MINUS] == PI_PS5_CREATE && m.icon[PB_L] == PI_PS_L1 && m.icon[PB_ZL] == PI_PS_L2);
    CHECK(!m.letter[0] && !m.letter[1]);
    CHECK(prompts_map(UIT_PROMPTS_PS4, get_cfg, &m) == PB_COUNT && m.icon[PB_MINUS] == PI_PS4_SHARE && m.icon[PB_PLUS] == PI_PS_OPTIONS);
    /* rebound: Switch A on the bottom button (Xbox A), Switch B unbound */
    cfg[0][0] = "TOTK_PAD_A", cfg[0][1] = "south,leftpaddle1";
    cfg[1][0] = "TOTK_PAD_B", cfg[1][1] = "none";
    CHECK(prompts_map(UIT_PROMPTS_XBOX, get_cfg, &m) == PB_COUNT - 2);
    CHECK(m.game[PB_A] == -1 && m.game[PB_B] == -1 && !m.letter[0] && !m.letter[1] && m.letter[2] == 'Y');
    CHECK(prompts_map(UIT_PROMPTS_PS4, get_cfg, &m) == PB_COUNT - 1 && m.icon[PB_A] == PI_PS_CROSS && m.icon[PB_B] == -1);
    cfg[0][1] = "rightshoulder";
    CHECK(prompts_map(UIT_PROMPTS_XBOX, get_cfg, &m) && m.game[PB_A] == -1 && m.icon[PB_A] == PI_XB_RB && !m.letter[0]);
    cfg[0][0] = cfg[1][0] = NULL;
    /* prompts.c's defaults are input_map.h's */
    const char *keys[PB_COUNT] = {"TOTK_PAD_A", "TOTK_PAD_B", "TOTK_PAD_X", "TOTK_PAD_Y", "TOTK_PAD_L", "TOTK_PAD_R", "TOTK_PAD_ZL", "TOTK_PAD_ZR", "TOTK_PAD_PLUS", "TOTK_PAD_MINUS"};
    for (int b = 0; b < PB_COUNT; b++)
        for (int i = 0; i < UIT_BUTTON_COUNT; i++)
            if (!strcmp(kUitButtons[i].pad_key, keys[b])) {
                cfg[0][0] = keys[b], cfg[0][1] = kUitButtons[i].pad_def;
                PromptMap a, d;
                prompts_map(UIT_PROMPTS_PS5, get_cfg, &a);
                cfg[0][0] = NULL;
                prompts_map(UIT_PROMPTS_PS5, get_cfg, &d);
                CHECK(a.icon[b] == d.icon[b]);
            }
}

/* ---- a made-up texture list: six R8 108x108 images, one mip, block height 16; plus one BC1 of the same name set ---- */
#define TW 108
static uint32_t tex_size(void) { uint32_t wg = (TW + 63) / 64, bh = 16; return wg * 512 * bh * ((TW + 8 * bh - 1) / (8 * bh)); }
static Buf bntx(uint32_t fmt_l) {
    static const char *names[6] = {"Nt_KeyTexA_00^q", "Nt_KeyTexB_00^q", "Nt_KeyTexX_00^q", "Nt_KeyTexY_00^q", "Nt_KeyTexL_00^q", "Nt_KeyTexZL_00^q"};
    Buf b = {0};
    zero(&b, 0x40);
    memcpy(b.d, "BNTX", 4), memcpy(b.d + 0x20, "NX  ", 4), w32(b.d + 0x24, 7);
    size_t table = b.n;
    zero(&b, 8 * 7);
    w32(b.d + 0x28, (uint32_t)table);
    for (int k = 0; k < 7; k++) {
        align(&b, 8);
        size_t info = b.n;
        zero(&b, 0x80);
        w32(b.d + table + 8 * k, (uint32_t)info);
        memcpy(b.d + info, "BRTI", 4);
        w16(b.d + info + 0x16, 1);
        w32(b.d + info + 0x1c, k == 4 ? fmt_l : 0x201);
        w32(b.d + info + 0x24, TW), w32(b.d + info + 0x28, TW), w32(b.d + info + 0x34, 4);
        w32(b.d + info + 0x50, tex_size());
        const char *nm = k < 6 ? names[k] : "Other_00^q";
        size_t na = b.n;
        uint8_t l2[2];
        w16(l2, (unsigned)strlen(nm));
        put(&b, l2, 2), put(&b, nm, strlen(nm) + 1);
        w32(b.d + info + 0x60, (uint32_t)na);
        align(&b, 8);
        size_t mt = b.n;
        zero(&b, 8);
        w32(b.d + info + 0x70, (uint32_t)mt);
        align(&b, 512);
        size_t data = b.n;
        w32(b.d + mt, (uint32_t)data);
        for (uint32_t i = 0; i < tex_size(); i++) { uint8_t v = (uint8_t)(k * 31 + i * 7); put(&b, &v, 1); }
    }
    return b;
}
static const uint8_t *tex_data(const uint8_t *d, size_t lo, int k) {
    size_t info = lo + r32(d + lo + 0x28 + 0) ; (void)info;
    size_t table = r32(d + lo + 0x28);
    size_t ia = lo + r32(d + lo + table + 8 * k);
    size_t mt = r32(d + ia + 0x70);
    return d + lo + r32(d + lo + mt);
}
/* a layout with the action prompt's letter panes (txt1, one UTF-16 letter) and one other text pane */
static Buf letter_flyt(void) {
    Buf b = {0};
    zero(&b, 0x14);
    memcpy(b.d, "FLYT", 4), w16(b.d + 6, 0x14);
    const char *names[6] = {"T_A_00", "T_B_00", "T_X_00", "T_Y_01", "T_Note_00", "T_A_00x"};
    const char *text[6] = {"A", "B", "X", "Y", "A", "A"};
    for (int i = 0; i < 6; i++) {
        size_t at = b.n;
        zero(&b, 0x70);
        memcpy(b.d + at, "txt1", 4), w32(b.d + at + 4, 0x70);
        strncpy((char *)b.d + at + 12, names[i], 24);
        w16(b.d + at + 0x56, 4), w32(b.d + at + 0x64, 0x68);
        b.d[at + 0x68] = (uint8_t)text[i][0];
    }
    return b;
}
static uint16_t pane_letter(const uint8_t *d, size_t lo, int i) { return (uint16_t)(d[lo + 0x14 + 0x70 * i + 0x68] | d[lo + 0x14 + 0x70 * i + 0x69] << 8); }

static void test_common(void) {
    Mem m[3];
    snprintf(m[0].name, 128, "timg/__Combined.bntx"), m[0].data = bntx(0x201);
    snprintf(m[1].name, 128, "blyt/DoCommand_00.bflyt"), m[1].data = letter_flyt();
    snprintf(m[2].name, 128, "anim/Other_00.bflan"), m[2].data = flan(NULL, NULL, 0);
    Buf a = sarc(m, 3);
    for (int style = UIT_PROMPTS_XBOX; style <= UIT_PROMPTS_PS5; style++) {
        uint8_t *d = malloc(a.n);
        memcpy(d, a.d, a.n);
        RomfixMember *mm = NULL;
        int nm = romfix_sarc(d, a.n, &mm);
        CHECK(nm == 3);
        PromptMap pm;
        prompts_map(style, get_cfg, &pm);
        int r = prompts_common(d, a.n, mm, nm, &pm);
        size_t lo = mm[0].lo, flo = mm[1].lo;
        const uint8_t *orig = a.d;
        uint8_t mask[TW * TW];
        uint32_t wg = (TW + 63) / 64;
        if (style == UIT_PROMPTS_XBOX) {
            CHECK(r == 6 + 4);   /* 6 images, 4 letters */
            CHECK(!memcmp(tex_data(d, lo, 0), tex_data(orig, lo, 1), tex_size()));   /* A shows B */
            CHECK(!memcmp(tex_data(d, lo, 1), tex_data(orig, lo, 0), tex_size()));
            CHECK(!memcmp(tex_data(d, lo, 2), tex_data(orig, lo, 3), tex_size()));
            CHECK(!memcmp(tex_data(d, lo, 3), tex_data(orig, lo, 2), tex_size()));
            prompts_raster(PI_XB_LB, TW, 4, mask);
            int same = 1;
            for (uint32_t y = 0; y < TW; y++) for (uint32_t x = 0; x < TW; x++) same &= tex_data(d, lo, 4)[prompts_gob_offset(x, y, wg, 16)] == mask[y * TW + x];
            CHECK(same);
            CHECK(!memcmp(tex_data(d, lo, 6), tex_data(orig, lo, 6), tex_size()));   /* other images untouched */
            CHECK(pane_letter(d, flo, 0) == 'B' && pane_letter(d, flo, 1) == 'A' && pane_letter(d, flo, 2) == 'Y' && pane_letter(d, flo, 3) == 'X');
            CHECK(pane_letter(d, flo, 4) == 'A' && pane_letter(d, flo, 5) == 'A');   /* not a letter pane */
        } else {
            CHECK(r == 6);
            prompts_raster(PI_PS_CIRCLE, TW, 4, mask);
            int same = 1;
            for (uint32_t y = 0; y < TW; y++) for (uint32_t x = 0; x < TW; x++) same &= tex_data(d, lo, 0)[prompts_gob_offset(x, y, wg, 16)] == mask[y * TW + x];
            CHECK(same);
            CHECK(pane_letter(d, flo, 0) == 'A');   /* letters stay for PlayStation */
        }
        /* nothing outside the images and the letters changed */
        size_t diff_out = 0;
        for (size_t i = 0; i < a.n; i++) if (d[i] != orig[i] && !(i >= lo && i < mm[0].hi) && !(i >= flo && i < mm[1].hi)) diff_out++;
        CHECK(diff_out == 0);
        CHECK(memcmp(d + mm[2].lo, orig + mm[2].lo, mm[2].hi - mm[2].lo) == 0);
        free(mm), free(d);
    }
    /* an L image that is not R8: left, the rest still done */
    free(m[0].data.d), m[0].data = bntx(0x1a01);
    Buf b = sarc(m, 3);
    RomfixMember *mm = NULL;
    int nm = romfix_sarc(b.d, b.n, &mm);
    PromptMap pm;
    prompts_map(UIT_PROMPTS_PS4, get_cfg, &pm);
    uint8_t *keep = malloc(b.n);
    memcpy(keep, b.d, b.n);
    CHECK(prompts_common(b.d, b.n, mm, nm, &pm) == 5);
    CHECK(!memcmp(tex_data(b.d, mm[0].lo, 4), tex_data(keep, mm[0].lo, 4), tex_size()));
    free(keep), free(mm), free(b.d), free(a.d);
    /* a BNTX that is not one */
    Mem bad[1];
    snprintf(bad[0].name, 128, "timg/__Combined.bntx"), bad[0].data = letter_flyt();
    Buf c = sarc(bad, 1);
    nm = romfix_sarc(c.d, c.n, &mm);
    CHECK(prompts_common(c.d, c.n, mm, nm, &pm) < 0);
    free(mm), free(c.d);
    for (int i = 0; i < 3; i++) free(m[i].data.d);
}

/* ---- a message file with the letters and other messages ---- */
static Buf msbt(void) {
    const char *labels[5] = {"T_A_00", "T_B_00", "T_X_00", "Talk", "T_Y_00"};
    const char *texts[5] = {"A", "B", "X", "Talk", "Y"};
    Buf lbl = {0}, txt = {0}, b = {0};
    uint8_t w[4];
    w32(w, 1), put(&lbl, w, 4), w32(w, 5), put(&lbl, w, 4), w32(w, 12), put(&lbl, w, 4);
    for (int i = 0; i < 5; i++) {
        uint8_t l = (uint8_t)strlen(labels[i]);
        put(&lbl, &l, 1), put(&lbl, labels[i], l), w32(w, (uint32_t)i), put(&lbl, w, 4);
    }
    w32(w, 5), put(&txt, w, 4);
    zero(&txt, 20);
    for (int i = 0; i < 5; i++) {
        w32(txt.d + 4 + 4 * i, (uint32_t)txt.n);
        for (const char *p = texts[i]; *p; p++) { uint8_t c[2] = {(uint8_t)*p, 0}; put(&txt, c, 2); }
        zero(&txt, 2);
    }
    zero(&b, 0x20);
    memcpy(b.d, "MsgStdBn", 8), b.d[8] = 0xff, b.d[9] = 0xfe, b.d[0x0c] = 1, w16(b.d + 0x0e, 2);
    Buf *sec[2] = {&lbl, &txt};
    const char *mag[2] = {"LBL1", "TXT2"};
    for (int s = 0; s < 2; s++) {
        size_t at = b.n;
        zero(&b, 16);
        memcpy(b.d + at, mag[s], 4), w32(b.d + at + 4, (uint32_t)sec[s]->n);
        put(&b, sec[s]->d, sec[s]->n), align(&b, 16);
    }
    free(lbl.d), free(txt.d);
    return b;
}
static void test_mals(void) {
    Mem m[2];
    snprintf(m[0].name, 128, "EventFlowMsg/Other.msbt"), m[0].data = msbt();
    snprintf(m[1].name, 128, "LayoutMsg/DoCommand_00.msbt"), m[1].data = msbt();
    Buf a = sarc(m, 2);
    PromptMap pm;
    prompts_map(UIT_PROMPTS_XBOX, get_cfg, &pm);
    uint8_t *d = malloc(a.n);
    memcpy(d, a.d, a.n);
    CHECK(prompts_mals(d, a.n, &pm) == 4);
    RomfixMember *mm = NULL;
    romfix_sarc(d, a.n, &mm);
    CHECK(!memcmp(d + mm[0].lo, a.d + mm[0].lo, mm[0].hi - mm[0].lo));   /* only DoCommand_00 */
    const uint8_t *t = d + mm[1].lo;
    size_t txt = 0x20;
    txt += 16 + ((r32(t + txt + 4) + 15) & ~15u);   /* past LBL1 */
    txt += 16;
    char got[6] = {0};
    for (int i = 0; i < 5; i++) got[i] = (char)t[txt + r32(t + txt + 4 + 4 * i)];
    CHECK(!memcmp(got, "BAYTX", 5));
    prompts_map(UIT_PROMPTS_PS5, get_cfg, &pm);
    CHECK(prompts_mals(d, a.n, &pm) == 0);
    free(mm), free(d), free(a.d), free(m[0].data.d), free(m[1].data.d);
}

static void test_icons(void) {
    /* every icon: a charstring, and the padding is exact */
    for (int i = 0; i < PI_COUNT; i++) {
        uint8_t buf[4096];
        int xmin = 0;
        size_t n = prompts_charstring(i, 760, 300, buf, sizeof buf, &xmin);
        CHECK(n > 10 && n < 1500 && buf[n - 1] == 14);
        CHECK(xmin >= 20 && xmin < 380);
        uint8_t mask[64 * 64];
        prompts_raster(i, 64, 4, mask);
        unsigned on = 0, edge = 0;
        for (int k = 0; k < 64 * 64; k++) on += mask[k] > 128;
        for (int k = 0; k < 64; k++) edge += mask[k] + mask[63 * 64 + k] + mask[k * 64] + mask[k * 64 + 63];
        CHECK(on > 64 * 64 / 6 && on < 64 * 64 * 9 / 10);   /* a shape with holes, not a blank or a solid square */
        CHECK(edge == 0);                                    /* the margin stays empty */
    }
    CHECK(prompts_gob_offset(0, 0, 2, 16) == 0 && prompts_gob_offset(16, 0, 2, 16) == 32 && prompts_gob_offset(0, 1, 2, 16) == 16);
    CHECK(prompts_gob_offset(64, 0, 2, 16) == 512 * 16 && prompts_gob_offset(0, 8, 2, 16) == 512);
}

static uint8_t *slurp(const char *p, size_t *n) {
    FILE *f = fopen(p, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long l = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *d = malloc((size_t)l + 1);
    *n = fread(d, 1, (size_t)l, f);
    fclose(f);
    return d;
}
static void spit(const char *p, const void *d, size_t n) { FILE *f = fopen(p, "wb"); fwrite(d, 1, n, f); fclose(f); }

int main(int argc, char **argv) {
    if (argc == 5 && !strcmp(argv[1], "font")) {
        int style = !strcmp(argv[2], "xbox") ? UIT_PROMPTS_XBOX : !strcmp(argv[2], "ps4") ? UIT_PROMPTS_PS4 : UIT_PROMPTS_PS5;
        size_t n = 0;
        uint8_t *d = slurp(argv[3], &n);
        if (!d) return 2;
        PromptMap pm;
        prompts_map(style, NULL, &pm);
        int r = prompts_font_archive(d, n, &pm);
        printf("result %d\n", r);
        spit(argv[4], d, n);
        return 0;
    }
    if (argc == 5 && !strcmp(argv[1], "glyphs")) {
        int adv = atoi(argv[3]), nom = atoi(argv[4]);
        for (int i = 0; i < PI_COUNT; i++) {
            for (int pad = 0; pad <= 9; pad++) {
                uint8_t buf[8192];
                size_t len = 0;
                int xmin = 0;
                extern int prompts_test_build(int, int, int, size_t, uint8_t *, size_t, size_t *, int *);
                if (prompts_test_build(i, adv, nom, (size_t)pad, buf, sizeof buf, &len, &xmin)) { printf("FAIL %s pad %d\n", kPromptIcons[i].name, pad); return 1; }
                char p[512];
                snprintf(p, sizeof p, "%s/%s.%d.cs", argv[2], kPromptIcons[i].name, pad);
                spit(p, buf, len);
            }
        }
        return 0;
    }
    if (argc == 3 && !strcmp(argv[1], "raster")) {
        for (int i = 0; i < PI_COUNT; i++) {
            uint8_t mask[TW * TW];
            prompts_raster(i, TW, 4, mask);
            char p[512];
            snprintf(p, sizeof p, "%s/%s.pgm", argv[2], kPromptIcons[i].name);
            FILE *f = fopen(p, "wb");
            fprintf(f, "P5\n%d %d\n255\n", TW, TW);
            fwrite(mask, 1, sizeof mask, f);
            fclose(f);
        }
        return 0;
    }
    test_map();
    test_common();
    test_mals();
    test_icons();
    printf(fails ? "prompts: %d FAILED\n" : "prompts: all ok\n", fails);
    return fails != 0;
}
