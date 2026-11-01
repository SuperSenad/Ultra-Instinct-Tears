/* [UIT 0070] The switches for the code tools/lifter/scan.py found in the player's own main at build time, and the
 * run-time check that picks the one shadow-size site among the candidates the scan could not tell apart.
 *
 *   TOTK_QUALITY_REDUCTION=off  the game no longer lowers its detail when the frame gets busy (main_quality_patch)
 *   TOTK_GAME_FXAA=off           the game's own FXAA flag reads 0 (main_fxaa_flag_patch); vkb_draw.c also copies the
 *                                FXAA pass's input through when the pass is still drawn
 *   TOTK_LOD_IMPROVE=1           the far-detail cut-off check is skipped (main_lod_patch)
 *   TOTK_SHADOW_RES=512..4096    the size the shadow-map setup is handed (main_shadow_patch, uit_display.c)
 *
 * Shadow probe: when the scan found more than one place that could be the shadow-map setup, each one is lifted with
 * a probe (totk_shadow_probe) that notes the size it passes. When the game then creates a square depth texture of
 * exactly that size within a second (uit_lift_texture, from nvnTextureInitialize), and none of that size was made in
 * the second before the probe (a texture made every frame anyway proves nothing), the place gets a hit. One place
 * with hits and no rival, and diag/lift-confirm.txt names it; the next setup run (the stamp covers the file) lifts
 * that place with the real patch. Nothing changes the game while probing: the probe only reads a register.
 * diag/lift-probe.txt shows what every candidate saw (About > Report a problem collects both). TOTK_LIFT_PROBE=0
 * turns the probe off. */
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "uit_str.h"   /* [UIT 0104] uit_stricmp: no <strings.h> on Windows */
#ifdef _WIN32
#include <direct.h>
#define MKDIR(p) _mkdir(p)
#else
#include <sys/stat.h>
#define MKDIR(p) mkdir(p, 0777)
#endif

#include "uit_display.h"

uint64_t runtime_now_ns(void);   /* host.h (kept out: this file builds alone in tools/tests/test_lift_probe.c) */
void uit_lift_init(void), uit_lift_texture(unsigned, unsigned, unsigned), uit_lift_exit(void);

#undef LOG
#define LOG(...) (fprintf(stderr, "[totk] lift: " __VA_ARGS__), fputc('\n', stderr))

extern const int main_quality_patch, main_fxaa_flag_patch, main_lod_patch, main_shadow_patch, main_shadow_probes;
extern const unsigned long long main_shadow_probe_sites[];
extern const char main_lift_build[];

static int off_value(const char *e) { return e && (!strcmp(e, "0") || !uit_stricmp(e, "off") || !uit_stricmp(e, "false") || !uit_stricmp(e, "no")); }
static int on_value(const char *e) { return e && (!strcmp(e, "1") || !uit_stricmp(e, "on") || !uit_stricmp(e, "true") || !uit_stricmp(e, "yes")); }

/* ------------------------------------------------------------- the switches (the lifted code calls these) */
static int g_quality = -1, g_fxaa = -1, g_lod = -1;
int totk_quality_reduction_off(void) {
    if (g_quality < 0) g_quality = main_quality_patch && off_value(getenv("TOTK_QUALITY_REDUCTION"));
    return g_quality;
}
int totk_game_fxaa_off(void) {
    if (g_fxaa < 0) g_fxaa = off_value(getenv("TOTK_GAME_FXAA"));
    return g_fxaa;
}
int totk_lod_improve(void) {
    if (g_lod < 0) g_lod = main_lod_patch && on_value(getenv("TOTK_LOD_IMPROVE"));
    return g_lod;
}
uint32_t totk_shadow_size(void) { return uit_shadow_res(); }

/* ------------------------------------------------------------- the probe */
#define PROBE_MAX 64
#define HOT_LIMIT 100000ull   /* a place this busy is not the once-per-setting setup */
typedef struct {
    _Atomic uint64_t fired, last_ns, sizes;   /* sizes: bit log2(v) of every size seen */
    _Atomic uint64_t before_ns;               /* when a depth texture of that size was last made before the probe */
    _Atomic uint32_t last;
    _Atomic uint32_t hits;
} Probe;
static Probe g_probe[PROBE_MAX];
volatile int totk_lift_probe;
static _Atomic int g_confirmed = -1;
static _Atomic uint64_t g_textures, g_written_ns, g_hit_ns;
static _Atomic uint64_t g_seen_ns[14];   /* the last square depth texture of size 1 << b */

