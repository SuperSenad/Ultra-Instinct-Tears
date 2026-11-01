/* [UIT 0104] ASCII case-insensitive compare for setting values ("off", "On", "2160p", ...), in place of POSIX strcasecmp
 * from <strings.h>: the Windows toolchain (the builder package's trimmed MinGW headers) has no <strings.h>, and
 * setting values are ASCII, so no locale is involved. Same result sign convention as strcmp. */
#ifndef UIT_STR_H
#define UIT_STR_H

static inline int uit_lower(int c) { return c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c; }
static inline int uit_stricmp(const char *a, const char *b) {
    const unsigned char *x = (const unsigned char *)a, *y = (const unsigned char *)b;
    for (;; x++, y++) {
        int d = uit_lower(*x) - uit_lower(*y);
        if (d || !*x) return d;
    }
}
#endif
