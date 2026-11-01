"""Box test for tools/update.py: a fake game folder, packages, success, failure with rollback, refusals, scan."""
import json, os, pathlib, shutil, subprocess, sys, tempfile, zipfile

HERE = pathlib.Path(__file__).resolve().parents[2]
ok = True
def check(c, what):
    global ok
    print(('ok   ' if c else 'FAIL ') + what); ok &= bool(c)

def snapshot(d):
    return {str(p.relative_to(d)): p.read_bytes() for p in d.rglob('*') if p.is_file() and 'updates' not in p.parts and 'logs' not in p.parts and p.name != 'fail.txt'}

def pkg(path, files, top='UltraInstinctTears/'):
    with zipfile.ZipFile(path, 'w') as z:
        for n, data in files.items(): z.writestr(top + n, data)

SETUP = "import os,sys\nprint('[1/2] check'); print('[2/2] compile')\nprint('stamp', os.environ.get('UIT_BUILD_VERSION'), os.environ.get('UIT_BUILD_COMMIT'))\nopen('UltraInstinctTears.exe','w').write('new exe')\nsys.exit(int(open('fail.txt').read()) if os.path.exists('fail.txt') else 0)\n"
t = pathlib.Path(tempfile.mkdtemp())
root = t / 'My Game Folder'
(root / 'tools').mkdir(parents=True); (root / 'src').mkdir(); (root / 'data').mkdir(); (root / 'userdata').mkdir()
shutil.copy(HERE / 'tools/update.py', root / 'tools/update.py'); shutil.copy(HERE / 'tools/progress_file.py', root / 'tools/progress_file.py')
(root / 'tools/setup.py').write_text(SETUP)
(root / 'src/a.c').write_text('old a'); (root / 'src/same.c').write_text('same')
(root / 'data/big.bin').write_text('lifted'); (root / 'userdata/save.dat').write_text('my save')
(root / 'ultrainstincttears.cfg').write_text('TOTK_FPS=60\n'); (root / 'VERSION.txt').write_text('Ultra Instinct Tears 0.0.0-fps (commit aaaaaaa)\n')
(root / 'UltraInstinctTears.exe').write_text('old exe'); (root / 'UltraInstinctTearsLauncher.exe').write_text('old launcher')
new = {'tools/setup.py': SETUP, 'tools/update.py': (HERE / 'tools/update.py').read_text(),
       'tools/progress_file.py': (HERE / 'tools/progress_file.py').read_text(), 'src/a.c': 'new a', 'src/same.c': 'same',
       'src/b.c': 'added', 'VERSION.txt': 'Ultra Instinct Tears 0.0.0-fps (commit bbbbbbb)\n',
       'data/big.bin': 'from the zip', 'userdata/save.dat': 'zip save', 'ultrainstincttears.cfg': 'TOTK_FPS=30\n'}
dl = t / 'somewhere else'; dl.mkdir()
pkg(dl / 'UltraInstinctTears-src-new.zip', new)
pkg(dl / 'UltraInstinctTears-src-junk.zip', {'readme.txt': 'x'})
pkg(dl / 'UltraInstinctTears-src-evil.zip', {'tools/setup.py': 'x', 'src/a.c': 'x', '../escape.txt': 'x'})
U = [sys.executable, str(root / 'tools/update.py')]
run = lambda *a: subprocess.run(U + list(a), capture_output=True, text=True, cwd=t)

