#!/usr/bin/env python3
"""[UIT 0359] tools/cc_try.py and tools/build_game.py --o0-try.

cc_try with stand-in compilers (small Python scripts that log their runs): an optimized compile that works keeps
its object and is remembered as optimized; one that runs past the limit is stopped and the -O0 compile makes the
object; the next build of the same file goes straight to -O0 (the slow compile is not run again); a changed source
tries again; a failed optimized compile falls back too; a failed -O0 compile fails the step and remembers nothing.
build_game.py (its build.ninja, as tools/tests/check_windows_toolchain.py writes it): without --o0-try the -O0
stand-in keeps the gamecc rule and GAME_O0_FLAGS; with it, the gametry rule with -O1 flags, the -O0 flags as the
fallback, the time limit and the o0try pool; build/o0-files.txt lists the file.
With a clang on PATH: one real file through cc_try at -O1.
"""
import os
import pathlib
import shutil
import subprocess
import sys
import tempfile
import time

ROOT = pathlib.Path(__file__).resolve().parents[2]
CC_TRY = ROOT / 'tools/cc_try.py'
FAKE = '''import pathlib, sys, time
args = sys.argv[1:]
log, mode = args[0], args[1]
out = args[args.index('-o') + 1]
with open(log, 'a') as f:
    f.write(mode + chr(10))
if mode == 'slow':
    time.sleep(30)
if mode == 'fail':
    sys.exit(7)
pathlib.Path(out).write_text(mode)
'''
fails = []


def check(cond, what):
    print(('ok   ' if cond else 'FAIL ') + what, flush=True)
    if not cond:
        fails.append(what)


def cc_try(tmp, seconds, opt_mode, o0_mode, src='a.c'):
    log = tmp / 'log'
    fake = [sys.executable, str(tmp / 'fake.py'), str(log)]
    cmd = [sys.executable, str(CC_TRY), str(seconds), str(tmp / src), str(tmp / 'a.o'), '--',
           *fake, opt_mode, '-c', 'x', '-o', str(tmp / 'a.o'), '--', *fake, o0_mode, '-c', 'x', '-o', str(tmp / 'a.o')]
    before = log.read_text().split() if log.is_file() else []
    t0 = time.monotonic()
    r = subprocess.run(cmd, capture_output=True, text=True)
    runs = (log.read_text().split() if log.is_file() else [])[len(before):]
    obj = (tmp / 'a.o').read_text() if (tmp / 'a.o').is_file() else None
    return r.returncode, runs, obj, time.monotonic() - t0, r.stdout


def unit():
    with tempfile.TemporaryDirectory() as d:
        tmp = pathlib.Path(d)
        (tmp / 'fake.py').write_text(FAKE)
        (tmp / 'a.c').write_text('int x;\n')
        rc, runs, obj, _, _ = cc_try(tmp, 20, 'opt', 'o0')
        check(rc == 0 and runs == ['opt'] and obj == 'opt', f'optimized compile in time: kept ({rc}, {runs}, {obj})')
        rc, runs, obj, took, out = cc_try(tmp, 1, 'slow', 'o0', 'a.c')
        # the key has the commands in it: 'slow' is a different command, so it is tried
        check(rc == 0 and runs == ['slow', 'o0'] and obj == 'o0' and took < 15,
              f'past the limit: stopped after {took:.1f} s, -O0 made the object ({rc}, {runs}, {obj})')
        check('built unoptimized (-O0) instead' in out, 'the fallback says so')
        rc, runs, obj, _, _ = cc_try(tmp, 1, 'slow', 'o0')
        check(rc == 0 and runs == ['o0'] and obj == 'o0', f'remembered: straight to -O0 the next time ({runs})')
        (tmp / 'a.c').write_text('int y;\n')
        rc, runs, _, _, _ = cc_try(tmp, 1, 'slow', 'o0')
        check(runs == ['slow', 'o0'], f'a changed source is tried again ({runs})')
        rc, runs, obj, _, _ = cc_try(tmp, 20, 'fail', 'o0')
        check(rc == 0 and runs == ['fail', 'o0'] and obj == 'o0', f'a failed optimized compile falls back ({rc}, {runs})')
        rc, runs, _, _, _ = cc_try(tmp, 20, 'fail', 'fail')
        check(rc == 7 and not (tmp / 'a.o.try').exists(), f'a failed -O0 compile fails the step, nothing kept ({rc})')


