/* [UIT 0301-0303] Box test of the frame-time ring (src/gfx/uit_frametime.c: nothing recorded without a reader, take /
 * recent, a reader that falls behind, a writer thread racing a reader), the background cap's setting
 * (uit_bg_parse / set_fps / set_away -> g_uit_bg_period_ns) and the benchmark's numbers and report (src/host/uit_bench.c).
 *   cc -O1 -Wall -pthread tools/tests/test_bench.c src/gfx/uit_frametime.c src/host/uit_bench.c -lm -o test_bench && ./test_bench */
#include "../../src/gfx/uit_frametime.h"
#include "../../src/host/uit_bench.h"

#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)
static int near(double a, double b, double eps) { return fabs(a - b) <= eps; }

static void reset_ring(void) {
    atomic_store(&g_uit_ft_users, 0);
    atomic_store(&g_uit_ft_head, 0);
}

static void test_ring(void) {
    reset_ring();
    uit_ft_push(5);   // no reader: nothing recorded
    CHECK(atomic_load(&g_uit_ft_head) == 0, "recorded without a reader");
    uit_ft_use(1);
    for (int i = 1; i <= 10; i++) uit_ft_push((float)i);
    CHECK(atomic_load(&g_uit_ft_head) == 10, "head %llu", (unsigned long long)atomic_load(&g_uit_ft_head));
    float out[8192];
    uint64_t since = 0, lost = 9;
    int n = uit_ft_take(&since, out, 4, &lost);   // first take: the newest 4
    CHECK(n == 4 && out[0] == 7 && out[3] == 10 && since == 10 && lost == 0, "first take n=%d", n);
    for (int i = 11; i <= 13; i++) uit_ft_push((float)i);
    n = uit_ft_take(&since, out, 100, &lost);
    CHECK(n == 3 && out[0] == 11 && out[2] == 13 && lost == 0, "next take n=%d", n);
    n = uit_ft_take(&since, out, 100, &lost);
    CHECK(n == 0, "nothing new n=%d", n);
    // fell far behind: what the game thread overwrote (or is about to) is counted as lost
    for (int i = 0; i < 5000; i++) uit_ft_push(1000.0f + (float)i);
    n = uit_ft_take(&since, out, 8192, &lost);
    CHECK(n == (int)UIT_FT_SAFE && lost == 5000 - UIT_FT_SAFE && out[n - 1] == 5999.0f, "behind n=%d lost=%llu", n, (unsigned long long)lost);
    // a reader with less room: the newest that fit, the rest lost
    for (int i = 0; i < 50; i++) uit_ft_push((float)i);
    n = uit_ft_take(&since, out, 20, &lost);
    CHECK(n == 20 && lost == 30 && out[19] == 49 && out[0] == 30, "small n=%d lost=%llu", n, (unsigned long long)lost);
    // recent: the newest frames covering the span, oldest first
    for (int i = 0; i < 200; i++) uit_ft_push(10.0f);
    n = uit_ft_recent(3000.0f, out, 512);
    CHECK(n == 229 && out[0] == 21 && out[28] == 49 && out[n - 1] == 10, "recent: %d", n);   // 2 s of 10 ms, then 49 + 48 + .. + 21
    for (int i = 0; i < 400; i++) uit_ft_push(10.0f);
    n = uit_ft_recent(3000.0f, out, 512);
    CHECK(n == 300, "recent 3 s of 10 ms frames: %d", n);
    n = uit_ft_recent(3000.0f, out, 64);
    CHECK(n == 64, "recent capped: %d", n);
    uit_ft_use(0);
    uit_ft_push(1);
    CHECK(atomic_load(&g_uit_ft_head) == 10 + 3 + 5000 + 50 + 600, "stopped recording");
    // a fresh ring: recent with fewer frames than the span
    reset_ring();
    uit_ft_use(1);
    uit_ft_push(16.6f); uit_ft_push(40.0f);
    n = uit_ft_recent(3000.0f, out, 512);
    CHECK(n == 2 && out[0] == 16.6f && out[1] == 40.0f, "short recent %d", n);
    uit_ft_use(0);
}