r = run('scan', '--folder', str(dl), '--json'); j = json.loads(r.stdout)
check([p['label'] for p in j['packages']] == ['UltraInstinctTears-src-new.zip'], 'scan: the package found, junk and unsafe zips left out')
check(j['current']['commit'] == 'aaaaaaa', 'scan: current version read')
r = subprocess.run(U + ['scan', '--source', 'github', '--json'], capture_output=True, text=True, cwd=t, env=dict(os.environ, UIT_UPDATE_API='http://127.0.0.1:9'))
check(r.returncode == 1 and 'could not reach GitHub' in json.loads(r.stdout)['error'], 'github source: offline said in one line (UIT 0280; tools/tests/test_update_github.py tests it)')
before = snapshot(root)
prog = t / 'p.json'
r = run('install', str(dl / 'UltraInstinctTears-src-evil.zip'), '--progress', str(prog))
check(r.returncode == 1 and json.loads(prog.read_text())['state'] == 'failed' and not (t / 'escape.txt').exists(), 'unsafe zip refused')
check(snapshot(root) == before, 'unsafe zip: nothing changed')
# failing build: rolled back
(root / 'fail.txt').write_text('1')
r = run('install', str(dl / 'UltraInstinctTears-src-new.zip'), '--progress', str(prog))
st = json.loads(prog.read_text())
check(r.returncode == 1 and st['state'] == 'rolled_back', 'failing build: rolled back')
check(snapshot(root) == before, 'failing build: every file as before (exe, sources, cfg, data, saves), added file removed')
# success
(root / 'fail.txt').unlink(); before = snapshot(root)
r = run('install', str(dl / 'UltraInstinctTears-src-new.zip'), '--progress', str(prog))
st = json.loads(prog.read_text()); after = snapshot(root)
check(r.returncode == 0 and st['state'] == 'done' and st['pct'] == 100, 'install: done')
check(after['src/a.c'] == b'new a' and after['src/b.c'] == b'added' and after['UltraInstinctTears.exe'] == b'new exe', 'install: new files in, rebuilt')
check(after['data/big.bin'] == b'lifted' and after['userdata/save.dat'] == b'my save' and after['ultrainstincttears.cfg'] == b'TOTK_FPS=60\n',
      'install: data, saves and settings untouched')
check(after.get('UltraInstinctTearsLauncher.old.exe') == b'old launcher', 'install: running launcher moved aside')
ulog = max((root / 'logs').glob('update-*.log'), key=lambda p: p.stat().st_mtime).read_text(encoding='utf-8')
check('stamp 0.0.0-fps bbbbbbb' in ulog, "install: the build is given the package's version and commit, not VERSION.txt's old one (UIT 0129)")
bks = list((root / 'updates').glob('backup-*')); check(len(bks) >= 1 and (bks[-1] / 'files/ultrainstincttears.cfg').is_file(), 'install: cfg backed up')
r = run('install', str(dl / 'UltraInstinctTears-src-new.zip'), '--progress', str(prog))
check(r.returncode == 1 and 'already installed' in json.loads(prog.read_text())['msg'], 'same version refused without --force')
import time; time.sleep(1.1); r = run('rollback'); after2 = snapshot(root)
check(after2['src/a.c'] == b'old a' and 'src/b.c' not in after2 and after2['UltraInstinctTears.exe'] == b'old exe', 'manual rollback')
(root / '.git').mkdir(); r = run('install', str(dl / 'UltraInstinctTears-src-new.zip'), '--progress', str(prog), '--force')
check(r.returncode == 1 and 'Git checkout' in json.loads(prog.read_text())['msg'], 'git checkout refused')

# ---------------------------------------------------------------- [UIT 0046]
# setup.py stand-in: fail.txt '1' fails the build, 'kill' kills the updater mid-build (a PC turned off, a closed console)
SETUP2 = ("import os,sys,signal\nprint('[1/2] check', flush=True)\nf=open('fail.txt').read() if os.path.exists('fail.txt') else ''\n"
          "if f=='kill': os.kill(os.getppid(), signal.SIGKILL)\nprint('[2/2] compile')\n"
          "open('UltraInstinctTears.exe','w').write('new exe')\nsys.exit(int(f) if f.isdigit() else 0)\n")
def fresh(name):
    r = t / name
    (r / 'tools').mkdir(parents=True); (r / 'src').mkdir()
    for f in ('update.py', 'progress_file.py'): shutil.copy(HERE / 'tools' / f, r / 'tools' / f)
    (r / 'tools/setup.py').write_text(SETUP2)
    (r / 'src/a.c').write_text('old a'); (r / 'src/same.c').write_text('same')
    (r / 'ultrainstincttears.cfg').write_text('TOTK_FPS=60\n'); (r / 'VERSION.txt').write_text('Ultra Instinct Tears 0.0.0-fps (commit aaaaaaa)\n')
    (r / 'UltraInstinctTears.exe').write_text('old exe')
    return r
