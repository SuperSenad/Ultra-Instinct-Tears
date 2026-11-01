"""[UIT 0190] tools/lifter/restrict.py: `Cpu *__restrict c` (TOTK_LIFT_RESTRICT=1) and the loop barriers that it and the
register cache (UIT 0078) need.          py -3 tools/tests/test_lift_restrict.py

1. The edits: definitions rewritten, HLE stubs keep `Cpu *c` and let it escape first, a barrier at the function entry,
   at L_dispatch and at every label a later goto jumps back to, nothing else touched; off = the lines unchanged.
2. Spin waits: three guest loops that wait on a word another thread sets (plain ldr / cbz, ldar / cbz, ldr / cmp /
   b.ne) are lifted with each code generation and run on a host thread while the main thread sets the word: each must
   end. Without the barriers the compiler moves the load out of the loop and the wait never ends; that control is
   printed (it depends on the compiler, so it is not a pass condition).
3. The Windows build's compiler (ltools/zwcc, when present) takes the code without warnings.
The random-program differential (old = restrict = restrict with the register cache) is in test_regcache.py.
Needs a C compiler that runs on this machine (CC, default clang / ltools/zcc) and pthreads."""
import os, pathlib, subprocess, sys, tempfile
from collections import Counter
HERE = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(HERE / 'tools' / 'lifter'))
os.environ['TOTK_LIFT_REGCACHE'] = '1'
os.environ['TOTK_LIFT_SPIN_GATES'] = '0'   # the waits below must hold without rt_spin_gate (0077) too
import regcache
import gen
import restrict
from lifter import Lifter

CC = os.environ.get('CC') or (str(HERE.parent / 'ltools' / 'zcc') if (HERE.parent / 'ltools' / 'zcc').exists() else 'clang')
ok = True
def check(c, w):
    global ok; print(('ok   ' if c else 'FAIL ') + w, flush=True); ok &= bool(c)

# ---------------------------------------------------------------- 1. the edits
F = ['void f_800000000(Cpu *c) {', '    FN_ENTER(0x800000000);', 'L_800000004:;', '    c->x[0] = LD32(c->x[1]);',
     '    if (c->x[0] == 0) { goto L_800000004; }', '    if (c->x[2]) { goto L_800000010; }', 'L_800000010:;',
     '    return;', '}']
restrict.ON = False
check(restrict.apply(list(F), False, False) == F and restrict.apply(list(F), True, False) == F,
      'off (and no register cache): the lines are unchanged')
restrict.ON = True
out = restrict.apply(list(F), False, True)
B = '__asm__ volatile("" : : : "memory");'
check(out[0] == 'void f_800000000(Cpu *__restrict c) {', 'the definition takes Cpu *__restrict c')
check(out[1] == F[1], 'no barrier at the function entry (0192: it made inlined calls write registers back)')
i = out.index('L_800000004:;'); j = out.index('L_800000010:;')
check(B in out[i + 1] and B not in out[j + 1], 'a barrier after the label a later goto jumps back to, none after a forward-only label')
check(sum(B in l for l in out) == 1 and len(out) == len(F) + 1, 'nothing else is added')
D = ['void f_800000100(Cpu *c) {', 'L_800000100:;', '    c->pc = c->x[3]; goto L_dispatch;', 'L_800000104:;', '    return;',
     'L_dispatch:', '    switch (c->pc) { case 0x800000100: goto L_800000100; case 0x800000104: goto L_800000104; default: break; }',
     '    __attribute__((musttail)) return rt_dispatch(c);', '}']
od = restrict.apply(list(D), False, True)
check(all(B in od[od.index(l) + 1] for l in ('L_800000100:;', 'L_800000104:;', 'L_dispatch:')),
      'L_dispatch and every label its switch reaches get a barrier')
check(od[-2] == '    ' + B + ' __attribute__((musttail)) return rt_dispatch(c);', 'a barrier right before a tail call (0192)')
T = ['void f_800000400(Cpu *c) {', '    if (c->x[1] == 0) { __attribute__((musttail)) return f_800000500(c); }', '    return;', '}']
ot = restrict.apply(list(T), False, True)
check(ot[1] == '    if (c->x[1] == 0) { ' + B + ' __attribute__((musttail)) return f_800000500(c); }' and len(ot) == len(T),
      'a barrier inside the braces of a conditional tail call, nothing else')
