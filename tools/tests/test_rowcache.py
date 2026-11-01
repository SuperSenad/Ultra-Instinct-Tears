"""Box test for UIT 0356 (tools/lifter/rowcache.py, src/host/rowcache.c): the row-lookup cache.
Lift side: synthetic AArch64 functions (assembled with llvm-mc) - a search through four vtable slots and a getRow
calling it - are found; look-alikes that store outside their stack, call through something else, pass the name object
on or read a third argument are not; the vtable's writing functions become mutators, its pure getters do not;
TOTK_ROWCACHE off finds nothing. Run side: rowcache.c in a harness - hits, invalidation by a mutator, long names and
unknown tables bypass, the verify mode catches a change made behind the cache's back, concurrent readers and a
mutator stay exact, and the probe writes its confirmation line.
    LLVM_BIN=<llvm bin> py -3 tools/tests/test_rowcache.py"""
import os, pathlib, re, shutil, subprocess, sys, tempfile
HERE = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(HERE / 'tools' / 'lifter'))
import rowcache
from lifter import Insn, Lifter, Unsupported

LLVM = pathlib.Path(os.environ.get('LLVM_BIN') or os.path.dirname(shutil.which('llvm-mc') or shutil.which('clang') or '.'))
ok = True
def check(c, w):
    global ok; print(('ok   ' if c else 'FAIL ') + w); ok &= bool(c)

BASE = 0x800000000
ASM = r'''
.text
.p2align 2
search:                      // (table, query): count, index, key, row through the vtable
  stp x29, x30, [sp, #-0x30]!
  mov x29, sp
  stp x19, x20, [sp, #0x10]
  mov x19, x0
  mov x20, x1
  ldr x8, [x19]
  ldr x8, [x8, #0x60]
  blr x8
  ldr x8, [x19]
  ldr x8, [x8, #0x70]
  mov x0, x19
  blr x8
  ldr x8, [x19]
  ldr x8, [x8, #0x78]
  mov x0, x19
  blr x8
  ldr x8, [x19]
  ldr x8, [x8, #0x80]
  mov x0, x19
  blr x8
  bl helper
  ldp x19, x20, [sp, #0x10]
  ldp x29, x30, [sp], #0x30
  ret
helper:
  add x0, x0, #1
  ret
.macro GETROW tag, extra
getrow\tag:
  sub sp, sp, #0x50
  stp x29, x30, [sp, #0x40]
  add x29, sp, #0x40
  str x19, [sp, #0x30]
  ldr x9, [x1, #8]
  mov w8, #8
  str x9, [sp, #8]
  str w8, [sp, #0x10]
  str xzr, [sp, #0x28]
  \extra
  add x1, sp, #8
  bl search
  mov x19, x0
  ldr x8, [sp, #0x28]
  cbz x8, 1f
  ldr x9, [x8]
  ldr x9, [x9, #0x18]
  mov x0, x8
  blr x9
1:
  mov x0, x19
  ldr x19, [sp, #0x30]
  ldp x29, x30, [sp, #0x40]
  add sp, sp, #0x50
  ret
.endm
  GETROW _good, nop
  GETROW _global, "adrp x10, search ; str x9, [x10]"
  GETROW _escape, "mov x2, x1"
  GETROW _arg2, "str x2, [sp, #0x18]"
  GETROW _vcall, "ldr x10, [sp, #0x28] ; blr x10"
getter:
  ldr x0, [x0, #8]
  ret
setter:
  str x1, [x0, #8]
  ret
dtor:
  b freelike
freelike:
  adrp x9, search
  str x0, [x9]
  ret
caller:
  stp x29, x30, [sp, #-16]!
  bl getrow_good
  ldp x29, x30, [sp], #16
  ret
'''
SLOTS = ['dtor', 'dtor', 'getter', 'getter', 'getter', 'setter', 'setter', 'setter', 'setter', 'setter', 'setter',
         'setter', 'getter', 'getter', 'getter', 'getter', 'getter', 'getter', 'getter']   # 0x00 .. 0x90


