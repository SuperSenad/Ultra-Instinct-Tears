/* [UIT 0175] Box test of amiibo-from-file (src/host/amiibo.c): dump parsing on synthetic dumps (made here; no real
 * amiibo data, no keys) and the nn::nfp device state machine the game drives, with a fake clock and in-memory
 * application-area store.
 *   cc -O1 -Wall -pthread tools/tests/test_amiibo.c src/host/amiibo.c -o /tmp/t/amiibo && /tmp/t/amiibo */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../src/host/amiibo.h"

static int fails, checks;
#define CHECK(c) do { checks++; if (!(c)) { fails++; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static const uint8_t kSerial[7] = {0x04, 0x12, 0x34, 0x56, 0x78, 0x9A, 0xBC};
static const uint8_t kLinkTotk[8] = {0x01, 0x00, 0x00, 0x00, 0x04, 0x18, 0x09, 0x02};   /* "Link - Tears of the Kingdom" */
static const uint8_t kUnknown[8] = {0x7F, 0x7E, 0x00, 0x00, 0x12, 0x34, 0x00, 0x02};

/* A raw (tag-layout) dump: serial with check bytes, amiibo header, ID; the "encrypted" sections are filler. */
static void make_raw(uint8_t *d, size_t n, const uint8_t id[8]) {
    memset(d, 0x5A, n);
    d[0] = kSerial[0]; d[1] = kSerial[1]; d[2] = kSerial[2]; d[3] = 0x88 ^ kSerial[0] ^ kSerial[1] ^ kSerial[2];
    d[4] = kSerial[3]; d[5] = kSerial[4]; d[6] = kSerial[5]; d[7] = kSerial[6];
    d[8] = kSerial[3] ^ kSerial[4] ^ kSerial[5] ^ kSerial[6];
    d[0xC] = 0xF1; d[0xD] = 0x10; d[0xE] = 0xFF; d[0xF] = 0xEE;
    d[0x10] = 0xA5;
    memcpy(d + 0x54, id, 8);
}
/* The same tag in the plain (decrypted, reordered) layout. */
static void make_plain(uint8_t *d, size_t n, const uint8_t id[8]) {
    uint8_t raw[540];
    make_raw(raw, sizeof raw, id);
    memset(d, 0x33, n);
    memcpy(d, raw + 8, 8);
    d[0x28] = 0xA5;
    memcpy(d + 0x1D4, raw, 8);
    memcpy(d + 0x1DC, raw + 0x54, 0x2C);
}