static int probe_count(void) { return main_shadow_probes < PROBE_MAX ? main_shadow_probes : PROBE_MAX; }
static int pow2_size(uint32_t v) { return v >= 256 && v <= 8192 && !(v & (v - 1)); }

void totk_shadow_probe(int k, uint32_t v) {
    if (k < 0 || k >= probe_count()) return;
    Probe *p = &g_probe[k];
    uint64_t n = atomic_fetch_add(&p->fired, 1) + 1;
    if (n > HOT_LIMIT) return;
    if (!pow2_size(v)) return;
    atomic_fetch_or(&p->sizes, 1ull << __builtin_ctz(v));
    atomic_store(&p->last, v);
    atomic_store(&p->before_ns, atomic_load(&g_seen_ns[__builtin_ctz(v)]));
    atomic_store(&p->last_ns, runtime_now_ns());
}

static void write_probe(int final) {
    int n = probe_count();
    if (!n) return;
    MKDIR("diag");
    FILE *f = fopen("diag/lift-probe.txt", "w");
    if (!f) return;
    fprintf(f, "# Ultra Instinct Tears: the shadow-size candidates the lift probed (src/host/uit_lift.c)%s\n",
            final ? "" : " (written while the game ran)");
    fprintf(f, "build %s\nsquare depth textures seen %llu\n", main_lift_build, (unsigned long long)atomic_load(&g_textures));
    int c = atomic_load(&g_confirmed);
    if (c >= 0) fprintf(f, "confirmed %llx\n", main_shadow_probe_sites[c]);
    else fprintf(f, "confirmed none yet\n");
    for (int k = 0; k < n; k++) {
        Probe *p = &g_probe[k];
        uint64_t fired = atomic_load(&p->fired), sizes = atomic_load(&p->sizes);
        fprintf(f, "candidate %llx fired %llu%s hits %u sizes", main_shadow_probe_sites[k], (unsigned long long)fired,
                fired > HOT_LIMIT ? " (too often: not the setup)" : "", atomic_load(&p->hits));
        if (!sizes) fprintf(f, " none");
        for (int b = 8; b <= 13; b++) if (sizes >> b & 1) fprintf(f, " %u", 1u << b);
        fputc('\n', f);
    }
    fclose(f);
}

static void confirm(int k) {
    int none = -1;
    if (!atomic_compare_exchange_strong(&g_confirmed, &none, k)) return;
    totk_lift_probe = 0;
    MKDIR("diag");
    FILE *f = fopen("diag/lift-confirm.txt", "a");
    if (f) {
        fprintf(f, "shadow %llx build %s value %u hits %u\n", main_shadow_probe_sites[k], main_lift_build,
                atomic_load(&g_probe[k].last), atomic_load(&g_probe[k].hits));
        fclose(f);
    }
    LOG("the shadow-map setup is the candidate at %llx (the game's own size %u, %u matching textures): written to "
        "diag/lift-confirm.txt; run tools/setup.py again so Graphics > Detail and pop-in > Shadow resolution can be used",
        main_shadow_probe_sites[k], atomic_load(&g_probe[k].last), atomic_load(&g_probe[k].hits));
    write_probe(0);
}

/* the decision: 2 or more hits, and no rival with half as many */
static int decide(int at_exit) {
    int n = probe_count(), best = -1, with = 0;
    uint32_t b = 0, second = 0;
    for (int k = 0; k < n; k++) {
        uint32_t h = atomic_load(&g_probe[k].hits);
        if (h) with++;
        if (h > b) second = b, b = h, best = k;
        else if (h > second) second = h;
    }
    if (best < 0) return -1;
    if (b >= 2 && second * 2 < b) return best;
    if (at_exit && with == 1 && b == 1 && atomic_load(&g_probe[best].fired) == 1) return best;
    return -1;
}

int uit_lift_decide_for_test(int at_exit) { return decide(at_exit); }

