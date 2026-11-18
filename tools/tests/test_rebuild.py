"""[UIT 0389] About > Rebuild the game: tools/update.py rebuild (backup, success, rollback, refusals) and the build record
tools/setup.py writes (data/built-settings.txt) from launcher/settings.ini "rebuild = yes"."""
import json, os, pathlib, shutil, subprocess, sys, tempfile

HERE = pathlib.Path(__file__).resolve().parents[2]
ok = True
def check(c, what):
    global ok
    print(('ok   ' if c else 'FAIL ') + what); ok &= bool(c)

def snapshot(d):
    return {str(p.relative_to(d)): p.read_bytes() for p in d.rglob('*')
            if p.is_file() and not {'updates', 'logs', '__pycache__'} & set(p.parts) and p.name != 'fail.txt'}

# the record: the rebuild keys of settings.ini, the cfg's values (env wins), empty when unset
sys.path.insert(0, str(HERE / 'tools'))
import setup  # noqa: E402
t = pathlib.Path(tempfile.mkdtemp(prefix='uit-rebuild-'))
r0 = t / 'rec'; (r0 / 'launcher').mkdir(parents=True)
shutil.copy(HERE / 'launcher/settings.ini', r0 / 'launcher/settings.ini')
(r0 / 'ultrainstincttears.cfg').write_text('TOTK_FPS=60\nTOTK_LIFT_SPLIT_HUGE=1\nTOTK_LIFT_ZSTD=0\n')
keys = setup.rebuild_keys(r0)
check('TOTK_LIFT_SPLIT_HUGE' in keys and 'TOTK_SETUP_O0_TRY' in keys and 'TOTK_ROWCACHE' in keys, 'record: lift, compile and row cache keys are rebuild keys')
check('TOTK_FPS' not in keys and 'TOTK_SETUP_REWARM' not in keys, 'record: game-start and warm-up settings are not')
os.environ.pop('TOTK_LIFT_REGCACHE', None); os.environ['TOTK_ROWCACHE'] = '1'
setup.write_built_settings(r0)
del os.environ['TOTK_ROWCACHE']
rec = dict(l.split('=', 1) for l in (r0 / 'data/built-settings.txt').read_text().splitlines() if l and not l.startswith('#'))
check(rec.get('TOTK_LIFT_SPLIT_HUGE') == '1' and rec.get('TOTK_LIFT_ZSTD') == '0', 'record: values from the settings file')
check(rec.get('TOTK_LIFT_REGCACHE') == '' and rec.get('TOTK_ROWCACHE') == '1', 'record: unset is empty, the environment wins')
check(set(rec) == set(keys) and not (r0 / 'data/built-settings.txt.new').exists(), 'record: every rebuild key, written in one go')

# update.py rebuild on a fake folder
SETUP = ("import os,sys\nprint('[1/3] lift'); print('[2/3] compile'); print('[3/3] finish')\n"
         "print('stamp', os.environ.get('UIT_BUILD_VERSION'))\n"
         "open('UltraInstinctTears.exe','w').write('rebuilt exe'); open('UltraInstinctTearsLauncher.exe','w').write('rebuilt launcher')\n"
         "open('ultrainstincttears.cfg','a').write('# setup tidied\\n')\n"
         "if os.path.exists('fail.txt'): sys.exit(int(open('fail.txt').read()))\n"
         "os.makedirs('data', exist_ok=True); open('data/built-settings.txt','w').write('TOTK_LIFT_SPLIT_HUGE=1\\n')\n")
root = t / 'My Game'
(root / 'tools').mkdir(parents=True); (root / 'data').mkdir(); (root / 'userdata').mkdir()
for f in ('update.py', 'progress_file.py'):
    shutil.copy(HERE / 'tools' / f, root / 'tools' / f)
(root / 'tools/setup.py').write_text(SETUP)
(root / 'data/big.bin').write_text('lifted'); (root / 'userdata/save.dat').write_text('my save')
(root / 'data/built-settings.txt').write_text('TOTK_LIFT_SPLIT_HUGE=\n')
(root / 'ultrainstincttears.cfg').write_text('TOTK_LIFT_SPLIT_HUGE=1\n')
(root / 'VERSION.txt').write_text('Ultra Instinct Tears 1.0.3 (commit aaaaaaa)\n')
(root / 'UltraInstinctTears.exe').write_text('old exe'); (root / 'UltraInstinctTearsLauncher.exe').write_text('old launcher')
U = [sys.executable, str(root / 'tools/update.py')]
prog = t / 'p.json'
run = lambda *a: subprocess.run(U + list(a), capture_output=True, text=True, cwd=t, env=dict(os.environ, UIT_BUILD_VERSION='9.9.9'))

