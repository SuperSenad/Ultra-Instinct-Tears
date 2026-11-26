/* [UIT 0399] bufcat: formatted text appended to a fixed buffer, never past its end.
 *     at = bufcat(buf, sizeof buf, at, "...", ...);
 * takes the length so far and returns the new one, at most size - 1; text that does not fit is cut off, and later calls
 * write nothing. It replaces at += snprintf(buf + at, sizeof buf - at, ...): once one piece was cut off, at pointed
 * past the end and the next call's size wrapped around to a huge number, so it wrote past the buffer (long log lines:
 * many counter types, long stacks, many sampler slots). After upstream UltraTears 7730ac6 (kuraeow). */
#ifndef UIT_BUFCAT_H
#define UIT_BUFCAT_H
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>

#if defined(__GNUC__) || defined(__clang__)
__attribute__((format(printf, 4, 5)))
#endif
static inline size_t bufcat(char *buf, size_t size, size_t at, const char *fmt, ...) {
    if (!buf || !size) return 0;
    if (at >= size - 1) { buf[size - 1] = 0; return size - 1; }
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf + at, size - at, fmt, ap);
    va_end(ap);
    if (n < 0) { buf[at] = 0; return at; }
    return (size_t)n >= size - at ? size - 1 : at + (size_t)n;
}
#endif
