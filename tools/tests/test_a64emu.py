"""Box test for tools/lifter/a64emu.py (UIT 0083), the interpreter gen.py uses to find jump-table targets (it
replaced Unicorn). Three parts:
  1. differential: random AArch64 integer programs (every add/sub, logical, move, bitfield, EXTR, shift, multiply,
     divide, CSEL, CCMP, CLZ/CLS/RBIT/REV, flag-moving form, loads and stores of every width and addressing mode,
     general and vector registers, forward branches) run in the interpreter and, as the lifter's own C, natively, from
     the same register / flag / memory states: every register, the flags, the vector registers and the memory must
     come out the same;
  2. jump tables: switch functions compiled for AArch64 (clang, -O2 and -Os, byte / halfword / word tables, case
     ranges not starting at zero) are scanned by gen.py: the targets found must be exactly the ones in the table;
  3. what the interpreter does not model stops it or makes the value unknown (never a wrong value), and nothing in
     tools/ imports unicorn any more.
    py -3 tools/tests/test_a64emu.py [programs] [states]       Needs a C compiler that runs here (CC, default clang; the AArch64 build links with lld)."""
import os, pathlib, random, re, struct, subprocess, sys, tempfile, types
from collections import Counter
HERE = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(HERE / 'tools' / 'lifter'))
os.environ['TOTK_LIFT_REGCACHE'] = '0'
os.environ['TOTK_LIFT_SPIN_GATES'] = '0'
import a64emu
import gen
from lifter import Lifter, Unsupported

NPROG = int(sys.argv[1]) if len(sys.argv) > 1 else 120
NSTATE = int(sys.argv[2]) if len(sys.argv) > 2 else 8
CC = os.environ.get('CC') or 'clang'
ok = True
def check(c, w):
    global ok; print(('ok   ' if c else 'FAIL ') + w, flush=True); ok &= bool(c)

BASE = 0x800100000
BUF, BUFSZ = 0x30000000, 16384
M64 = (1 << 64) - 1
POOL = list(range(0, 26))     # registers random code writes (x26: small index, x27: buffer pointer)
READ = list(range(0, 31))
r = random.Random(5)
LIFT = Lifter(BASE, 'c')
def rr(pool=POOL): return r.choice(pool)

