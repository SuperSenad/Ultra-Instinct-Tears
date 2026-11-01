"""[UIT 0190] Microbenchmark of the lifted code generations: game-like kernels (tools/tests/bench_lift/kern.c: a point
transform with a leaf call per element, an actor list walk with virtual calls, a name hash table, an insertion sort
through a comparator pointer, an LZ-style byte decoder, accessor-heavy calls) are compiled for AArch64, lifted with
tools/lifter (gen.py, regcache.py, restrict.py) in each code generation, built for the PC at -O2 x86-64-v3 like the
game and timed; each kernel's result must be the same in every generation.

    python3 tools/tests/bench_lift.py [rounds]          (Linux box; LLVM=<dir with clang, ld.lld, llvm-nm, llvm-objcopy>,
                                                          default: from PATH; clang must target aarch64 too)
Generations: old, restrict (TOTK_LIFT_RESTRICT=1), cache (TOTK_LIFT_REGCACHE=1), cache + flags
(+ TOTK_LIFT_REGCACHE_FLAGS=1), cache + flags + restrict; the last four with the loop barriers, as gen.py emits them."""
import os, pathlib, re, shutil, struct, subprocess, sys, tempfile
from collections import Counter, defaultdict
HERE = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(HERE / 'tools' / 'lifter'))
os.environ['TOTK_LIFT_REGCACHE'] = '1'
os.environ['TOTK_LIFT_SPIN_GATES'] = '0'
import regcache, gen, restrict
from lifter import Lifter

ROUNDS = int(sys.argv[1]) if len(sys.argv) > 1 else 5
def tool(n):
    d = os.environ.get('LLVM')
    p = str(pathlib.Path(d) / n) if d else shutil.which(n)
    if not p or not os.path.exists(p):
        sys.exit(f'bench_lift: {n} not found (set LLVM to an LLVM bin folder)')
    return p
CLANG, LLD, NM, OBJCOPY = tool('clang'), tool('ld.lld'), tool('llvm-nm'), tool('llvm-objcopy')
SRC = HERE / 'tools' / 'tests' / 'bench_lift'
BASE = 0x800100000
# (tag, register cache, operand flags, restrict: 1 = loops / cache only (0192), 2 = every function (0190), clean: 0 = write
# every cached register back at calls (0078), 1 = only the ones the function changes (0192))
GENS = [('old', 0, 0, 0, 1), ('restrict', 0, 0, 1, 1), ('cache-0078', 1, 0, 0, 0), ('cache', 1, 0, 0, 1),
        ('cache+flags', 1, 1, 0, 1), ('c+f+restrict-all', 1, 1, 2, 0), ('cache+flags+restrict', 1, 1, 1, 1)]

with tempfile.TemporaryDirectory() as d:
    t = pathlib.Path(d)
    rd = subprocess.run([CLANG, '-print-resource-dir'], capture_output=True, text=True, check=True).stdout.strip()
    subprocess.run([CLANG, '--target=aarch64-linux-gnu', '-O2', '-ffreestanding', '-fno-builtin', '-fno-stack-protector',
                    '-fno-jump-tables', '-nostdinc', '-isystem', f'{rd}/include', '-c', str(SRC / 'kern.c'), '-o', str(t / 'k.o')], check=True)
    subprocess.run([LLD, f'-Ttext={BASE:#x}', '-e', '0', str(t / 'k.o'), '-o', str(t / 'k.elf')], check=True)
    syms = []
    for l in subprocess.run([NM, '-S', '--defined-only', str(t / 'k.elf')], capture_output=True, text=True, check=True).stdout.splitlines():
        p = l.split()
        if len(p) == 4 and p[2] in 'Tt':
            syms.append((int(p[0], 16), int(p[1], 16), p[3]))
    syms.sort()
    subprocess.run([OBJCOPY, '-O', 'binary', '--only-section=.text', str(t / 'k.elf'), str(t / 'text.bin')], check=True)
    data = (t / 'text.bin').read_bytes()
    words = struct.unpack(f'<{len(data) // 4}I', data)
    base = syms[0][0]

    def lift(on, fl, rst, clean):
        regcache.ON, regcache.FLAGS, restrict.ON, restrict.ALL, regcache.CLEAN = on, on and fl, bool(rst), rst == 2, bool(clean)
        mod = gen.Module.__new__(gen.Module)
        mod.name, mod.base, mod.lo, mod.hi, mod.words = 'main', base, base, base + 4 * len(words), words
        mod.lifter, mod.cache, mod.stats, mod.mu, mod.spins = Lifter(base, 'c'), {}, Counter(), None, {}
        src = ['#include "cpu.h"', '#include "simd.h"', '#include "regcache.h"'] + [f'void {gen.fname(a)}(Cpu *c);' for a, _, _ in syms]
        for a, n, _ in syms:
            labels, ext, tables = mod.scan(a, a + n)
            src += restrict.apply(mod.emit_function(a, a + n, labels, tables, {}), False, bool(on or rst))
        return '\n'.join(src) + '\n'

    with open(t / 'syms.h', 'w') as f:
        for a, n, name in syms:
            f.write(f'#define A_{name} {a:#x}ull\n')
        f.write('#define FN_TABLE ' + ' '.join(f'X({a:#x}ull, {gen.fname(a)})' for a, _, _ in syms) + '\n')
        f.write(''.join(f'void {gen.fname(a)}(Cpu *c);\n' for a, _, _ in syms))
    flags = ['-O2', '-march=x86-64-v3', '-fno-strict-aliasing', '-w', f'-I{HERE / "src/runtime"}', f'-I{t}']
    subprocess.run([CLANG, *flags, '-c', str(SRC / 'harness.c'), '-o', str(t / 'h.o')], check=True)
    for tag, on, fl, rst, clean in GENS:
        (t / f'{tag}.c').write_text(lift(on, fl, rst, clean))
        subprocess.run([CLANG, *flags, '-c', str(t / f'{tag}.c'), '-o', str(t / f'{tag}.o')], check=True)
        subprocess.run([CLANG, str(t / f'{tag}.o'), str(t / 'h.o'), '-lm', '-o', str(t / tag)], check=True)
    best, res = defaultdict(lambda: 1e18), defaultdict(set)
    pin = ['taskset', '-c', '2'] if shutil.which('taskset') else []
    for _ in range(ROUNDS):
        for tag, *_ in GENS:
            for l in subprocess.run(pin + [str(t / tag), '40'], capture_output=True, text=True, check=True).stdout.splitlines():
                m = re.match(r'(\w+)\s+([\d.]+) us(?:\s+result (\w+))?', l)
                if m:
                    best[tag, m[1]] = min(best[tag, m[1]], float(m[2]))
                    if m[3]:
                        res[m[1]].add(m[3])
    ks = ['xform', 'actors', 'hash', 'sort', 'bits', 'calls', 'total']
    print(f'best of {ROUNDS} rounds x 40 runs, microseconds (lower is better); % against old')
    print('%-22s' % 'generation' + ''.join('%15s' % k for k in ks))
    for tag, *_ in GENS:
        print('%-22s' % tag + ''.join('%8.1f %+5.0f%%' % (best[tag, k], 100 * (best[tag, k] / best['old', k] - 1)) for k in ks))
    if os.environ.get('BENCH_LIFT_KEEP'):   # keep the lifted C, objects and programs for a look
        shutil.copytree(t, os.environ['BENCH_LIFT_KEEP'], dirs_exist_ok=True)
    same = all(len(v) == 1 for v in res.values())
    print('results the same in every generation:', same)
    sys.exit(0 if same else 1)
