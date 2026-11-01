/* [UIT 0065] Box test of romfix inside src/host/fs.c: a made-up RomFS folder (no game data) with a Common layout archive,
 * the dictionary pack and a HeapDef, read through the game's own nn::fs entry points. Each case runs in its own process
 * (the settings are read once per run, as in the game):
 *   default             the quick-menu fix is served (exactly the edit, the game's dictionary, any offset readable)
 *   TOTK_MENU_BG_FIX=0  the game's archive byte for byte; the HeapDef untouched without an output resolution
 *   a mod's archive     already fixed: the mod's bytes as they are; not fixed: the fix on top of the mod's file
 *   no dictionary pack  the usual file (no crash, no half edit)
 *   TOTK_OUTPUT_RES     the HeapDef grown; a HeapDef with other sizes is refused at the mount (before any game code
 *                       reads the canvas size) and the canvas stays 1080p
 *   zcc -O1 -Isrc -Isrc/host -I<zstd.h> tools/tests/test_fs_romfix.c src/host/modfs.c src/host/restbl.c src/host/romfix.c \
 *       src/host/uit_display.c src/host/save_txn.c -lzstd -lm -lpthread -o /tmp/t/fsromfix && /tmp/t/fsromfix */
#include <fcntl.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#define O_BINARY 0
#define _chsize_s(fd, size) ftruncate(fd, size)
#include "../../src/host/fs.c"
#include "../../src/host/uit_display.h"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)
#include "romfix_fixtures.h"

static char base[300];
_Noreturn void runtime_fail(const char *m) { fprintf(stderr, "runtime_fail: %s\n", m); exit(3); }
uint64_t runtime_now_ns(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec; }
void runtime_account_uid(uint64_t out[2]) { out[0] = 1, out[1] = 2; }
const char *runtime_profile_dir(void) { return base; }
void hle_native_thunk(Cpu *c) { (void)c; }
void plat_commit(uint64_t a, uint64_t s) { (void)a, (void)s; }
int plat_thread_ecores(void) { return 0; }   // [UIT 0089] platform stubs (TOTK_HYBRID_PLACEMENT, Windows only)
int plat_thread_game(void) { return 0; }
const int main_output_res_patch = 1, main_aspect_patch = 1;
const int main_shadow_patch = 1;   /* [UIT 0070] the confirmed shadow-size site (TOTK_SHADOW_RES) */

static void wfile(const char *rel, const void *d, size_t n) {
    char p[1024], cmd[1200];
    snprintf(p, sizeof p, "%s/%s", base, rel);
    snprintf(cmd, sizeof cmd, "mkdir -p \"$(dirname '%s')\"", p);
    if (system(cmd)) exit(2);
    FILE *f = fopen(p, "wb");
    if (!f || fwrite(d, 1, n, f) != n) exit(2);
    fclose(f);
}
static void rmrf(const char *rel) {
    char cmd[1200];
    snprintf(cmd, sizeof cmd, "rm -rf '%s/%s'", base, rel);
    if (system(cmd)) exit(2);
}
/* the whole file through nn::fs (NULL: cannot be opened); reads in odd pieces too and checks they agree */
static uint8_t *game_read(const char *path, size_t *n) {
    uint64_t h = 0, got = 0;
    int64_t size = -1;
    if (open_file(&h, path, OPEN_READ)) return NULL;
    CHECK(file_size(&size, h) == 0 && size > 0);
    uint8_t *d = malloc((size_t)size + 64), *e = malloc((size_t)size + 64);
    CHECK(read_file(&got, h, 0, d, (uint64_t)size + 64) == 0 && (int64_t)got == size);
    for (int64_t at = 0; at < size;) {   /* 777-byte pieces from odd offsets */
        uint64_t g = 0;
        CHECK(read_file(&g, h, at, e + at, 777) == 0 && g > 0);
        if (!g) break;
        at += (int64_t)g;
    }
    CHECK(!memcmp(d, e, (size_t)size));
    free(e);
    close_file(h);
    *n = (size_t)size;
    return d;
}
static Buf game_common, game_heap;   /* the files as written to the RomFS folder (compressed) */

