"""[UIT 0280] Box test for tools/update.py's GitHub Releases source against a mock api.github.com (mock_github.py):
version compare, asset selection, SHA-256 from every place a release can publish it, size/hash/version mismatches,
404 / rate limit / 401 / offline / stalled / not-JSON answers, pre-releases, the token (sent to the API, never to the
download host, never printed or logged), and download -> the existing install."""
import json, os, pathlib, shutil, subprocess, sys, tempfile, time, zipfile

HERE = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(HERE / 'tools')); sys.path.insert(0, str(HERE / 'tools/tests'))
import update as U  # noqa: E402
from mock_github import MockGitHub  # noqa: E402
ok = True
def check(c, what):
    global ok
    print(('ok   ' if c else 'FAIL ') + what); ok &= bool(c)

# ---- version compare and asset selection (no network)
K = U.version_key
check(K('1.0.0') > K('1.0.0-rc1') and K('1.0.0') > K('1.0.0-fps') and K('1.0.0') > K('1.0.0-dev'), 'version: a release is newer than its pre-releases')
check(K('1.0.0-3-gabc1234') > K('1.0.0') and K('1.0.0-3-gabc1234-dirty') < K('1.0.1') and K('1.0.0-dirty') == K('1.0.0'), 'version: git describe after a tag; -dirty ignored')
check(K('1.10.0') > K('1.9.9') and K('2.0.0') > K('1.99.99') and K('v1.2.3') == K('1.2.3'), 'version: numeric, not text, order; v prefix')
check(K('1.0.0-rc2') > K('1.0.0-rc1') and K('1.0.0-rc.10') > K('1.0.0-rc.2'), 'version: pre-release order')
check(U.is_newer('1.0.0', '0.0.0-fps') and U.is_newer('1.0.0', '') and U.is_newer('1.0.0', 'unknown') and not U.is_newer('1.0.0', '1.0.0')
      and not U.is_newer('1.0.0', '1.0.0-2-g1234567') and not U.is_newer('garbage', '0.0.1'), 'version: is_newer, unknown local version -> newer')
check(U.tag_version('v1.2.3') == '1.2.3' and U.tag_version('v1.2.3-rc1') == '1.2.3-rc1' and U.tag_version('1.2.3') == '' and U.tag_version('latest') == '', 'tag: only vX.Y.Z[-pre]')
A = lambda *ns: [{'name': n} for n in ns]
check(U.pick_asset(A('notes.txt', 'UltraInstinctTears-src.zip.sha256', 'UltraInstinctTears-src.zip'))['name'] == 'UltraInstinctTears-src.zip', 'asset: the exact name, not its .sha256')
check(U.pick_asset(A('UltraInstinctTears-src-v1.0.0.zip', 'x.zip'))['name'] == 'UltraInstinctTears-src-v1.0.0.zip', 'asset: the one src zip otherwise')
for names, word in ((A('a.zip', 'UltraTears.7z'), 'no '), (A('UltraInstinctTears-src-a.zip', 'UltraInstinctTears-src-b.zip'), 'several')):
    try: U.pick_asset(names); check(False, f'asset: {word} refused')
    except U.Fail as e: check(word in str(e), f'asset: {word} -> "{e}"')
h = 'ab' * 32
check(U.sha_from_text(f'{h}  UltraInstinctTears-src.zip\n', 'UltraInstinctTears-src.zip') == h and U.sha_from_text(h.upper(), 'x.zip') == h, 'sha: sha256sum line, lone hex')
check(U.sha_from_text(f'commit {h}\n', 'x.zip', need_word=True) == '' and U.sha_from_text(f'**SHA256**: `{h}`', 'x.zip', need_word=True) == h, 'sha: notes need the word or the name')
n = U.notes_text(f'## What\'s new\n\n- **Faster** [shaders](http://x)\n\n\n\nsha256: {h}\n')
check(n == "What's new\n\n\u2022 Faster shaders" , f'notes: Markdown reduced, checksum line dropped ({n!r})')
check(U.tsv_escape('a\tb\nc\\d') == 'a\\tb\\nc\\\\d', 'tsv escape')
long = U.notes_text('\n'.join(f'- item {i}' for i in range(40)))
check(long.count('\n') <= 17 and long.endswith('on the release page)'), 'notes: long notes cut for the dialog, pointing at the release page')
_real, U._online_line = U._online_line, lambda: (time.sleep(5), 'late')[1]
t0 = time.time(); r = U.online_line(deadline=0.5); U._online_line = _real
check(r.startswith('online\terror\t') and 'did not answer in time' in r and time.time() - t0 < 2, 'online check: an overall deadline (a stuck DNS lookup cannot hang the scan)')

