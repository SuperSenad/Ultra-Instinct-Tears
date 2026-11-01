/* [UIT 0041] Box test of the mods overlay in src/host/fs.c: a synthetic RomFS folder (made-up files, no game data)
 * and mods laid over it, read through the game's own nn::fs entry points (open, size, read, entry type, close).
 * Run with an argument "off" for TOTK_MODS=0 (the game's files only), "none" for no mods folder, "norestbl" for
 * TOTK_MODS_RESTBL=0 (mods, but the resource size table not merged), "vanilla" for [UIT 0062]: every way to turn
 * mods off (Load mods Off, Disable Mods, no mods folder), and off again after modded runs, reads the very same bytes
 * (every file, the resource size table byte for byte) as no mods at all, writes nothing, and says so in the log.
 *   zcc -O1 -Isrc -Isrc/host -I<zstd.h> tools/tests/test_fs_mods.c src/host/modfs.c src/host/restbl.c src/host/romfix.c \
 *       src/host/uit_display.c src/host/save_txn.c -lzstd -lm -lpthread -o /tmp/t/fsmods
 *   for m in on off none norestbl vanilla; do /tmp/t/fsmods $m; done */
#include <fcntl.h>
#include <time.h>
#include <unistd.h>
#define O_BINARY 0                                   /* Windows-only names fs.c uses */
#define _chsize_s(fd, size) ftruncate(fd, size)
#include "../../src/host/fs.c"
#include <zstd.h>

_Noreturn void runtime_fail(const char *m) { fprintf(stderr, "runtime_fail: %s\n", m); exit(3); }
uint64_t runtime_now_ns(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec; }
void runtime_account_uid(uint64_t out[2]) { out[0] = 1, out[1] = 2; }
const char *runtime_profile_dir(void) { return "/tmp"; }
void hle_native_thunk(Cpu *c) { (void)c; }
void plat_commit(uint64_t a, uint64_t s) { (void)a, (void)s; }
int plat_thread_ecores(void) { return 0; }   // [UIT 0089] platform stubs (TOTK_HYBRID_PLACEMENT, Windows only)
int plat_thread_game(void) { return 0; }
const int main_output_res_patch = 0, main_aspect_patch = 0;   /* [UIT 0068] the lift's flags (main_table.c) */
const int main_shadow_patch = 0;   /* [UIT 0070] */

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)
static char base[300];
static void put(const char *rel, const char *text) {
    char p[1024], cmd[1200];
    snprintf(p, sizeof p, "%s/%s", base, rel);
    snprintf(cmd, sizeof cmd, "mkdir -p \"$(dirname '%s')\"", p);
    if (system(cmd)) exit(2);
    FILE *f = fopen(p, "wb");
    if (!f) exit(2);
    fputs(text, f);
    fclose(f);
}
/* the whole file through nn::fs, "" when it cannot be opened */
static const char *slurp(const char *path) {
    static char buf[256];
    uint64_t h = 0, got = 0;
    int64_t size = -1;
    buf[0] = 0;
    if (open_file(&h, path, OPEN_READ)) return "";
    CHECK(file_size(&size, h) == 0 && size >= 0 && size < (int64_t)sizeof buf);
    CHECK(read_file(&got, h, 0, buf, sizeof buf - 1) == 0 && (int64_t)got == size);
    buf[got] = 0;
    uint64_t got2 = 0;
    char two[3] = {0};
    if (size >= 3) CHECK(read_file(&got2, h, 1, two, 2) == 0 && got2 == 2 && two[0] == buf[1]);   /* an offset read */
    close_file(h);
    return buf;
}
/* a made-up resource size table with one entry, Actor/Link.bfres = 1000 */
static size_t table_zs(uint8_t *z, size_t cap) {
    uint8_t raw[0x16 + 8] = "RESTBL";
    uint32_t v[] = {1, 0xA0, 1, 0}, e[] = {restbl_crc32("Actor/Link.bfres"), 1000};
    memcpy(raw + 6, v, 16), memcpy(raw + 0x16, e, 8);
    return ZSTD_compress(z, cap, raw, sizeof raw, 3);
}
/* the size table as the game reads it, decoded: the entry for name, 0 when it has none, -1 unreadable */
static int64_t game_table(const char *name) {
    static uint8_t z[4096], raw[8192];
    uint64_t h = 0, got = 0;
    int64_t size = 0;
    if (open_file(&h, "content:/System/Resource/ResourceSizeTable.Product.143.rsizetable.zs", OPEN_READ)) return -1;
    file_size(&size, h);
    CHECK(size > 0 && size < (int64_t)sizeof z && read_file(&got, h, 0, z, sizeof z) == 0 && (int64_t)got == size);
    close_file(h);
    size_t n = ZSTD_decompress(raw, sizeof raw, z, got);
    if (ZSTD_isError(n) || n < 0x16 || memcmp(raw, "RESTBL", 6)) return -1;
    uint32_t nc;
    memcpy(&nc, raw + 14, 4);
    for (uint32_t i = 0; i < nc; i++) {
        uint32_t e[2];
        memcpy(e, raw + 0x16 + i * 8, 8);
        if (e[0] == restbl_crc32(name)) return e[1];
    }
    return 0;
}
static int type_of(const char *path) { int t = -1; return entry_type(&t, path) ? -1 : t; }

