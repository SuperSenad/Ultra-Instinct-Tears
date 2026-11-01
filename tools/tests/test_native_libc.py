"""Box test for the PC versions of the C library's memory and string routines (UIT 0086): tools/lifter/native_libc.py
picks the routines by exported function name and gives each a front; the front runs the lifted routine unchanged
unless TOTK_NATIVE_LIBC is on; src/host/native_libc.c gives the C standard's results on random and edge inputs
(compared with byte-by-byte reference routines that play the game's part, including the registers a guest routine may
change), changes only x0, and its check mode finds a routine that answers differently.
   python3 tools/tests/test_native_libc.py"""
import os, pathlib, re, subprocess, sys, tempfile
from collections import Counter
HERE = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(HERE / 'tools' / 'lifter'))
import native_libc as nl

ok = True
def check(c, w):
    global ok; print(('ok   ' if c else 'FAIL ') + w); ok &= bool(c)

csrc = (HERE / 'src/host/native_libc.c').read_text()
cnames = re.findall(r'"(\w+)"', re.search(r'uit_nl_names\[NL_N\] = \{(.*?)\};', csrc, re.S).group(1))
check(tuple(cnames) == nl.NAMES, f'the C side lists the same {len(nl.NAMES)} routines in the same order')

class Nso:   # (name, info, shndx, value, size)
    syms = [('memcpy', 0x12, 7, 0x1000, 64), ('strlen', 0x12, 7, 0x2000, 32), ('memset', 0x12, 0, 0, 0),
            ('strcmp', 0x11, 7, 0x3000, 8), ('memcpy_s', 0x12, 7, 0x4000, 8), ('_Z6strlenPKc', 0x12, 7, 0x5000, 8)]
got = nl.find(Nso, 0x805000000)
check(got == {0x805001000: 'memcpy', 0x805002000: 'strlen'},
      'only defined functions with exactly these names (an import, an object, other names: left alone)')
r = subprocess.run([sys.executable, '-c', 'import sys; sys.path.insert(0, sys.argv[1]); import native_libc as nl\n'
                    'class N: syms = [("memcpy", 0x12, 7, 0x10, 4)]\nprint(nl.find(N, 0x805000000))', str(HERE / 'tools/lifter')],
                   env=dict(os.environ, TOTK_LIFT_NATIVE_LIBC='0'), capture_output=True, text=True)
check(r.stdout.strip() == '{}', 'TOTK_LIFT_NATIVE_LIBC=0: no routine gets a front (the code converted exactly as before)')
fr = nl.front(['void f_805001000(Cpu *c) {', '    FN_ENTER(0x805001000ull);', '    return;', '}'], 'f_805001000', 'memcpy')
check(fr[1] == 'void f_805001000_lifted(Cpu *c) {' and fr[2] == '    FN_ENTER(0x805001000ull);'
      and any('uit_nl_call(c, 0, f_805001000_lifted)' in l for l in fr)
      and any('musttail)) return f_805001000_lifted(c);' in l for l in fr), 'the front: lifted code renamed and unchanged')
nl.STATS.clear()
check(nl.front(['/* odd */'], 'f_1', 'memcpy') == ['/* odd */'] and nl.STATS['skipped'] == 1, 'unexpected lines: left as they were')

