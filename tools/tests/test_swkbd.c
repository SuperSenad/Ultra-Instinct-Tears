/* [UIT 0177] Box test of the software keyboard's portable part (src/host/swkbd.c): ShowKeyboardArg parsing on
 * synthetic arguments built here (the layout is switchbrew's KeyboardConfig), the character filters, the length
 * limits, UTF-16 / UTF-8 output into the game's String and the cancel result.
 *   cc -O1 -Wall -Wextra tools/tests/test_swkbd.c src/host/swkbd.c -o /tmp/t/swkbd && /tmp/t/swkbd */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../src/host/swkbd.h"

static int fails, checks;
#define CHECK(c) do { checks++; if (!(c)) { fails++; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static void w16(uint8_t *a, size_t at, uint16_t v) { memcpy(a + at, &v, 2); }
static void w32(uint8_t *a, size_t at, uint32_t v) { memcpy(a + at, &v, 4); }
static void w64(uint8_t *a, size_t at, uint64_t v) { memcpy(a + at, &v, 8); }
static void wtext(uint8_t *a, size_t at, const char *s) { for (size_t i = 0; s[i]; i++) w16(a, at + i * 2, (uint8_t)s[i]); }

static uint8_t work[0x5000] __attribute__((aligned(16)));

/* The argument TotK passes naming a horse: full keyboard, at most 9, no @ % \, UTF-16, a header, the work buffer at
 * 0x518 with an initial name at offset 0. */
static void horse(uint8_t *a, uint32_t work_at, const char *initial) {
    memset(a, 0, UIT_SWKBD_ARG_SIZE);
    memset(work, 0, sizeof work);
    w32(a, 0x0, 8);
    w32(a, 0x1c, UIT_KBINV_AT | UIT_KBINV_PERCENT | UIT_KBINV_BACKSLASH);
    wtext(a, 0x24, "Name your horse");
    wtext(a, 0xa6, "Up to 9 characters");
    wtext(a, 0x1a8, "Horse name");
    w32(a, 0x3ac, 9);
    w32(a, 0x3b0, 0);
    w64(a, work_at, (uint64_t)(uintptr_t)work);
    w64(a, work_at + 8, sizeof work);
    if (initial) {
        w32(a, 0x3c0, 0x10);
        w32(a, 0x3c4, (uint32_t)strlen(initial));
        for (size_t i = 0; initial[i]; i++) w16(work, 0x10 + i * 2, (uint8_t)initial[i]);
    }
}

static int eq16(const uint16_t *t, const char *s) {
    size_t i = 0;
    for (; s[i]; i++) if (t[i] != (uint8_t)s[i]) return 0;
    return t[i] == 0;
}
static size_t u16(const char *s, uint16_t *out) { return uit_swkbd_utf8_to_utf16(s, strlen(s), out, 600); }