/* one run: the settings in env ("K=V;K=V"), then check() in a fresh process */
typedef void (*Check)(void);
static int run(const char *name, const char *env, Check check) {
    fflush(stdout);
    pid_t pid = fork();
    if (!pid) {
        char lp[600], rom[400], md[400], e[400];
        snprintf(lp, sizeof lp, "%s/log-%s.txt", base, name);
        if (!freopen(lp, "w", stderr)) _exit(2);
        snprintf(rom, sizeof rom, "%s/romfs", base), snprintf(md, sizeof md, "%s/mods", base);
        setenv("TOTK_ROMFS", rom, 1), setenv("TOTK_MODS_DIR", md, 1);
        snprintf(e, sizeof e, "%s", env);
        for (char *t = strtok(e, ";"); t; t = strtok(NULL, ";")) { char *q = strchr(t, '='); if (q) *q = 0, setenv(t, q + 1, 1); }
        if (mount_rom("content", NULL, 0)) _exit(3);
        check();
        fflush(stdout);
        fflush(stderr);
        _exit(fails ? 1 : 0);
    }
    int st = 0;
    waitpid(pid, &st, 0);
    int ok = WIFEXITED(st) && WEXITSTATUS(st) == 0;
    if (!ok) printf("FAIL case %s (exit %d)\n", name, WIFEXITED(st) ? WEXITSTATUS(st) : -1), fails++;
    return ok;
}
#define C_PATH "content:/" ROMFIX_COMMON
#define H_PATH "content:/" ROMFIX_HEAPDEF

/* the archive the game reads: decompressed with the game's dictionary; the capture key frame */
static float served_key(int *same_as_game) {
    size_t n = 0, dn = 0;
    uint8_t *z = game_read(C_PATH, &n);
    CHECK(z != NULL);
    if (!z) return -2;
    *same_as_game = n == game_common.n && !memcmp(z, game_common.d, n);
    CHECK(ZSTD_getDictID_fromFrame(z, n) == ZSTD_getDictID_fromDict(dict_buf, dict_n));
    uint8_t *d = unzs(z, n, &dn);
    CHECK(!ZSTD_isError(dn) && dn == ZSTD_getFrameContentSize(z, n));
    RomfixMember *m = NULL;
    int nm = romfix_sarc(d, dn, &m);
    CHECK(nm > 0);
    float k = nm > 0 ? key_of(d, find(m, nm, "anim/ShortCutBG_00_In.bflan"), "C_CaptureBG_00") : -2;
    float o = nm > 0 ? key_of(d, find(m, nm, "anim/ShortCutBG_00_In.bflan"), "N_Other_00") : -2;
    CHECK(o == 1.0f);   /* a pane the fix does not touch */
    /* exactly the fix: the game's archive with the fix applied by hand is the same bytes */
    size_t gn = 0;
    uint8_t *g = unzs(game_common.d, game_common.n, &gn);
    RomfixMember *gm = NULL;
    int gnm = romfix_sarc(g, gn, &gm);
    if (k == 0.0f && key_of(g, find(gm, gnm, "anim/ShortCutBG_00_In.bflan"), "C_CaptureBG_00") == 1.0f) {
        CHECK(romfix_menu_bg(g, gn, gm, gnm) == 1);
        CHECK(gn == dn && !memcmp(g, d, dn));
    }
    free(gm), free(g), free(m), free(d), free(z);
    return k;
}
static void c_default(void) {
    int same = 1;
    CHECK(served_key(&same) == 0.0f && !same);
    CHECK(served_key(&same) == 0.0f);                 /* a second open: the same */
    size_t n = 0;
    uint8_t *h = game_read(H_PATH, &n);               /* no output resolution: the HeapDef as shipped */
    CHECK(h && n == game_heap.n && !memcmp(h, game_heap.d, n));
    free(h);
    unsigned w = 0, hh = 0;
    CHECK(uit_output_res(&w, &hh) == 0);
}
static void c_off(void) {
    int same = 0;
    CHECK(served_key(&same) == 1.0f && same);
}
static void c_mod_fixed(void) {   /* the mod's archive (already captures at 0): served as the mod's file */
    size_t n = 0;
    uint8_t *z = game_read(C_PATH, &n);
    char p[600];
    snprintf(p, sizeof p, "%s/mods/Fix/romfs/%s", base, ROMFIX_COMMON);
    size_t mn = 0;
    uint8_t *mod = host_slurp(p, 1u << 30, &mn);
    CHECK(z && mod && n == mn && !memcmp(z, mod, n));
    free(z), free(mod);
}
static void c_mod_unfixed(void) { int same = 1; CHECK(served_key(&same) == 0.0f); }
static void c_no_dict(void) { int same = 0; size_t n = 0; uint8_t *z = game_read(C_PATH, &n); CHECK(z && n == game_common.n && !memcmp(z, game_common.d, n)); free(z); (void)same; }
static void c_output(void) {
    size_t n = 0, dn = 0;
    uint8_t *z = game_read(H_PATH, &n);
    CHECK(z && ZSTD_getDictID_fromFrame(z, n) == 0);
    uint8_t *d = z ? unzs(z, n, &dn) : NULL;
    CHECK(d && heap_size(d, 0) == 113664 + 89088 && heap_size(d, 1) == 201728 + 89088 && heap_size(d, 2) == 4096);
    unsigned w = 0, h = 0;
    CHECK(uit_output_res(&w, &h) == 1 && w == 2560 && h == 1440);
    free(d), free(z);
}
static void c_output_refused(void) {
    unsigned w0 = 0, h0 = 0;
    CHECK(uit_output_res(&w0, &h0) == 0);              /* decided at the mount, before the game opens the file */
    size_t n = 0;
    uint8_t *z = game_read(H_PATH, &n);
    CHECK(z && n == game_heap.n && !memcmp(z, game_heap.d, n));
    unsigned w = 0, h = 0;
    CHECK(uit_output_res(&w, &h) == 0);
    free(z);
}