static void test_parse(void) {
    uint8_t d[600]; UitAmiibo a;
    static const size_t sizes[3] = {532, 540, 572};
    for (int i = 0; i < 3; i++) {
        make_raw(d, sizes[i], kLinkTotk);
        CHECK(uit_amiibo_parse(d, sizes[i], "x/link.bin", &a) == UIT_AMIIBO_OK);
        CHECK(a.uid_len == 7 && !memcmp(a.uid, kSerial, 7));
        CHECK(!memcmp(a.id, kLinkTotk, 8));
        CHECK(!strcmp(a.name, "Link - Tears of the Kingdom"));
        make_plain(d, sizes[i], kLinkTotk);
        memset(&a, 0, sizeof a);
        CHECK(uit_amiibo_parse(d, sizes[i], "link.bin", &a) == UIT_AMIIBO_OK);
        CHECK(a.uid_len == 7 && !memcmp(a.uid, kSerial, 7) && !memcmp(a.id, kLinkTotk, 8));
    }
    char key[64];
    make_raw(d, 540, kLinkTotk);
    uit_amiibo_parse(d, 540, NULL, &a);
    uit_amiibo_key(&a, key, sizeof key);
    CHECK(!strcmp(key, "04123456789abc-0100000004180902"));

    /* unknown ID: file name (no folder, no extension) plus the ID */
    make_raw(d, 540, kUnknown);
    CHECK(uit_amiibo_parse(d, 540, "C:\\amiibo\\My Figure.bin", &a) == UIT_AMIIBO_OK);
    CHECK(!strcmp(a.name, "My Figure (7f7e000012340002)"));

    /* errors */
    CHECK(uit_amiibo_parse(d, 0, "a", &a) == UIT_AMIIBO_EMPTY);
    CHECK(uit_amiibo_parse(d, 539, "a", &a) == UIT_AMIIBO_SIZE);
    CHECK(uit_amiibo_parse(d, 1024, "a", &a) == UIT_AMIIBO_SIZE);
    make_raw(d, 540, kLinkTotk); d[0x10] = 0;
    CHECK(uit_amiibo_parse(d, 540, "a", &a) == UIT_AMIIBO_NOT_AMIIBO);
    make_raw(d, 540, kLinkTotk); d[3] ^= 1;   /* serial check byte */
    CHECK(uit_amiibo_parse(d, 540, "a", &a) == UIT_AMIIBO_NOT_AMIIBO);
    make_raw(d, 540, kLinkTotk); d[0x5B] = 0;   /* amiibo version */
    CHECK(uit_amiibo_parse(d, 540, "a", &a) == UIT_AMIIBO_NOT_AMIIBO);
    memset(d, 0, 540);
    CHECK(uit_amiibo_parse(d, 540, "a", &a) == UIT_AMIIBO_NOT_AMIIBO);
    CHECK(strstr(uit_amiibo_error_text(UIT_AMIIBO_SIZE), "540"));

    /* virtual amiibo JSON */
    const char *j1 = "\xEF\xBB\xBF {\n \"FileVersion\": 0,\n \"Name\": \"Caf\\u00e9 \\\"L\\\"\",\n"
                     " \"TagUuid\": \"BBI0VniavA==\",\n \"AmiiboId\": \"7f7e000012340002\",\n \"ApplicationAreas\": []\n}";
    CHECK(uit_amiibo_parse((const uint8_t *)j1, strlen(j1), "v.json", &a) == UIT_AMIIBO_OK);
    CHECK(a.uid_len == 7 && !memcmp(a.uid, kSerial, 7) && !memcmp(a.id, kUnknown, 8));
    CHECK(!strcmp(a.name, "Caf\xC3\xA9 \"L\""));
    const char *j2 = "{\"AmiiboId\":\"0x0100000004180902\"}";   /* no serial: derived, stable */
    UitAmiibo b;
    CHECK(uit_amiibo_parse((const uint8_t *)j2, strlen(j2), "v.json", &a) == UIT_AMIIBO_OK);
    CHECK(uit_amiibo_parse((const uint8_t *)j2, strlen(j2), "w.json", &b) == UIT_AMIIBO_OK);
    CHECK(a.uid_len == 7 && a.uid[0] == 0x04 && !memcmp(a.uid, b.uid, 7));
    CHECK(!strcmp(a.name, "Link - Tears of the Kingdom"));
    const char *j3 = "{\"Name\":\"x\",\"AmiiboId\":\"12\"}";
    CHECK(uit_amiibo_parse((const uint8_t *)j3, strlen(j3), "v.json", &a) == UIT_AMIIBO_BAD_JSON);
    const char *j4 = "{\"Name\":\"x\"}";
    CHECK(uit_amiibo_parse((const uint8_t *)j4, strlen(j4), "v.json", &a) == UIT_AMIIBO_BAD_JSON);

    /* the name table is sorted (binary search) */
    CHECK(uit_amiibo_known_name(kUnknown) == NULL);
    static const uint8_t sonic[8] = {0x32, 0x00, 0x00, 0x00, 0x00, 0x30, 0x00, 0x02};
    CHECK(uit_amiibo_known_name(sonic) && !strcmp(uit_amiibo_known_name(sonic), "Sonic"));
}

/* ---- nfp ---- */
static uint64_t fake_now;
static uint64_t now(void) { return fake_now; }
static struct { char key[64]; uint8_t data[0x100]; int n; uint32_t acc; int saves; } mem;
static int load(const char *key, uint8_t *buf, size_t cap, uint32_t *acc) {
    if (!mem.n || strcmp(key, mem.key)) return -1;
    memcpy(buf, mem.data, (size_t)mem.n < cap ? (size_t)mem.n : cap);
    *acc = mem.acc;
    return mem.n;
}
static int save(const char *key, const uint8_t *buf, size_t n, uint32_t acc) {
    snprintf(mem.key, sizeof mem.key, "%s", key);
    memcpy(mem.data, buf, n); mem.n = (int)n; mem.acc = acc; mem.saves++;
    return 0;
}

