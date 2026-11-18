#!/usr/bin/env python3
"""[UIT 0433] Tests for the first-time installer (UltraInstinctTears-Setup.exe: launcher/installer.inc).

usage: python3 tools/test_installer.py LAUNCHER [--cxx CMD] [--shots DIR]
LAUNCHER is a Linux launcher build (tools/build_launcher.py --target linux ...); --install --payload ZIP starts it as
the installer, the same code the Windows installer runs. Needs no game files and no network:
  - the zip reader, inflate and SHA-256 (launcher/unzip.cpp, sha256.cpp) against Python's zipfile and hashlib
    (tools/tests/test_unzip.cpp, built here with --cxx)
  - the three code-file hashes the installer checks are the ones tools/setup.py checks (EXEFS)
  - every page through --script: the folder rules, each game-files problem (a synthetic stand-in folder: the
    installer's test-only hash override UIT_SETUP_TEST_EXEFS, honoured only in a --script run), the whole install
    with a stand-in build (tools/setup.py replaced by a stub that prints setup's steps), a failed build (Try again,
    Close removes everything), Stop and remove, an unfinished install picked up again, and an update over an install
    (saves kept).
  - [UIT 0435] the newest release, online: a fake GitHub on 127.0.0.1 (an http server with a releases list, the packages and
    their checksums) through the installer's UIT_SETUP_RELEASES and UIT_SETUP_TEST_HTTP (the first only honoured in a --script run, the second only for 127.0.0.1):
    a newer release is installed instead of the embedded package (also over an install), the same or an older one is ignored,
    pre-releases are followed only by an installer that is itself newer than Latest, a wrong or missing checksum, a version
    that is not the tag's, a server that is down, GitHub's rate limit and notes with non-ASCII text all end in the embedded
    package or the right one, and Stop ends a download at once.
"""
import argparse, hashlib, http.server, json, os, pathlib, re, shutil, subprocess, sys, tempfile, threading, time, zipfile

ROOT = pathlib.Path(__file__).resolve().parent.parent

STUB_SETUP = r'''"""stand-in for tools/setup.py (tools/test_installer.py): setup's step lines, a few counts, then the launcher"""
import os, pathlib, sys, time
args = sys.argv[1:]
root = pathlib.Path(__file__).resolve().parent.parent
(root / 'logs').mkdir(exist_ok=True)
(root / 'logs' / 'stub-setup-args.txt').write_text(' '.join(args))
slow = float(os.environ.get('UIT_STUB_SLOW', '0'))
steps = ['check', 'romfs', 'lift', 'compile', 'shaders', 'textures', 'finish']
for k, name in enumerate(steps, 1):
    print(f'[{k}/{len(steps)}] {name}', flush=True)
    if name == 'compile':
        for j in range(1, 6):
            print(f'[{j}/5] compiling part {j}', flush=True)
            time.sleep(slow / 5)
    if os.environ.get('UIT_STUB_FAIL') == name:
        print('error: the stand-in build failed here', flush=True)
        sys.exit(1)
    time.sleep(slow / 7)
(root / 'UltraInstinctTears.exe').write_bytes(b'stand-in')
(root / 'UltraInstinctTearsLauncher.exe').write_bytes(b'stand-in')
print('done', flush=True)
'''
STUB_REPORT = 'import sys\nprint("report: stand-in")\n'


def version_txt(v, c):
    return f'Ultra Instinct Tears {v} (commit {c})\n'


def make_payload(path, version='1.0.4', commit='abc1234', setup=STUB_SETUP, readme='stand-in\n', pad=0):
    """the source package as the installer carries it: the real tools/*.py, a stand-in tools/setup.py"""
    with zipfile.ZipFile(path, 'w', zipfile.ZIP_DEFLATED) as z:
        for f in sorted((ROOT / 'tools').glob('*.py')):
            if f.name not in ('setup.py', 'report.py'):
                z.write(f, 'UltraInstinctTears/tools/' + f.name)
        z.writestr('UltraInstinctTears/tools/setup.py', setup)
        z.writestr('UltraInstinctTears/tools/report.py', STUB_REPORT)
        z.writestr('UltraInstinctTears/VERSION.txt', version_txt(version, commit))
        z.writestr('UltraInstinctTears/README.md', readme)
        z.writestr('UltraInstinctTears/src/stand-in.c', 'int x;\n')
        z.writestr('UltraInstinctTears/docs/', '')
        z.writestr('UltraInstinctTears/docs/a b/notes.txt', 'nested\n' * 1000)
        if pad:   # incompressible bytes: a package of a known size
            z.writestr('UltraInstinctTears/docs/pad.bin', os.urandom(pad), compress_type=zipfile.ZIP_STORED)


