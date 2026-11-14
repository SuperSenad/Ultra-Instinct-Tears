// [UIT 0302] the benchmark (src/host/bench_live.h). The game thread only pushes frame times into the ring
// (uit_ft_push); this side copies them out on the window thread (at most every 100 ms) into a buffer allocated when the
// run starts, and a detached thread computes the numbers and writes the files, so no file I/O and no allocation happen
// on the game, replay or present threads.
#include "bench_live.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "uit_bench.h"
#include "uit_live.h"
#include "../gfx/uit_frametime.h"
#include "../vk/dlss.h"
#include "../vk/present.h"
#include "../vk/vk.h"
#include "version.h"   // build/: ULTRS_VERSION, ULTRS_COMMIT

#ifdef _WIN32
#define SEP "\\"
#else
#define SEP "/"
#endif

static SDL_Window *win;
static int seconds = 60;
static int running, saving, background;
static float *buf;
static int cap, count;
static uint64_t since, lost, started_ms;
static double sum_ms;
static char started[32], name[40];
static char ind[64], res[160], res2[200];
static uint64_t res_until;
static SDL_Mutex *mx;              // res / res2 / saving from the writer thread
static char done[160], done2[200];
static int done_new;

static int clamp_s(int s) { return s < 10 ? 10 : s > 300 ? 300 : s; }

void uit_bench_init(SDL_Window *w) {
    win = w;
    const char *e = SDL_getenv("TOTK_BENCH_SECONDS");
    if (e && *e && *e != '-') seconds = clamp_s(atoi(e));
    if (!mx) mx = SDL_CreateMutex();
}
int uit_bench_seconds(void) { return seconds; }
void uit_bench_set_seconds(int s) { seconds = clamp_s(s); }
int uit_bench_running(void) { return running; }

typedef struct {
    float *ms;
    int n, requested, early, lost, background;
    char started[32], name[40], live[768], gpu[256];
    char warm[320];   // [UIT 0384]
} Job;

static char *cfg_lines(void) {   // the TOTK_* lines of the settings file but the bindings, as "KEY=value\0" ... "\0" (NULL: none)
    const char *p = uit_cfg_path();
    size_t n = 0;
    char *t = p && *p ? (char *)SDL_LoadFile(p, &n) : NULL;
    if (!t) return NULL;
    char *out = (char *)SDL_malloc(n + 2), *o = out;
    for (char *l = t; l && *l;) {
        char *e = strpbrk(l, "\r\n");
        size_t k = e ? (size_t)(e - l) : strlen(l);
        if (k > 5 && !strncmp(l, "TOTK_", 5) && strncmp(l + 5, "PAD_", 4) && strncmp(l + 5, "KEY_", 4) && memchr(l, '=', k)) {
            memcpy(o, l, k); o += k; *o++ = 0;
        }
        l = e ? e + strspn(e, "\r\n") : NULL;
    }
    *o = 0;
    SDL_free(t);
    return out;
}

static int write_file(const char *path, const char *data, size_t n) {
    SDL_IOStream *f = SDL_IOFromFile(path, "wb");
    if (!f) return 0;
    int ok = SDL_WriteIO(f, data, n) == n;
    return SDL_CloseIO(f) && ok;
}