# ---------------------------------------------------------------- the routines, run (Linux build of native_libc.c)
REF = r'''
#include <stdint.h>
#include <string.h>
#include "cpu.h"
static void clob(Cpu *c) { for (int i = 1; i <= 17; i++) c->x[i] ^= 0x5a5a0000ull + i; c->nzcv ^= 0xf0000000u; }
#define P(a) ((unsigned char *)(uintptr_t)(a))
static int BAD;   /* 1: memcmp gives only -1/0/1 (the PC's value differs, same sign); 2: strlen one short */
#define G(name) void ref_##name(Cpu *c)
G(memcpy) { for (uint64_t i = 0; i < c->x[2]; i++) P(c->x[0])[i] = P(c->x[1])[i]; clob(c); c->x[1] ^= 0; }
G(memmove) { unsigned char *d = P(c->x[0]), *s = P(c->x[1]); uint64_t n = c->x[2];
    if (d < s) for (uint64_t i = 0; i < n; i++) d[i] = s[i]; else for (uint64_t i = n; i--;) d[i] = s[i]; clob(c); }
G(memset) { for (uint64_t i = 0; i < c->x[2]; i++) P(c->x[0])[i] = (uint8_t)c->x[1]; clob(c); }
G(memcmp) { int r = 0; for (uint64_t i = 0; i < c->x[2]; i++) if (P(c->x[0])[i] != P(c->x[1])[i]) { r = P(c->x[0])[i] - P(c->x[1])[i]; break; }
    if (BAD == 1) r = (r > 0) - (r < 0); clob(c); c->x[0] = (c->x[0] & ~0xffffffffull) | (uint32_t)r; }
G(memchr) { uint64_t r = 0; for (uint64_t i = 0; i < c->x[2]; i++) if (P(c->x[0])[i] == (uint8_t)c->x[1]) { r = c->x[0] + i; break; } clob(c); c->x[0] = r; }
G(strlen) { uint64_t n = 0; while (P(c->x[0])[n]) n++; if (BAD == 2 && n) n--; clob(c); c->x[0] = n; }
G(strnlen) { uint64_t n = 0; while (n < c->x[1] && P(c->x[0])[n]) n++; clob(c); c->x[0] = n; }
static int scmp(const unsigned char *a, const unsigned char *b, uint64_t n) {
    for (uint64_t i = 0; i < n; i++) { if (a[i] != b[i]) return a[i] - b[i]; if (!a[i]) break; } return 0; }
G(strcmp) { int r = scmp(P(c->x[0]), P(c->x[1]), ~0ull); clob(c); c->x[0] = (c->x[0] & ~0xffffffffull) | (uint32_t)r; }
G(strncmp) { int r = scmp(P(c->x[0]), P(c->x[1]), c->x[2]); clob(c); c->x[0] = (c->x[0] & ~0xffffffffull) | (uint32_t)r; }
G(strchr) { unsigned char ch = (uint8_t)c->x[1]; uint64_t a = c->x[0], r = 0; for (;; a++) { if (*P(a) == ch) { r = a; break; } if (!*P(a)) break; } clob(c); c->x[0] = r; }
G(strrchr) { unsigned char ch = (uint8_t)c->x[1]; uint64_t a = c->x[0], r = 0; for (;; a++) { if (*P(a) == ch) r = a; if (!*P(a)) break; } clob(c); c->x[0] = r; }
G(strcpy) { uint64_t i = 0; do P(c->x[0])[i] = P(c->x[1])[i]; while (P(c->x[1])[i++]); clob(c); }
G(strncpy) { uint64_t i = 0, n = c->x[2]; for (; i < n && P(c->x[1])[i]; i++) P(c->x[0])[i] = P(c->x[1])[i]; for (; i < n; i++) P(c->x[0])[i] = 0; clob(c); }
'''
HARNESS = r'''
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
void uit_nl_stats(uint64_t *calls, uint64_t *diffs);
static uint64_t rng = 88172645463325252ull;
static uint64_t rnd(void) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return rng; }
typedef void (*Fn)(Cpu *);
static const Fn fronts[] = {FRONTS};
static unsigned char A1[4096], B1[4096], A2[4096], B2[4096];
static void fill(unsigned char *p, int n, int alpha) { for (int i = 0; i < n; i++) p[i] = alpha ? "ab\0\x80\xff"[rnd() % 5] : (unsigned char)rnd(); }
int sgn(int v) { return (v > 0) - (v < 0); }
int main(int argc, char **argv) {
    int mode = atoi(argv[1]); BAD = atoi(argv[2]);
    long bad = 0, cases = 0, other = 0;
    for (int it = 0; it < 40000; it++) {
        int i = it % NNAMES;
        int alpha = rnd() & 1;
        fill(A1, 4096, alpha); fill(B1, 4096, alpha);
        for (int k = 4000; k < 4096; k++) A1[k] = B1[k] = 0;   /* the strings end */
        if (rnd() & 1) memcpy(B1, A1, 1 + rnd() % 64);           /* equal prefixes */
        uint64_t n = rnd() % 5 == 0 ? 0 : rnd() % (rnd() & 1 ? 9 : 600);
        uint64_t oa = rnd() % 64, ob = rnd() % 64, ch = rnd() & 1 ? A1[oa + rnd() % 32] : rnd();
        int ov = (rnd() % 4 == 0) && (i == 0 || i == 1);         /* overlapping copies */
        memcpy(A2, A1, 4096); memcpy(B2, B1, 4096);
        Cpu c1, c2; memset(&c1, 0, sizeof c1); for (int r = 0; r < 31; r++) c1.x[r] = rnd(); c1.nzcv = 0x60000000u;
        c1.x[0] = (uint64_t)(uintptr_t)(A1 + oa); c1.x[1] = ov ? (uint64_t)(uintptr_t)(A1 + ob) : (uint64_t)(uintptr_t)(B1 + ob);
        c1.x[2] = n; if (i == 2 || i == 4 || i == 9 || i == 10) c1.x[1] = ch | (rnd() & 1 ? 0xab00ull : 0); if (i == 6) c1.x[1] = n;
        c2 = c1; c2.x[0] = c1.x[0] - (uint64_t)(uintptr_t)A1 + (uint64_t)(uintptr_t)A2;
        c2.x[1] = (i == 2 || i == 4 || i == 6 || i == 9 || i == 10) ? c1.x[1] : c1.x[1] - (uint64_t)(uintptr_t)(ov ? A1 : B1) + (uint64_t)(uintptr_t)(ov ? A2 : B2);
        Cpu before = c1;
        fronts[i](&c1);          /* the front: the PC version (mode 1), the reference with a check (mode 2) */
        GUESTS[i](&c2);          /* the reference (the game's part) */
        cases++;
        uint64_t r1 = c1.x[0], r2 = c2.x[0];
        if (r2 >= (uint64_t)(uintptr_t)A2 && r2 < (uint64_t)(uintptr_t)A2 + 4096) r2 = r2 - (uint64_t)(uintptr_t)A2 + (uint64_t)(uintptr_t)A1;
        int cmp = i == 3 || i == 7 || i == 8;
        int same = cmp ? (mode == 1 && !BAD ? (uint32_t)r1 == (uint32_t)r2 && r1 >> 32 == 0 : sgn((int)r1) == sgn((int)r2)) : r1 == r2;
        if (mode == 1 && BAD) same = 1;
        if (ov && i == 0) { if (!same) bad++; continue; }   /* overlapping memcpy: undefined; the PC copies as memmove */
        if (!same || memcmp(A1, A2, 4096) || memcmp(B1, B2, 4096)) { if (bad++ < 5) printf("  differ: %s it %d n %llu\n", NAMES[i], it, (unsigned long long)n); }
        if (mode == 1) { for (int r = 1; r < 31; r++) other += c1.x[r] != before.x[r]; other += c1.nzcv != before.nzcv; }
    }
    uint64_t calls, diffs; uit_nl_stats(&calls, &diffs);
    printf("mode %d bad %d: %ld cases, %ld differ, %ld other registers changed, %llu calls through the PC side, %llu check differences\n",
           mode, BAD, cases, bad, other, (unsigned long long)calls, (unsigned long long)diffs);
    return 0;
}
'''
cc = os.environ.get('CC') or 'clang'
fronts = []
for k, name in enumerate(nl.NAMES):
    fronts += nl.front([f'void f_{k}(Cpu *c) {{', f'    ref_{name}(c);', '}'], f'f_{k}', name)