def make_builder(path):
    """a stand-in builder package: the files tools/setup.py's deps_ready looks for"""
    top = 'UltraTears-builder-0.1.0-windows-x86_64/'
    with zipfile.ZipFile(path, 'w', zipfile.ZIP_DEFLATED) as z:
        for rel in ('deps/python/python.exe', 'deps/clang64/bin/clang.exe', 'deps/clang64/bin/clang++.exe', 'deps/clang64/bin/lld.exe',
                    'deps/clang64/bin/ninja.exe', 'deps/shaders/ultrs-shaders.exe', 'licenses/README.txt', 'README.md'):
            z.writestr(top + rel, b'stand-in ' + rel.encode() * 50)


class FakeGitHub:
    """[UIT 0435] a stand-in for api.github.com and the release downloads, on 127.0.0.1.
    releases: newest first, each {tag, pre, draft, assets: {name: bytes}, body, digest: name->bool}. status: what every API
    answer is (403: the hourly limit). slow: seconds a download takes (streamed). hits: the paths asked for, in order."""

    def __init__(self, releases, status=200, slow=0.0, ascii_json=False, burst=0):
        self.releases, self.status, self.slow, self.ascii_json, self.burst = releases, status, slow, ascii_json, burst   # burst: bytes sent before the slow part
        self.hits, self.cut = [], 0
        me = self

        class H(http.server.BaseHTTPRequestHandler):
            protocol_version = 'HTTP/1.1'

            def log_message(self, *a):
                pass

            def do_GET(self):
                me.hits.append(self.path)
                try:
                    me.serve(self)
                except (BrokenPipeError, ConnectionResetError):
                    me.cut += 1

        self.srv = http.server.ThreadingHTTPServer(('127.0.0.1', 0), H)
        self.srv.daemon_threads = True
        self.port = self.srv.server_address[1]
        self.base = f'http://127.0.0.1:{self.port}'
        threading.Thread(target=self.srv.serve_forever, daemon=True).start()

    def env(self):
        return {'UIT_SETUP_RELEASES': self.base + '/api', 'UIT_SETUP_TEST_HTTP': '1'}

    def release_json(self, r):
        assets = []
        for name, data in r['assets'].items():
            a = {'name': name, 'size': len(data), 'browser_download_url': f'{self.base}/dl/{r["tag"]}/{name}',
                 'state': 'uploaded', 'content_type': 'application/zip'}
            if r.get('digest', {}).get(name):
                a['digest'] = 'sha256:' + hashlib.sha256(data).hexdigest()
            assets.append(a)
        return {'tag_name': r['tag'], 'name': r['tag'], 'draft': bool(r.get('draft')), 'prerelease': bool(r.get('pre')),
                'body': r.get('body', ''), 'published_at': '2026-11-18T12:00:00Z', 'assets': assets,
                'html_url': f'{self.base}/rel/{r["tag"]}'}

    def send(self, h, code, body, ctype='application/json'):
        h.send_response(code)
        h.send_header('Content-Type', ctype)
        h.send_header('Content-Length', str(len(body)))
        h.end_headers()
        h.wfile.write(body)

    def serve(self, h):
        path = h.path.replace('/repos/SuperSenad/Ultra-Instinct-Tears', '/api', 1)   # (tools/update.py's own address form too)
        if path.startswith('/dl/'):
            _, _, tag, name = path.split('/', 3)
            r = next((x for x in self.releases if x['tag'] == tag), None)
            data = r['assets'].get(name) if r else None
            if data is None:
                return self.send(h, 404, b'{}')
            h.send_response(200)
            h.send_header('Content-Type', 'application/octet-stream')
            h.send_header('Content-Length', str(len(data)))
            h.end_headers()
            chunks = [data[i:i + 8192] for i in range(0, len(data), 8192)] or [b'']
            sent = 0
            for c in chunks:
                h.wfile.write(c)
                h.wfile.flush()
                sent += len(c)
                if self.slow and sent > self.burst:
                    time.sleep(self.slow / len(chunks))
            return None
        if self.status != 200:
            return self.send(h, self.status, json.dumps({'message': 'API rate limit exceeded'}).encode())
        dump = lambda o: json.dumps(o, ensure_ascii=self.ascii_json).encode('utf-8')
        if path == '/api/releases/latest':
            r = next((x for x in self.releases if not x.get('pre') and not x.get('draft')), None)
            return self.send(h, 200, dump(self.release_json(r))) if r else self.send(h, 404, b'{"message":"Not Found"}')
        if path.startswith('/api/releases?'):
            return self.send(h, 200, dump([self.release_json(x) for x in self.releases]))
        return self.send(h, 404, b'{}')


def package(version, commit='fedcba9', readme='newest\n', pad=0):
    """a source package (bytes) as a release carries it: its own VERSION.txt and README.md"""
    tmpf = pathlib.Path(tempfile.mkdtemp(prefix='uit-pkg-')) / 'p.zip'
    make_payload(tmpf, version, commit, readme=readme, pad=pad)
    data = tmpf.read_bytes()
    shutil.rmtree(tmpf.parent, ignore_errors=True)
    return data


