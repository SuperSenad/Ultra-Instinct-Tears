"""[UIT 0370] Differential test of TOTK_LIFT_SPLIT_HUGE (tools/lifter/gen.py): a big function with an unresolved
computed branch, written as today (a label at every instruction, built -O0 as tools/build_game.py does) and split
(the fast part built -O2, the complete part -O0), run from the same random register / memory states: every register,
the flags, the buffer and the stack must come out the same.

The function is an interpreter loop, as in the game's switch functions: it reads a byte, computes a target in a block
of random code (ALU, flags, loads / stores, forward branches inside each case) and jumps there with `br`, which the test
marks unresolved ('all'). Two kinds of targets: case starts only (each after an unconditional branch: the fast part
must take every one, no [split] entry), and any instruction of the case area (most of them have no label in the fast
part, so its complete part runs them: still the same result).
    py -3 tools/tests/test_lift_split.py [programs] [states]
Needs a C compiler that runs on this machine (CC, default clang)."""
import os, pathlib, random, subprocess, sys, tempfile
from collections import Counter
HERE = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(HERE / 'tools' / 'lifter'))
os.environ['TOTK_LIFT_REGCACHE'] = '1'
os.environ['TOTK_LIFT_REGCACHE_FLAGS'] = '1'
os.environ['TOTK_LIFT_SPIN_GATES'] = '0'
os.environ['TOTK_LIFT_RESTRICT'] = '1'
os.environ['TOTK_LIFT_RESTRICT_ALL'] = '1'
import regcache
import gen
import restrict
from lifter import Lifter

NPROG = int(sys.argv[1]) if len(sys.argv) > 1 else 6
NSTATE = int(sys.argv[2]) if len(sys.argv) > 2 else 6
CC = os.environ.get('CC') or 'clang'
ok = True


def check(c, w):
    global ok
    print(('ok   ' if c else 'FAIL ') + w, flush=True)
    ok &= bool(c)


BASE = 0x800100000
POOL = list(range(0, 9)) + list(range(12, 24))   # x9 target, x10 case base, x11 op, x24 index, x25 count, x27 buffer
READ = list(range(0, 29))
r = random.Random(370)


def rr(pool=POOL):
    return r.choice(pool)


def alu():
    k = r.randrange(16); sf = r.random() < 0.75; S = 1 << 31 if sf else 0
    d, n, m = rr(), rr(READ), rr(READ)
    if k == 0: return (0x11000000 | S | r.randrange(4096) << 10 | n << 5 | d) | (r.randrange(4) << 29 & 0x60000000)
    if k == 1: return 0x0B000000 | S | r.randrange(3) << 22 | m << 16 | r.randrange(32 if not sf else 64) << 10 | n << 5 | d | (r.randrange(4) << 29)
    if k == 2: return [0x0A000000, 0x2A000000, 0x4A000000, 0x6A000000][r.randrange(4)] | S | r.randrange(4) << 22 | m << 16 | r.randrange(32 if not sf else 64) << 10 | n << 5 | d
    if k == 3: return 0xD2800000 | r.randrange(4) << 21 | r.randrange(65536) << 5 | d
    if k == 4: return 0xF2800000 | r.randrange(4) << 21 | r.randrange(65536) << 5 | d
    if k == 5: return 0x9B000000 | m << 16 | rr(READ) << 10 | n << 5 | d
    if k == 6: return 0x1A800000 | S | m << 16 | r.randrange(14) << 12 | r.randrange(2) << 10 | n << 5 | d
    if k == 7: return 0x7A400000 | S | m << 16 | r.randrange(14) << 12 | n << 5 | r.randrange(16)
    if k == 8: return [0x1A000000, 0x3A000000, 0x5A000000, 0x7A000000][r.randrange(4)] | S | m << 16 | n << 5 | d
    if k == 9:
        s = r.randrange(1, 63); return 0xD3400000 | s << 16 | 63 << 10 | n << 5 | d
    if k == 10: return 0xF9400000 | r.randrange(512) << 10 | 27 << 5 | d
    if k == 11: return 0xF9000000 | r.randrange(512) << 10 | 27 << 5 | rr(READ)
    if k == 12: return 0x39400000 | r.randrange(4096) << 10 | 27 << 5 | d
    if k == 13: return 0x39000000 | r.randrange(4096) << 10 | 27 << 5 | rr(READ)
    if k == 14: return [0xD53B4200 | d, 0xD51B4200 | rr(READ)][r.randrange(2)]
    return 0xF1000000 | r.randrange(4096) << 10 | n << 5 | 31