def assemble(src):
    d = pathlib.Path(tempfile.mkdtemp())
    (d / 'a.s').write_text(src)
    subprocess.run([LLVM / 'llvm-mc', '-triple=aarch64', '-mattr=+lse', '-filetype=obj', '-o', d / 'a.o', d / 'a.s'], check=True)
    subprocess.run([LLVM / 'llvm-objcopy', '-O', 'binary', '-j', '.text', d / 'a.o', d / 'a.bin'], check=True)
    syms = {}
    for line in subprocess.run([LLVM / 'llvm-nm', '-n', d / 'a.o'], capture_output=True, text=True, check=True).stdout.splitlines():
        p = line.split()
        if len(p) == 3 and p[1] in 'tT':
            syms[p[2]] = int(p[0], 16)
    return (d / 'a.bin').read_bytes(), syms


class Nso:
    pass


class Mod:
    def __init__(self, code, syms):
        self.words = [int.from_bytes(code[i:i + 4], 'little') for i in range(0, len(code), 4)]
        self.lo, self.hi = BASE, BASE + len(code)
        self.lifter, self.cache = Lifter(BASE, 'c'), {}
        self.nso = Nso()
        self.vt = (len(code) + 0xFFF) & ~0xFFF   # the vtable's image offset (after the code)
        self.nso.relocs = [(self.vt + 8 * k, 1027, 0, syms[s]) for k, s in enumerate(SLOTS)]
        self.nso.relocs.append((self.vt - 8, 1027, 0, 0x2000))   # (data before it: not read)
        self.nso.size = self.vt + 0x1000
        self.nso.build_id = 'ab' * 16

    def word(self, a):
        return self.words[(a - self.lo) // 4]

    def lift(self, a):
        if a not in self.cache:
            try:
                self.cache[a] = self.lifter.lift(self.word(a), a)
            except Unsupported as e:
                self.cache[a] = Insn('trap', f'rt_udf(c, 0); /* {e} */')
        return self.cache[a]


code, syms = assemble(ASM)
m = Mod(code, syms)
names = sorted(syms, key=syms.get)
starts = [syms[n] for n in names if not n.startswith('.')]
entries = {}
for i, s in enumerate(starts):
    entries[BASE + s] = ((BASE + starts[i + 1]) if i + 1 < len(starts) else m.hi, [], {})
order = sorted(entries)
A = lambda n: BASE + syms[n]
gen = pathlib.Path(tempfile.mkdtemp())
root = pathlib.Path(tempfile.mkdtemp())
(root / 'diag').mkdir()

os.environ.pop('TOTK_ROWCACHE', None)
check(rowcache.find(m, entries, order, str(root), str(gen), BASE) == {}, 'TOTK_ROWCACHE off: nothing searched, no fronts')
check(rowcache.table_c().startswith('const int main_rowcache_stage = 0;'), 'off: the table says stage 0')

os.environ['TOTK_ROWCACHE'] = '1'
check(rowcache.search_fn(m, entries, A('search')) == 0, 'the search through +0x60/+0x70/+0x78/+0x80 is found (shift 0)')
check(rowcache.pure(m, entries, A('getter')) and not rowcache.pure(m, entries, A('setter'))
      and not rowcache.pure(m, entries, A('dtor')), 'pure: a getter is, a setter and a tail call to a global store are not')
srch = {A('search'): 0}
g, why = rowcache.getrow_fn(m, entries, A('getrow_good'), srch)
check(g and g['s'] == A('search') and g['off'] == 8, f'getRow found: one call to the search, name at +8 ({why})')
for tag, want in (('global', 'outside its stack'), ('escape', 'name object on'), ('arg2', 'x2..x7'),
                  ('vcall', "+0x18")):
    g, why = rowcache.getrow_fn(m, entries, A('getrow_' + tag), srch)
    check(g is None and want in why, f'look-alike {tag}: not taken ({why})')
check(rowcache.getrow_fn(m, entries, A('getrow_good'), srch, aliases={A('getrow_good'): [1]})[0] is None,
      'a getRow with alias entries: not taken')

fr = rowcache.find(m, entries, order, str(root), str(gen), BASE)
rowcache.append_report(str(gen))
check(fr == {A('getrow_good'): ('probe', 0)} and rowcache.STATE['stage'] == 1, 'stage 1: only the real getRow gets a probe front')
rep = (gen / 'lift-scan.txt').read_text()
check('[rowcache] getRow-shaped functions: 1 of' in rep and 'not taken' in rep and ' 0x' in rep, 'lift-scan.txt: [rowcache] counts and reasons')
check('getrow' not in rep.split('[rowcache]')[0] and all(not re.search(r'\b[0-9a-f]{8} [0-9a-f]{8}\b', l) for l in rep.splitlines()),
      'lift-scan.txt: no instruction words (those go to lift-scan-words.txt)')
lines = ['void f_%x(Cpu *c) {' % A('getrow_good'), '    c->x[0] = 1;', '}']
out, done = rowcache.front(lines, 'f_%x' % A('getrow_good'), 'probe', 0)
check(done and out[0].endswith('_lifted(Cpu *c);') and 'uit_rc_probe(c, 0);' in '\n'.join(out) and 'musttail' in out[-2],
      'probe front: the lifted body renamed, a probe call, a tail call on')
rowcache.STATE['emitted'].add(A('getrow_good'))
t = rowcache.table_c()
check('main_rowcache_stage = 1;' in t and 'main_rowcache_name_off[] = { 8 }' in t, 'stage 1 table')

vt = BASE + m.vt
(root / 'diag' / 'lift-confirm.txt').write_text(f'shadow 1 build x\nrowcache {A("getrow_good"):x} vtable {vt:x} build {"ab" * 8} samples 8\n'
                                                f'rowcache {A("getrow_good"):x} vtable {vt + 8:x} build {"cd" * 8} samples 8\n')
fr = rowcache.find(m, entries, order, str(root), str(gen), BASE)
muts = sorted(a for a, (k, _) in fr.items() if k == 'mutator')
check(rowcache.STATE['stage'] == 2 and fr.get(A('getrow_good')) == ('getrow', 0), 'stage 2: the confirmed getRow gets the cache front')
check(muts == sorted({A('setter'), A('dtor')}), 'stage 2: the setters and the destructor become mutators, the getters do not')
check(rowcache.STATE['v'][A('getrow_good')] == [vt], 'stage 2: only the vtable confirmed for this build')
for a in fr:
    rowcache.STATE['emitted'].add(a)
t = rowcache.table_c()
check('main_rowcache_stage = 2;' in t and f'{vt:#x}ull, 0x0ull' in t, 'stage 2 table')
rowcache.STATE['emitted'].discard(A('setter'))
check('main_rowcache_stage = 0;' in rowcache.table_c(), 'a mutator front not emitted: nothing cached')
out, done = rowcache.front(['void f_1(Cpu *c) {', '}'], 'f_1', 'mutator', 0)
check(done and 'uit_rc_mutate(c, f_1_lifted);' in '\n'.join(out), 'mutator front')

SLOTS[14] = 'freelike'   # a search slot (+0x70) that writes: the pair is not used
m2 = Mod(code, syms)
fr2 = rowcache.find(m2, entries, order, str(root), str(gen), BASE)
rowcache.append_report(str(gen))
check(fr2 == {} and 'search slot is not pure' in (gen / 'lift-scan.txt').read_text(),
      'an impure search slot: nothing cached')
SLOTS[14] = 'getter'
m3 = Mod(code, syms)
m3.nso.relocs[3] = (m3.vt + 24, 1025, 5, 0)   # an import in the vtable
check(rowcache.find(m3, entries, order, str(root), str(gen), BASE) == {}, 'a vtable slot that is not a lifted function: nothing cached')

# ---------------------------------------------------------------- run side
HARNESS = r'''
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "cpu.h"
typedef void (*GuestRoutine)(Cpu *c);
void uit_rc_probe(Cpu *c, int k); void uit_rc_getrow(Cpu *c, int k, GuestRoutine fn); void uit_rc_mutate(Cpu *c, GuestRoutine fn);
extern int uit_rc_on;
static uint64_t vtab[4], vother[4];
const int main_rowcache_stage = STAGE, main_rowcache_n = 1;
const unsigned long long main_rowcache_g[] = { 0x800001234ull }, main_rowcache_v[4] = { (unsigned long long)0 };
unsigned long long *vptr_slot = (unsigned long long *)main_rowcache_v;
const unsigned main_rowcache_name_off[] = { 8 };
unsigned long long main_rowcache_lo, main_rowcache_hi;
const char main_lift_build[] = "abababababababababab";
typedef struct { uint64_t vptr; const char *names[8]; uint64_t rows[8]; } Table;
static Table T = { 0, { "Npc_A", "Npc_B", "Weapon_Sword_001", "Item_Apple" }, { 100, 200, 300, 400 } };
static _Atomic long real_calls;
static void real(Cpu *c) {   /* the table's own search */
    atomic_fetch_add(&real_calls, 1);
    Table *t = (Table *)c->x[0];
    const char *n = *(const char **)(c->x[1] + 8);
    c->x[0] = 0;
    for (int i = 0; i < 8; i++) if (t->names[i] && !strcmp(t->names[i], n)) c->x[0] = t->rows[i];
    c->x[1] = 0x5150;
}
static uint64_t look(Table *t, const char *n) {
    struct { uint64_t pad; const char *n; } q = { 0, n };
    Cpu c; memset(&c, 0, sizeof c); c.x[0] = (uint64_t)t; c.x[1] = (uint64_t)&q;
    if (uit_rc_on) uit_rc_getrow(&c, 0, real); else real(&c);
    return c.x[0];
}
static void set_row(Cpu *c) { ((Table *)c->x[0])->rows[c->x[1]] = c->x[2]; }
static void mutate(Table *t, int i, uint64_t v) { Cpu c; memset(&c, 0, sizeof c); c.x[0] = (uint64_t)t; c.x[1] = i; c.x[2] = v; uit_rc_mutate(&c, set_row); }
static _Atomic uint64_t ver; static _Atomic int stop; static _Atomic long bad;
static void *reader(void *p) {
    (void)p;
    for (long i = 0; !atomic_load(&stop); i++) {
        uint64_t v1 = atomic_load(&ver);
        uint64_t r = look(&T, "Npc_A");
        uint64_t v2 = atomic_load(&ver);
        if (v1 == v2 && !(v1 & 1) && r != 1000 + v1 / 2) atomic_fetch_add(&bad, 1);
    }
    return 0;
}
int main(int argc, char **argv) {
    vptr_slot[0] = (unsigned long long)vtab;
    T.vptr = (uint64_t)vtab;
    main_rowcache_lo = (uint64_t)vtab; main_rowcache_hi = (uint64_t)vtab + 64;
#if STAGE == 1
    char nm[] = "Npc_A";
    struct { uint64_t pad; const char *n; } q = { 0, nm };
    Cpu c; memset(&c, 0, sizeof c); c.x[0] = (uint64_t)&T; c.x[1] = (uint64_t)&q;
    for (int i = 0; i < 256 * 9; i++) uit_rc_probe(&c, 0);
    printf("probe done\n");
    return 0;
#else
    if (argc > 1 && !strcmp(argv[1], "verify")) {
        uint64_t a = look(&T, "Npc_A"); look(&T, "Npc_A");
        T.rows[0] = 111;   /* behind the cache's back */
        uint64_t b = look(&T, "Npc_A");
        printf("verify %llu %llu\n", (unsigned long long)a, (unsigned long long)b);
        return 0;
    }
    long r0 = real_calls;
    uint64_t a = look(&T, "Npc_B"), b = look(&T, "Npc_B"), d = look(&T, "Item_Apple"), e = look(&T, "Nobody");
    printf("hit %llu %llu %llu %llu %ld\n", (unsigned long long)a, (unsigned long long)b, (unsigned long long)d,
           (unsigned long long)e, real_calls - r0);
    mutate(&T, 1, 222);
    r0 = real_calls; a = look(&T, "Npc_B"); b = look(&T, "Npc_B");
    printf("inval %llu %llu %ld\n", (unsigned long long)a, (unsigned long long)b, real_calls - r0);
    T.names[5] = "Long_name_of_more_than_sixty_four_bytes_xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"; T.rows[5] = 6;
    r0 = real_calls; look(&T, T.names[5]); look(&T, T.names[5]);
    printf("long %ld\n", real_calls - r0);
    Table U = T; U.vptr = (uint64_t)vother;
    r0 = real_calls; look(&U, "Npc_B"); look(&U, "Npc_B");
    printf("othervt %ld\n", real_calls - r0);
    Cpu z; memset(&z, 0, sizeof z); uint64_t row = 7; z.x[0] = (uint64_t)&T; z.x[1] = (uint64_t)&row;
    /* threads */
    T.rows[0] = 1000; atomic_store(&ver, 0);
    pthread_t th[4];
    for (int i = 0; i < 4; i++) pthread_create(&th[i], 0, reader, 0);
    for (uint64_t k = 1; k <= 20000; k++) {
        atomic_fetch_add(&ver, 1);
        mutate(&T, 0, 1000 + k);
        atomic_fetch_add(&ver, 1);
        if (!(k & 1023)) usleep(100);
    }
    atomic_store(&stop, 1);
    for (int i = 0; i < 4; i++) pthread_join(th[i], 0);
    printf("threads bad %ld\n", (long)bad);
    return 0;
#endif
}
'''
d = pathlib.Path(tempfile.mkdtemp())
(d / 'h.c').write_text(HARNESS.replace('const unsigned long long main_rowcache_g[] = { 0x800001234ull }, main_rowcache_v[4] = { (unsigned long long)0 };',
                                      'const unsigned long long main_rowcache_g[] = { 0x800001234ull }; unsigned long long main_rowcache_v[4];')
                               .replace('unsigned long long *vptr_slot = (unsigned long long *)main_rowcache_v;', 'unsigned long long *vptr_slot = main_rowcache_v;'))
cc = os.environ.get('CC') or str(LLVM / 'clang')
src = HERE / 'src' / 'host' / 'rowcache.c'
# (the harness defines the tables writable; rowcache.c sees them as const: same symbols)
bins = {}
for st in (1, 2):
    exe = d / f'h{st}'
    r = subprocess.run([cc, '-O2', '-w', f'-DSTAGE={st}', '-I', str(HERE / 'src' / 'runtime'), str(d / 'h.c'), str(src), '-lpthread', '-o', str(exe)],
                       capture_output=True, text=True)
    check(r.returncode == 0, f'harness stage {st} builds' + (': ' + r.stderr[-800:] if r.returncode else ''))
    bins[st] = exe


def run(st, env, *args, cwd=None):
    e = {k: v for k, v in os.environ.items() if not k.startswith('TOTK_ROWCACHE')}
    e.update(env)
    return subprocess.run([str(bins[st]), *args], env=e, capture_output=True, text=True, cwd=cwd, timeout=120)

r = run(2, {'TOTK_ROWCACHE': '1'})
out = r.stdout
check('hit 200 200 400 0 3' in out, f'a repeated name is answered from the cache (3 real searches for 4 look-ups): {out.splitlines()[:1]}')
check('inval 222 222 1' in out, 'a mutator drops the old answer: the new row, then a hit')
check('long 2' in out, 'a name of 64 bytes or more: always the real search')
check('othervt 2' in out, 'a table with another vtable: always the real search')
check('threads bad 0' in out, 'four reader threads and a mutator: every answer matches the table at that time')
check('[totk] rowcache: on' in r.stderr, 'the log says it is on')
r = run(2, {})
check('hit 200 200 400 0 4' in r.stdout, 'TOTK_ROWCACHE unset: every look-up is the real search')
r = run(2, {'TOTK_ROWCACHE': '1', 'TOTK_ROWCACHE_VERIFY': '1'}, 'verify')
check('verify 100 111' in r.stdout and 'MISMATCH' in r.stderr and '"Npc_A": cached 64, real 6f' in r.stderr,
      f'verify: a change made without a mutator is logged and the real row returned ({r.stderr.strip().splitlines()[-1:]})')
r = run(2, {'TOTK_ROWCACHE': '1'}, 'verify')
check('verify 100 100' in r.stdout, '(without verify that change is not seen: why the cache stays off by default)')
cw = pathlib.Path(tempfile.mkdtemp()); (cw / 'diag').mkdir()
r = run(1, {'TOTK_ROWCACHE': '1'}, cwd=cw)
conf = (cw / 'diag' / 'lift-confirm.txt').read_text() if (cw / 'diag' / 'lift-confirm.txt').exists() else ''
check(re.fullmatch(r'rowcache 800001234 vtable [0-9a-f]+ build abababababababab samples 8\n', conf),
      f'probe: one confirmation line after 8 samples ({conf.strip()})')
p = rowcache.read_confirm(str(cw / 'diag' / 'lift-confirm.txt'), 'ab' * 16)
check(list(p) == [0x800001234], 'read_confirm reads it back for this build')
r = run(1, {}, cwd=pathlib.Path(tempfile.mkdtemp()))
check('probe done' in r.stdout and 'rowcache' not in r.stderr, 'probe with TOTK_ROWCACHE off: nothing')

print('all ok' if ok else 'FAILURES')
sys.exit(0 if ok else 1)
