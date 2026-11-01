"""[UIT 0086] The C library's memory and string routines on the PC's own (src/host/native_libc.c).

The SDK module exports the C library (memcpy, memset, strlen, ...); the game calls them through its import stubs (an
indirect call that lands on the lifted function at the exported address) and, inside the SDK, directly. When the code
is lifted, every function whose exported name is in NAMES below gets a small front:

    void f_<addr>(Cpu *c) { if (uit_nl_mode) { uit_nl_call(c, <index>, f_<addr>_lifted); return; }
                            tail call f_<addr>_lifted(c) }     -- f_<addr>_lifted: the lifted code, unchanged

so the game's own routine still runs unless TOTK_NATIVE_LIBC is on (one load and branch per call when it is off).
The routine is chosen by its exported name only (a symbol that is not a function, or is not defined in this module,
is left alone), so another game version or SDK keeps working: whatever is named memcpy is the C memcpy.

What the PC version promises (the C standard's contract, which is all a compiled caller can rely on):
- memcpy, memmove, memset, strcpy, strncpy return the destination; memchr, strchr, strrchr a pointer or 0; strlen,
  strnlen the length. These are exactly the guest's results.
- memcmp, strcmp, strncmp: the sign is exact; the value is the difference of the first differing bytes (as unsigned
  char), the usual implementation. A caller that uses more than the sign would see a different number only if the
  SDK's own version returns something else (e.g. only -1/0/1); TOTK_NATIVE_LIBC=check logs any such call.
- memcpy is done as memmove (overlapping copies give the memmove result, which is what most AArch64 memcpy
  versions also do for overlap; the C standard leaves that undefined).
- Only x0 changes. The guest version may also change x1..x18, v0..v7 and the flags: callers may not rely on those
  after a call (AAPCS64), so leaving them as they were is allowed.

TOTK_LIFT_NATIVE_LIBC=0 (setup reads it from ultrainstincttears.cfg) lifts these functions as before (no front)."""
import os
from collections import Counter

# Index order is the C side's (src/host/native_libc.c NL_NAMES); tools/tests/test_native_libc.py checks they match.
NAMES = ('memcpy', 'memmove', 'memset', 'memcmp', 'memchr', 'strlen', 'strnlen', 'strcmp', 'strncmp', 'strchr',
         'strrchr', 'strcpy', 'strncpy')
INDEX = {n: i for i, n in enumerate(NAMES)}
ON = os.environ.get('TOTK_LIFT_NATIVE_LIBC', '1') != '0'
STATS = Counter()
STT_FUNC = 2


def find(nso, base):
    """{guest address: name} of the defined functions in this module whose exported name is in NAMES."""
    if not ON:
        return {}
    out = {}
    for name, info, shndx, value, size in nso.syms:
        if shndx and (info & 0xF) == STT_FUNC and name in INDEX and value:
            out.setdefault(base + value, name)
    return out


def front(lines, fn, name):
    """The lifted function's lines, renamed to <fn>_lifted, followed by the front under the original name."""
    head = f'void {fn}(Cpu *c) {{'
    if not lines or lines[0] != head:
        STATS['skipped'] += 1
        return lines
    STATS['fronts'] += 1
    i = INDEX[name]
    return ([f'void {fn}_lifted(Cpu *c);', f'void {fn}_lifted(Cpu *c) {{'] + lines[1:] +
            [f'void {fn}(Cpu *c) {{   /* [UIT 0086] {name}: the PC version when TOTK_NATIVE_LIBC is on */',
             '    extern int uit_nl_mode; extern void uit_nl_call(Cpu *, int, void (*)(Cpu *));',
             f'    if (__builtin_expect(uit_nl_mode != 0, 0)) {{ uit_nl_call(c, {i}, {fn}_lifted); return; }}',
             f'    __attribute__((musttail)) return {fn}_lifted(c);',
             '}'])
