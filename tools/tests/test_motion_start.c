/* [UIT 0398] motion_fusion.c runtime_motion_start: reads right after a (re)start return no samples from before it,
 * the first sample after it starts the angle at zero; TOTK_GYRO_RESTART=0 keeps the old behaviour (the old samples are
 * still read in the frame of the start).
 *   clang -O1 -Wall -Isrc/host -Isrc/runtime -Ibuild tools/tests/test_motion_start.c src/host/motion_fusion.c -lm -lpthread -o /tmp/tms && /tmp/tms && TOTK_GYRO_RESTART=0 /tmp/tms old */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "host.h"

static uint64_t now = 1000000000ull;
uint64_t runtime_now_ns(void) { return now; }
void runtime_hid_feedback_stats(uint64_t *v, uint64_t *r) { *v = *r = 0; }
int plat_sleep_ns_precise(uint64_t ns) { (void)ns; return 0; }   /* [UIT 0454] the mouse sample thread (not started here) */
_Noreturn void runtime_fail(const char *m) { printf("fail: %s\n", m); _Exit(2); }
static int fails;
#define CHECK(c, w) do { if (!(c)) { printf("FAIL %s\n", w); fails++; } } while (0)

int main(int argc, char **argv) {
    int old = argc > 1 && !strcmp(argv[1], "old");
    const double acc[3] = {0, 0, -1}, spin[3] = {0, 0, 6.283185307179586};   /* 1 rev/s about z */
    RuntimeMotionState s[16];
    runtime_motion_live(1);
    for (int i = 0; i < 50; i++) { now += 5000000; runtime_motion_feed(acc, spin, now); }   /* 0.25 s: angle z ~0.25 rev */
    int n = runtime_motion_read(s, 16);
    CHECK(n == 16 && s[0].angle[2] > 0.2f, "samples before the start are read");
    uint64_t before = s[0].sampling;
    runtime_motion_start();
    n = runtime_motion_read(s, 16);
    if (old) CHECK(n == 16, "old: the previous session's samples are still read in the frame of the start");
    else CHECK(n == 0, "no sample from before the start is read");
    now += 5000000; runtime_motion_feed(acc, spin, now);
    n = runtime_motion_read(s, 16);
    if (!old) {
        CHECK(n == 1, "one sample after the start");
        CHECK(s[0].angle[2] >= 0 && s[0].angle[2] < 0.01f, "the angle starts over at zero");
        CHECK(s[0].sampling > before, "sampling numbers go on");
    } else CHECK(n == 16 && s[0].angle[2] < 0.01f, "old: fresh frame at the next sample, older samples still listed");
    printf("test_motion_start (%s): %s\n", old ? "TOTK_GYRO_RESTART=0" : "default", fails ? "FAILED" : "ok");
    return fails != 0;
}
