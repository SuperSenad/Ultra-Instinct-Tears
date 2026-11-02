"""Build Ultra Instinct Tears from the player's own dump, in one command: every step is checked and can be resumed.

From a Git checkout the build goes to dist\\, or to --output <folder>. The tracked source files are copied there and
missing build tools are downloaded from the latest GitHub builder release. In a builder package setup runs in place.
usage: python tools/setup.py [--output <build folder>] [--dump <folder with exefs\\ and romfs\\>] [--cpus N]
                             [--get-dlss] [--no-dlss | --require-dlss]
                             [--pgo-profile | --pgo | --no-pgo] [--lto | --no-lto] [--O3 | --O2]
The dump is game\\ in the checkout/build folder unless --dump names another (remembered for later runs); run it again
to resume, or to rebuild what changed after an update of the port.
The dump: the ExeFS and RomFS of The Legend of Zelda: Tears of the Kingdom with update 1.4.3 applied, as plain files,
extracted by the player from their own console. Encrypted packages (.nsp, .xci, .nca, ...) are refused: the port
decrypts nothing and reads no keys (README.md, Legal). Optional: <dump>\\sysdata\\fonts\\*.bfttf, the console's shared fonts.

Steps (data\\ holds everything made from the dump; it never leaves this PC):
  check     the tools, the dump's version (SHA-256 of the code modules) and its files
  romfs     the RomFS folder packed into one image, data\\romfs (its content hash checked against 1.4.3)
  lift      the game's code translated to C, data\\generated
  compile   the C compiled and linked: UltraInstinctTears.exe (objects in build\\), and the settings launcher UltraInstinctTearsLauncher.exe
  shaders   every shader of the game found and translated to SPIR-V, data\\shaders
  textures  the ASTC textures re-encoded for PC graphics cards (BC7 / BC1), data\\textures
  finish    ultrainstincttears.cfg

DLSS is always compiled in (src/vk/dlss_sl.c; no NVIDIA headers needed). It runs when NVIDIA's Streamline 2.14.16 DLLs
are next to UltraInstinctTears.exe: setup.py copies them from deps\\streamline (the SDK, extracted there by the player, or
fetched from NVIDIA's GitHub release with --get-dlss). Without them TOTK_UPSCALER=dlss uses FSR, and setup says so.
  --get-dlss      download NVIDIA's Streamline SDK 2.14.16 (276 MB, pinned SHA-256) to deps\\streamline once
  --require-dlss  stop with an error when the DLSS DLLs are not found (instead of a warning)
  --no-dlss       build without DLSS (stubs: TOTK_UPSCALER=dlss runs FSR), no Streamline needed

Speed (the game is CPU-bound; these change only how the C is compiled, nothing it does, and are remembered):
  --pgo-profile   step 1 of profile-guided optimization: builds a profiling UltraInstinctTears.exe (slower while it
                  counts). Play 10+ minutes the way you normally do (exploring, fights, Ultrahand, the sky, a cave);
                  the counts are saved to build\\pgo\\*.profraw every 30 s and when the game closes.
  --pgo           step 2: merges build\\pgo\\*.profraw into build\\pgo\\uit.profdata and builds the fast exe with it
                  (later runs keep using it; --no-pgo goes back to a normal build). The two LLVM files PGO needs
                  (the profile runtime and llvm-profdata, MSYS2 CLANG64 23.1.3, pinned SHA-256) are downloaded once.
  --pgo-guide     prints where you are in the three PGO steps and what to do next (builds nothing)
  --lto           ThinLTO: inlining across the C files at the link (longer link, more memory). On by default; --no-lto:
                  off (remembered). Folders set up before ThinLTO was the default get it once, unless --lto/--no-lto
                  or a value other than the old default was chosen there. If the ThinLTO link fails, it is tried
                  once more with fewer link jobs, then setup builds again with the normal link by itself, says so,
                  and stays on the normal link (--lto tries ThinLTO again).
  --O3            -O3 instead of -O2 (not the default: not yet measured on the game's translated code). --O2: back.
Each changes every object file: the compile step then rebuilds all of the game's C (as long as a first build).
"""
import argparse
import filecmp
import hashlib
import importlib
import json
import os
import pathlib
import shutil
import ssl
import subprocess
import sys
import time
import urllib.request
import zipfile

ROOT = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'tools'))
import toolchain  # noqa: E402
import public_tree  # noqa: E402
import version  # noqa: E402
import warm_shaders  # noqa: E402

DATA = ROOT / 'data'
STATE = DATA / 'setup.json'
# The version the port is made for: TOTK 1.4.3 (facts about the player's files, no game data).
EXEFS = {
    'main': 'e84a90d551fed0824b4b74933b9c6d043c458521363a7cea58b2faca1822dc9e',
    'subsdk0': '767c35bcd7d23e78f9063c69b638290b8b83f91e839beb10799e6576960179b1',
    'sdk': '1d187f29784cd0af46e9e123fc2a62c77b3b07c5a39c8633d90d6c80de64e4ad',
}
ROMFS_FILES = 304243
ROMFS_BYTES = 17026725026
ROMFS_CONTENT = 'b2a2e966a06158e098c03e564f77fd72809bc57275f04eb0725bcde14d522056'
ENCRYPTED = ('.nsp', '.nsz', '.xci', '.xcz', '.nca', '.ncz')
GITHUB_RELEASES = 'https://api.github.com/repos/kuraeow/ultratears/releases?per_page=20'
KEEP = {'game', 'data', 'build', 'userdata', 'diag', 'deps', 'dist', 'dev-local'}
OUTPUT_OWN = {'setup.bat', 'VERSION.txt', 'THIRD-PARTY-NOTICES.md', 'licenses', 'ultrainstincttears.cfg', 'UltraInstinctTears.exe',
              'UltraInstinctTearsLauncher.exe', 'UltraInstinctTears.pdb', 'setup-files.txt',
              'ultratears.cfg', 'UltraTears.exe', 'UltraTearsLauncher.exe', 'UltraTears.pdb',   # the names before the rename
              'sl.interposer.dll', 'sl.common.dll', 'sl.dlss.dll', 'nvngx_dlss.dll',
              'sl.pcl.dll', 'NvLowLatencyVk.dll'}
# NVIDIA Streamline / DLSS (downloaded by the user, never shipped: BUILD-WINDOWS-FSR.md sections 14 and 17).
# src/vk/sl_abi.h is the C form of this version's ABI; slInit refuses DLLs it is not compatible with.
GAME_EXE, LAUNCHER_EXE = 'UltraInstinctTears.exe', 'UltraInstinctTearsLauncher.exe'
OLD_EXES = ('UltraTears.exe', 'UltraTearsLauncher.exe', 'UltraTears.pdb')   # before the rename: left in place, see old_exes_note
OLD_CFG = 'ultratears.cfg'   # likewise: step_finish copies it to ultrainstincttears.cfg once
SL_VERSION = '2.14.16'
SL_DLLS = ('sl.interposer.dll', 'sl.common.dll', 'sl.dlss.dll', 'nvngx_dlss.dll')
SL_OPTIONAL = ('sl.pcl.dll', 'NvLowLatencyVk.dll')   # sl.pcl: loaded by default like sl.common (ProgrammingGuidePCL.md); NvLowLatencyVk: sl.common loads it from the exe's folder (Reflex on Vulkan, nvllvk.cpp); absent, only a log line
SL_RELEASE = f'https://github.com/NVIDIA-RTX/Streamline/releases/tag/v{SL_VERSION}'
SL_ZIP = (f'streamline-sdk-v{SL_VERSION}.zip', 276016170,
          '32041e51db0cab709a271b72a352281b690c2abbd669a827809401a5dd23956b',
          f'https://github.com/NVIDIA-RTX/Streamline/releases/download/v{SL_VERSION}/streamline-sdk-v{SL_VERSION}.zip')