# ---- a fake game folder
SETUP = "open('UltraInstinctTears.exe','w').write('new exe')\n"
t = pathlib.Path(tempfile.mkdtemp())
root = t / 'My Game Folder'
(root / 'tools').mkdir(parents=True); (root / 'src').mkdir()
for f in ('update.py', 'progress_file.py', 'version.py'): shutil.copy(HERE / 'tools' / f, root / 'tools' / f)
(root / 'tools/setup.py').write_text(SETUP); (root / 'src/a.c').write_text('old a')
(root / 'ultrainstincttears.cfg').write_text('TOTK_FPS=60\n'); (root / 'VERSION.txt').write_text('Ultra Instinct Tears 0.0.0-fps (commit aaaaaaa)\n')
(root / 'UltraInstinctTears.exe').write_text('old exe')
def zipbytes(ver, commit='bbbbbbb', a='new a'):
    p = t / 'z.zip'
    with zipfile.ZipFile(p, 'w') as z:
        for k, v in {'tools/setup.py': SETUP, 'tools/update.py': (HERE / 'tools/update.py').read_text(), 'tools/progress_file.py': (HERE / 'tools/progress_file.py').read_text(),
                     'src/a.c': a, 'VERSION.txt': f'Ultra Instinct Tears {ver} (commit {commit})\n'}.items():
            z.writestr('UltraInstinctTears/' + k, v)
    return p.read_bytes()

srv = MockGitHub().start()
TOKEN = 'ghp_TESTTOKEN_0123456789abcdefSECRET'
# [UIT 0284] --curl: every request through curl (UIT_UPDATE_HTTP=curl), the way Windows' curl.exe takes over;
# CURL_ENV is set by tools/tests/test_update_curl.py (a curl wrapper that records its command lines)
CURL_ENV = json.loads(os.environ.get('UIT_TEST_CURL_ENV', '{}')) if '--curl' in sys.argv else {}
if '--curl' in sys.argv and not CURL_ENV:
    CURL_ENV = {'UIT_UPDATE_HTTP': 'curl', 'UIT_UPDATE_CURL': shutil.which('curl') or '/usr/bin/curl'}
def run(*a, token=None, timeout=60, **env):
    e = dict(os.environ); e.pop('UIT_GITHUB_TOKEN', None)
    e.update(UIT_UPDATE_API=srv.api, UIT_UPDATE_TIMEOUT='2'); e.update(CURL_ENV); e.update(env)
    if token: e['UIT_GITHUB_TOKEN'] = token
    return subprocess.run([sys.executable, str(root / 'tools/update.py')] + list(a), capture_output=True, text=True, cwd=t, env=e, timeout=timeout)
def online(**kw):
    r = run('scan', '--tsv', '--online', **kw)
    last = r.stdout.strip().splitlines()[-1] if r.stdout.strip() else ''
    return r, last.split('\t')

# no release at all (the private repository: 404) and the other refusals: one line, exit 0, never a traceback
srv.mode = '404'
r, f = online()
check(r.returncode == 0 and f[:2] == ['online', 'error'] and 'private' in f[2] and 'Traceback' not in r.stderr, f'404: "{f[-1]}"')
check(r.stdout.startswith('current\t'), '404: the local scan lines still come first')
r, f = online(token=TOKEN)
check('token cannot read' in f[2], f'404 with a token: says the token may lack access')
srv.mode = 'ratelimit'; r, f = online()
check(f[1] == 'error' and 'limit' in f[2] and 'try again after' in f[2], f'rate limit: "{f[2]}"')
srv.mode = '401'; r, f = online(token=TOKEN)
check(f[1] == 'error' and 'refused the access token' in f[2], f'401: "{f[2]}"')
srv.mode = 'garbage'; r, f = online()
check(f[1] == 'error' and 'not JSON' in f[2], f'not JSON: "{f[2]}"')
t0 = time.time(); srv.mode = 'stall'; r, f = online()
check(f[1] == 'error' and 'could not reach' in f[2] and time.time() - t0 < 20, f'stalled server: times out ({time.time() - t0:.1f}s): "{f[2]}"')
r, f = online(UIT_UPDATE_API='http://127.0.0.1:9')
check(f[1] == 'error' and 'could not reach GitHub' in f[2], f'offline: "{f[2]}"')
(root / 'ultrainstincttears.cfg').write_text('TOTK_FPS=60\nTOTK_LAUNCHER_UPDATE_REPO=not a repo\n')
srv.mode = 'ok'; r, f = online()
check(f[1] == 'error' and 'owner/name' in f[2], 'bad repository setting: says so')
(root / 'ultrainstincttears.cfg').write_text('TOTK_FPS=60\n')
r = run('scan', '--source', 'github', '--json', UIT_UPDATE_API='http://127.0.0.1:9')
check(r.returncode == 1 and 'could not reach GitHub' in json.loads(r.stdout)['error'], 'scan --source github: the error as JSON')
r, f = online()
check(f[1] == 'error' and 'private' in f[2], 'no release yet: 404 from /latest')

