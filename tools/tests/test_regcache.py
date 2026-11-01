"""Differential test of the register cache (UIT 0078, tools/lifter/regcache.py), no emulator: random AArch64
programs (loops, flags, csel/ccmp/adcs, loads/stores through a buffer and the stack, forward branches, bl, blr through
the dispatcher, svc) are lifted twice, with the old code generation and with the register cache, both are compiled
and run from the same random register / memory states, and every register, the flags, the buffer and the stack must
come out the same. Also: the flag macros in src/runtime/regcache.h against cpu.h's helpers, and the structural check
(undoing the edits gives the old C back).       py -3 tools/tests/test_regcache.py [programs] [states]
Needs a C compiler that runs on this machine (CC, default clang / ltools/zcc)."""
import os, pathlib, random, subprocess, sys, tempfile, types
from collections import Counter
HERE = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(HERE / 'tools' / 'lifter'))
os.environ['TOTK_LIFT_REGCACHE'] = '1'
os.environ['TOTK_LIFT_SPIN_GATES'] = '0'
import regcache
import gen
import restrict   # [UIT 0190]
from lifter import Lifter

NPROG = int(sys.argv[1]) if len(sys.argv) > 1 else 150
NSTATE = int(sys.argv[2]) if len(sys.argv) > 2 else 12
CC = os.environ.get('CC') or 'clang'
ok = True
def check(c, w):
    global ok; print(('ok   ' if c else 'FAIL ') + w, flush=True); ok &= bool(c)

check(regcache.ON, 'regcache on (TOTK_LIFT_REGCACHE=1) and the runtime headers pass its check')
BASE = 0x800100000
POOL = list(range(0, 24))      # registers random code may write (x24/x25/x28: loop counts, x26: blr target, x27: buffer)
READ = list(range(0, 29))
r = random.Random(1)
def rr(pool=POOL): return r.choice(pool)

def alu():
    k = r.randrange(16); sf = r.random() < 0.75; S = 1 << 31 if sf else 0
    d, n, m = rr(), rr(READ), rr(READ)
    if k == 0: return (0x11000000 | S | r.randrange(4096) << 10 | n << 5 | d) | (r.randrange(4) << 29 & 0x60000000)   # add/adds/sub/subs imm
    if k == 1: return 0x0B000000 | S | r.randrange(3) << 22 | m << 16 | r.randrange(32 if not sf else 64) << 10 | n << 5 | d | (r.randrange(4) << 29)
    if k == 2: return [0x0A000000, 0x2A000000, 0x4A000000, 0x6A000000][r.randrange(4)] | S | r.randrange(4) << 22 | m << 16 | r.randrange(32 if not sf else 64) << 10 | n << 5 | d
    if k == 3: return 0xD2800000 | r.randrange(4) << 21 | r.randrange(65536) << 5 | d
    if k == 4: return 0xF2800000 | r.randrange(4) << 21 | r.randrange(65536) << 5 | d
    if k == 5: return 0x9B000000 | m << 16 | rr(READ) << 10 | n << 5 | d
    if k == 6: return 0x1A800000 | S | m << 16 | r.randrange(14) << 12 | r.randrange(2) << 10 | n << 5 | d   # csel/csinc
    if k == 7: return 0x7A400000 | S | m << 16 | r.randrange(14) << 12 | n << 5 | r.randrange(16)          # ccmp reg
    if k == 8: return [0x1A000000, 0x3A000000, 0x5A000000, 0x7A000000][r.randrange(4)] | S | m << 16 | n << 5 | d   # adc/adcs/sbc/sbcs
    if k == 9:
        s = r.randrange(1, 63); return 0xD3400000 | s << 16 | 63 << 10 | n << 5 | d   # lsr
    if k == 10: return 0xF9400000 | r.randrange(512) << 10 | 27 << 5 | d   # ldr x, [x27, #]
    if k == 11: return 0xF9000000 | r.randrange(512) << 10 | 27 << 5 | rr(READ)   # str x
    if k == 12: return 0x39400000 | r.randrange(4096) << 10 | 27 << 5 | d   # ldrb
    if k == 13: return 0x39000000 | r.randrange(4096) << 10 | 27 << 5 | rr(READ)   # strb
    if k == 14: return [0xD53B4200 | d, 0xD51B4200 | rr(READ)][r.randrange(2)]   # mrs/msr nzcv
    return 0xF1000000 | r.randrange(4096) << 10 | n << 5 | 31   # cmp imm