SL_ZIP_KEEP = tuple(f'bin/x64/{n}' for n in SL_DLLS + SL_OPTIONAL) + (
    'bin/x64/nvngx_dlss.license.txt', 'license.txt', '3rd-party-licenses.md', 'include/sl.h', 'include/sl_version.h')
DLSS_MODE = 'auto'   # 'auto', 'require' or 'off' (--require-dlss, --no-dlss)
# [UIT 0019] PGO: the two files of MSYS2 CLANG64 23.1.3 that the builder's clang 23.1.3 lacks (packages pinned by SHA-256)
MSYS2_CLANG64 = 'https://repo.msys2.org/mingw/clang64/'
PGO_PACKAGES = (
    ('mingw-w64-clang-x86_64-compiler-rt-23.1.3-1-any.pkg.tar.zst', 814081,
     '3f0926a9b74eee208f0add40f05684ca1766bd44b64137de72c73eac165dc244',
     ('clang64/lib/clang/23/lib/windows/libclang_rt.profile-x86_64.a',)),
    ('mingw-w64-clang-x86_64-llvm-tools-23.1.3-1-any.pkg.tar.zst', 23348660,
     'be6386e7d28fdea73fdc03f5627183f06486052e1d51e01f2a411f6d0870233c',
     ('clang64/bin/llvm-profdata.exe',)),
)
SPEED_DEFAULT = {'pgo': 'off', 'lto': 'on', 'opt': 'O2'}   # [UIT] ThinLTO on by default (speed-v 2)
SPEED_OLD_DEFAULT = {'pgo': 'off', 'lto': 'off', 'opt': 'O2'}   # speed-v 1: before ThinLTO was the default
SPEED_V = 3   # [UIT 0148] 3: a --no-lto remembered from before is reset once (see resolve_speed)
SPEED = dict(SPEED_DEFAULT)   # this run's choice (main: the options, else data\setup.json, else SPEED_DEFAULT)
LTO_LINK_FAILED = 3   # tools/build_game.py's exit code when only the ThinLTO link failed


class Fail(Exception):
    pass


def say(msg):
    print(msg, flush=True)


def is_checkout():
    r = subprocess.run(['git', '-C', str(ROOT), 'rev-parse', '--show-toplevel'], capture_output=True, text=True,
                       stdin=subprocess.DEVNULL)
    return r.returncode == 0 and pathlib.Path(r.stdout.strip()).resolve() == ROOT


def source_files():
    out = subprocess.run(['git', '-C', str(ROOT), 'ls-files', '-z'], check=True, capture_output=True,
                         stdin=subprocess.DEVNULL).stdout
    files = [f for f in out.decode('utf-8').split('\0') if f]
    return [f for f in files if f.split('/')[0] not in public_tree.PRIVATE and (ROOT / f).is_file()]


def sync_source(home):
    """Copy tracked files, including uncommitted edits, while preserving the build folder's own files."""
    if home == ROOT:
        return 0, 0
    home.mkdir(parents=True, exist_ok=True)
    files = source_files()
    if any(f.split('/')[0] in KEEP for f in files):
        raise Fail('a tracked path would overwrite the build folder\'s own data: refusing')
    changed = 0
    for f in files:
        src, dst = ROOT / f, home / f
        if dst.is_file() and filecmp.cmp(src, dst, shallow=False):
            continue
        dst.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(src, dst)
        changed += 1
    listing = home / 'setup-files.txt'
    old = set(listing.read_text(encoding='utf-8').splitlines()) if listing.is_file() else set()
    removed = 0
    for f in sorted(old - set(files)):
        p = home / f
        if f.split('/')[0] not in KEEP | OUTPUT_OWN and p.is_file():
            p.unlink()
            removed += 1
            for d in p.parents:
                if d == home or any(d.iterdir()):
                    break
                d.rmdir()
    listing.write_text('\n'.join(files) + '\n', encoding='utf-8')
    v, commit = version.current()
    (home / 'VERSION.txt').write_text(version.version_txt(v, commit), encoding='utf-8')
    return changed, removed


def deps_ready(home):
    py = home / 'deps/python/python.exe'
    programs = (py, home / 'deps/clang64/bin/clang.exe', home / 'deps/clang64/bin/clang++.exe',
                home / 'deps/clang64/bin/lld.exe', home / 'deps/clang64/bin/ninja.exe',
                home / 'deps/shaders/ultrs-shaders.exe')
    if not all(p.is_file() for p in programs):
        return False
    packages = subprocess.run([str(py), '-c', 'import lz4, capstone'], stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                              stderr=subprocess.DEVNULL, env=dict(os.environ, PYTHONNOUSERSITE='1'))
    return packages.returncode == 0


def builder_asset():
    request = urllib.request.Request(GITHUB_RELEASES, headers={'Accept': 'application/vnd.github+json',
                                                               'User-Agent': 'UltraTears-setup'})
    try:
        with urlopen(request, 30) as response:
            releases = json.load(response)
    except Exception as e:
        raise Fail(f'could not query GitHub releases for the build tools: {e}') from e
    for release in releases:
        for asset in release.get('assets', []):
            name = asset.get('name', '')
            if name.startswith('UltraTears-builder-') and name.endswith('-windows-x86_64.zip'):
                return name, asset['browser_download_url']
    raise Fail('no (upstream) UltraTears builder package was found in the GitHub releases')


def tls_context():
    """[UIT 0140] The certificates Windows trusts, checked as strictly as ever (verification is never turned off).
    truststore (when installed) has Windows itself check the chain, as browsers do; else the default context with the
    system's certificate stores loaded. Python only reads the roots already installed; Windows fetches a missing root
    (ISRG Root X1 for repo.msys2.org) on demand, so on some PCs Python ends at an expired cross-signed root and fails
    with "certificate has expired" where a browser works: download() then asks Windows' own curl.exe."""
    try:
        import truststore
        return truststore.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
    except Exception:
        pass
    ctx = ssl.create_default_context()
    ctx.load_default_certs()
    return ctx


def windows_curl():
    """%SystemRoot%\\System32\\curl.exe (in Windows 10 1803 and later): TLS through Windows (Schannel) and its root store."""
    root = os.environ.get('SystemRoot')
    curl = pathlib.Path(root) / 'System32' / 'curl.exe' if root else None
    return curl if os.name == 'nt' and curl and curl.is_file() else None


def curl_command(curl, url, path):
    """Full certificate and revocation checks (curl's defaults; never -k / --insecure); https only, also on redirects."""
    return [str(curl), '--fail', '--location', '--proto', '=https', '--proto-redir', '=https', '--retry', '2',
            '--connect-timeout', '30', '--user-agent', 'UltraTears-setup', '--output', str(path), url]


def is_cert_error(e):
    while e is not None:
        if isinstance(e, ssl.SSLCertVerificationError) or (isinstance(e, ssl.SSLError) and 'CERTIFICATE' in str(e)):
            return True
        e = getattr(e, 'reason', None) if not isinstance(e, ssl.SSLError) else None
    return False


def urlopen(request, timeout):
    return urllib.request.urlopen(request, timeout=timeout, context=tls_context())


def download(url, path, what='the build tools'):
    request = urllib.request.Request(url, headers={'User-Agent': 'UltraTears-setup'})
    try:
        with urlopen(request, 60) as response, open(path, 'wb') as out:
            total = int(response.headers.get('Content-Length', 0))
            done, shown = 0, -1
            while True:
                block = response.read(1 << 20)
                if not block:
                    break
                out.write(block)
                done += len(block)
                mark = done // (64 << 20)
                if mark != shown:
                    shown = mark
                    suffix = f' / {total / 2**20:.0f} MiB' if total else ''
                    say(f'  downloading {what}: {done / 2**20:.0f} MiB{suffix}')
    except Exception as e:
        path.unlink(missing_ok=True)
        curl = windows_curl() if is_cert_error(e) else None
        if curl:
            # [UIT 0140] Python could not verify the certificate with the roots it sees; Windows' curl.exe verifies it
            # with Windows' own (fetching a missing root as Windows does). Same checks, same SHA-256 check after.
            say(f'  Python could not verify the certificate of {url} ({e}); downloading with Windows\' curl.exe instead')
            r = subprocess.run(curl_command(curl, url, path), stdin=subprocess.DEVNULL)
            if r.returncode == 0 and path.is_file():
                return
            path.unlink(missing_ok=True)
            raise Fail(f'could not download {what} ({url}): {e}; Windows\' curl.exe also failed (exit code {r.returncode}). '
                       'Check the PC\'s date and time, then run Windows Update (it updates the trusted root certificates)') from e
        raise Fail(f'could not download {what} ({url}): {e}') from e