def gen_insn():
    sf = r.random() < 0.7; S = 1 << 31 if sf else 0; SZ = 64 if sf else 32
    d, n, m = rr(), rr(READ), rr(READ)
    k = r.randrange(30)
    if k == 0: return 0x11000000 | S | r.randrange(4) << 29 | r.randrange(2) << 22 | r.randrange(4096) << 10 | n << 5 | d
    if k == 1:   # add/sub sp, sp, #small
        return 0x91000000 | r.randrange(2) << 30 | r.randrange(0, 64, 16) << 10 | 31 << 5 | 31
    if k == 2: return 0x0B000000 | S | r.randrange(4) << 29 | r.randrange(3) << 22 | m << 16 | r.randrange(SZ) << 10 | n << 5 | d
    if k == 3:   # extended register, rn may be sp
        opt = r.randrange(8) if sf else r.choice([0, 1, 2, 4, 5, 6])
        return 0x0B200000 | S | r.randrange(4) << 29 | m << 16 | opt << 13 | r.randrange(5) << 10 | r.choice([n, 31]) << 5 | d
    if k == 4: return 0x0A000000 | S | r.randrange(4) << 29 | r.randrange(4) << 22 | r.randrange(2) << 21 | m << 16 | r.randrange(SZ) << 10 | n << 5 | d
    if k == 5: return 0x12000000 | S | r.randrange(4) << 29 | (r.randrange(2) if sf else 0) << 22 | r.randrange(64 if sf else 32) << 16 | r.randrange(64 if sf else 32) << 10 | n << 5 | d
    if k == 6: return 0x12800000 | S | r.choice([0, 2, 3]) << 29 | r.randrange(4 if sf else 2) << 21 | r.randrange(65536) << 5 | d
    if k == 7: return 0x13000000 | S | r.randrange(3) << 29 | (1 << 22 if sf else 0) | r.randrange(SZ) << 16 | r.randrange(SZ) << 10 | n << 5 | d
    if k == 8: return 0x13800000 | S | (1 << 22 if sf else 0) | m << 16 | r.randrange(SZ) << 10 | n << 5 | d
    if k == 9: return 0x1A800000 | S | r.randrange(2) << 30 | m << 16 | r.randrange(16) << 12 | r.randrange(2) << 10 | n << 5 | d
    if k == 10: return 0x3A400000 | S | r.randrange(2) << 30 | r.randrange(32) << 16 | r.randrange(16) << 12 | r.randrange(2) << 11 | n << 5 | r.randrange(16)
    if k == 11: return 0x1A000000 | S | r.randrange(4) << 29 | m << 16 | n << 5 | d
    if k == 12: return 0x1AC00000 | S | m << 16 | r.choice([2, 3, 8, 9, 10, 11]) << 10 | n << 5 | d
    if k == 13: return 0x5AC00000 | S | r.choice([0, 1, 2, 4, 5] + ([3] if sf else [])) << 10 | n << 5 | d
    if k == 14:
        if sf and r.random() < 0.6:
            op31, o0 = r.choice([(1, 0), (1, 1), (5, 0), (5, 1), (2, 0), (6, 0)])
            ra = 31 if op31 in (2, 6) else rr(READ)
            return 0x9B000000 | op31 << 21 | m << 16 | o0 << 15 | ra << 10 | n << 5 | d
        return 0x1B000000 | S | m << 16 | r.randrange(2) << 15 | rr(READ) << 10 | n << 5 | d
    if k == 15:   # adr / adrp
        return 0x10000000 | r.randrange(2) << 31 | r.randrange(4) << 29 | r.randrange(1 << 19) << 5 | d
    if k == 16: return r.choice([0xD53B4200 | d, 0xD51B4200 | rr(READ), 0xD503201F, 0xD5033BBF])   # mrs/msr nzcv, nop, dmb
    # ---- loads / stores (base x27 or sp; index x26)
    base = r.choice([27, 27, 31])
    if k in (17, 18):   # unsigned offset, general
        size = r.randrange(4); opc = r.choice([0, 1] + ([2, 3] if size < 2 else [2] if size == 2 else []))
        rt = rr(READ) if opc == 0 else rr()
        return size << 30 | 0x39000000 | opc << 22 | r.randrange(200) << 10 | base << 5 | rt
    if k in (19, 20):   # unscaled / post / pre
        size = r.randrange(4); opc = r.choice([0, 1] + ([2, 3] if size < 2 else [2] if size == 2 else []))
        rt = rr(READ) if opc == 0 else rr()
        if rt == base:
            rt = 0
        return size << 30 | 0x38000000 | opc << 22 | (r.randrange(-256, 256) & 0x1FF) << 12 | r.choice([0, 1, 3]) << 10 | base << 5 | rt
    if k == 21:   # register offset
        size = r.randrange(4); opc = r.choice([0, 1] + ([2, 3] if size < 2 else [2] if size == 2 else []))
        rt = rr(READ) if opc == 0 else rr()
        return size << 30 | 0x38200800 | opc << 22 | 26 << 16 | r.choice([2, 3, 6, 7]) << 13 | r.randrange(2) << 12 | base << 5 | rt
    if k in (22, 23):   # pair
        opc = r.choice([0, 2, 1]); L = r.randrange(2)
        if opc == 1:
            L = 1
        mode = r.choice([1, 2, 3, 0])
        rt, rt2 = (rr(), rr()) if L else (rr(READ), rr(READ))
        while L and rt2 == rt:
            rt2 = rr()
        return opc << 30 | 0x28000000 | mode << 23 | L << 22 | (r.randrange(-16, 16) & 0x7F) << 15 | rt2 << 10 | base << 5 | rt
    if k in (24, 25):   # vector single (b h s d q), unsigned offset / unscaled / pre / post
        size, opc = r.choice([(0, 0), (0, 1), (1, 0), (1, 1), (2, 0), (2, 1), (3, 0), (3, 1), (0, 2), (0, 3)])
        t = r.randrange(32)
        if r.random() < 0.5:
            return size << 30 | 0x3D000000 | opc << 22 | r.randrange(100) << 10 | base << 5 | t
        return size << 30 | 0x3C000000 | opc << 22 | (r.randrange(-256, 256) & 0x1FF) << 12 | r.choice([0, 1, 3]) << 10 | base << 5 | t
    if k == 26:   # vector pair
        opc, L = r.randrange(3), r.randrange(2)
        t, t2 = r.randrange(32), r.randrange(32)
        while L and t2 == t:
            t2 = r.randrange(32)
        return opc << 30 | 0x2C000000 | r.choice([1, 2, 3]) << 23 | L << 22 | (r.randrange(-8, 8) & 0x7F) << 15 | t2 << 10 | base << 5 | t
    if k == 27:   # vector register offset
        size, opc = r.choice([(0, 1), (1, 1), (2, 0), (3, 1), (0, 3), (0, 2)])
        return size << 30 | 0x3C200800 | opc << 22 | 26 << 16 | r.choice([3, 7]) << 13 | r.randrange(2) << 12 | base << 5 | r.randrange(32)
    return 'branch'

