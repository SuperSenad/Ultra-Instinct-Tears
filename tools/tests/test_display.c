/* [native-1031] Box test of TOTK_OUTPUT_RES=display (src/host/uit_display.c): the main display's size becomes the canvas,
 * a 1080p display is the game's own, a display outside 1280x720..7680x4320 or of unknown size falls back to 1080p.
 * Off Windows the display size comes from UIT_TEST_DISPLAY, read once (as the real one is), so each case is a new process.
 *   cc -O1 -Wall tools/tests/test_display.c src/host/uit_display.c -lm -o /tmp/test_display && /tmp/test_display */
#include "../../src/host/uit_display.h"

#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>

const int main_output_res_patch = 1, main_aspect_patch = 1, main_shadow_patch = 1;

static int one(const char *display, int want_on, unsigned want_w, unsigned want_h, double want_aspect) {
    pid_t p = fork();
    if (!p) {
        if (display) setenv("UIT_TEST_DISPLAY", display, 1); else unsetenv("UIT_TEST_DISPLAY");
        setenv("TOTK_OUTPUT_RES", "display", 1);
        unsetenv("TOTK_ASPECT");
        unsigned w = 0, h = 0, sw = 0, sh = 0;
        int on = uit_output_res(&w, &h), on2 = uit_output_res_setting(&sw, &sh);
        int ok = on == want_on && on2 == want_on && (!on || (w == want_w && h == want_h && sw == w && sh == h));
        double a = uit_picture_aspect();
        ok = ok && (a > want_aspect - 1e-3 && a < want_aspect + 1e-3);
        if (on) ok = ok && (uit_heap_extra_kib() > 0) == ((double)w * h > 1920.0 * 1080.0);   /* grown only when bigger */
        _exit(ok ? 0 : 1);
    }
    int st = 0;
    waitpid(p, &st, 0);
    int ok = WIFEXITED(st) && WEXITSTATUS(st) == 0;
    printf("%s display %s\n", ok ? "ok  " : "FAIL", display ? display : "(unknown)");
    return ok;
}

int main(void) {
    int ok = 1;
    ok &= one("3840x2160", 1, 3840, 2160, 0);              /* 4K: the canvas, 16:9 */
    ok &= one("2560x1440", 1, 2560, 1440, 0);
    ok &= one("3440x1440", 1, 3440, 1440, 3440.0 / 1440);  /* ultrawide: the shape follows */
    ok &= one("1920x1080", 0, 0, 0, 0);                    /* the game's own */
    ok &= one("1366x768", 1, 1366, 768, 0);                /* within 1 % of 16:9 */
    ok &= one("1024x768", 0, 0, 0, 0);                     /* too small: 1080p */
    ok &= one("15360x8640", 0, 0, 0, 0);                   /* too large: 1080p */
    ok &= one(NULL, 0, 0, 0, 0);                           /* unknown: 1080p */
    printf(ok ? "test_display: all passed\n" : "test_display: FAILED\n");
    return !ok;
}
