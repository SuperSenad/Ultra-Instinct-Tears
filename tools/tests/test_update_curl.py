"""[UIT 0284] Box test for the updater's Windows curl.exe path (tools/update.py, GitHubReleasesSource._curl_open).
On the player's PC the launcher runs update.py with deps\\python\\python.exe, a python.org copy with _ssl.pyd and
OpenSSL, trusting Windows' certificate store; when Python cannot verify GitHub's certificate (a root Windows has not
fetched yet) or has no ssl at all, Windows' own curl.exe takes over. Here /usr/bin/curl stands in for it:
  1. the whole GitHub suite (test_update_github.py --curl) with every request through curl, via a wrapper that records
     each command line: the token is never on it (it goes on curl's stdin);
  2. an https mock with a certificate Python does not trust but "Windows" (curl with --cacert) does: the check and the
     download fall back to curl by themselves and say so in the log; with no curl, the error names the date and
     Windows Update;
  3. a Python without the ssl module: curl from the start;
  4. the curl command itself: https only for real hosts, never --insecure / -k, no --location, timeouts set."""
import json, os, pathlib, shutil, subprocess, sys, tempfile, zipfile

HERE = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(HERE / 'tools')); sys.path.insert(0, str(HERE / 'tools/tests'))
import update as U  # noqa: E402
from mock_github import MockGitHub  # noqa: E402
ok = True
def check(c, what):
    global ok
    print(('ok   ' if c else 'FAIL ') + what); ok &= bool(c)

CURL = shutil.which('curl')
if not CURL:
    print('skipped: no curl on this box'); sys.exit(0)
t = pathlib.Path(tempfile.mkdtemp())
argv_log = t / 'curl-argv.txt'
def wrapper(name, extra=''):
    w = t / name
    w.write_text(f'#!/bin/sh\nprintf "%s\\n" "$*" >> "{argv_log}"\nexec "{CURL}" {extra} "$@"\n')
    w.chmod(0o755)
    return w

# ---- 1. the whole GitHub suite through curl
w = wrapper('curl-plain')
TOKEN = 'ghp_TESTTOKEN_0123456789abcdefSECRET'   # the one test_update_github.py uses
env = dict(os.environ, UIT_TEST_CURL_ENV=json.dumps({'UIT_UPDATE_HTTP': 'curl', 'UIT_UPDATE_CURL': str(w)}))
r = subprocess.run([sys.executable, str(HERE / 'tools/tests/test_update_github.py'), '--curl'], capture_output=True, text=True, env=env, timeout=600)
fails = [l for l in r.stdout.splitlines() if l.startswith('FAIL')]
check(r.returncode == 0 and not fails, 'the GitHub suite passes with every request through curl' + (': ' + '; '.join(fails) if fails else ''))
lines = argv_log.read_text().splitlines() if argv_log.is_file() else []
check(len(lines) > 20, f'curl was really used ({len(lines)} runs)')
check(lines and not any(TOKEN in l or 'SECRET' in l or 'Authorization' in l for l in lines), 'the token is never on curl\'s command line')
check(lines and all('--location' not in l and '--insecure' not in l and ' -k ' not in l for l in lines), 'no --location, never --insecure')

# ---- 2. a certificate Python does not trust: falls back to curl by itself
cert, key = t / 'cert.pem', t / 'key.pem'
subprocess.run(['openssl', 'req', '-x509', '-newkey', 'rsa:2048', '-nodes', '-days', '2', '-subj', '/CN=127.0.0.1',
                '-addext', 'subjectAltName=IP:127.0.0.1', '-keyout', str(key), '-out', str(cert)], check=True, capture_output=True)
srv = MockGitHub(tls=(str(cert), str(key))).start()
root = t / 'game'
(root / 'tools').mkdir(parents=True)
for f in ('update.py', 'progress_file.py', 'version.py'):
    shutil.copy(HERE / 'tools' / f, root / 'tools' / f)
(root / 'VERSION.txt').write_text('Ultra Instinct Tears 0.0.0-fps (commit aaaaaaa)\n')
(root / 'ultrainstincttears.cfg').write_text('TOTK_FPS=60\n')
zb = t / 'z.zip'
with zipfile.ZipFile(zb, 'w') as z:
    z.writestr('UltraInstinctTears/VERSION.txt', 'Ultra Instinct Tears 1.0.0 (commit bbbbbbb)\n')
    z.writestr('UltraInstinctTears/tools/setup.py', 'pass\n')
    z.writestr('UltraInstinctTears/src/a.c', 'new a')
