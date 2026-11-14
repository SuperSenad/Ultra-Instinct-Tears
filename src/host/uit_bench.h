// [UIT 0302] The benchmark's numbers and its report (pure C: no SDL, no threads; tools/tests/test_bench.c).
// src/host/bench_live.c records the frame times (src/gfx/uit_frametime.h) and calls these on its writer thread.
#pragma once
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int frames;
    double seconds;                    // the frame times added up
    double avg_fps;                    // frames / seconds
    double low1_fps, low01_fps;        // 1% / 0.1% lows: the average frame rate of the slowest 1% / 0.1% of frames
    double min_fps, max_fps;           // from the slowest / fastest frame
    double avg_ms, min_ms, max_ms;
    double p50_ms, p90_ms, p95_ms, p99_ms, p999_ms;   // frame time percentiles (nearest rank)
    int over_16_7, over_33_3;          // frames slower than 60 / 30 FPS
} UitBenchStats;

// ms: the frame times in order; sorted: scratch for n floats (left sorted). 0 when n < 1.
int uit_bench_stats(const float *ms, int n, float *sorted, UitBenchStats *st);

typedef struct {
    const char *version, *commit;      // the build
    const char *started;               // "2026-11-01 14:05:09"
    const char *gpu;                   // may be NULL
    int requested_s;                   // the length asked for
    int stopped_early;                 // stopped by hand
    int lost;                          // frames not recorded (the window thread fell far behind)
    int background;                    // the window lost the focus during the run (the background frame-rate cap)
    const char *live;                  // what was in effect when it ran, "Name: value\n" lines (may be NULL)
    const char *const *env;            // the TOTK_* settings at start, "KEY=value" (NULL-terminated, may be NULL)
    const char *csv_name;              // the frame time file's name, for the reader
    const char *warm;                  // [UIT 0384] the shader warm-up was running: a note (NULL or "": none)
} UitBenchInfo;

// The .txt report. Returns the length it needs (as snprintf: write again with a bigger buffer when it is >= n).
int uit_bench_report(char *buf, size_t n, const UitBenchStats *st, const UitBenchInfo *in);
// The .csv: "frame,time_ms,elapsed_s" then one line a frame. Returns the length it needs (as snprintf).
int uit_bench_csv(char *buf, size_t n, const float *ms, int count);
// A one-line summary for the overlay ("87.4 FPS average, 1% low 61.2")
int uit_bench_summary(char *buf, size_t n, const UitBenchStats *st);
// benchmarks/<name>: "2026-11-01-140509" from the local date and time
void uit_bench_name(char *buf, size_t n, int year, int month, int day, int hour, int minute, int second);

#ifdef __cplusplus
}
#endif