def release(tag, version=None, commit='fedcba9', pre=False, sha='side', body='', readme='newest\n', zip_says=None, draft=False, pad=0):
    """a release with UltraInstinctTears-src.zip; sha: 'side' (a .sha256 asset), 'wrong' (a .sha256 with another hash), 'digest'
    (GitHub's asset digest only), 'notes' (a line in the notes), 'none'; zip_says: the version its VERSION.txt gives"""
    data = package(zip_says or version or tag.lstrip('v'), commit, readme, pad)
    name = 'UltraInstinctTears-src.zip'
    h = hashlib.sha256(data).hexdigest()
    r = {'tag': tag, 'pre': pre, 'draft': draft, 'assets': {name: data}, 'body': body, 'digest': {}}
    if sha == 'side':
        r['assets'][name + '.sha256'] = f'{h}  {name}\n'.encode()
    elif sha == 'wrong':
        r['assets'][name + '.sha256'] = f'{"0" * 64}  {name}\n'.encode()
    elif sha == 'digest':
        r['digest'][name] = True
    elif sha == 'notes':
        r['body'] = body + f'\n\nSHA-256 of {name}: {h}\n'
    return r


def check_updater_notes():
    """tools/update.py check against the fake GitHub with release notes in non-ASCII text (as JSON text and as \\u escapes), whatever
    encoding its output pipe has: one clean 'release' line, never an error or a traceback"""
    probs = []
    body = '## Neu \u2014 caf\u00e9 \u65e5\u672c\u8a9e \u2713 \U0001f600 \u201cq\u201d\nline \xa0 end\n\n- item \u2022 one'
    for ascii_json in (False, True):
        for enc in ('cp1252', 'ascii'):
            g = FakeGitHub([release('v1.0.5', sha='notes', body=body)], ascii_json=ascii_json)
            env = dict(os.environ, UIT_UPDATE_API=g.base, UIT_UPDATE_HTTP='python', PYTHONIOENCODING=enc)
            r = subprocess.run([sys.executable, str(ROOT / 'tools' / 'update.py'), 'check'], env=env, capture_output=True, cwd=ROOT, timeout=60)
            g.srv.shutdown()
            line = r.stdout.decode('utf-8', 'replace')
            if r.returncode or not line.startswith('release\tv1.0.5\t') or r.stderr:
                probs.append(f'update.py check (json ascii={ascii_json}, stdout {enc}): {line[:120]!r} {r.stderr[-200:]!r}')
    return probs, '4 runs'


def log_has(d, text, where='inst'):
    """text is in the install's log (the folder's logs/, or the copy kept when everything was removed)"""
    logs = list((d / where / 'logs').glob('install-*.log')) + list((d / 'pref').glob('last-install.log'))
    txt = '\n'.join(l.read_text(encoding='utf-8', errors='replace') for l in logs)
    return [] if text in txt else [f'the log has no "{text}": ' + txt[-600:]]


def log_lacks(d, text, where='inst'):
    logs = list((d / where / 'logs').glob('install-*.log'))
    txt = '\n'.join(l.read_text(encoding='utf-8', errors='replace') for l in logs)
    return [f'the log has "{text}"'] if text in txt else []


def readme_is(d, want, where='inst'):
    f = d / where / 'README.md'
    got = f.read_text() if f.is_file() else None
    return [] if got == want else [f'{where}/README.md is {got!r}, expected {want!r}']


def version_is(d, version, where='inst'):
    f = d / where / 'VERSION.txt'
    got = f.read_text() if f.is_file() else ''
    return [] if got.startswith(f'Ultra Instinct Tears {version} ') else [f'{where}/VERSION.txt says {got!r}, expected {version}']


def make_game(d, missing=None, wrong=None, romfs=True, packed=False):
    """a synthetic stand-in for the player's game files (never real ones): exefs/{main,subsdk0,sdk}, romfs/Pack"""
    d = pathlib.Path(d)
    hashes = {}
    if packed:
        d.mkdir(parents=True)
        (d / 'game.nsp').write_bytes(b'x')
        return hashes
    (d / 'exefs').mkdir(parents=True)
    for n in ('main', 'subsdk0', 'sdk'):
        data = f'stand-in {n}'.encode()
        if n != missing:
            (d / 'exefs' / n).write_bytes(data + (b'!' if n == wrong else b''))
        hashes[n] = hashlib.sha256(data).hexdigest()
    if romfs:
        (d / 'romfs' / 'Pack').mkdir(parents=True)
        (d / 'romfs' / 'Pack' / 'ZsDic.pack.zs').write_bytes(b'stand-in')
    return hashes


def check_hashes():
    """the installer's three hashes are tools/setup.py's EXEFS"""
    src = (ROOT / 'launcher' / 'installer.inc').read_text(encoding='utf-8')
    have = dict(re.findall(r'\{"(main|subsdk0|sdk)", "([0-9a-f]{64})"\}', src))
    setup = (ROOT / 'tools' / 'setup.py').read_text(encoding='utf-8')
    m = re.search(r'EXEFS = \{(.*?)\}', setup, re.S)
    want = dict(re.findall(r"'(\w+)': '([0-9a-f]{64})'", m.group(1))) if m else {}
    return [] if have == want and len(want) == 3 else [f'installer hashes {have} differ from tools/setup.py EXEFS {want}']