H = ['void f_800000200(Cpu *c) {', '    hle_strlen(c);', '}']
oh = restrict.apply(list(H), True, True)
check(oh[0] == H[0] and '"r"(c)' in oh[1] and oh[2:] == H[1:], 'an HLE stub keeps Cpu *c and lets c escape (memory clobber) before the HLE')
A = ['void f_800000300(Cpu *c) { c->entry_at = 0x800000300; __attribute__((musttail)) return f_800000000(c); }']
check(restrict.apply(list(A), False, True) == ['void f_800000300(Cpu *c) { c->entry_at = 0x800000300; ' + B + ' __attribute__((musttail)) return f_800000000(c); }'],
      'a one-line alias stub: the tail-call barrier, no restrict (0192: no loop, no register cache)')
restrict.ALL = True
check(restrict.apply(list(A), False, True) == ['void f_800000300(Cpu *__restrict c) { c->entry_at = 0x800000300; ' + B + ' __attribute__((musttail)) return f_800000000(c); }'],
      'TOTK_LIFT_RESTRICT_ALL=1: the alias stub takes Cpu *__restrict c too (as 0190)')
restrict.ALL = False
R = ['void f_800000400(Cpu *c) {', '    FN_ENTER(c);', '    uint64_t rc_x1 = c->x[1];', '    c->x[0] = rc_x1;', '}']
check(restrict.apply(list(R), False, True)[0] == 'void f_800000400(Cpu *__restrict c) {',
      'a function with register-cache locals and no loop takes Cpu *__restrict c (0192)')

# ---------------------------------------------------------------- 2. spin waits
BASE = 0x800100000
W = [0xB9400001, 0x34FFFFE1, 0xD65F03C0,                # wait_ldr:  ldr w1, [x0]; cbz w1, .-4; ret
     0x88DFFC01, 0x34FFFFE1, 0xD65F03C0,                # wait_ldar: ldar w1, [x0]; cbz w1, .-4; ret
     0xB9400001, 0x7100143F, 0x54FFFFC1, 0xD65F03C0,    # wait_cmp:  ldr w1, [x0]; cmp w1, #5; b.ne .-8; ret
     0xB9400001, 0x34000041, 0xD65F03C0,                # wait_tail: ldr w1, [x0]; cbz w1, again; ret
     0x17FFFFFD]                                        # again:     b wait_tail   (a loop of two tail calls)
FNS = [(BASE, BASE + 12), (BASE + 12, BASE + 24), (BASE + 24, BASE + 40), (BASE + 40, BASE + 52), (BASE + 52, BASE + 56)]

def lift(on, fl, rst, loops):
    regcache.ON, regcache.FLAGS, restrict.ON, restrict.ALL = on, on and fl, bool(rst), rst == 2   # 2: every function (0190)
    mod = gen.Module.__new__(gen.Module)
    mod.name, mod.base, mod.lo, mod.hi, mod.words = 'main', BASE, BASE, BASE + 4 * len(W), W
    mod.lifter, mod.cache, mod.stats, mod.mu, mod.spins = Lifter(BASE, 'c'), {}, Counter(), None, {}
    src = ['#include "cpu.h"', '#include "simd.h"', '#include "regcache.h"'] + [f'void {gen.fname(a)}(Cpu *c);' for a, _ in FNS]
    for a, e in FNS:
        labels, ext, tables = mod.scan(a, e)
        fl_ = restrict.apply(mod.emit_function(a, e, labels, tables, {}), False, bool(loops))
        if loops == 'labels':   # control: the label barriers without the tail-call ones
            fl_ = [l.replace(restrict.BARRIER + ' ' + restrict.TAIL, restrict.TAIL) for l in fl_]
        src += fl_
    return '\n'.join(src) + '\n'

