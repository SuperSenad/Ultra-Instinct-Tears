/* [UIT 0067] Box test of src/host/fps_unlock.c's frame step: measured (the default) follows the real time between
 * presents; TOTK_FRAME_STEP=fixed keeps exactly 1/TOTK_FPS whatever the frames take.
 *   cc -O1 tools/tests/test_frame_step.c -o /tmp/t/fstep && /tmp/t/fstep && /tmp/t/fstep fixed */
#include "../../src/host/fps_unlock.c"

int main(int argc, char **argv) {
    int fixed = argc > 1 && !strcmp(argv[1], "fixed"), fails = 0;
    setenv("TOTK_FPS", "60", 1);
    if (fixed) setenv("TOTK_FRAME_STEP", "fixed", 1); else unsetenv("TOTK_FRAME_STEP");
    if (totk_fps_target() != 60 || fabsf(totk_frame_step[1] - 1 / 60.0f) > 1e-7f) { printf("FAIL start step\n"); fails++; }
    uint64_t t = 1000000000ull;
    for (int i = 0; i < 20; i++) runtime_frame_step_update(t += 22222222);   /* 45 FPS */
    float want = fixed ? 1 / 60.0f : 0.022222222f;
    if (fabsf(totk_frame_step[1] - want) > 1e-6f || fabsf(totk_frame_step[0] - want * 30) > 1e-5f || fabsf(totk_frame_step[2] - want * 60) > 1e-5f) {
        printf("FAIL step %g, wanted %g\n", totk_frame_step[1], want); fails++;
    }
    printf(fails ? "frame step (%s): failed\n" : "frame step (%s): all ok\n", fixed ? "fixed" : "measured");
    return fails;
}