def program():
    n = r.randrange(8, 60)
    ws = []
    while len(ws) < n:
        w = gen_insn()
        if w == 'branch':
            ws.append(w); continue
        try:
            LIFT.lift(w, BASE + 4 * len(ws))
        except Unsupported:
            continue
        ws.append(w)
    for i, w in enumerate(ws):
        if w == 'branch':
            j = r.randrange(i + 1, n + 1); off = (j - i)
            ws[i] = [0x54000000 | off << 5 | r.randrange(16), 0x34000000 | r.randrange(2) << 31 | r.randrange(2) << 24 | off << 5 | rr(READ),
                     0x36000000 | r.randrange(2) << 24 | r.randrange(2) << 31 | r.randrange(32) << 19 | off << 5 | rr(READ),
                     0x14000000 | off][r.randrange(4)]
            if (ws[i] & 0x7E000000) == 0x36000000 and not ws[i] >> 31:   # tbz with b5 = 0: bit < 32
                pass
    return ws + [0xD65F03C0]

def sm(st):
    st[0] = (st[0] + 0x9E3779B97F4A7C15) & M64; z = st[0]
    z = ((z ^ (z >> 30)) * 0xBF58476D1CE4E5B9) & M64; z = ((z ^ (z >> 27)) * 0x94D049BB133111EB) & M64
    return z ^ (z >> 31)

EDGE = [0, 1, 2, 0x7f, 0x80, 0xff, 0x7fffffff, 0x80000000, 0xffffffff, 0x100000000, 0x7fffffffffffffff,
        0x8000000000000000, M64, M64 - 1, 0xfffffffe]

def init_state(s):
    st = [1000 + s]
    x = [(sm(st) if s & 1 else sm(st) & 0xff) for _ in range(31)]
    if s % 4 == 2:   # edge values: carries and overflows into exactly all-ones / the sign bit
        x = [EDGE[sm(st) % len(EDGE)] for _ in range(31)]
    buf = bytes(sm(st) & 0xff for _ in range(BUFSZ))
    nzcv = (sm(st) & 0xf) << 28
    x[26] = sm(st) & 0xff; x[27] = BUF + 8192
    return x, buf, nzcv, BUF + 12288

def fnv(vals, h=1469598103934665603):
    for v in vals:
        h = ((h ^ v) * 1099511628211) & M64
    return h