def ninja_text(o0_try, split=False):
    sys.path.insert(0, str(ROOT / 'tools/tests'))
    import check_windows_toolchain as c
    with tempfile.TemporaryDirectory() as d:
        tree = pathlib.Path(d)
        files = subprocess.run(['git', '-C', str(ROOT), 'ls-files', '-co', '--exclude-standard'], capture_output=True,
                               text=True, check=True).stdout.split('\n')
        for f in files:
            if f and (f.startswith(('tools/', 'src/', 'assets/', 'third_party/inter/')) or '/' not in f):
                p = ROOT / f
                if p.is_file():
                    (tree / f).parent.mkdir(parents=True, exist_ok=True)
                    shutil.copy2(p, tree / f)
        gen = tree / 'data/generated'
        for m in ('main', 'subsdk0', 'sdk'):
            (gen / m).mkdir(parents=True)
            (gen / m / f'{m}_stub.c').write_text('int x_%s;\n' % m)
        (gen / 'sdk/sdk_bigframe.c').write_text('L_dispatch:\n' + ''.join(f'\nL_{i}: ;' for i in range(2100)))
        if split:   # [UIT 0370] a fast part of a split function (as many labels as an -O0 file)
            sys.path.insert(0, str(ROOT / 'tools'))
            import build_game
            (gen / 'main/main_split.c').write_text(build_game.SPLIT_MARK + '\nL_dispatch:\n'
                                                   + ''.join(f'\nL_{i}: ;' for i in range(2100)))
            (gen / 'main/main_split_any.c').write_text('void rt_split_cold(Cpu *c, uint64_t fn);\nL_dispatch:\n'
                                                       + ''.join(f'\nL_{i}: ;' for i in range(2100)))
        sysroot = tree / 'fake64'
        (sysroot / 'bin').mkdir(parents=True)
        (sysroot / 'bin/clang.exe').write_text('')
        shim = ('import sys; sys.path.insert(0, "tools"); import build_game as b; '
                'b.compile_then_link = lambda *a, **k: 0; b.frame_report = lambda *a: 0; b.main()')
        args = ['--generated', 'data/generated', '--target', 'ultratears', '--cpus', '4', '--no-dlss']
        args += ['--o0-try', str(o0_try)] if o0_try else []
        args += ['--lto', 'thin'] if split else []
        r = subprocess.run([sys.executable, '-c', shim, *args], cwd=tree, capture_output=True, text=True,
                           env=dict(os.environ, ULTRS_CLANG64=str(sysroot / 'bin')))
        if r.returncode:
            check(False, f'build_game.py ran:\n{r.stdout}\n{r.stderr}')
            return '', ''
        o0 = (tree / 'build/o0-files.txt').read_text() if (tree / 'build/o0-files.txt').is_file() else None
        return (tree / 'build/build.ninja').read_text(), o0


def ninja():
    import re
    sys.path.insert(0, str(ROOT / 'tools'))
    text, o0 = ninja_text(0)
    m = re.search(r'build build/game/sdk/sdk_bigframe\.o: (\S+) \S+\n((?:  .*\n)*)', text)
    check(m and m.group(1) == 'gamecc' and '-O0' in m.group(2) and '-Dalways_inline=noinline' in m.group(2)
          and 'tryflags' not in m.group(2), 'off: the -O0 file keeps gamecc and the -O0 flags')
    check(o0 == 'sdk_bigframe O0\n', f'off: o0-files.txt lists it as O0 ({o0!r})')
    m2 = re.search(r'build build/game/sdk/sdk_stub\.o: (\S+)', text)
    check(m2 and m2.group(1) == 'gamecc', 'other game files: gamecc')
    text, o0 = ninja_text(240)
    m = re.search(r'build build/game/sdk/sdk_bigframe\.o: (\S+) \S+\n((?:  .*\n)*)', text)
    body = m.group(2) if m else ''
    try_flags = re.search(r'  tryflags = (.*)', body)
    game_flags = re.search(r'  gameflags = (.*)', body)
    check(m and m.group(1) == 'gametry', 'on: the -O0 file goes through gametry')
    check(bool(try_flags) and '-O1' in try_flags.group(1).split() and '-O0' not in try_flags.group(1)
          and '-Dalways_inline=noinline' in try_flags.group(1), 'on: -O1 flags (helpers still outlined)')
    check(bool(game_flags) and '-O0' in game_flags.group(1).split(), 'on: the -O0 flags stay as the fallback')
    check('  trysec = 240' in body, 'on: the time limit')
    rule = re.search(r'rule gametry\n((?:  .*\n)*)', text)
    check(rule and 'cc_try.py $trysec $in $out -- clang' in rule.group(1) and 'pool = o0try' in rule.group(1)
          and re.search(r'pool o0try\n  depth = 2', text), 'on: the rule, its pool of 2')
    check(o0 == 'sdk_bigframe O0\n', f'on, not built: listed as O0 ({o0!r})')


