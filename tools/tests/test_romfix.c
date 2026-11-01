/* [UIT 0065 / 0068 / 0069] Box test of src/host/romfix.c on made-up files (no game data): the quick-menu capture keys,
 * the UI layouts wider than 16:9, the HeapDef heaps, and the whole path with a zstd dictionary like the game's
 * (Pack/ZsDic.pack.zs), the result cache, a mod's already-fixed archive and data that is not the shipped one.
 *   cc -O1 -I<zstd.h folder> tools/tests/test_romfix.c src/host/romfix.c src/host/uit_display.c -lzstd -lm -o /tmp/t/romfix
 * (on the box: -I/tmp/zinc and /usr/lib/x86_64-linux-gnu/libzstd.so.1 in place of -lzstd) */
#include "../../src/host/romfix.h"
#include "../../src/host/uit_display.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <zstd.h>

const int main_output_res_patch = 1, main_aspect_patch = 1;
const int main_shadow_patch = 1;   /* [UIT 0070] the confirmed shadow-size site (TOTK_SHADOW_RES) */

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

#include "romfix_fixtures.h"

int main(void) {
    const double K21 = (16.0 / 9.0) / (21.0 / 9.0), W21 = 960 * (1 / K21 - 1);
    unsetenv("TOTK_ASPECT"), unsetenv("TOTK_OUTPUT_RES"), unsetenv("TOTK_RENDER_RES"), unsetenv("TOTK_MENU_BG_FIX");
    unsetenv("TOTK_UPSCALER"), unsetenv("TOTK_ASPECT_HUD");

    /* the capture keys */
    Buf c = common(1.0f, -1);
    RomfixMember *m = NULL;
    int nm = romfix_sarc(c.d, c.n, &m);
    CHECK(nm > 40);
    const RomfixMember *sc = find(m, nm, "anim/ShortCutBG_00_In.bflan"), *ab = find(m, nm, "anim/AutoBuilder_00_In.bflan");
    CHECK(sc && ab && key_of(c.d, sc, "C_CaptureBG_00") == 1.0f);
    CHECK(romfix_menu_bg(c.d, c.n, m, nm) == 1);
    for (int i = 0; i < 3; i++) CHECK(key_of(c.d, sc, SC[i]) == 0.0f);
    CHECK(key_of(c.d, sc, "N_Other_00") == 1.0f);   /* other panes stay */
    CHECK(key_of(c.d, ab, "C_CaptureBG_00") == 0.0f);
    CHECK(romfix_menu_bg(c.d, c.n, m, nm) == 0);    /* done already (a mod's fix): left */
    free(m), free(c.d);
    c = common(2.0f, -1);                            /* keys that are not 1.4.3's: refused, unchanged */
    nm = romfix_sarc(c.d, c.n, &m);
    CHECK(romfix_menu_bg(c.d, c.n, m, nm) < 0);
    CHECK(key_of(c.d, find(m, nm, "anim/ShortCutBG_00_In.bflan"), "C_CaptureBG_00") == 2.0f);
    free(m), free(c.d);
    CHECK(romfix_sarc((const uint8_t *)"SARCxxxxxxxxxxxxxxxxxxxx", 24, &m) < 0);

    /* the layouts at 21:9 */
    c = common(1.0f, -1);
    nm = romfix_sarc(c.d, c.n, &m);
    CHECK(romfix_layouts(c.d, c.n, m, nm, 21.0 / 9.0, 1) == 1);
    CHECK(fabs(field(c.d, m, nm, "Foo_00", "RootPane", 68) - K21) < 1e-6);          /* narrowed to 16:9 */
    CHECK(field(c.d, m, nm, "PauseMenuBG_00", "RootPane", 68) == 1.0f);             /* full-screen: kept */
    CHECK(fabs(field(c.d, m, nm, "SaveLoadIcon_00", "RootPane", 44) - -960 * (1 - K21)) < 1e-3);
    CHECK(fabs(field(c.d, m, nm, "Location_00", "N_Small_00", 44) - -(388 + W21)) < 1e-3);
    CHECK(fabs(field(c.d, m, nm, "PauseMenuOverlay_00", "P_DecoR_00", 44) - (737 + W21) * K21) < 1e-3);
    CHECK(fabs(field(c.d, m, nm, "GameOver_00", "P_BG_00", 68) - 1 / K21) < 1e-5);
    CHECK(field(c.d, m, nm, "ChallengeLog_00", "C_CaptureUp_00", 44) == 10000.0f);
    const RomfixMember *mini = find(m, nm, "anim/AppMap_00_MiniMap.bflan");
    CHECK(fabs(rf(c.d + mini->lo + 872) - (765 + W21)) < 1e-3);
    free(m), free(c.d);
    c = common(1.0f, -1);                            /* the HUD in the middle: no corner moves */
    nm = romfix_sarc(c.d, c.n, &m);
    CHECK(romfix_layouts(c.d, c.n, m, nm, 21.0 / 9.0, 0) == 1);
    CHECK(field(c.d, m, nm, "Location_00", "N_Small_00", 44) == -388.0f);
    CHECK(rf(c.d + find(m, nm, "anim/AppMap_00_MiniMap.bflan")->lo + 872) == 765.0f);
    CHECK(romfix_layouts(c.d, c.n, m, nm, 16.0 / 9.0, 1) < 0);                     /* only wider than 16:9 */
    free(m), free(c.d);
    for (int bad = 0; bad < 83; bad += 9) {        /* any field off its shipped value: refused */
        c = common(1.0f, bad);
        nm = romfix_sarc(c.d, c.n, &m);
        CHECK(romfix_layouts(c.d, c.n, m, nm, 21.0 / 9.0, 1) < 0);
        free(m), free(c.d);
    }

    /* damaged archives and HeapDefs: never read or write outside the data (run under -fsanitize=address) */
    srand(7);
    Buf good = common(1.0f, -1), hd = heapdef(113664, 201728);
    for (int it = 0; it < 3000; it++) {
        int heap = it % 3 == 2;
        Buf *src = heap ? &hd : &good;
        size_t n = it % 5 ? src->n : (size_t)rand() % src->n;
        uint8_t *d = malloc(n ? n : 1);
        memcpy(d, src->d, n);
        for (int f = 0; f < 1 + it % 8; f++) if (n) d[(size_t)rand() % (f % 2 && n > 512 ? 512 : n)] = (uint8_t)(f % 3 ? rand() : 0xff);
        if (heap) romfix_heapdef(d, n, 2048, 1);
        else {
            int k = romfix_sarc(d, n, &m);
            if (k > 0) { romfix_menu_bg(d, n, m, k), romfix_layouts(d, n, m, k, 21.0 / 9.0, 1); free(m); }
        }
        free(d);
    }
    free(good.d), free(hd.d);

    /* the HeapDef */
    Buf h = heapdef(113664, 201728);
    CHECK(romfix_heapdef(h.d, h.n, 2048, 1) == 1);
    CHECK(heap_size(h.d, 0) == 113664 + 2048 && heap_size(h.d, 1) == 201728 + 2048 && heap_size(h.d, 2) == 4096);
    CHECK(romfix_heapdef(h.d, h.n, 2048, 1) < 0);    /* not the shipped sizes any more */
    CHECK(romfix_heapdef(h.d, h.n, 1024, 0) == 1 && heap_size(h.d, 0) == 113664 + 3072);   /* a mod's: added on top */
    free(h.d);

    /* the whole path, with a trained dictionary like the game's */
    make_dict();
    CHECK(dict_n > 0);
    char cache[] = "/tmp/romfix-test-XXXXXX";
    CHECK(mkdtemp(cache) != NULL);
    setenv("TOTK_ASPECT", "21:9", 1);
    CHECK(romfix_wants(ROMFIX_COMMON) && !romfix_wants(ROMFIX_HEAPDEF) && !romfix_wants("Other/File.zs"));
    Buf raw = common(1.0f, -1), z = zs(&raw, dict_buf, dict_n);
    uint8_t *out = NULL, *out2 = NULL;
    size_t on = 0, on2 = 0, dn = 0;
    CHECK(romfix_apply(ROMFIX_COMMON, z.d, z.n, 0, rd, cache, &out, &on) == 1);
    CHECK(out && ZSTD_getDictID_fromFrame(out, on) == ZSTD_getDictID_fromDict(dict_buf, dict_n));
    CHECK(out && ZSTD_getFrameContentSize(out, on) == raw.n);
    uint8_t *dec = out ? unzs(out, on, &dn) : NULL;
    CHECK(dn == raw.n);
    nm = romfix_sarc(raw.d, raw.n, &m);
    CHECK(fabs(uit_present_aspect() - 21.0 / 9.0) < 1e-6);
    CHECK(romfix_menu_bg(raw.d, raw.n, m, nm) == 1 && romfix_layouts(raw.d, raw.n, m, nm, uit_present_aspect(), 1) == 1);
    CHECK(dec && !memcmp(dec, raw.d, raw.n));        /* exactly the two edits, nothing else */
    if (dec && memcmp(dec, raw.d, raw.n)) for (size_t i = 0; i < raw.n; i++) if (dec[i] != raw.d[i]) { printf("  differs at %zu: %02x vs %02x\n", i, dec[i], raw.d[i]); break; }
    free(m), free(dec);
    CHECK(romfix_apply(ROMFIX_COMMON, z.d, z.n, 0, rd, cache, &out2, &on2) == 1);   /* from the cache: the same */
    CHECK(on2 == on && out2 && !memcmp(out, out2, on));
    free(out), free(out2), free(z.d), free(raw.d);

    setenv("TOTK_ASPECT_HUD", "off", 1);             /* layouts off, the menu fix alone */
    raw = common(1.0f, -1), z = zs(&raw, dict_buf, dict_n);
    CHECK(romfix_apply(ROMFIX_COMMON, z.d, z.n, 0, rd, NULL, &out, &on) == 1);
    dec = unzs(out, on, &dn);
    nm = romfix_sarc(dec, dn, &m);
    CHECK(key_of(dec, find(m, nm, "anim/ShortCutBG_00_In.bflan"), "C_CaptureBG_00") == 0.0f);
    CHECK(field(dec, m, nm, "Foo_00", "RootPane", 68) == 1.0f);
    free(m), free(dec), free(out), free(z.d), free(raw.d);
    unsetenv("TOTK_ASPECT_HUD");

    setenv("TOTK_ASPECT", "16:10", 1);               /* narrower than 16:9: not supported, nothing but the menu fix */
    /* (totk_aspect_on decides once per run: its 21:9 stays; checked through the layouts call above instead) */
    unsetenv("TOTK_ASPECT");

    setenv("TOTK_MENU_BG_FIX", "0", 1);              /* a mod's archive that already captures early, layouts on: */
    raw = common(0.0f, -1), z = zs(&raw, dict_buf, dict_n);
    CHECK(romfix_apply(ROMFIX_COMMON, z.d, z.n, 1, rd, NULL, &out, &on) == 1);   /* only the layouts */
    dec = unzs(out, on, &dn);
    nm = romfix_sarc(dec, dn, &m);
    CHECK(fabs(field(dec, m, nm, "Foo_00", "RootPane", 68) - K21) < 1e-6);
    free(m), free(dec), free(out);
    setenv("TOTK_ASPECT_HUD", "off", 1);
    CHECK(!romfix_wants(ROMFIX_COMMON));             /* nothing to do: the file as it is */
    CHECK(romfix_apply(ROMFIX_COMMON, z.d, z.n, 1, rd, NULL, &out, &on) == 0 && !out);
    unsetenv("TOTK_MENU_BG_FIX");
    CHECK(romfix_apply(ROMFIX_COMMON, z.d, z.n, 1, rd, NULL, &out, &on) == 0 && !out);   /* a mod's fix: left */
    free(z.d), free(raw.d);
    raw = common(3.0f, -1), z = zs(&raw, dict_buf, dict_n);                        /* unknown keys: left */
    CHECK(romfix_apply(ROMFIX_COMMON, z.d, z.n, 0, rd, NULL, &out, &on) == 0 && !out);
    free(z.d), free(raw.d);

    /* [UIT 0070] Shadow resolution: the setting, and the heap it adds (6 square 32-bit layers over 512) */
    CHECK(uit_shadow_res_setting() == 0);
    const char *bad[] = {"-", "0", "off", "game", "300", "1024x", "8192", ""};
    for (int i = 0; i < 8; i++) { setenv("TOTK_SHADOW_RES", bad[i], 1); CHECK(uit_shadow_res_setting() == 0); }
    const unsigned sizes[] = {512, 1024, 2048, 4096};
    const uint64_t kib[] = {0, 18432, 92160, 387072};
    for (int i = 0; i < 4; i++) {
        char v[8]; snprintf(v, sizeof v, "%u", sizes[i]); setenv("TOTK_SHADOW_RES", v, 1);
        CHECK(uit_shadow_res_setting() == sizes[i] && uit_shadow_res() == sizes[i]);
        CHECK(uit_shadow_extra_kib_for(sizes[i]) == kib[i]);
        CHECK(uit_heap_extra_kib() == kib[i]);
        CHECK(romfix_wants(ROMFIX_HEAPDEF) == (kib[i] > 0));
    }
    CHECK(uit_shadow_extra_kib_for(0) == 0 && uit_shadow_extra_kib_for(256) == 0);
    unsetenv("TOTK_SHADOW_RES");

    /* the HeapDef through the whole path (no dictionary, as the game ships it) */
    CHECK(!romfix_wants(ROMFIX_HEAPDEF));
    setenv("TOTK_OUTPUT_RES", "2560x1440", 1);
    uint64_t extra = uit_heap_extra_kib();
    CHECK(extra == 89088);   /* ceil(113664 * 2560*1440/1920/1080 / 1024) * 1024 - 113664 */
    CHECK(romfix_wants(ROMFIX_HEAPDEF));
    raw = heapdef(113664, 201728), z = zs(&raw, NULL, 0);
    CHECK(romfix_apply(ROMFIX_HEAPDEF, z.d, z.n, 0, rd, cache, &out, &on) == 1);
    CHECK(out && ZSTD_getDictID_fromFrame(out, on) == 0);
    dec = out ? unzs(out, on, &dn) : NULL;
    CHECK(dec && dn == raw.n && heap_size(dec, 0) == 113664 + (int32_t)extra && heap_size(dec, 1) == 201728 + (int32_t)extra);
    free(dec), free(out), free(z.d), free(raw.d);
    raw = heapdef(200000, 300000), z = zs(&raw, NULL, 0);                          /* a mod's own sizes: grown too */
    CHECK(romfix_apply(ROMFIX_HEAPDEF, z.d, z.n, 1, rd, NULL, &out, &on) == 1);
    dec = unzs(out, on, &dn);
    CHECK(heap_size(dec, 0) == 200000 + (int32_t)extra);
    free(dec), free(out);
    unsigned w = 0, hh = 0;
    CHECK(uit_output_res(&w, &hh) == 1 && w == 2560);
    CHECK(romfix_apply(ROMFIX_HEAPDEF, z.d, z.n, 0, rd, NULL, &out, &on) == 0);   /* the game's with other sizes: refused */
    CHECK(uit_output_res(&w, &hh) == 0);             /* ... and the canvas falls back to 1080p */
    free(z.d), free(raw.d);

    char cmd[200];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", cache);
    if (system(cmd)) {}
    printf(fails ? "romfix: %d failed\n" : "romfix: all ok\n", fails);
    return fails != 0;
}