def run_emu(words, s):
    x, buf, nzcv, sp = init_state(s)
    e = a64emu.Emu(struct.pack(f'<{len(words)}I', *words), BASE)
    e.x, e.nzcv, e.sp = list(x), nzcv, sp
    e.mem = dict(zip(range(BUF, BUF + BUFSZ), buf))
    e.pc = BASE
    end = BASE + 4 * (len(words) - 1)
    for _ in range(10000):
        if e.pc == end:
            break
        e.step()
    else:
        return 'no end'
    mem = e.read(BUF, BUFSZ)
    vb = b''.join(e.v)
    return '%016x %016x %016x %016x' % (fnv(e.x + [e.sp, e.nzcv]), fnv(mem), fnv(vb), e.nzcv)

HARNESS = r'''
#include "cpu.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
volatile int g_fnprof;
void fnprof_hit(uint64_t a, uint64_t l, uint64_t x) { (void)a; (void)l; (void)x; }
void FN0(Cpu *c);
void rt_dispatch(Cpu *c) { exit(3); }
void rt_svc(Cpu *c, uint32_t imm, uint64_t pc) { exit(8); }
void rt_brk(Cpu *c, uint32_t i, uint64_t p) { exit(4); }
void rt_udf(Cpu *c, uint64_t p) { printf("udf %llx\n", (unsigned long long)p); exit(5); }
void rt_bad_entry(Cpu *c, uint64_t a, uint64_t f) { exit(6); }
void rt_spin_gate(Cpu *c, uint64_t h) { }
uint64_t rt_ticks(void) { return 0; }
static uint64_t sm(uint64_t *s) { uint64_t z = (*s += 0x9E3779B97F4A7C15ull); z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull; z = (z ^ (z >> 27)) * 0x94D049BB133111EBull; return z ^ (z >> 31); }
#define MIX(h, v) h = (h ^ (uint64_t)(v)) * 1099511628211ull
int main(int argc, char **argv) {
    int n = atoi(argv[1]);
    uint8_t *buf = mmap((void *)0x30000000, BUFSZ, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    if (buf != (uint8_t *)0x30000000) return 7;
    for (int s = 0; s < n; s++) {
        static Cpu cpu; Cpu *c = &cpu; memset(c, 0, sizeof *c);
        uint64_t st = 1000 + s;
        for (int i = 0; i < 31; i++) c->x[i] = (s & 1) ? sm(&st) : sm(&st) & 0xff;
        static const uint64_t E[] = {0, 1, 2, 0x7f, 0x80, 0xff, 0x7fffffffull, 0x80000000ull, 0xffffffffull, 0x100000000ull,
                                     0x7fffffffffffffffull, 0x8000000000000000ull, ~0ull, ~0ull - 1, 0xfffffffeull};
        if (s % 4 == 2) for (int i = 0; i < 31; i++) c->x[i] = E[sm(&st) % 15];
        for (int i = 0; i < BUFSZ; i++) buf[i] = (uint8_t)sm(&st);
        c->nzcv = (sm(&st) & 0xf) << 28;
        c->x[26] = sm(&st) & 0xff; c->x[27] = 0x30000000 + 8192; c->sp = 0x30000000 + 12288;
        FN0(c);
        uint64_t h1 = 1469598103934665603ull, h2 = h1, h3 = h1;
        for (int i = 0; i < 31; i++) MIX(h1, c->x[i]);
        MIX(h1, c->sp); MIX(h1, c->nzcv);
        for (int i = 0; i < BUFSZ; i++) MIX(h2, buf[i]);
        const uint8_t *vb = (const uint8_t *)c->v;
        for (int i = 0; i < 512; i++) MIX(h3, vb[i]);
        printf("%016llx %016llx %016llx %016llx\n", (unsigned long long)h1, (unsigned long long)h2, (unsigned long long)h3, (unsigned long long)c->nzcv);
    }
    return 0;
}
'''