before = snapshot(root)
(root / 'fail.txt').write_text('1')
r = run('rebuild', '--progress', str(prog)); st = json.loads(prog.read_text())
check(r.returncode == 1 and st['state'] == 'rolled_back', 'failing rebuild: rolled back')
check(st['msg'] == "The game couldn't be rebuilt. Your game is back to how it was.", 'failing rebuild: plain words')
aft = snapshot(root)
check(aft == before, 'failing rebuild: exes, settings file and build record as before')
if aft != before: print({k: (before.get(k), aft.get(k)) for k in set(before) | set(aft) if before.get(k) != aft.get(k)})
check(not (root / 'updates/pending.json').exists(), 'failing rebuild: nothing left pending')
(root / 'fail.txt').unlink()
r = run('rebuild', '--progress', str(prog)); st = json.loads(prog.read_text()); after = snapshot(root)
check(r.returncode == 0 and st['state'] == 'done' and st['pct'] == 100 and st['msg'] == 'Game rebuilt', 'rebuild: done')
check(after['UltraInstinctTears.exe'] == b'rebuilt exe' and after['data/built-settings.txt'] == b'TOTK_LIFT_SPLIT_HUGE=1\n', 'rebuild: new exe and record')
check(after['userdata/save.dat'] == b'my save' and after['data/big.bin'] == b'lifted' and after['VERSION.txt'] == before['VERSION.txt'],
      'rebuild: saves, game data and version kept')
log = sorted((root / 'logs').glob('rebuild-*.log'))[-1].read_text()
check('stamp None' in log, 'rebuild: the version stamp comes from VERSION.txt (no forced version)')
# [UIT 0440] no backup step and nothing kept afterwards: the safeguard's folder of a finished rebuild is deleted
mans = [json.loads(m.read_text()) for m in (root / 'updates').glob('backup-*/manifest.json')]
check(not any(m.get('finished') for m in mans), 'rebuild: the safeguard of a finished rebuild is not kept (no backup)')
check(st.get('title') == 'Done', 'rebuild: the same progress fields as an update')
# an unfinished update: refused, nothing changed
bk = root / 'updates/backup-x'; (bk / 'files').mkdir(parents=True)
(bk / 'manifest.json').write_text(json.dumps({'to': {'version': '1.0.4'}, 'replaced': [], 'added': [], 'kept': [], 'exes': []}))
(root / 'updates/pending.json').write_text(json.dumps({'backup': 'backup-x'}))
before = snapshot(root)
r = run('rebuild', '--progress', str(prog)); st = json.loads(prog.read_text())
check(r.returncode == 1 and st['state'] == 'failed' and st['msg'].startswith("The game couldn't be rebuilt. Nothing in your game was changed."),
      'unfinished update: refused in plain words')
check(snapshot(root) == before, 'unfinished update: nothing changed')
# an unfinished rebuild: put back first, then rebuilt
(bk / 'manifest.json').write_text(json.dumps({'rebuild': True, 'to': {}, 'replaced': [], 'added': [], 'kept': [], 'exes': ['UltraInstinctTears.exe']}))
(bk / 'files/UltraInstinctTears.exe').write_text('exe before')
r = run('rebuild', '--progress', str(prog)); st = json.loads(prog.read_text())
check(r.returncode == 0 and st['state'] == 'done' and json.loads((bk / 'manifest.json').read_text()).get('rolled_back'),
      'unfinished rebuild: put back, then rebuilt')
# no setup.py: not started
(root / 'tools/setup.py').unlink()
r = run('rebuild', '--progress', str(prog)); st = json.loads(prog.read_text())
check(r.returncode == 1 and 'Nothing in your game was changed' in st['msg'] and not (root / 'updates/pending.json').exists(), 'no setup.py: not started')
shutil.rmtree(t, ignore_errors=True)
print('all ok' if ok else 'FAILED')
sys.exit(0 if ok else 1)