HARNESS = r'''
#include "cpu.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
volatile int g_fnprof;
void fnprof_hit(uint64_t a, uint64_t l, uint64_t x) { (void)a; (void)l; (void)x; }
void rt_dispatch(Cpu *c) { (void)c; exit(3); }
void rt_svc(Cpu *c, uint32_t imm, uint64_t pc) { (void)c; (void)imm; (void)pc; exit(8); }
void rt_brk(Cpu *c, uint32_t i, uint64_t p) { (void)c; (void)i; (void)p; exit(4); }
void rt_udf(Cpu *c, uint64_t p) { (void)c; (void)p; exit(5); }
void rt_bad_entry(Cpu *c, uint64_t a, uint64_t f) { (void)c; (void)a; (void)f; exit(6); }
void rt_spin_gate(Cpu *c, uint64_t h) { (void)c; (void)h; }
uint64_t rt_ticks(void) { return 0; }
void F0(Cpu *c); void F1(Cpu *c); void F2(Cpu *c); void F3(Cpu *c); void F4(Cpu *c);
static void (*const fn[4])(Cpu *) = {F0, F1, F2, F3};
static uint32_t word; static int which; static Cpu cpu;
static void *run(void *a) { (void)a; cpu.x[0] = (uint64_t)(uintptr_t)&word; fn[which](&cpu); return 0; }
int main(int argc, char **argv) {
    which = atoi(argv[1]);
    pthread_t t; pthread_create(&t, 0, run, 0);
    usleep(20000); __atomic_store_n(&word, 5, __ATOMIC_RELEASE);
    pthread_join(t, 0); printf("%u\n", (unsigned)cpu.x[1]); return 0;
}
'''
VARIANTS = [('old', 0, 0, 0, 0), ('restrict', 0, 0, 1, 1), ('register cache', 1, 0, 0, 1), ('cache + flags', 1, 1, 0, 1),
            ('cache + flags + restrict', 1, 1, 1, 1), ('cache + flags + restrict on every function', 1, 1, 2, 1)]
CONTROLS = [('restrict, no barriers', 0, 0, 2, 0), ('cache + flags, no barriers', 1, 1, 0, 0),
            ('cache + flags + restrict, loop barriers only', 1, 1, 1, 'labels')]
names = ('ldr/cbz', 'ldar/cbz', 'ldr/cmp/b.ne', 'ldr/cbz through two tail calls')
last = ''
with tempfile.TemporaryDirectory() as d:
    t = pathlib.Path(d)
    h = HARNESS
    for i, (a, _) in enumerate(FNS):
        h = h.replace(f'F{i}', gen.fname(a))
    (t / 'h.c').write_text(h)
    for tag, on, fl, rst, loops in VARIANTS + CONTROLS:
        src = lift(on, fl, rst, loops)
        if rst and not on and loops:   # (the cache's code is built by test_regcache.py's check)
            last = src
        (t / 'v.c').write_text(src)
        exe = t / 'v'
        p = subprocess.run([CC, '-O2', '-w', '-fno-strict-aliasing', f'-I{HERE / "src/runtime"}', str(t / 'v.c'), str(t / 'h.c'),
                            '-lpthread', '-o', str(exe)], capture_output=True, text=True)
        if p.returncode:
            check(False, f'{tag}: compiles: {p.stderr[-800:]}'); continue
        res = []
        for k in range(4):
            try:
                q = subprocess.run([str(exe), str(k)], capture_output=True, text=True, timeout=3)
                res.append(q.returncode == 0 and q.stdout.strip() == '5')
            except subprocess.TimeoutExpired:
                res.append(False)
        desc = ', '.join(f'{n} {"ends" if r else "NEVER ENDS"}' for n, r in zip(names, res))
        if (tag, on, fl, rst, loops) in VARIANTS:
            check(all(res), f'{tag}: a guest wait on a word another thread sets ends ({desc})')
        else:
            print(f'info {tag} (control, not a pass condition): {desc}')

# ---------------------------------------------------------------- 3. the Windows build's compiler
zw = HERE.parent / 'ltools' / 'zwcc'
if zw.exists() and last:
    with tempfile.TemporaryDirectory() as d:
        code = last + '\n'.join(restrict.apply(list(H), True, True)).replace('hle_strlen(c)', 'rt_svc(c, 0, 0)') + '\n'
        (pathlib.Path(d) / 'w.c').write_text(code)
        p = subprocess.run([str(zw), '-O2', '-mcpu=x86_64_v3', '-Wall', '-Werror', '-Wno-unused-function', '-Wno-unused-label',
                            '-fno-strict-aliasing', f'-I{HERE / "src/runtime"}', '-c', str(pathlib.Path(d) / 'w.c'), '-o', str(pathlib.Path(d) / 'w.o')],
                           capture_output=True, text=True)
        check(p.returncode == 0, 'the Windows compiler builds restrict, the barriers and an HLE stub without warnings'
              + ('' if p.returncode == 0 else ': ' + p.stderr[-800:]))
print('PASS' if ok else 'FAIL'); sys.exit(0 if ok else 1)