def lift_c(words):
    mod = gen.Module.__new__(gen.Module)
    mod.name, mod.base, mod.lo, mod.hi, mod.words = 'main', BASE, BASE, BASE + 4 * len(words), words
    mod.lifter, mod.cache, mod.stats, mod.mu, mod.spins = Lifter(BASE, 'c'), {}, Counter(), None, {}
    mod.nso = types.SimpleNamespace(image=struct.pack(f'<{len(words)}I', *words))
    src = ['#include "cpu.h"', '#include "simd.h"', f'void {gen.fname(BASE)}(Cpu *c);']
    labels, ext, tables = mod.scan(BASE, mod.hi)
    src += mod.emit_function(BASE, mod.hi, labels, tables, {})
    return '\n'.join(src) + '\n'

# ---------------------------------------------------------------- 1. differential
mism, stops, nprog, classes = 0, Counter(), 0, Counter()
with tempfile.TemporaryDirectory() as d:
    t = pathlib.Path(d)
    (t / 'h.c').write_text(HARNESS.replace('FN0', gen.fname(BASE)).replace('BUFSZ', str(BUFSZ)))
    p = subprocess.run([CC, '-O1', '-w', f'-I{HERE / "src/runtime"}', '-c', str(t / 'h.c'), '-o', str(t / 'h.o')], capture_output=True, text=True)
    if p.returncode:
        print(p.stderr[-1500:]); check(False, 'harness compiles'); sys.exit(1)
    for pi in range(NPROG):
        words = program()
        for w in words:
            classes[(w >> 25) & 0xF] += 1
        src = lift_c(words)
        (t / 'p.c').write_text(src)
        p = subprocess.run([CC, '-O1', '-w', '-fno-strict-aliasing', f'-I{HERE / "src/runtime"}', str(t / 'p.c'), str(t / 'h.o'), '-o', str(t / 'p')],
                           capture_output=True, text=True)
        if p.returncode:
            print(p.stderr[-1500:]); (pathlib.Path(tempfile.gettempdir()) / 'a64emu_fail.c').write_text(src)
            check(False, f'program {pi}: C compiles'); sys.exit(1)
        q = subprocess.run([str(t / 'p'), str(NSTATE)], capture_output=True, text=True, timeout=60)
        native = q.stdout.split('\n')
        nprog += 1
        for s in range(NSTATE):
            try:
                got = run_emu(words, s)
            except a64emu.Stop as e:
                stops[str(e)] += 1
                continue
            if q.returncode or got != native[s]:
                mism += 1
                if mism <= 4:
                    print(f'  program {pi} state {s}: interpreter {got} native {native[s] if s < len(native) else q.returncode}')
                    print('   ', ' '.join(f'{w:08x}' for w in words))
                break
check(mism == 0, f'{nprog} random programs x {NSTATE} states: the interpreter and the lifted C end in the same registers, flags, '
      f'vector registers and memory ({mism} differ)')
check(sum(stops.values()) == 0, f'the interpreter ran every program to the end (stops: {dict(stops)})')