names = ', '.join(f'"{n}"' for n in nl.NAMES)
src = (REF + '\n'.join(fronts) + f'\n#define NNAMES {len(nl.NAMES)}\nstatic const char *NAMES[] = {{{names}}};\n'
       + 'static void (*const GUESTS[])(Cpu *) = {' + ', '.join(f'ref_{n}' for n in nl.NAMES) + '};\n'
       + HARNESS.replace('FRONTS', ', '.join(f'f_{k}' for k in range(len(nl.NAMES)))))
runs = {}
with tempfile.TemporaryDirectory() as d:
    t = pathlib.Path(d)
    (t / 'h.c').write_text(src)
    r = subprocess.run([cc, '-O2', '-Wall', '-Wno-unused-function', f'-I{HERE / "src/runtime"}', str(t / 'h.c'),
                        str(HERE / 'src/host/native_libc.c'), '-o', str(t / 'h')], capture_output=True, text=True)
    check(r.returncode == 0, 'harness and src/host/native_libc.c build' + ('' if r.returncode == 0 else ': ' + r.stderr[-1500:]))
    if r.returncode:
        print('FAIL'); sys.exit(1)
    for env, mode, bad in (('', 0, 0), ('1', 1, 0), ('check', 2, 0), ('check', 2, 1), ('check', 2, 2), ('1', 1, 1)):
        e = dict(os.environ); e.pop('TOTK_NATIVE_LIBC', None)
        if env: e['TOTK_NATIVE_LIBC'] = env
        q = subprocess.run([str(t / 'h'), str(max(mode, 0) or 2), str(bad)] if mode == 0 else [str(t / 'h'), str(mode), str(bad)],
                           env=e, capture_output=True, text=True, timeout=120)
        runs[(env, bad)] = q.stdout + q.stderr
        print("     " + "\n     ".join(q.stdout.strip().splitlines()[-6:]))
def nums(s):
    m = re.search(r'(\d+) cases, (\d+) differ, (\d+) other registers changed, (\d+) calls through the PC side, (\d+) check differences', s)
    return tuple(map(int, m.groups())) if m else None
off, on, chk = nums(runs[('', 0)]), nums(runs[('1', 0)]), nums(runs[('check', 0)])
check(off and off[1] == 0 and off[3] == 0, 'TOTK_NATIVE_LIBC unset: the lifted routines run, nothing goes to the PC side')
check(on and on[1] == 0 and on[3] == on[0], f'TOTK_NATIVE_LIBC=1: {on and on[0]} calls, every result and every byte of memory as the reference '
      '(compares: the same value, as a 32-bit int)')
check(on and on[2] == 0, 'TOTK_NATIVE_LIBC=1: no register but x0 changes')
check(chk and chk[1] == 0 and chk[4] == 0, 'check mode on a correct routine: no difference logged')
b1, b2 = nums(runs[('check', 1)]), nums(runs[('check', 2)])
check(b1 and b1[1] == 0 and b1[4] > 0 and 'value (same sign)' in runs[('check', 1)] and 'sign:' not in runs[('check', 1)],
      'check mode: a memcmp that returns only -1/0/1 is reported as "value (same sign)"')
check('overlapping source and destination' in runs[('check', 0)], 'check mode: an overlapping memcpy is reported')
check(b2 and b2[4] > 0 and 'strlen' in runs[('check', 2)] and 'result' in runs[('check', 2)], 'check mode: a wrong strlen is reported')
print('PASS' if ok else 'FAIL'); sys.exit(0 if ok else 1)