static void test_nfp(void) {
    static const UitNfpStore st = {load, save};
    uit_nfp_setup(now, &st);
    uit_nfp_reset();
    fake_now = 1000;
    const uint64_t H = UIT_NFP_HANDLE;
    uint64_t hs[4] = {0}; int cnt = -1;
    uint8_t buf[0x100];
    UitAmiibo a;
    uint8_t d[540];
    make_raw(d, 540, kLinkTotk);
    CHECK(uit_amiibo_parse(d, 540, "link.bin", &a) == 0);

    /* before Initialize */
    CHECK(uit_nfp_state() == 0);
    CHECK(uit_nfp_list(hs, &cnt, 4) == UIT_NFP_INVALID_STATE && cnt == 0);
    CHECK(uit_nfp_start(H) == UIT_NFP_INVALID_STATE);

    /* the game's flow: Initialize, ListDevices, StartDetection, wait activate, Mount, GetTagInfo, GetModelInfo */
    CHECK(uit_nfp_initialize() == 0 && uit_nfp_state() == 1);
    CHECK(uit_nfp_list(hs, &cnt, 4) == 0 && cnt == 1 && hs[0] == H);
    CHECK(uit_nfp_list(hs, &cnt, 0) == UIT_NFP_INVALID_ARGUMENT);
    CHECK(uit_nfp_device_state(H) == UIT_NFP_DS_INIT);
    CHECK(uit_nfp_start(H + 1) == UIT_NFP_DEVICE_NOT_FOUND);
    CHECK(uit_nfp_start(H) == 0 && uit_nfp_device_state(H) == UIT_NFP_DS_SEARCHING);
    CHECK(uit_nfp_mount(H, 0, 1) == UIT_NFP_INVALID_STATE);
    CHECK(uit_nfp_tag_info(H, buf) == UIT_NFP_INVALID_STATE);
    CHECK(uit_nfp_take_signals() == 0);
    CHECK(uit_nfp_scan_result() == 0);

    CHECK(uit_nfp_present(&a) == 1);   /* game was searching: seen at once */
    CHECK(uit_nfp_take_signals() == UIT_NFP_SIG_ACTIVATE);
    CHECK(uit_nfp_scan_result() == 2);
    CHECK(uit_nfp_device_state(H) == UIT_NFP_DS_FOUND);
    CHECK(uit_nfp_tag_info(H, buf) == 0);
    CHECK(!memcmp(buf, kSerial, 7) && buf[0xA] == 7 && buf[0x20] == 1 && buf[0x24] == 2);
    CHECK(uit_nfp_model_info(H, buf) == UIT_NFP_NOT_MOUNTED);
    CHECK(uit_nfp_mount(H, 1, 1) == UIT_NFP_INVALID_ARGUMENT);
    CHECK(uit_nfp_mount(H, 0, 1) == 0 && uit_nfp_device_state(H) == UIT_NFP_DS_MOUNTED);
    CHECK(uit_nfp_scan_result() == 4);
    CHECK(uit_nfp_model_info(H, buf) == 0);
    CHECK(buf[0] == 0x01 && buf[1] == 0x00 && buf[2] == 0x00 && buf[3] == 0x00);
    CHECK(buf[4] == 0x18 && buf[5] == 0x04 && buf[6] == 0x09 && buf[7] == 0);
    CHECK(uit_nfp_common_info(H, buf) == 0 && buf[8] == 0xD8 && buf[0] == 0xE7 && buf[1] == 0x07);
    CHECK(uit_nfp_register_info(H, buf) == 0 && !strcmp((char *)buf + 0x5C, "Link - Tea"));

    /* application area: absent, created, opened, read, written, flushed, persisted across scans */
    CHECK(uit_nfp_open_area(H, 0x1234) == UIT_NFP_AREA_NOT_CREATED);
    uint64_t got = 0;
    CHECK(uit_nfp_get_area(H, buf, &got, sizeof buf) == UIT_NFP_AREA_NOT_CREATED);
    uint8_t init[16] = "hello amiibo";
    CHECK(uit_nfp_create_area(H, 0x1234, init, sizeof init) == 0 && mem.saves == 1);
    CHECK(!strcmp(mem.key, "04123456789abc-0100000004180902") && mem.n == 0xD8 && mem.acc == 0x1234);
    CHECK(uit_nfp_create_area(H, 0x1234, init, sizeof init) == UIT_NFP_AREA_EXISTS);
    CHECK(uit_nfp_open_area(H, 0x9999) == UIT_NFP_ACCESS_ID_MISMATCH);
    CHECK(uit_nfp_open_area(H, 0x1234) == 0);
    CHECK(uit_nfp_get_area(H, buf, &got, sizeof buf) == 0 && got == 0xD8 && !strcmp((char *)buf, "hello amiibo"));
    CHECK(uit_nfp_set_area(H, buf, 0xD9) == UIT_NFP_AREA_TOO_LARGE);
    CHECK(uit_nfp_set_area(H, "changed", 8) == 0);
    CHECK(!strcmp((char *)mem.data, "hello amiibo"));   /* not written until Flush */
    CHECK(uit_nfp_flush(H) == 0 && mem.saves == 2 && !strcmp((char *)mem.data, "changed"));
    CHECK(uit_nfp_unmount(H) == 0 && uit_nfp_device_state(H) == UIT_NFP_DS_FOUND);
    CHECK(uit_nfp_unmount(H) == UIT_NFP_INVALID_STATE);

    /* lifted off after UIT_NFP_PRESENT_MS */
    fake_now += UIT_NFP_PRESENT_MS - 1;
    uit_nfp_tick();
    CHECK(uit_nfp_device_state(H) == UIT_NFP_DS_FOUND && uit_nfp_take_signals() == 0);
    CHECK(uit_nfp_mount(H, 0, 3) == 0);
    fake_now += 1;
    uit_nfp_tick();
    CHECK(uit_nfp_device_state(H) == UIT_NFP_DS_REMOVED);
    CHECK(uit_nfp_take_signals() == UIT_NFP_SIG_DEACTIVATE);
    CHECK(uit_nfp_model_info(H, buf) == UIT_NFP_NEED_RESTART);
    CHECK(uit_nfp_mount(H, 0, 1) == UIT_NFP_NEED_RESTART);

    /* scanned while the game isn't searching: kept on the reader, seen shortly after it starts searching */
    CHECK(uit_nfp_stop(H) == 0 && uit_nfp_device_state(H) == UIT_NFP_DS_INIT);
    CHECK(uit_nfp_present(&a) == 2 && uit_nfp_scan_result() == 1);
    fake_now += 5000;
    uit_nfp_tick();
    CHECK(uit_nfp_device_state(H) == UIT_NFP_DS_INIT && uit_nfp_take_signals() == 0);
    CHECK(uit_nfp_start(H) == 0 && uit_nfp_device_state(H) == UIT_NFP_DS_SEARCHING);
    CHECK(uit_nfp_take_signals() == 0);
    fake_now += UIT_NFP_ARRIVE_MS;
    CHECK(uit_nfp_device_state(H) == UIT_NFP_DS_FOUND);   /* lazily ticked by the game's own call */
    CHECK(uit_nfp_take_signals() == UIT_NFP_SIG_ACTIVATE && uit_nfp_scan_result() == 2);
    CHECK(uit_nfp_mount(H, 0, 2) == 0);
    CHECK(uit_nfp_open_area(H, 0x1234) == 0);   /* the area came back from the store */
    CHECK(uit_nfp_get_area(H, buf, &got, 4) == 0 && got == 4 && !memcmp(buf, "chan", 4));
    CHECK(uit_nfp_start(H) == UIT_NFP_INVALID_STATE);   /* a figure is on the reader */

    /* the wait runs out */
    CHECK(uit_nfp_stop(H) == 0);
    uit_nfp_take_signals();
    CHECK(uit_nfp_present(&a) == 2);
    fake_now += UIT_NFP_HOLD_MS;
    uit_nfp_tick();
    CHECK(uit_nfp_scan_result() == 3);
    CHECK(uit_nfp_start(H) == 0);
    fake_now += 1000;
    uit_nfp_tick();
    CHECK(uit_nfp_device_state(H) == UIT_NFP_DS_SEARCHING && uit_nfp_take_signals() == 0);

    /* scanned before the game initialised nfp at all */
    CHECK(uit_nfp_finalize() == 0 && uit_nfp_state() == 0);
    CHECK(uit_nfp_present(&a) == 2);
    fake_now += 100;
    CHECK(uit_nfp_initialize() == 0 && uit_nfp_device_state(H) == UIT_NFP_DS_INIT);
    CHECK(uit_nfp_start(H) == 0);
    fake_now += UIT_NFP_ARRIVE_MS;
    uit_nfp_tick();
    CHECK(uit_nfp_device_state(H) == UIT_NFP_DS_FOUND && uit_nfp_take_signals() == UIT_NFP_SIG_ACTIVATE);
    CHECK(uit_nfp_finalize() == 0 && uit_nfp_device_state(H) == UIT_NFP_DS_INIT);
    CHECK(uit_nfp_tag_info(H, buf) == UIT_NFP_INVALID_STATE);
    fake_now += 10000;
    uit_nfp_tick();
    CHECK(uit_nfp_take_signals() == 0);   /* nothing to lift after Finalize */
}