# releases
z1 = zipbytes('1.0.0'); h1 = srv.add_release('v1.0.0', z1, body="## What's new\n- Faster shaders\n- Fixed the map")
r, f = online()
check(f[0] == 'release' and f[1] == 'v1.0.0' and f[2] == '1.0.0' and f[3] == '1' and f[4] == 'UltraInstinctTears-src.zip' and f[5] == str(len(z1)),
      f'release: v1.0.0 offered as newer than 0.0.0-fps ({f[:6]})')
check(f[8] == 'sha256' and 'Faster shaders' in f[9].replace('\\n', '\n') and f[10].startswith('https://github.com/') and f[11] == '0.0.0-fps', 'release: sha256 published, notes, page, local version')
srv.add_release('v1.1.0-rc1', zipbytes('1.1.0-rc1'), prerelease=True, body='test build')
srv.add_release('v9.9.9', zipbytes('9.9.9'), draft=True)
r, f = online()
check(f[1] == 'v1.0.0', 'pre-release and draft not offered by default')
(root / 'ultrainstincttears.cfg').write_text('TOTK_FPS=60\nTOTK_LAUNCHER_UPDATE_PRERELEASE=1\n')
r, f = online()
check(f[1] == 'v1.1.0-rc1' and f[7] == '1', 'TOTK_LAUNCHER_UPDATE_PRERELEASE=1: the pre-release offered (never the draft)')
(root / 'ultrainstincttears.cfg').write_text('TOTK_FPS=60\nTOTK_LAUNCHER_UPDATE_PRERELEASE=-\n')
r, f = online(); check(f[1] == 'v1.0.0', 'TOTK_LAUNCHER_UPDATE_PRERELEASE=- (default): releases only')

# download: size, hash, version checks
prog = t / 'p.json'
def dl(tag, **kw):
    prog.unlink(missing_ok=True)
    r = run('download', '--tag', tag, '--progress', str(prog), **kw)
    return r, (json.loads(prog.read_text()) if prog.is_file() else {})