def block(n, calls):
    """n instructions with forward conditional branches inside; calls: list of encoders (pos -> word)"""
    ws = [alu() for _ in range(n)]
    for _ in range(n // 6):
        i = r.randrange(n - 1); j = r.randrange(i + 1, n + 1); off = (j - i) * 4
        ws[i] = [0x54000000 | (off // 4) << 5 | r.randrange(14), 0xB4000000 | (off // 4) << 5 | rr(READ),
                 0x36000000 | r.randrange(32) << 19 | (off // 4) << 5 | rr(READ)][r.randrange(3)]
    return ws, calls

def program():
    """F0 (loops, may call F1 by bl, F2 by blr through the dispatcher, svc), F1 and F2 (leaf loops)."""
    def leaf(cnt_reg):
        ws = [0xA9BE7BFD]   # stp x29, x30, [sp, #-32]!
        ws += [0xD2800000 | r.randrange(1, 12) << 5 | cnt_reg]
        body, _ = block(r.randrange(4, 30), [])
        top = len(ws)
        ws += body
        ws += [0xF1000400 | cnt_reg << 5 | cnt_reg]   # subs xc, xc, #1
        ws += [0x54000001 | ((top - len(ws)) & 0x7FFFF) << 5]   # b.ne top
        ws += [0xF9000FE0 | rr(READ), 0xF9400FE0 | rr()]   # str/ldr [sp, #24]
        ws += [0xA8C27BFD, 0xD65F03C0]   # ldp x29, x30, [sp], #32; ret
        return ws
    f1, f2 = leaf(25), leaf(24)
    ncall = r.random()
    f0 = [0xA9BE7BFD, 0xD2800000 | r.randrange(1, 9) << 5 | 28]
    top = len(f0)
    body, _ = block(r.randrange(6, 40), [])
    f0 += body
    calls = []
    if ncall > 0.4:
        calls.append('bl')
    if ncall > 0.6:
        calls.append('blr')
    if ncall > 0.75:
        calls.append('svc')
    for k in calls:
        f0.append(k)
        f0 += [alu() for _ in range(r.randrange(0, 4))]
    f0 += [0xF1000400 | 28 << 5 | 28, None]
    f0[-1] = 0x54000001 | ((top - (len(f0) - 1)) & 0x7FFFF) << 5
    f0 += [0xF9000FE0 | rr(READ), 0xF9400FE0 | rr(), 0xA8C27BFD, 0xD65F03C0]
    a0, a1 = BASE, BASE + 4 * (len(f0) + 3)   # + the 3-word prefix below
    a2 = a1 + 4 * len(f1)
    out = []
    for i, w in enumerate(f0):
        pc = a0 + 4 * i
        if w == 'bl':
            out.append(0x94000000 | ((a1 - pc) // 4) & 0x3FFFFFF)
        elif w == 'blr':
            out.append(0xD63F0000 | 26 << 5)
        elif w == 'svc':
            out.append(0xD4000001 | r.randrange(1, 0x80) << 5)
        else:
            out.append(w)
    # x26 = a2 before F0's loop
    pre = [0xD2800000 | (a2 & 0xFFFF) << 5 | 26, 0xF2A00000 | (a2 >> 16 & 0xFFFF) << 5 | 26,
           0xF2C00000 | (a2 >> 32 & 0xFFFF) << 5 | 26]
    # (the prefix shifts everything: branches inside are relative, bl target recomputed)
    words = out[:1] + pre + out[1:]
    for i, w in enumerate(words):
        if (w & 0xFC000000) == 0x94000000:
            words[i] = 0x94000000 | ((a1 - (a0 + 4 * i)) // 4) & 0x3FFFFFF
    f0n = len(words)
    words += f1 + f2
    return words, [(a0, a0 + 4 * f0n), (a0 + 4 * f0n, a0 + 4 * (f0n + len(f1))), (a0 + 4 * (f0n + len(f1)), a0 + 4 * len(words))]

def lift(words, fns, on, fl=False, rst=False):
    regcache.ON, regcache.FLAGS = on, on and fl
    restrict.ON = rst   # [UIT 0190] Cpu *__restrict c; loop barriers whenever it or the cache is on, as gen.py does
    mod = gen.Module.__new__(gen.Module)
    mod.name, mod.base, mod.lo, mod.hi, mod.words = 'main', BASE, BASE, BASE + 4 * len(words), words
    mod.lifter, mod.cache, mod.stats, mod.mu, mod.spins = Lifter(BASE, 'c'), {}, Counter(), None, {}
    src = ['#include "cpu.h"', '#include "simd.h"', '#include "regcache.h"']
    src += [f'void {gen.fname(a)}(Cpu *c);' for a, _ in fns]
    for a, e in fns:
        labels, ext, tables = mod.scan(a, e)
        src += restrict.apply(mod.emit_function(a, e, labels, tables, {}), False, rst or on)
    return '\n'.join(src) + '\n'

HARNESS = r'''
#include "cpu.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
volatile int g_fnprof;
void fnprof_hit(uint64_t a, uint64_t l, uint64_t x) { (void)a; (void)l; (void)x; }
void FN0(Cpu *c); void FN2(Cpu *c);
void rt_dispatch(Cpu *c) { if (c->pc == A2) { FN2(c); return; } printf("bad dispatch %llx\n", (unsigned long long)c->pc); exit(3); }
void rt_svc(Cpu *c, uint32_t imm, uint64_t pc) { c->x[0] = c->x[0] * 0x9E3779B97F4A7C15ull + imm + pc; c->x[19] ^= imm; c->x[7] += 3; c->nzcv ^= 0x60000000ull; }
void rt_brk(Cpu *c, uint32_t i, uint64_t p) { (void)c; (void)i; (void)p; exit(4); }
void rt_udf(Cpu *c, uint64_t p) { (void)c; printf("udf %llx\n", (unsigned long long)p); exit(5); }
void rt_bad_entry(Cpu *c, uint64_t a, uint64_t f) { (void)c; (void)a; (void)f; exit(6); }
void rt_spin_gate(Cpu *c, uint64_t h) { (void)c; (void)h; }
uint64_t rt_ticks(void) { return 0; }
static uint64_t sm(uint64_t *s) { uint64_t z = (*s += 0x9E3779B97F4A7C15ull); z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull; z = (z ^ (z >> 27)) * 0x94D049BB133111EBull; return z ^ (z >> 31); }
static uint8_t *buf, *stk;   /* fixed addresses: pointers end up in registers and memory, so both builds must see the same ones */
#define BUFSZ 8192
#define STKSZ 4096
int main(int argc, char **argv) {
    int n = atoi(argv[1]);
    buf = mmap((void *)0x30000000, BUFSZ + STKSZ, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    if (buf != (uint8_t *)0x30000000) return 7;
    stk = buf + BUFSZ;
    for (int s = 0; s < n; s++) {
        static Cpu cpu; Cpu *c = &cpu; memset(c, 0, sizeof *c);
        uint64_t st = 1000 + s;
        for (int i = 0; i < 31; i++) c->x[i] = (s & 1) ? sm(&st) : sm(&st) & 0xff;
        for (int i = 0; i < (int)BUFSZ; i++) buf[i] = (uint8_t)sm(&st);
        memset(stk, 0, STKSZ);
        c->nzcv = (sm(&st) & 0xf) << 28; c->sp = (uint64_t)(uintptr_t)(stk + STKSZ - 64);
        c->x[27] = (uint64_t)(uintptr_t)buf; c->x[26] = A2;
        FN0(c);
        uint64_t h = 1469598103934665603ull;
#define MIX(v) h = (h ^ (uint64_t)(v)) * 1099511628211ull
        for (int i = 0; i < 31; i++) MIX(c->x[i]);
        MIX(c->sp - (uint64_t)(uintptr_t)stk); MIX(c->nzcv);
        for (int i = 0; i < (int)BUFSZ; i++) MIX(buf[i]);
        for (int i = 0; i < (int)STKSZ; i++) MIX(stk[i]);
        printf("%016llx\n", (unsigned long long)h);
    }
    return 0;
}
'''

# ---------------------------------------------------------------- flag macros vs cpu.h helpers
with tempfile.TemporaryDirectory() as d:
    t = pathlib.Path(d)
    ops = ['adds64', 'adds32', 'subs64', 'subs32', 'adcs64', 'adcs32', 'sbcs64', 'sbcs32', 'adc64', 'adc32', 'sbc64', 'sbc32']
    body = ['#include "regcache.h"', '#include <stdio.h>', 'int main(void) { uint64_t s = 7, bad = 0; Cpu c0; uint64_t nz;',
            'static const uint64_t E[] = {0, 1, 0x7fffffffull, 0x80000000ull, 0xffffffffull, 0x7fffffffffffffffull, 0x8000000000000000ull, ~0ull};',
            'for (int i = 0; i < 400000; i++) { s = s * 6364136223846793005ull + 1442695040888963407ull;',
            'uint64_t a = i < 64 ? E[i & 7] : s ^ (s >> 17), b = i < 64 ? E[(i >> 3) & 7] : s * 0x9E3779B97F4A7C15ull, f = (s >> 13 & 0xf) << 28;']
    for o in ops:
        body.append(f'c0.nzcv = f; nz = f; {{ uint64_t r1 = {o}(&c0, a, b), r2 = rc_{o}(nz, a, b); bad += r1 != r2 || c0.nzcv != nz; }}')
    for o in ('logic64', 'logic32'):
        body.append(f'c0.nzcv = f; nz = f; {{ uint64_t r1 = {o}(&c0, a ^ b), r2 = rc_{o}(nz, a ^ b); bad += r1 != r2 || c0.nzcv != nz; }}')
    for cnd in regcache.CONDS:
        body.append(f'c0.nzcv = f; bad += !!COND_{cnd}(&c0) != !!RCOND_{cnd}(f);')
    # [UIT 0084] the operand forms: same result, same packed flags, conditions from the operands = from the flags
    for N in (64, 32):
        for op, pre in (('subs', 'RCOS'), ('adds', 'RCOA')):
            body.append(f'{{ uint64_t fa = 0, fb = 0, f1 = f, f2 = 0; uint64_t r1 = rc_{op}{N}(f1, a, b), r2 = rco_{op}{N}(fa, fb, a, b), '
                        f'r3 = rcob_{op}{N}(f2, fa, fb, a, b); bad += r1 != r2 || r1 != r3 || f1 != f2 || RCPK_{op.upper()}{N}(fa, fb) != f1;')
            for cnd in regcache.CONDS:
                body.append(f'bad += !!RCOND_{cnd}(f1) != !!{pre}_{cnd}{N}(fa, fb);')
            body.append('}')
        body.append(f'{{ uint64_t fa = 0, f1 = f, f2 = 0; uint64_t r1 = rc_logic{N}(f1, a & b), r2 = rco_logic{N}(fa, a & b), '
                    f'r3 = rcob_logic{N}(f2, fa, a & b); bad += r1 != r2 || r1 != r3 || f1 != f2 || RCPK_LOGIC{N}(fa) != f1;')
        for cnd in regcache.CONDS:
            body.append(f'bad += !!RCOND_{cnd}(f1) != !!RCOL_{cnd}{N}(fa);')
        body.append('}')
    body.append('} printf("%llu\\n", (unsigned long long)bad); return bad != 0; }')
    (t / 'f.c').write_text('\n'.join(body))
    p = subprocess.run([CC, '-O1', '-w', f'-I{HERE / "src/runtime"}', str(t / 'f.c'), '-o', str(t / 'f')], capture_output=True, text=True)
    res = subprocess.run([str(t / 'f')], capture_output=True, text=True) if p.returncode == 0 else None
    check(res is not None and res.returncode == 0, 'regcache.h flag macros = cpu.h helpers on 400 000 inputs (all conditions; the operand forms of 0084 too)'
          + ('' if p.returncode == 0 else ': ' + p.stderr[-800:]))

# ---------------------------------------------------------------- [UIT 0084] dataflow cases the random programs rarely make
def fp(lines, kinds):
    r = regcache.flags_pass(lines, kinds)
    return r and r[1:]
SP, RL = 'c->x[0] = rc_x0; c->nzcv = rc_nz; ', 'rc_x0 = c->x[0]; rc_nz = c->nzcv;'
d0 = '    rc_x0 = (uint64_t)(rc_subs64(rc_nz, rc_x0, 0x1ull));'
rd = '    if (RCOND_LT(rc_nz)) { goto L_1; }'
lab = 'L_1:;'
r1 = fp([d0, '    if (rc_x0 == 0) { ' + SP + 'rt_svc(c, 1, 0); ' + RL + ' }', rd, lab], ['plain', 'if', 'plain', 'label'])
check(r1 and 'RCOND_LT(rc_nz)' in r1[2] and 'rcob_subs64' in r1[0] and 'RCPK_SUBS64' in r1[1],
      'operand flags: after a conditional barrier the read stays packed and the subs keeps writing rc_nz')
r2 = fp([d0, rd, '    ' + SP + 'rt_svc(c, 1, 0); ' + RL, lab], ['plain', 'plain', 'unit', 'label'])
check(r2 and 'RCOS_LT64(rc_fa, rc_fb)' in r2[1] and 'rco_subs64' in r2[0] and 'RCPK_SUBS64' in r2[2],
      'operand flags: subs / b.lt reads the operands, the write-back packs them')
r3 = fp(['    if (rc_x0 == 0) { goto L_1; }', d0, lab, rd], ['plain', 'plain', 'label', 'plain'])
check(r3 is None or 'RCOND_LT(rc_nz)' in r3[3], 'operand flags: a read the entry flags can reach (around a branch) stays packed')
r4 = fp([d0, '    rc_x1 = (uint64_t)(rc_logic32(rc_nz, (uint32_t)rc_x1));', rd, lab], ['plain', 'plain', 'plain', 'label'])
check(r4 and 'RCOL_LT32(rc_fa)' in r4[2] and r4[0] == d0, 'operand flags: a later definition hides the earlier one')
r5 = fp(['    if (RCOND_EQ(rc_nz)) (void)rc_subs64(rc_nz, rc_x0, rc_x1); else rc_nz = 0x20000000ull;', rd], ['plain', 'plain'])
check(r5 is None or 'RCOND_LT(rc_nz)' in r5[1], 'operand flags: after a ccmp (mixed definitions) the read stays packed')

# ---------------------------------------------------------------- differential programs
regcache.STATS.clear()
mism, runs, compiled, cached_any, bad_exit = 0, 0, 0, 0, 0
kinds, last_new, last_fl, last_rst, flags_any = Counter(), '', '', '', 0
with tempfile.TemporaryDirectory() as d:
    t = pathlib.Path(d)
    for pi in range(NPROG):
        words, fns = program()
        before = regcache.STATS['cached']
        old, new = lift(words, fns, False), lift(words, fns, True)
        fb = regcache.STATS['flags_operands']
        fln = lift(words, fns, True, True)   # [UIT 0084] TOTK_LIFT_REGCACHE_FLAGS=1
        rst = lift(words, fns, False, False, True)   # [UIT 0190] TOTK_LIFT_RESTRICT=1 alone
        rfr = lift(words, fns, True, True, True)   # [UIT 0190] with the cache and the operand flags
        last_rst = rfr
        flags_any += regcache.STATS['flags_operands'] > fb
        if fln != new:
            last_fl = fln
        for k, pat in (('bl', 'c->x[30] = 0x'), ('blr', 'rt_dispatch(c)'), ('svc', 'rt_svc(')):
            kinds[k] += pat in old
        last_new = new
        cached_any += regcache.STATS['cached'] > before
        res = []
        for tag, src in (('old', old), ('new', new), ('fl', fln), ('rst', rst), ('rfr', rfr)):
            (t / f'{tag}.c').write_text(src)
            h = HARNESS.replace('FN0', gen.fname(fns[0][0])).replace('FN2', gen.fname(fns[2][0])).replace('A2', hex(fns[2][0]) + 'ull')
            (t / 'h.c').write_text(h)
            p = subprocess.run([CC, '-O2', '-w', '-fno-strict-aliasing', f'-I{HERE / "src/runtime"}', str(t / f'{tag}.c'), str(t / 'h.c'),
                                '-o', str(t / tag)], capture_output=True, text=True)
            if p.returncode:
                print(p.stderr[-1500:]); (HERE / 'build').mkdir(exist_ok=True)
                (pathlib.Path(tempfile.gettempdir()) / f'regcache_fail_{tag}.c').write_text(src)
                check(False, f'program {pi}: {tag} C compiles'); sys.exit(1)
            try:
                q = subprocess.run([str(t / tag), str(NSTATE)], capture_output=True, text=True, timeout=30)
                res.append((q.returncode, q.stdout))
            except subprocess.TimeoutExpired:   # a loop that no longer ends counts as a difference
                res.append((-1, f'timeout {tag}'))
        compiled += 1
        runs += NSTATE
        if any(x != res[0] for x in res[1:]):
            mism += 1
            if mism <= 3:
                (pathlib.Path(tempfile.gettempdir()) / f'regcache_mismatch_{pi}_old.c').write_text(old)
                (pathlib.Path(tempfile.gettempdir()) / f'regcache_mismatch_{pi}_new.c').write_text(new)
                (pathlib.Path(tempfile.gettempdir()) / f'regcache_mismatch_{pi}_fl.c').write_text(fln)
                print(f'  program {pi}: old {res[0][0]} {res[0][1][:40]!r} new {res[1][0]} {res[1][1][:40]!r} '
                      f'flags {res[2][0]} {res[2][1][:40]!r} restrict {res[3][0]} {res[3][1][:40]!r} all {res[4][0]} {res[4][1][:40]!r}')
        elif res[0][0] != 0:
            bad_exit += 1
            print(f'  program {pi}: both exit {res[0][0]} {res[0][1][:60]!r}')
st = regcache.STATS
check(mism == 0, f'{compiled} random programs x {NSTATE} states: old, register-cached, operand-flags, restrict (0190) and all-three code end in the same registers, flags, memory '
      f'({mism} differ)')
check(all(kinds[k] for k in ('bl', 'blr', 'svc')), f'calls are covered: programs with bl {kinds["bl"]}, blr {kinds["blr"]}, svc {kinds["svc"]}')
check(bad_exit == 0, f'every program ran to the end ({bad_exit} stopped early)')
check(cached_any >= compiled // 2 and st['flags'] > 0 and st['regs'] > 0,
      f'the cache was used: {st["cached"]} of {st["functions"]} functions, {st["regs"]} registers, flags in {st["flags"]}, '
      f'{st["not_worth"]} not worth it, programs with a cached function: {cached_any}')
check(st['spill_dropped'] > 0 or os.environ.get('TOTK_LIFT_REGCACHE_CLEAN') == '0',
      f'write-backs of registers unchanged since the last read left out (0192): {st["spill_dropped"]} of '
      f'{st["spill_dropped"] + st["spill_kept"]} register write-backs')
check(st['verify_failed'] == 0, f'structural check: undoing the edits gives the old C for every function ({st["verify_failed"]} failed)')
check(flags_any >= compiled // 4 and st['flags_reads_direct'] > 0 and st['flags_verify_failed'] == 0,
      f'operand flags (0084) used in {st["flags_operands"]} functions ({flags_any} programs): {st["flags_reads_direct"]} conditions '
      f'from the operands, {st["flags_pack_at_spill"]} packed at a write-back; {st["flags_no_gain"]} no gain, '
      f'{st["flags_order"]} odd order, {st["flags_multi_def"]} multi-def; structural check failed {st["flags_verify_failed"]}')
zw = HERE.parent / 'ltools' / 'zwcc'
for tag, code in (('register cache', last_new), ('operand flags', last_fl), ('restrict + loop barriers (0190)', last_rst),
                  ('FP-rounding-in-the-FP-unit (0085, SIMD_FPCR_HOST)', last_new and '#define SIMD_FPCR_HOST 1\n' + last_new)):
    if not (zw.exists() and code):   # the Windows build's compiler (musttail, -O2 x86-64-v3) takes the new code too
        continue
    with tempfile.TemporaryDirectory() as d:
        (pathlib.Path(d) / 'w.c').write_text(code)
        p = subprocess.run([str(zw), '-O2', '-mcpu=x86_64_v3', '-Wall', '-Werror', '-Wno-unused-function', '-Wno-unused-label',
                            '-fno-strict-aliasing', f'-I{HERE / "src/runtime"}', '-c', str(pathlib.Path(d) / 'w.c'), '-o', str(pathlib.Path(d) / 'w.o')],
                           capture_output=True, text=True)
        check(p.returncode == 0, f'the Windows compiler builds the {tag} code without warnings' + ('' if p.returncode == 0 else ': ' + p.stderr[-800:]))
print('PASS' if ok else 'FAIL'); sys.exit(0 if ok else 1)
