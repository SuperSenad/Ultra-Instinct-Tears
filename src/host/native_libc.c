// [UIT 0086] The C library's memory and string routines on the PC's own (TOTK_NATIVE_LIBC, default off).
//
// tools/lifter/native_libc.py gives every SDK function exported as memcpy, memmove, memset, memcmp, memchr, strlen,
// strnlen, strcmp, strncmp, strchr, strrchr, strcpy or strncpy a front that calls uit_nl_call() when uit_nl_mode is not
// 0 (the lifted routine is passed along). Guest addresses are host addresses, so the PC routine works on the game's
// memory directly. The contract kept (see native_libc.py): the C standard's results in x0 (32-bit int for the compares,
// the sign exact, the value the difference of the first differing bytes), memcpy done as memmove, no other register
// changed (AAPCS64 lets the guest routine change x1..x18 and v0..v7, so no caller relies on them).
//   TOTK_NATIVE_LIBC=1      the PC routines (Performance > CPU+ > PC versions of memory and text routines)
//   TOTK_NATIVE_LIBC=check  the game's routines run as before; each call's result is also worked out the PC's way and
//                           every difference is logged ("[totk] libc: ..."), a few lines per routine
//   unset / 0               the game's routines (the front costs one load and branch per call)
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../runtime/cpu.h"   /* [UIT 0104] relative, like the other host files (cc rule: -Ibuild only) */

typedef void (*GuestRoutine)(Cpu *c);
void uit_nl_call(Cpu *c, int i, GuestRoutine guest);
void uit_nl_stats(uint64_t *calls, uint64_t *diffs);
#define LOG(...) (fprintf(stderr, "[totk] libc: " __VA_ARGS__), fputc('\n', stderr))

enum { NL_MEMCPY, NL_MEMMOVE, NL_MEMSET, NL_MEMCMP, NL_MEMCHR, NL_STRLEN, NL_STRNLEN, NL_STRCMP, NL_STRNCMP, NL_STRCHR,
       NL_STRRCHR, NL_STRCPY, NL_STRNCPY, NL_N };
// The order of tools/lifter/native_libc.py NAMES (tools/tests/test_native_libc.py compares them)
const char *const uit_nl_names[NL_N] = {"memcpy", "memmove", "memset", "memcmp", "memchr", "strlen", "strnlen", "strcmp",
                                        "strncmp", "strchr", "strrchr", "strcpy", "strncpy"};

int uit_nl_mode = -1;   // -1 not read yet, 0 off, 1 on, 2 check (read once, at the first call of any of them)
static uint64_t n_calls, n_diffs, n_diff[NL_N], n_overlap;   // (statistics; racy counts are fine)

static void *P(uint64_t a) { return (void *)(uintptr_t)a; }
static uint64_t A(const void *p) { return (uint64_t)(uintptr_t)p; }
static int bytecmp(const unsigned char *a, const unsigned char *b, size_t n) {   // memcmp with the byte difference
    for (size_t i = 0; i < n; i++) if (a[i] != b[i]) return (int)a[i] - (int)b[i];
    return 0;
}
static int strcmp_d(const unsigned char *a, const unsigned char *b, size_t n) {   // strncmp with the byte difference
    for (size_t i = 0; i < n; i++) { if (a[i] != b[i]) return (int)a[i] - (int)b[i]; if (!a[i]) return 0; }
    return 0;
}

// The PC's answer for routine i on the arguments x0..x2; the destination is written for the copying routines
static uint64_t pc_routine(int i, uint64_t x0, uint64_t x1, uint64_t x2) {
    switch (i) {
    case NL_MEMCPY: case NL_MEMMOVE: return A(memmove(P(x0), P(x1), (size_t)x2));
    case NL_MEMSET: return A(memset(P(x0), (int)(uint8_t)x1, (size_t)x2));
    case NL_MEMCMP: {   // the C library's memcmp finds the first difference fast; the difference comes from those bytes
        size_t n = (size_t)x2; const unsigned char *a = P(x0), *b = P(x1);
        if (!n || !memcmp(a, b, n)) return 0;
        while (*a == *b) a++, b++;   // (memcmp said they differ within n)
        return (uint32_t)((int)*a - (int)*b);
    }
    case NL_MEMCHR: return A(memchr(P(x0), (int)(uint8_t)x1, (size_t)x2));
    case NL_STRLEN: return strlen(P(x0));
    case NL_STRNLEN: return strnlen(P(x0), (size_t)x1);
    case NL_STRCMP: case NL_STRNCMP: {
        size_t n = i == NL_STRCMP ? (size_t)-1 : (size_t)x2;
        int r = i == NL_STRCMP ? strcmp(P(x0), P(x1)) : strncmp(P(x0), P(x1), n);
        return r ? (uint32_t)strcmp_d(P(x0), P(x1), n) : 0;
    }
    case NL_STRCHR: return A(strchr(P(x0), (int)(char)(uint8_t)x1));
    case NL_STRRCHR: return A(strrchr(P(x0), (int)(char)(uint8_t)x1));
    case NL_STRCPY: return A(strcpy(P(x0), P(x1)));
    case NL_STRNCPY: return A(strncpy(P(x0), P(x1), (size_t)x2));
    }
    return 0;
}