def fetch_pinned(name, size, digest, url, what):
    """dev-local\\downloads\\<name>: kept from an earlier run or put there by hand, else downloaded; SHA-256 checked."""
    cache = ROOT / 'dev-local/downloads'
    cache.mkdir(parents=True, exist_ok=True)
    path = cache / name
    if path.is_file() and (path.stat().st_size != size or sha256(path) != digest):
        bad = path.with_name(name + '.bad')
        path.replace(bad)
        say(f'  {path} is not the expected file (SHA-256 {digest[:16]}...): moved to {bad.name}, downloading again')
    if not path.is_file():
        say(f'  downloading {name} ({size / 2**20:.1f} MiB) from {url}')
        part = path.with_name(name + '.part')
        download(url, part, what)
        if part.stat().st_size != size or sha256(part) != digest:
            part.unlink(missing_ok=True)
            raise Fail(f'{name} from {url} does not have the expected SHA-256 {digest}')
        part.replace(path)
    return path


def install_deps(home):
    if deps_ready(home):
        return
    name, url = builder_asset()
    cache = ROOT / 'dev-local/downloads'
    cache.mkdir(parents=True, exist_ok=True)
    archive = cache / name
    if not archive.is_file():
        say(f'Build tools are missing; downloading {name} once.')
        partial = archive.with_suffix(archive.suffix + '.part')
        download(url, partial)
        partial.replace(archive)
    say(f'  installing tools in {home / "deps"}')
    try:
        with zipfile.ZipFile(archive) as zf:
            for info in zf.infolist():
                parts = pathlib.PurePosixPath(info.filename).parts
                indexes = [i for i, p in enumerate(parts) if p in ('deps', 'licenses')]
                if not indexes:
                    continue
                rel = pathlib.Path(*parts[indexes[0]:])
                if '..' in rel.parts:
                    raise Fail(f'unsafe path in the builder archive: {info.filename}')
                dst = home / rel
                if info.is_dir():
                    dst.mkdir(parents=True, exist_ok=True)
                else:
                    dst.parent.mkdir(parents=True, exist_ok=True)
                    try:
                        with zf.open(info) as src, open(dst, 'wb') as out:
                            shutil.copyfileobj(src, out)
                    except PermissionError:
                        # An in-place repair may be running with deps/python/python.exe, whose EXE/DLLs Windows locks.
                        if not dst.is_file():
                            raise
    except (OSError, zipfile.BadZipFile) as e:
        archive.unlink(missing_ok=True)
        raise Fail(f'could not unpack the build tools: {e}') from e
    if not deps_ready(home):
        raise Fail(f'{name} did not contain all required build tools')


def choose_dump(home, requested):
    if requested:
        return requested.resolve()
    for candidate in (home / 'game', ROOT / 'game'):
        if (candidate / 'exefs').is_dir() or (candidate / 'romfs').is_dir():
            return candidate.resolve()
    return (home / 'game').resolve()


def zstd_reader(fileobj):
    """A .zst stream reader: Python 3.14+ compression.zstd, else the zstandard package."""
    try:
        from compression import zstd
        return zstd.ZstdFile(fileobj)
    except ImportError:
        pass
    try:
        import zstandard
        return zstandard.ZstdDecompressor().stream_reader(fileobj)
    except ImportError:
        raise Fail('unpacking the PGO tools needs Python 3.14 or newer (compression.zstd) or the zstandard package: '
                   'run setup.py with py -3.14 (or newer), or "py -m pip install zstandard"') from None


def ensure_pgo_tools(home):
    """deps\\clang64: the profile runtime and llvm-profdata, from the pinned MSYS2 packages (only these two files).
    With another clang (ULTRS_CLANG64, MSYS2) it says which packages to install instead."""
    if not (home / 'deps/clang64/bin/clang.exe').is_file():
        bin_ = toolchain.CLANG64
        if bin_ and not (bin_ / 'llvm-profdata.exe').is_file():
            raise Fail(f'PGO needs llvm-profdata and the profile runtime next to {bin_}: in MSYS2 run '
                       '"pacman -S mingw-w64-clang-x86_64-compiler-rt mingw-w64-clang-x86_64-llvm-tools"')
        return
    if not (home / 'deps/clang64/lib/clang/23').is_dir():
        raise Fail('PGO: the builder\'s clang is not version 23 (deps\\clang64\\lib\\clang\\23 missing), so the pinned '
                   'clang 23.1.3 profile runtime does not fit; install MSYS2 CLANG64 and set ULTRS_CLANG64')
    import tarfile
    for name, size, digest, members in PGO_PACKAGES:
        if all((home / 'deps' / m).is_file() for m in members):
            continue
        path = fetch_pinned(name, size, digest, MSYS2_CLANG64 + name, 'the PGO tools')
        want = set(members)
        with open(path, 'rb') as f, tarfile.open(fileobj=zstd_reader(f), mode='r|') as tf:
            for info in tf:
                if info.name in want and info.isfile():
                    dst = home / 'deps' / info.name
                    dst.parent.mkdir(parents=True, exist_ok=True)
                    with tf.extractfile(info) as src, open(dst.with_name(dst.name + '.part'), 'wb') as out:
                        shutil.copyfileobj(src, out)
                    dst.with_name(dst.name + '.part').replace(dst)
                    want.discard(info.name)
        if want:
            raise Fail(f'{name} did not contain {", ".join(sorted(want))}')
        say(f'  PGO tools: {", ".join(pathlib.PurePosixPath(m).name for m in members)} -> {home / "deps/clang64"}')


def speed_flags(a):
    """The speed options given on this run's command line, as {key: value}."""
    f = {}
    if a.pgo_profile or a.pgo or a.no_pgo:
        f['pgo'] = 'gen' if a.pgo_profile else 'use' if a.pgo else 'off'
    if a.lto or a.no_lto:
        f['lto'] = 'on' if a.lto else 'off'
    if a.O3 or a.O2:
        f['opt'] = 'O3' if a.O3 else 'O2'
    return f