static int SDLCALL writer(void *arg) {
    Job *j = (Job *)arg;
    float *sorted = (float *)SDL_malloc(sizeof(float) * (size_t)(j->n > 0 ? j->n : 1));
    UitBenchStats st;
    uit_bench_stats(j->ms, j->n, sorted, &st);
    SDL_free(sorted);
    char *cfg = cfg_lines();
    const char *envv[160];
    int ne = 0;
    for (char *c = cfg; c && *c && ne < 159; c += strlen(c) + 1) envv[ne++] = c;
    envv[ne] = NULL;
    char csv_name[64];
    snprintf(csv_name, sizeof csv_name, "%s.csv", j->name);
    UitBenchInfo in = {ULTRS_VERSION, ULTRS_COMMIT, j->started, j->gpu, j->requested, j->early, j->lost, j->background,
                       j->live, envv, csv_name, j->warm};
    const char *base = SDL_GetBasePath();
    char dir[1024], path[1100];
    snprintf(dir, sizeof dir, "%sbenchmarks", base ? base : "");
    SDL_CreateDirectory(dir);
    int ok = 1;
    int n = uit_bench_report(NULL, 0, &st, &in);
    char *t = (char *)SDL_malloc((size_t)n + 1);
    uit_bench_report(t, (size_t)n + 1, &st, &in);
    snprintf(path, sizeof path, "%s" SEP "%s.txt", dir, j->name);
    ok &= write_file(path, t, (size_t)n);
    SDL_free(t);
    n = uit_bench_csv(NULL, 0, j->ms, j->n);
    t = (char *)SDL_malloc((size_t)n + 1);
    uit_bench_csv(t, (size_t)n + 1, j->ms, j->n);
    snprintf(path, sizeof path, "%s" SEP "%s.csv", dir, j->name);
    ok &= write_file(path, t, (size_t)n);
    SDL_free(t);
    SDL_free(cfg);
    char sum[160];
    uit_bench_summary(sum, sizeof sum, &st);
    fprintf(stderr, "[bench] %d frames, %.1f s: %s; %s %s" SEP "%s.txt\n", st.frames, st.seconds, sum,
            ok ? "saved" : "could not save", dir, j->name);
    SDL_LockMutex(mx);
    snprintf(done, sizeof done, "Benchmark: %s", sum);
    if (ok) snprintf(done2, sizeof done2, "Saved benchmarks" SEP "%s.txt%s%s", j->name, j->background ? " (was in the background)" : "",
                     j->warm[0] ? " (shader warm-up was running)" : "");
    else snprintf(done2, sizeof done2, "Could not save it in the benchmarks folder (see the log)");
    done_new = 1;
    SDL_UnlockMutex(mx);
    SDL_free(j->ms);
    SDL_free(j);
    return 0;
}

static void live_state(Job *j) {
    int w = 0, h = 0;
    if (win) SDL_GetWindowSizeInPixels(win, &w, &h);
    int fs = win && (SDL_GetWindowFlags(win) & SDL_WINDOW_FULLSCREEN);
    const char *up = dlss_active() ? "DLSS" : vkp_fsr_wanted() ? "FSR 1" : "Off";
    char upm[64] = "";
    if (dlss_active()) snprintf(upm, sizeof upm, " (%s)", dlss_mode_name(dlss_mode()));
    snprintf(j->live, sizeof j->live,
             "  Window:       %dx%d pixels%s\n"
             "  Upscaler:     %s%s\n"
             "  FPS target:   %d (started at %d)\n"
             "  Present mode: %s\n",
             w, h, fs ? ", full screen" : ", windowed", up, upm, totk_fps_target(), totk_fps_start(), vkp_present_mode_name());
    snprintf(j->gpu, sizeof j->gpu, "%s", vkc.props.deviceName);
}

// [UIT 0384] TOTK_BENCH_WARM_NOTE (on unless =0): a benchmark started while the shader warm-up is still making shaders
// says so (the log, the overlay while it runs, a Note: line in the .txt): its compiles compete with the game's and its
// waits, the build-4 report's 17 hitches came from it with the graphics driver's cache cold, and a run then is not
// comparable with one after the warm-up ("shaders made ahead" in the log).
int vkb_shader_warm_left(size_t *left, size_t *total);   // gfx/vkb_shader.c
static int warm_at_start;
static size_t warm_left0, warm_total0;
static int warm_note_on(void) { const char *e = getenv("TOTK_BENCH_WARM_NOTE"); return !(e && *e == '0'); }
static void warm_note(Job *j) {
    j->warm[0] = 0;
    if (!warm_at_start) return;
    size_t left = 0, total = 0;
    int still = vkb_shader_warm_left(&left, &total);
    char end[96];
    if (still) snprintf(end, sizeof end, "and at the end (%zu left)", left);
    else snprintf(end, sizeof end, "and finished during the run");
    snprintf(j->warm, sizeof j->warm,
             "the shader warm-up was still running when it started (%zu of %zu shaders left) %s:\n"
             "                its shader builds can cause hitches; for a fair result wait for \"shaders made ahead\"\n"
             "                in the log, then run it again", warm_left0, warm_total0, end);
}