p2 = dict(new, **{'tools/setup.py': SETUP2}); pkg(dl / 'UltraInstinctTears-src-p2.zip', p2)
p3 = dict(p2, **{'src/a.c': 'third a', 'VERSION.txt': 'Ultra Instinct Tears 0.0.0-fps (commit ccccccc)\n'}); pkg(dl / 'UltraInstinctTears-src-p3.zip', p3)
def runin(r, *a, wrap=None):
    cmd = [sys.executable] + (['-c', wrap, str(r / 'tools/update.py')] if wrap else [str(r / 'tools/update.py')]) + list(a)
    return subprocess.run(cmd, capture_output=True, text=True, cwd=t)
def lastlog(r):
    return sorted((r / 'logs').glob('update-*.log'))[-1].read_text()
# 1. the progress file is locked for the whole install (os.replace onto it always raises PermissionError, like
#    "[WinError 5] Access is denied" while a reader holds it without FILE_SHARE_DELETE): the update still completes
LOCK = ("import os,sys,runpy\nreal=os.replace\ncount=[0]\n"
        "def rp(a,b,*k,**kw):\n    if str(b).endswith('p.json'):\n        count[0]+=1; raise PermissionError(13,'[WinError 5] Access is denied')\n    return real(a,b,*k,**kw)\n"
        "os.replace=rp\nsys.argv=sys.argv[1:]\ntry:\n    runpy.run_path(sys.argv[0], run_name='__main__')\n"
        "finally:\n    sys.stderr.write('replace-attempts=%d\\n' % count[0])\n")
r1 = fresh('locked'); prog1 = t / 'p.json'; prog1.unlink(missing_ok=True)
r = runin(r1, 'install', str(dl / 'UltraInstinctTears-src-p2.zip'), '--progress', str(prog1), wrap=LOCK)
st = json.loads(prog1.read_text()) if prog1.is_file() else {}
check(r.returncode == 0 and (r1 / 'src/a.c').read_text() == 'new a' and (r1 / 'UltraInstinctTears.exe').read_text() == 'new exe',
      'locked progress file: the update completes (installed and built)')
check('replace-attempts=' in r.stderr and int(r.stderr.split('replace-attempts=')[1].split()[0]) > 5, 'locked progress file: os.replace really failed every time')
check(st.get('state') == 'done', 'locked progress file: the final state still reaches the launcher (written in place)')
log1 = lastlog(r1)
check(log1.count('progress display could not be updated') == 1, 'locked progress file: noted once in the log')
check('commit bbbbbbb' in (r1 / 'VERSION.txt').read_text() and not (r1 / 'updates/pending.json').exists(), 'locked: VERSION.txt new, nothing pending')
# 2. the updater is killed mid-build: VERSION.txt still the old one, pending.json kept; installing again resumes
r2 = fresh('killed'); (r2 / 'fail.txt').write_text('kill')
r = runin(r2, 'install', str(dl / 'UltraInstinctTears-src-p2.zip'), '--progress', str(prog1))
check(r.returncode != 0 and (r2 / 'updates/pending.json').is_file(), 'killed mid-build: the update is marked unfinished')
check('aaaaaaa' in (r2 / 'VERSION.txt').read_text() and (r2 / 'src/a.c').read_text() == 'new a',
      'killed mid-build: new sources in, VERSION.txt still says the old version')
(r2 / 'fail.txt').unlink()
r = runin(r2, 'install', str(dl / 'UltraInstinctTears-src-p2.zip'), '--progress', str(prog1))
log2 = lastlog(r2)
check(r.returncode == 0 and 'resuming the unfinished update' in log2 and json.loads(prog1.read_text())['state'] == 'done',
      'install again: resumes and finishes')
check('1 files change, 0 are new' in log2 and 'changes: VERSION.txt' in log2 and 'bbbbbbb' in (r2 / 'VERSION.txt').read_text() and not (r2 / 'updates/pending.json').exists(),
      'resume: only VERSION.txt left to write (after the build), written, nothing pending')
