"""[UIT 0175] Box test for the cheat patch sites (tools/lifter/patches.py CHEATS) on a synthetic main: the shipped
1.4.3 words at the four sites -> the emitted C switches on uit_cheat_durability / uit_cheat_amiibo, the amiibo branch
still jumps to its label, main_cheat_*_patch = 1, and the C compiles; another word at a site -> a warning, the
original instruction only and the flag 0 (no stopped lift).     python3 tools/tests/test_cheats_lift.py"""
import collections, importlib, os, pathlib, re, shutil, subprocess, sys, tempfile, types
HERE = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(HERE / 'tools' / 'lifter'))
import gen, patches, scan
from lifter import Lifter

ok = True
def check(c, w):
    global ok; print(('ok   ' if c else 'FAIL ') + w); ok &= bool(c)

MAIN = patches.MAIN
NOP, RET = 0xD503201F, 0xD65F03C0


class Sparse:
    """main's words: NOP everywhere except where set (the real main is 0x2200000+ bytes of code)"""
    def __init__(self, n, w): self.n, self.w = n, w
    def __len__(self): return self.n
    def __getitem__(self, i):
        if isinstance(i, slice): return tuple(self[j] for j in range(*i.indices(self.n)))
        return self.w.get(i, NOP)


# (function start, function end) around each site; the amiibo branch's target main+0x1f87e44 is inside its function
FUNCS = [(0x21eca80, 0x21ecac0), (0x2829e0, 0x282e80), (0x1f87e00, 0x1f87e60)]