static int is_cmp(int i) { return i == NL_MEMCMP || i == NL_STRCMP || i == NL_STRNCMP; }
static int sgn(int32_t v) { return (v > 0) - (v < 0); }
static void diff(int i, const char *what, uint64_t x0, uint64_t x1, uint64_t x2, uint64_t game, uint64_t pc) {
    __atomic_fetch_add(&n_diffs, 1, __ATOMIC_RELAXED);
    uint64_t k = __atomic_fetch_add(&n_diff[i], 1, __ATOMIC_RELAXED);
    if (k < 4 || !(k & (k + 1)))   // the first few, then at 7, 15, 31, ...
        LOG("check: %s(%#llx, %#llx, %#llx): %s: game %#llx, PC %#llx (%llu differences in %s so far)", uit_nl_names[i],
            (unsigned long long)x0, (unsigned long long)x1, (unsigned long long)x2, what, (unsigned long long)game,
            (unsigned long long)pc, (unsigned long long)k + 1, uit_nl_names[i]);
}

// Check mode: the game's routine runs (its result is the one kept); the PC's answer is worked out alongside. The
// copying routines are checked after the game's run, from what the destination holds (only when source and
// destination do not overlap); the others before, on the same memory (another thread changing it in between would
// show as a difference: rare, and logged as one).
static void check_call(Cpu *c, int i, GuestRoutine guest) {
    uint64_t x0 = c->x[0], x1 = c->x[1], x2 = c->x[2];
    if (i == NL_MEMCPY || i == NL_MEMMOVE || i == NL_MEMSET || i == NL_STRCPY || i == NL_STRNCPY) {
        guest(c);
        uint64_t r = c->x[0];
        if (r != x0) diff(i, "returned pointer", x0, x1, x2, r, x0);
        const unsigned char *d = P(x0), *s = P(x1);
        if (i == NL_MEMSET) {
            for (uint64_t k = 0; k < x2; k++) if (d[k] != (uint8_t)x1) { diff(i, "byte at offset", x0, x1, x2, k, (uint8_t)x1); break; }
        } else if (i == NL_MEMCPY || i == NL_MEMMOVE) {
            if (x0 + x2 <= x1 || x1 + x2 <= x0) {
                if (bytecmp(d, s, (size_t)x2)) diff(i, "copied bytes", x0, x1, x2, 1, 0);
            } else if (i == NL_MEMCPY && x0 != x1 && x2) {   // the PC version copies these as memmove; the game's may not
                uint64_t k = __atomic_fetch_add(&n_overlap, 1, __ATOMIC_RELAXED);
                if (k < 4 || !(k & (k + 1)))
                    LOG("check: memcpy(%#llx, %#llx, %#llx) with overlapping source and destination (%llu so far): the PC "
                        "version copies these as memmove does; if this shows up, keep TOTK_NATIVE_LIBC off",
                        (unsigned long long)x0, (unsigned long long)x1, (unsigned long long)x2, (unsigned long long)k + 1);
            }
        } else {
            uint64_t n = strlen((const char *)s), want = i == NL_STRCPY ? n + 1 : x2;
            uint64_t sn = n + 1 < want ? n + 1 : want;   // source bytes read
            if (x0 + want <= x1 || x1 + sn <= x0) {
                int bad = bytecmp(d, s, (size_t)sn) != 0;
                for (uint64_t k = sn; !bad && k < want; k++) bad = d[k] != 0;
                if (bad) diff(i, "copied bytes", x0, x1, x2, 1, 0);
            }
        }
        return;
    }
    uint64_t pc = pc_routine(i, x0, x1, x2);
    guest(c);
    uint64_t g = c->x[0];
    if (is_cmp(i)) {
        if (sgn((int32_t)g) != sgn((int32_t)pc)) diff(i, "sign", x0, x1, x2, (uint32_t)g, pc);
        else if ((uint32_t)g != (uint32_t)pc) diff(i, "value (same sign)", x0, x1, x2, (uint32_t)g, pc);
    } else if (g != pc) diff(i, "result", x0, x1, x2, g, pc);
}

static int env_mode(void) {
    const char *e = getenv("TOTK_NATIVE_LIBC");
    if (!e || !*e || !strcmp(e, "0") || !strcmp(e, "off")) return 0;
    return !strcmp(e, "check") ? 2 : 1;
}

void uit_nl_call(Cpu *c, int i, GuestRoutine guest) {
    int m = __atomic_load_n(&uit_nl_mode, __ATOMIC_RELAXED);
    if (m < 0) {
        m = env_mode();
        if (__atomic_exchange_n(&uit_nl_mode, m, __ATOMIC_RELAXED) < 0 && m)
            LOG("%s", m == 2 ? "check mode (TOTK_NATIVE_LIBC=check): the game's memory and text routines, results compared"
                             : "memory and text routines on the PC's (TOTK_NATIVE_LIBC)");
    }
    if (m == 0 || (unsigned)i >= NL_N) { guest(c); return; }
    __atomic_fetch_add(&n_calls, 1, __ATOMIC_RELAXED);
    if (m == 2) { check_call(c, i, guest); return; }
    uint64_t r = pc_routine(i, c->x[0], c->x[1], c->x[2]);
    c->x[0] = is_cmp(i) ? (uint32_t)r : r;
}

void uit_nl_stats(uint64_t *calls, uint64_t *diffs) { *calls = n_calls; *diffs = n_diffs; }
