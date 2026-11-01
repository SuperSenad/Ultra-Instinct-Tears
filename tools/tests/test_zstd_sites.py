"""Box test for the zstd call sites (UIT 0079): tools/lifter/zstd_sites.py's table is the 1.4.3 one (seven bl, never
main+0x547a04), and gen.py lifts a listed bl as a call of the PC decoder (passing the lifted decoder along) only when the
instruction is exactly the expected one; TOTK_LIFT_ZSTD=0 lifts it as before; the emitted C compiles.
   py -3 tools/tests/test_zstd_sites.py"""
import importlib, os, pathlib, re, subprocess, sys, tempfile, types
from collections import Counter
HERE = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(HERE / 'tools' / 'lifter'))
import zstd_sites as zs
import gen
from lifter import Lifter

ok = True
def check(c, w):
    global ok; print(('ok   ' if c else 'FAIL ') + w); ok &= bool(c)

MAIN = 0x800000000
check(len(zs.SITES) == 7 and Counter(t for _, t in zs.SITES.values()) == {0x3a1f0: 4, 0x3a1e0: 3},
      'table: 4 MultiFrame + 3 usingDDict sites')
check(0x547a04 in zs.NEVER and 0x547a04 not in zs.SITES, 'main+0x547a04 is never redirected')

# ---------------------------------------------------------------- call() on a stand-in module
class Fake:
    def __init__(s, name, base, words): s.name, s.base, s.w = name, base, words
    def word(s, a): return s.w.get(a, 0xD503201F)
words = {MAIN + o: w for o, (w, _) in zs.SITES.items()}
f = Fake('main', MAIN, words)
got = {o: zs.call(f, MAIN + o, MAIN + t) for o, (_, t) in zs.SITES.items()}
check(all(got.values()), 'every listed site becomes a PC call')
check(all(('uit_zstd_multiframe(c, f_80003a1f0)' if t == 0x3a1f0 else 'uit_zstd_using_ddict(c, f_80003a1e0)') in got[o]
          and f'c->x[30] = 0x{MAIN + o + 4:x}ull;' in got[o] for o, (_, t) in zs.SITES.items()),
      'with the right host function, the lifted decoder and the return address')
words[MAIN + 0x547a04] = 0x97ebc9fb   # whatever is there: never
check(zs.call(f, MAIN + 0x547a04, MAIN + 0x3a1f0) is None, 'main+0x547a04: lifted as before')
zs.STATS.clear()
f2 = Fake('main', MAIN, {**words, MAIN + 0x5f5358: 0x97e913a7})   # another game version: a different bl
check(zs.call(f2, MAIN + 0x5f5358, MAIN + 0x3a1f4) is None and zs.STATS['skipped'] == 1, 'a different instruction: lifted as before')
check(zs.call(Fake('subsdk0', 0x804000000, {}), 0x804000000 + 0x5f5358, MAIN + 0x3a1f0) is None, 'other modules: untouched')
check(zs.call(Fake('main', MAIN, words), MAIN + 0x5f5360, MAIN + 0x3a1f0) is None, 'unlisted addresses: untouched')

# ---------------------------------------------------------------- emission through gen.py
NOP, RET = 0xD503201F, 0xD65F03C0
LO = MAIN + 0x5f5350
ws = [NOP, NOP, zs.SITES[0x5f5358][0], RET]
mod = gen.Module.__new__(gen.Module)
mod.name, mod.base, mod.lo, mod.hi, mod.words = 'main', MAIN, LO, LO + 4 * len(ws), ws
mod.lifter, mod.cache, mod.stats, mod.mu, mod.spins = Lifter(LO, 'c'), {}, Counter(), None, {}

def emit():
    mod.cache = {}
    return '\n'.join(mod.emit_function(LO, mod.hi, {LO}, {}, {}))
out = emit()
check('uit_zstd_multiframe(c, f_80003a1f0);' in out and 'rt_dispatch' not in out, 'gen.py: the bl becomes the PC call')
os.environ['TOTK_LIFT_ZSTD'] = '0'; importlib.reload(zs); gen.zstd_sites = zs
plain = emit()
del os.environ['TOTK_LIFT_ZSTD']; importlib.reload(zs); gen.zstd_sites = zs
check('uit_zstd' not in plain and 'c->pc = 0x80003a1f0ull; rt_dispatch(c);' in plain, f'TOTK_LIFT_ZSTD=0: lifted as before')
cc = os.environ.get('CC') or 'clang'
with tempfile.TemporaryDirectory() as d:
    src = pathlib.Path(d) / 't.c'
    src.write_text('#include "cpu.h"\n#include "simd.h"\nvoid f_80003a1f0(Cpu *c);\n' + out + '\n')
    r = subprocess.run([cc, '-O2', '-c', '-Wall', '-Wno-unused-function', '-Wno-unused-label', '-Wno-unused-variable',
                        f'-I{HERE / "src/runtime"}', str(src), '-o', str(pathlib.Path(d) / 't.o')], capture_output=True, text=True)
    check(r.returncode == 0, 'the emitted C compiles' + ('' if r.returncode == 0 else ': ' + r.stderr[-600:]))
print('PASS' if ok else 'FAIL'); sys.exit(0 if ok else 1)