/* ---------------------------------------- [UIT 0062] mods off = the unmodified game, byte for byte */
#include <sys/wait.h>
static const char *const vpaths[] = {"content:/Actor/Link.bfres.zs", "content:/UI/Common.blarc.zs", "content:/Pack/Npc.pack.zs",
                                     "content:/UI/New/Added.bntx.zs", "content:/System/Resource/ResourceSizeTable.Product.143.rsizetable.zs",
                                     "content:/Nope.zs", "content:/UI/New", "content:/UI"};
static uint64_t fnv(uint64_t h, const void *p, size_t n) {
    for (size_t i = 0; i < n; i++) h = (h ^ ((const uint8_t *)p)[i]) * 0x100000001b3ull;
    return h;
}
/* everything the game can see through nn::fs for vpaths: open or not, size, all bytes, the entry type */
static uint64_t game_view(void) {
    uint64_t h = 0xcbf29ce484222325ull;
    for (size_t i = 0; i < sizeof vpaths / sizeof *vpaths; i++) {
        static uint8_t buf[65536];
        uint64_t fh = 0, got = 0;
        int64_t size = -1;
        int r = open_file(&fh, vpaths[i], OPEN_READ) ? 0 : 1, t = type_of(vpaths[i]);
        if (r) file_size(&size, fh), read_file(&got, fh, 0, buf, sizeof buf), close_file(fh);
        h = fnv(h, &r, sizeof r), h = fnv(h, &t, sizeof t), h = fnv(h, &size, sizeof size), h = fnv(h, buf, r ? (size_t)got : 0);
    }
    return h;
}
/* every file under base (names, sizes, bytes), the logs aside: what a run left behind */
static uint64_t tree(const char *sub) {
    char cmd[800];
    snprintf(cmd, sizeof cmd, "cd '%s' && find %s -type f ! -name 'log-*' | LC_ALL=C sort | while read f; do echo \"$f\"; cat \"$f\"; done", base, sub);
    FILE *f = popen(cmd, "r");
    uint64_t h = 0xcbf29ce484222325ull;
    char b[4096];
    size_t n;
    while (f && (n = fread(b, 1, sizeof b, f)) > 0) h = fnv(h, b, n);
    if (f) pclose(f);
    return h;
}
/* one game start in a fresh process (fs.c keeps its mount in statics): what it sees, and its log in log-<tag>.txt */
static const uint8_t *want_tbl;      /* when set: the size table the game reads must be these bytes */
static size_t want_n;
static uint64_t start_game(const char *tag, const char *mods_env, const char *mods_dir) {
    int fd[2];
    uint64_t h = 0;
    if (pipe(fd)) exit(2);
    fflush(stdout), fflush(stderr);
    pid_t pid = fork();
    if (!pid) {
        char lp[600], rom[400];
        snprintf(lp, sizeof lp, "%s/log-%s.txt", base, tag);
        if (!freopen(lp, "w", stderr)) _exit(2);
        snprintf(rom, sizeof rom, "%s/romfs", base);
        setenv("TOTK_ROMFS", rom, 1);
        setenv("TOTK_MODS_DIR", mods_dir, 1);
        if (mods_env) setenv("TOTK_MODS", mods_env, 1);
        if (mount_rom("content", NULL, 0)) { fflush(stderr); _exit(3); }
        h = game_view();
        if (want_tbl) {   /* the view is the files themselves: the size table is the file's own bytes, not a merge */
            static uint8_t b[4096];
            uint64_t th = 0, got = 0;
            CHECK(open_file(&th, vpaths[4], OPEN_READ) == 0 && read_file(&got, th, 0, b, sizeof b) == 0);
            close_file(th);
            CHECK(got == want_n && !memcmp(b, want_tbl, want_n));
        }
        (void)!write(fd[1], &h, sizeof h);
        fflush(stdout), fflush(stderr);
        _exit(fails ? 4 : 0);
    }
    close(fd[1]);
    int st = 0;
    CHECK(read(fd[0], &h, sizeof h) == sizeof h);
    close(fd[0]);
    waitpid(pid, &st, 0);
    if (!WIFEXITED(st) || WEXITSTATUS(st)) printf("game start %s: status %d\n", tag, st);
    CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0);
    return h;
}
static int log_has(const char *tag, const char *text) {
    char p[600], b[8192] = {0};
    snprintf(p, sizeof p, "%s/log-%s.txt", base, tag);
    FILE *f = fopen(p, "r");
    if (!f) return 0;
    size_t n = fread(b, 1, sizeof b - 1, f);
    fclose(f);
    b[n] = 0;
    if (!strstr(b, text)) printf("log %s lacks \"%s\":\n%s", tag, text, b);
    return strstr(b, text) != NULL;
}
static void set_order(const char *md, int top, int low) {
    const char *names[] = {"Top", "Low"};
    int en[] = {top, low};
    CHECK(modfs_write_order(md, names, en, 2) == 0);   /* the launcher's own writer */
}
static int vanilla_test(void) {
    uint8_t z[256];
    char p[600], md[400], nomods[400];
    size_t zn = table_zs(z, sizeof z);
    put("romfs/Actor/Link.bfres.zs", "game-link");
    put("romfs/UI/Common.blarc.zs", "game-ui");
    put("romfs/Pack/Npc.pack.zs", "game-npc");
    put("romfs/System/Resource/ResourceSizeTable.Product.143.rsizetable.zs", "");
    snprintf(p, sizeof p, "%s/romfs/System/Resource/ResourceSizeTable.Product.143.rsizetable.zs", base);
    FILE *f = fopen(p, "wb");
    fwrite(z, 1, zn, f), fclose(f);
    snprintf(md, sizeof md, "%s/mods", base);
    snprintf(nomods, sizeof nomods, "%s/no-such-mods", base);
    /* the reference: no mods folder at all */
    want_tbl = z, want_n = zn;
    uint64_t vanilla = start_game("none", NULL, nomods), game = tree("romfs");
    want_tbl = NULL;
    CHECK(log_has("none", "mods: none active, playing the unmodified game (no mods installed"));
    /* mods that change files, add one, and ship their own size table; leftovers of earlier runs and installs */
    put("mods/Top/romfs/Actor/Link.bfres.zs", "top-link");
    put("mods/Top/romfs/UI/New/Added.bntx.zs", "top-added");
    put("mods/Low/romfs/ui/common.BLARC.zs", "low-ui");
    {   /* Low ships its own size table (Link = 2000): merged when Low is on, never read when it is off */
        uint8_t raw[0x16 + 8] = "RESTBL", lz[256];
        uint32_t v[] = {1, 0xA0, 1, 0}, e[] = {restbl_crc32("Actor/Link.bfres"), 2000};
        memcpy(raw + 6, v, 16), memcpy(raw + 0x16, e, 8);
        size_t ln = ZSTD_compress(lz, sizeof lz, raw, sizeof raw, 1);
        put("mods/Low/romfs/System/Resource/ResourceSizeTable.Product.143.rsizetable.zs", "");
        snprintf(p, sizeof p, "%s/mods/Low/romfs/System/Resource/ResourceSizeTable.Product.143.rsizetable.zs", base);
        f = fopen(p, "wb");
        fwrite(lz, 1, ln, f), fclose(f);
    }
    put("mods/Low/uit-placed.txt", "UI/Common.blarc.zs\n");
    put("mods/.undo/old/Top/romfs/Pack/Npc.pack.zs", "undo-npc");
    put("mods/.uit-mods-before-disable.txt", "Top\nLow\n");
    put("mods/README.txt", "r");
    put("mods/empty.txt", "");
    set_order(md, 1, 1);
    uint64_t modded = start_game("on", NULL, md);
    CHECK(modded != vanilla);
    CHECK(log_has("on", "mods: 2 active (highest first): Top, Low"));
    CHECK(log_has("on", "mods: resource size table merged: 1 mod table"));   /* the modded run really merged one */
    uint64_t after_on = tree(".");
    /* Load mods: Off with every mod still on */
    CHECK(start_game("loadoff", "0", md) == vanilla);
    CHECK(log_has("loadoff", "mods: none active, playing the unmodified game (Load mods is Off"));
    /* Disable Mods: all off; then on, off, on, off again, the same folder each time */
    for (int round = 0; round < 3; round++) {
        char tag[16];
        set_order(md, 0, 0);
        snprintf(tag, sizeof tag, "off%d", round);
        CHECK(start_game(tag, NULL, md) == vanilla);
        CHECK(log_has(tag, "mods: none active, playing the unmodified game (2 installed, all off)"));
        set_order(md, 1, 1);
        snprintf(tag, sizeof tag, "on%d", round);
        CHECK(start_game(tag, NULL, md) == modded);   /* and back on: the same mods as before */
    }
    /* one on, one off: only that one (a check that "off" really is per mod) */
    set_order(md, 0, 1);
    uint64_t low = start_game("lowonly", NULL, md);
    CHECK(low != vanilla && low != modded);
    CHECK(log_has("lowonly", "mods: 1 active (highest first): Low"));
    /* no run wrote anything: the game's files as they were, the mods folder as it was (load-order.txt aside) */
    CHECK(tree("romfs") == game);
    set_order(md, 1, 1);
    CHECK(tree(".") == after_on);
    snprintf(p, sizeof p, "rm -rf '%s'", base);
    if (system(p)) printf("note: could not remove %s\n", base);
    printf(fails ? "fs mods (vanilla): %d FAILED\n" : "fs mods (vanilla): all ok\n", fails);
    return fails != 0;
}