/* [UIT 0180] the reader's log: every answer in words, the figure's arrival and removal, a scan that waited for nothing,
 * a polled call with the same answer once (plus a count) */
static char lg[64][200];
static int lgn;
static void sink(const char *line) { if (lgn < 64) snprintf(lg[lgn++], sizeof lg[0], "%s", line); }
static int logged(const char *a, const char *b) {
    for (int i = 0; i < lgn; i++) if (strstr(lg[i], a) && (!b || strstr(lg[i], b))) return 1;
    return 0;
}
static void test_log(void) {
    static const UitNfpStore st = {load, save};
    uit_nfp_setup(now, &st);
    uit_nfp_reset();
    uit_nfp_set_log(sink);
    lgn = 0;
    fake_now = 50000;
    const uint64_t H = UIT_NFP_HANDLE;
    uint8_t buf[0x100];
    UitAmiibo a;
    uint8_t d[540];
    make_raw(d, 540, kLinkTotk);
    CHECK(uit_amiibo_parse(d, 540, "link.bin", &a) == 0);
    CHECK(uit_nfp_start(H) == UIT_NFP_INVALID_STATE);
    CHECK(logged("StartDetection: WrongDeviceState (2115-0097)", "not initialised"));
    CHECK(uit_nfp_present(&a) == 2);
    CHECK(logged("scanned ", "the game is not searching (nfp not initialised)"));
    fake_now += UIT_NFP_HOLD_MS;
    uit_nfp_tick();
    CHECK(logged("waited 20 s and was taken off", "did not search"));
    CHECK(uit_nfp_initialize() == 0 && logged("Initialize: ok", "idle"));
    CHECK(uit_nfp_start(H) == 0 && logged("StartDetection: ok", "searching"));
    CHECK(uit_nfp_present(&a) == 1);
    CHECK(logged("scanned ", "the game was searching") && logged("on the reader: ", "amiibo ID 0100000004180902"));
    int before = lgn;
    for (int i = 0; i < 5; i++) CHECK(uit_nfp_model_info(H, buf) == UIT_NFP_NOT_MOUNTED);
    CHECK(lgn == before + 1 && logged("GetModelInfo: NotMounted (2115-0073)", "figure found"));
    CHECK(uit_nfp_mount(H, 0, 3) == 0);
    CHECK(logged("(the line before: 4 more times)", NULL));
    CHECK(logged("Mount: ", "read by the game") && logged("Mount: ok", "figure mounted"));
    CHECK(uit_nfp_tag_info(H, buf) == 0 && logged("GetTagInfo: ok", NULL));
    CHECK(uit_nfp_model_info(H, buf) == 0 && logged("GetModelInfo: ok", NULL));
    fake_now += UIT_NFP_PRESENT_MS;
    uit_nfp_tick();
    CHECK(logged("taken off the reader after 2.5 s", NULL) && !logged("never mounted", NULL));
    CHECK(uit_nfp_mount(H, 0, 3) == UIT_NFP_NEED_RESTART && logged("Mount: NeedRestart", "figure removed"));
    /* found but never mounted: said so */
    CHECK(uit_nfp_start(H) == 0 && uit_nfp_present(&a) == 1);
    fake_now += UIT_NFP_PRESENT_MS;
    uit_nfp_tick();
    CHECK(logged("taken off the reader after 2.5 s", "the game never mounted it"));
    CHECK(uit_nfp_stop(H) == 0 && logged("StopDetection: ok", "idle"));
    CHECK(!strcmp(uit_nfp_result_text(UIT_NFP_INVALID_STATE), "WrongDeviceState (2115-0097)"));
    CHECK(!strcmp(uit_nfp_state_text(UIT_NFP_DS_MOUNTED), "figure mounted"));
    uit_nfp_set_log(NULL);
    int n = lgn;
    CHECK(uit_nfp_finalize() == 0 && lgn == n);   /* off: nothing more */
}

