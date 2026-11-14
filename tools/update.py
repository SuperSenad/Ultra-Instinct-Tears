"""Install a newer Ultra Instinct Tears source package over this folder, rebuild, and roll back if anything fails.

    py -3 tools/update.py scan [--folder F ...] [--json]         list the packages found, newest first
    py -3 tools/update.py install PACKAGE.zip [--progress FILE] [--force] [--no-build]
    py -3 tools/update.py rollback [BACKUP-FOLDER]                 undo the last update (or the one named)

    py -3 tools/update.py scan --tsv --online                      also ask GitHub for the newest release (UIT 0280)
    py -3 tools/update.py download --tag vX.Y.Z [--progress FILE]  download and check that release's zip; prints its path

Where packages come from is pluggable (Source below). LocalZipSource: zip files named UltraInstinctTears-src*.zip in the
folders given (--folder, or TOTK_LAUNCHER_UPDATE_DIRS in ultrainstincttears.cfg, ";"-separated; nothing is assumed when
neither is set except this folder and the folder above it). [UIT 0280] GitHubReleasesSource: the repository's GitHub
Releases (only when asked: scan --online / --source github, download). A downloaded zip is installed by exactly the same
install below as a local one; the local zip stays the fallback whenever GitHub cannot be reached.

What install does, in order (each step is written to --progress as one JSON object, for the launcher's progress screen):
  check     the zip is an Ultra Instinct Tears source package (tools/setup.py, src/, VERSION.txt at its top folder),
            no entry escapes the folder; its version against this folder's VERSION.txt (same or older needs --force)
  backup    updates/backup-<time>/: ultrainstincttears.cfg, every file the package replaces, both .exe files, and the
            list of files it adds (so they can be removed again)
  extract   the package's files over this folder. Never touched: data/, userdata/, saves/, game/, build/, deps/, logs/,
            updates/, ultrainstincttears.cfg and anything else the package does not contain
  build     python tools/setup.py (resumes: only what changed is rebuilt; setup decides itself whether the game's code
            has to be translated again)
On a failure in extract or build every replaced file is put back, the added ones removed and the old .exe files
restored: the folder plays as before. A Git checkout is refused (use git there).

[UIT 0046] Interrupted updates and newer updaters:
  - VERSION.txt is written last, after the build: a folder only says it has the new version once it does.
  - updates/pending.json names the backup of an update that has not finished (the process was killed, the PC turned
    off, or the rollback itself failed). The next install of the same package resumes it (same backup, so a rollback
    still returns to the version before it); another package first rolls the unfinished one back.
  - When the package's tools/update.py is a different one that supports it, install runs that one instead (from a
    temporary copy, UIT_UPDATE_ROOT=this folder): fixes to the updater apply to the update that brings them.
  - The progress file never fails an update (tools/progress_file.py).
  - [UIT 0055] Child processes (the build, a handed-over updater) get stdin from NUL and stdout/stderr through a
    pipe into the log and the progress, with no console window: started from the launcher, this script's own std
    handles may be invalid ("[WinError 6] The handle is invalid").
"""
import argparse, datetime, hashlib, json, os, pathlib, re, shutil, subprocess, sys, time, zipfile

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
try:
    from progress_file import ProgressFile  # noqa: E402  [UIT 0046] progress writes never fail the task
except ImportError:   # a copy of this script without its neighbour: progress best-effort, never fatal
    class ProgressFile:
        def __init__(self, path, note=None):
            self.path = pathlib.Path(path) if path else None

        def write(self, state):
            if self.path:
                try:
                    tmp = self.path.with_name(self.path.name + '.tmp'); tmp.write_text(json.dumps(state)); os.replace(tmp, self.path)
                except OSError:
                    pass

ROOT = pathlib.Path(os.environ.get('UIT_UPDATE_ROOT') or pathlib.Path(__file__).resolve().parent.parent)   # [UIT 0046] set by a handoff
PATTERN = re.compile(r'^UltraInstinctTears-src.*\.zip$', re.I)
PROTECTED = ('data/', 'userdata/', 'saves/', 'game/', 'build/', 'deps/', 'logs/', 'updates/', 'dist/', '.git/')
PROTECTED_FILES = {'ultrainstincttears.cfg', 'totk.cfg'}
EXES = ('UltraInstinctTears.exe', 'UltraInstinctTearsLauncher.exe')
KEEP_BACKUPS = 3
MAX_ENTRY = 512 << 20        # no source file is anywhere near this: a bigger entry is not ours


class Fail(Exception):
    pass


# ---------------------------------------------------------------- child processes  [UIT 0055]
# The launcher starts this script with no console and stdin set to an invalid handle. subprocess on Windows duplicates
# every std handle it is not given (GetStdHandle + DuplicateHandle), so Popen(stdout=PIPE) with stdin left alone
# failed the build with "[WinError 6] The handle is invalid". Children get all three handles explicitly: stdin from
# NUL, stdout and stderr through a pipe read here (into the log and the progress), and no console window.
NO_WINDOW = getattr(subprocess, 'CREATE_NO_WINDOW', 0)


def spawn(cmd, **kw):
    env = dict(kw.pop('env', None) or os.environ, PYTHONIOENCODING='utf-8:replace', PYTHONUNBUFFERED='1')
    return subprocess.Popen(cmd, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            encoding='utf-8', errors='replace', creationflags=NO_WINDOW, env=env, **kw)


def run_lines(cmd, on_line, **kw):
    """Runs cmd (see spawn), calls on_line(line) for each line it prints, returns its exit code. Never leaves it
    running behind an exception (a rollback must not race a build writing into the restored files)."""
    p = spawn(cmd, **kw)
    try:
        for line in p.stdout:
            on_line(line)
        return p.wait()
    except BaseException:
        p.kill(); p.wait()
        raise
    finally:
        p.stdout.close()


def std_streams():
    """[UIT 0055] This process may have no console: sys.stdin/stdout/stderr None (pythonw, closed handles) or OS std
    handles that are invalid. Points the missing ones at NUL so print(), input() and any subprocess call work."""
    for name, mode in (('stdin', 'r'), ('stdout', 'w'), ('stderr', 'w')):
        if getattr(sys, name) is None:
            try:
                setattr(sys, name, open(os.devnull, mode, encoding='utf-8'))
            except OSError:
                pass
    if os.name != 'nt':
        return
    try:   # best effort: Windows only, never fatal
        import ctypes, msvcrt
        k = ctypes.WinDLL('kernel32', use_last_error=True)
        k.GetStdHandle.restype, k.GetStdHandle.argtypes = ctypes.c_void_p, [ctypes.c_uint32]
        k.SetStdHandle.argtypes = [ctypes.c_uint32, ctypes.c_void_p]
        k.GetFileType.restype, k.GetFileType.argtypes = ctypes.c_uint32, [ctypes.c_void_p]
        for which in (-10, -11, -12):   # STD_INPUT_HANDLE, STD_OUTPUT_HANDLE, STD_ERROR_HANDLE
            n = which & 0xFFFFFFFF
            h = k.GetStdHandle(n)
            bad = h in (None, 0, ctypes.c_void_p(-1).value)
            if not bad:
                ctypes.set_last_error(0)
                bad = k.GetFileType(h) == 0 and ctypes.get_last_error() != 0
            if bad:
                f = open(os.devnull, 'r' if which == -10 else 'w')
                _NUL.append(f)   # open for the life of the process
                k.SetStdHandle(n, msvcrt.get_osfhandle(f.fileno()))
    except Exception:   # noqa: BLE001
        pass


_NUL = []


# ---------------------------------------------------------------- sources (pluggable)
class Candidate:
    def __init__(self, source, ref, label, version, commit, when, size):
        self.source, self.ref, self.label, self.version, self.commit, self.when, self.size = \
            source, ref, label, version, commit, when, size

    def as_dict(self):
        return {'source': self.source, 'ref': str(self.ref), 'label': self.label, 'version': self.version,
                'commit': self.commit, 'when': self.when, 'size': self.size}


class Source:
    """A place packages come from. candidates() lists them (cheap, newest first is not required); fetch() returns a
    local zip path for one (a local source returns the file itself; an online one would download into dest, check
    its published SHA-256 and return the copy). Installing only ever reads that local zip."""
    name = '?'

    def candidates(self):
        raise NotImplementedError

    def fetch(self, cand, dest):
        raise NotImplementedError


class LocalZipSource(Source):
    name = 'local'

    def __init__(self, folders):
        self.folders = [pathlib.Path(f) for f in folders]

    def candidates(self):
        out, seen = [], set()
        for d in self.folders:
            try:
                files = [p for p in d.iterdir() if p.is_file() and PATTERN.match(p.name)]
            except OSError:
                continue
            for p in files:
                key = os.path.normcase(str(p.resolve()))
                if key in seen:
                    continue
                seen.add(key)
                try:
                    info = read_package(p)
                except Fail:
                    continue
                out.append(Candidate(self.name, p, p.name, info['version'], info['commit'], info['when'], p.stat().st_size))
        return out

    def fetch(self, cand, dest):
        return pathlib.Path(cand.ref)