def check_unzip(cxx, sdl, tmp):
    if not cxx:
        return [], 'skipped: no --cxx'
    exe = tmp / 'test_unzip'
    cmd = [*cxx.split(), '-std=c++17', '-O2', '-nostdinc++', '-nostdlib++', '-fno-exceptions', '-fno-rtti', '-fno-threadsafe-statics',
           '-Ilauncher', f'-I{sdl}/include', 'tools/tests/test_unzip.cpp', 'launcher/unzip.cpp', 'launcher/sha256.cpp',
           f'{sdl}/lib/libSDL3.a', '-lpthread', '-lm', '-ldl', '-o', str(exe)]
    r = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True)
    if r.returncode:
        return ['test_unzip did not build:\n' + r.stderr[-2000:]], ''
    probs = []
    z = tmp / 'mixed.zip'
    with zipfile.ZipFile(z, 'w') as w:
        w.writestr('stored.bin', os.urandom(5000), compress_type=zipfile.ZIP_STORED)
        w.writestr('empty.txt', b'', compress_type=zipfile.ZIP_DEFLATED)
        for lvl in (1, 6, 9):
            w.writestr(f'text{lvl}.txt', (b'the quick brown fox ' * 3000) + os.urandom(3000), compress_type=zipfile.ZIP_DEFLATED, compresslevel=lvl)
        w.writestr('random.bin', os.urandom(300000), compress_type=zipfile.ZIP_DEFLATED)
        w.writestr('runs.bin', bytes(range(256)) * 4000 + b'\0' * 100000, compress_type=zipfile.ZIP_DEFLATED)
    pre = tmp / 'prefixed.bin'   # the same zip with something before it (as appended to the installer exe)
    pre.write_bytes(os.urandom(12345) + z.read_bytes())
    cases = [(z, 0), (pre, 12345)]
    big = ROOT.parent / 'builder' / 'UltraTears-builder-0.1.0-windows-x86_64.zip'
    if big.is_file():
        cases.append((big, 0))
    for path, start in cases:
        want = []
        with zipfile.ZipFile(path) as w:
            for i in w.infolist():
                if i.is_dir():
                    continue
                b = w.read(i)
                want.append(f'{i.filename}\t{len(b)}\t{i.CRC:08x}\t{hashlib.sha256(b).hexdigest()}')
        r = subprocess.run([str(exe), 'list', str(path), str(start)], capture_output=True, text=True)
        have = r.stdout.splitlines()
        if r.returncode or have != want:
            bad = [h for h, w2 in zip(have, want) if h != w2][:3]
            probs.append(f'{path.name}: {len(have)} of {len(want)} entries read alike (exit {r.returncode}) {bad}')
    for name, ok in (('a/b.txt', True), ('../x', False), ('/etc/passwd', False), ('C:/x', False), ('a\\..\\b', False)):
        r = subprocess.run([str(exe), 'safe', name], capture_output=True, text=True)
        if (r.stdout.strip() != '-') != ok:
            probs.append(f'zip_safe_name({name!r}) = {r.stdout.strip()!r}')
    return probs, f'{len(cases)} zips'


class Run:
    def __init__(self, exe, tmp, shots):
        self.exe, self.tmp, self.shots = exe, tmp, shots
        self.payload = tmp / 'payload.zip'
        make_payload(self.payload)
        self.builder = tmp / 'builder.zip'
        make_builder(self.builder)
        self.n = 0

    def case(self, name, script, free_gb='100', env_extra=None, prep=None, check=None, payload=None):
        self.n += 1
        d = self.tmp / f'case{self.n}'
        d.mkdir()
        ph = {'D': str(d), 'GOOD': str(d / 'game')}
        hashes = make_game(d / 'game')
        (d / 'home').mkdir()
        (d / 'desktop').mkdir()
        (d / 'start').mkdir()
        env = dict(os.environ, SDL_VIDEODRIVER='offscreen', SDL_AUDIODRIVER='dummy', UIT_SETUP_TEST_PREF=str(d / 'pref'),
                   UIT_SETUP_TOOLS=str(self.builder), UIT_SETUP_TEST_FREE_GB=free_gb, UIT_SETUP_TEST_DESKTOP=str(d / 'desktop'), UIT_SETUP_TEST_START=str(d / 'start'),
                   UIT_SETUP_TEST_STARTED=str(d / 'started.txt'), UIT_SETUP_API='http://127.0.0.1:9/none', UIT_SETUP_RELEASES='http://127.0.0.1:9/none', UIT_SETUP_TEST_HTTP='1', HOME=str(d / 'home'),
                   UIT_SETUP_TEST_EXEFS=';'.join(f'{k}={v}' for k, v in hashes.items()))
        env.update(env_extra or {})
        if prep:
            prep(d)
        for k, v in ph.items():
            script = script.replace('{' + k + '}', v)
        if self.shots:
            script = script.replace('shot:', 'shot:' + str(self.shots) + '/')
        else:
            script = ';'.join(c for c in script.split(';') if not c.startswith('shot:'))
        t0 = time.time()
        r = subprocess.run([self.exe, '--install', '--payload', str(payload or self.payload), '--dir', str(d / 'exe-dir'), '--size', '1280x800',
                            '--script', 'wait:2;' + script + ';quit'], env=env, capture_output=True, text=True, timeout=300)
        self.last_secs = time.time() - t0
        out = r.stdout + r.stderr
        probs = [] if r.returncode == 0 else [f'exit code {r.returncode}']
        probs += [l for l in out.splitlines() if 'FAILED' in l or 'unknown command' in l or 'Sanitizer' in l or 'runtime error' in l]
        if check:
            probs += check(d)
        print(('ok    ' if not probs else 'FAIL  ') + name, flush=True)
        for p in probs:
            print('        ' + p)
        if probs:
            print('\n'.join('      | ' + l for l in out.splitlines()[-30:]))
        return not probs


