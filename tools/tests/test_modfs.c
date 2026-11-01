/* [UIT 0041] Box test of src/host/modfs.c (the mods index the game and the launcher share): layouts, load order,
 * priority and conflicts, unsupported content, info.json, the game's spelling of a path, and the index's speed.
 * Only synthetic files in a temporary folder: no game data.
 *   cc -O2 -Isrc/host tools/tests/test_modfs.c src/host/modfs.c -o /tmp/t/modfs && /tmp/t/modfs */
#include "modfs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)
static char base[512];

static void mkdirs(const char *rel) {
    char p[2048];
    snprintf(p, sizeof p, "%s/%s", base, rel);
    for (char *s = p + strlen(base) + 1; *s; s++)
        if (*s == '/') { *s = 0; mkdir(p, 0700); *s = '/'; }
    mkdir(p, 0700);
}
static void put(const char *rel, const char *text) {
    char p[2048];
    snprintf(p, sizeof p, "%s", rel);
    char *slash = strrchr(p, '/');
    if (slash) { *slash = 0; mkdirs(p); }
    snprintf(p, sizeof p, "%s/%s", base, rel);
    FILE *f = fopen(p, "wb");
    if (!f) { perror(p); exit(2); }
    fputs(text, f);
    fclose(f);
}
static int mod_index(const Modfs *m, const char *name) {
    for (int i = 0; i < m->nmods; i++)
        if (!strcmp(m->mods[i].name, name)) return i;
    return -1;
}
/* the "game": a few paths in its own spelling */
static const char *const kGame[] = {"Actor/Link.bfres.zs", "Pack/Actor/Npc.pack.zs", "UI/LayoutArchive/Common.blarc.zs",
                                    "System/Resource/ResourceSizeTable.Product.143.rsizetable.zs"};
static const char *canon(const char *path, void *ud) {
    (void)ud;
    for (size_t i = 0; i < sizeof kGame / sizeof *kGame; i++)
        if (!strcasecmp(kGame[i], path)) return kGame[i];
    return NULL;
}
static double secs(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }

int main(void) {
    snprintf(base, sizeof base, "/tmp/test_modfs_%d", (int)getpid());
    mkdir(base, 0700);
    char mods[600];
    snprintf(mods, sizeof mods, "%s/mods", base);
    Modfs m;

    /* no folder, empty folder: nothing */
    CHECK(modfs_load(&m, mods, 0, canon, NULL, NULL) == 0 && m.nfiles == 0);
    modfs_free(&m);
    mkdirs("mods");
    CHECK(modfs_load(&m, mods, 0, canon, NULL, NULL) == 0);
    modfs_free(&m);

    /* layouts: romfs/, RomFS/ (any case), bare game folders; junk ignored */
    put("mods/Alpha/romfs/Actor/Link.bfres.zs", "alpha-link");
    put("mods/Alpha/romfs/UI/LayoutArchive/Common.blarc.zs", "alpha-ui");
    put("mods/Alpha/romfs/.DS_Store", "junk");
    put("mods/Alpha/info.json", "{\n \"name\": \"Alpha \\\"Tweaks\\\"\",\n \"version\": 1.2,\n \"Author\": \"Someone\",\n"
                                " \"description\": \"Line one\\nline two \\u00e9\"\n}\n");
    put("mods/Beta/RomFS/Actor/Link.bfres.zs", "beta-link");
    put("mods/Beta/RomFS/Pack/Actor/Npc.pack.zs", "beta-npc");
    put("mods/Charlie/Actor/Link.bfres.zs", "charlie-link");       /* bare: no romfs folder */
    put("mods/Charlie/readme.txt", "not romfs");
    put("mods/Charlie/UI/New/Thing.bntx.zs", "charlie-new");
    put("mods/__MACOSX/Actor/x", "junk folder");
    /* order file: Beta off, then Alpha; Charlie is not listed (new: on, on top) */
    put("mods/load-order.txt", "\xEF\xBB\xBF# comment\r\n-Beta\r\n  +Alpha  \r\n+Gone\r\n\r\n");
    int n = modfs_load(&m, mods, 1, canon, NULL, NULL);
    CHECK(n == 3);
    CHECK(n == 3 && !strcmp(m.mods[0].name, "Charlie") && !strcmp(m.mods[1].name, "Beta") && !strcmp(m.mods[2].name, "Alpha"));
    int a = mod_index(&m, "Alpha"), b = mod_index(&m, "Beta"), c = mod_index(&m, "Charlie");
    CHECK(a >= 0 && b >= 0 && c >= 0);
    if (a < 0 || b < 0 || c < 0) return 1;
    CHECK(m.mods[c].enabled && !m.mods[c].listed && !m.mods[b].enabled && m.mods[b].listed && m.mods[a].enabled);
    CHECK(strstr(m.mods[a].root, "Alpha/romfs") && strstr(m.mods[b].root, "Beta/RomFS"));
    CHECK(!strcmp(m.mods[c].root, "") == 0 && strstr(m.mods[c].root, "mods/Charlie"));
    CHECK(m.mods[a].nfiles == 2 && m.mods[b].nfiles == 2 && m.mods[c].nfiles == 2);   /* walk_disabled counts Beta */
    CHECK(m.mods[a].bytes == strlen("alpha-link") + strlen("alpha-ui"));
    CHECK(!strcmp(m.mods[a].title, "Alpha \"Tweaks\"") && !strcmp(m.mods[a].version, "1.2") && !strcmp(m.mods[a].author, "Someone"));
    CHECK(!strcmp(m.mods[a].description, "Line one\nline two ?"));
    CHECK(m.mods[a].unsupported == 0 && m.mods[b].unsupported == 0 && m.mods[c].unsupported == 0);
    /* winners: Charlie (top) takes Link from Alpha; Beta is off; files sorted */
    CHECK(m.nfiles == 3);
    const ModfsFile *f = modfs_find(&m, "Actor/Link.bfres.zs");
    CHECK(f && f->mod == c && f->nshadow == 1 && f->vanilla == 1 && strstr(f->host, "Charlie/Actor/Link.bfres.zs"));
    CHECK(m.pairs[c * n + a] == 1 && m.pairs[a * n + c] == 0 && m.mods[a].nlost == 1 && m.mods[c].nwon == 1);
    f = modfs_find(&m, "UI/New/Thing.bntx.zs");
    CHECK(f && f->mod == c && f->vanilla == 0 && m.mods[c].nnew == 1);
    CHECK(!modfs_find(&m, "Pack/Actor/Npc.pack.zs"));                          /* Beta is off */
    CHECK(!modfs_find(&m, "Actor/Link.bfres") && !modfs_find(&m, "actor/Link.bfres.zs"));
    CHECK(modfs_has_folder(&m, "UI/") && modfs_has_folder(&m, "UI/New/") && !modfs_has_folder(&m, "Pack/"));
    for (size_t k = 1; k < m.nfiles; k++) CHECK(strcmp(m.files[k - 1].path, m.files[k].path) < 0);
    modfs_free(&m);

    /* the launcher's order: Beta on, top; then Alpha; Charlie last */
    const char *names[] = {"Beta", "Alpha", "Charlie"};
    int en[] = {1, 1, 1};
    CHECK(modfs_write_order(mods, names, en, 3) == 0);
    n = modfs_load(&m, mods, 0, canon, NULL, NULL);
    a = mod_index(&m, "Alpha"), b = mod_index(&m, "Beta"), c = mod_index(&m, "Charlie");
    CHECK(n == 3 && b == 0 && a == 1 && c == 2);
    f = modfs_find(&m, "Actor/Link.bfres.zs");
    CHECK(f && f->mod == b && f->nshadow == 2 && m.pairs[b * n + a] == 1 && m.pairs[b * n + c] == 1);
    CHECK(m.mods[c].nlost == 1 && m.mods[a].nlost == 1 && m.mods[b].nwon == 1);
    CHECK(modfs_find(&m, "Pack/Actor/Npc.pack.zs") && m.nfiles == 4);
    modfs_free(&m);

    /* a path in another letter case: the game's spelling is the key, the mod's own is the file */
    put("mods/Delta/romfs/actor/link.BFRES.zs", "delta");
    n = modfs_load(&m, mods, 0, canon, NULL, NULL);
    int dl = mod_index(&m, "Delta");
    CHECK(dl == 0);   /* unlisted: on top */
    f = modfs_find(&m, "Actor/Link.bfres.zs");
    CHECK(f && f->mod == dl && strstr(f->host, "Delta/romfs/actor/link.BFRES.zs") && f->vanilla == 1 && m.mods[dl].nnew == 0);
    FILE *fp = f ? fopen(f->host, "rb") : NULL;
    CHECK(fp != NULL);
    if (fp) fclose(fp);
    modfs_free(&m);

    /* unsupported content: flagged, never silent */
    put("mods/Echo 60FPS/exefs/abcdef.ips", "PATCH");
    put("mods/Foxtrot/exefs/main.pchtxt", "@nsobid-");
    put("mods/Foxtrot/romfs/UI/LayoutArchive/Common.blarc.zs", "fox");
    put("mods/Golf/cheats/0123.txt", "[inf hearts]");
    put("mods/Hotel/readme.md", "just a readme");
    put("mods/India/subsdk9", "nso");
    put("mods/India/romfs/Actor/x.zs", "x");
    put("mods/Juliet/tool.dll", "MZ");
    put("mods/Kilo Res 1440p/exefs/x.pchtxt", "p");
    n = modfs_load(&m, mods, 0, canon, NULL, NULL);
    int e = mod_index(&m, "Echo 60FPS"), fo = mod_index(&m, "Foxtrot"), g = mod_index(&m, "Golf"), h = mod_index(&m, "Hotel"),
        in = mod_index(&m, "India"), j = mod_index(&m, "Juliet"), k = mod_index(&m, "Kilo Res 1440p");
    CHECK(e >= 0 && fo >= 0 && g >= 0 && h >= 0 && in >= 0 && j >= 0 && k >= 0);
    if (e < 0 || fo < 0 || g < 0 || h < 0 || in < 0 || j < 0 || k < 0) return 1;
    CHECK(m.mods[e].unsupported == (MODFS_U_EXEFS | MODFS_U_IPS | MODFS_U_FPSRES | MODFS_U_EMPTY));
    CHECK(m.mods[fo].unsupported == (MODFS_U_EXEFS | MODFS_U_PCHTXT) && m.mods[fo].nfiles == 1);
    CHECK(m.mods[g].unsupported == (MODFS_U_CHEATS | MODFS_U_EMPTY));
    CHECK(m.mods[h].unsupported == MODFS_U_EMPTY);
    CHECK(m.mods[in].unsupported == MODFS_U_NSO && m.mods[in].nfiles == 1);
    CHECK(m.mods[j].unsupported == (MODFS_U_DLL | MODFS_U_EMPTY));
    CHECK(m.mods[k].unsupported & MODFS_U_FPSRES);
    CHECK(strstr(modfs_unsupported_text(m.mods[e].unsupported), "built in"));
    CHECK(strstr(modfs_unsupported_text(m.mods[fo].unsupported), "code patches"));
    CHECK(strstr(modfs_unsupported_text(MODFS_U_CHEATS), "cheat"));
    CHECK(!*modfs_unsupported_text(0));
    /* Foxtrot (unlisted, by name) is above Alpha (listed): it wins the UI file */
    f = modfs_find(&m, "UI/LayoutArchive/Common.blarc.zs");
    CHECK(f && f->mod == fo);
    modfs_free(&m);

    /* every enabled mod's size tables, highest first (not only the winner's), any letter case */
    put("mods/Foxtrot/romfs/System/Resource/ResourceSizeTable.Product.143.rsizetable.zs", "t1");
    put("mods/India/romfs/system/resource/ResourceSizeTable.Product.121.rsizetable.zs", "t2");
    n = modfs_load(&m, mods, 0, canon, NULL, NULL);
    CHECK(m.nrestbls == 2 && strstr(m.restbls[0], "Foxtrot") && strstr(m.restbls[1], "India"));
    CHECK(m.mods[mod_index(&m, "Foxtrot")].nrestbl == 1);
    modfs_free(&m);
    CHECK(modfs_is_restbl("system/Resource/resourcesizetable.product.143.RSIZETABLE.zs"));

    /* resource size table names */
    char nm[256];
    CHECK(!strcmp(modfs_restbl_name("Actor/Link.bfres.zs", nm, sizeof nm), "Actor/Link.bfres"));
    CHECK(!strcmp(modfs_restbl_name("Tex/a.ta.zs", nm, sizeof nm), "Tex/a.ta.zs"));
    CHECK(!strcmp(modfs_restbl_name("Shader/x.mc", nm, sizeof nm), "Shader/x"));
    CHECK(!strcmp(modfs_restbl_name("Sound/y.bars", nm, sizeof nm), "Sound/y.bars"));
    CHECK(modfs_is_restbl("System/Resource/ResourceSizeTable.Product.143.rsizetable.zs"));
    CHECK(!modfs_is_restbl("System/Resource/Other.zs"));
    CHECK(modfs_known_game_dir("pack") && modfs_known_game_dir("UI") && !modfs_known_game_dir("exefs"));

    /* copied in by hand with extra folders around it: followed down while each level holds one folder */
    {
        char m2[700];
        snprintf(m2, sizeof m2, "%s/mods2", base);
        put("mods2/Fix/Fix/romfs/UI/LayoutArchive/Common.blarc.zs", "fix");
        put("mods2/Fix/Fix/readme.txt", "r");
        put("mods2/Atmo/atmosphere/contents/0100F2C0115B6000/romfs/Actor/Link.bfres.zs", "atmo");
        put("mods2/Deep/a/b/Pack/Actor/Npc.pack.zs", "deep");
        put("mods2/Two/x/romfs/Actor/Link.bfres.zs", "two");
        put("mods2/Two/y/romfs/Actor/Link.bfres.zs", "two");                 /* two choices: not guessed */
        put("mods2/Code/sub/exefs/main.ips", "ips");                          /* a code mod: not followed */
        put("mods2/Code/sub/cheats/x.txt", "c");
        n = modfs_load(&m, m2, 0, canon, NULL, NULL);
        int fx = mod_index(&m, "Fix"), at = mod_index(&m, "Atmo"), dp = mod_index(&m, "Deep"), tw = mod_index(&m, "Two"),
            cd = mod_index(&m, "Code");
        CHECK(n == 5 && fx >= 0 && at >= 0 && dp >= 0 && tw >= 0 && cd >= 0);
        if (fx >= 0 && at >= 0 && dp >= 0 && tw >= 0 && cd >= 0) {
            CHECK(strstr(m.mods[fx].root, "Fix/Fix/romfs") && m.mods[fx].nfiles == 1);
            CHECK(strstr(m.mods[at].root, "0100F2C0115B6000/romfs") && m.mods[at].nfiles == 1);
            CHECK(strstr(m.mods[dp].root, "Deep/a/b") && m.mods[dp].nfiles == 1);
            CHECK(m.mods[tw].root[0] == 0 && m.mods[tw].nfiles == 0);
            CHECK(m.mods[cd].nfiles == 0 && (m.mods[cd].unsupported & MODFS_U_EXEFS));
        }
        modfs_free(&m);
    }

    /* speed: a 20,000-file mod, indexed once at start; lookups are a binary search */
    for (int d = 0; d < 100; d++)
        for (int i = 0; i < 200; i++) {
            char rel[200];
            snprintf(rel, sizeof rel, "mods/Big/romfs/Model/D%03d/F%04d.bfres.zs", d, i);
            put(rel, "x");
        }
    double t0 = secs();
    n = modfs_load(&m, mods, 0, canon, NULL, NULL);
    double t1 = secs();
    CHECK(m.nfiles > 20000);
    volatile int hits = 0;
    char q[200];
    double t2 = secs();
    for (int r = 0; r < 1000000; r++) {
        snprintf(q, sizeof q, "Model/D%03d/F%04d.bfres.zs", r % 100, (r / 100) % 200);
        hits += modfs_find(&m, q) != NULL;
    }
    double t3 = secs();
    for (int r = 0; r < 1000000; r++) snprintf(q, sizeof q, "Model/D%03d/F%04d.bfres.zs", r % 100, (r / 100) % 200);
    double t4 = secs();
    CHECK(hits == 1000000);
    printf("index: %zu files in %.0f ms; lookup %.0f ns each (after the path formatting)\n", m.nfiles, (t1 - t0) * 1e3,
           ((t3 - t2) - (t4 - t3)) * 1e9 / 1e6);
    modfs_free(&m);

    char cmd[700];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", base);
    if (system(cmd)) printf("note: could not remove %s\n", base);
    printf(fails ? "modfs: %d FAILED\n" : "modfs: all ok\n", fails);
    return fails != 0;
}