r, st = dl('v1.0.0')
zp = pathlib.Path(r.stdout.strip().splitlines()[-1].split('\t', 1)[1]) if r.returncode == 0 else None
check(r.returncode == 0 and st['state'] == 'done' and st['pct'] == 100 and zp and zp.read_bytes() == z1 and st['zip'] == str(zp), 'download: done, zip in place')
check(zp and zp.parent == root / 'updates' / 'downloads' and not list(zp.parent.glob('*.part')), 'download: into updates/downloads, no .part left')
dlog = (root / 'logs/update-download.log').read_text()
check(h1 in dlog and 'UltraInstinctTears-src.zip.sha256' in dlog, 'download: the SHA-256 and where it was checked are logged')
z2 = zipbytes('1.0.1'); srv.add_release('v1.0.1', z2, sha='wrong')
r, st = dl('v1.0.1')
check(r.returncode == 1 and st['state'] == 'failed' and 'SHA-256' in st['msg'] and not list((root / 'updates/downloads').glob('*v1.0.1*')), f'wrong sha256: refused, nothing kept ("{st.get("msg")}")')
check(zp.is_file(), 'wrong sha256: the previous good download kept')
srv.add_release('v1.0.2', zipbytes('1.0.2'), sha='digest'); r, st = dl('v1.0.2')
check(r.returncode == 0 and "GitHub's asset digest" in (root / 'logs/update-download.log').read_text(), "sha256 from GitHub's asset digest")
check(not zp.exists(), 'older download removed once a new one is in')
srv.add_release('v1.0.3', zipbytes('1.0.3'), sha='body'); r, st = dl('v1.0.3')
check(r.returncode == 0 and 'the release notes' in (root / 'logs/update-download.log').read_text(), 'sha256 from the release notes')
zb = zipbytes('1.0.4'); srv.add_release('v1.0.4', zb, sha=None)
srv.releases[0]['body'] = f'sha256: {"1" * 64}'; r, st = dl('v1.0.4')
check(r.returncode == 1 and 'SHA-256' in st['msg'], 'wrong sha256 in the notes: refused')
srv.add_release('v1.0.5', zipbytes('1.0.5'), sha=None); r, st = dl('v1.0.5')
check(r.returncode == 0 and 'publishes none' in st['msg'] + (root / 'logs/update-download.log').read_text(), 'no sha256 published: size and package checked, says so')
srv.add_release('v1.0.6', zipbytes('1.0.6'), sha=None); srv.releases[0]['assets'][0]['size'] += 10; r, st = dl('v1.0.6')
check(r.returncode == 1 and 'ended after' in st['msg'], f'size mismatch: refused ("{st["msg"]}")')
srv.add_release('v1.0.7', zipbytes('1.0.7'), sha=None); srv.truncate = True; r, st = dl('v1.0.7'); srv.truncate = False
check(r.returncode == 1 and st['state'] == 'failed' and not list((root / 'updates/downloads').glob('*.part')), f'cut-off download: refused, .part removed ("{st["msg"]}")')
srv.add_release('v1.0.8', zipbytes('0.0.0-fps')); r, st = dl('v1.0.8')
check(r.returncode == 1 and 'without the tag' in st['msg'], 'zip of another version than the tag: refused')
p = t / 'n.zip'; zipfile.ZipFile(p, 'w').writestr('readme.txt', 'x'); srv.add_release('v1.0.9', p.read_bytes()); r, st = dl('v1.0.9')
check(r.returncode == 1 and 'not an Ultra Instinct Tears source package' in st['msg'], 'not a source package: refused')
srv.add_release('v1.1.0', b'x' * 100, extra_assets=(('UltraInstinctTears-src.zip.sha256x', b''),), name='Other.zip'); r, st = dl('v1.1.0')
check(r.returncode == 1 and 'has no UltraInstinctTears-src.zip' in st['msg'], 'release without the zip: refused')
r, st = dl('v7.7.7'); check(r.returncode == 1 and 'no published release' in st['msg'], 'unknown tag: 404 said')
srv.mode = 'ratelimit'; r, st = dl('v1.0.0'); srv.mode = 'ok'
check(r.returncode == 1 and 'limit' in st['msg'] and 'Traceback' not in r.stderr + r.stdout, 'download while rate-limited: failed state, no traceback')

# the token: sent to the API only (not to the download host), never printed or logged
srv.auth_seen.clear()
(root / 'updates').mkdir(exist_ok=True); (root / 'updates/github-token.txt').write_text(TOKEN + '\n')
r, f = online(); r2, st = dl('v1.0.0')
check(r2.returncode == 0 and srv.auth_seen and all(h == 'Bearer ' + TOKEN for _, h in srv.auth_seen), 'token from updates/github-token.txt sent as Bearer' + ('' if r2.returncode == 0 else f' (download: {st.get("msg")})'))
check(not any(p.startswith('/dl/') for p, _ in srv.auth_seen) and any('/assets/' in p for p, _ in srv.auth_seen), 'token: API asset URL used, not forwarded across the redirect')
seen = r.stdout + r.stderr + r2.stdout + r2.stderr + prog.read_text() + ''.join(p.read_text(errors='replace') for p in (root / 'logs').glob('*'))
check(TOKEN not in seen and 'SECRET' not in seen, 'token never printed, logged or in the progress file')
check('with an access token' in (root / 'logs/update-download.log').read_text(), 'token: the log says one was used (not what it is)')
(root / 'updates/github-token.txt').unlink(); srv.auth_seen.clear()
r, f = online(); check(not srv.auth_seen, 'no token: no Authorization header')
r, f = online(UIT_GITHUB_TOKEN='bad token with spaces'); check(not srv.auth_seen, 'a malformed token is ignored, not sent')

# download -> the existing install
r, st = dl('v1.0.0'); zp = pathlib.Path(st['zip'])
r = run('install', str(zp), '--progress', str(prog))
st = json.loads(prog.read_text())
check(r.returncode == 0 and st['state'] == 'done' and (root / 'src/a.c').read_text() == 'new a' and 'commit bbbbbbb' in (root / 'VERSION.txt').read_text(), 'downloaded zip installs with the same install')
srv.releases = [x for x in srv.releases if x['tag_name'] == 'v1.0.0']
r, f = online(); check(f[0] == 'release' and f[3] == '0', 'after the install: v1.0.0 is not newer (up to date)')
srv.stop(); shutil.rmtree(t)
print('all ok' if ok else 'FAILURES'); sys.exit(0 if ok else 1)
