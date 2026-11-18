#!/usr/bin/env python3
"""[UIT 0432] A build that was stopped half way (the PC lost power) builds again by itself.

A small stand-in of the game's build (generated files with f_<address> functions, a <module>_table.c that lists them,
a runtime file, the same objs-then-link order as tools/build_game.py) is built with real clang and ninja; then:
  1. ninja is killed in the middle of a compile (SIGKILL, as a power cut stops it);
  2. what a power cut leaves behind is made by hand: objects ninja has as finished but that read as zeros (size kept,
     the data lost: the user's case, lld takes them as empty objects and the link misses thousands of symbols), one
     cut short, the table object zeroed, a clang temporary file left over;
  3. build_game.compile_then_link with tools/obj_check.py must find them, build them again and link, and the program
     must give the right answer;
  4. an object that looks whole but lacks its functions (and is vouched for by its record) makes the link fail with
     undefined symbols: it is deleted, built again, and the link is tried once more by itself;
  5. a zeroed build/.ninja_log (ninja's own record cut off) still ends in a correct build;
  6. generated C left as zeros is reported (build_game exit GEN_DAMAGED, setup.py translates again);
  7. tools/obj_check.py on COFF/ELF/bitcode objects: whole ones pass, empty / zeros / cut short fail.
Needs clang (with ld.lld) and ninja on PATH (or CLANG / NINJA); skipped without them.
"""
import os
import pathlib
import shutil
import signal
import subprocess
import sys
import tempfile
import time

ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools'))
CLANG = os.environ.get('CLANG') or shutil.which('clang')
NINJA = os.environ.get('NINJA') or shutil.which('ninja')
if not CLANG or not NINJA:
    print('skipped: clang and ninja are needed (CLANG=, NINJA=)')
    sys.exit(0)
import build_game  # noqa: E402
import obj_check  # noqa: E402

ok = True


def check(c, what):
    global ok
    print(('ok   ' if c else 'FAIL ') + what, flush=True)
    ok &= bool(c)


N = 6          # generated files
FN = 40        # functions in each
BASE = 0x800000000
REPS = 400     # statements in each function (enough work that a kill lands in the middle of a compile)


CPU = '#include <stdint.h>\ntypedef struct { uint64_t x[2]; } Cpu;\n'


def put(p, text):   # as the lifter writes (gen.py write_if_changed): an unchanged file keeps its time
    if not p.is_file() or p.read_text() != text:
        p.write_text(text)


def write_tree(t, salt=0):
    """Files shaped like the lifter's (tools/lifter/gen.py file_text): prototypes first, then the functions."""
    gen = t / 'data/generated/main'
    gen.mkdir(parents=True, exist_ok=True)
    addrs = []
    for i in range(N):
        body = [CPU, f'void f_{BASE + 0x10:x}(Cpu *c);\n']
        for k in range(FN):
            a = BASE + (i * FN + k) * 0x40
            addrs.append(a)
            body.append(f'void f_{a:x}(Cpu *__restrict c) {{\n  uint64_t x = c->x[0];\n'
                        + ''.join(f'  x = x * {k + j + 3 + salt}u + {j};\n' for j in range(REPS))
                        + '  c->x[1] = x;\n}\n')
        put(gen / f'main_{i:03d}.c', ''.join(body))
    tab = [CPU] + [f'void f_{a:x}(Cpu *c);\n' for a in addrs]
    tab.append('void (*const main_fn_ptrs[])(Cpu *) = {' + ','.join(f'f_{a:x}' for a in addrs) + '};\n')
    tab.append(f'const uint64_t main_fn_count = {len(addrs)};\n')
    put(gen / 'main_table.c', ''.join(tab))
    (t / 'src').mkdir(exist_ok=True)
    put(t / 'src/run.c',
        CPU + '#include <stdio.h>\nextern void (*const main_fn_ptrs[])(Cpu *);\n'
        'extern const uint64_t main_fn_count;\nint main(void) { uint64_t h = 0;\n'
        '  for (uint64_t i = 0; i < main_fn_count; i++) { Cpu c = {{i, 0}}; main_fn_ptrs[i](&c); h = h * 31 + c.x[1]; }\n'
        '  printf("%llu %llu\\n", (unsigned long long)main_fn_count, (unsigned long long)h); return 0; }\n')


def expected(salt=0):
    """The program's answer, computed here."""
    m = (1 << 64) - 1
    h, cnt = 0, N * FN
    for idx in range(cnt):
        k = idx % FN
        x = idx
        for j in range(REPS):
            x = (x * (k + j + 3 + salt) + j) & m
        h = (h * 31 + x) & m
    return f'{cnt} {h}'