# ---------------------------------------------------------------- 2. jump tables in compiled switch functions
def elf_image(path):
    d = path.read_bytes()
    phoff, phn = struct.unpack_from('<Q', d, 0x20)[0], struct.unpack_from('<H', d, 0x38)[0]
    segs = []
    for i in range(phn):
        typ, fl, off, va, pa, fs, ms, al = struct.unpack_from('<IIQQQQQQ', d, phoff + 56 * i)
        if typ == 1:
            segs.append((va, d[off:off + fs], ms))
    size = max(va + ms for va, _, ms in segs)
    img = bytearray(size)
    for va, data, ms in segs:
        img[va:va + len(data)] = data
    shoff, shn, shstr = struct.unpack_from('<Q', d, 0x28)[0], struct.unpack_from('<H', d, 0x3c)[0], struct.unpack_from('<H', d, 0x3e)[0]
    secs = [struct.unpack_from('<IIQQQQIIQQ', d, shoff + 64 * i) for i in range(shn)]
    names = d[secs[shstr][4]:secs[shstr][4] + secs[shstr][5]]
    sec = {names[s[0]:names.index(b'\0', s[0])].decode(): s for s in secs}
    syms = {}
    st = sec.get('.symtab') or sec['.dynsym']; strt = secs[st[6]]
    for i in range(st[5] // 24):
        nm, info, oth, shx, val, sz = struct.unpack_from('<IBBHQQ', d, st[4] + 24 * i)
        if (info & 0xF) == 2 and sz:
            syms[d[strt[4] + nm:d.index(b'\0', strt[4] + nm)].decode()] = (val, val + sz)
    return bytes(img), sec['.text'], syms

def switch_src(name, lo, ncase, body_words, idx_type):
    cases, decl = [], []
    for i in range(ncase):
        decl.append(f'int {name}_f{i}(int);')
        extra = ' '.join(f'x = x * {3 + j} + g({i * 7 + j});' for j in range(body_words))
        cases.append(f'case {lo + i}: {extra} return {name}_f{i}(x) + {i};')
    return ' '.join(decl) + f'\nint {name}({idx_type} v, int x) {{ switch (v) {{ {" ".join(cases)} default: return -1; }} }}\n'

def table_truth(img, fn):
    """the targets straight from the code: clang's `cmp wI, #N-1; b.hi; adr(p+add) xT, table; adr xA, anchor;
    ldr{b,h,} wE, [xT, xI...]; add xA, xA, wE, ... #2; br xA`: read the N table entries."""
    import capstone
    cs = capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_ARM); cs.detail = True
    ins = list(cs.disasm(img[fn[0]:fn[1]], fn[0]))
    def addr_of(reg, k):   # value an adr / adrp+add left in reg before ins[k]
        for i in range(k - 1, max(-1, k - 16), -1):
            j = ins[i]
            if j.op_str.split(',')[0] != reg:
                continue
            if j.mnemonic == 'adr':
                return j.operands[1].imm
            if j.mnemonic == 'add' and j.operands[2].type == capstone.arm64.ARM64_OP_IMM:
                hi = addr_of(reg, i)
                return None if hi is None else hi + j.operands[2].imm
            if j.mnemonic == 'adrp':
                return j.operands[1].imm
            return None
        return None
    out = []
    for k, j in enumerate(ins):
        if j.mnemonic != 'br':
            continue
        ld = next((i for i in reversed(ins[max(0, k - 12):k]) if i.mnemonic in ('ldrb', 'ldrh', 'ldr', 'ldrsw')), None)
        cmp = next((i for i in reversed(ins[:k]) if i.mnemonic == 'cmp'), None)
        if not (ld and cmp):
            return None
        kl = ins.index(ld)
        table = addr_of(j.reg_name(ld.operands[1].mem.base), kl)
        add = next((i for i in reversed(ins[kl:k]) if i.mnemonic == 'add' and i.op_str.split(',')[0] == j.op_str), None)
        anchor = addr_of(j.op_str, ins.index(add)) if add else None
        if table is None or anchor is None:
            return None
        count = cmp.operands[1].imm + 1
        sz = {'ldrb': 1, 'ldrh': 2, 'ldr': 4, 'ldrsw': 4}[ld.mnemonic]
        ts = set()
        for i in range(count):
            raw = int.from_bytes(img[table + i * sz:table + (i + 1) * sz], 'little', signed=sz == 4)
            ts.add(anchor + (raw * 4 if sz < 4 else raw))
        out.append((j.address, sorted(ts)))
    return out