import time as _t; _t.sleep(1.1)
r = runin(r2, 'rollback')
check((r2 / 'src/a.c').read_text() == 'old a' and 'aaaaaaa' in (r2 / 'VERSION.txt').read_text() and not (r2 / 'src/b.c').exists(),
      'rollback after a resumed update: back to the version before the first attempt')
# 3. killed, then a different package: the unfinished one is rolled back first, then the new one installs
r3 = fresh('other'); (r3 / 'fail.txt').write_text('kill')
runin(r3, 'install', str(dl / 'UltraInstinctTears-src-p2.zip'), '--progress', str(prog1)); (r3 / 'fail.txt').unlink()
r = runin(r3, 'install', str(dl / 'UltraInstinctTears-src-p3.zip'), '--progress', str(prog1))
check(r.returncode == 0 and 'an unfinished update' in lastlog(r3) and (r3 / 'src/a.c').read_text() == 'third a' and 'ccccccc' in (r3 / 'VERSION.txt').read_text(),
      'another package over an unfinished update: rolled back first, then installed')
bk3 = sorted((r3 / 'updates').glob('backup-*'))[-1]
check((bk3 / 'files/src/a.c').read_text() == 'old a', "  and its backup holds the original files (a rollback returns to before both)")
# 4. a failing build still rolls back, with the version named
r4 = fresh('fails'); (r4 / 'fail.txt').write_text('1')
r = runin(r4, 'install', str(dl / 'UltraInstinctTears-src-p2.zip'), '--progress', str(prog1))
st = json.loads(prog1.read_text())
check(r.returncode == 1 and st['state'] == 'rolled_back' and 'aaaaaaa back in place' in st['msg'] and (r4 / 'src/a.c').read_text() == 'old a'
      and not (r4 / 'updates/pending.json').exists(), 'failing build: rolled back, message names the version, nothing pending')
# 5. the package brings a newer updater: it runs (from a temporary copy) on this folder
p5 = dict(p2, **{'tools/update.py': (HERE / 'tools/update.py').read_text() + '\n# a newer updater\n'}); pkg(dl / 'UltraInstinctTears-src-p5.zip', p5)
r5 = fresh('handoff')
r = runin(r5, 'install', str(dl / 'UltraInstinctTears-src-p5.zip'), '--progress', str(prog1))
logs5 = ''.join(p.read_text() for p in (r5 / 'logs').glob('update-*.log'))
check(r.returncode == 0 and 'handed over' in logs5 and (r5 / 'src/a.c').read_text() == 'new a' and 'a newer updater' in (r5 / 'tools/update.py').read_text()
      and json.loads(prog1.read_text())['state'] == 'done', "newer updater in the package: it does the install, in this folder")
# ---------------------------------------------------------------- [UIT 0055] no console, invalid std handles
# Windows: subprocess duplicates every std handle a child is not given; the launcher starts the updater with stdin
# invalid, so Popen(stdout=PIPE) with stdin left alone failed: "[WinError 6] The handle is invalid". Emulated here: a
# Popen that leaves any std handle to inheritance while setting another raises that error.
WINH = ("import subprocess,sys,runpy\nreal=subprocess.Popen.__init__\n"
        "def init(self,args,*a,**kw):\n    h=[kw.get(k) for k in ('stdin','stdout','stderr')]\n"
        "    if any(x is None for x in h) and any(x is not None for x in h):\n"
        "        raise OSError(6,'[WinError 6] The handle is invalid')\n    return real(self,args,*a,**kw)\n"
        "subprocess.Popen.__init__=init\nsys.argv=sys.argv[1:]\nrunpy.run_path(sys.argv[0], run_name='__main__')\n")
r6 = fresh('winhandles'); prog1.unlink(missing_ok=True)
r = runin(r6, 'install', str(dl / 'UltraInstinctTears-src-p2.zip'), '--progress', str(prog1), wrap=WINH)
log6 = lastlog(r6)
check(r.returncode == 0 and json.loads(prog1.read_text())['state'] == 'done' and (r6 / 'UltraInstinctTears.exe').read_text() == 'new exe',
      'invalid std handles (Windows emulated): the build runs, update done')