int main(void) {
    snprintf(base, sizeof base, "/tmp/test_fs_romfix_%d", (int)getpid());
    make_dict();
    Buf raw = common(1.0f, -1);
    game_common = zs(&raw, dict_buf, dict_n);
    free(raw.d);
    raw = heapdef(113664, 201728), game_heap = zs(&raw, NULL, 0), free(raw.d);
    wfile("romfs/" ROMFIX_COMMON, game_common.d, game_common.n);
    wfile("romfs/" ROMFIX_HEAPDEF, game_heap.d, game_heap.n);
    wfile("romfs/Pack/ZsDic.pack.zs", pack_zs.d, pack_zs.n);
    wfile("romfs/Actor/Link.bfres.zs", "game-link", 9);

    run("default", "TOTK_MODS=0", c_default);
    run("default-mods", "", c_default);
    run("off", "TOTK_MENU_BG_FIX=0", c_off);
    { char p[600]; snprintf(p, sizeof p, "%s/romfix-cache", base); CHECK(access(p, F_OK) == 0); }
    run("cached", "", c_default);                     /* from the cache made by the first run */
    rmrf("romfix-cache");
    wfile("romfs/" ROMFIX_COMMON, "", 0);             /* a file the cache must not answer for: */
    rmrf("romfs/" ROMFIX_COMMON);
    wfile("romfs/" ROMFIX_COMMON, game_common.d, game_common.n);

    raw = common(0.0f, -1);
    Buf fixed = zs(&raw, dict_buf, dict_n);
    free(raw.d);
    wfile("mods/Fix/romfs/" ROMFIX_COMMON, fixed.d, fixed.n);
    wfile("mods/load-order.txt", "+Fix\n", 5);
    run("mod-fixed", "", c_mod_fixed);
    run("mod-fixed-mods-off", "TOTK_MODS=0", c_default);   /* mods off: the game's file, fixed */
    raw = common(1.0f, -1);
    Buf unfixed = zs(&raw, dict_buf, dict_n);
    free(raw.d);
    wfile("mods/Fix/romfs/" ROMFIX_COMMON, unfixed.d, unfixed.n);
    run("mod-unfixed", "", c_mod_unfixed);
    rmrf("mods");
    rmrf("romfix-cache");

    rmrf("romfs/Pack/ZsDic.pack.zs");
    run("no-dict", "", c_no_dict);
    wfile("romfs/Pack/ZsDic.pack.zs", pack_zs.d, pack_zs.n);

    run("output", "TOTK_OUTPUT_RES=2560x1440", c_output);
    raw = heapdef(100000, 201728), game_heap = zs(&raw, NULL, 0), free(raw.d);
    wfile("romfs/" ROMFIX_HEAPDEF, game_heap.d, game_heap.n);
    rmrf("romfix-cache");
    run("output-refused", "TOTK_OUTPUT_RES=2560x1440", c_output_refused);

    char cmd[400];
    if (!fails && !getenv("TEST_KEEP")) { snprintf(cmd, sizeof cmd, "rm -rf '%s'", base); if (system(cmd)) {} }
    else printf("logs kept in %s\n", base);
    printf(fails ? "fs romfix: %d FAILED\n" : "fs romfix: all ok\n", fails);
    return fails != 0;
}
