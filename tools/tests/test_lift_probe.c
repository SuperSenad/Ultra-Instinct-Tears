/* [UIT 0070] Box test of src/host/uit_lift.c: the switches and the shadow-size probe that picks one candidate (no game
 * code: the probe calls and the textures are made up, the clock is the test's). Each case runs in its own process
 * (the probe's state lives for the run, as in the game), in a scratch folder (diag/ is written there):
 *   cc -O1 tools/tests/test_lift_probe.c src/host/uit_lift.c -o /tmp/t/liftprobe && /tmp/t/liftprobe */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

void uit_lift_init(void), uit_lift_texture(unsigned, unsigned, unsigned), uit_lift_exit(void);

const int main_quality_patch = 1, main_fxaa_flag_patch = 1, main_lod_patch = 0, main_shadow_patch = 0, main_shadow_probes = 2;
const unsigned long long main_shadow_probe_sites[] = { 0x1234560ull, 0x2345670ull };
const char main_lift_build[] = "277178b7dba1b6d4";
static uint64_t g_now = 5000000000ull;
uint64_t runtime_now_ns(void) { return g_now; }
unsigned uit_shadow_res_setting(void) { const char *e = getenv("TOTK_SHADOW_RES"); return e ? (unsigned)atoi(e) : 0; }
unsigned uit_shadow_res(void) { return main_shadow_patch ? uit_shadow_res_setting() : 0; }
uint64_t uit_shadow_extra_kib_for(unsigned s) { return s; }

extern volatile int totk_lift_probe;
void totk_shadow_probe(int k, uint32_t v);
int totk_quality_reduction_off(void), totk_game_fxaa_off(void), totk_lod_improve(void);
uint32_t totk_shadow_size(void);

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)
#define MS 1000000ull
static void at(uint64_t ms) { g_now += ms * MS; }
static char *slurp(const char *p) {
    static char b[4096];
    FILE *f = fopen(p, "r");
    if (!f) return NULL;
    size_t n = fread(b, 1, sizeof b - 1, f);
    fclose(f);
    b[n] = 0;
    return b;
}
static int confirmed(void) { return access("diag/lift-confirm.txt", F_OK) == 0; }

