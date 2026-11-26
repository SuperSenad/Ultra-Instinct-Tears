/* [UIT 0399] bufcat (src/runtime/bufcat.h): pieces appended up to the end of the buffer, never past it; a guard byte
 * after the buffer stays untouched however many pieces follow the cut.
 *   cc -O1 -Wall -Wextra -Werror tools/tests/test_bufcat.c -o /tmp/tbc && /tmp/tbc */
#include <stdio.h>
#include <string.h>
#include "../../src/runtime/bufcat.h"

static int fails;
#define CHECK(c, w) do { if (!(c)) { printf("FAIL %s\n", w); fails++; } } while (0)

int main(void) {
    struct { char b[16]; char guard[16]; } s;
    memset(&s, 'G', sizeof s);
    size_t at = 0;
    at = bufcat(s.b, sizeof s.b, at, "%s", "abc");
    CHECK(at == 3 && !strcmp(s.b, "abc"), "a piece that fits");
    at = bufcat(s.b, sizeof s.b, at, " %d", 12345);
    CHECK(at == 9 && !strcmp(s.b, "abc 12345"), "a second piece");
    at = bufcat(s.b, sizeof s.b, at, " %s", "this one is cut off");
    CHECK(at == 15 && !strcmp(s.b, "abc 12345 this "), "a cut piece: at stops at size - 1");
    for (int i = 0; i < 100; i++) at = bufcat(s.b, sizeof s.b, at, " more %d", i);
    CHECK(at == 15 && !strcmp(s.b, "abc 12345 this "), "pieces after the cut write nothing");
    int g = 1; for (size_t i = 0; i < sizeof s.guard; i++) g &= s.guard[i] == 'G';
    CHECK(g, "nothing written past the buffer");
    at = bufcat(s.b, sizeof s.b, 400, "x");
    CHECK(at == 15 && s.b[15] == 0, "at past the end (from older code): clamped");
    char one[1] = {'x'};
    CHECK(bufcat(one, 1, 0, "abc") == 0 && one[0] == 0, "a one-byte buffer holds the end only");
    CHECK(bufcat(one, 0, 0, "abc") == 0, "size 0: nothing");
    char e[8];
    at = bufcat(e, sizeof e, 0, "%s", "");
    CHECK(at == 0 && e[0] == 0, "an empty piece");
    at = bufcat(e, sizeof e, 0, "1234567");
    CHECK(at == 7 && !strcmp(e, "1234567"), "exactly full");
    printf("test_bufcat: %s\n", fails ? "FAILED" : "ok");
    return fails != 0;
}