def ninja_file(t):
    objs = [f'build/game/main/main_{i:03d}.o' for i in range(N)] + ['build/game/main/main_table.o']
    lines = ['builddir = build', 'rule gamecc', f'  command = {CLANG} -MD -MF $out.d -O1 -g -c $in -o $out',
             '  depfile = $out.d', '  deps = gcc',
             'rule link', f'  command = {CLANG} -fuse-ld=lld -o $out $in', '']
    for o in objs:
        src = 'data/generated/main/' + pathlib.Path(o).stem + '.c'
        lines.append(f'build {o}: gamecc {src}')
    lines.append('build build/obj/run.o: gamecc src/run.c')
    objs.append('build/obj/run.o')
    lines += [f'build build/game.exe: link {" ".join(objs)}', f'build objs: phony {" ".join(objs)}',
              'default build/game.exe']
    (t / 'build').mkdir(exist_ok=True)
    (t / 'build/build.ninja').write_text('\n'.join(lines) + '\n')
    return objs


def run_ninja_in(t, j=2):
    def run(target):
        p = subprocess.run([NINJA, '-C', str(t), '-f', 'build/build.ninja', f'-j{j}', '-k', '0']
                           + ([target] if target else []), capture_output=True, text=True)
        return p.returncode, p.stdout + p.stderr
    return run


def build(t, objs):
    build_game.ROOT = t
    b = t / 'build'
    build_game.remove_leftovers(b)
    return build_game.compile_then_link(
        run_ninja_in(t), False, lambda: None,
        check_objs=lambda keep=False: obj_check.verify(t, b, objs, 4, keep),
        remake=lambda out: build_game.remake_for_link(out, t / 'data/generated', objs, b),
        reset_logs=lambda: build_game.reset_ninja_logs(b))


def answer(t):
    p = subprocess.run([str(t / 'build/game.exe')], capture_output=True, text=True)
    return p.stdout.strip()


def keep_time(p, data):
    """New content with the old time: what ninja recorded still matches, so ninja has the file as up to date."""
    st = p.stat()
    p.write_bytes(data)
    os.utime(p, ns=(st.st_atime_ns, st.st_mtime_ns))


def zero(p):
    keep_time(p, bytes(p.stat().st_size))