srv.add_release('v1.0.0', zb.read_bytes(), body='- Faster shaders')
trusting = wrapper('curl-windows-trusts', f'--cacert "{cert}"')
def run(*a, **kw):
    e = dict(os.environ); e.pop('UIT_GITHUB_TOKEN', None); e.pop('UIT_UPDATE_HTTP', None)
    e.update(UIT_UPDATE_API=srv.api, UIT_UPDATE_TIMEOUT='3', UIT_UPDATE_CURL=str(trusting)); e.update(kw)
    return subprocess.run([sys.executable, str(root / 'tools/update.py')] + list(a), capture_output=True, text=True, env=e, timeout=120, cwd=t)
def online(**kw):
    r = run('scan', '--tsv', '--online', **kw)
    return r, (r.stdout.strip().splitlines() or [''])[-1].split('\t')
r, f = online(UIT_UPDATE_HTTP='python')
check(f[:2] == ['online', 'error'] and 'certificate' in f[2].lower() and 'Windows Update' in f[2], f'Python alone: a certificate error that says what to do ("{f[-1][:110]}")')
r, f = online(UIT_UPDATE_CURL=str(t / 'no-curl.exe'))
check(f[:2] == ['online', 'error'] and 'Windows Update' in f[2], 'no curl.exe: the same clear error, no crash')
r, f = online()
check(f[0] == 'release' and f[1] == 'v1.0.0' and f[3] == '1', f'certificate Python does not trust: the check falls back to curl ({f[:2]})')
prog = t / 'p.json'
r = run('download', '--tag', 'v1.0.0', '--progress', str(prog))
st = json.loads(prog.read_text()) if prog.is_file() else {}
log = (root / 'logs/update-download.log').read_text() if (root / 'logs/update-download.log').is_file() else ''
check(r.returncode == 0 and st.get('state') == 'done' and pathlib.Path(st.get('zip', '/')).read_bytes() == zb.read_bytes(), 'download falls back to curl: done, the same bytes')
check("using Windows' curl.exe instead" in log, 'the download log says curl.exe took over')
wrong = wrapper('curl-trusts-nothing')   # curl that cannot verify either: refused, never --insecure
r, f = online(UIT_UPDATE_CURL=str(wrong))
check(f[:2] == ['online', 'error'] and 'Traceback' not in r.stderr and 'certificate' in f[2] and 'Windows Update' in f[2], f'neither can verify: refused cleanly, with curl\'s reason and the hint ("{f[-1][:110]}")')

# ---- 3. a Python without ssl (TOTK_LAUNCHER_PYTHON set to one): curl from the start
nossl = t / 'nossl'; nossl.mkdir()
(nossl / 'ssl.py').write_text("raise ImportError('this Python was built without ssl')\n")
prog.unlink(missing_ok=True)
r = run('download', '--tag', 'v1.0.0', '--progress', str(prog), PYTHONPATH=str(nossl))
st = json.loads(prog.read_text()) if prog.is_file() else {}
log = (root / 'logs/update-download.log').read_text() if (root / 'logs/update-download.log').is_file() else ''
check(r.returncode == 0 and st.get('state') == 'done' and 'no ssl module' in log, f'no ssl module: downloads through curl ({st.get("msg")})')
r, f = online(PYTHONPATH=str(nossl), UIT_UPDATE_CURL=str(t / 'no-curl.exe'))
check(f[:2] == ['online', 'error'] and 'Traceback' not in r.stderr and 'no ssl module' in f[2], f'no ssl and no curl: one error line ("{f[-1][:100]}")')
srv.stop()

# ---- 4. the curl command for the real GitHub
seen = []
class P:
    def __init__(self, cmd, **kw):
        seen.append(cmd); raise OSError('not run')
real, U.subprocess.Popen = U.subprocess.Popen, P
try:
    os.environ['UIT_UPDATE_CURL'] = CURL
    s = U.GitHubReleasesSource(token=TOKEN, api='https://api.github.com')
    s.use_curl = True
    try: s._json('/repos/x/y/releases/latest')
    except U.Fail as e: msg = str(e)
finally:
    U.subprocess.Popen = real
c = seen[0] if seen else []
check(c and c[c.index('--proto') + 1] == '=https' and '--connect-timeout' in c and '--speed-time' in c and '--config' in c
      and not any(TOKEN in x for x in c) and '--insecure' not in c and '-k' not in c and '--location' not in c,
      'curl command: https only, timeouts, token via --config - (stdin), never --insecure / --location')
check('could not start curl.exe' in msg, 'curl.exe that will not start: a Fail, not a traceback')
check(U.find_curl() and str(U.find_curl()) == CURL, 'find_curl: UIT_UPDATE_CURL')
os.environ['UIT_UPDATE_CURL'] = str(t / 'missing')
check(U.find_curl() is None, 'find_curl: a missing curl is None (no fallback, Python only)')
del os.environ['UIT_UPDATE_CURL']
check(U.find_curl() is None, 'find_curl: not Windows: no System32\\curl.exe')
shutil.rmtree(t)
print('all ok' if ok else 'FAILURES'); sys.exit(0 if ok else 1)