def resolve_speed(state, flags):
    """[UIT] This run's speed options. Only the ones the player chose (a flag, now or on an earlier run: remembered in
    state['speed-chosen']) are kept; the rest follow SPEED_DEFAULT, so a new default reaches everyone who did not pick.
    One-time migration (state['speed-v'] < 2): setup.json from before saved every option on every run, so a saved
    value equal to the old default cannot be told from an explicit --no-lto / --O2; it counts as not chosen (the
    note says how to keep it off). A value different from the old default was certainly chosen and stays.
    state['lto-failed']: the ThinLTO link failed on this PC, so LTO stays off until --lto or --no-lto is given.
    Updates state; returns (speed, notes)."""
    saved = dict(state.get('speed') or {})
    chosen = set(state.get('speed-chosen') or [])
    notes = []
    v = state.get('speed-v', 1 if saved else SPEED_V)
    old = v < 2
    # [UIT 0148] speed-v 3: a --no-lto remembered from an earlier run is reset once (the 2026-10-30 21:08 log: a PC on
    # the slower build long after a one-off --no-lto). Not when this run gives --no-lto again, nor after a failed
    # ThinLTO link (that has its own state); a failing ThinLTO link falls back to the normal link by itself.
    relto = (v < 3 and not old and 'lto' in chosen and saved.get('lto') == 'off' and 'lto' not in flags
             and not state.get('lto-failed'))
    if relto:
        chosen.discard('lto')
        notes.append('ThinLTO is on again (faster game code): the --no-lto from an earlier run was reset once by this '
                     'update. This run compiles the game\'s C once more. To keep it off: py -3 tools\\setup.py --no-lto '
                     '(remembered from then on).')
    if old:
        chosen |= {k for k, v in saved.items() if k in SPEED_OLD_DEFAULT and v != SPEED_OLD_DEFAULT[k]}
    for k, v in flags.items():
        saved[k] = v
        chosen.add(k)
    if 'lto' in flags:
        state.pop('lto-failed', None)
    speed = dict(SPEED_DEFAULT)
    speed.update({k: saved[k] for k in chosen if k in saved and k in SPEED_DEFAULT})
    if state.get('lto-failed') and speed['lto'] == 'on':
        speed['lto'] = 'off'
        notes.append(f'ThinLTO stays off: its link failed on this PC ({state["lto-failed"]}); setup.py --lto tries it again')
    if old and saved.get('lto', 'off') == 'off' and speed['lto'] == 'on':
        notes.append('ThinLTO is now on by default (faster game code): this run compiles all of the game\'s C once '
                     'more (as long as a first build). To keep it off: setup.py --no-lto (remembered).')
    if speed['lto'] == 'off' and 'lto' in chosen and 'lto' not in flags and not state.get('lto-failed'):
        # [UIT 0142] --no-lto from an earlier run is remembered without a word on later runs, so a PC can stay on the
        # slower build long after a one-off test (the 2026-10-30 21:08 log: game thread 15.6 ms with LTO off by choice)
        notes.append('ThinLTO is off because setup.py --no-lto was given on an earlier run (remembered in setup.json). '
                     'It makes the game code faster; to turn it back on: py -3 tools\\setup.py --lto (remembered too).')
    state['speed'], state['speed-chosen'], state['speed-v'] = dict(speed), sorted(chosen), SPEED_V
    return speed, notes


def pgo_state(state, home=ROOT):
    """[UIT 0149] (step, saved sessions) for the launcher: 'normal', 'profiling' or 'fast', as pgo_guide reads it."""
    pgo = home / 'build/pgo'
    raws = len(list(pgo.glob('*.profraw'))) if pgo.is_dir() else 0
    mode = (state.get('speed') or {}).get('pgo', 'off')
    prof = state.get('pgo-profile')
    if mode == 'gen':
        return 'profiling', raws
    if mode == 'use' and prof and (home / prof).is_file():
        return 'fast', raws
    return 'normal', raws


def pgo_guide(state, home=ROOT):
    """[UIT] --pgo-guide: the three steps of profile-guided optimization in plain words, and which one is next."""
    pgo = home / 'build/pgo'
    raws = sorted(pgo.glob('*.profraw')) if pgo.is_dir() else []
    mode = (state.get('speed') or {}).get('pgo', 'off')
    prof = state.get('pgo-profile')
    have_prof = bool(prof) and (home / prof).is_file()
    lines = ['Profile-guided optimization (PGO): the compiler learns from your own play which code paths are hot and',
             'lays the game code out for them. Three steps, about 15-20 minutes of play plus two builds:',
             '  1. Profiling build:   py -3 tools\\setup.py --pgo-profile',
             '     (builds a slower exe that counts what runs; this compiles all of the game\'s C again)',
             '  2. Play 10+ minutes the way you normally do: explore, a fight, Ultrahand, the sky, a cave, a town.',
             '     Then close the game normally (the counts are saved every 30 s and at exit to build\\pgo\\).',
             '  3. Fast build:        py -3 tools\\setup.py --pgo',
             '     (merges the counts and builds the fast exe; later setup runs keep using it, --no-pgo goes back)',
             'You can repeat step 2 more than once before step 3; every session adds its counts.',
             '']
    if mode == 'gen':
        mb = sum(r.stat().st_size for r in raws) / 2**20
        lines.append(f'Now: the PROFILING build is installed (slower). {len(raws)} session file(s) saved so far ({mb:.0f} MiB).')
        lines.append('Next: ' + ('play (step 2), then run step 3.' if not raws else
                                 'play more if you like, or run step 3: py -3 tools\\setup.py --pgo'))
    elif mode == 'use' and have_prof:
        lines.append(f'Now: the fast PGO build is in use ({prof}). Nothing to do; after big updates you can redo steps 1-3.')
    else:
        lines.append('Now: a normal build (no PGO).' + (f' {len(raws)} saved session file(s) are waiting: step 3 uses them.'
                                                       if raws else ' Next: step 1.'))
    return '\n'.join(lines)


def speed_args(a):
    return [f for f, on in (('--pgo-profile', a.pgo_profile), ('--pgo', a.pgo), ('--no-pgo', a.no_pgo),
                            ('--lto', a.lto), ('--no-lto', a.no_lto), ('--O3', a.O3), ('--O2', a.O2)) if on]


def build_from_checkout(a):
    home = (a.output or ROOT / 'dist').resolve()
    try:
        changed, removed = sync_source(home)
        say(f'Ultra Instinct Tears source -> {home}: {changed} files updated, {removed} removed')
        install_deps(home)
        local_xlate = ROOT / 'dev-local/build/shaders/ultrs-shaders.exe'
        target_xlate = home / 'deps/shaders/ultrs-shaders.exe'
        if local_xlate.is_file() and not filecmp.cmp(local_xlate, target_xlate, shallow=False):
            target_xlate.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(local_xlate, target_xlate)
            say('  installed the locally built shader translator')
        if a.pgo_profile or a.pgo:
            ensure_pgo_tools(home)   # here: the user's Python (3.14+ unpacks .zst), not deps\\python
    except Fail as e:
        say(f'\nERROR preparing the build folder: {e}')
        return 1
    cmd = [str(home / 'deps/python/python.exe'), str(home / 'tools/setup.py'), '--in-place', '--cpus', str(a.cpus),
           '--dump', str(choose_dump(home, a.dump))]
    if a.only:
        cmd += ['--only', a.only]
    if a.warm_shaders:
        cmd += ['--warm-shaders']
    cmd += [f for f, on in (('--no-dlss', a.no_dlss), ('--require-dlss', a.require_dlss), ('--get-dlss', a.get_dlss)) if on]
    cmd += speed_args(a)
    env = dict(os.environ, PYTHONNOUSERSITE='1')
    if 'STREAMLINE_SDK' not in env and (ROOT / 'deps/streamline').is_dir() and not (home / 'deps/streamline').is_dir():
        env['STREAMLINE_SDK'] = str(ROOT / 'deps/streamline')   # the SDK extracted into the checkout's deps
    return subprocess.run(cmd, cwd=home, env=env).returncode