int main(int argc, char **argv) {
    const char *mode = argc > 1 ? argv[1] : "on";
    if (!strcmp(mode, "vanilla")) {
        snprintf(base, sizeof base, "/tmp/test_fs_mods_%d", (int)getpid());
        return vanilla_test();
    }
    snprintf(base, sizeof base, "/tmp/test_fs_mods_%d", (int)getpid());
    put("romfs/Actor/Link.bfres.zs", "game-link");
    put("romfs/UI/Common.blarc.zs", "game-ui");
    put("romfs/Pack/Npc.pack.zs", "game-npc");
    {
        uint8_t z[256];
        char p[600];
        size_t n = table_zs(z, sizeof z);
        put("romfs/System/Resource/ResourceSizeTable.Product.143.rsizetable.zs", "");
        snprintf(p, sizeof p, "%s/romfs/System/Resource/ResourceSizeTable.Product.143.rsizetable.zs", base);
        FILE *f = fopen(p, "wb");
        fwrite(z, 1, n, f), fclose(f);
    }
    if (strcmp(mode, "none")) {
        put("mods/Top/romfs/Actor/Link.bfres.zs", "top-link");
        put("mods/Top/romfs/UI/New/Added.bntx.zs", "top-added");
        put("mods/Low/romfs/Actor/Link.bfres.zs", "low-link");
        put("mods/Low/romfs/ui/common.BLARC.zs", "low-ui");        /* another letter case than the game's */
        put("mods/load-order.txt", "+Top\n+Low\n");
    }
    char rom[400], md[400];
    snprintf(rom, sizeof rom, "%s/romfs", base);
    snprintf(md, sizeof md, "%s/mods", base);
    setenv("TOTK_ROMFS", rom, 1);
    setenv("TOTK_MODS_DIR", md, 1);
    if (!strcmp(mode, "off")) setenv("TOTK_MODS", "0", 1);
    if (!strcmp(mode, "norestbl")) setenv("TOTK_MODS_RESTBL", "0", 1);
    CHECK(mount_rom("content", NULL, 0) == 0);
    int on = !strcmp(mode, "on") || !strcmp(mode, "norestbl"), merged = !strcmp(mode, "on");
    /* the size table: the game's entry for Link; with the merge, an entry for the mod's new texture too */
    CHECK(game_table("Actor/Link.bfres") == 1000);
    CHECK(game_table("UI/New/Added.bntx") == (merged ? (int64_t)restbl_estimate("UI/New/Added.bntx", 9) : 0));
    CHECK(game_table("Actor/Link.bfres") == 1000);   /* a second open: the same merge, made once */
    CHECK(!strcmp(slurp("content:/Actor/Link.bfres.zs"), on ? "top-link" : "game-link"));
    CHECK(!strcmp(slurp("content:/UI/Common.blarc.zs"), on ? "low-ui" : "game-ui"));
    CHECK(!strcmp(slurp("content:/Pack/Npc.pack.zs"), "game-npc"));
    CHECK(!strcmp(slurp("content:/UI/New/Added.bntx.zs"), on ? "top-added" : ""));
    CHECK(!strcmp(slurp("content:/Nope.zs"), ""));
    CHECK(type_of("content:/UI/New/Added.bntx.zs") == (on ? 1 : -1));
    CHECK(type_of("content:/UI/New") == (on ? 0 : -1));
    CHECK(type_of("content:/UI") == 0 && type_of("content:/Actor/Link.bfres.zs") == 1);
    /* handles are reused: open and close many times (no fd leak: the descriptor count stays put) */
    for (int i = 0; i < 5000; i++) CHECK(slurp("content:/Actor/Link.bfres.zs")[0]);
    int fd = open("/dev/null", O_RDONLY);
    CHECK(fd >= 0 && fd < 20);
    close(fd);
    char cmd[400];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", base);
    if (system(cmd)) printf("note: could not remove %s\n", base);
    printf(fails ? "fs mods (%s): %d FAILED\n" : "fs mods (%s): all ok\n", mode, fails);
    return fails != 0;
}