def missing(d, *rels):
    return [f'{r} is missing' for r in rels if not (d / r).exists()]


def prep_installed(d):
    """an older install (not a git checkout) with a save and settings; remembered by the installer"""
    old = d / 'old'
    shutil.unpack_archive(str(d.parent / 'payload.zip'), str(d / 'unz'))
    shutil.move(str(d / 'unz' / 'UltraInstinctTears'), str(old))
    (old / 'VERSION.txt').write_text(version_txt('1.0.3', 'fff0000'))
    (old / 'userdata' / 'save' / '0001').mkdir(parents=True)
    (old / 'userdata' / 'save' / '0001' / 'progress.sav').write_bytes(b'save')
    (old / 'ultrainstincttears.cfg').write_text('TOTK_FPS=60\n')
    (old / 'UltraInstinctTearsLauncher.exe').write_bytes(b'stand-in')
    (d / 'pref').mkdir()
    (d / 'pref' / 'install.txt').write_text(str(old) + '\n')


def prep_unfinished(d):
    shutil.unpack_archive(str(d.parent / 'payload.zip'), str(d / 'unz'))
    shutil.move(str(d / 'unz' / 'UltraInstinctTears'), str(d / 'inst'))
    (d / 'inst' / '.uit-setup').write_text('x')


def check_full(d):
    p = missing(d, 'inst/UltraInstinctTearsLauncher.exe', 'inst/deps/clang64/bin/ninja.exe', 'inst/licenses/README.txt', 'inst/tools/update.py',
                'inst/docs/a b/notes.txt', 'desktop/Ultra Instinct Tears.lnk', 'start/Ultra Instinct Tears.lnk', 'pref/install.txt')
    if (d / 'inst' / '.uit-setup').exists():
        p.append('the unfinished-install mark was left')
    args = d / 'inst' / 'logs' / 'stub-setup-args.txt'
    if not args.is_file() or '--dump ' + str(d / 'game') not in args.read_text():
        p.append('the build was not given the game files')
    return p


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('launcher')
    ap.add_argument('--cxx', help='the compiler for tools/tests/test_unzip.cpp (as the launcher build)')
    ap.add_argument('--sdl', default=str(ROOT.parent / 'ltools' / 'sdlinst'), help='SDL3 prefix for test_unzip')
    ap.add_argument('--shots', help='save screenshots of the pages here')
    ap.add_argument('-k', help='only the cases whose name has this')
    a = ap.parse_args()
    tmp = pathlib.Path(tempfile.mkdtemp(prefix='uit-setup-test-'))
    bad = total = 0
    if not a.k:
        for name, (p, what) in (('the game-file hashes match tools/setup.py', (check_hashes(), '')),
                                ('zip reading and SHA-256 against Python', check_unzip(a.cxx, a.sdl, tmp)),
                                ('the updater check reads release notes in any text', check_updater_notes())):
            total += 1
            print(('ok    ' if not p else 'FAIL  ') + name + (f' ({what})' if what else ''))
            for x in p:
                print('        ' + x)
            bad += bool(p)
    shots = pathlib.Path(a.shots).resolve() if a.shots else None
    if shots:
        shots.mkdir(parents=True, exist_ok=True)
    R = Run(os.path.abspath(a.launcher), tmp, shots)
    to_game = 'inext;ifolder:{D}/inst;inext;ipage:game'
    cases = [
        ('welcome', 'ipage:welcome;ihas:Welcome;ihas:About 32 GB of free space;ifocus:Get started;shot:01-welcome.png;left;ifocus:Quit;right;accept;ipage:folder', {}),
        ('folder: default, Back', 'inext;ipage:folder;ihas:Where should it go;ihas:100 GB free;ifocus:Next;shot:02-folder.png;back;ipage:welcome', {}),
        ('folder: spaces refused', 'inext;ifolder:{D}/my games;ihas:no spaces;inext;ipage:folder;shot:02b-folder-spaces.png', {}),
        ('folder: not enough space', 'inext;ifolder:{D}/inst;ihas:enough free space;ihas:needs about 32 GB;inext;ipage:folder;shot:02c-folder-space.png', {'free_gb': '10'}),
        ('folder: a folder with files gets its own', 'inext;ifolder:{D}/busy;ihas:busy/UltraInstinctTears', {
            'prep': lambda d: ((d / 'busy').mkdir(), (d / 'busy' / 'x.txt').write_text('x'))}),
        ('folder: the game files folder refused', 'inext;ifolder:{D}/inst;inext;igame:{GOOD};iback;ifolder:{GOOD};ihas:This is where your game files are;inext;ipage:folder', {}),
        ('game: nothing chosen', to_game + ';ihas:Where are your game files;inext;ipage:game;shot:03-game.png', {}),
        ('game: fine', to_game + ';igame:{GOOD};ihas:Tears of the Kingdom 1.4.3 found;ifocus:Next;shot:03b-game-ok.png;inext;ipage:ready;ihas:Ready to install;shot:04-ready.png', {}),
        ('game: exefs chosen', to_game + ';igame:{GOOD}/exefs;ihas:1.4.3 found', {}),
        ('game: one folder up', to_game + ';igame:{D}/dump;ihas:1.4.3 found;ihas:dump/g', {'prep': lambda d: make_game(d / 'dump' / 'g')}),
        ('game: one folder up holds the install folder', to_game + ';igame:{D};ihas:inside the install folder', {}),
        ('game: empty folder', to_game + ';igame:{D}/empty;ihas:no game files in this folder;inext;ipage:game;shot:03c-game-empty.png', {
            'prep': lambda d: (d / 'empty').mkdir()}),
        ('game: packed file', to_game + ';igame:{D}/packed;ihas:packed game file (game.nsp);shot:03d-game-packed.png', {
            'prep': lambda d: make_game(d / 'packed', packed=True)}),
        ('game: a code file missing', to_game + ';igame:{D}/g2;ihas:missing a file named subsdk0', {
            'prep': lambda d: make_game(d / 'g2', missing='subsdk0')}),
        ('game: another version', to_game + ';igame:{D}/g3;ihas:different version;ihas:update 1.4.3;shot:03e-game-version.png', {
            'prep': lambda d: make_game(d / 'g3', wrong='main')}),
        ('game: no romfs', to_game + ';igame:{D}/g4;ihas:romfs folder is missing', {
            'prep': lambda d: make_game(d / 'g4', romfs=False)}),
        ('game: inside the install folder', 'inext;ifolder:{D}/inst;ihas:inst/UltraInstinctTears;inext;igame:{D}/inst/UltraInstinctTears/g;'
         'ihas:inside the install folder', {'prep': lambda d: make_game(d / 'inst' / 'UltraInstinctTears' / 'g')}),
        ('install: the whole way, Play', to_game + ';igame:{GOOD};inext;ipage:ready;inext;ipage:work;isleep:1;shot:05-installing.png;'
         'iwait:120;ipage:done;ihas:is installed;ifocus:Play;shot:06-done.png;ipress:Play;istarted:play', {
            'env_extra': {'UIT_STUB_SLOW': '3'}, 'check': check_full}),
        ('install: no desktop shortcut, Open launcher', to_game + ';igame:{GOOD};inext;inext;iwait:120;ipage:done;'
         'ipress:Add a shortcut to the desktop;ipress:Open launcher;istarted:launcher', {
            'check': lambda d: (['a desktop shortcut was made'] if (d / 'desktop' / 'Ultra Instinct Tears.lnk').exists() else [])
            + missing(d, 'start/Ultra Instinct Tears.lnk')}),
        ('install: the build fails, Try again works', to_game + ';igame:{GOOD};inext;inext;iwait:120;ipage:failed;ihas:couldn\'t be installed;'
         'ihas:Try again picks up;ifocus:Try again;shot:07-failed.png;iexists:{D}/inst/tools/update.py', {
            'env_extra': {'UIT_STUB_FAIL': 'compile'}}),
        ('install: the build fails, Close removes everything', 'inext;ifolder:{D}/new/inst;inext;igame:{GOOD}' + ';inext;inext;iwait:120;ipage:failed;ipress:Close;iwait:30', {
            'env_extra': {'UIT_STUB_FAIL': 'lift'},
            'check': lambda d: ([f'{d / "new"} was left behind'] if (d / 'new').exists() else [])
            + ([] if (d / 'pref' / 'last-install.log').exists() else ['no copy of the log was kept'])}),
        ('install: Stop and remove', to_game + ';igame:{GOOD};inext;inext;isleep:2;icancel;shot:08-stop.png;ifocus:Keep installing;'
         'left;accept;iwait:60;ipage:stopped;ihas:Installation stopped;ihas:Nothing was left;shot:09-stopped.png;igone:{D}/inst', {
            'env_extra': {'UIT_STUB_SLOW': '30'}}),
        ('install: Keep installing', to_game + ';igame:{GOOD};inext;inext;isleep:0.5;icancel;back;ipage:work;iwait:120;ipage:done', {
            'env_extra': {'UIT_STUB_SLOW': '2'}}),
        ('install: an unfinished one picked up', 'inext;ifolder:{D}/inst;ihas:unfinished install;inext;igame:{GOOD};inext;inext;iwait:120;ipage:done', {
            'prep': prep_unfinished}),
        ('install: no build tools, offline', to_game + ';igame:{GOOD};inext;inext;iwait:60;ipage:failed;ihas:internet connection;shot:07b-failed-offline.png', {
            'env_extra': {'UIT_SETUP_TOOLS': '/nonexistent'}}),
        ('update over an install, saves kept', 'ipage:welcome;ihas:Welcome back;ifocus:Update;shot:10-welcome-back.png;ipress:Update;ipage:ready;'
         'ihas:Ready to update;inext;iwait:120;ipage:done;ihas:up to date', {
            'prep': prep_installed,
            'check': lambda d: missing(d, 'old/userdata/save/0001/progress.sav', 'old/ultrainstincttears.cfg')
            + ([] if 'commit abc1234' in (d / 'old' / 'VERSION.txt').read_text() else ['VERSION.txt was not updated'])}),
        ('an install chosen as the folder: update', 'ipress:Install somewhere else;ifolder:{D}/old;ihas:already installed here;inext;ipage:ready', {
            'prep': prep_installed}),
    ]
    # [UIT 0435] the newest release, online. Embedded package: 1.0.4 (abc1234). A fake GitHub per case.
    fakes = []

    def gh(*releases, **kw):
        g = FakeGitHub(list(releases), **kw)
        fakes.append(g)
        return g

    full = to_game + ';igame:{GOOD};inext;inext;iwait:120;ipage:done'
    old_pkg = tmp / 'payload-1.0.2.zip'
    make_payload(old_pkg, '1.0.2', '1111111')
    g_new = gh(release('v1.0.5'))
    g_old = gh(release('v1.0.3'))
    g_same = gh(release('v1.0.4', commit='9999999'))
    g_pre = gh(release('v1.0.5-rc1', pre=True), release('v1.0.3'))
    g_stable = gh(release('v1.0.4-1-gaaaaaaa', pre=True), release('v1.0.3', readme='latest\n'))
    g_wrong = gh(release('v1.0.5', sha='wrong'))
    g_none = gh(release('v1.0.5', sha='none'))
    g_digest = gh(release('v1.0.5', sha='digest'))
    g_utf = gh(release('v1.0.5', sha='notes', body='## Neu \u2014 caf\u00e9 \u65e5\u672c\u8a9e \u2713 \U0001f600 \u201cquotes\u201d\nline two \xa0 end'))
    g_utf_ascii = gh(release('v1.0.5', sha='notes', body='## caf\u00e9 \u65e5\u672c\u8a9e \U0001f600 \u201cquotes\u201d'), ascii_json=True)
    g_tag = gh(release('v1.0.5', zip_says='1.0.9'))
    g_403 = gh(release('v1.0.5'), status=403)
    g_draft = gh(release('v1.0.9', draft=True), release('v1.0.5', pre=True), release('v1.0.3'))
    g_slow = gh(release('v1.0.5', pad=6 << 20), slow=40.0)
    g_slow_ok = gh(release('v1.0.5', pad=1 << 20), slow=3.0)
    g_two = gh(release('v1.0.6'), release('v1.0.5'))
    cases += [
        ('newest: a newer release is installed instead', full, {
            'env_extra': g_new.env(), 'check': lambda d: readme_is(d, 'newest\n') + version_is(d, '1.0.5') + log_has(d, 'installing the newest version 1.0.5')
            + log_has(d, 'matches UltraInstinctTears-src.zip.sha256')
            + ([] if not list((d / 'inst' / 'dev-local' / 'downloads').glob('*src-newest*')) else ['the downloaded package was left behind'])
            + ([] if g_new.hits[:1] == ['/api/releases/latest'] else [f'asked {g_new.hits}'])}),
        ('newest: the build gets the newer files and the game files', full + ';ipress:Play;istarted:play', {
            'env_extra': g_new.env(), 'check': lambda d: check_full(d)}),
        ('newest: an older release is ignored', full, {
            'env_extra': g_old.env(), 'check': lambda d: readme_is(d, 'stand-in\n') + log_has(d, 'is already the newest (1.0.3)')}),
        ('newest: the same version is ignored', full, {
            'env_extra': g_same.env(), 'check': lambda d: readme_is(d, 'stand-in\n') + log_has(d, 'is already the newest (1.0.4)')}),
        ('newest: a test installer follows pre-releases (newer than Latest)', full, {
            'env_extra': g_pre.env(), 'check': lambda d: readme_is(d, 'newest\n') + version_is(d, '1.0.5-rc1') + log_has(d, 'looking at pre-releases too')}),
        ('newest: a release installer never follows pre-releases', full, {
            'payload': old_pkg, 'env_extra': g_stable.env(), 'check': lambda d: readme_is(d, 'latest\n') + version_is(d, '1.0.3')
            + ([] if not any('releases?' in h for h in g_stable.hits) else [f'the release list was asked for: {g_stable.hits}'])}),
        ('newest: drafts are skipped, the newest of the rest wins', full, {
            'env_extra': g_draft.env(), 'check': lambda d: readme_is(d, 'newest\n') + version_is(d, '1.0.5')}),
        ('newest: the highest version wins even when listed second', full, {
            'env_extra': g_two.env(), 'check': lambda d: version_is(d, '1.0.6')}),
        ('newest: a wrong SHA-256 falls back to the embedded package', full, {
            'env_extra': g_wrong.env(), 'check': lambda d: readme_is(d, 'stand-in\n') + version_is(d, '1.0.4') + log_has(d, 'is not the one in UltraInstinctTears-src.zip.sha256')
            + ([] if not list((d / 'inst' / 'dev-local' / 'downloads').glob('*newest*')) else ['the rejected download was left behind'])}),
        ('newest: no published SHA-256 falls back', full, {
            'env_extra': g_none.env(), 'check': lambda d: readme_is(d, 'stand-in\n') + log_has(d, 'publishes no SHA-256')}),
        ('newest: GitHub\'s asset digest is enough', full, {
            'env_extra': g_digest.env(), 'check': lambda d: readme_is(d, 'newest\n') + log_has(d, "matches GitHub's asset digest")}),
        ('newest: notes with non-ASCII text and the checksum in them', full, {
            'env_extra': g_utf.env(), 'check': lambda d: readme_is(d, 'newest\n') + log_has(d, 'matches the release notes')}),
        ('newest: notes with \\u escapes and surrogate pairs', full, {
            'env_extra': g_utf_ascii.env(), 'check': lambda d: readme_is(d, 'newest\n') + log_has(d, 'matches the release notes')}),
        ('newest: a package that is not the tag\'s version falls back', full, {
            'env_extra': g_tag.env(), 'check': lambda d: readme_is(d, 'stand-in\n') + log_has(d, 'but its package says version "1.0.9"')}),
        ('newest: GitHub\'s rate limit (403) falls back', full, {
            'env_extra': g_403.env(), 'check': lambda d: readme_is(d, 'stand-in\n') + log_has(d, 'using the installer\'s own copy: GitHub did not answer')}),
        ('newest: the server is down, falls back', full, {
            'check': lambda d: readme_is(d, 'stand-in\n') + log_has(d, 'using the installer\'s own copy: GitHub did not answer') + version_is(d, '1.0.4')}),
        ('newest: the download step', to_game + ';igame:{GOOD};inext;inext;ipage:work;isleep:1.2;busyhas:Getting the newest version;busyhas:MB;shot:05c-getting-newest.png;'
         'iwait:120;ipage:done', {
            'env_extra': g_slow_ok.env(), 'check': lambda d: readme_is(d, 'newest\n')}),
        ('newest: Stop during the download ends it at once and removes everything', to_game + ';igame:{GOOD};inext;inext;ipage:work;isleep:1.5;icancel;'
         'ifocus:Keep installing;left;accept;iwait:60;ipage:stopped;ihas:Nothing was left;igone:{D}/inst', {
            'env_extra': g_slow.env(), 'check': lambda d: ([] if R.last_secs < 20 else [f'it took {R.last_secs:.0f} s: the download was not cut']) + (
                [] if g_slow.cut else ['the server never saw the connection closed'])}),
        ('newest: Keep installing after the question finishes the download', to_game + ';igame:{GOOD};inext;inext;isleep:0.5;icancel;back;ipage:work;iwait:120;ipage:done', {
            'env_extra': g_slow_ok.env(), 'check': lambda d: readme_is(d, 'newest\n')}),
        ('newest: an update over an install uses the newest package, saves kept', 'ipage:welcome;ipress:Update;ipage:ready;inext;iwait:120;ipage:done;ihas:up to date', {
            'prep': prep_installed, 'env_extra': g_new.env(), 'check': lambda d: missing(d, 'old/userdata/save/0001/progress.sav', 'old/ultrainstincttears.cfg')
            + readme_is(d, 'newest\n', 'old') + version_is(d, '1.0.5', 'old') + ([] if not list((d / 'old' / 'updates').glob('*.zip')) else ['a package was left in updates/'])}),
        ('newest: an update over an install, server down, uses the embedded package', 'ipage:welcome;ipress:Update;ipage:ready;inext;iwait:120;ipage:done', {
            'prep': prep_installed, 'check': lambda d: readme_is(d, 'stand-in\n', 'old') + version_is(d, '1.0.4', 'old')}),
    ]
    for name, script, kw in cases:
        if a.k and a.k not in name:
            continue
        total += 1
        bad += not R.case(name, script, **kw)
    for g in fakes:
        g.srv.shutdown()
    print(f'{bad} of {total} installer checks failed' if bad else f'all {total} installer checks passed')
    if not bad:
        shutil.rmtree(tmp, ignore_errors=True)
    else:
        print(f'(left for a look: {tmp})')
    sys.exit(1 if bad else 0)


if __name__ == '__main__':
    main()
