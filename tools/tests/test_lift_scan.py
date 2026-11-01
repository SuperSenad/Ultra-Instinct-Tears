"""Box test for tools/lifter/scan.py (UIT 0070): the patch-site search on synthetic AArch64 code.
Each finder: one clear match -> found; two -> ambiguous (nothing on); none -> nothing. Plus the shadow search's
liveness rules (a register the skipped block writes and later code reads rejects the block) and the run-time
confirmation file.     py -3 tools/tests/test_lift_scan.py"""
import os, pathlib, sys, tempfile
HERE = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(HERE / 'tools' / 'lifter'))
import scan
from lifter import Lifter

ok = True
def check(c, w):
    global ok; print(('ok   ' if c else 'FAIL ') + w); ok &= bool(c)

BASE = 0x800100000
NOP, RET = 0xD503201F, 0xD65F03C0
ldrb = lambda rt, rn, imm=0: 0x39400000 | imm << 10 | rn << 5 | rt
strb = lambda rt, rn, imm: 0x39000000 | imm << 10 | rn << 5 | rt
movx = lambda rd, rm: 0xAA0003E0 | rm << 16 | rd
bl = lambda off: 0x94000000 | (off // 4) & 0x3FFFFFF
b = lambda off: 0x14000000 | (off // 4) & 0x3FFFFFF
cmpw = lambda rn, imm: 0x7100001F | imm << 10 | rn << 5
bcond = lambda cond, off: 0x54000000 | ((off // 4) & 0x7FFFF) << 5 | cond
movz = lambda rd, imm: 0x52800000 | imm << 5 | rd
addw = lambda rd, rn, rm: 0x0B000000 | rm << 16 | rn << 5 | rd
GT, GE = 0xC, 0xA


def trio_a(k=0x15c):
    return [ldrb(8, 0), movx(0, 20), strb(8, 19, k), bl(0x400), ldrb(9, 0), cmpw(10, 8), strb(9, 19, k + 1),
            bcond(GT, 0x200)]


def trio_b(k=0x160):   # the same work, scheduled differently: tier B only
    return [ldrb(8, 0), strb(8, 19, k), movx(0, 20), bl(0x400), ldrb(9, 0), NOP, strb(9, 19, k + 1), cmpw(10, 8),
            NOP, bcond(GE, 0x300)]


def pad(n):
    return [NOP] * n


# ---------------------------------------------------------------- trio
ta, tb = scan.find_trio(pad(20) + trio_a() + pad(200), BASE)
m, why = scan.pick_trio(ta, tb)
check(m and m['tier'] == 'A' and m['fxaa'] == BASE + 80 and m['branch'] == BASE + 80 + 28 and m['k'] == 0x15c,
      f'trio tier A: one exact match ({why})')
check(m and m['target'] == m['branch'] + 0x200, 'trio: the b.gt target')
ta, tb = scan.find_trio(pad(20) + trio_a() + pad(50) + trio_a(0x200) + pad(200), BASE)
m, why = scan.pick_trio(ta, tb)
check(m is None and 'ambiguous' in why, f'trio tier A twice: nothing switched on ({why})')
ta, tb = scan.find_trio(pad(20) + trio_b() + pad(200), BASE)
m, why = scan.pick_trio(ta, tb)
check(m and m['tier'] == 'B' and m['fxaa'] == BASE + 80 and m['fsr'] == BASE + 80 + 16, f'trio tier B: relaxed match ({why})')
ta, tb = scan.find_trio(pad(20) + trio_b() + pad(30) + trio_b(0x40) + pad(200), BASE)
check(scan.pick_trio(ta, tb)[0] is None, 'trio tier B twice: nothing switched on')
ta, tb = scan.find_trio(pad(20) + trio_a() + pad(30) + trio_b() + pad(200), BASE)
m, why = scan.pick_trio(ta, tb)
check(m and m['tier'] == 'A', f'trio: an exact match wins over a looser look-alike ({why})')
bad = trio_a(); bad[5] = cmpw(10, 7)
ta, tb = scan.find_trio(pad(20) + bad + pad(200), BASE)
check(not ta, 'trio: cmp #7 instead of #8 is not tier A')
bad = trio_a(); bad[6] = strb(9, 19, 0x15e)
ta, tb = scan.find_trio(pad(20) + bad + pad(200), BASE)
check(not ta and not tb, 'trio: stores not at k / k+1: no match')
bad = trio_a(); bad[7] = bcond(GT, -0x200)
ta, tb = scan.find_trio(pad(20) + bad + pad(200), BASE)
check(not ta and not tb, 'trio: a backward b.gt: no match')
ta, tb = scan.find_trio(pad(400), BASE)
check(scan.pick_trio(ta, tb)[0] is None and 'no match' in scan.pick_trio(ta, tb)[1], 'trio: nothing in plain code')

# ---------------------------------------------------------------- LOD
w = {scan.LOD_SITE: 0x2A1303E0}
okk, why = scan.check_lod(scan.LOD_BUILD, w.get, lambda a, c: True)
check(okk, f'LOD: the 1.4.3 build and a plain instruction at the site ({why})')
okk, why = scan.check_lod('1234567890abcdef' + '0' * 24, w.get, lambda a, c: True)
check(not okk and 'not 1.4.3' in why, 'LOD: another build: not applied')
okk, why = scan.check_lod(scan.LOD_BUILD, {scan.LOD_SITE: RET}.get, lambda a, c: True)
check(not okk, 'LOD: a ret at the site: not applied')
okk, why = scan.check_lod(scan.LOD_BUILD, w.get, lambda a, c: False)
check(not okk and 'one function' in why, 'LOD: the skip target in another function: not applied')

# ---------------------------------------------------------------- shadow size
L = Lifter(BASE & ~0xFFFFFFF, 'c')


def shadow_run(words, fns=None):
    cache = {}
    def lift(a):
        if a not in cache:
            cache[a] = L.lift(words[(a - BASE) // 4], a)
        return cache[a]
    fns = fns or [(BASE, BASE + len(words) * 4)]
    def fn_of(a):
        return next((f for f in fns if f[0] <= a < f[1]), None)
    return scan.shadow_candidates(words, BASE, lift, fn_of, lambda lo: set())


def block(extra_reg=12, size=1024):
    """24 instructions: w11 = size computed with a scratch register; the rest arithmetic on x12"""
    blk = [movz(11, size), movz(extra_reg, 3), addw(11, 11, 11)] + [addw(extra_reg, extra_reg, extra_reg)] * 21
    return blk


def after(read12):
    return ([addw(0, 11, 11)] + ([addw(1, 12, 12)] if read12 else [movz(12, 0)]) + [RET])


pre = pad(3) + [RET]
code = pre + block() + after(False) + pad(8)
c = shadow_run(code)
S = BASE + len(pre) * 4
hit = [x for x in c if x['s'] == S]
check(hit and hit[0]['e'] == S + 0x60, f'shadow: the block whose w11 is read after S+0x60 is a candidate ({len(c)} in all)')
check(hit and hit[0]['strict'] and 'constant 1024' in hit[0]['cues'], 'shadow: starts a block, no stores, the 1024 cue')
check(all(x['e'] - x['s'] == 0x60 for x in c), 'shadow: every candidate spans 0x60')
c = shadow_run(pre + block() + after(True) + pad(8))
check(not any(x['s'] == S for x in c), 'shadow: rejected when x12 (written in the block) is read after it')
blk = block(); blk[10] = bl(0x400)
c = shadow_run(pre + blk + after(False) + pad(8))
check(not any(x['s'] <= S + 40 < x['e'] for x in c), 'shadow: a call inside the block rejects it')
blk = block(); blk[5] = b(0x200)
c = shadow_run(pre + blk + after(False) + pad(200))
check(not any(x['s'] == S for x in c), 'shadow: a branch out of the block rejects it')
blk = block(); blk[5] = bcond(0, 0x10)
c = shadow_run(pre + blk + after(False) + pad(8))
check(any(x['s'] == S for x in c), 'shadow: a branch inside the block is fine')
c = shadow_run(pre + block() + [addw(0, 13, 13), RET] + pad(8))
check(not any(x['s'] == S for x in c), 'shadow: w11 not read after the block: no candidate')
code = pre + block() + after(False) + pad(8)
c = shadow_run(code, [(BASE, S + 0x30), (S + 0x30, BASE + len(code) * 4)])
check(not any(x['s'] == S for x in c), 'shadow: a block across two functions is rejected')
c = shadow_run(pre + block() + [addw(9, 11, 11), bl(0x400)] + pad(8))
check(any(x['s'] == S for x in c), 'shadow: x12 is caller-saved, so dead at a call after the block: still a candidate')
c = shadow_run(pre + block(extra_reg=1) + [addw(9, 11, 11), bl(0x400)] + pad(8))
check(not any(x['s'] == S for x in c), 'shadow: x1 written in the block and passed to a call after it: rejected')
c = shadow_run(pre + block(extra_reg=19) + [addw(0, 11, 11), RET] + pad(8))
check(not any(x['s'] == S for x in c), 'shadow: a callee-saved register left changed at ret: rejected')

# ---------------------------------------------------------------- confirmation file
d = tempfile.mkdtemp()
p = os.path.join(d, 'lift-confirm.txt')
with open(p, 'w') as f:
    f.write('# UIT\nshadow 0x8012345670 build 277178b7dba1b6d4 value 1024 hits 5\nshadow 0x1 build ffff value 1 hits 9\n')
check(scan.read_confirm(p, scan.LOD_BUILD) == {0x8012345670}, 'confirm: this build\'s line only')
check(scan.read_confirm(os.path.join(d, 'none.txt'), scan.LOD_BUILD) == set(), 'confirm: no file, nothing confirmed')

# ---------------------------------------------------------------- vreg decode
r, wr, ex = scan.vreg_effects(0x1E220160)   # scvtf s0, w11
check(wr == {32} and not r and ex, 'vreg: scvtf writes s0 only')
r, wr, ex = scan.vreg_effects(0x1E26016B)   # fmov w11, s11
check(r == {43} and not wr, 'vreg: fmov w11, s11 reads s11')
r, wr, ex = scan.vreg_effects(0x3D800FE0)   # str q0, [sp, #48]
check(r == {32} and not wr, 'vreg: str q0 reads q0')
r, wr, ex = scan.vreg_effects(0x1E202000)   # fcmp s0, s0
check(r == {32} and not wr, 'vreg: fcmp writes no register')
# ---------------------------------------------------------------- end to end: scan.run + gen's emission + C compile
import collections, importlib, shutil, subprocess, types
import gen, patches


def fake_main(confirm=None):
    """a main laid out like 1.4.3 where it matters: the trio in one function, a plain instruction at the LOD site with
    its +0x90 target in the same function, one shadow-like block. Returns (module, entries, order, root, gen_dir)."""
    importlib.reload(patches); importlib.reload(scan)
    gen.PATCHES, gen.OPTIONAL, gen.APPLIED, gen.table_flags = patches.PATCHES, patches.OPTIONAL, patches.APPLIED, patches.table_flags
    MAIN = scan.MAIN
    n = (scan.LOD_SITE - MAIN) // 4 + 0x100
    w = [NOP] * n
    f1 = 0x1000 // 4
    fn1 = [0xD10043FF] + trio_a() + pad(0x200 // 4 - 1) + [RET]     # sub sp, sp, #16; the trio; b.gt lands on the ret
    w[f1:f1 + len(fn1)] = fn1
    f2 = 0x4000 // 4
    fn2 = pad(3) + [RET] + block() + after(False)
    w[f2:f2 + len(fn2)] = fn2
    f3 = (scan.LOD_SITE - MAIN) // 4 - 4
    fn3 = pad(4) + [cmpw(1, 3)] + pad(0x90 // 4 - 1) + [movz(0, 1), RET]
    w[f3:f3 + len(fn3)] = fn3
    m = gen.Module.__new__(gen.Module)
    m.name, m.base, m.lo, m.hi, m.words = 'main', MAIN, MAIN, MAIN + n * 4, tuple(w)
    m.lifter, m.cache, m.stats, m.mu, m.spins = Lifter(MAIN, 'c'), {}, collections.Counter(), None, {}   # [UIT 0077] no spin-wait gates here
    m.nso = types.SimpleNamespace(build_id=scan.LOD_BUILD)
    entries = {}
    for start, ln in ((f1, len(fn1)), (f2, len(fn2)), (f3, len(fn3))):
        a = MAIN + start * 4
        labels, ext, tables = m.scan(a, a + ln * 4)
        entries[a] = (a + ln * 4, labels, tables)
    root = tempfile.mkdtemp(); gdir = os.path.join(root, 'data', 'generated'); os.makedirs(gdir)
    if confirm:
        os.makedirs(os.path.join(root, 'diag'))
        with open(os.path.join(root, 'diag', 'lift-confirm.txt'), 'w') as f:
            f.write(confirm)
    return m, entries, sorted(entries), root, gdir, MAIN + f2 * 4 + 16


def emit(m, entries):
    return '\n'.join(l for a in sorted(entries) for l in m.emit_function(a, entries[a][0], entries[a][1], entries[a][2], {}))


def compiles(text, name):
    zcc = os.environ.get('ZCC') or shutil.which('clang') or 'clang'
    if not (os.path.exists(zcc) or shutil.which(zcc)):
        print('skip compile (no clang): ' + name); return True
    d = tempfile.mkdtemp(); c = os.path.join(d, 't.c')
    with open(c, 'w') as f:
        import re
        protos = ''.join(f'void {n}(Cpu *c);\n' for n in sorted(set(re.findall(r'\bf_[0-9a-f]+', text))))
        f.write('#include "cpu.h"\n#include "rt.h"\n' + protos + text + '\n')
    r = subprocess.run([zcc, '-c', '-o', os.path.join(d, 't.o'), '-w', '-I' + str(HERE / 'src/runtime'), c], capture_output=True, text=True)
    if r.returncode: print(r.stderr[:2000])
    return r.returncode == 0


m, entries, order, root, gdir, S2 = fake_main()
feat = scan.run(m, entries, order, root, gdir, patches)
c_text = emit(m, entries)
flags = patches.table_flags(m.lo, m.hi)
ini = open(os.path.join(gdir, 'lift-features.ini')).read()
rep = open(os.path.join(gdir, 'lift-scan.txt')).read()
check(feat['quality'][0] == 1 and feat['fxaa'][0] == 1 and feat['lod'][0] == 1, 'e2e: trio and LOD found')
check(feat['shadow'][0] == 0 and len(scan.STATE['probes']) >= 1 and any(c['s'] == S2 for c in scan.STATE['probes']),
      'e2e: shadow not switched on from the static match; the block gets a probe')
check('totk_quality_reduction_off() ||' in c_text and 'totk_game_fxaa_off()' in c_text and 'totk_lod_improve()' in c_text
      and 'totk_shadow_probe(0' in c_text, 'e2e: the patches are in the emitted C')
check(f'goto L_{scan.LOD_SITE + 0x90:x};' in c_text and f'L_{scan.LOD_SITE + 0x90:x}:;' in c_text, 'e2e: LOD skip is a goto to a label')
check('main_quality_patch = 1' in flags and 'main_lod_patch = 1' in flags and 'main_shadow_patch = 0' in flags and
      'main_shadow_probes = 1' in flags and 'main_lift_build[] = "277178b7dba1b6d4"' in flags, 'e2e: main_table flags')
check('quality=1' in ini and 'lod=1' in ini and 'shadow=0' in ini and 'shadow_note=' in ini, 'e2e: lift-features.ini')
check('[quality + fxaa] one exact match' in rep and '[lod] found' in rep and 'words from' not in rep and
      os.path.exists(os.path.join(gdir, 'lift-scan-words.txt')), 'e2e: lift-scan.txt (no instruction words; those go to lift-scan-words.txt)')
check(compiles(c_text + '\n' + flags, 'probe build'), 'e2e: the emitted C compiles')

m, entries, order, root, gdir, S2 = fake_main(f'shadow {S2:#x} build 277178b7dba1b6d4 value 1024 hits 4\n')
feat = scan.run(m, entries, order, root, gdir, patches)
c_text = emit(m, entries)
flags = patches.table_flags(m.lo, m.hi)
check(feat['shadow'][0] == 1 and 'main_shadow_patch = 1' in flags and 'main_shadow_probes = 0' in flags,
      'e2e: a confirmed site switches the shadow size on (no probes then)')
check(f'c->x[11] = sz_; goto L_{S2 + 0x60:x};' in c_text and f'L_{S2 + 0x60:x}:;' in c_text, 'e2e: shadow patch jumps to S + 0x60')
check(compiles(c_text + '\n' + flags, 'confirmed build'), 'e2e: the emitted C compiles (confirmed)')

m, entries, order, root, gdir, S2 = fake_main(f'shadow {S2 + 4:#x} build 277178b7dba1b6d4 value 1024 hits 4\n')
feat = scan.run(m, entries, order, root, gdir, patches)
check(feat['shadow'][0] == 0 and 'not among' in feat['shadow'][1], 'e2e: a confirmed address that is no candidate is not used')
m, entries, order, root, gdir, S2 = fake_main()
m.nso.build_id = 'ffff' * 10
w = list(m.words); f1 = 0x1000 // 4 + 1; w[f1 + 100:f1 + 108] = trio_a(0x300); m.words = tuple(w)
feat = scan.run(m, entries, order, root, gdir, patches)
emit(m, entries)
flags = patches.table_flags(m.lo, m.hi)
check(feat['quality'][0] == 0 and 'ambiguous' in feat['quality'][1] and feat['lod'][0] == 0 and
      'main_quality_patch = 0' in flags and 'main_lod_patch = 0' in flags, 'e2e: ambiguous trio, other build: all off')
print('all ok' if ok else 'FAILURES'); sys.exit(0 if ok else 1)
