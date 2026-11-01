// [UIT 0302] The benchmark (window thread): Overlay > Performance > Start benchmark, or Shift+F8. It records the game's
// frame times (src/gfx/uit_frametime.h) for Game > In-game overlay > Benchmark length (TOTK_BENCH_SECONDS, 10..300,
// default 60) and writes benchmarks/<date>-<time>.txt (the report) and .csv (every frame) next to the exe, from a
// thread of its own. Nothing is recorded while none runs. Every function: window (SDL main) thread only.
#pragma once
#include <SDL3/SDL.h>
#include <stdint.h>

void uit_bench_init(SDL_Window *w);
int uit_bench_toggle(void);                // starts one, or stops the running one early (it is still saved); 1 = running
int uit_bench_running(void);
int uit_bench_seconds(void);
void uit_bench_set_seconds(int s);         // the next run's length (10..300)
void uit_bench_poll(uint64_t now_ms);      // each window loop: takes the new frames, ends the run when it is long enough
// What the overlay shows: "Benchmarking 42 s" while running, then the result for a few seconds; NULL: nothing.
const char *uit_bench_text(uint64_t now_ms, const char **line2);