def split():
    """[UIT 0370] the fast part of a split function: gamesplit (cc_try --stamp .split, the game's flags without ThinLTO,
    -O0 as the fallback, its own pool); no such file: no such rule."""
    import re
    text, o0 = ninja_text(0)
    check('gamesplit' not in text and 'pool split' not in text, 'no split file: build.ninja has no split rule')
    text, o0 = ninja_text(0, True)
    m = re.search(r'build build/game/main/main_split\.o: (\S+) \S+\n((?:  .*\n)*)', text)
    check(m and m.group(1) == 'gamesplit' and not m.group(2), f'split: the fast part goes through gamesplit, as is')
    rule = re.search(r'rule gamesplit\n((?:  .*\n)*)', text)
    cmd = rule.group(1) if rule else ''
    opt, _, o0c = cmd.partition(' -- clang')[2].partition(' -- clang')
    check('cc_try.py --stamp .split 900 $in $out' in cmd and 'pool = split' in cmd
          and re.search(r'pool split\n  depth = 4', text), 'split: the rule (15 minute limit, .split stamp) and its pool')
    check('-O2' in opt.split() and '-flto=thin' not in opt and '-flto=thin' in text,
          'split: optimized with the game flags but without ThinLTO (which the other files have)')
    check('-O0' in o0c.split() and '-Dalways_inline=noinline' in o0c, 'split: the -O0 flags as the fallback')
    check(sorted((o0 or '').splitlines()) == ['main_split O0', 'main_split_any O0', 'sdk_bigframe O0'],
          f'split: o0-files.txt lists the fast part and the complete part (not built: O0) ({o0!r})')
    text, o0 = ninja_text(240, True)   # [UIT 0371] with the -O1 try on
    m = re.search(r'build build/game/main/main_split_any\.o: (\S+) \S+\n((?:  .*\n)*)', text)
    check(m and m.group(1) == 'gamecc' and '-O0' in m.group(2) and 'tryflags' not in m.group(2),
          'split, -O1 try on: the complete part stays -O0 (no try)')
    m = re.search(r'build build/game/sdk/sdk_bigframe\.o: (\S+)', text)
    check(m and m.group(1) == 'gametry', 'split, -O1 try on: other -O0 files are still tried')
    with tempfile.TemporaryDirectory() as d:
        tmp = pathlib.Path(d)
        (tmp / 'fake.py').write_text(FAKE)
        (tmp / 'a.c').write_text('int x;\n')
        fake = [sys.executable, str(tmp / 'fake.py'), str(tmp / 'log')]
        r = subprocess.run([sys.executable, str(CC_TRY), '--stamp', '.split', '20', str(tmp / 'a.c'), str(tmp / 'a.o'), '--',
                            *fake, 'opt', '-o', str(tmp / 'a.o'), '--', *fake, 'o0', '-o', str(tmp / 'a.o')])
        check(r.returncode == 0 and (tmp / 'a.o.split').is_file() and not (tmp / 'a.o.try').exists()
              and '"opt"' in (tmp / 'a.o.split').read_text(), 'cc_try --stamp .split: the outcome in OBJECT.split')


def real():
    cc = shutil.which('clang')
    if not cc:
        print('skip real clang (none on PATH)')
        return
    with tempfile.TemporaryDirectory() as d:
        tmp = pathlib.Path(d)
        (tmp / 'r.c').write_text('int f(int x) { int a = x * 3; return a + 1; }\n')
        o = str(tmp / 'r.o')
        r = subprocess.run([sys.executable, str(CC_TRY), '60', str(tmp / 'r.c'), o, '--', cc, '-O1', '-c', str(tmp / 'r.c'),
                            '-o', o, '--', cc, '-O0', '-c', str(tmp / 'r.c'), '-o', o], capture_output=True, text=True)
        stamp = (tmp / 'r.o.try').read_text() if (tmp / 'r.o.try').is_file() else ''
        check(r.returncode == 0 and os.path.getsize(o) > 0 and '"opt"' in stamp, f'real clang -O1 through cc_try ({stamp})')


unit()
ninja()
split()
real()
print('test_o0_try: ' + ('PASS' if not fails else f'{len(fails)} FAILED'))
sys.exit(1 if fails else 0)