def case_block(n):
    """n words: random code with forward branches inside (to at most the word after the block)"""
    ws = [alu() for _ in range(n)]
    for _ in range(n // 6):
        i = r.randrange(n - 1); j = r.randrange(i + 1, n + 1); off = j - i
        ws[i] = [0x54000000 | off << 5 | r.randrange(14), 0xB4000000 | off << 5 | rr(READ),
                 0x36000000 | r.randrange(32) << 19 | off << 5 | rr(READ)][r.randrange(3)]
    return ws


def program(nc_log, bw_log, anywhere, iters=1500):
    """(words, br address): an interpreter loop over 2**nc_log cases of 2**bw_log words; anywhere: the target is any
    word of the case area, else a case start"""
    nc, bw = 1 << nc_log, 1 << bw_log
    pre = [0xA9BE7BFD,                                   # stp x29, x30, [sp, #-32]!
           None,                                         # adr x10, cases
           0xD2800000 | iters << 5 | 25,                 # movz x25, #iters
           0xD2800000 | 24]                              # movz x24, #0
    loop = len(pre)
    body = [0x38606800 | 24 << 16 | 27 << 5 | 11,        # ldrb w11, [x27, x24]
            0x91000400 | 24 << 5 | 24]                   # add x24, x24, #1
    if anywhere:   # x11 = byte & (nc * bw - 1) (in words) -> x9 = x10 + x11 * 4
        w = nc_log + bw_log
        body += [0xD3400000 | (w - 1) << 10 | 11 << 5 | 11,   # ubfx x11, x11, #0, #w (w <= 8: the byte itself)
                 0x8B000000 | 11 << 16 | 2 << 10 | 10 << 5 | 9]   # add x9, x10, x11, lsl #2
    else:
        body += [0xD3400000 | (nc_log - 1) << 10 | 11 << 5 | 11,
                 0x8B000000 | 11 << 16 | (bw_log + 2) << 10 | 10 << 5 | 9]
    body.append(0xD61F0000 | 9 << 5)                     # br x9
    words = pre + body
    br_at = BASE + 4 * (len(words) - 1)
    cases_at = len(words)
    blocks = [case_block(bw - 1) + [None] for _ in range(nc)]   # each ends with b check
    for b in blocks:
        words += b
    check_at = len(words)
    for i, w in enumerate(words):
        if w is None and i >= cases_at:
            words[i] = 0x14000000 | (check_at - i) & 0x3FFFFFF
    words += [0xF1000400 | 25 << 5 | 25,                 # subs x25, x25, #1
              0x54000001 | ((loop - (check_at + 1)) & 0x7FFFF) << 5,   # b.ne loop
              0xA8C27BFD, 0xD65F03C0]                    # ldp x29, x30, [sp], #32; ret
    off = (cases_at - 1) * 4                             # adr x10, cases (at word 1)
    words[1] = 0x10000000 | (off & 3) << 29 | (off >> 2) << 5 | 10
    return words, br_at


def lift(words, br_at, split, region=None):
    gen.SPLIT_HUGE = split
    if region is not None:
        gen.SPLIT_REGION = region
    regcache.ON, regcache.FLAGS = True, True
    restrict.ON, restrict.ALL = True, True
    mod = gen.Module.__new__(gen.Module)
    mod.name, mod.base, mod.lo, mod.hi, mod.words = 'main', BASE, BASE, BASE + 4 * len(words), words
    mod.lifter, mod.cache, mod.stats, mod.mu, mod.spins = Lifter(BASE, 'c'), {}, Counter(), None, {}
    end = BASE + 4 * len(words)
    labels, ext, tables = mod.scan(BASE, end)
    tables[br_at] = 'all'   # (the jump table the lifter could not read)
    hdr = ['#include "cpu.h"', '#include "simd.h"', '#include "regcache.h"', f'void {gen.fname(BASE)}(Cpu *c);']
    main = restrict.apply(mod.emit_function(BASE, end, labels, tables, {}), False, True)
    parts = [hdr + main]
    for extra in getattr(mod, '_regions', None) or []:
        parts.append(hdr + restrict.apply(extra, False, True))
    if mod._cold:
        parts.append(hdr + restrict.apply(mod._cold, False, True))
    return ['\n'.join(p) + '\n' for p in parts]


HARNESS = r'''
#include "cpu.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
volatile int g_fnprof;
void fnprof_hit(uint64_t a, uint64_t l, uint64_t x) { (void)a; (void)l; (void)x; }
void FN0(Cpu *c);
void rt_dispatch(Cpu *c) { printf("bad dispatch %llx\n", (unsigned long long)c->pc); exit(3); }
void rt_svc(Cpu *c, uint32_t imm, uint64_t pc) { (void)c; (void)imm; (void)pc; exit(8); }
void rt_brk(Cpu *c, uint32_t i, uint64_t p) { (void)c; (void)i; (void)p; exit(4); }
void rt_udf(Cpu *c, uint64_t p) { (void)c; printf("udf %llx\n", (unsigned long long)p); exit(5); }
void rt_bad_entry(Cpu *c, uint64_t a, uint64_t f) { (void)c; printf("bad entry %llx\n", (unsigned long long)a); (void)f; exit(6); }
void rt_spin_gate(Cpu *c, uint64_t h) { (void)c; (void)h; }
uint64_t rt_ticks(void) { return 0; }
static long cold;
void rt_split_cold(Cpu *c, uint64_t fn) { (void)c; (void)fn; cold++; }
static uint64_t sm(uint64_t *s) { uint64_t z = (*s += 0x9E3779B97F4A7C15ull); z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull; z = (z ^ (z >> 27)) * 0x94D049BB133111EBull; return z ^ (z >> 31); }
static uint8_t *buf, *stk;
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
        c->x[27] = (uint64_t)(uintptr_t)buf;
        FN0(c);
        uint64_t h = 1469598103934665603ull;
#define MIX(v) h = (h ^ (uint64_t)(v)) * 1099511628211ull
        for (int i = 0; i < 31; i++) MIX(c->x[i]);
        MIX(c->sp - (uint64_t)(uintptr_t)stk); MIX(c->nzcv);
        for (int i = 0; i < (int)BUFSZ; i++) MIX(buf[i]);
        for (int i = 0; i < (int)STKSZ; i++) MIX(stk[i]);
        printf("%016llx\n", (unsigned long long)h);
    }
    fprintf(stderr, "cold %ld\n", cold);
    return 0;
}
'''


def build_run(t, tag, srcs, opts):
    objs = []
    for i, (src, o) in enumerate(zip(srcs, opts)):
        (t / f'{tag}{i}.c').write_text(src)
        fl = ['-O0', '-Dalways_inline=noinline'] if o == 'O0' else ['-O2']
        p = subprocess.run([CC, *fl, '-c', '-w', '-fno-strict-aliasing', '-march=x86-64-v3', f'-I{HERE / "src/runtime"}',
                            str(t / f'{tag}{i}.c'), '-o', str(t / f'{tag}{i}.o')], capture_output=True, text=True)
        if p.returncode:
            (pathlib.Path(tempfile.gettempdir()) / f'split_fail_{tag}{i}.c').write_text(src)
            return None, p.stderr[-1500:]
        objs.append(str(t / f'{tag}{i}.o'))
    (t / 'h.c').write_text(HARNESS.replace('FN0', gen.fname(BASE)))
    p = subprocess.run([CC, '-O2', '-w', f'-I{HERE / "src/runtime"}', *objs, str(t / 'h.c'), '-o', str(t / tag)],
                       capture_output=True, text=True)
    if p.returncode:
        return None, p.stderr[-1500:]
    try:
        q = subprocess.run([str(t / tag), str(NSTATE)], capture_output=True, text=True, timeout=120)
    except subprocess.TimeoutExpired:
        return (-1, 'timeout', 0), ''
    cold = int(q.stderr.split('cold ')[-1]) if 'cold ' in q.stderr else -1
    return (q.returncode, q.stdout, cold), ''


def main():
    mism, colds, files = 0, Counter(), Counter()
    with tempfile.TemporaryDirectory() as d:
        t = pathlib.Path(d)
        for pi in range(NPROG):
            anywhere = pi % 2 == 1
            words, br_at = program(6, 6, anywhere)
            old = lift(words, br_at, False)
            check(len(old) == 1 and 'L_dispatch:' in old[0], f'program {pi}: without the split one all-labels function')
            ref, err = build_run(t, 'old', old, ['O0'])
            if ref is None:
                check(False, f'program {pi}: the all-labels C compiles: {err}'); continue
            variants = [('split', lift(words, br_at, True))]
            if hasattr(gen, 'SPLIT_REGION'):   # [UIT 0371] several regions
                keep = gen.SPLIT_REGION
                variants.append(('regions', lift(words, br_at, True, 700)))
                if pi == 0:   # TOTK_LIFT_SPLIT_REGION=0: one fast part
                    check(len(lift(words, br_at, True, 0)) == 2,
                          'region size 0 keeps one fast part (and the complete part)')
                gen.SPLIT_REGION = keep
            for tag, srcs in variants:
                files[tag] = len(srcs)
                fast = ''.join(srcs[:-1])
                nlab = sum(l.startswith('L_') and l.endswith(':;') for l in fast.splitlines())
                nbar = fast.count('__asm__ volatile("" : : : "memory");')
                check(nbar * 4 < nlab, f'program {pi}: {tag}: the dispatch has one barrier, not one per label '
                      f'({nbar} barriers, {nlab} labels in the fast part)')
                res, err = build_run(t, tag, srcs, ['O2'] * (len(srcs) - 1) + ['O0'])
                if res is None:
                    check(False, f'program {pi}: {tag}: the C compiles: {err}'); continue
                same = res[:2] == ref[:2] and ref[0] == 0
                mism += not same
                colds[(tag, anywhere)] += res[2]
                if not same:
                    print(f'  program {pi} {tag}: old {ref[0]} {ref[1][:40]!r} new {res[0]} {res[1][:40]!r}')
                    for i, s in enumerate(srcs):
                        (pathlib.Path(tempfile.gettempdir()) / f'split_mismatch_{pi}_{tag}{i}.c').write_text(s)
    check(mism == 0, f'{NPROG} programs x {NSTATE} states: the split code (fast part -O2, complete part -O0) ends in the same '
          f'registers, flags and memory as the all-labels code at -O0 ({mism} differ)')
    for tag in sorted(files):
        check(colds[(tag, False)] == 0, f'{tag}: jumps to case starts all stay in the fast part ({colds[(tag, False)]} complete-part entries)')
        check(colds[(tag, True)] > 0, f'{tag}: jumps into the middle of a case run the complete part ({colds[(tag, True)]} entries)')
    if 'regions' in files:   # [UIT 0371]
        check(files['regions'] >= 5, f'regions: the fast part is cut into regions ({files["regions"] - 1} files with the '
              f'complete part)')
    check(gen.SPLIT_MARK == restrict.SPLIT_MARK, 'gen.py and restrict.py agree on the fast-part marker')
    print('test_lift_split: ' + ('PASS' if ok else 'FAIL'))
    sys.exit(0 if ok else 1)


if __name__ == '__main__':
    main()