static void finish(int early) {
    running = 0;
    uit_ft_use(0);
    if (count < 1) {
        SDL_free(buf); buf = NULL;
        snprintf(res, sizeof res, "Benchmark stopped: no frames yet");
        res2[0] = 0;
        res_until = SDL_GetTicks() + 5000;
        return;
    }
    Job *j = (Job *)SDL_calloc(1, sizeof *j);
    j->ms = buf; buf = NULL;
    j->n = count; j->requested = seconds; j->early = early; j->lost = (int)lost; j->background = background;
    memcpy(j->started, started, sizeof started);
    memcpy(j->name, name, sizeof name);
    live_state(j);
    warm_note(j);   // [UIT 0384]
    saving = 1;
    SDL_Thread *t = SDL_CreateThread(writer, "uit-bench", j);
    if (t) SDL_DetachThread(t);
    else { writer(j); }
}

int uit_bench_toggle(void) {
    if (running) { finish(1); return 0; }
    if (saving) return 0;   // the last one is still being written (well under a second)
    cap = seconds * 500 + 1024;   // up to 500 FPS
    buf = (float *)SDL_malloc(sizeof(float) * (size_t)cap);
    if (!buf) return 0;
    count = 0; lost = 0; sum_ms = 0;
    background = uit_bg_away();
    SDL_Time now = 0;
    SDL_DateTime dt = {0};
    SDL_GetCurrentTime(&now);
    SDL_TimeToDateTime(now, &dt, true);
    snprintf(started, sizeof started, "%04d-%02d-%02d %02d:%02d:%02d", dt.year, dt.month, dt.day, dt.hour, dt.minute, dt.second);
    uit_bench_name(name, sizeof name, dt.year, dt.month, dt.day, dt.hour, dt.minute, dt.second);
    uit_ft_use(1);
    since = atomic_load_explicit(&g_uit_ft_head, memory_order_acquire);
    started_ms = SDL_GetTicks();
    running = 1;
    res_until = 0;
    fprintf(stderr, "[bench] started: %d s\n", seconds);
    warm_at_start = warm_note_on() && vkb_shader_warm_left(&warm_left0, &warm_total0);   // [UIT 0384]
    if (warm_at_start)
        fprintf(stderr, "[bench] warning: the shader warm-up is still running (%zu of %zu shaders left): its shader builds can "
                "cause hitches; for a fair result wait for \"shaders made ahead\" in the log, then run it again\n",
                warm_left0, warm_total0);
    return 1;
}

void uit_bench_poll(uint64_t now) {
    if (saving) {
        SDL_LockMutex(mx);
        if (done_new) {
            done_new = 0; saving = 0;
            memcpy(res, done, sizeof res); memcpy(res2, done2, sizeof res2);
            res_until = now + 10000;
        }
        SDL_UnlockMutex(mx);
    }
    if (!running) return;
    if (uit_bg_away()) background = 1;
    uint64_t l = 0;
    int room = cap - count;
    int n = uit_ft_take(&since, buf + count, room > 0 ? room : 0, &l);
    lost += l;
    double goal = seconds * 1000.0;
    for (int i = 0; i < n; i++) {
        if (sum_ms >= goal) break;
        sum_ms += buf[count++];
    }
    if (sum_ms >= goal || count >= cap) { finish(0); return; }
    if (now - started_ms > (uint64_t)seconds * 3000 + 30000) finish(1);   // the game stopped sending frames: end it
}

const char *uit_bench_text(uint64_t now, const char **line2) {
    *line2 = NULL;
    if (running) {
        int left = (int)((seconds * 1000.0 - sum_ms + 999) / 1000);
        snprintf(ind, sizeof ind, "Benchmarking %d s", left < 0 ? 0 : left);
        *line2 = warm_at_start ? "Shader warm-up still running: may hitch (Shift+F8 stops it)" : "Shift+F8 stops it";   // [UIT 0384]
        return ind;
    }
    if (saving) return "Saving the benchmark...";
    if (res_until && now < res_until) { *line2 = res2[0] ? res2 : NULL; return res; }
    return NULL;
}