check('[1/2] check' in log6 and '[2/2] compile' in log6, '  and the build output is in the log')
r6b = fresh('winhandles-handoff')
r = runin(r6b, 'install', str(dl / 'UltraInstinctTears-src-p5.zip'), '--progress', str(prog1), wrap=WINH)
logs6b = ''.join(p.read_text() for p in (r6b / 'logs').glob('update-*.log'))
check(r.returncode == 0 and 'handed over' in logs6b and json.loads(prog1.read_text())['state'] == 'done',
      'invalid std handles: the handoff to a newer updater runs too')
# stdin, stdout and stderr closed (no console at all): the build's child gets NUL for stdin (reads end at once,
# input() does not hang), its output (not ASCII too) still reaches the log and the progress
SETUP3 = ("import sys\nprint('[1/2] check')\nprint('stdin:', repr(sys.stdin.read()))\n"
          "try:\n    input('question? ')\nexcept EOFError:\n    print('input: EOF')\n"
          "print('[2/2] compile \u2192 done')\nopen('UltraInstinctTears.exe','w').write('new exe')\n")
p7 = dict(p2, **{'tools/setup.py': SETUP3}); pkg(dl / 'UltraInstinctTears-src-p7.zip', p7)
r7 = fresh('closed'); prog1.unlink(missing_ok=True)
r = subprocess.run(['sh', '-c', 'exec "$0" "$1" install "$2" --progress "$3" <&- >&- 2>&-', sys.executable,
                    str(r7 / 'tools/update.py'), str(dl / 'UltraInstinctTears-src-p7.zip'), str(prog1)], cwd=t, timeout=60)
log7 = lastlog(r7)
check(r.returncode == 0 and json.loads(prog1.read_text())['state'] == 'done' and (r7 / 'UltraInstinctTears.exe').read_text() == 'new exe',
      'std handles closed: update done')
check("stdin: ''" in log7 and 'input: EOF' in log7 and '[2/2] compile \u2192 done' in log7, '  build stdin is NUL, its output (UTF-8) in the log')
# setup.py's own guard: print, flush and input() with no std streams at all
GUARD = ("import sys\nsys.path.insert(0, sys.argv[1])\nfrom update import std_streams\nstd_streams()\n"
         "print('x', flush=True); sys.stderr.write('y'); sys.stderr.flush()\n"
         "try:\n    input()\nexcept EOFError:\n    pass\nopen(sys.argv[2], 'w').write('ok')\n")
mark = t / 'guard.txt'
r = subprocess.run(['sh', '-c', 'exec "$0" -c "$1" "$2" "$3" <&- >&- 2>&-', sys.executable, GUARD, str(HERE / 'tools'), str(mark)], timeout=60)
check(r.returncode == 0 and mark.is_file(), 'std_streams: print, flush and input() work with no std streams')
# ---------------------------------------------------------------- [UIT 0103] progress during the compile step
# setup.py's steps are "[i/N] name"; ninja's own "[k/N] <command>" lines (the first build, ThinLTO's one-time full
# rebuild) are progress inside setup's compile step, not a step of their own, and the bar does not run back
sys.path.insert(0, str(HERE / 'tools')); import update as U2
cur, seen = {}, []
for line in ['setup.py starting', '[1/7] check', '  NOTE: ThinLTO is now on by default', '[4/7] compile',
             '[1/4500] C:\\msys64\\clang64\\bin\\clang.exe -c src/a.c', '[2250/4500] C:\\msys64\\clang64\\bin\\clang.exe -c src/b.c',
             '[4500/4500] C:\\msys64\\clang64\\bin\\ld.lld.exe -o UltraInstinctTears.exe', '  ThinLTO link FAILED (the lines above say why).',
             '[1/4500] C:\\msys64\\clang64\\bin\\clang.exe -c src/a.c', '[5/7] launcher', '[7/7] finish']:
    seen.append(U2.build_progress(line, cur))