/* [UIT 0181] a Flipper Zero .nfc dump (text pages) and the built-in figures */
static size_t make_nfc(char *t, size_t cap, const uint8_t id[8], int skip_page) {
    uint8_t raw[540];
    make_raw(raw, sizeof raw, id);
    size_t o = (size_t)snprintf(t, cap, "Filetype: Flipper NFC device\r\nVersion: 4\r\n# comment\r\nDevice type: NTAG215\r\n"
                                        "UID: 04 12 34 56 78 9A BC\r\nPages total: 135\r\n");
    for (int p = 0; p < 135 && o < cap; p++)
        if (p != skip_page)
            o += (size_t)snprintf(t + o, cap - o, "Page %d: %02X %02X %02X %02x\r\n", p, raw[4 * p], raw[4 * p + 1],
                                  raw[4 * p + 2], raw[4 * p + 3]);
    o += (size_t)snprintf(t + o, cap - o, "Failed authentication attempts: 0\r\n");
    return o;
}
static void test_nfc_and_builtin(void) {
    static char t[8192];
    UitAmiibo a;
    size_t n = make_nfc(t, sizeof t, kLinkTotk, -1);
    CHECK(uit_amiibo_parse((const uint8_t *)t, n, "link.nfc", &a) == UIT_AMIIBO_OK);
    CHECK(a.uid_len == 7 && !memcmp(a.uid, kSerial, 7) && !memcmp(a.id, kLinkTotk, 8));
    CHECK(!strcmp(a.name, "Link - Tears of the Kingdom"));
    n = make_nfc(t, sizeof t, kLinkTotk, 21);   /* the ID's page missing */
    CHECK(uit_amiibo_parse((const uint8_t *)t, n, "link.nfc", &a) == UIT_AMIIBO_NOT_AMIIBO);
    n = make_nfc(t, sizeof t, kLinkTotk, -1);
    char *p4 = strstr(t, "Page 4: A5");
    CHECK(p4 != NULL);
    if (p4) p4[8] = '0';   /* no amiibo header byte */
    CHECK(uit_amiibo_parse((const uint8_t *)t, n, "link.nfc", &a) == UIT_AMIIBO_NOT_AMIIBO);
    CHECK(uit_amiibo_parse((const uint8_t *)"Filetype: something else\n", 25, "x.nfc", &a) == UIT_AMIIBO_SIZE);
    CHECK(strstr(uit_amiibo_error_text(UIT_AMIIBO_SIZE), ".nfc"));

    /* the built-in figures: the Zelda series only, every one a valid amiibo with a fixed serial, names all different */
    int nb = uit_amiibo_builtin_count();
    CHECK(nb >= 30 && nb < 64);
    char names[64][64];
    int have_link_totk = 0, have_smash = 0;
    for (int i = 0; i < nb && i < 64; i++) {
        UitAmiibo b, c;
        CHECK(uit_amiibo_builtin(i, &b) == 0 && uit_amiibo_builtin(i, &c) == 0 && !memcmp(&b, &c, sizeof b));
        CHECK(b.id[0] == 0x01 && b.id[1] < 0x80 && (b.id[7] == 0x02 || b.id[7] == 0x03));
        CHECK(b.uid_len == 7 && b.uid[0] == 0x04 && b.name[0]);
        CHECK(uit_amiibo_known_name(b.id) != NULL);
        snprintf(names[i], sizeof names[i], "%s", b.name);
        for (int k = 0; k < i; k++) CHECK(strcmp(names[k], b.name));
        if (!memcmp(b.id, kLinkTotk, 8)) have_link_totk = 1;
        if (strstr(b.name, "(Smash Bros.)")) have_smash = 1;
    }
    CHECK(have_link_totk && have_smash);
    CHECK(uit_amiibo_builtin(nb, &a) == -1 && uit_amiibo_builtin(-1, &a) == -1 && a.name[0] == 0);
    /* and the game reads one like any other: searching, scanned, mounted, its tag info */
    const uint64_t H = UIT_NFP_HANDLE;
    UitAmiibo b;
    CHECK(uit_amiibo_builtin(0, &b) == 0);
    uit_nfp_reset();
    CHECK(uit_nfp_initialize() == 0 && uit_nfp_start(H) == 0);
    CHECK(uit_nfp_present(&b) == 1 && uit_nfp_device_state(H) == UIT_NFP_DS_FOUND);
    CHECK(uit_nfp_mount(H, 0, 1) == 0);
    uint8_t ti[0x58];
    memset(ti, 0, sizeof ti);
    CHECK(uit_nfp_tag_info(H, ti) == 0 && ti[10] == 7 && !memcmp(ti, b.uid, 7));
    uint8_t mi[0x40];
    CHECK(uit_nfp_model_info(H, mi) == 0 && !memcmp(mi, b.id, 3));
    uit_nfp_reset();
}