def sha256(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for block in iter(lambda: f.read(1 << 20), b''):
            h.update(block)
    return h.hexdigest()


def tree_hash(paths):
    """A stamp of source files: when it changes, the step that uses them runs again."""
    h = hashlib.sha256()
    for p in sorted(paths):
        h.update(str(p.relative_to(ROOT)).encode() + b'\0' + p.read_bytes())
    return h.hexdigest()[:16]


def run_rc(cmd):
    return subprocess.run([str(c) for c in cmd], cwd=ROOT).returncode


def run(cmd, what):
    t0 = time.time()
    rc = run_rc(cmd)
    if rc:
        raise Fail(f'{what} failed (exit code {rc}); the lines above say why')
    return time.time() - t0


def load_state():
    try:
        return json.loads(STATE.read_text())
    except (OSError, ValueError):
        return {}


def save_state(state):
    DATA.mkdir(exist_ok=True)
    STATE.write_text(json.dumps(state, indent=1))


# ---------------------------------------------------------------- steps
def step_check(dump, state):
    if sys.version_info < (3, 14):
        raise Fail(f'Python 3.14 or newer is needed (this is {sys.version.split()[0]})')
    for mod in ('lz4', 'capstone'):   # [UIT 0083] no unicorn: jump tables use tools/lifter/a64emu.py
        try:
            __import__(mod)
        except ImportError:
            raise Fail(f'the Python package {mod} is missing: python -m pip install lz4 capstone')
    toolchain.env()
    if toolchain.XLATE is None:
        raise Fail('the shader translator ultrs-shaders.exe is missing: build it with python tools/build_xlate.py '
                   '(needs the .NET 10 SDK) or take deps\\shaders from the builder package')
    check_dlss()
    if packed(state) and all(ok_module(DATA / 'exefs' / n, h) for n, h in EXEFS.items()):
        return 'Tears of the Kingdom 1.4.3, already taken in (data\\exefs, data\\romfs)'
    if not dump.is_dir():
        raise Fail(f'{dump} does not exist: put your extracted game there (game\\exefs and game\\romfs), '
                   'or give its folder with --dump')
    for p in list(dump.iterdir()) + list((dump / 'exefs').glob('*') if (dump / 'exefs').is_dir() else []):
        if p.suffix.lower() in ENCRYPTED:
            raise Fail(f'{p.name}: encrypted packages are not accepted. Give the extracted exefs and romfs folders '
                       '(this project decrypts nothing and reads no keys)')
    exefs, romfs = dump / 'exefs', dump / 'romfs'
    if not exefs.is_dir() or not romfs.is_dir():
        raise Fail(f'{dump} must hold the folders exefs and romfs (extracted, as plain files)')
    for name, want in EXEFS.items():
        f = exefs / name
        if not f.is_file():
            raise Fail(f'exefs\\{name} is missing')
        if sha256(f) != want:
            raise Fail(f'exefs\\{name} is not the one of Tears of the Kingdom 1.4.3. Extract the ExeFS again with '
                       'update 1.4.3 applied (the base game 1.0.0 and other updates are not supported)')
    if not (romfs / 'Pack/ZsDic.pack.zs').is_file():
        raise Fail(f'{romfs} does not look like the RomFS of Tears of the Kingdom (Pack\\ZsDic.pack.zs is missing)')
    return 'Tears of the Kingdom 1.4.3 (the code modules match)'


def packed(state):
    out = DATA / 'romfs'
    return state.get('romfs') == ROMFS_CONTENT and (out / 'romfs.bin').is_file() and (out / 'runtime-index.bin').is_file()


def ok_module(path, want):
    return path.is_file() and sha256(path) == want


def step_romfs(dump, state, cpus):
    out = DATA / 'romfs'
    if packed(state):
        return 'already packed'
    from pack_romfs import pack
    n, size, content = pack(dump / 'romfs', out, threads=16, progress=say)
    if n != ROMFS_FILES or size < ROMFS_BYTES or content != ROMFS_CONTENT:
        raise Fail(f'the RomFS is not the one of Tears of the Kingdom 1.4.3 ({n} files, expected {ROMFS_FILES}; '
                   'content differs). Extract the RomFS again with update 1.4.3 applied; mods do not go here')
    state['romfs'] = content
    return f'{n} files, {size / 2**30:.1f} GiB, content verified'


LIFT_OPTIONS = ('TOTK_LIFT_REGCACHE', 'TOTK_LIFT_REGCACHE_FLAGS', 'TOTK_LIFT_FPCR_HOST', 'TOTK_LIFT_SPIN_GATES', 'TOTK_LIFT_ZSTD', 'TOTK_LIFT_NATIVE_LIBC',
               'TOTK_LIFT_ISOLATE_O0', 'TOTK_LIFT_RESTRICT',
               'TOTK_LIFT_RESTRICT_ALL', 'TOTK_LIFT_REGCACHE_CLEAN', 'TOTK_LIFT_OTHER_TABLES',
               'TOTK_ROWCACHE')   # read when the code is translated, not by the game


def lift_options():
    """The lift-time TOTK_LIFT_* values: from the environment, else from ultrainstincttears.cfg (the launcher writes
    them there: Performance > CPU+). Only set ones are returned, so a build without any keeps its old stamp."""
    cfg = ROOT / 'ultrainstincttears.cfg'
    found = {}
    if cfg.is_file():
        for line in cfg.read_text(errors='replace').splitlines():
            k, sep, v = line.strip().partition('=')
            if sep and k.strip() in LIFT_OPTIONS:
                found[k.strip()] = v.strip()
    for k in LIFT_OPTIONS:
        if os.environ.get(k) not in (None, ''):
            found[k] = os.environ[k].strip()
    return found


def step_lift(dump, state, cpus):
    exefs = DATA / 'exefs'
    exefs.mkdir(parents=True, exist_ok=True)
    for name in list(EXEFS) + ['main.npdm']:   # the code modules are taken in, so the dump can go after the build
        src = dump / 'exefs' / name
        if src.is_file() and (not (exefs / name).is_file() or (exefs / name).stat().st_size != src.stat().st_size):
            shutil.copyfile(src, exefs / name)
    # diag/lift-confirm.txt: the game found which shadow-size site is the real one (src/host/uit_lift.c); the code is
    # lifted again so that site's patch goes in (Graphics > Detail and pop-in > Shadow resolution)
    confirm = ROOT / 'diag/lift-confirm.txt'
    stamp = tree_hash(sorted((ROOT / 'tools/lifter').glob('*.py')) + sorted((ROOT / 'tools/lifter').glob('*.txt')) +   # (*.txt: spin_waits_v143.txt, UIT 0077)
                      [ROOT / 'tools/gen_game.py'] +
                      ([confirm] if confirm.is_file() else []))
    opts = lift_options()   # lift-time settings (UIT 0078): changing one lifts the code again
    if opts:
        stamp += '|' + ','.join(f'{k}={v}' for k, v in sorted(opts.items()))
        os.environ.update(opts)
        say('  translation options: ' + ', '.join(f'{k}={v}' for k, v in sorted(opts.items())))
    os.environ['TOTK_LIFT_ROOT'] = str(ROOT)
    if state.get('lift') == stamp and (DATA / 'generated/main').is_dir():
        return 'up to date'
    run([sys.executable, 'tools/gen_game.py', exefs, DATA / 'generated'], 'translating the code')
    state['lift'] = stamp
    return 'done'


def streamline_dir():
    """The Streamline SDK the user extracted: %STREAMLINE_SDK%, deps\\streamline or one folder below it. Only its
    bin\\x64 DLLs are used (Ultra Instinct Tears has the C form of its headers, src/vk/sl_abi.h)."""
    dep = ROOT / 'deps' / 'streamline'
    for base in [os.environ.get('STREAMLINE_SDK'), dep] + (sorted(dep.glob('*/')) if dep.is_dir() else []):
        if base and any((pathlib.Path(base) / sub / 'sl.interposer.dll').is_file() for sub in ('bin/x64', '.')):
            return pathlib.Path(base)
    return None


def find_streamline_dlls(sdk):
    """name -> path of the production (signed) DLLs in the SDK's bin\\x64 (or the folder itself)."""
    found = {}
    for name in SL_DLLS + SL_OPTIONAL:
        for cand in [sdk / 'bin' / 'x64' / name, sdk / name] + sorted(sdk.glob(f'**/{name}')):
            low = [p.lower() for p in cand.parts]
            if cand.is_file() and 'development' not in low and 'debug' not in low and '_artifacts' not in low:
                found[name] = cand
                break
    return found


def pe_version(path):
    """The file version of a Windows DLL (its VS_FIXEDFILEINFO), or None."""
    try:
        data = path.read_bytes()
    except OSError:
        return None
    i = data.find((0xFEEF04BD).to_bytes(4, 'little'))
    if i < 0 or i + 16 > len(data):
        return None
    ms, ls = int.from_bytes(data[i + 8:i + 12], 'little'), int.from_bytes(data[i + 12:i + 16], 'little')
    return (ms >> 16, ms & 0xFFFF, ls >> 16)


def get_streamline():
    """--get-dlss: NVIDIA's Streamline SDK release zip (pinned) -> deps\\streamline: only its bin\\x64 DLLs, their
    licenses and sl.h/sl_version.h are unpacked (about 60 MB of the 276 MB)."""
    name, size, digest, url = SL_ZIP
    say(f'  --get-dlss: NVIDIA Streamline SDK {SL_VERSION} from {SL_RELEASE} (NVIDIA\'s licenses: deps\\streamline)')
    archive = fetch_pinned(name, size, digest, url, 'the Streamline SDK')
    dest = ROOT / 'deps' / 'streamline'
    try:
        with zipfile.ZipFile(archive) as zf:
            for member in SL_ZIP_KEEP:
                dst = dest / member
                info = zf.getinfo(member)
                if dst.is_file() and dst.stat().st_size == info.file_size:
                    continue
                dst.parent.mkdir(parents=True, exist_ok=True)
                with zf.open(info) as src, open(dst, 'wb') as out:
                    shutil.copyfileobj(src, out)
    except (OSError, KeyError, zipfile.BadZipFile) as e:
        raise Fail(f'could not unpack {archive}: {e}') from e
    say(f'  Streamline {SL_VERSION} DLLs in {dest}\\bin\\x64')


def dlss_banner(why):
    """Loud: the build goes on with FSR only, and how to get DLSS."""
    lines = [f'DLSS WILL NOT RUN YET: {why}',
             'UltraInstinctTears.exe has DLSS compiled in, but it needs NVIDIA\'s Streamline DLLs next to it; until they are',
             'there TOTK_UPSCALER=dlss (and the launcher\'s DLSS modes) run FSR instead. To enable DLSS, either:',
             '  a) run setup.py again with --get-dlss, e.g.   py -3.15 tools\\setup.py --get-dlss',
             f'     (downloads streamline-sdk-v{SL_VERSION}.zip, 276 MB, from NVIDIA\'s GitHub release once, checks its',
             '     SHA-256, unpacks its DLLs to deps\\streamline and copies them next to UltraInstinctTears.exe), or',
             f'  b) download streamline-sdk-v{SL_VERSION}.zip from {SL_RELEASE}',
             f'     and extract it to {ROOT}\\deps\\streamline (so that deps\\streamline\\bin\\x64\\',
             '     sl.interposer.dll exists), then run setup.py again; it only copies the DLLs (nothing is recompiled).',
             'Build options: --require-dlss makes this an error, --no-dlss builds without DLSS and hides this.']
    bar = '  ' + '=' * 100
    say('\n'.join([bar] + ['  ' + l for l in lines] + [bar]))


def check_dlss():
    """Early, before the long steps: is DLSS going to work, and what to do if not."""
    if DLSS_MODE == 'off':
        say('  DLSS: off (--no-dlss): stubs are built, TOTK_UPSCALER=dlss runs FSR')
        return
    sdk = streamline_dir()
    found = find_streamline_dlls(sdk) if sdk else {}
    missing = [n for n in SL_DLLS if n not in found]
    if sdk and not missing:
        v = pe_version(found['sl.interposer.dll'])
        shown = '.'.join(map(str, v)) if v else 'unknown version'
        say(f'  DLSS: Streamline {shown} in {sdk}: its DLLs go next to UltraInstinctTears.exe')
        if v and v[:2] != tuple(int(x) for x in SL_VERSION.split('.')[:2]):
            say(f'  DLSS: note: Ultra Instinct Tears is made for Streamline {SL_VERSION}; if DLSS does not start, '
                f'TOTK_DLSS_LOG=2 logs why (slInit), and {SL_RELEASE} has the expected version')
        return
    in_folder = all((ROOT / n).is_file() for n in SL_DLLS)
    if in_folder:
        say('  DLSS: the Streamline DLLs are already next to UltraInstinctTears.exe (no SDK in deps\\streamline: kept as they are)')
        return
    why = (f'{", ".join(missing)} not found in {sdk}' if sdk
           else f'NVIDIA\'s Streamline SDK {SL_VERSION} is not in {ROOT}\\deps\\streamline')
    if DLSS_MODE == 'require':
        raise Fail(f'--require-dlss: {why}. Run setup.py with --get-dlss, or download streamline-sdk-v{SL_VERSION}.zip '
                   f'from {SL_RELEASE} and extract it to {ROOT}\\deps\\streamline (or set STREAMLINE_SDK to its folder)')
    dlss_banner(why)


def copy_streamline(sdk):
    """The DLLs DLSS needs, from the SDK's bin\\x64 (production, signed) to UltraInstinctTears.exe's folder."""
    found = find_streamline_dlls(sdk)
    missing = [n for n in SL_DLLS if n not in found]
    for name, src in found.items():
        dst = ROOT / name
        if not (dst.is_file() and filecmp.cmp(src, dst, shallow=False)):
            shutil.copy2(src, dst)
    return missing


def pgo_merge(state):
    """build\\pgo\\*.profraw (the profiling build's counts, one file per game session) -> build\\pgo\\uit-<hash>.profdata.
    The name carries the content hash, so a new profile changes the compile commands and ninja rebuilds with it."""
    pgo = ROOT / 'build/pgo'
    raws = sorted(pgo.glob('*.profraw'))
    key = hashlib.sha256(repr([(r.name, r.stat().st_size, r.stat().st_mtime_ns) for r in raws]).encode()).hexdigest()
    old = state.get('pgo-profile')
    if old and (ROOT / old).is_file() and (state.get('pgo-key') == key or not raws):
        return old
    if not raws:
        raise Fail('--pgo: no build\\pgo\\*.profraw yet: build with --pgo-profile first, play 10+ minutes, '
                   'close the game, then run --pgo')
    prof = toolchain.CLANG64 / 'llvm-profdata.exe'
    if not prof.is_file():
        raise Fail(f'{prof} is missing: run setup.py --pgo from the Git checkout with Python 3.14+ (it downloads it)')
    mb = sum(r.stat().st_size for r in raws) / 2**20
    say(f'  PGO: merging {len(raws)} profile file(s) ({mb:.0f} MiB)')
    part = pgo / 'uit.profdata.part'
    # --failure-mode=all: a damaged file is skipped with a warning (only all of them failing is an error)
    run([prof, 'merge', '--failure-mode=all', '-o', part] + raws, 'merging the PGO profile')
    out = pgo / f'uit-{sha256(part)[:12]}.profdata'
    part.replace(out)
    for f in pgo.glob('uit-*.profdata'):
        if f != out:
            f.unlink(missing_ok=True)
    rel = out.relative_to(ROOT).as_posix()
    state['pgo-profile'], state['pgo-key'] = rel, key
    say(f'  PGO: profile {rel}')
    return rel


O0_TRY_SECONDS = 240


def step_compile(dump, state, cpus):
    no_dlss = ['--no-dlss'] if DLSS_MODE == 'off' else []
    speed = ['--opt', SPEED['opt'], '--lto', 'thin' if SPEED['lto'] == 'on' else 'off']
    if SPEED['pgo'] == 'gen':
        speed += ['--pgo', 'gen']
    elif SPEED['pgo'] == 'use':
        speed += ['--pgo', 'use', '--profile', pgo_merge(state)]
    cmd = [sys.executable, 'tools/build_game.py', '--generated', DATA / 'generated', '--target', 'ultratears', '--cpus', cpus] + no_dlss
    # [UIT 0359] Performance > CPU+ > Try to optimize the unoptimized game files: those files at -O1 first, at most
    # O0_TRY_SECONDS each, else -O0 as before (tools/cc_try.py, which remembers a file that took too long)
    if warm_shaders.setting(ROOT, 'TOTK_SETUP_O0_TRY') in ('1', 'on'):
        cmd += ['--o0-try', str(O0_TRY_SECONDS)]
        say(f'  trying the files that are normally unoptimized at -O1 (at most {O0_TRY_SECONDS // 60} minutes each)')
    rc = run_rc(cmd + speed)
    fell_back = False
    if rc == LTO_LINK_FAILED and SPEED['lto'] == 'on':
        # [UIT] the ThinLTO link failed twice (build_game.py): build again with the normal link, by itself
        say('  ThinLTO link FAILED (the lines above say why). Building again with the normal link instead: this '
            'compiles all of the game\'s C once more (as long as a first build). Later runs stay on the normal '
            'link; setup.py --lto tries ThinLTO again.')
        SPEED['lto'] = 'off'
        state.setdefault('speed', {})['lto'] = 'off'
        state['lto-failed'] = time.strftime('%Y-%m-%d %H:%M')
        save_state(state)
        speed[speed.index('--lto') + 1] = 'off'
        run(cmd + speed, 'compiling with the normal link (after the ThinLTO link failed)')
        fell_back = True
    elif rc:
        raise Fail(f'compiling failed (exit code {rc}); the lines above say why')
    # the settings launcher (tools/build_launcher.py): a few seconds; the game works without it, so a failure only warns
    # [UIT 0150] setup.py run from the launcher (About > Faster game code (PGO)): Windows lets the running launcher .exe be
    # renamed, not overwritten, so move it aside first (as tools/update.py does); a copy stays if the link fails
    exe = ROOT / 'UltraInstinctTearsLauncher.exe'
    if os.name == 'nt' and exe.is_file():
        old = exe.with_name('UltraInstinctTearsLauncher.old.exe')
        try:
            old.unlink(missing_ok=True)
        except OSError:   # the .old.exe is the one running (moved aside earlier), so the .exe is free to overwrite
            old = None
        try:
            if old:
                os.replace(exe, old)
                shutil.copy2(old, exe)
        except OSError as e:
            say(f'  note: could not move the launcher aside ({e}); its rebuild may fail, the game is unaffected')
    launcher = subprocess.run([sys.executable, 'tools/build_launcher.py', '-j', str(cpus)], cwd=ROOT).returncode == 0
    if not launcher:
        say('  WARNING: UltraInstinctTearsLauncher.exe did not build (the lines above say why); '
            'UltraInstinctTears.exe and ultrainstincttears.cfg work without it')
    extra = ' + UltraInstinctTearsLauncher.exe' if launcher else ' (no launcher: see the warning above)'
    if SPEED['pgo'] == 'gen':
        extra += ('. PROFILING BUILD (slower): play 10+ minutes as usual, close the game, then run setup.py --pgo '
                  'for the fast build')
    elif SPEED != SPEED_OLD_DEFAULT:
        extra += f' (speed: {SPEED["opt"]}, LTO {SPEED["lto"]}, PGO {SPEED["pgo"]})'
    if fell_back:
        extra += '. NOTE: built with the normal link because the ThinLTO link failed (see above)'
    if DLSS_MODE == 'off':
        return f'UltraInstinctTears.exe{extra}, FSR only (--no-dlss)'
    sdk = streamline_dir()
    missing = copy_streamline(sdk) if sdk else list(SL_DLLS)
    if not missing or all((ROOT / n).is_file() for n in SL_DLLS):
        return f'UltraInstinctTears.exe{extra} with DLSS (Streamline {SL_VERSION} DLLs next to it)'
    if DLSS_MODE == 'require':
        raise Fail(f'--require-dlss: {", ".join(missing)} missing (see the check step)')
    state['dlss-missing'] = True
    return f'UltraInstinctTears.exe{extra}; DLSS compiled in but NOT ACTIVE: Streamline DLLs missing, FSR is used (see the check step)'


def step_shaders(dump, state, cpus):
    out = DATA / 'shaders'
    stamp = tree_hash([ROOT / 'tools/shaders/inventory.py', ROOT / 'tools/export_shaders.py'])
    if state.get('shaders-export') != stamp or not (out / 'jobs.jsonl').is_file():
        run([sys.executable, 'tools/export_shaders.py', DATA / 'romfs', out, DATA / 'exefs'], 'finding the shaders')
        state['shaders-export'] = stamp
        save_state(state)
    xlate = sha256(toolchain.XLATE)[:16]
    new_only = ['--new-only'] if state.get('shaders-xlate') == xlate else []
    run([sys.executable, 'tools/run_ryujinx_xlate.py', out, '--workers', min(cpus, 32), '--quiet'] + new_only,
        'translating the shaders')
    state['shaders-xlate'] = xlate
    return 'done'


def step_textures(dump, state, cpus):
    run([sys.executable, 'tools/bc7_build.py', '--romfs', DATA / 'romfs', '--cache', DATA / 'textures', '--threads',
         min(cpus, 64)], 're-encoding the textures')
    return 'done'


def old_exes_note():
    """The exes from before the rename to Ultra Instinct Tears are left where they are: say they can go."""
    old = [n for n in OLD_EXES if (ROOT / n).is_file()]
    if old:
        say(f'  note: {" and ".join(old)} {"are" if len(old) > 1 else "is"} from before the rename to Ultra Instinct Tears '
            f'(now {GAME_EXE} and {LAUNCHER_EXE}); {"they are" if len(old) > 1 else "it is"} no longer built or used '
            'and can be deleted')


def step_finish(dump, state, cpus):
    fonts = dump / 'sysdata/fonts'
    lines = ['# Written by tools/setup.py: where Ultra Instinct Tears finds its data (relative to this folder).',
             'exefs=data/exefs', 'romfs=data/romfs', 'shaders=data/shaders/ryujinx', 'textures=data/textures',
             'saves=userdata']
    if fonts.is_dir() and list(fonts.glob('*.bfttf')):
        shutil.copytree(fonts, DATA / 'fonts', dirs_exist_ok=True)
    lines.append('fonts=data/fonts')
    cfg = ROOT / 'ultrainstincttears.cfg'   # the user's own TOTK_* lines (TOTK_FPS, TOTK_FSR, ...) survive a rerun
    old_cfg = ROOT / OLD_CFG
    if not cfg.is_file() and old_cfg.is_file():   # the name before the rename: its settings carry over, the file stays
        shutil.copy2(old_cfg, cfg)
        say(f'  settings: copied {OLD_CFG} to {cfg.name} (your values are kept; {OLD_CFG} stays as a backup and is '
            'no longer read)')
    own = [l.strip() for l in cfg.read_text(errors='replace').splitlines() if l.strip().startswith('TOTK_')] \
        if cfg.is_file() else []
    if own:
        lines += ['# Your settings (kept when tools/setup.py runs again):'] + own
    cfg.write_text('\n'.join(lines) + '\n')
    (ROOT / 'build/UltraTears.exe').unlink(missing_ok=True)   # where builds before 0.1.0 linked it (the old name)
    # Only the dump's own exefs\\ and romfs\\ can go, and only when the dump folder holds nothing of the build: a dump
    # given as the build folder itself (or a folder above it, or data\\) is never offered for deletion.
    if dump_is_separate(dump) and (dump / 'romfs').is_dir():
        say(f'  The game reads data\\romfs and data\\exefs now: {dump / "exefs"} and {dump / "romfs"} (16 GB) are '
            'no longer needed.')
        if sys.stdin is not None and sys.stdin.isatty() and dump.resolve() == (ROOT / 'game').resolve():
            answer = input('  Delete game\\exefs and game\\romfs to free 16 GB? [y/N] ').strip().lower()
            if answer in ('y', 'yes', 'д', 'да'):
                for sub in ('exefs', 'romfs'):
                    shutil.rmtree(dump / sub, ignore_errors=True)
                say('  deleted')
    old_exes_note()
    return 'ultrainstincttears.cfg ready'


def dump_is_separate(dump):
    """True when the dump folder is neither the build folder (ROOT) nor data\\, and contains neither of them nor is
    inside data\\: only then is it safe to say its exefs\\ and romfs\\ are not needed any more."""
    try:
        d = dump.resolve()
    except OSError:
        return False
    for keep in (ROOT.resolve(), DATA.resolve()):
        if d == keep or keep.is_relative_to(d):
            return False
    return not d.is_relative_to(DATA.resolve())


def rewarm(state, asked):
    """[UIT 0081] Make the shaders you have seen again after an update or a rebuild (tools/warm_shaders.py), so the
    first start afterwards does not wait for them. asked: --warm-shaders; else only when the game or its shaders
    changed since the last warm-up and Performance > Stutter > Shaders+ > "Warm up shaders after an update" is on."""
    now = warm_shaders.stamp(ROOT, GAME_EXE, state)
    stale = state.get('warm-stamp') != now
    n = warm_shaders.listed(ROOT)
    if not asked:
        if not stale or not n:
            return
        if warm_shaders.setting(ROOT, 'TOTK_SETUP_REWARM') not in ('1', 'on'):
            say(f'  tip: the game or its shaders changed; tools/setup.py --warm-shaders makes your {n} shaders now '
                'instead of at the next start (or turn on Performance > Stutter > Shaders+ > Warm up shaders after an update)')
            return
    say('[warm] making the shaders you have seen (the game opens its window and closes it when done)')
    if not (ROOT / GAME_EXE).is_file():
        say(f'  {GAME_EXE} is not built yet: run tools/setup.py first')
        return
    if not n:
        say('  nothing to make yet: the list of shaders you have seen fills as you play')
        return
    if warm_shaders.setting(ROOT, 'TOTK_SHADER_PREWARM') == '0':
        say('  shader warm-up is off (Performance > Stutter > Shaders > Shader warm-up): nothing to do')
        return
    t0 = time.time()
    ok, msg = warm_shaders.run([ROOT / GAME_EXE], ROOT, say, ROOT / 'build/warm-shaders.log')
    say(f'  {msg} ({time.time() - t0:.0f} s)')
    if ok:
        state['warm-stamp'] = now
        save_state(state)


STEPS = [('check', step_check), ('romfs', step_romfs), ('lift', step_lift), ('compile', step_compile),
         ('shaders', step_shaders), ('textures', step_textures), ('finish', step_finish)]


def main():
    try:   # [UIT 0055] no console (run by the launcher's updater): missing std handles point at NUL
        from update import std_streams
        std_streams()
    except Exception:   # noqa: BLE001
        pass
    ap = argparse.ArgumentParser(description='Build Ultra Instinct Tears from your own dump of the game.')
    ap.add_argument('--output', type=pathlib.Path,
                    help='build folder when run from a Git checkout (default: dist here)')
    ap.add_argument('--dump', type=pathlib.Path,
                    help='the folder with exefs\\ and romfs\\ (default: game\\ here; remembered for later runs)')
    ap.add_argument('--cpus', type=int, default=os.cpu_count() or 4,
                    help='logical CPUs the build may use (default: all; it runs at low priority)')
    ap.add_argument('--only', choices=[s for s, _ in STEPS], help='run one step only')
    ap.add_argument('--get-dlss', action='store_true',
                    help=f'download NVIDIA\'s Streamline SDK {SL_VERSION} (276 MB) to deps\\streamline for DLSS')
    mode = ap.add_mutually_exclusive_group()
    mode.add_argument('--require-dlss', action='store_true', help='stop with an error if DLSS cannot be enabled')
    mode.add_argument('--no-dlss', action='store_true', help='build without DLSS (FSR only; no Streamline needed)')
    pgo = ap.add_mutually_exclusive_group()
    pgo.add_argument('--pgo-profile', action='store_true', help='PGO step 1: build the profiling exe (then play 10+ minutes)')
    pgo.add_argument('--pgo', action='store_true', help='PGO step 2: merge the counts and build the fast exe (remembered)')
    pgo.add_argument('--no-pgo', action='store_true', help='back to a build without PGO')
    ap.add_argument('--pgo-guide', action='store_true', help='print the PGO steps and which one is next (builds nothing)')
    ap.add_argument('--pgo-tsv', action='store_true', help=argparse.SUPPRESS)   # [UIT 0149] + a pgo-state line (launcher)
    lto = ap.add_mutually_exclusive_group()
    lto.add_argument('--lto', action='store_true', help='ThinLTO (remembered)')
    lto.add_argument('--no-lto', action='store_true', help='no ThinLTO')
    opt = ap.add_mutually_exclusive_group()
    opt.add_argument('--O3', action='store_true', help='-O3 instead of -O2 (remembered)')
    opt.add_argument('--O2', action='store_true', help='-O2 (the default)')
    ap.add_argument('--warm-shaders', action='store_true',
                    help='only make the shaders you have seen, ahead of the next start (after an update or a rebuild); '
                         'the game opens its window and closes it when done')
    ap.add_argument('--in-place', action='store_true', help=argparse.SUPPRESS)
    a = ap.parse_args()
    if a.get_dlss and a.no_dlss:
        ap.error('--get-dlss and --no-dlss contradict each other')
    global DLSS_MODE
    DLSS_MODE = 'off' if a.no_dlss else 'require' if a.require_dlss else 'auto'
    if a.pgo_guide:   # reads data\\setup.json and build\\pgo\\ of the build folder; builds and downloads nothing
        home = (a.output or ROOT / 'dist').resolve() if is_checkout() and not a.in_place else ROOT
        try:
            st = json.loads((home / 'data/setup.json').read_text())
        except (OSError, ValueError):
            st = {}
        say(pgo_guide(st, home))
        if a.pgo_tsv:
            say('pgo-state\t%s\t%d' % pgo_state(st, home))
        sys.exit(0)
    if is_checkout() and not a.in_place:
        sys.exit(build_from_checkout(a))
    if a.output:
        ap.error('--output is used only when setup.py is run from a Git checkout')
    if not deps_ready(ROOT):
        try:
            install_deps(ROOT)
        except Fail as e:
            say(f'\nERROR preparing the build tools: {e}')
            sys.exit(1)
        importlib.reload(toolchain)
    state = load_state()
    if a.dump:
        state['dump'] = str(a.dump.resolve())
    if not state.get('dump'):
        state['dump'] = str(ROOT / 'game')
    dump = pathlib.Path(state['dump'])
    speed, notes = resolve_speed(state, speed_flags(a))
    if speed['pgo'] == 'use' and not (ROOT / str(state.get('pgo-profile', '-'))).is_file() and not a.pgo:
        speed['pgo'] = 'off'   # the remembered profile is gone (build\\ deleted)
        state['speed']['pgo'] = 'off'
    SPEED.update(speed)
    for n in notes:
        say(f'NOTE: {n}')
    if speed['pgo'] != 'off':
        try:
            ensure_pgo_tools(ROOT)
        except Fail as e:
            say(f'\nERROR preparing the PGO tools: {e}')
            sys.exit(1)
    say(f'Ultra Instinct Tears setup: dump {dump}, {a.cpus} CPUs, DLSS {DLSS_MODE}, speed {speed["opt"]} / LTO {speed["lto"]} / PGO {speed["pgo"]}')
    t_all = time.time()
    state.pop('dlss-missing', None)
    if a.get_dlss:
        try:
            get_streamline()
        except Fail as e:
            say(f'\nERROR getting the Streamline SDK: {e}')
            sys.exit(1)
    if a.warm_shaders:   # [UIT 0081] the warm-up only: nothing is built
        rewarm(state, True)
        return
    for i, (name, fn) in enumerate(STEPS, 1):
        if a.only and name != a.only and name != 'check':
            continue
        say(f'[{i}/{len(STEPS)}] {name}')
        t0 = time.time()
        try:
            result = fn(dump, state) if name == 'check' else fn(dump, state, a.cpus)
        except Fail as e:
            say(f'\nERROR in step "{name}": {e}')
            save_state(state)
            sys.exit(1)
        save_state(state)
        say(f'  {result or "ok"} ({time.time() - t0:.0f} s)')
    rewarm(state, False)
    say(f'\nDone in {(time.time() - t_all) / 60:.0f} min. Start the game: UltraInstinctTearsLauncher.exe (settings) or UltraInstinctTears.exe in {ROOT}')
    if state.pop('dlss-missing', None):
        save_state(state)
        dlss_banner('the Streamline DLLs are not next to UltraInstinctTears.exe (see the check step above)')


if __name__ == '__main__':
    main()