// the game thread pushes 1, 2, 3, ... while the reader takes: every value it gets is in order and nothing is skipped
// unless reported lost
static void *pusher(void *arg) {
    int n = *(int *)arg;
    for (int i = 1; i <= n; i++) uit_ft_push((float)i);
    return NULL;
}
static void test_race(void) {
    reset_ring();
    uit_ft_use(1);
    int total = 2000000;
    pthread_t t;
    pthread_create(&t, NULL, pusher, &total);
    static float out[UIT_FT_RING];
    uint64_t since = atomic_load(&g_uit_ft_head), lost_all = 0, got = 0, pending = 0;
    float last = 0;
    int bad = 0;
    for (;;) {
        uint64_t lost = 0;
        int n = uit_ft_take(&since, out, (int)UIT_FT_RING, &lost);
        lost_all += lost;
        pending += lost;   // a take can drop all it copied: the gap shows at the next one that gets frames
        for (int i = 0; i < n; i++) {
            if (out[i] != last + 1.0f + (i == 0 ? (float)pending : 0.0f)) bad++;
            last = out[i];
        }
        if (n) pending = 0;
        got += (uint64_t)n;
        if (last >= (float)total) break;
    }
    pthread_join(t, NULL);
    CHECK(!bad && got + lost_all == (uint64_t)total, "race: bad %d got %llu lost %llu", bad, (unsigned long long)got, (unsigned long long)lost_all);
    uit_ft_use(0);
}

static void test_bg(void) {
    CHECK(uit_bg_parse(NULL) == 30 && uit_bg_parse("-") == 30 && uit_bg_parse("") == 30, "default 30");
    CHECK(uit_bg_parse("0") == 0 && uit_bg_parse("15") == 15 && uit_bg_parse("60") == 60 && uit_bg_parse("999") == 240, "parse");
    uit_bg_set_fps(30);
    CHECK(atomic_load(&g_uit_bg_period_ns) == 0, "in front: no cap");
    uit_bg_set_away(1);
    CHECK(atomic_load(&g_uit_bg_period_ns) == 33333333, "away 30: %llu", (unsigned long long)atomic_load(&g_uit_bg_period_ns));
    uit_bg_set_fps(15);
    CHECK(atomic_load(&g_uit_bg_period_ns) == 66666666, "away 15 (changed live)");
    uit_bg_set_fps(0);
    CHECK(atomic_load(&g_uit_bg_period_ns) == 0, "Off: no cap even away");
    uit_bg_set_fps(60);
    CHECK(atomic_load(&g_uit_bg_period_ns) == 16666666 && uit_bg_away(), "away 60");
    uit_bg_set_away(0);
    CHECK(atomic_load(&g_uit_bg_period_ns) == 0 && !uit_bg_away() && uit_bg_fps() == 60, "back in front: no cap at once");
}

static void test_stats(void) {
    UitBenchStats st;
    float sorted[2000], ms[2000] = {0};
    CHECK(!uit_bench_stats(ms, 0, sorted, &st) && st.frames == 0, "empty");
    // 1000 frames: 990 of 10 ms, 9 of 20 ms, 1 of 50 ms
    for (int i = 0; i < 1000; i++) ms[i] = 10.0f;
    for (int i = 0; i < 9; i++) ms[100 + i * 50] = 20.0f;
    ms[777] = 50.0f;
    CHECK(uit_bench_stats(ms, 1000, sorted, &st), "stats");
    double sum = 990 * 10 + 9 * 20 + 50;
    CHECK(st.frames == 1000 && near(st.seconds, sum / 1000, 1e-6), "frames / seconds %f", st.seconds);
    CHECK(near(st.avg_fps, 1000.0 * 1000 / sum, 1e-6), "avg %f", st.avg_fps);
    CHECK(near(st.min_ms, 10, 1e-6) && near(st.max_ms, 50, 1e-6) && near(st.min_fps, 20, 1e-6) && near(st.max_fps, 100, 1e-6), "min/max");
    CHECK(near(st.p50_ms, 10, 1e-6) && near(st.p99_ms, 10, 1e-6) && near(st.p999_ms, 20, 1e-6), "pct %f %f", st.p99_ms, st.p999_ms);
    CHECK(near(st.low1_fps, 1000.0 * 10 / (9 * 20 + 50), 1e-6), "1%% low %f", st.low1_fps);   // slowest 10 frames
    CHECK(near(st.low01_fps, 20, 1e-6), "0.1%% low %f", st.low01_fps);                       // the slowest one
    CHECK(st.over_16_7 == 10 && st.over_33_3 == 1, "counts %d %d", st.over_16_7, st.over_33_3);
    CHECK(ms[777] == 50.0f, "input untouched");
    // exactly 16.7 ms / 33.4 ms boundaries: 16.6 is not slow, 16.7 is (> 1000/60 = 16.667)
    float b[4] = {16.6f, 16.7f, 33.3f, 33.4f};
    uit_bench_stats(b, 4, sorted, &st);
    CHECK(st.over_16_7 == 3 && st.over_33_3 == 1, "boundaries %d %d", st.over_16_7, st.over_33_3);
    // a single frame
    float one = 8.0f;
    uit_bench_stats(&one, 1, sorted, &st);
    CHECK(near(st.avg_fps, 125, 1e-6) && near(st.low1_fps, 125, 1e-6) && near(st.p50_ms, 8, 1e-6), "one frame");
}