static void run(const char *c) {
    uit_lift_init();
    if (!strcmp(c, "two")) {                     /* two setups, each followed by its map: the first candidate */
        CHECK(totk_lift_probe);
        totk_shadow_probe(1, 77), totk_shadow_probe(0, 1024), at(5), uit_lift_texture(1024, 1024, 0x34);
        CHECK(!confirmed());
        at(3000), totk_shadow_probe(0, 1024), at(5), uit_lift_texture(1024, 1024, 0x34);
        CHECK(confirmed() && !totk_lift_probe);
        char *s = slurp("diag/lift-confirm.txt");
        CHECK(s && !strcmp(s, "shadow 1234560 build 277178b7dba1b6d4 value 1024 hits 2\n"));
        uit_lift_exit();
        s = slurp("diag/lift-probe.txt");
        CHECK(s && strstr(s, "confirmed 1234560") && strstr(s, "candidate 1234560 fired 2 hits 2 sizes 1024") &&
              strstr(s, "candidate 2345670 fired 1 hits 0 sizes none"));
    } else if (!strcmp(c, "rivals")) {           /* both pass the same size each time: never told apart */
        for (int i = 0; i < 4; i++) totk_shadow_probe(0, 2048), totk_shadow_probe(1, 2048), at(5), uit_lift_texture(2048, 2048, 0x33), at(2000);
        uit_lift_exit();
        CHECK(!confirmed());
        CHECK(strstr(slurp("diag/lift-probe.txt"), "confirmed none yet") != NULL);
    } else if (!strcmp(c, "everyframe")) {       /* a 1024 depth texture made every frame anyway proves nothing */
        for (int i = 0; i < 10; i++) uit_lift_texture(1024, 1024, 0x34), at(16);
        totk_shadow_probe(0, 1024), at(5), uit_lift_texture(1024, 1024, 0x34);
        at(16), uit_lift_texture(1024, 1024, 0x34);
        uit_lift_exit();
        CHECK(!confirmed());
    } else if (!strcmp(c, "late")) {             /* the map more than a second after: no hit */
        totk_shadow_probe(0, 1024), at(1500), uit_lift_texture(1024, 1024, 0x34);
        uit_lift_exit();
        CHECK(!confirmed());
    } else if (!strcmp(c, "lone")) {             /* one setup, one map, nothing else: settled at exit */
        totk_shadow_probe(1, 4096), at(5), uit_lift_texture(4096, 4096, 0x36);
        CHECK(!confirmed());
        uit_lift_exit();
        CHECK(confirmed() && strstr(slurp("diag/lift-confirm.txt"), "shadow 2345670 build 277178b7dba1b6d4 value 4096 hits 1"));
    } else if (!strcmp(c, "lonetime")) {         /* ... or two minutes later without an exit (the window close) */
        totk_shadow_probe(1, 4096), at(5), uit_lift_texture(4096, 4096, 0x36);
        at(60000), uit_lift_texture(512, 512, 0x34);
        CHECK(!confirmed());
        at(61000), uit_lift_texture(512, 512, 0x34);
        CHECK(confirmed());
    } else if (!strcmp(c, "lonetwice")) {        /* a lone hit from a place that fired twice: not settled */
        totk_shadow_probe(1, 1024), at(3000), totk_shadow_probe(1, 1024), at(5), uit_lift_texture(1024, 1024, 0x34);
        uit_lift_exit();
        CHECK(!confirmed());
    } else if (!strcmp(c, "hot")) {              /* busy code is not the setup */
        for (int i = 0; i < 100001; i++) totk_shadow_probe(0, 1024);
        at(5), uit_lift_texture(1024, 1024, 0x34);
        uit_lift_exit();
        CHECK(!confirmed());
        CHECK(strstr(slurp("diag/lift-probe.txt"), "(too often: not the setup)") != NULL);
    } else if (!strcmp(c, "shape")) {            /* colour formats, rectangles, odd sizes: not a shadow map */
        totk_shadow_probe(0, 1024), at(5);
        uit_lift_texture(1024, 1024, 0x25), uit_lift_texture(1024, 512, 0x34), uit_lift_texture(1000, 1000, 0x34);
        totk_shadow_probe(1, 300);
        uit_lift_exit();
        CHECK(!confirmed());
        CHECK(strstr(slurp("diag/lift-probe.txt"), "square depth textures seen 0") != NULL);
    } else if (!strcmp(c, "off")) {              /* TOTK_LIFT_PROBE=0 */
        CHECK(!totk_lift_probe);
        totk_shadow_probe(0, 1024), at(5), uit_lift_texture(1024, 1024, 0x34);
        at(3000), totk_shadow_probe(0, 1024), at(5), uit_lift_texture(1024, 1024, 0x34);
        CHECK(!confirmed());
    } else if (!strcmp(c, "switches")) {         /* each switch needs its site and its setting */
        CHECK(totk_quality_reduction_off() && totk_game_fxaa_off() && !totk_lod_improve() && totk_shadow_size() == 0);
    } else if (!strcmp(c, "defaults")) {
        CHECK(!totk_quality_reduction_off() && !totk_game_fxaa_off() && !totk_lod_improve() && totk_shadow_size() == 0);
    }
}

int main(void) {
    const char *cases[] = {"two", "rivals", "everyframe", "late", "lone", "lonetime", "lonetwice", "hot", "shape", "off", "switches", "defaults"};
    char tmp[] = "/tmp/liftprobe.XXXXXX";
    if (!mkdtemp(tmp) || chdir(tmp)) return 2;
    for (size_t i = 0; i < sizeof cases / sizeof *cases; i++) {
        if (system("rm -rf diag")) {}
        fflush(stdout);
        pid_t p = fork();
        if (!p) {
            unsetenv("TOTK_LIFT_PROBE"), unsetenv("TOTK_QUALITY_REDUCTION"), unsetenv("TOTK_GAME_FXAA"), unsetenv("TOTK_LOD_IMPROVE");
            if (!strcmp(cases[i], "off")) setenv("TOTK_LIFT_PROBE", "0", 1);
            if (!strcmp(cases[i], "switches"))
                setenv("TOTK_QUALITY_REDUCTION", "off", 1), setenv("TOTK_GAME_FXAA", "off", 1), setenv("TOTK_LOD_IMPROVE", "1", 1),
                setenv("TOTK_SHADOW_RES", "2048", 1);
            fails = 0;
            run(cases[i]);
            fflush(stdout);
            _exit(fails != 0);
        }
        int st = 0;
        waitpid(p, &st, 0);
        if (!WIFEXITED(st) || WEXITSTATUS(st)) { printf("FAIL case %s\n", cases[i]); fails++; }
    }
    if (chdir("/tmp") || system("rm -rf /tmp/liftprobe.*")) {}
    printf(fails ? "lift probe: %d failed\n" : "lift probe: all ok\n", fails);
    return fails != 0;
}
