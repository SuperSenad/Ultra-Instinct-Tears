// [UIT 0302] the benchmark's numbers and its report (src/host/uit_bench.h)
#include "uit_bench.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int cmp_f(const void *a, const void *b) {
    float x = *(const float *)a, y = *(const float *)b;
    return x < y ? -1 : x > y;
}

static double pct(const float *s, int n, double p) {   // nearest rank: the smallest value with at least p of them <= it
    int k = (int)ceil(p * n) - 1;
    return s[k < 0 ? 0 : k >= n ? n - 1 : k];
}

// the average frame rate of the slowest share of frames (at least one): frames / their time
static double low_fps(const float *s, int n, double share) {
    int k = (int)floor(n * share);
    if (k < 1) k = 1;
    double t = 0;
    for (int i = n - k; i < n; i++) t += s[i];
    return t > 0 ? 1000.0 * k / t : 0;
}

int uit_bench_stats(const float *ms, int n, float *sorted, UitBenchStats *st) {
    memset(st, 0, sizeof *st);
    if (n < 1) return 0;
    double sum = 0;
    for (int i = 0; i < n; i++) {
        float v = ms[i];
        sorted[i] = v;
        sum += v;
        st->over_16_7 += v > 1000.0f / 60.0f;
        st->over_33_3 += v > 1000.0f / 30.0f;
    }
    qsort(sorted, (size_t)n, sizeof *sorted, cmp_f);
    st->frames = n;
    st->seconds = sum / 1000.0;
    st->avg_ms = sum / n;
    st->avg_fps = sum > 0 ? 1000.0 * n / sum : 0;
    st->min_ms = sorted[0];
    st->max_ms = sorted[n - 1];
    st->max_fps = st->min_ms > 0 ? 1000.0 / st->min_ms : 0;
    st->min_fps = st->max_ms > 0 ? 1000.0 / st->max_ms : 0;
    st->p50_ms = pct(sorted, n, 0.50);
    st->p90_ms = pct(sorted, n, 0.90);
    st->p95_ms = pct(sorted, n, 0.95);
    st->p99_ms = pct(sorted, n, 0.99);
    st->p999_ms = pct(sorted, n, 0.999);
    st->low1_fps = low_fps(sorted, n, 0.01);
    st->low01_fps = low_fps(sorted, n, 0.001);
    return 1;
}

typedef struct { char *b; size_t n, at; } Out;
static void put(Out *o, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    char *dst = o->at < o->n ? o->b + o->at : NULL;
    int k = vsnprintf(dst, dst ? o->n - o->at : 0, fmt, ap);
    va_end(ap);
    if (k > 0) o->at += (size_t)k;
}

int uit_bench_summary(char *buf, size_t n, const UitBenchStats *st) {
    return snprintf(buf, n, "%.1f FPS average, 1%% low %.1f, 0.1%% low %.1f", st->avg_fps, st->low1_fps, st->low01_fps);
}

int uit_bench_report(char *buf, size_t n, const UitBenchStats *st, const UitBenchInfo *in) {
    Out o = {buf, n, 0};
    if (n) buf[0] = 0;
    put(&o, "Ultra Instinct Tears benchmark\n");
    put(&o, "Build:          %s (commit %s)\n", in->version ? in->version : "?", in->commit ? in->commit : "?");
    put(&o, "Started:        %s\n", in->started ? in->started : "?");
    if (in->gpu && in->gpu[0]) put(&o, "GPU:            %s\n", in->gpu);
    put(&o, "Length:         %.1f s of %d s%s\n", st->seconds, in->requested_s, in->stopped_early ? " (stopped early)" : "");
    put(&o, "Frames:         %d\n", st->frames);
    if (in->lost) put(&o, "Not recorded:   %d frames (the game window was busy; the numbers cover the rest)\n", in->lost);
    if (in->background)
        put(&o, "Note:           the game window was in the background for part of the run (Performance > Frame rate >\n"
                "                Frame rate in the background limits it then): run it again in front for a fair result\n");
    if (in->warm && in->warm[0]) put(&o, "Note:           %s\n", in->warm);   // [UIT 0384]
    put(&o, "\nFrame rate\n");
    put(&o, "  Average:      %.1f FPS\n", st->avg_fps);
    put(&o, "  1%% low:       %.1f FPS\n", st->low1_fps);
    put(&o, "  0.1%% low:     %.1f FPS\n", st->low01_fps);
    put(&o, "  Minimum:      %.1f FPS (slowest frame)\n", st->min_fps);
    put(&o, "  Maximum:      %.1f FPS (fastest frame)\n", st->max_fps);
    put(&o, "\nFrame time\n");
    put(&o, "  Average:      %.2f ms\n", st->avg_ms);
    put(&o, "  Minimum:      %.2f ms\n", st->min_ms);
    put(&o, "  50th pct:     %.2f ms\n", st->p50_ms);
    put(&o, "  90th pct:     %.2f ms\n", st->p90_ms);
    put(&o, "  95th pct:     %.2f ms\n", st->p95_ms);
    put(&o, "  99th pct:     %.2f ms\n", st->p99_ms);
    put(&o, "  99.9th pct:   %.2f ms\n", st->p999_ms);
    put(&o, "  Maximum:      %.2f ms\n", st->max_ms);
    double pa = st->frames ? 100.0 * st->over_16_7 / st->frames : 0, pb = st->frames ? 100.0 * st->over_33_3 / st->frames : 0;
    put(&o, "  > 16.7 ms:    %d frames (%.1f%%; slower than 60 FPS)\n", st->over_16_7, pa);
    put(&o, "  > 33.3 ms:    %d frames (%.1f%%; slower than 30 FPS)\n", st->over_33_3, pb);
    if (in->live && in->live[0]) put(&o, "\nIn effect during the run\n%s", in->live);
    put(&o, "\nSettings at start (ultrainstincttears.cfg; everything else at its default)\n");
    int any = 0;
    for (const char *const *e = in->env; e && *e; e++, any = 1) put(&o, "  %s\n", *e);
    if (!any) put(&o, "  (none: all defaults)\n");
    if (in->csv_name) put(&o, "\nEvery frame's time: %s\n", in->csv_name);
    put(&o, "1%% / 0.1%% low: the average frame rate of the slowest 1%% / 0.1%% of frames.\n");
    return (int)o.at;
}

int uit_bench_csv(char *buf, size_t n, const float *ms, int count) {
    Out o = {buf, n, 0};
    if (n) buf[0] = 0;
    put(&o, "frame,time_ms,elapsed_s\n");
    double t = 0;
    for (int i = 0; i < count; i++) {
        t += ms[i];
        put(&o, "%d,%.3f,%.4f\n", i + 1, ms[i], t / 1000.0);
    }
    return (int)o.at;
}

void uit_bench_name(char *buf, size_t n, int y, int mo, int d, int h, int mi, int s) {
    snprintf(buf, n, "%04d-%02d-%02d-%02d%02d%02d", y, mo, d, h, mi, s);
}