/* [UIT 0208] the time on the reader is settable and clamped; the reader lifts the figure off at that time */
static void test_present_ms(void) {
    static const UitNfpStore st = {load, save};
    const uint64_t H = UIT_NFP_HANDLE;
    CHECK(uit_nfp_present_ms() == UIT_NFP_PRESENT_MS && UIT_NFP_PRESENT_MS == 2500);
    CHECK(uit_nfp_set_present_ms(0) == UIT_NFP_PRESENT_MIN_MS && uit_nfp_set_present_ms(-5) == 1000);
    CHECK(uit_nfp_set_present_ms(999999) == UIT_NFP_PRESENT_MAX_MS && uit_nfp_present_ms() == 15000);
    CHECK(uit_nfp_set_present_ms(6000) == 6000);
    UitAmiibo b;
    CHECK(uit_amiibo_builtin(0, &b) == 0);
    uit_nfp_setup(now, &st);
    uit_nfp_reset();
    fake_now = 50000;
    CHECK(uit_nfp_initialize() == 0 && uit_nfp_start(H) == 0);
    CHECK(uit_nfp_present(&b) == 1 && uit_nfp_device_state(H) == UIT_NFP_DS_FOUND);
    fake_now += 5999;
    uit_nfp_tick();
    CHECK(uit_nfp_device_state(H) == UIT_NFP_DS_FOUND);   /* the old 2.5 s would have lifted it */
    CHECK(uit_nfp_mount(H, 0, 1) == 0);
    fake_now += 1;
    uit_nfp_tick();
    CHECK(uit_nfp_device_state(H) == UIT_NFP_DS_REMOVED && (uit_nfp_take_signals() & UIT_NFP_SIG_DEACTIVATE));
    uit_nfp_set_present_ms(UIT_NFP_PRESENT_MS);
    uit_nfp_reset();
}