static void test_report(void) {
    float ms[300], sorted[300];
    for (int i = 0; i < 300; i++) ms[i] = i % 100 == 0 ? 40.0f : 16.0f;
    UitBenchStats st;
    uit_bench_stats(ms, 300, sorted, &st);
    const char *env[] = {"TOTK_FPS=60", "TOTK_UPSCALER=2", NULL};
    UitBenchInfo in = {"1.0.1", "abc1234", "2026-11-01 14:05:09", "Test GPU", 60, 1, 7, 1,
                       "  Window:       1920x1080 pixels, full screen\n", env, "2026-11-01-140509.csv"};
    int need = uit_bench_report(NULL, 0, &st, &in);
    char *r = malloc((size_t)need + 1);
    int got = uit_bench_report(r, (size_t)need + 1, &st, &in);
    CHECK(got == need && (int)strlen(r) == need, "length %d %d", got, need);
    const char *want[] = {"Build:          1.0.1 (commit abc1234)", "GPU:            Test GPU", "(stopped early)", "Frames:         300",
                          "Not recorded:   7 frames", "in the background", "Average:", "1% low:", "0.1% low:", "99.9th pct:",
                          "> 16.7 ms:    3 frames", "> 33.3 ms:    3 frames", "1920x1080", "  TOTK_FPS=60\n", "TOTK_UPSCALER=2",
                          "2026-11-01-140509.csv"};
    for (size_t i = 0; i < sizeof want / sizeof *want; i++) CHECK(strstr(r, want[i]) != NULL, "report lacks \"%s\"", want[i]);
    // a small buffer: cut, NUL-terminated, the needed length returned
    char small[50];
    CHECK(uit_bench_report(small, sizeof small, &st, &in) == need && strlen(small) == sizeof small - 1, "truncated");
    free(r);
    in.env = NULL; in.lost = 0; in.background = 0; in.gpu = NULL; in.live = NULL;
    char r2[4096];
    uit_bench_report(r2, sizeof r2, &st, &in);
    CHECK(strstr(r2, "(none: all defaults)") && !strstr(r2, "Not recorded") && !strstr(r2, "GPU:") && !strstr(r2, "background"), "plain");
    // csv
    float c[3] = {16.5f, 17.25f, 33.25f};
    char csv[256];
    int k = uit_bench_csv(csv, sizeof csv, c, 3);
    CHECK(k == (int)strlen(csv) && !strcmp(csv, "frame,time_ms,elapsed_s\n1,16.500,0.0165\n2,17.250,0.0338\n3,33.250,0.0670\n"), "csv:\n%s", csv);
    CHECK(uit_bench_csv(NULL, 0, c, 3) == k, "csv length");
    char name[40], sum[160];
    uit_bench_name(name, sizeof name, 2026, 11, 1, 9, 5, 7);
    CHECK(!strcmp(name, "2026-11-01-090507"), "name %s", name);
    uit_bench_summary(sum, sizeof sum, &st);
    CHECK(strstr(sum, "FPS average, 1% low") != NULL, "summary %s", sum);
}

int main(void) {
    test_ring();
    test_race();
    test_bg();
    test_stats();
    test_report();
    printf("%s: %d checks, %d failed\n", fails ? "FAILED" : "ok", checks, fails);
    return fails != 0;
}