void uit_lift_texture(unsigned w, unsigned h, unsigned fmt) {
    if (!totk_lift_probe) return;
    if (w != h || !pow2_size(w) || fmt < 0x32 || fmt > 0x36) return;   /* DEPTH16 .. DEPTH32F_STENCIL8 */
    atomic_fetch_add(&g_textures, 1);
    uint64_t now = runtime_now_ns();
    atomic_store(&g_seen_ns[__builtin_ctz(w)], now);
    for (int k = 0; k < probe_count(); k++) {
        Probe *p = &g_probe[k];
        uint64_t t = atomic_load(&p->last_ns);
        if (!t || atomic_load(&p->fired) > HOT_LIMIT || atomic_load(&p->last) != w || now - t > 1000000000ull) continue;
        /* only a size that was new after the probe: one made every frame anyway proves nothing */
        uint64_t before = atomic_load(&p->before_ns);
        if (before && t - before < 1000000000ull) { atomic_store(&p->last_ns, 0); continue; }
        if (!atomic_compare_exchange_strong(&p->last_ns, &t, 0)) continue;   /* each probe event counts once */
        atomic_fetch_add(&p->hits, 1);
        atomic_store(&g_hit_ns, now);
    }
    /* a lone single hit settles two minutes later as it would at exit (the window's close skips atexit) */
    uint64_t hit = atomic_load(&g_hit_ns);
    int k = decide(hit && now - hit > 120000000000ull);
    if (k >= 0) confirm(k);
    else if (now - atomic_load(&g_written_ns) > 10000000000ull) atomic_store(&g_written_ns, now), write_probe(0);
}

/* at exit (window_sdl.c before it closes, atexit otherwise): settle a lone hit, write what was seen */
void uit_lift_exit(void) {
    static _Atomic int done;
    if (!main_shadow_probes || atomic_exchange(&done, 1)) return;
    if (totk_lift_probe) { int k = decide(1); if (k >= 0) confirm(k); }
    write_probe(1);
}
static void lift_exit(void) { uit_lift_exit(); }

/* ------------------------------------------------------------- start-up */
void uit_lift_init(void) {
    const char *q = getenv("TOTK_QUALITY_REDUCTION"), *x = getenv("TOTK_GAME_FXAA"), *l = getenv("TOTK_LOD_IMPROVE");
    unsigned s = uit_shadow_res_setting();
    LOG("build %s: quality-reduction site %s, FXAA flag %s, far-detail (LOD) site %s, shadow-size site %s (%d candidates)",
        *main_lift_build ? main_lift_build : "?", main_quality_patch ? "found" : "not found",
        main_fxaa_flag_patch ? "found" : "not found", main_lod_patch ? "found" : "not found",
        main_shadow_patch ? "confirmed" : main_shadow_probes ? "probing" : "not found", main_shadow_probes);
    if (off_value(q)) LOG("%s", main_quality_patch ? "quality reduction off (TOTK_QUALITY_REDUCTION=off)"
                                             : "TOTK_QUALITY_REDUCTION=off: the site is not in this build, the game's own behaviour");
    if (off_value(x)) LOG("%s", main_fxaa_flag_patch ? "game FXAA off: its flag reads 0, and any FXAA pass still drawn is copied through"
                                               : "game FXAA off: the flag is not in this build; any FXAA pass drawn is copied through (experimental)");
    if (on_value(l)) LOG("%s", main_lod_patch ? "improve LOD on (TOTK_LOD_IMPROVE=1)"
                                        : "TOTK_LOD_IMPROVE=1: the site is not in this build, the game's own LOD");
    if (s) {
        if (main_shadow_patch) LOG("shadow resolution %u (TOTK_SHADOW_RES), %llu MiB more texture heap", s,
                                   (unsigned long long)(uit_shadow_extra_kib_for(s) / 1024));
        else LOG("TOTK_SHADOW_RES=%u: the shadow-size site is not confirmed in this build, the game's own size%s", s,
                 main_shadow_probes ? " (the probe is looking for it: play a minute outdoors, then run setup again)" : "");
    }
    const char *pe = getenv("TOTK_LIFT_PROBE");
    totk_lift_probe = main_shadow_probes > 0 && !off_value(pe);
    if (main_shadow_probes > 0 && !totk_lift_probe) LOG("shadow probe off (TOTK_LIFT_PROBE=0)");
    if (totk_lift_probe) atexit(lift_exit);
}