/* [UIT 0210, 0211] Limitless amiibo scanning at the reader: each scan a new serial number (NTAG215-shaped), the
 * amiibo ID unchanged; without it the file's serial, and a serial seen before (same or another amiibo) is said */
static int fresh_on;
static int want_fresh(void) { return fresh_on; }
static void scan_once(const UitAmiibo *a, uint8_t ti[0x58]) {
    const uint64_t H = UIT_NFP_HANDLE;
    CHECK(uit_nfp_start(H) == 0 && uit_nfp_present(a) == 1);
    CHECK(uit_nfp_mount(H, 0, 1) == 0);
    memset(ti, 0, 0x58);
    CHECK(uit_nfp_tag_info(H, ti) == 0);
    char ser[24], want[24];
    uit_nfp_tag_serial(ser, sizeof ser);
    for (int i = 0; i < 7; i++) snprintf(want + 2 * i, 3, "%02x", ti[i]);
    CHECK(!strcmp(ser, want));
    uint8_t mi[0x40];
    CHECK(uit_nfp_model_info(H, mi) == 0 && !memcmp(mi, a->id, 3));
    fake_now += UIT_NFP_PRESENT_MS;
    uit_nfp_tick();
    CHECK(uit_nfp_stop(H) == 0);
}
static void test_fresh_serial(void) {
    static const UitNfpStore st = {load, save};
    uit_nfp_setup(now, &st);
    uit_nfp_reset();
    uit_nfp_set_log(sink);
    lgn = 0; fake_now = 90000;
    UitAmiibo a, b; uint8_t d[540], t1[0x58], t2[0x58];
    make_raw(d, 540, kLinkTotk);
    CHECK(uit_amiibo_parse(d, 540, "link.bin", &a) == 0);
    make_raw(d, 540, kUnknown);   /* another amiibo with the same serial (a pack made with one serial) */
    CHECK(uit_amiibo_parse(d, 540, "other.bin", &b) == 0);
    CHECK(uit_nfp_initialize() == 0);
    uit_nfp_set_fresh_serial(want_fresh, 12345);
    fresh_on = 0;
    scan_once(&a, t1);
    CHECK(t1[10] == 7 && !memcmp(t1, kSerial, 7));
    CHECK(logged("on the reader: ", "serial number 04123456789abc") && logged("Mount asked: model type 0, target 1", "ROM"));
    CHECK(!logged("already scanned this session", NULL) && !logged("new serial number", NULL));
    scan_once(&a, t1);
    CHECK(logged("serial number 04123456789abc: this figure was already scanned this session", NULL));
    scan_once(&b, t1);
    CHECK(logged("with another amiibo (ID 0100000004180902)", "same figure"));
    fresh_on = 1;
    lgn = 0;
    scan_once(&a, t1);
    scan_once(&a, t2);
    CHECK(t1[10] == 7 && t2[10] == 7 && t1[0] == 0x04 && t2[0] == 0x04 && t1[3] != 0x88 && t2[3] != 0x88);
    CHECK(memcmp(t1, kSerial, 7) && memcmp(t2, kSerial, 7) && memcmp(t1, t2, 7));
    CHECK(logged("Limitless amiibo scanning: the game reads it with a new serial number", NULL));
    CHECK(!logged("already scanned this session", NULL));
    int diff = 0, zero3 = 0;   /* many scans: all different, never the cascade tag */
    uint8_t prev[7]; memcpy(prev, t2, 7);
    for (int i = 0; i < 200; i++) {
        uit_nfp_set_log(NULL);
        scan_once(&a, t1);
        diff += memcmp(t1, prev, 7) != 0; zero3 += t1[3] == 0x88; memcpy(prev, t1, 7);
    }
    CHECK(diff == 200 && zero3 == 0);
    uit_nfp_set_fresh_serial(NULL, 0);
    char ser[24];
    uit_nfp_tag_serial(ser, sizeof ser);
    CHECK(ser[0] == 0);   /* nothing on the reader */
    uit_nfp_reset();
}

int main(void) {
    test_parse();
    test_present_ms();
    test_nfp();
    test_log();
    test_nfc_and_builtin();
    test_fresh_serial();
    printf("test_amiibo: %d checks, %d failed\n", checks, fails);
    return fails != 0;
}
