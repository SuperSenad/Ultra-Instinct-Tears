// [UIT 0362] One log line, written whole: the prefix, the text and the line end formatted into one buffer and handed to
// stderr in a single fwrite, so a line takes the stream lock once. The LOG macros before 0362 were an fprintf and then a
// separate fputc('\n'): two locked writes, between which another thread's line could land (torn lines joined
// mid-text, and empty lines where two line ends met). Lines up to 1 KiB are formatted on the stack, longer ones on the
// heap (truncated to 1 KiB if that fails).
#pragma once
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

#if defined(__MINGW_PRINTF_FORMAT)
#define UIT_LOGLINE_FORMAT __MINGW_PRINTF_FORMAT
#else
#define UIT_LOGLINE_FORMAT printf
#endif

__attribute__((format(UIT_LOGLINE_FORMAT, 1, 2), unused)) static int uit_logline(const char *fmt, ...) {
    char stack[1024];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(stack, sizeof stack - 1, fmt, ap);   // room for the '\n'
    va_end(ap);
    if (n < 0) return n;
    char *buf = stack;
    if ((size_t)n >= sizeof stack - 1) {
        char *big = (char *)malloc((size_t)n + 2);
        if (big) {
            va_start(ap, fmt);
            vsnprintf(big, (size_t)n + 1, fmt, ap);
            va_end(ap);
            buf = big;
        } else {
            n = (int)sizeof stack - 2;   // what fit
        }
    }
    buf[n] = '\n';
    size_t w = fwrite(buf, 1, (size_t)n + 1, stderr);
    if (buf != stack) free(buf);
    return (int)w;
}
