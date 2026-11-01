/* [UIT 0042] Box test of src/host/restbl.c: the resource size table merge for mods, on made-up tables (no game data).
 *   cc -O2 -I<zstd.h folder> tools/tests/test_restbl.c src/host/restbl.c -lzstd -o /tmp/t/restbl && /tmp/t/restbl
 * (on the box: -I/tmp/zinc and /usr/lib/x86_64-linux-gnu/libzstd.so.1 in place of -lzstd) */
#include "../../src/host/restbl.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zstd.h>

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

typedef struct { uint32_t crc, size; } E;
typedef struct { const char *name; uint32_t size; } N;
static void w32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v, p[1] = (uint8_t)(v >> 8), p[2] = (uint8_t)(v >> 16), p[3] = (uint8_t)(v >> 24); }
static uint32_t r32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static int ecmp(const void *a, const void *b) { uint32_t x = ((const E *)a)->crc, y = ((const E *)b)->crc; return x < y ? -1 : x > y; }
/* a table: names become crc entries, named[] go to the name section */
static uint8_t *make(const N *crcs, int nc, const N *named, int nn, size_t *zn, int content_size) {
    size_t raw_n = 0x16 + (size_t)nc * 8 + (size_t)nn * 0xA4;
    uint8_t *raw = calloc(raw_n, 1);
    memcpy(raw, "RESTBL", 6), w32(raw + 6, 1), w32(raw + 10, 0xA0), w32(raw + 14, (uint32_t)nc), w32(raw + 18, (uint32_t)nn);
    E *e = malloc(sizeof(E) * (size_t)(nc + 1));
    for (int i = 0; i < nc; i++) e[i].crc = restbl_crc32(crcs[i].name), e[i].size = crcs[i].size;
    qsort(e, (size_t)nc, sizeof *e, ecmp);
    for (int i = 0; i < nc; i++) w32(raw + 0x16 + i * 8, e[i].crc), w32(raw + 0x1A + i * 8, e[i].size);
    for (int i = 0; i < nn; i++) {
        uint8_t *p = raw + 0x16 + nc * 8 + i * 0xA4;
        strcpy((char *)p, named[i].name), w32(p + 0xA0, named[i].size);
    }
    size_t cap = ZSTD_compressBound(raw_n);
    uint8_t *z = malloc(cap);
    if (content_size) *zn = ZSTD_compress(z, cap, raw, raw_n, 3);
    else {   /* a frame without its content size */
        ZSTD_CCtx *c = ZSTD_createCCtx();
        ZSTD_CCtx_setParameter(c, ZSTD_c_contentSizeFlag, 0);
        *zn = ZSTD_compress2(c, z, cap, raw, raw_n);
        ZSTD_freeCCtx(c);
    }
    free(raw), free(e);
    return z;
}
/* the merged table, decoded: the size for a name (crc section or name section), 0 if absent */
static uint8_t *dec;
static size_t dec_n;
static void decode(const uint8_t *z, size_t n) {
    free(dec);
    dec_n = (size_t)ZSTD_getFrameContentSize(z, n);
    dec = malloc(dec_n);
    CHECK(ZSTD_decompress(dec, dec_n, z, n) == dec_n);
}
static uint32_t lookup(const char *name) {
    uint32_t nc = r32(dec + 14), nn = r32(dec + 18);
    for (uint32_t i = 0; i < nn; i++)
        if (!strcmp((const char *)dec + 0x16 + nc * 8 + i * 0xA4, name)) return r32(dec + 0x16 + nc * 8 + i * 0xA4 + 0xA0);
    uint32_t c = restbl_crc32(name);
    for (uint32_t i = 0; i < nc; i++) if (r32(dec + 0x16 + i * 8) == c) return r32(dec + 0x1A + i * 8);
    return 0;
}
static int sorted(void) {
    uint32_t nc = r32(dec + 14);
    for (uint32_t i = 1; i < nc; i++) if (r32(dec + 0x16 + (i - 1) * 8) >= r32(dec + 0x16 + i * 8)) return 0;
    return dec_n == 0x16 + nc * 8 + r32(dec + 18) * 0xA4ull;
}

