"""Box test for tools/report.py: the right files, newest logs only, no game data or saves, big logs trimmed."""
import os, pathlib, shutil, subprocess, sys, tempfile, time, zipfile
HERE = pathlib.Path(__file__).resolve().parents[2]
t = pathlib.Path(tempfile.mkdtemp()); root = t / 'g'
for d in ('tools', 'logs', 'data', 'userdata'): (root / d).mkdir(parents=True)
shutil.copy(HERE / 'tools/report.py', root / 'tools')
for k in range(4):
    (root / f'logs/ultrainstincttears-2026102{k}-000000.log').write_text(f'log {k}'); os.utime(root / f'logs/ultrainstincttears-2026102{k}-000000.log', (1e9 + k, 1e9 + k))
(root / 'logs/update-1.log').write_text('u'); (root / 'launcher-crash.txt').write_text('crash')
(root / 'ultrainstincttears.cfg').write_text('TOTK_FPS=60\n'); (root / 'VERSION.txt').write_text('v')
(root / 'data/generated').mkdir(); (root / 'diag').mkdir()
for n in ('data/generated/lift-scan.txt', 'data/generated/lift-features.ini', 'data/generated/lift-scan-words.txt', 'diag/lift-probe.txt', 'diag/lift-confirm.txt'): (root / n).write_text('x')
(root / 'data/setup.json').write_text('{}'); (root / 'data/romfs').write_text('GAME'); (root / 'userdata/save.dat').write_text('SAVE')
with open(root / 'logs/ultrainstincttears-20261029-000000.log', 'wb') as f: f.seek(70 << 20); f.write(b'end')
out = t / 'out'
r = subprocess.run([sys.executable, str(root / 'tools/report.py'), '--out', str(out)], capture_output=True, text=True)
zp = pathlib.Path(r.stdout.strip().splitlines()[-1]); names = set(zipfile.ZipFile(zp).namelist())
ok = True
def check(c, w):
    global ok; print(('ok   ' if c else 'FAIL ') + w); ok &= bool(c)
check(zp.parent == out and zp.is_file(), 'zip written to the chosen folder')
check({'logs/ultrainstincttears-20261029-000000.log', 'logs/ultrainstincttears-20261023-000000.log'} <= names and 'logs/ultrainstincttears-20261020-000000.log' not in names, 'two newest game logs')
check({'ultrainstincttears.cfg', 'VERSION.txt', 'launcher-crash.txt', 'logs/update-1.log', 'data/setup.json', 'README.txt'} <= names, 'cfg, version, crash, update log, setup state')
check({'data/generated/lift-scan.txt', 'data/generated/lift-features.ini', 'diag/lift-probe.txt', 'diag/lift-confirm.txt'} <= names, 'lift scan results and shadow-size check')
check('data/generated/lift-scan-words.txt' not in names, 'no game instruction words')
check(not any('romfs' in n or 'save' in n for n in names), 'no game data or saves')
big = zipfile.ZipFile(zp).getinfo('logs/ultrainstincttears-20261029-000000.log').file_size
check(big < (65 << 20) and zipfile.ZipFile(zp).read('logs/ultrainstincttears-20261029-000000.log').endswith(b'end'), 'big log trimmed, its end kept')
check('The newest game run' not in zipfile.ZipFile(zp).read('README.txt').decode() and 'diag/amiibo.txt' not in names,
      'no "run without a log" note when the newest run has its log')
# [UIT 0180] the amiibo log is added; a run newer than the newest log (Play, not Play with log) is pointed out
(root / 'diag/amiibo.txt').write_text('12:00:00 scanned Link')
(root / 'diag/frame-capture-20261031-131345.txt').write_text('frame')
for f in (root / 'logs').glob('ultrainstincttears-*.log'): os.utime(f, (1e9, 1e9))
r = subprocess.run([sys.executable, str(root / 'tools/report.py'), '--out', str(out)], capture_output=True, text=True)
zp2 = pathlib.Path(r.stdout.strip().splitlines()[-1]); z2 = zipfile.ZipFile(zp2)
readme = z2.read('README.txt').decode()
check('diag/amiibo.txt' in z2.namelist(), 'diag/amiibo.txt in the report')
check('The newest game run' in readme and 'Play with log' in readme and 'The newest game run' in r.stdout,
      'a run newer than the newest log: README and output say to use Play with log')
# [UIT 0259] lasrecomp.log (a run started without the launcher) is added; as the newest run's log it needs no note
(root / 'lasrecomp.log').write_text('[boot] double-click run')
r = subprocess.run([sys.executable, str(root / 'tools/report.py'), '--out', str(out)], capture_output=True, text=True)
zp3 = pathlib.Path(r.stdout.strip().splitlines()[-1]); z3 = zipfile.ZipFile(zp3)
check('lasrecomp.log' in z3.namelist() and z3.read('lasrecomp.log') == b'[boot] double-click run', 'lasrecomp.log in the report')
check('The newest game run' not in z3.read('README.txt').decode(), 'lasrecomp.log newer than the frame capture: no missing-log note')
# [UIT 0368] the CPU profiler's report is added, with its time in the README; its .tmp is not
check('diag/profile.txt' not in names, 'no profile.txt when the profiler never ran')
(root / 'diag/profile.txt').write_text('[prof] top functions'); (root / 'diag/profile.txt.tmp').write_text('half')
r = subprocess.run([sys.executable, str(root / 'tools/report.py'), '--out', str(out)], capture_output=True, text=True)
zp4 = pathlib.Path(r.stdout.strip().splitlines()[-1]); z4 = zipfile.ZipFile(zp4)
check(z4.read('diag/profile.txt') == b'[prof] top functions' and 'diag/profile.txt.tmp' not in z4.namelist(), 'diag/profile.txt in the report')
check('diag/profile.txt (CPU profiler) was last written' in z4.read('README.txt').decode(), 'README gives the profile time')
shutil.rmtree(t); print('all ok'  if ok else 'FAILURES'); sys.exit(0 if ok else 1)
