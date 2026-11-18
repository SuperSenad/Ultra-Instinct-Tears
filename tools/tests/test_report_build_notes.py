"""[UIT 0446] What a problem report says about the build behind the newest game log, and the -O0 fallback.
  - tools/report.py takes the newest rebuild logs (About > Rebuild the game) and build/o0-files.txt, and its README says
    whether the newest game log is the first launch after the last build (cold shader cache), a later one, or from before
    it; that diag/profile.txt is STALE when it is older than the newest game log; which split functions' fast parts are
    built -O0, with the reason;
  - tools/build_game.py builds a file again (optimized first) that tools/cc_try.py built -O0 only because the PC was busy
    (its object is removed, so ninja runs the step), and the build log names each -O0 fast part with its reason.
usage: python tools/tests/test_report_build_notes.py
"""
import io
import json
import os
import pathlib
import shutil
import subprocess
import sys
import tempfile
import time
import zipfile
from contextlib import redirect_stdout

HERE = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(HERE / 'tools'))
import build_game  # noqa: E402

fails = []
neg = os.environ.get('TOTK_PROBE_NEGATIVE') == '1'   # a broken check must fail this test


def check(c, what):
    print(('ok   ' if c else 'FAIL ') + what)
    if not c:
        fails.append(what)


def stamp_name(t):
    return time.strftime('%Y%m%d-%H%M%S', time.localtime(t))


t = pathlib.Path(tempfile.mkdtemp(prefix='uit-rep-'))
try:
    root = t / 'g'
    for d in ('tools', 'logs', 'diag', 'build/obj'):
        (root / d).mkdir(parents=True)
    shutil.copy(HERE / 'tools/report.py', root / 'tools')
    now = time.time()
    exe = root / 'UltraInstinctTears.exe'
    exe.write_text('exe'); os.utime(exe, (now - 3600, now - 3600))   # built an hour ago

    def game_log(age, text='x'):
        p = root / 'logs' / f'ultrainstincttears-{stamp_name(now - age)}.log'
        p.write_text(text); os.utime(p, (now - age + 60, now - age + 60))
        return p

    def report():
        out = t / 'out'
        r = subprocess.run([sys.executable, str(root / 'tools/report.py'), '--out', str(out)], capture_output=True, text=True)
        zp = pathlib.Path(r.stdout.strip().splitlines()[-1])
        z = zipfile.ZipFile(zp)
        return z.read('README.txt').decode(), set(z.namelist())

    game_log(7200)   # before the build
    readme, _ = report()
    check('is from before the last build' in readme, 'newest game log older than the exe: said so')
    game_log(1800, '[UIT 0380] shader warm-up: 18.9 ms a shader over the first 200: the graphics driver\'s shader cache looks cold (warm: 1.5-4 ms)\n')
    readme, names = report()
    check('is the FIRST launch after the build' in readme and 'shader cache looks cold' in readme,
          'first launch after the build: said, with the log\'s cold-cache line')
    game_log(600)
    readme, _ = report()
    check('is launch 2 after the build' in readme and 'FIRST' not in readme, 'second launch after the build: said so')

    for k in range(5):   # the newest three rebuild logs
        p = root / 'logs' / f'rebuild-2026111{k}-000000.log'; p.write_text('r'); os.utime(p, (now - 5000 + k, now - 5000 + k))
    (root / 'logs/update-1.log').write_text('u')
    (root / 'build/o0-files.txt').write_text('main_405 O2\nmain_301 O0\n')
    readme, names = report()
    check({f'logs/rebuild-2026111{k}-000000.log' for k in (2, 3, 4)} <= names and 'logs/rebuild-20261110-000000.log' not in names
          and 'logs/update-1.log' in names, 'the newest three rebuild logs and the update log')
    check('build/o0-files.txt' in names, 'build/o0-files.txt')

    # a profile older than the newest game log: an earlier run's
    prof = root / 'diag/profile.txt'
    prof.write_text('old'); os.utime(prof, (now - 1000, now - 1000))
    readme, _ = report()
    check(('STALE: diag/profile.txt is older than the newest game log' in readme) != neg, 'an older profile is marked STALE')
    os.utime(prof, (now - 10, now - 10))
    readme, _ = report()
    check('STALE' not in readme, 'a profile written after the newest log started: not marked')

    # the -O0 fast parts of split functions, with the reason
    (root / 'build/obj/main_301.o.split').write_text(json.dumps({'key': 'k', 'level': 'O0', 'why': 'took more than 900 s of CPU time'}))
    (root / 'build/obj/main_391.o.split').write_text(json.dumps({'key': 'k', 'level': 'O0', 'why': 'got 300 s of CPU time in 1800 s (the PC was busy)', 'busy': True}))
    (root / 'build/obj/main_405.o.split').write_text(json.dumps({'key': 'k', 'level': 'opt', 'seconds': 400}))
    readme, _ = report()
    check('2 fast part(s) of split functions are built unoptimized (-O0)' in readme and 'main_301 (took more than 900 s of CPU time)' in readme
          and 'main_391 (got 300 s' in readme and 'tried again in the next build' in readme and 'main_405' not in readme.split('fast part(s)')[1],
          'the -O0 fast parts with their reasons')

    # build_game: a busy fallback is built again; a real one (out of CPU time) is kept; the log names the reasons
    saved = build_game.ROOT
    try:
        build_game.ROOT = root
        objs = ['build/obj/main_301.o', 'build/obj/main_391.o', 'build/obj/main_405.o']
        for o in objs:
            (root / o).write_text('obj')
        out = io.StringIO()
        with redirect_stdout(out):
            build_game.retry_busy(objs)
        check(not (root / objs[1]).exists() and (root / objs[0]).exists() and (root / objs[2]).exists(),
              'retry_busy: only the busy fallback\'s object is removed (ninja builds it again)')
        check('trying optimized again (built -O0 last time because the PC was busy): main_391' in out.getvalue(),
              f'retry_busy: says so ({out.getvalue().strip()})')
        out = io.StringIO()
        with redirect_stdout(out):
            build_game.o0_list(root / 'build', [], objs)
        line = out.getvalue()
        check('1 of 3 fast part(s) built optimized; WARNING, built -O0 instead: main_301 (took more than 900 s of CPU time), '
              'main_391 (PC busy, tried again next build)' in line, f'o0_list: each -O0 fast part with its reason ({line.strip()})')
        check((root / 'build/o0-files.txt').read_text() == 'main_301 O0\nmain_391 O0\nmain_405 O2\n', 'o0-files.txt unchanged in form')
    finally:
        build_game.ROOT = saved
finally:
    shutil.rmtree(t, ignore_errors=True)

print(f'{len(fails)} failed' if fails else 'all passed')
sys.exit(1 if fails else 0)
