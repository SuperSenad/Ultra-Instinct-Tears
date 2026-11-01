// [UIT 0204] Box test of vkb_polyoff.h: depth bias by polygon mode.
// cc -Wall -Werror -Isrc/gfx tools/tests/test_poly_offset.c -o /tmp/t && /tmp/t
#include <stdio.h>
#include "vkb_polyoff.h"
static int fails;
#define CHECK(c, m) do { if (!(c)) { fails++; printf("FAIL: %s\n", m); } } while (0)
int main(void) {
    for (unsigned en = 0; en < 8; en++) {   // FILL (the default mode): identical to before for every enable mask
        CHECK(vkb_bias_on(1, en, 2, 1) == vkb_bias_on(1, en, 2, 0), "fill mode unchanged");
        CHECK(vkb_bias_on(1, en, 2, 1) == ((en & 4) != 0), "fill uses the FILL bit");
        CHECK(vkb_bias_on(1, en, 1, 1) == ((en & 2) != 0), "line mode uses the LINE bit");
        CHECK(vkb_bias_on(1, en, 0, 1) == ((en & 1) != 0), "point mode uses the POINT bit");
        CHECK(vkb_bias_on(1, en, 1, 0) == ((en & 4) != 0), "off: the FILL bit (as before)");
        CHECK(!vkb_bias_on(0, en, 2, 1), "no polygon state: no bias");
        CHECK(vkb_bias_on(1, en, 3, 1) == ((en & 4) != 0), "an unknown mode: as before");
    }
    printf(fails ? "FAIL\n" : "ok\n");
    return fails != 0;
}