t = pathlib.Path(tempfile.mkdtemp(prefix='uit-resume-'))
try:
    write_tree(t)
    objs = ninja_file(t)
    rc = build(t, objs)
    check(rc == 0 and answer(t) == expected(), 'clean build: links and gives the right answer')
    rec = obj_check.load_records(t / 'build')
    check(all(o in rec for o in objs), 'clean build: every object checked and recorded')

    # 1. killed in the middle of a compile
    write_tree(t, salt=1)
    p = subprocess.Popen([NINJA, '-C', str(t), '-f', 'build/build.ninja', '-j1', 'objs'],
                         stdout=subprocess.PIPE, stderr=subprocess.STDOUT, start_new_session=True)
    log = t / 'build/.ninja_log'
    size0 = log.stat().st_size
    t0 = time.time()
    t0ns = time.time_ns()
    while time.time() - t0 < 60 and log.stat().st_size < size0 + 1 and p.poll() is None:
        time.sleep(0.02)
    time.sleep(0.05)
    os.killpg(p.pid, signal.SIGKILL)
    p.wait()
    done = [o for o in objs if (t / o).stat().st_mtime_ns > t0ns]
    check(0 < len(done) < len(objs), f'killed mid-compile: {len(done)} of {len(objs)} objects new')
    # 2. what the power cut leaves: the finished ones read as zeros, one cut short, the table zeroed, a temporary file
    for o in done:
        zero(t / o)
    tab = t / 'build/game/main/main_table.o'
    zero(tab)
    cut = t / 'build/obj/run.o'
    keep_time(cut, cut.read_bytes()[:cut.stat().st_size // 2])
    (t / 'build/game/main/main_002-12345.o.tmp').write_bytes(b'half')
    plan = subprocess.run([NINJA, '-C', str(t), '-f', 'build/build.ninja', '-n', 'objs'], capture_output=True,
                          text=True).stdout
    trap = [o for o in done + ['build/game/main/main_table.o', 'build/obj/run.o'] if o in plan]
    check(not trap, f'ninja alone has the {len(done) + 2} damaged objects as up to date (the trap of the failed update)')
    rc = build(t, objs)
    check(rc == 0 and answer(t) == expected(1), 'after the kill and the damage: built again by itself, right answer')
    check(not list((t / 'build').rglob('*.tmp')), 'leftover temporary files removed')
    check(all(obj_check.check(t / o) is None for o in objs), 'every object whole again')

    # 4. whole-looking object without its functions, vouched for by its record: the link fails, then repairs itself
    victim = t / 'build/game/main/main_003.o'
    (t / 'empty.c').write_text('int uit_nothing_here;\n')
    subprocess.run([CLANG, '-c', str(t / 'empty.c'), '-o', str(t / 'empty.o')], check=True)
    keep_time(victim, (t / 'empty.o').read_bytes())
    rec = obj_check.load_records(t / 'build')
    st = victim.stat()
    rec['build/game/main/main_003.o'] = [st.st_size, st.st_mtime_ns]
    obj_check.save_records(t / 'build', rec)
    (t / 'build/game.exe').unlink()
    outs = []
    real = run_ninja_in(t)

    def spy(target):
        r = real(target)
        outs.append((target, r[0], r[1]))
        return r
    build_game.ROOT = t
    b = t / 'build'
    rc = build_game.compile_then_link(spy, False, lambda: None,
                                      check_objs=lambda keep=False: obj_check.verify(t, b, objs, 4, keep),
                                      remake=lambda out: build_game.remake_for_link(out, t / 'data/generated', objs, b))
    links = [r for r in outs if r[0] is None]
    if os.environ.get('UIT_TEST_VERBOSE'):
        for r in outs:
            print(r[0], r[1], r[2][-600:])
    check(len(links) == 2 and links[0][1] != 0 and 'undefined symbol' in links[0][2], 'object without its functions: '
          'the first link fails with undefined symbols')
    check(rc == 0 and answer(t) == expected(1), 'the object that defines them is built again; the second link works')
    want, unplaced = obj_check.undefined_objects(links[0][2], t / 'data/generated')
    check(want == ['build/game/main/main_003.o'] and not unplaced, f'missing symbols traced to their file: {want}')
    w2, _ = obj_check.undefined_objects('ld.lld: error: undefined symbol: main_fn_count\n>>> referenced by '
                                        'build/obj/run.o:(main)\n', t / 'data/generated')
    check(w2 == ['build/game/main/main_table.o'], 'a missing <module>_... symbol points at the table object')

    # a check that is wrong about a file never loops: the second time the compiler's own output is kept
    calls = []
    rc = build_game.compile_then_link(lambda target: (calls.append(target), (0, ''))[1], False, lambda: None,
                                      check_objs=lambda keep=False: [('build/x.o', 'odd')])
    check(rc == 0 and calls == ['objs', 'objs', None], 'a file the check always rejects: compiled once more, then kept')

    # 5. ninja's own record zeroed
    zero(t / 'build/.ninja_log')
    zero(t / 'build/.ninja_deps')
    write_tree(t, salt=2)
    rc = build(t, objs)
    check(rc == 0 and answer(t) == expected(2), 'zeroed .ninja_log / .ninja_deps: still a correct build')

    # 6. generated C left as zeros
    zero(t / 'data/generated/main/main_001.c')
    bad = build_game.damaged_sources(t / 'build', sorted((t / 'data/generated/main').glob('*.c')))
    check([p.name for p in bad] == ['main_001.c'], 'zeroed generated file found (build_game exits GEN_DAMAGED)')
    write_tree(t, salt=2)
    check(build_game.damaged_sources(t / 'build', sorted((t / 'data/generated/main').glob('*.c'))) == [],
          'translated again: no damaged file left')

    # 7. obj_check on objects of every kind it knows
    kinds = [('elf', []), ('bitcode', ['-flto=thin']), ('coff', ['--target=x86_64-w64-windows-gnu'])]
    for name, extra in kinds:
        o = t / f'k_{name}.o'
        r = subprocess.run([CLANG, '-O1', '-c', str(t / 'data/generated/main/main_000.c'), '-o', str(o)] + extra,
                           capture_output=True)
        if r.returncode:
            print(f'skip {name}: no such target here')
            continue
        whole = o.read_bytes()
        check(obj_check.check(o) is None, f'{name}: a whole object passes')
        o.write_bytes(bytes(len(whole)))
        check(obj_check.check(o) is not None, f'{name}: zeros fail')
        o.write_bytes(whole[:len(whole) // 3 * 2 // 4 * 4 - 4] if name != 'bitcode' else b'')
        check(obj_check.check(o) is not None, f'{name}: cut short (or empty) fails')
finally:
    shutil.rmtree(t, ignore_errors=True)
print('all ok' if ok else 'FAILED')
sys.exit(0 if ok else 1)