cases_ok, cases_n, cases_detail = 0, 0, []
with tempfile.TemporaryDirectory() as d:
    t = pathlib.Path(d)
    src = 'int g(int);\n'
    specs = []
    for i, (lo, n, bw, ty) in enumerate([(0, 8, 0, 'unsigned'), (0, 12, 1, 'int'), (100, 20, 2, 'int'), (3, 40, 1, 'unsigned char'),
                                         (0, 16, 40, 'unsigned'), (7, 30, 80, 'long'), (0, 64, 3, 'unsigned short'), (-5, 11, 1, 'int')]):
        nm = f'sw{i}'; specs.append(nm); src += switch_src(nm, lo, n, bw, ty)
    (t / 'jt.c').write_text(src)
    for opt in ('-O2', '-Os'):
        so = t / f'jt{opt}.so'
        p = subprocess.run([CC, '-target', 'aarch64-linux-musl', opt] + (['-fuse-ld=lld'] if 'clang' in os.path.basename(CC) else []) + ['-fPIC', '-shared', '-nostdlib', str(t / 'jt.c'), '-o', str(so)],
                           capture_output=True, text=True)
        if p.returncode:
            check(False, 'AArch64 switch functions compile: ' + p.stderr[-500:]); break
        img, text, syms = elf_image(so)
        mod = gen.Module.__new__(gen.Module)
        mod.name, mod.base = 'main', 0
        mod.lo, mod.hi = text[3], text[3] + text[5]
        mod.words = struct.unpack(f'<{text[5] // 4}I', img[text[3]:text[3] + text[5]])
        mod.lifter, mod.cache, mod.stats, mod.mu, mod.spins = Lifter(0, 'c'), {}, Counter(), None, {}
        mod.nso = types.SimpleNamespace(image=img)
        for nm in specs:
            fn = syms[nm]
            truth = table_truth(img, fn)
            if not truth:
                continue
            labels, ext, tables = mod.scan(*fn)
            for br, ts in truth:
                cases_n += 1
                got = tables.get(br)
                emu = mod._jump_table_fallthrough(br, *fn)
                if emu is None:
                    emu = mod._jump_table_taken(br, *fn)
                if got == ts and emu == ts:   # (found by the interpreter itself, not the image scan)
                    cases_ok += 1
                else:
                    cases_detail.append(f'{nm}{opt} br@{br:x}: got {got if got == "all" else len(got or [])}, interpreter {len(emu or [])}, want {len(ts)}')
check(cases_n >= 10 and cases_ok == cases_n, f'compiled switch functions: {cases_ok} of {cases_n} jump tables found exactly, by the interpreter '
      + '; '.join(cases_detail[:4]))

# ---------------------------------------------------------------- 3. limits stop or poison, never guess
e = a64emu.Emu(b'', BASE)
def one(w, setup=None):
    e.__init__(b'', BASE)
    if setup:
        setup(e)
    e.mem = {}
    e.exec(w, BASE, BASE + 4)
    return e
check(one(0x9E660000).x[0] is None, 'FMOV x0, d0 makes x0 unknown')
check(one(0x1E602000).nzcv is None, 'FCMP makes the flags unknown')
check(one(0x4E208400).v[0] is None, 'vector ADD makes the vector registers unknown')
check(one(0x0E0C3C20).x[0] is None, 'UMOV w0, v1.s[1] makes w0 unknown')
def raises(w, setup=None):
    try:
        one(w, setup); return False
    except a64emu.Stop:
        return True
check(raises(0xD4000001) and raises(0xC85F7C00) and raises(0xD53BE040) and raises(0x00000000),
      'SVC, LDXR, MRS CNTVCT and an unknown encoding stop the run')
check(raises(0xF9400020, lambda m: m.x.__setitem__(1, None)), 'a load through an unknown address stops the run')
check(raises(0xF9400020, lambda m: m.x.__setitem__(1, 1 << 48)), 'an address at or above 2^48 faults')
check(raises(0x54000040, lambda m: setattr(m, 'nzcv', None)), 'a branch on unknown flags stops the run')
check(one(0xF9400020, lambda m: m.x.__setitem__(1, 0x1234560)).x[0] == 0, 'memory outside the image reads as zeros')
hits = [str(p.relative_to(HERE)) for p in (HERE / 'tools').rglob('*.py')
        if re.search(r'^\s*(import unicorn|from unicorn)', p.read_text(errors='replace'), re.M)]
check(not hits, 'nothing in tools/ imports unicorn' + (': ' + ', '.join(hits) if hits else ''))
print('ALL OK' if ok else 'FAILED')
sys.exit(0 if ok else 1)