int main(void) {
    static uint8_t a[UIT_SWKBD_ARG_SIZE];
    UitSwkbdReq r;

    /* parsing */
    horse(a, 0x518, "Epona");
    uit_swkbd_parse(a, &r);
    CHECK(r.mode == 8);
    CHECK(r.max_len == 9 && r.min_len == 0);
    CHECK(r.invalid == 0x2c);
    CHECK(!r.utf8 && !r.password && !r.newline && !r.text_check);
    CHECK(eq16(r.header, "Name your horse"));
    CHECK(eq16(r.sub, "Up to 9 characters"));
    CHECK(eq16(r.guide, "Horse name"));
    CHECK(r.work_at == 0x518);
    CHECK(r.initial_len == 5 && eq16(r.initial, "Epona"));

    horse(a, 0x4d0, "Storm");   /* the older layout */
    uit_swkbd_parse(a, &r);
    CHECK(r.work_at == 0x4d0 && r.initial_len == 5 && eq16(r.initial, "Storm"));

    horse(a, 0x518, "ABCDEFGHIJKLMNOP");   /* an initial text longer than max is cut to max */
    uit_swkbd_parse(a, &r);
    CHECK(r.initial_len == 9);

    horse(a, 0x518, "a@b%c\\d");   /* the initial text goes through the filters */
    uit_swkbd_parse(a, &r);
    CHECK(r.initial_len == 4 && eq16(r.initial, "abcd"));

    horse(a, 0x518, NULL);
    w64(a, 0x520, 0x123);   /* not a size the SDK allocates: no work buffer, no initial text */
    uit_swkbd_parse(a, &r);
    CHECK(r.work_at == 0 && r.initial_len == 0);

    horse(a, 0x518, "Epona");
    w32(a, 0x3c0, sizeof work + 2);   /* the offset outside the work buffer */
    uit_swkbd_parse(a, &r);
    CHECK(r.initial_len == 0);

    memset(a, 0, sizeof a);   /* everything zero: unlimited (the applet's 500), nothing disabled */
    uit_swkbd_parse(a, &r);
    CHECK(r.max_len == UIT_SWKBD_LIMIT && r.min_len == 0 && r.mode == UIT_KBMODE_FULL);
    w32(a, 0x3ac, 100000); w32(a, 0x3b0, 7);
    uit_swkbd_parse(a, &r);
    CHECK(r.max_len == UIT_SWKBD_LIMIT && r.min_len == 7);
    w32(a, 0x3ac, 4); w32(a, 0x3b0, 9);   /* min above max: max */
    uit_swkbd_parse(a, &r);
    CHECK(r.max_len == 4 && r.min_len == 4);
    w32(a, 0x3b4, 1); a[0x3bc] = 1; a[0x3bd] = 1; a[0x3d0] = 1; w32(a, 0x20, 1);
    w16(a, 0x16, '-'); w16(a, 0x18, '.');
    wtext(a, 0x4, "Done");
    uit_swkbd_parse(a, &r);
    CHECK(r.password && r.newline && r.utf8 && r.text_check && r.cursor_start);
    CHECK(r.left_key == '-' && r.right_key == '.');
    CHECK(eq16(r.ok, "Done"));
    for (int i = 0; i < 0x40; i++) w16(a, 0x24 + i * 2, 'H');   /* a header with no NUL: cut at its field */
    uit_swkbd_parse(a, &r);
    CHECK(uit_swkbd_u16len(r.header, 0x41) == 0x40);

    /* the filters */
    horse(a, 0x518, NULL);
    uit_swkbd_parse(a, &r);
    CHECK(uit_swkbd_allowed(&r, 'a') && uit_swkbd_allowed(&r, ' ') && uit_swkbd_allowed(&r, '7'));
    CHECK(!uit_swkbd_allowed(&r, '@') && !uit_swkbd_allowed(&r, '%') && !uit_swkbd_allowed(&r, '\\'));
    CHECK(uit_swkbd_allowed(&r, '/'));
    CHECK(uit_swkbd_allowed(&r, 0xe9) && uit_swkbd_allowed(&r, 0x1f600));
    CHECK(!uit_swkbd_allowed(&r, '\n') && !uit_swkbd_allowed(&r, '\t') && !uit_swkbd_allowed(&r, 0x7f));
    CHECK(!uit_swkbd_allowed(&r, 0x85) && !uit_swkbd_allowed(&r, 0xd800) && !uit_swkbd_allowed(&r, 0xffff));
    r.newline = 1;
    CHECK(uit_swkbd_allowed(&r, '\n'));
    r.invalid = UIT_KBINV_SPACE | UIT_KBINV_SLASH | UIT_KBINV_NUMBERS;
    CHECK(!uit_swkbd_allowed(&r, ' ') && !uit_swkbd_allowed(&r, 0x3000) && !uit_swkbd_allowed(&r, '/'));
    CHECK(!uit_swkbd_allowed(&r, '5') && uit_swkbd_allowed(&r, '@'));
    r.invalid = 0;
    r.mode = UIT_KBMODE_NUMERIC; r.left_key = '-'; r.right_key = 0;
    CHECK(uit_swkbd_allowed(&r, '0') && uit_swkbd_allowed(&r, '9') && uit_swkbd_allowed(&r, '-'));
    CHECK(!uit_swkbd_allowed(&r, 'a') && !uit_swkbd_allowed(&r, ' ') && !uit_swkbd_allowed(&r, '\n') && !uit_swkbd_allowed(&r, 0));
    r.mode = UIT_KBMODE_ASCII;
    CHECK(uit_swkbd_allowed(&r, '~') && !uit_swkbd_allowed(&r, 0xe9));

    /* clean: drops what is not allowed, stops at max, never splits a pair */
    uint16_t t[600];
    horse(a, 0x518, NULL);
    uit_swkbd_parse(a, &r);
    size_t n = u16("Sh@dow%fax\\", t);
    n = uit_swkbd_clean(&r, t, n);
    CHECK(n == 8);
    t[n] = 0;
    CHECK(eq16(t, "Shdowfax"));
    n = u16("ABCDEFGHIJKL", t);
    CHECK(uit_swkbd_clean(&r, t, n) == 9);
    n = u16("12345678\xF0\x9F\x90\xB4", t);   /* 8 + a pair: 10 units, max 9 -> the pair is dropped whole */
    CHECK(n == 10);
    CHECK(uit_swkbd_clean(&r, t, n) == 8);
    n = u16("1234567\xF0\x9F\x90\xB4", t);    /* 7 + a pair fits */
    CHECK(uit_swkbd_clean(&r, t, n) == 9 && t[7] == 0xd83d && t[8] == 0xdc34);
    t[0] = 0xdc00; t[1] = 'a';                /* a lone low surrogate is dropped */
    CHECK(uit_swkbd_clean(&r, t, 2) == 1 && t[0] == 'a');

    /* the converters */
    n = u16("Caf\xC3\xA9 \xE2\x82\xAC\xF0\x9F\x90\xB4", t);
    CHECK(n == 8 && t[3] == 0xe9 && t[5] == 0x20ac && t[6] == 0xd83d && t[7] == 0xdc34);
    char s8[64];
    size_t w = uit_swkbd_utf16_to_utf8(t, n, s8, sizeof s8);
    CHECK(w == 13 && !memcmp(s8, "Caf\xC3\xA9 \xE2\x82\xAC\xF0\x9F\x90\xB4", 13));
    CHECK(uit_swkbd_utf16_to_utf8(t, n, s8, 12) == 9);   /* stops before the 4-byte char */
    CHECK(uit_swkbd_utf8_to_utf16("a\xFF" "b", 3, t, 10) == 3 && t[1] == 0xfffd);
    CHECK(uit_swkbd_utf8_to_utf16("\xE2\x82", 2, t, 10) == 1 && t[0] == 0xfffd);          /* cut short */
    CHECK(uit_swkbd_utf8_to_utf16("\xED\xA0\x80", 3, t, 10) == 1 && t[0] == 0xfffd);      /* an encoded surrogate */
    CHECK(uit_swkbd_utf8_to_utf16("\xE0\x80\x80", 3, t, 10) == 1 && t[0] == 0xfffd);      /* overlong */
    CHECK(uit_swkbd_utf8_to_utf16("ab\xF0\x9F\x90\xB4", 6, t, 3) == 2);                   /* a pair that does not fit */
    CHECK(uit_swkbd_utf8_to_utf16("ab\xF0\x9F\x90\xB4", 6, NULL, 0) == 4);                /* counting */

    /* finish: UTF-16 into the String */
    uint8_t out[64];
    horse(a, 0x518, NULL);
    uit_swkbd_parse(a, &r);
    n = u16("Epona", t);
    memset(out, 0xee, sizeof out);
    CHECK(uit_swkbd_finish(&r, 1, t, n, out, sizeof out) == 0);
    CHECK(!memcmp(out, "E\0p\0o\0n\0a\0\0\0", 12) && out[12] == 0xee);
    memset(out, 0xee, sizeof out);   /* an unaligned buffer */
    CHECK(uit_swkbd_finish(&r, 1, t, n, out + 1, 20) == 0 && !memcmp(out + 1, "E\0p\0o\0n\0a\0\0\0", 12));
    memset(out, 0xee, sizeof out);   /* a small one: cut, NUL-terminated */
    CHECK(uit_swkbd_finish(&r, 1, t, n, out, 8) == 0 && !memcmp(out, "E\0p\0o\0\0\0", 8));
    n = u16("ab\xF0\x9F\x90\xB4", t);   /* a pair that does not fit is not split */
    memset(out, 0xee, sizeof out);
    CHECK(uit_swkbd_finish(&r, 1, t, n, out, 8) == 0 && !memcmp(out, "a\0b\0\0\0", 6));
    n = u16("ABCDEFGHIJKL", t);         /* over max: max */
    memset(out, 0xee, sizeof out);
    CHECK(uit_swkbd_finish(&r, 1, t, n, out, sizeof out) == 0 && out[18] == 0 && out[19] == 0 && out[16] == 'I');
    /* cancel: 0x29f and an empty string */
    n = u16("Epona", t);
    memset(out, 0xee, sizeof out);
    CHECK(uit_swkbd_finish(&r, 0, t, n, out, sizeof out) == UIT_SWKBD_RESULT_CANCELED && out[0] == 0 && out[1] == 0);
    CHECK(UIT_SWKBD_RESULT_CANCELED == 0x29f);
    /* under min: a cancel */
    r.min_len = 6;
    CHECK(uit_swkbd_finish(&r, 1, t, n, out, sizeof out) == UIT_SWKBD_RESULT_CANCELED);
    r.min_len = 5;
    CHECK(uit_swkbd_finish(&r, 1, t, n, out, sizeof out) == 0);
    /* UTF-8 output */
    r.utf8 = 1; r.min_len = 0;
    n = u16("Caf\xC3\xA9", t);
    memset(out, 0xee, sizeof out);
    CHECK(uit_swkbd_finish(&r, 1, t, n, out, sizeof out) == 0 && !strcmp((char *)out, "Caf\xC3\xA9"));
    memset(out, 0xee, sizeof out);
    CHECK(uit_swkbd_finish(&r, 1, t, n, out, 5) == 0 && !strcmp((char *)out, "Caf"));   /* é does not fit in 4 */
    CHECK(uit_swkbd_finish(&r, 0, t, n, out, sizeof out) == UIT_SWKBD_RESULT_CANCELED && out[0] == 0);
    CHECK(uit_swkbd_finish(&r, 1, t, n, NULL, 0) == 0);   /* no buffer: only the result */
    r.utf8 = 0;
    out[0] = 0xee;
    CHECK(uit_swkbd_finish(&r, 1, t, n, out, 1) == 0 && out[0] == 0);   /* one byte: an empty string */

    printf("%s: %d checks, %d failures\n", fails ? "FAIL" : "ok", checks, fails);
    return fails != 0;
}