int main(void) {
    /* crc32 is zlib's */
    CHECK(restbl_crc32("") == 0 && restbl_crc32("123456789") == 0xCBF43926u);

    /* decoded sizes */
    uint8_t src[5000], z[6000];
    memset(src, 'a', sizeof src);
    size_t zn = ZSTD_compress(z, sizeof z, src, sizeof src, 3);
    CHECK(restbl_decoded_size("x.bfres.zs", z, zn < 32 ? zn : 32, zn) == 5000);
    CHECK(restbl_decoded_size("x.bars", (const uint8_t *)"BARS....", 8, 1234) == 1234);
    uint8_t mc[12] = {'M', 'C', 'P', 'K', 0, 0, 0, 0};
    uint32_t flags = (3u << 5) | 10;   /* 3 << 10 = 3072 */
    w32(mc + 8, flags);
    CHECK(restbl_decoded_size("x.bfres.mc", mc, 12, 100) == 3072);
    CHECK(restbl_decoded_size("x.bfres.mc", (const uint8_t *)"????????????", 12, 100) == 400);   /* unknown: 4x */
    CHECK(restbl_estimate("a.bgyml", 100) == (100 + 2048) * 8);
    CHECK(restbl_estimate("a.ainb", 1000) == 1280 + 0x1000);
    CHECK(restbl_estimate("a.bfres", 1000) == 1024 * 2);
    CHECK(restbl_estimate("a.bntx", 1000) == 1024 + 0x1000);

    /* the game's table */
    N game[] = {{"Actor/Link.bfres", 100000}, {"UI/Common.blarc", 50000}, {"Pack/Npc.pack", 30000}, {"AI/Brain.ainb", 8000}};
    N gnamed[] = {{"Collide/A.bphsh", 7000}, {"Collide/B.bphsh", 7100}};   /* names whose crc32 is shared */
    size_t gn;
    uint8_t *g = make(game, 4, gnamed, 2, &gn, 1);
    uint8_t *out = NULL;
    size_t on = 0;
    RestblStats st;

    /* nothing to change: 1, no output */
    RestblFile same[] = {{"UI/Common.blarc", 40000, 40000}, {"Actor/Link.bfres", 1000, 2000}};
    CHECK(restbl_merge(g, gn, NULL, NULL, 0, same, 2, &out, &on, &st) == 1 && !out);

    /* mod files: grown, new, a game file without an entry, a name-section file */
    RestblFile files[] = {
        {"Actor/Link.bfres", 60000, 50000},     /* + estimate(60000) - estimate(50000) */
        {"UI/Common.blarc", 40000, 41000},      /* smaller: unchanged */
        {"UI/New/Thing.bntx", 3000, 0},         /* new: 3008 + 0x1000 */
        {"Sound/NoEntry.bars", 9000, 100},      /* the game has it, with no entry: left alone */
        {"Collide/B.bphsh", 2000, 1000},        /* raised on the name entry */
        {"AI/New.ainb", 100, 0},                /* new: align(125) + 0x1000 */
    };
#define GROW(name, a, b) ((uint32_t)(restbl_estimate(name, a) - restbl_estimate(name, b)))
    CHECK(GROW("Actor/Link.bfres", 60000, 50000) == 120000 - 100032 && GROW("x.bphsh", 2000, 1000) == 992);
    CHECK(restbl_merge(g, gn, NULL, NULL, 0, files, 6, &out, &on, &st) == 0 && out);
    decode(out, on);
    CHECK(sorted());
    CHECK(lookup("Actor/Link.bfres") == 100000 + GROW("Actor/Link.bfres", 60000, 50000));
    CHECK(lookup("UI/Common.blarc") == 50000 && lookup("Pack/Npc.pack") == 30000);
    CHECK(lookup("UI/New/Thing.bntx") == 3008 + 0x1000);
    CHECK(lookup("AI/New.ainb") == 128 + 0x1000);
    CHECK(lookup("Sound/NoEntry.bars") == 0);
    CHECK(lookup("Collide/B.bphsh") == 7100 + 992 && lookup("Collide/A.bphsh") == 7000);
    CHECK(st.raised == 2 && st.added == 2 && st.no_entry == 1 && st.entries == 8);
    free(out);

    /* mods' tables: max per entry, entries the game lacks added, a name entry added, a broken table skipped */
    N t1[] = {{"Actor/Link.bfres", 150000}, {"UI/Common.blarc", 10}, {"Mod/Only.bfres", 4444}};
    N t1n[] = {{"Collide/C.bphsh", 999}};
    N t2[] = {{"Actor/Link.bfres", 120000}, {"Pack/Npc.pack", 35000}};
    size_t n1, n2;
    uint8_t *m1 = make(t1, 3, t1n, 1, &n1, 1), *m2 = make(t2, 2, NULL, 0, &n2, 0);   /* m2: no content size */
    uint8_t junk[] = {0x28, 0xB5, 0x2F, 0xFD, 1, 2, 3};
    const uint8_t *tabs[] = {m1, junk, m2};
    size_t tabn[] = {n1, sizeof junk, n2};
    RestblFile grow[] = {{"Actor/Link.bfres", 60000, 50000}, {"Mod/Only.bfres", 100, 0}};
    CHECK(restbl_merge(g, gn, tabs, tabn, 3, grow, 2, &out, &on, &st) == 0 && out);
    decode(out, on);
    CHECK(sorted());
    CHECK(st.tables == 2 && st.tables_bad == 1);
    CHECK(lookup("Actor/Link.bfres") == 150000 + GROW("Actor/Link.bfres", 60000, 50000));   /* a mod table's value, plus this run's growth */
    CHECK(lookup("UI/Common.blarc") == 50000);              /* never lowered */
    CHECK(lookup("Pack/Npc.pack") == 35000);
    CHECK(lookup("Mod/Only.bfres") == 4444);                /* the mod table's (bigger than the estimate 256) */
    CHECK(lookup("Collide/C.bphsh") == 999 && lookup("Collide/A.bphsh") == 7000);
    free(out);

    /* the game's table unreadable: < 0 (the game gets its own file) */
    CHECK(restbl_merge(junk, sizeof junk, NULL, NULL, 0, files, 6, &out, &on, &st) < 0 && !out);
    uint8_t notrestbl[64];
    size_t nz = ZSTD_compress(notrestbl, sizeof notrestbl, "HELLO WORLD, NOT A TABLE", 24, 3);
    CHECK(restbl_merge(notrestbl, nz, NULL, NULL, 0, files, 6, &out, &on, &st) < 0);

    /* a big table: 200,000 entries and 20,000 mod files */
    int big = 200000;
    N *bg = malloc(sizeof(N) * (size_t)big);
    char (*names)[48] = malloc(48 * (size_t)big);
    for (int i = 0; i < big; i++) snprintf(names[i], 48, "Model/D%03d/F%05d.bfres", i % 1000, i), bg[i].name = names[i], bg[i].size = 10000;
    size_t bn;
    uint8_t *bt = make(bg, big, NULL, 0, &bn, 1);
    RestblFile *bf = malloc(sizeof(RestblFile) * 20000);
    char (*nn)[48] = malloc(48 * 20000);
    for (int i = 0; i < 20000; i++) {
        if (i % 2) bf[i].name = names[i * 7], bf[i].mod_decoded = 9000, bf[i].game_decoded = 8000;
        else snprintf(nn[i], 48, "New/F%05d.bntx", i), bf[i].name = nn[i], bf[i].mod_decoded = 5000, bf[i].game_decoded = 0;
    }
    CHECK(restbl_merge(bt, bn, NULL, NULL, 0, bf, 20000, &out, &on, &st) == 0);
    decode(out, on);
    CHECK(sorted() && st.raised == 10000 && st.added == 10000 && st.entries == (uint32_t)big + 10000);
    CHECK(lookup(names[7]) == 10000 + GROW("a.bfres", 9000, 8000) && lookup(names[0]) == 10000 && lookup("New/F00000.bntx") == 5024 + 0x1000);
    free(out), free(bg), free(names), free(bt), free(bf), free(nn), free(m1), free(m2), free(g), free(dec);
    printf(fails ? "restbl: %d FAILED\n" : "restbl: all ok\n", fails);
    return fails != 0;
}