class GitHubReleasesSource(Source):
    """[UIT 0280] A repository's GitHub Releases: each release is tagged vX.Y.Z and carries the source package
    UltraInstinctTears-src.zip (tools/make_source_zip.py) and, next to it, UltraInstinctTears-src.zip.sha256.

    Only runs when the player presses About > Install update (the launcher refuses while the game runs) or, [UIT 0320],
    as the launcher's background start-up check (update.py check), in this helper process: the launcher never blocks
    on the network. Every request has a timeout; 404 (no public release yet: the
    repository is private until the updater is proven), the hourly rate limit, a bad token and no connection all end in
    a Fail with a sentence for the player, never a traceback.
      - repository: TOTK_LAUNCHER_UPDATE_REPO (owner/name), default REPO;
      - pre-releases: TOTK_LAUNCHER_UPDATE_PRERELEASE=1 also offers releases marked pre-release (a test release);
      - token (only for testing while the repository is private; never logged, never written anywhere): the environment
        variable UIT_GITHUB_TOKEN, else the first line of updates/github-token.txt in the game folder (updates/ is never in a
        package and never in a problem report);
      - UIT_UPDATE_API replaces https://api.github.com, UIT_UPDATE_TIMEOUT shortens the timeout (box tests only).
    [UIT 0284] HTTPS goes through Python's own ssl (deps\\python is a full python.org copy: _ssl.pyd, libssl-3.dll,
    libcrypto-3.dll; it trusts the roots in Windows' certificate store). Windows only adds many roots to that store the
    first time Windows itself needs one, so on a fresh PC Python can fail where a browser works (setup.py met this in
    0140 with repo.msys2.org). On a certificate error, or a Python without ssl (TOTK_LAUNCHER_PYTHON), every request
    goes through Windows' own %SystemRoot%\\System32\\curl.exe instead (Windows 10 1803 and later; Schannel, Windows'
    root store with its on-demand root download), with the same checks: certificates always verified, https only,
    redirects followed here (the token only ever to the API host), the token on curl's stdin, never its command line.
      - UIT_UPDATE_HTTP=curl / python forces one way; UIT_UPDATE_CURL names another curl (box tests only).
    fetch() downloads into updates/downloads/, checks the size and every SHA-256 the release publishes (the .sha256
    asset, GitHub's own asset digest, a "sha256" line in the notes), checks that it is a source package of the tag's
    version, and only then names the zip: tools/update.py install takes it from there exactly like a zip from chat."""
    name = 'github'
    REPO = 'SuperSenad/Ultra-Instinct-Tears'
    ASSET = 'UltraInstinctTears-src.zip'
    TIMEOUT = 15          # seconds per connect / read: a dead connection fails, it never hangs
    MAX_NOTES = 1200      # characters of release notes in the launcher's dialog

    def __init__(self, repo=None, prerelease=None, token=None, api=None, timeout=None):
        self.repo = (repo or cfg_value('TOTK_LAUNCHER_UPDATE_REPO') or self.REPO).strip().strip('/')
        if not re.fullmatch(r'[\w.-]+/[\w.-]+', self.repo):
            raise Fail(f'"{self.repo}" is not a GitHub repository (owner/name): see Advanced > Updates and reports')
        self.prerelease = (cfg_value('TOTK_LAUNCHER_UPDATE_PRERELEASE') == '1') if prerelease is None else prerelease
        self.token = token if token is not None else github_token()
        self.api = (api or os.environ.get('UIT_UPDATE_API') or 'https://api.github.com').rstrip('/')
        try:   # the box tests' stalled server; never longer than TIMEOUT
            self.timeout = min(self.TIMEOUT, max(1.0, float(os.environ.get('UIT_UPDATE_TIMEOUT', self.TIMEOUT))))
        except ValueError:
            self.timeout = self.TIMEOUT
        if timeout:   # [UIT 0320] the launcher's start-up check: shorter still
            self.timeout = min(self.timeout, float(timeout))
        self.http = os.environ.get('UIT_UPDATE_HTTP', '').strip().lower()   # '', 'curl', 'python'
        self.curl = find_curl()
        self.notes = []   # how it connected, for the download log (never the token)
        self.use_curl = self.http == 'curl'
        if self.use_curl and not self.curl:
            raise Fail('UIT_UPDATE_HTTP=curl, but there is no curl.exe')
        if not self.use_curl and self.http != 'python':
            try:
                import ssl  # noqa: F401
            except ImportError:   # a Python built without ssl: https needs Windows' curl.exe
                if self.curl:
                    self.use_curl = True
                    self.notes.append('this Python has no ssl module: using Windows\' curl.exe for HTTPS')

    # ---- HTTP
    def _request(self, url, accept='application/vnd.github+json', auth=True):
        import urllib.request
        req = urllib.request.Request(url, headers={'Accept': accept, 'User-Agent': 'UltraInstinctTears-updater',
                                                   'X-GitHub-Api-Version': '2022-11-28'})
        if auth and self.token:   # unredirected: never sent on to the storage host a download redirects to
            req.add_unredirected_header('Authorization', 'Bearer ' + self.token)
        return req

    def _open(self, url, accept='application/vnd.github+json', auth=True):
        if self.use_curl:
            return self._curl_open(url, accept, auth)
        import urllib.request, urllib.error, socket
        try:
            import ssl
            ssl_errors = (ssl.SSLError,)
        except ImportError:
            ssl_errors = ()
        try:
            return urllib.request.urlopen(self._request(url, accept, auth), timeout=self.timeout, context=tls_context())
        except urllib.error.HTTPError as e:
            raise Fail(self._http_error(e))
        except (urllib.error.URLError, socket.timeout, TimeoutError, ConnectionError, OSError) + ssl_errors as e:
            why = getattr(e, 'reason', e)
            if self.curl and self.http != 'python' and (is_cert_error(e) or 'unknown url type' in str(why)):
                # [UIT 0284] Python could not verify GitHub's certificate with the roots it sees (or has no ssl):
                # Windows' curl.exe checks it with Windows' own, fetching a missing root as a browser would
                self.use_curl = True
                self.notes.append(f'Python could not verify the connection ({why}): using Windows\' curl.exe instead')
                return self._curl_open(url, accept, auth)
            if 'unknown url type' in str(why):
                why = 'this Python has no ssl module and there is no curl.exe'
            hint = (' Check the PC\'s date and time, then run Windows Update (it updates the trusted certificates)'
                    if is_cert_error(e) else ' Check the internet connection, or install from a zip instead')
            raise Fail(f'could not reach GitHub ({why}).' + hint)

    def _curl_open(self, url, accept, auth, hops=0):
        """[UIT 0284] One GET through curl: a file-like answer (read, close, context manager) or a Fail, like _open.
        No --location: a redirect is followed here, without the token (it only ever goes to the API host, never on)."""
        import urllib.parse
        local = urllib.parse.urlsplit(url).hostname in ('127.0.0.1', 'localhost')   # the box tests' http mock
        cmd = [str(self.curl), '--silent', '--show-error', '--globoff', '--proto', '=http,https' if local else '=https',
               '--connect-timeout', str(int(self.timeout)), '--speed-limit', '1', '--speed-time', str(int(self.timeout)),
               '--user-agent', 'UltraInstinctTears-updater', '--header', 'Accept: ' + accept,
               '--header', 'X-GitHub-Api-Version: 2022-11-28', '--dump-header', '-', '--config', '-', url]
        send_token = auth and self.token and hops == 0 and same_host(url, self.api)   # like urllib's unredirected header
        conf = ('header = "Authorization: Bearer %s"\n' % self.token) if send_token else ''
        try:
            p = subprocess.Popen(cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                 creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
        except OSError as e:
            raise Fail(f'could not start curl.exe ({e}). Install from a zip instead')
        try:
            p.stdin.write(conf.encode('ascii')); p.stdin.close()
        except OSError:
            pass
        r = _CurlAnswer(p)
        try:
            r.read_head()
        except OSError as e:
            r.close()
            hint = (' Check the PC\'s date and time, then run Windows Update (it updates the trusted certificates)'
                    if r.p.returncode in (35, 51, 58, 60, 77, 83, 90, 91) else
                    ' Check the internet connection, or install from a zip instead')
            raise Fail(f'could not reach GitHub (curl.exe: {e}).' + hint)
        if r.code in (301, 302, 303, 307, 308) and r.headers.get('Location'):
            r.close()
            if hops >= 5:
                raise Fail('GitHub redirected the download too often. Try again later, or install from a zip instead')
            return self._curl_open(urllib.parse.urljoin(url, r.headers['Location']), accept, auth, hops + 1)
        if r.code >= 400:
            r.close()
            raise Fail(self._http_error(r))
        return r

    def _http_error(self, e):
        h = e.headers or {}
        where = f'github.com/{self.repo}'
        if e.code in (403, 429) and (h.get('X-RateLimit-Remaining') == '0' or e.code == 429):
            reset = h.get('X-RateLimit-Reset', '')
            when = datetime.datetime.fromtimestamp(int(reset)).strftime('%H:%M') if reset.isdigit() else 'in an hour'
            return f'GitHub\'s limit of update checks for this connection is used up for now; try again after {when}'
        if e.code == 401:
            return 'GitHub refused the access token (UIT_GITHUB_TOKEN or updates\\github-token.txt): it is wrong or expired'
        if e.code == 404:
            if self.token:
                return f'{where} has no such release, or the token cannot read that repository'
            return (f'no published release found at {where}: the repository may still be private or have no release yet. '
                    'Install from a zip instead')
        return f'GitHub answered {e.code} {e.reason} for {where}'

    def _json(self, path):
        with self._open(self.api + path) as r:
            try:
                return json.loads(r.read(4 << 20).decode('utf-8'))
            except (ValueError, UnicodeDecodeError):
                raise Fail('GitHub sent an answer that is not JSON (a captive portal or proxy?)')

    # ---- releases
    def release(self, tag=None):
        """The release to offer (or the one tagged tag): its dict from the API. Drafts are never offered."""
        if tag:
            return self._json(f'/repos/{self.repo}/releases/tags/{tag}')
        if not self.prerelease:
            return self._json(f'/repos/{self.repo}/releases/latest')
        rels = self._json(f'/repos/{self.repo}/releases?per_page=30')
        rels = [r for r in rels if isinstance(r, dict) and not r.get('draft') and version_key(tag_version(r.get('tag_name')))]
        if not rels:
            raise Fail(f'github.com/{self.repo} has no release tagged vX.Y.Z yet. Install from a zip instead')
        return max(rels, key=lambda r: version_key(tag_version(r['tag_name'])))

    def candidates(self):
        rel = self.release()
        return [self.candidate(rel)]

    def candidate(self, rel):
        tag = rel.get('tag_name') or ''
        v = tag_version(tag)
        if not version_key(v):
            raise Fail(f'the newest release is tagged "{tag}", not vX.Y.Z')
        asset = pick_asset(rel.get('assets') or [])
        c = Candidate(self.name, tag, asset['name'], v, '', (rel.get('published_at') or '')[:16].replace('T', ' '), asset.get('size') or 0)
        c.release, c.asset = rel, asset
        return c

    def expected_sha256(self, rel, asset):
        """Every SHA-256 the release publishes for asset: {where: hex}."""
        out = {}
        dg = asset.get('digest') or ''
        if dg.lower().startswith('sha256:') and re.fullmatch(r'[0-9a-fA-F]{64}', dg[7:]):
            out['GitHub\'s asset digest'] = dg[7:].lower()
        side = next((a for a in rel.get('assets') or [] if a.get('name', '').lower() in
                     (asset['name'].lower() + '.sha256', 'sha256sums', 'sha256sums.txt')), None)
        if side:
            text = self._download_small(side)
            h = sha_from_text(text, asset['name'])
            if not h:
                raise Fail(f'{side["name"]} in the release has no SHA-256 for {asset["name"]}')
            out[side['name']] = h
        h = sha_from_text(rel.get('body') or '', asset['name'], need_word=True)
        if h:
            out['the release notes'] = h
        return out

    def _asset_url(self, a):
        # with a token (a private repository) the API's asset URL; the public download URL needs none
        if self.token and a.get('url'):
            return a['url'], 'application/octet-stream'
        return a.get('browser_download_url') or a.get('url'), 'application/octet-stream'

    def _download_small(self, a):
        url, acc = self._asset_url(a)
        with self._open(url, acc) as r:
            return r.read(64 << 10).decode('utf-8', 'replace')

    def fetch(self, cand, dest, say=None):
        rel, asset = cand.release, cand.asset
        dest = pathlib.Path(dest)
        dest.mkdir(parents=True, exist_ok=True)
        size = int(asset.get('size') or 0)
        if size <= 0:
            raise Fail(f'{asset["name"]} in the release has no size')
        shas = self.expected_sha256(rel, asset)
        if len(set(shas.values())) > 1:
            raise Fail('the release publishes different SHA-256 values for ' + asset['name'] + ': ' + ', '.join(shas))
        out = dest / f'UltraInstinctTears-src-{cand.ref}.zip'
        part = out.with_name(out.name + '.part')
        url, acc = self._asset_url(asset)
        h, got, t0, last = hashlib.sha256(), 0, time.time(), 0.0
        import socket
        try:
            with self._open(url, acc) as r, open(part, 'wb') as f:
                while True:
                    try:
                        b = r.read(256 << 10)
                    except (socket.timeout, TimeoutError, OSError) as e:
                        raise Fail(f'the download stopped ({e}). Try again, or install from a zip instead')
                    if not b:
                        break
                    got += len(b)
                    if got > size:
                        raise Fail(f'the download is bigger than the {size} bytes the release lists')
                    h.update(b); f.write(b)
                    if say and time.time() - last > 0.25:   # the progress file only, not one log line per chunk
                        last = time.time()
                        say.state['msg'] = f'{got >> 20} of {max(1, size >> 20)} MB'
                        say(pct=got * 100 // size, done=got >> 20, total=max(1, size >> 20), unit='MB')
            if got != size:
                raise Fail(f'the download ended after {got} of {size} bytes. Try again')
            digest = h.hexdigest()
            for where, want in shas.items():
                if digest != want:
                    raise Fail(f'the download\'s SHA-256 {digest[:12]}... is not the one in {where} ({want[:12]}...): '
                               'not installed. Try again; if it repeats, tell the developer')
            info = read_package(part)
            if info['version'] and version_key(info['version']) != version_key(cand.version):
                raise Fail(f'the release is tagged {cand.ref} but its zip says version {info["version"]} '
                           '(made without the tag?): not installed. Tell the developer; install from a zip instead')
            os.replace(part, out)
        except BaseException:
            try:
                part.unlink()
            except OSError:
                pass
            raise
        for old in dest.glob('UltraInstinctTears-src-*.zip'):   # keep only this download
            if old != out:
                try:
                    old.unlink()
                except OSError:
                    pass
        return out, digest, sorted(shas), time.time() - t0


class _CurlAnswer:
    """[UIT 0284] curl's stdout (--dump-header - then the body) as a urllib-like answer: code, reason, headers, read().
    read() raises OSError when curl ends with an error (a cut connection, a stall over --speed-time)."""

    class Headers(dict):
        def get(self, k, d=None):
            return dict.get(self, k.lower(), d)

        def __getitem__(self, k):
            return dict.__getitem__(self, k.lower())

    def __init__(self, p):
        self.p, self.code, self.reason, self.headers = p, 0, '', self.Headers()

    def read_head(self):
        while True:   # 100 Continue / a proxy's "200 Connection established" come before the real answer
            line = self.p.stdout.readline()
            if not line:
                self.p.wait()
                raise OSError(self._why())
            m = re.match(rb'HTTP/[\d.]+ (\d{3}) ?(.*)', line.strip())
            if not m:
                continue
            code, reason, h = int(m.group(1)), m.group(2).decode('latin-1'), self.Headers()
            while True:
                hl = self.p.stdout.readline()
                if not hl.strip():
                    break
                k, _, v = hl.decode('latin-1').partition(':')
                h[k.strip().lower()] = v.strip()
            if code == 100 or (code == 200 and b'established' in line.lower()):
                continue
            self.code, self.reason, self.headers = code, reason or str(code), h
            return

    def _why(self):
        err = self.p.stderr.read().decode('utf-8', 'replace').strip().splitlines()
        main = [l for l in err if l.startswith('curl: (')]   # curl's own error line, not the advice after it
        return (main[0][6:] if main else err[0] if err else 'no answer') + f' (exit code {self.p.returncode})'

    def read(self, n=-1):
        b = self.p.stdout.read(n)
        if not b or (n > 0 and len(b) < n):
            if self.p.wait() != 0:
                raise OSError(self._why())
        return b

    def close(self):
        if self.p.poll() is None:
            self.p.kill()
        self.p.wait()
        for f in (self.p.stdout, self.p.stderr):
            f.close()

    def __enter__(self):
        return self

    def __exit__(self, *a):
        self.close()


def find_curl():
    """[UIT 0284] Windows' own %SystemRoot%\\System32\\curl.exe (Windows 10 1803 and later); UIT_UPDATE_CURL for the box tests."""
    c = os.environ.get('UIT_UPDATE_CURL')
    if c:
        return pathlib.Path(c) if pathlib.Path(c).is_file() else None
    root = os.environ.get('SystemRoot')
    curl = pathlib.Path(root) / 'System32' / 'curl.exe' if root else None
    return curl if os.name == 'nt' and curl and curl.is_file() else None


def tls_context():
    """[UIT 0284] As setup.py's: truststore (Windows checks the chain itself) when that Python has it, else the default
    context with the system's certificate stores. Verification is never turned off."""
    try:
        import ssl
    except ImportError:
        return None
    try:
        import truststore
        return truststore.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
    except Exception:
        pass
    ctx = ssl.create_default_context()
    ctx.load_default_certs()
    return ctx


def is_cert_error(e):
    try:
        import ssl
    except ImportError:
        return False
    while e is not None:
        if isinstance(e, ssl.SSLCertVerificationError) or (isinstance(e, ssl.SSLError) and 'CERTIFICATE' in str(e).upper()):
            return True
        e = getattr(e, 'reason', None) if not isinstance(e, ssl.SSLError) else None
    return False


def same_host(a, b):
    import urllib.parse
    x, y = urllib.parse.urlsplit(a), urllib.parse.urlsplit(b)
    return (x.scheme, x.hostname, x.port) == (y.scheme, y.hostname, y.port)


def github_token():
    """[UIT 0280] For testing while the repository is private: UIT_GITHUB_TOKEN, else updates/github-token.txt. Never logged."""
    t = os.environ.get('UIT_GITHUB_TOKEN', '').strip()
    if not t:
        try:
            t = (ROOT / 'updates' / 'github-token.txt').read_text(encoding='utf-8-sig').strip().splitlines()[0].strip()
        except (OSError, IndexError):
            t = ''
    return t if re.fullmatch(r'[\w-]{10,255}', t) else ''


def tag_version(tag):
    m = re.fullmatch(r'v(\d+\.\d+\.\d+(?:-[0-9A-Za-z.-]+)?)', (tag or '').strip())
    return m.group(1) if m else ''


def version_key(v):
    """[UIT 0280] A sortable key for a version, or None when it is not x.y.z...:
    1.0.0 (a release) > 1.0.0-rc1 / 1.0.0-fps / 1.0.0-dev (before it); 1.0.0-3-gabc1234 (git describe, 3 commits after
    the tag) > 1.0.0; a trailing -dirty is ignored."""
    m = re.match(r'^v?(\d+)\.(\d+)\.(\d+)(.*)$', (v or '').strip())
    if not m:
        return None
    base = tuple(int(x) for x in m.groups()[:3])
    rest = re.sub(r'-dirty$', '', m.group(4))
    if not rest:
        return base + (1, ())
    post = re.fullmatch(r'-(\d+)-g[0-9a-f]+', rest)
    if post:
        return base + (2, (int(post.group(1)),))
    parts = tuple((0, int(p), '') if p.isdigit() else (1, 0, p) for p in re.split(r'[.-]', rest.lstrip('-+')) if p)
    return base + (0, parts)


def is_newer(remote, local):
    """True when version remote is newer than local (an unknown local version: any release is newer)."""
    r, l = version_key(remote), version_key(local)
    return bool(r) and (l is None or r > l)


def pick_asset(assets):
    """The source package among a release's assets: UltraInstinctTears-src.zip, else the one UltraInstinctTears-src*.zip."""
    zips = [a for a in assets if isinstance(a, dict) and PATTERN.match(a.get('name', ''))]
    exact = [a for a in zips if a['name'].lower() == GitHubReleasesSource.ASSET.lower()]
    if exact:
        return exact[0]
    if len(zips) == 1:
        return zips[0]
    if not zips:
        raise Fail(f'the release has no {GitHubReleasesSource.ASSET} attached')
    raise Fail('the release has several UltraInstinctTears-src*.zip and none named ' + GitHubReleasesSource.ASSET)


def sha_from_text(text, name, need_word=False):
    """A SHA-256 for file name in text: "<hex>  name" (sha256sum), "name: <hex>", or (need_word False) a lone hex
    (a .sha256 file of one line). need_word: release notes, where a line must name the file or say sha256."""
    hexes = []
    for line in (text or '').splitlines():
        hs = re.findall(r'(?<![0-9a-fA-F])[0-9a-fA-F]{64}(?![0-9a-fA-F])', line)
        if len(hs) != 1:
            continue
        low = line.lower()
        if name.lower() in low:
            return hs[0].lower()
        if 'sha256' in low or 'sha-256' in low or not need_word:
            hexes.append(hs[0].lower())
    return hexes[0] if len(hexes) == 1 else ''


def notes_text(body, limit=GitHubReleasesSource.MAX_NOTES, max_lines=16):
    """The release notes as plain lines (Markdown headings, emphasis and links reduced to their text), short enough for
    the launcher's dialog: the rest is on the release page."""
    out = []
    for line in (body or '').replace('\r\n', '\n').split('\n'):
        s = line.rstrip()
        s = re.sub(r'^#{1,6}\s*', '', s)
        s = re.sub(r'!\[[^\]]*\]\([^)]*\)', '', s)
        s = re.sub(r'\[([^\]]+)\]\([^)]*\)', r'\1', s)
        s = re.sub(r'(\*\*|__|`)', '', s)
        s = re.sub(r'^\s*[-*+]\s+', '\u2022 ', s)
        if re.search(r'(?<![0-9a-fA-F])[0-9a-fA-F]{64}(?![0-9a-fA-F])', s):
            continue   # the checksum line: no use to the player
        if s or (out and out[-1]):
            out.append(s)
    lines = '\n'.join(out).strip().split('\n')
    text = '\n'.join(lines[:max_lines])
    if len(lines) > max_lines or len(text) > limit:
        text = text[:limit].rstrip() + '\n... (the full notes are on the release page)'
    return text


def tsv_escape(x):
    return str(x).replace('\\', '\\\\').replace('\t', '\\t').replace('\r', '').replace('\n', '\\n')


SOURCES = {'local': LocalZipSource, 'github': GitHubReleasesSource}


# ---------------------------------------------------------------- the package
def norm_version(text):
    m = re.search(r'Ultra Instinct Tears\s+(\S+)(?:\s+\(commit\s+([0-9a-f]+)\))?', text or '')
    return (m.group(1), m.group(2) or '') if m else ((text or '').strip()[:40], '')


def local_version():
    for n in ('VERSION.txt', 'VERSION'):
        p = ROOT / n
        if p.is_file():
            v, c = norm_version(p.read_text(errors='replace'))
            return {'version': v, 'commit': c, 'when': dt(p.stat().st_mtime)}
    return {'version': '', 'commit': '', 'when': ''}


def compare_version():
    """[UIT 0280] This folder's version for comparing with a release tag: VERSION.txt, else tools/version.py (git)."""
    v = local_version()['version']
    if version_key(v):
        return v
    try:
        import version as _version   # tools/version.py, next to this script
        return _version.current()[0]
    except Exception:   # a copy of this script without its neighbour, no git: unknown, any release is newer
        return v


def dt(t):
    return datetime.datetime.fromtimestamp(t).strftime('%Y-%m-%d %H:%M')


def top_and_names(z):
    """The package's top folder ('' when files are at the root) and its file entries relative to it, checked."""
    names = [i for i in z.infolist() if not i.is_dir()]
    if not names:
        raise Fail('the zip is empty')
    firsts = {i.filename.replace('\\', '/').split('/', 1)[0] for i in names}
    top = ''
    if len(firsts) == 1 and all('/' in i.filename.replace('\\', '/') for i in names):
        top = firsts.pop() + '/'
    out = []
    for i in names:
        n = i.filename.replace('\\', '/')
        rel = n[len(top):]
        parts = rel.split('/')
        if not rel or n.startswith('/') or re.match(r'^[A-Za-z]:', n) or '..' in parts or any(p == '' for p in parts):
            raise Fail(f'unsafe path in the zip: {i.filename!r}')
        if i.file_size > MAX_ENTRY:
            raise Fail(f'{rel} is {i.file_size >> 20} MB: not a source package')
        out.append((i, rel))
    return top, out


def read_package(path):
    try:
        with zipfile.ZipFile(path) as z:
            top, ents = top_and_names(z)
            rels = {r for _, r in ents}
            if 'tools/setup.py' not in rels or not any(r.startswith('src/') for r in rels):
                raise Fail('not an Ultra Instinct Tears source package (no tools/setup.py and src/)')
            vi = next((i for i, r in ents if r in ('VERSION.txt', 'VERSION')), None)
            text = z.read(vi).decode('utf-8', 'replace') if vi else ''
            v, c = norm_version(text)
            when = datetime.datetime(*vi.date_time).strftime('%Y-%m-%d %H:%M') if vi else dt(path.stat().st_mtime)
            info = {'version': v, 'commit': c, 'when': when, 'top': top, 'entries': ents}
            if not vi:
                # [UIT 0157] a package without VERSION.txt (a plain "git archive", as the 843a8f7 zip): the update used to
                # leave the folder's old VERSION.txt in place, so the folder, the launcher and the exe (tools/version.py
                # reads it at the build) kept naming the version before (cc3add4). git archive puts the commit in the zip
                # comment: take it from there, and write VERSION.txt from it; without one, VERSION.txt is removed
                # (version unknown) rather than left naming another version.
                cm = (z.comment or b'').decode('ascii', 'replace').strip()
                info['no_version_file'] = True
                if re.fullmatch(r'[0-9a-f]{40}', cm):
                    info['commit'] = cm[:7]
                    info['version'] = local_version()['version'] or '0.0.0-dev'
                    info['version_text'] = f'Ultra Instinct Tears {info["version"]} (commit {info["commit"]})\n'.encode()
                else:
                    info['version'], info['version_text'] = '', None
            return info
    except (zipfile.BadZipFile, OSError) as e:
        raise Fail(f'cannot read {path.name}: {e}')


def protected(rel):
    low = rel.lower()
    return low in PROTECTED_FILES or any(low.startswith(p) for p in PROTECTED)


# ---------------------------------------------------------------- progress
# [UIT 0430] What the progress screen shows, in plain words. The launcher reads these fields (newer launchers draw the
# step, its counts, clocks and a "still working" note from them); "msg" stays one self-contained sentence for older
# launchers, which show only that line: it is written again every second while the build runs, so even they never
# show a frozen screen.
STEP_TITLES = {   # setup.py's step names (its "[i/N] name" lines), then update.py's own stages
    'check': 'Checking the tools and game files', 'romfs': 'Packing the game\'s data', 'lift': 'Translating the game code',
    'compile': 'Compiling the game', 'shaders': 'Translating the shaders', 'textures': 'Preparing the textures',
    'finish': 'Finishing up', 'warm': 'Warming up your shaders'}
STEP_HINTS = {
    'check': 'Takes a few seconds.',
    'romfs': 'A few seconds, or several minutes when the game\'s data must be packed again.',
    'lift': 'A few seconds when nothing changed. When the game code must be translated again this can take many '
            'minutes, with long quiet stretches: that is normal.',
    'compile': 'Builds only what changed: from about a minute to half an hour or more when everything is rebuilt.',
    'shaders': 'A few seconds when no shaders changed, several minutes otherwise.',
    'textures': 'A few seconds when the textures are ready, longer the first time.',
    'finish': 'Takes a few seconds.',
    'warm': 'The game opens its window briefly and closes it by itself when done.'}
STEP_DOING = {   # [UIT 0430] what runs during the step, as Task Manager shows it
    'romfs': 'Python packs the game\'s data files (python.exe in Task Manager).',
    'lift': 'Python does this step (python.exe in Task Manager, no clang yet). High CPU use is normal.',
    'compile': 'clang compiles the game\'s C code (many clang.exe in Task Manager). High CPU use is normal.',
    'shaders': 'The shader translator runs (ultrs-shaders.exe in Task Manager).',
    'textures': 'bc7build.exe re-encodes textures the game shows often.',
    'warm': 'UltraInstinctTears.exe runs without a window to make your shaders.'}
SETUP_ORDER = ('check', 'romfs', 'lift', 'compile', 'shaders', 'textures', 'finish')   # setup.py STEPS
STAGES = {   # update.py's stages before setup.py: (step number, title, hint)
    'check': (1, 'Checking the update package', 'Takes a few seconds.'),
    'backup': (2, 'Backing up your current version', 'Takes a few seconds. Saves, settings and game data are not touched.'),
    'extract': (3, 'Copying the new files', 'Usually under a minute.'),
    'build': (4, 'Starting the rebuild', 'tools/setup.py rebuilds only what changed.'),
}
PRE_STEPS = 3          # the stages before setup.py's own steps in "step N of M"
SETUP_STEPS = 7        # setup.py's step count until its first "[i/N]" line says
TAIL_LINES = 12
QUIET_NOTE_S = 20      # no new output this long: the line says it is still working


def human_secs(s):
    s = int(max(0, s))
    if s < 60:
        return f'{s} s'
    if s < 3600:
        return f'{s // 60} min {s % 60:02d} s'
    return f'{s // 3600} h {s % 3600 // 60:02d} min'


def count_text(done, total, unit):
    if total and total > 0:
        return f'{done:,} of {total:,} {unit}'.strip()
    return ''


class Progress:
    def __init__(self, path, log):
        self.path, self.log, self.t0 = pathlib.Path(path) if path else None, log, time.time()
        self.state = {'state': 'running', 'step': '', 'pct': 0, 'msg': '', 'log': str(log.name) if log else ''}  # log: the full path, the launcher opens it
        self.out = ProgressFile(self.path, note=self.line)
        import threading
        self.lock = threading.RLock()
        self.tail, self.step_t0, self.active = [], self.t0, self.t0
        self.beat = None
        # [UIT 0430] every step in order, for the launcher's list that ticks them off (setup's own names once it says them)
        self.plan = [STAGES[k][1] for k in ('check', 'backup', 'extract')] + [STEP_TITLES[n] for n in SETUP_ORDER]
        self.state['plan'] = '|'.join(self.plan)

    def line(self, text):   # into the log (and the console) only; never raises
        try:
            print(text, flush=True)
            if self.log: self.log.write(text + '\n'); self.log.flush()
        except (OSError, ValueError):
            pass

    def __call__(self, step=None, pct=None, msg=None, state=None, **extra):
        with self.lock:
            if step is not None:
                self.stage(step)
            if pct is not None: self.state['pct'] = max(0, min(100, int(pct)))
            if msg is not None:
                self.state['msg'] = msg
                self.line(f'[{time.time() - self.t0:6.1f}s] {self.state["step"]}: {msg}')
            if state is not None: self.state['state'] = state
            self.state.update(extra)
            self.write()

    def stage(self, step):
        """update.py's own stage names (check, backup, ...) get their plain title and step number."""
        self.state['step'] = step
        if step in STAGES:
            k, title, hint = STAGES[step]
            self.new_step(title, hint, k, PRE_STEPS + SETUP_STEPS)
        elif step == 'rollback':
            self.new_step('Putting the previous version back', 'Takes a few seconds.', None, None)
        elif step == 'done':
            self.new_step('Done', '', None, None)

    def new_step(self, title, hint, k, n):
        if title != self.state.get('title'):
            self.step_t0 = time.time()
        self.state.update(title=title, hint=hint, k=k, n=n, done=None, total=None, unit='', doing='', now='')
        for f in ('k', 'n', 'done', 'total'):
            if self.state[f] is None: self.state.pop(f)

    def output(self, line, fields):
        """One line of the build's output (the log already has it) and what build_progress made of it."""
        with self.lock:
            text = line.strip()
            if text:
                self.tail = (self.tail + [text[:300]])[-TAIL_LINES:]
                self.active = time.time()
            if fields.get('title') and fields['title'] != self.state.get('title'):
                self.step_t0 = time.time()
                for f in ('done', 'total'): self.state.pop(f, None)
                self.state.update(unit='', now='', doing='')
            k, n = fields.get('k'), fields.get('n')
            if k and n and fields.get('title') and k > PRE_STEPS:   # setup's own step list: its count and names
                self.plan = (self.plan + [''] * n)[:n]
                self.plan[k - 1] = fields['title']
                self.plan = [t or f'Step {i + 1}' for i, t in enumerate(self.plan)]
                self.state['plan'] = '|'.join(self.plan)
            self.state.update({k: v for k, v in fields.items() if k != 'msg'})
            self.state['line'] = fields.get('msg', text[:200])
            self.write()

    def friendly(self):
        """The one sentence older launchers show (and newer ones fall back to)."""
        st, now = self.state, time.time()
        parts = []
        title = st.get('title') or st.get('step') or 'Working'
        parts.append((f'Step {st["k"]} of {st["n"]}: ' if st.get('k') and st.get('n') else '') + title + '.')
        if st.get('now'): parts.append(st['now'][0].upper() + st['now'][1:] + '.')
        if st.get('doing'): parts.append(st['doing'])
        c = count_text(st.get('done'), st.get('total'), st.get('unit', ''))
        if c: parts.append(c[0].upper() + c[1:] + '.')
        parts.append(f'{human_secs(now - self.step_t0)} on this step, {human_secs(now - self.t0)} in all.')
        quiet = now - self.active
        if quiet >= QUIET_NOTE_S:
            parts.append(f'Still working (no new output for {human_secs(quiet)}): this step can take several minutes.')
        return ' '.join(parts)

    def write(self):
        with self.lock:
            st, now = self.state, time.time()
            if st.get('state') == 'running' and st.get('title'):
                st['msg'] = self.friendly()
            st['t'] = round(now - self.t0, 1)
            st['st'] = round(now - self.step_t0, 1)
            st['quiet'] = round(now - self.active, 1)
            st['tail'] = '\n'.join(self.tail)
            self.out.write(st)   # [UIT 0046] was a bare os.replace: "[WinError 5] Access is denied" failed a build

    def heartbeat(self, on):
        """[UIT 0430] While the build runs: the progress file again every second (clocks, the "still working" note)."""
        import threading
        if not on:
            if self.beat:
                self.beat[0].set(); self.beat[1].join(timeout=5); self.beat = None
            return
        stop = threading.Event()

        def run():
            while not stop.wait(1.0):
                try:
                    self.write()
                except Exception:   # noqa: BLE001 - a display, never worth failing for
                    pass
        t = threading.Thread(target=run, daemon=True)
        t.start()
        self.beat = (stop, t)


# ---------------------------------------------------------------- install / rollback
def same_file(path, data):
    try:
        return path.stat().st_size == len(data) and path.read_bytes() == data
    except OSError:
        return False


def restore(bk, say):
    man = json.loads((bk / 'manifest.json').read_text())
    for rel in man.get('added', []):
        p = ROOT / rel
        if p.is_file():
            p.unlink()
    files = bk / 'files'
    for rel in man.get('replaced', []) + man.get('kept', []):
        src = files / rel
        if src.is_file():   # copied with a new time: a later build must see these as changed again
            dst = ROOT / rel
            dst.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(src, dst)
    for exe in man.get('exes', []):
        cur, old = ROOT / exe, files / exe
        if old.is_file():
            try:
                shutil.copy2(old, cur)
            except OSError:   # the running launcher: put it beside, it is used on the next start
                os.replace(cur, cur.with_suffix('.new-failed.exe')) if cur.exists() else None
                shutil.copy2(old, cur)
    old = ROOT / 'UltraInstinctTearsLauncher.old.exe'
    if 'UltraInstinctTearsLauncher.exe' in man.get('exes', []) and old.is_file():
        try:
            old.unlink()
        except OSError:   # the launcher that is running: removed by the next update
            pass
    man['rolled_back'] = dt(time.time())
    (bk / 'manifest.json').write_text(json.dumps(man, indent=1))
    clear_pending(bk)
    say(msg=f'restored {len(man.get("replaced", []))} files, removed {len(man.get("added", []))}, from {bk.name}')


PENDING = 'pending.json'


def read_pending():
    """The unfinished update's backup folder and its manifest, or (None, None)."""
    try:
        pend = json.loads((ROOT / 'updates' / PENDING).read_text())
        bk = ROOT / 'updates' / pend['backup']
        man = json.loads((bk / 'manifest.json').read_text())
        if man.get('rolled_back') or man.get('finished'):
            return None, None
        return bk, man
    except (OSError, ValueError, KeyError, TypeError):
        return None, None


def set_pending(bk):
    (ROOT / 'updates' / PENDING).write_text(json.dumps({'backup': bk.name, 'since': dt(time.time())}))


def clear_pending(bk=None):
    p = ROOT / 'updates' / PENDING
    try:
        if bk is None or json.loads(p.read_text()).get('backup') == bk.name:
            p.unlink()
    except (OSError, ValueError):
        pass


def same_target(man, info):
    to = man.get('to', {})
    return (to.get('commit') or to.get('version')) == (info['commit'] or info['version']) and \
        to.get('version') == info['version']


def handoff(pkg, info, a, say):
    """[UIT 0046] Runs the package's own tools/update.py when it differs from this one and supports UIT_UPDATE_ROOT.
    Returns its exit code, or None to go on here."""
    if os.environ.get('UIT_UPDATE_HANDOFF'):
        return None
    tops = {r: i for i, r in info['entries'] if r in ('tools/update.py', 'tools/progress_file.py')}
    if 'tools/update.py' not in tops:
        return None
    with zipfile.ZipFile(pkg) as z:
        mine = pathlib.Path(__file__).read_bytes()
        theirs = z.read(tops['tools/update.py'])
        if theirs == mine or b'UIT_UPDATE_ROOT' not in theirs or b'UIT_UPDATE_HANDOFF' not in theirs:
            return None
        import tempfile
        tmp = pathlib.Path(tempfile.mkdtemp(prefix='uit-update-'))
        for r, i in tops.items():
            (tmp / pathlib.Path(r).name).write_bytes(z.read(i))
    env = dict(os.environ, UIT_UPDATE_ROOT=str(ROOT), UIT_UPDATE_HANDOFF='1')
    try:
        args = ['install', str(pkg)] + (['--progress', str(pathlib.Path(a.progress).resolve())] if a.progress else []) + \
            (['--force'] if a.force else []) + (['--no-build'] if a.no_build else []) + (['--python', a.python] if a.python else [])
        return run_lines([sys.executable, str(tmp / 'update.py')] + args, lambda l: say.line(l.rstrip('\n')), cwd=ROOT, env=env)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


# [UIT 0430] Counts inside setup's long steps: (regex, unit). setup's children print these (tools/pack_romfs.py,
# tools/gen_game.py, tools/run_ryujinx_xlate.py, tools/bc7build, tools/warm_shaders.py); ninja's "[k/N] <command>" lines
# are the compile step's.
COUNT_LINES = [
    (re.compile(r'RomFS: (\d+) / (\d+) files'), 'files'),
    (re.compile(r'translating the code: (\d+) of (\d+) (files|functions)'), None),   # None: the unit is the line's
    (re.compile(r'translating the shaders: (\d+) of (\d+)'), 'shaders'),
    (re.compile(r'^(\d+) / (\d+) files, '), 'files'),
    (re.compile(r'^shaders: (\d+) of (\d+) \('), 'shaders'),
]


def build_progress(line, cur):
    """[UIT] What one line of setup.py's output shows on the update screen: {'msg'} and, for progress lines, 'step'
    and 'pct' (25..99). setup's own steps are lines of only "[i/N] name"; ninja's lines during the compile step are
    "[k/N] <command>" (the first build, ThinLTO's one-time full rebuild): they are shown inside setup's current step
    ("step 4 of 7: compile (1200 of 4500)") instead of replacing it, so the bar keeps moving forward. cur holds
    setup's current step between calls.
    [UIT 0430] Also the plain words of the step ('title', 'hint', 'k' of 'n' counting update.py's own stages first),
    and counts inside a step ('done' of 'total' 'unit') from the lines in COUNT_LINES. The bar never runs back
    within a step."""
    text = line.strip()
    out = {'msg': text[:200]}
    m = re.match(r'\[(\d+)/(\d+)\] (\w+)$', text)
    if m and int(m.group(2)) > 0:
        k, n = int(m.group(1)), int(m.group(2))
        cur.update(k=k, n=n, name=m.group(3), best=0)
        name = m.group(3)
        out.update(step=f'Rebuilding, step {k} of {n}: {name}', pct=max(0, min(99, int(25 + 74 * (k - 1) / n))),
                   title=STEP_TITLES.get(name, name[:1].upper() + name[1:]), hint=STEP_HINTS.get(name, ''),
                   k=PRE_STEPS + k, n=PRE_STEPS + n, doing=STEP_DOING.get(name, ''))
        cur['best'] = out['pct']
        return out
    if text.startswith('[warm]'):   # the shader warm-up after the last step (setup.py rewarm)
        cur.update(name='warm', best=99)
        out.update(step='Rebuilding: warming up shaders', pct=99, title=STEP_TITLES['warm'], hint=STEP_HINTS['warm'], doing=STEP_DOING['warm'])
        if cur.get('n'): out.update(k=PRE_STEPS + cur['n'], n=PRE_STEPS + cur['n'])
        return out
    now = re.match(r'translating the code: now (.+)$', text)   # [UIT 0430] tools/gen_game.py: what the lift does now
    if now and cur.get('n'):
        out['now'] = now.group(1)[:120]
        return out
    j = total = None
    unit = 'files'
    s = re.match(r'\[(\d+)/(\d+)\] ', text)
    if s and int(s.group(2)) > 0 and cur.get('n'):
        j, total = int(s.group(1)), int(s.group(2))
    else:
        for rx, u in COUNT_LINES:
            c = rx.search(text)
            if c and int(c.group(2)) > 0 and cur.get('n'):
                j, total, unit = int(c.group(1)), int(c.group(2)), u or c.group(3)
                break
    if total:
        k, n = cur['k'], cur['n']
        frac = min(1.0, max(0.0, j / total))
        pct = max(cur.get('best', 0), max(0, min(99, int(25 + 74 * (k - 1 + frac) / n))))
        cur['best'] = pct
        out.update(step=f'Rebuilding, step {k} of {n}: {cur["name"]} ({j} of {total})', pct=pct,
                   done=min(j, total), total=total, unit=unit)
    return out


def prune_backups():
    bks = sorted((ROOT / 'updates').glob('backup-*'))
    pbk, _ = read_pending()
    for b in bks[:-KEEP_BACKUPS]:
        if pbk and b.name == pbk.name:
            continue
        shutil.rmtree(b, ignore_errors=True)


def install(pkg, a):
    (ROOT / 'logs').mkdir(exist_ok=True)
    stamp = datetime.datetime.now().strftime('%Y%m%d-%H%M%S')
    log = open(ROOT / 'logs' / f'update-{stamp}.log', 'w', encoding='utf-8')
    say = Progress(a.progress, log)
    bk = man = None
    try:
        say('check', 1, f'{pkg}')
        if (ROOT / '.git').exists():
            raise Fail('this folder is a Git checkout: update it with git instead')
        info = read_package(pkg)
        code = handoff(pkg, info, a, say)
        if code is not None:   # the package's newer updater did the install (its own log and progress)
            say.line(f'handed over to the package\'s tools/update.py (exit {code})')
            return code
        cur = local_version()
        say(msg=f'package {info["version"]} {info["commit"]} ({info["when"]}); this folder '
                + (f'{cur["version"]} {cur["commit"]} ({cur["when"]})' if cur['version'] else 'has no VERSION.txt (version unknown)'))
        if info.get('no_version_file'):   # [UIT 0157]
            say.line(f'warning: the package has no VERSION.txt: commit {info["commit"]} from the zip comment, VERSION.txt is written from it'
                     if info['commit'] else 'warning: the package has no VERSION.txt and no commit in its zip comment: '
                     'VERSION.txt is removed after the update (version unknown), not left naming the version before')
        pbk, pman = read_pending()
        if pbk and not same_target(pman, info):
            say(msg=f'an unfinished update to {pman["to"].get("version")} {pman["to"].get("commit")} is in this folder: '
                    f'putting the version before it back first (from updates/{pbk.name})')
            restore(pbk, say)
            pbk = pman = None
            cur = local_version()
        resume = pbk is not None
        if resume:
            say(msg=f'resuming the unfinished update from updates/{pbk.name} (started {pman.get("started", "?")})')
        elif info['commit'] and info['commit'] == cur['commit'] and not a.force:
            raise Fail('this version is already installed (choose it again with "install anyway" to reinstall)')
        ents = [(i, r) for i, r in info['entries'] if not protected(r)]
        skipped = len(info['entries']) - len(ents)
        say('backup', 5, f'{len(ents)} files in the package, {skipped} skipped (your data, saves, settings, build)')
        (ROOT / 'updates').mkdir(exist_ok=True)
        if resume:
            bk, man = pbk, pman
        else:
            for k in range(100):   # a folder of its own: a rollback must only ever read this update's backup
                cand = ROOT / 'updates' / (f'backup-{stamp}' + (f'-{k}' if k else ''))
                try:
                    cand.mkdir()
                    break
                except FileExistsError:
                    continue
            else:
                raise Fail('cannot make a backup folder in updates/')
            bk = cand
            (bk / 'files').mkdir()
            man = {'package': str(pkg), 'from': cur, 'to': {k: info[k] for k in ('version', 'commit', 'when')},
                   'replaced': [], 'added': [], 'kept': [], 'exes': [], 'started': dt(time.time())}
        files = bk / 'files'
        known = set(man['replaced']) | set(man['added'])
        with zipfile.ZipFile(pkg) as z:
            plan, changed, added = [], [], []
            for n, (i, rel) in enumerate(ents):
                data = z.read(i)
                dst = ROOT / rel
                if dst.is_file() and same_file(dst, data):
                    continue   # already the package's bytes (an earlier extract, or unchanged between versions)
                if rel not in known:   # on a resume, files the first run recorded keep their original backup
                    if dst.is_file():
                        (files / rel).parent.mkdir(parents=True, exist_ok=True)
                        shutil.copy2(dst, files / rel)
                        man['replaced'].append(rel)
                    else:
                        man['added'].append(rel)
                (changed if dst.is_file() else added).append(rel)
                plan.append((rel, i))
            synth = []   # [UIT 0157] VERSION.txt for a package without one (read_package), written after the build as one would be
            if info.get('no_version_file'):
                rel, data = 'VERSION.txt', info.get('version_text')
                dst = ROOT / rel
                if (dst.is_file() and (data is None or not same_file(dst, data))) or (not dst.is_file() and data is not None):
                    if rel not in known:
                        if dst.is_file():
                            shutil.copy2(dst, files / rel)
                            man['replaced'].append(rel)
                        else:
                            man['added'].append(rel)
                    (changed if dst.is_file() else added).append(rel)
                    synth.append((rel, data))
            if not resume:
                for c in sorted(PROTECTED_FILES):
                    if (ROOT / c).is_file():
                        shutil.copy2(ROOT / c, files / c)
                        man['kept'].append(c)
                for exe in EXES:
                    if (ROOT / exe).is_file():
                        shutil.copy2(ROOT / exe, files / exe)
                        man['exes'].append(exe)
            (bk / 'manifest.json').write_text(json.dumps(man, indent=1))
            set_pending(bk)
            same = len(ents) - len(plan)
            say('extract', 10, f'{len(changed)} files change, {len(added)} are new, {same} already match the package '
                               f'(backup: updates/{bk.name})')
            for what, names in (('changes', changed), ('new', added)):
                if names:
                    say.line(f'    {what}: ' + ', '.join(names[:40]) + (f' and {len(names) - 40} more' if len(names) > 40 else ''))
            # VERSION.txt last (after the build): until then the folder still says what it had
            version_ents = [(rel, i) for rel, i in plan if rel in ('VERSION.txt', 'VERSION')]
            plan = [(rel, i) for rel, i in plan if rel not in ('VERSION.txt', 'VERSION')]
            for n, (rel, i) in enumerate(plan):
                dst = ROOT / rel
                dst.parent.mkdir(parents=True, exist_ok=True)
                tmp = dst.with_name(dst.name + '.uit-new')
                tmp.write_bytes(z.read(i))
                os.replace(tmp, dst)
                if n % 200 == 0:
                    say(pct=10 + 15 * n / max(1, len(plan)), done=n, total=len(plan), unit='files')
            version_data = [(rel, z.read(i)) for rel, i in version_ents] + synth

        def finish(text):
            for rel, data in version_data:
                if data is None: (ROOT / rel).unlink(missing_ok=True)
                else: (ROOT / rel).write_bytes(data)
            man['finished'] = dt(time.time())
            (bk / 'manifest.json').write_text(json.dumps(man, indent=1))
            clear_pending(bk)
            say('done', 100, text, state='done')
            prune_backups()
            return 0

        if a.no_build:
            return finish('files updated (no build: --no-build)')
        # the launcher may be running: Windows lets a running .exe be renamed, not overwritten
        launcher = ROOT / EXES[1]
        if launcher.is_file():
            old = launcher.with_name('UltraInstinctTearsLauncher.old.exe')
            try:
                old.unlink(missing_ok=True)
                os.replace(launcher, old)
                shutil.copy2(old, launcher)   # still there if the build does not make a new one
            except OSError as e:
                say(msg=f'note: could not move the launcher aside ({e}); its rebuild may fail, the game is unaffected')
        py = a.python or sys.executable
        say('build', 25, f'{py} tools/setup.py (only what changed is rebuilt)')
        cur = {}
        def build_line(line):
            log.write(line); log.flush()
            if line.strip():
                say.output(line, build_progress(line, cur))
        # [UIT 0129] the build stamps the package's version and commit into the exe and the launcher (tools/version.py):
        # VERSION.txt is only written after the build, so it still names the version being replaced
        benv = dict(os.environ)
        benv.pop('UIT_BUILD_VERSION', None); benv.pop('UIT_BUILD_COMMIT', None)
        if info['version'] and info['commit']:
            benv.update(UIT_BUILD_VERSION=info['version'], UIT_BUILD_COMMIT=info['commit'])
        say.heartbeat(True)   # [UIT 0430] the clocks keep moving through quiet stretches
        try:
            rc = run_lines([py, '-u', 'tools/setup.py'], build_line, cwd=ROOT, env=benv)   # [UIT 0055] every std handle explicit
        finally:
            say.heartbeat(False)
        if rc != 0:
            raise Fail(f'tools/setup.py failed (exit {rc}); see logs/{pathlib.Path(log.name).name}')
        return finish(f'Updated to {info["version"]} {info["commit"]}')
    except (Fail, OSError, zipfile.BadZipFile) as e:
        say(msg=f'FAILED: {e}')
        if bk and (bk / 'manifest.json').is_file():
            say('rollback', None, 'putting the previous version back')
            try:
                restore(bk, say)
                was = (man or {}).get('from', {})
                back = f'{was.get("version")} {was.get("commit")}'.strip() if was.get('version') else 'the previous files'
                say(state='rolled_back', msg=f'update failed and was undone ({back} back in place): {e}')
            except Exception as e2:   # noqa: BLE001 - report, the backup stays for a manual rollback
                say(state='failed', msg=f'update failed ({e}) and the rollback too ({e2}). Install the same update again '
                                        f'to resume it, or run: py -3 tools/update.py rollback (backup kept in updates/{bk.name})')
        else:
            say(state='failed', msg=f'update not started: {e}')
        return 1
    finally:
        log.close()


def cfg_value(key):
    cfg = ROOT / 'ultrainstincttears.cfg'
    if cfg.is_file():
        for line in cfg.read_text(errors='replace').splitlines():
            k, _, v = line.strip().partition('=')
            if k.strip() == key and v.strip() not in ('', '-'):   # '-' = left at the default
                return v.strip()
    return ''


def default_folders(extra):
    fs = list(extra)
    cfg = cfg_value('TOTK_LAUNCHER_UPDATE_DIRS')
    fs += [f for f in cfg.split(';') if f.strip()] if cfg else []
    fs += [str(ROOT), str(ROOT.parent)]
    return [os.path.expandvars(os.path.expanduser(f.strip())) for f in fs]


ONLINE_DEADLINE = 30   # seconds for the whole check: a DNS lookup ignores the socket timeout


CHECK_DEADLINE = 12   # [UIT 0320] check: the whole start-up check
CHECK_TIMEOUT = 6     # [UIT 0320] check: per connect / read


def online_line(deadline=None, timeout=None):
    """[UIT 0280] scan --online's last line: release / online error, tab-separated, never raises and never takes longer
    than ONLINE_DEADLINE (the check runs in a daemon thread; a stuck lookup is abandoned with the process)."""
    import threading
    out = []
    t = threading.Thread(target=lambda: out.append(_online_line(timeout)), daemon=True)
    t.start()
    t.join(deadline or ONLINE_DEADLINE)
    if out:
        return out[0]
    return 'online\terror\t' + tsv_escape('GitHub did not answer in time. Check the internet connection, or install from a zip instead')


def _online_line(timeout=None):
    try:
        src = GitHubReleasesSource(timeout=timeout)
        c = src.candidate(src.release())
        rel, mine = c.release, compare_version()
        sha = 'sha256' if any(a.get('name', '').lower() in (c.label.lower() + '.sha256', 'sha256sums', 'sha256sums.txt')
                              for a in rel.get('assets') or []) or (c.asset.get('digest') or '').startswith('sha256:') \
            or sha_from_text(rel.get('body') or '', c.label, need_word=True) else ''
        return '\t'.join(tsv_escape(x) for x in (
            'release', c.ref, c.version, '1' if is_newer(c.version, mine) else '0', c.label, c.size, c.when,
            '1' if rel.get('prerelease') else '0', sha, notes_text(rel.get('body')), rel.get('html_url') or '', mine))
    except Fail as e:
        return 'online\terror\t' + tsv_escape(e)
    except Exception as e:   # never a traceback in the launcher: one line it can show
        return 'online\terror\t' + tsv_escape(f'the update check failed ({type(e).__name__}: {e})')


def download(a):
    """[UIT 0280] Download one release's source zip into updates/downloads/ and check it; prints "zip<TAB>path" last.
    The launcher then runs install on that path: the same install, backup and rollback as a zip from chat."""
    (ROOT / 'logs').mkdir(exist_ok=True)
    log = open(ROOT / 'logs' / 'update-download.log', 'w', encoding='utf-8')
    say = Progress(a.progress, log)
    say.state['log'] = str(ROOT / 'logs' / 'update-download.log')
    src = None
    try:
        say.new_step('Downloading the update', 'Depends on your connection; the download is checked before anything is installed.', None, None)
        say(step='download', pct=0, msg='asking GitHub for the release')
        src = GitHubReleasesSource()
        say.line(f'repository github.com/{src.repo}{" (pre-releases too)" if src.prerelease else ""}'
                 f'{" (with an access token)" if src.token else ""}')   # never the token itself
        c = src.candidate(src.release(a.tag))
        say(msg=f'{c.ref}: {c.label}, {c.size >> 20} MB')
        out, digest, checked, secs = src.fetch(c, ROOT / 'updates' / 'downloads', say)
        for n in src.notes:   # [UIT 0284] e.g. Windows' curl.exe took over from Python's ssl
            say.line(n)
        src.notes = []
        say(pct=100, msg=f'downloaded in {secs:.0f}s; SHA-256 {digest}' +
            (f' matches {", ".join(checked)}' if checked else ' (the release publishes none: size and package checked)'))
        say.state['zip'] = str(out)
        say(state='done', msg=f'ready: {out.name}')
        print(f'zip\t{out}', flush=True)
        return 0
    except Fail as e:
        say(state='failed', msg=f'download failed: {e}')
        return 1
    except Exception as e:
        say(state='failed', msg=f'download failed ({type(e).__name__}: {e})')
        return 1
    finally:
        for n in (src.notes if src else []):
            say.line(n)
        log.close()


def check(a):
    """[UIT 0320] Advanced > Updates and reports > Check for updates when the launcher starts: the launcher runs this
    once, in the background, a few seconds after it opens (never while the game runs; it stops this process when Play
    is pressed). Only GitHub, no local zips; one "release" or "online error" line (as scan --online), at most
    --deadline seconds, never a traceback. The launcher shows a newer release as a notice and logs everything else."""
    if cfg_value('TOTK_LAUNCHER_UPDATE_ONLINE') == '0':   # Check GitHub for updates off: never the network
        print('online\toff\tCheck GitHub for updates is off', flush=True)
        return 0
    deadline = max(1.0, min(float(a.deadline), ONLINE_DEADLINE))
    print(online_line(deadline=deadline, timeout=min(CHECK_TIMEOUT, deadline)), flush=True)
    return 0


def main():
    std_streams()
    ap = argparse.ArgumentParser(description='Install an Ultra Instinct Tears source package over this folder.')
    sub = ap.add_subparsers(dest='cmd', required=True)
    s = sub.add_parser('scan'); s.add_argument('--folder', action='append', default=[]); s.add_argument('--json', action='store_true')
    s.add_argument('--source', choices=sorted(SOURCES), default='local')
    s.add_argument('--file', type=pathlib.Path, help='read this one package (a zip the player picked)')
    s.add_argument('--tsv', action='store_true', help='tab-separated lines for the launcher: current/package, path, name, version, commit, date')
    i = sub.add_parser('install'); i.add_argument('package', type=pathlib.Path)
    i.add_argument('--progress'); i.add_argument('--force', action='store_true'); i.add_argument('--no-build', action='store_true')
    i.add_argument('--python', help='the Python that runs tools/setup.py (default: this one)')
    s.add_argument('--online', action='store_true', help='[UIT 0280] with --tsv: also a "release" (or "online error") line from GitHub')
    d = sub.add_parser('download', help='[UIT 0280] download and check a GitHub release\'s source zip')
    d.add_argument('--tag', help='the release (default: the newest one scan --online offers)'); d.add_argument('--progress')
    r = sub.add_parser('rollback'); r.add_argument('backup', nargs='?')
    k = sub.add_parser('check', help='[UIT 0320] the launcher\'s start-up check: one "release" / "online error" line, short timeouts')
    k.add_argument('--deadline', type=float, default=CHECK_DEADLINE, help='seconds for the whole check')
    a = ap.parse_args()
    if a.cmd == 'download':
        return download(a)
    if a.cmd == 'check':
        return check(a)
    if a.cmd == 'scan':
        try:
            if a.file:
                info = read_package(a.file)
                cands = [Candidate('local', a.file.resolve(), a.file.name, info['version'], info['commit'], info['when'], a.file.stat().st_size)]
            else:
                src = LocalZipSource(default_folders(a.folder)) if a.source == 'local' else SOURCES[a.source]()
                cands = sorted(src.candidates(), key=lambda c: c.when, reverse=True)
        except (Fail, OSError) as e:
            print(json.dumps({'error': str(e)}) if a.json else f'error\t{e}' if a.tsv else f'ERROR: {e}'); return 1
        cur = local_version()
        if a.tsv:
            clean = lambda x: str(x).replace('\t', ' ').replace('\n', ' ')
            print('\t'.join(['current', '', '', clean(cur['version']), clean(cur['commit']), clean(cur['when'])]))
            for c in cands:
                print('\t'.join(['package', clean(c.ref), clean(c.label), clean(c.version), clean(c.commit), clean(c.when)]))
            if a.online and not a.file:
                print(online_line(), flush=True)
            return 0
        if a.json:
            print(json.dumps({'current': cur, 'packages': [c.as_dict() for c in cands]}))
        else:
            print(f'this folder: {cur["version"]} {cur["commit"]} ({cur["when"]})')
            for c in cands:
                print(f'  {c.when}  {c.version} {c.commit}  {c.ref}{"  (installed)" if c.commit and c.commit == cur["commit"] else ""}')
            if not cands: print('  no UltraInstinctTears-src*.zip found')
        return 0
    if a.cmd == 'install':
        return install(a.package.resolve(), a)
    bks = sorted((ROOT / 'updates').glob('backup-*'))
    bk = (ROOT / 'updates' / a.backup) if a.backup else (bks[-1] if bks else None)
    if not bk or not (bk / 'manifest.json').is_file():
        print('no update backup to roll back to'); return 1
    restore(bk, Progress(None, None))
    print('rolled back; run tools/setup.py to rebuild the previous version'); return 0


if __name__ == '__main__':
    sys.exit(main())