check(seen[0] == {'msg': 'setup.py starting'} and seen[2] == {'msg': 'NOTE: ThinLTO is now on by default'}, 'progress: plain lines only change the message')
check(seen[1]['step'] == 'Rebuilding, step 1 of 7: check' and seen[1]['pct'] == 25, 'progress: setup step 1 of 7')
check(seen[4]['step'] == 'Rebuilding, step 4 of 7: compile (1 of 4500)', 'progress: ninja line shown inside the compile step')
check(seen[3]['pct'] <= seen[4]['pct'] <= seen[5]['pct'] <= seen[6]['pct'] <= seen[9]['pct'], 'progress: the bar moves forward through the compile')
check(seen[6]['pct'] <= int(25 + 74 * 4 / 7), 'progress: the compile never shows more than the next step')
check(seen[8]['step'].endswith('compile (1 of 4500)') and seen[9]['step'] == 'Rebuilding, step 5 of 7: launcher', 'progress: rebuild after the ThinLTO fallback, then the next step')
check(U2.build_progress('[3/0] x', {}) == {'msg': '[3/0] x'} and 'step' not in U2.build_progress('[3/9] ninja before any step', {}),
      'progress: no division by zero, no step before setup names one')
SETUP4 = ("print('[1/2] check'); print('[1/3] C:/clang.exe -c a.c'); print('[3/3] C:/ld.lld.exe -o x.exe'); print('[2/2] compile')\n"
          "open('UltraInstinctTears.exe','w').write('new exe')\n")
p8 = dict(p2, **{'tools/setup.py': SETUP4}); pkg(dl / 'UltraInstinctTears-src-p8.zip', p8)
r8 = fresh('ninja-lines'); prog1.unlink(missing_ok=True)
r = runin(r8, 'install', str(dl / 'UltraInstinctTears-src-p8.zip'), '--progress', str(prog1))
check(r.returncode == 0 and json.loads(prog1.read_text())['state'] == 'done' and '[3/3] C:/ld.lld.exe' in lastlog(r8), 'install with ninja lines in the build output: done, lines in the log')
# [UIT 0157] a package without VERSION.txt (plain git archive): the commit from the zip comment, not the folder's old one
def pkg_comment(path, files, comment):
    pkg(path, files)
    with zipfile.ZipFile(path, 'a') as z: z.comment = comment
p9 = {k: v for k, v in p2.items() if k != 'VERSION.txt'}
pkg_comment(dl / 'UltraInstinctTears-src-p9.zip', dict(p9, **{'src/a.c': 'ninth a'}), b'843a8f7' + b'0' * 33)
r9 = fresh('no-version'); prog1.unlink(missing_ok=True)
r = runin(r9, 'install', str(dl / 'UltraInstinctTears-src-p9.zip'), '--progress', str(prog1))
check(r.returncode == 0 and (r9 / 'VERSION.txt').read_text() == 'Ultra Instinct Tears 0.0.0-fps (commit 843a8f7)\n' and 'no VERSION.txt' in lastlog(r9),
      'no VERSION.txt in the package: written from the zip comment\'s commit, with a warning')
time.sleep(1.1); r = runin(r9, 'rollback')
check('aaaaaaa' in (r9 / 'VERSION.txt').read_text() and (r9 / 'src/a.c').read_text() == 'old a', 'no VERSION.txt: rollback puts the old VERSION.txt back')
pkg(dl / 'UltraInstinctTears-src-p10.zip', dict(p9, **{'src/a.c': 'tenth a'}))
r10 = fresh('no-version-no-comment'); prog1.unlink(missing_ok=True)
r = runin(r10, 'install', str(dl / 'UltraInstinctTears-src-p10.zip'), '--progress', str(prog1))
check(r.returncode == 0 and not (r10 / 'VERSION.txt').exists() and (r10 / 'src/a.c').read_text() == 'tenth a',
      'no VERSION.txt and no commit: the old VERSION.txt is removed, not left naming another version')
shutil.rmtree(t)
print('all ok' if ok else 'FAILURES'); sys.exit(0 if ok else 1)
