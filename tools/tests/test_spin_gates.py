"""Box test for the spin-wait gates (UIT 0077): tools/lifter/spin_waits_v143.txt is well formed, spins.py reads it,
and gen.py puts rt_spin_gate before the listed back-edges only (b, b.cond, cbz/cbnz, tbz/tbnz), leaving a listed
address that is not a branch to the head as it was; the emitted C compiles.   py -3 tools/tests/test_spin_gates.py"""
import os, pathlib, re, subprocess, sys, tempfile
from collections import Counter
HERE = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(HERE / 'tools' / 'lifter'))
import spins
import types
import gen
from lifter import Lifter

ok = True
def check(c, w):
    global ok; print(('ok   ' if c else 'FAIL ') + w); ok &= bool(c)

# ---------------------------------------------------------------- the table
lines = [l for l in spins.TABLE.read_text().splitlines() if l.startswith('spin ')]
mods = Counter(l.split()[1] for l in lines)
check(len(lines) == 857 and mods == {'main': 845, 'subsdk0': 9, 'sdk': 3}, f'table: {len(lines)} waits {dict(mods)}')
seen, bad = set(), 0
for l in lines:
    f = l.split('#')[0].split(); h = int(f[2], 16)
    for b in f[3:]:
        b = int(b, 16)
        bad += (f[1], b) in seen or not (h <= b < h + 512) or b % 4 or h % 4
        seen.add((f[1], b))
check(bad == 0, f'table: {len(seen)} back-edges, each once, aligned, within 512 bytes after its head')
m = spins.load('main', 0x800000000)
check(len(m) == sum(1 for (mm, _) in seen if mm == 'main') and m[0x8000231ec] == 0x8000231dc, 'spins.load: main+231ec -> 231dc')
os.environ['TOTK_LIFT_SPIN_GATES'] = '0'
check(spins.load('main', 0x800000000) == {}, 'TOTK_LIFT_SPIN_GATES=0: no gates')
del os.environ['TOTK_LIFT_SPIN_GATES']

# ---------------------------------------------------------------- emission (a module without an NSO)
BASE = 0x800100000
NOP = 0xD503201F
ldrw = lambda rt, rn: 0xB9400000 | rn << 5 | rt
cbz = lambda rt, off: 0x34000000 | ((off // 4) & 0x7FFFF) << 5 | rt
cbnz = lambda rt, off: 0x35000000 | ((off // 4) & 0x7FFFF) << 5 | rt
tbz = lambda rt, bit, off: 0x36000000 | bit << 19 | ((off // 4) & 0x3FFF) << 5 | rt
b = lambda off: 0x14000000 | (off // 4) & 0x3FFFFFF
bne = lambda off: 0x54000001 | ((off // 4) & 0x7FFFF) << 5
cmpw = lambda rn, imm: 0x7100001F | imm << 10 | rn << 5
RET = 0xD65F03C0
words = [ldrw(8, 0), cbz(8, -4),            # +0: loop 1 (cbz back to +0)
         ldrw(9, 1), cmpw(9, 1), bne(-8),   # +8: loop 2 (b.ne back to +8)
         ldrw(10, 2), tbz(10, 3, -4),       # +20: loop 3 (tbz back to +20)
         ldrw(11, 3), cbnz(11, 8), b(-8),   # +28: loop 4 (b back to +28), +32 cbnz forward to +40 (listed: not to its head)
         NOP, RET]
mod = gen.Module.__new__(gen.Module)
mod.name, mod.base, mod.lo, mod.hi, mod.words = 'main', BASE, BASE, BASE + 4 * len(words), words
mod.lifter, mod.cache, mod.stats, mod.mu = Lifter(BASE, 'c'), {}, Counter(), None
mod.spins = {BASE + 4: BASE, BASE + 16: BASE + 8, BASE + 24: BASE + 20, BASE + 36: BASE + 28, BASE + 32: BASE + 28}
labels = {BASE, BASE + 8, BASE + 20, BASE + 28, BASE + 40}
spins.STATS.update(gates=0, skipped=0)
out = '\n'.join(mod.emit_function(BASE, mod.hi, labels, {}, {}))
gates = re.findall(r'rt_spin_gate\(c, (0x[0-9a-f]+)\)', out)
check(sorted(int(g, 16) - BASE for g in gates) == [0, 8, 20, 28], f'gates before cbz, b.ne, tbz, b back-edges ({gates})')
check(spins.STATS == {'gates': 4, 'skipped': 1}, f'a listed cbnz to another place is left as it was ({spins.STATS})')
check(all(re.search(r'rt_spin_gate\(c, 0x%x\); goto L_%x;' % (h, h), out) for h in (BASE, BASE + 8, BASE + 20, BASE + 28)),
      'each gate is followed by the jump to its own head')
mod.spins = {}
plain = '\n'.join(mod.emit_function(BASE, mod.hi, labels, {}, {}))
check(re.sub(r'rt_spin_gate\(c, 0x[0-9a-f]+\); ', '', out) == plain, 'apart from the gates the C is unchanged')
cc = os.environ.get('CC') or 'clang'
with tempfile.TemporaryDirectory() as d:
    src = pathlib.Path(d) / 't.c'
    src.write_text('#include "cpu.h"\n#include "simd.h"\n' + out + '\n')
    r = subprocess.run([cc, '-O2', '-c', '-w', f'-I{HERE / "src/runtime"}', str(src), '-o', str(pathlib.Path(d) / 't.o')],
                       capture_output=True, text=True)
    check(r.returncode == 0, 'the emitted C compiles' + ('' if r.returncode == 0 else ': ' + r.stderr[-600:]))
print('PASS' if ok else 'FAIL'); sys.exit(0 if ok else 1)