def fake_main(override=None):
    importlib.reload(patches); importlib.reload(scan)
    gen.PATCHES, gen.OPTIONAL, gen.APPLIED, gen.table_flags = patches.PATCHES, patches.OPTIONAL, patches.APPLIED, patches.table_flags
    w = {}
    for off, word in patches._DUR_WORDS + patches._AMIIBO_WORDS:
        w[off // 4] = word
    for s, e in FUNCS:
        w[(e - 4) // 4] = RET
    w.update({k // 4: v for k, v in (override or {}).items()})
    n = 0x2200000 // 4
    m = gen.Module.__new__(gen.Module)
    m.name, m.base, m.lo, m.hi, m.words = 'main', MAIN, MAIN, MAIN + n * 4, Sparse(n, w)
    m.lifter, m.cache, m.stats, m.mu, m.spins = Lifter(MAIN, 'c'), {}, collections.Counter(), None, {}
    m.nso = types.SimpleNamespace(build_id='277178b7dba1b6d4')
    entries = {}
    for s, e in FUNCS:
        a = MAIN + s
        labels, ext, tables = m.scan(a, MAIN + e)
        entries[a] = (MAIN + e, labels, tables)
    root = tempfile.mkdtemp()
    scan.STATE['gen_dir'] = root
    return m, entries


def emit(m, entries):
    return '\n'.join(l for a in sorted(entries) for l in m.emit_function(a, entries[a][0], entries[a][1], entries[a][2], {}))


def compiles(text, name):
    zcc = os.environ.get('ZCC') or shutil.which('clang') or shutil.which('cc')
    if not zcc:
        print('skip compile (no C compiler): ' + name); return True
    d = tempfile.mkdtemp(); c = os.path.join(d, 't.c')
    with open(c, 'w') as f:
        protos = ''.join(f'void {n}(Cpu *c);\n' for n in sorted(set(re.findall(r'\bf_[0-9a-f]+', text))))
        f.write('#include "cpu.h"\n#include "rt.h"\n' + protos + text + '\n')
    r = subprocess.run([zcc, '-c', '-o', os.path.join(d, 't.o'), '-w', '-I' + str(HERE / 'src/runtime'), c], capture_output=True, text=True)
    if r.returncode: print(r.stderr[:2000])
    return r.returncode == 0


check(all(MAIN + o in patches.OPTIONAL for o, _ in patches._DUR_WORDS + patches._AMIIBO_WORDS), 'cheat sites are optional')

m, entries = fake_main()
c_text = emit(m, entries)
if os.environ.get("CHEATS_DUMP"): open(os.environ["CHEATS_DUMP"], "a").write(c_text + "\n----\n")
flags = patches.table_flags(m.lo, m.hi)
check(c_text.count('uit_cheat_durability()') == 3, 'durability: three switched sites')
check('if (uit_cheat_amiibo_site((' in c_text and 'goto L_801f87e44;' in c_text and 'L_801f87e44:;' in c_text,
      'amiibo: the branch is decided by uit_cheat_amiibo_site (taken only with the cheat off), to its label')
site = c_text[c_text.index('uit_cheat_amiibo_site(('):]
site = site[:site.index('goto L_801f87e44;')]
check('& 1' in site and ') != 0)' in site, 'amiibo: the game\'s bit 0 of w0 is what the site is given [UIT 0180]: ' + site.strip()[:80])
check('main_cheat_durability_patch = 1' in flags and 'main_cheat_amiibo_patch = 1' in flags, 'flags on with the 1.4.3 words')
check(all(MAIN + o in patches.APPLIED for o, _ in patches._DUR_WORDS + patches._AMIIBO_WORDS), 'all sites emitted')
check(compiles(c_text + '\n' + flags, 'cheats on'), 'the emitted C compiles')
ini = open(os.path.join(scan.STATE['gen_dir'], 'lift-features.ini')).read()
check('cheat_durability=1' in ini and 'cheat_amiibo=1' in ini, 'lift-features.ini: both cheats (the launcher reads it)')

m, entries = fake_main({0x282e58: 0xB9400D01})   # ldr w1, [x8, #12]: another build
c_text = emit(m, entries)
if os.environ.get("CHEATS_DUMP"): open(os.environ["CHEATS_DUMP"], "a").write(c_text + "\n----\n")
flags = patches.table_flags(m.lo, m.hi)
check('uit_cheat_durability' not in c_text and 'main_cheat_durability_patch = 0' in flags,
      'durability: one word differs -> all three sites keep the original code, flag 0')
ini = open(os.path.join(scan.STATE['gen_dir'], 'lift-features.ini')).read()
check('cheat_durability=0' in ini and 'cheat_durability_note=the TotK 1.4.3 instructions' in ini and 'cheat_amiibo=1' in ini,
      'lift-features.ini: durability unavailable, with why')
check('uit_cheat_amiibo_site(' in c_text and 'main_cheat_amiibo_patch = 1' in flags, 'amiibo unaffected')
check(compiles(c_text + '\n' + flags, 'durability off'), 'the emitted C compiles (durability off)')

m, entries = fake_main({0x1f87e08: 0x360001E0})   # tbz instead of tbnz
c_text = emit(m, entries)
if os.environ.get("CHEATS_DUMP"): open(os.environ["CHEATS_DUMP"], "a").write(c_text + "\n----\n")
flags = patches.table_flags(m.lo, m.hi)
check('uit_cheat_amiibo' not in c_text and 'main_cheat_amiibo_patch = 0' in flags and 'goto L_801f87e44;' in c_text,
      'amiibo: another word -> the original branch only, flag 0')
check(compiles(c_text + '\n' + flags, 'amiibo off'), 'the emitted C compiles (amiibo off)')

# [UIT 0180, 0210] src/host/cheats.c's uit_cheat_amiibo_site, run. Cheat on, learn (default): the first answer (a
# figure with a new serial number) is kept for every later run, whichever way it goes; nop: never taken; game: the
# game's answer; cheat off: the game's answer. Every run logged (the first 64, then every 100th).
SITE_TEST = r"""
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
const int main_cheat_durability_patch = 1, main_cheat_amiibo_patch = 1;
int uit_cheat_amiibo_site(int used); int uit_cheat_set_amiibo(int on);
static int lines; static char last[600];
void runtime_amiibo_log(const char *fmt, ...) { va_list ap; va_start(ap, fmt); vsnprintf(last, sizeof last, fmt, ap); va_end(ap); lines++; }
int main(int argc, char **argv) {
    int bad = argc < 2, first = argc < 2 ? 0 : atoi(argv[1]);
    const char *mode = getenv("TOTK_CHEAT_AMIIBO_BRANCH");
    if (!mode) {                                          /* learn */
        bad |= uit_cheat_amiibo_site(first) != first;     /* the new serial's answer stands */
        bad |= !strstr(last, "first check with a new serial number") || lines != 1 || !strstr(last, "1 time so far)");
        for (int i = 0; i < 99; i++) bad |= uit_cheat_amiibo_site(!first) != first;   /* runs 2..100 */
        bad |= !strstr(last, "changed to what a figure never seen gets") || !strstr(last, "100 times") || lines != 65;
        bad |= uit_cheat_amiibo_site(first) != first;     /* run 101: not logged */
        bad |= lines != 65;
    } else if (!strcmp(mode, "nop")) {
        bad |= uit_cheat_amiibo_site(1) != 0 || uit_cheat_amiibo_site(0) != 0 || !strstr(last, "public cheat");
    } else {
        bad |= uit_cheat_amiibo_site(1) != 1 || uit_cheat_amiibo_site(0) != 0 || !strstr(last, "own answer");
    }
    uit_cheat_set_amiibo(0);
    bad |= uit_cheat_amiibo_site(1) != 1 || uit_cheat_amiibo_site(0) != 0;
    bad |= !mode && lines != 65;
    bad |= mode && (!strstr(last, "is off") || !strstr(last, "would not be taken"));
    printf("%s\n", bad ? "BAD" : "GOOD"); return bad;
}
"""
zcc = os.environ.get('ZCC') or shutil.which('clang') or shutil.which('cc')
if zcc:
    d = tempfile.mkdtemp(); src = os.path.join(d, 's.c'); exe = os.path.join(d, 's')
    open(src, 'w').write(SITE_TEST)
    r = subprocess.run([zcc, '-O1', '-Wall', '-I' + str(HERE / 'src/host'), src, str(HERE / 'src/host/cheats.c'), '-o', exe],
                       capture_output=True, text=True)
    if r.returncode: print(r.stderr[:2000])
    runs = [] if r.returncode else [
        ({'TOTK_CHEAT_AMIIBO': '1'}, '0'), ({'TOTK_CHEAT_AMIIBO': '1'}, '1'),
        ({'TOTK_CHEAT_AMIIBO': '1', 'TOTK_CHEAT_AMIIBO_BRANCH': 'nop'}, '1'),
        ({'TOTK_CHEAT_AMIIBO': '1', 'TOTK_CHEAT_AMIIBO_BRANCH': 'game'}, '1')]
    res = []
    for extra, arg in runs:
        env = {k: v for k, v in os.environ.items() if k not in ('TOTK_CHEAT_AMIIBO', 'TOTK_CHEAT_AMIIBO_BRANCH')}
        env.update(extra)
        r2 = subprocess.run([exe, arg], capture_output=True, text=True, env=env)
        print(extra, arg, r2.stdout.strip())
        res.append(r2.returncode == 0 and 'GOOD' in r2.stdout)
    check(len(res) == 4 and all(res),
          'uit_cheat_amiibo_site: learn keeps the new-serial answer (either way), nop / game modes, off = the game\'s')
else:
    print('skip uit_cheat_amiibo_site run (no C compiler)')
print('all ok' if ok else 'FAILURES'); sys.exit(0 if ok else 1)
