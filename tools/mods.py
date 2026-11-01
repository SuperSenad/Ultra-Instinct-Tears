"""Install and remove RomFS mods for Ultra Instinct Tears (the launcher's Mods tab runs this; it works by hand too).

    py -3 tools/mods.py install SOURCE... [--mods DIR] [--name NAME] [--progress FILE]
    py -3 tools/mods.py inbox [--inbox DIR] [--mods DIR] [--progress FILE]
    py -3 tools/mods.py remove NAME... [--mods DIR]

SOURCE is a .zip, .7z or .rar (also .tar/.tar.gz/.tgz), or a folder. Nothing is downloaded: only the files given are
read, and a folder given is copied, never moved. What install does:
  read      list the archive; refuse an entry that would land outside the mod folder (absolute, drive letter, "..").
            A .zip is read by Python; .7z / .rar by Windows' own tar.exe (bsdtar), else 7-Zip's 7z.exe when installed
  extract   into <mods>/.inst-<id>/ (a dot folder: the game and the launcher never read it); a damaged or
            incomplete archive stops here with a plain message and nothing changes. An archive whose only content is
            another archive is opened too
  detect    where the mod's files are: <Name>/romfs/..., romfs/... at the top, any depth (atmosphere/contents/<title>/
            romfs, mods/<Name>/romfs, ...), or the game's own folders (Actor, Pack, UI, ...) without a romfs folder. Several
            mods in one archive (variants) are installed as several mods; when they replace the same files only the
            first is turned on. An archive with only exefs code patches (.ips/.pchtxt/NSO) or cheats is refused: those
            cannot load here (FPS and resolution are built-in settings)
  place     <mods>/<Name>/romfs/... (plus info.json, readme and the rest of the mod's own folder). A mod already
            installed under that name is replaced in place: its place in the load order and on/off stay as they were
  order     a new mod goes on top of load-order.txt (highest priority) and is turned on
Output: one tab-separated line per result for the launcher:
  installed <TAB> name <TAB> files <TAB> bytes <TAB> new|updated|off <TAB> note
  error <TAB> source <TAB> message
  removed <TAB> name
and --progress FILE gets {"state","step","pct","msg"} as JSON while it runs.
inbox installs everything in mods-inbox/ (next to the mods folder; for a controller: copy a download there, then press
Install in the launcher), oldest first so the newest ends on top, then moves each item to mods-inbox/installed/ (or
mods-inbox/failed/ with a .txt that says why).
"""
import argparse, json, os, pathlib, re, shutil, stat, subprocess, sys, time, unicodedata, zipfile

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

ROOT = pathlib.Path(__file__).resolve().parent.parent
ARCHIVES = ('.zip', '.7z', '.rar', '.tar', '.tgz', '.tar.gz', '.tar.xz', '.tar.bz2')
GAME_DIRS = {d.lower() for d in (
    'Actor', 'AI', 'AnimationEvent', 'AS', 'Banc', 'Cave', 'Collision', 'Cooking', 'Effect', 'Event', 'EventFlow', 'Font',
    'GameData', 'Gyml', 'Layout', 'LevelSensor', 'Logic', 'Mals', 'Material', 'Model', 'Movie', 'Pack', 'Phive', 'Physics',
    'Preset', 'RSDB', 'Sequence', 'Shader', 'Sound', 'Speech', 'System', 'Tex', 'TexToGo', 'UI', 'Voice', 'XLink')}  # = modfs.c
GENERIC = {'romfs', 'exefs', 'cheats', 'contents', 'atmosphere', 'titles', 'mods', 'sdmc', 'switch', 'load', 'mod', 'files',
           'the legend of zelda tears of the kingdom', 'tears of the kingdom', 'totk', 'zelda totk',
           # emulator and platform folders a mod page sorts its layouts into ("Mod/Yuzu/romfs", "Mod/Ryujinx/...")
           'yuzu', 'ryujinx', 'suyu', 'sudachi', 'citron', 'eden', 'torzu', 'emulator', 'emulators', 'pc', 'console',
           'ultra instinct tears', 'ultratears', 'uit'}
TITLE_ID = re.compile(r'^0100[0-9a-f]{12}$', re.I)
VERSION_DIR = re.compile(r'^(v|ver|version)?[ ._-]?\d+([._]\d+|[._]x)*$', re.I)   # "1.4.x", "v1.2.1", "143": a game version folder
CODE_EXT = ('.ips', '.pchtxt', '.nso')
CODE_NAMES = {'main', 'main.npdm', 'rtld', 'sdk', *(f'subsdk{i}' for i in range(10))}
CODE_DIRS = {'exefs', 'exefs_patches', 'nro_patches'}
NOT_ROMFS = ('.txt', '.md', '.json', '.png', '.jpg', '.jpeg', '.webp', '.gif', '.bmp', '.url', '.html', '.htm', '.pdf',
             '.ini', '.nfo', '.rtf', '.doc', '.docx', '.ini', '.log', '.dll', '.exe', '.bat', '.cmd', '.ps1', '.sh', '.py')
RESERVED = {'con', 'prn', 'aux', 'nul', *(f'com{i}' for i in range(10)), *(f'lpt{i}' for i in range(10))}
HEADER = ('# Ultra Instinct Tears mods, highest priority first: a file in a mod higher up wins.\n'
          '# + on, - off. The launcher writes this file; a mod folder not listed here is on and goes on top.\n')   # = modfs.c
MAX_DEPTH = 8


class Fail(Exception):
    pass


def junk(name):            # = modfs.c mf_junk
    n = name.lower()
    return name.startswith('.') or n in ('desktop.ini', 'thumbs.db', '__macosx')


def is_archive(p):
    n = p.name.lower()
    return p.is_file() and any(n.endswith(e) for e in ARCHIVES)


def stem(p):
    n = p.name
    for e in sorted(ARCHIVES, key=len, reverse=True):
        if n.lower().endswith(e): return n[:-len(e)]
    return n


def clean_name(s):
    """A folder name that works everywhere: ASCII (the launcher's font), no characters Windows refuses, none that
    load-order.txt reads as a marker (+ - #), not a reserved device name."""
    s = unicodedata.normalize('NFKD', s).encode('ascii', 'ignore').decode()
    s = re.sub(r'[<>:"/\\|?*\x00-\x1f]', ' ', s).replace('_', ' ')
    s = re.sub(r'\s+', ' ', s).strip().lstrip('+-#. ').rstrip('. ')
    s = s[:64].rstrip('. ')
    if s.lower() in RESERVED: s += ' mod'
    return s or 'Mod'


def rmtree(p, ignore=False):
    """shutil.rmtree that also removes read-only files (archives often carry the Windows read-only attribute)."""
    def fix(fn, path, *_):
        try:
            os.chmod(path, stat.S_IWRITE); fn(path)
        except OSError:
            if not ignore: raise
    if sys.version_info >= (3, 12): shutil.rmtree(p, onexc=fix)
    else: shutil.rmtree(p, onerror=fix)


def human(n):
    return f'{n / (1 << 30):.1f} GB' if n >= 1 << 30 else f'{n / (1 << 20):.1f} MB' if n >= 1 << 20 else f'{n / 1024:.0f} KB'


def cfg_value(key):
    cfg = ROOT / 'ultrainstincttears.cfg'
    if cfg.is_file():
        for line in cfg.read_text(errors='replace').splitlines():
            k, _, v = line.strip().partition('=')
            if k.strip() == key and v.strip() not in ('', '-'):
                return v.strip()
    return ''


def mods_dir(arg):
    d = pathlib.Path(arg or cfg_value('TOTK_MODS_DIR') or 'mods')
    return d if d.is_absolute() else ROOT / d


class Progress:
    def __init__(self, path):
        self.path = pathlib.Path(path) if path else None
        self.state = {'state': 'running', 'step': '', 'pct': 0, 'msg': ''}
        self.out = ProgressFile(path, note=lambda t: print(t, file=sys.stderr, flush=True))

    def __call__(self, step=None, pct=None, msg=None, state=None):
        if step is not None: self.state['step'] = step
        if pct is not None: self.state['pct'] = max(0, min(100, int(pct)))
        if msg is not None: self.state['msg'] = msg
        if state is not None: self.state['state'] = state
        self.out.write(self.state)   # [UIT 0046] never fails the task (retries, then skips one update)


def tsv(*f):
    print('\t'.join(str(x).replace('\t', ' ').replace('\n', ' ').replace('\r', '') for x in f), flush=True)


# ---------------------------------------------------------------- load-order.txt (the same rules as modfs.c)
def mod_folders(md):
    try:
        return sorted((p.name for p in md.iterdir() if p.is_dir() and not junk(p.name)), key=lambda n: n.encode())
    except OSError:
        return []


def read_order(md):
    """[(name, on)] in effect: unlisted folders first (by name, on), then the file's order; missing folders dropped."""
    listed = []
    try:
        for line in (md / 'load-order.txt').read_text(encoding='utf-8', errors='replace').splitlines():
            s = line.lstrip('\ufeff').strip()
            if not s or s.startswith('#'): continue
            on = True
            if s[0] in '+-': on, s = s[0] == '+', s[1:].strip()
            if s: listed.append((s, on))
    except OSError:
        pass
    have = mod_folders(md)
    names = {n for n, _ in listed}
    out = [(n, True) for n in have if n not in names]
    seen = set()
    for n, on in listed:
        if n in have and n not in seen: out.append((n, on)), seen.add(n)
    return out


def write_order(md, order):
    tmp = md / 'load-order.txt.new'
    tmp.write_text(HEADER + ''.join(f'{"+" if on else "-"}{n}\n' for n, on in order), encoding='utf-8', newline='\n')
    os.replace(tmp, md / 'load-order.txt')


# ---------------------------------------------------------------- reading archives
def safe_rel(name):
    """An archive entry name as a relative path, or None when it would land outside the folder."""
    n = name.replace('\\', '/')
    while n.startswith('./'): n = n[2:]
    if not n or n.startswith('/') or re.match(r'^[A-Za-z]:', n): return None if n else ''
    parts = [p for p in n.split('/') if p not in ('', '.')]
    if any(p == '..' for p in parts): return None
    return '/'.join(parts)


def find_tool(kind):
    """Windows' own tar.exe (bsdtar reads 7z and rar), 7-Zip's 7z.exe. UIT_MODS_TAR / UIT_MODS_7Z override (tests)."""
    env = os.environ.get('UIT_MODS_TAR' if kind == 'tar' else 'UIT_MODS_7Z')
    if env: return env if env != '-' else None
    if kind == 'tar':
        if os.name == 'nt':
            t = pathlib.Path(os.environ.get('SystemRoot', r'C:\Windows')) / 'System32' / 'tar.exe'
            return str(t) if t.is_file() else None
        return shutil.which('bsdtar')         # GNU tar cannot read 7z / rar
    for c in ([pathlib.Path(os.environ.get(v, '')) / '7-Zip' / '7z.exe' for v in ('ProgramFiles', 'ProgramFiles(x86)', 'ProgramW6432')]
              if os.name == 'nt' else []):
        if c.is_file(): return str(c)
    return shutil.which('7z') or shutil.which('7zz') or shutil.which('7za')


def run(cmd):
    try:
        r = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=3600,
                           creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
    except (OSError, subprocess.TimeoutExpired) as e:
        return 1, '', str(e)
    return r.returncode, r.stdout.decode(errors='replace'), r.stderr.decode(errors='replace')


def last_line(t):
    ls = [l.strip() for l in t.splitlines() if l.strip()]
    return ls[-1][:200] if ls else ''


def extract_zip(src, dest, say):
    try:
        z = zipfile.ZipFile(src)
    except zipfile.BadZipFile:
        raise Fail('is not a zip file, or it is damaged or only partly downloaded. Download it again.')
    with z:
        ents = z.infolist()
        total = sum(e.file_size for e in ents) or 1
        check_space(dest, total)
        done = 0
        for e in ents:
            rel = safe_rel(e.filename)
            if rel is None: raise Fail(f'has a file that would land outside the mod folder ({e.filename}): not installed.')
            if not rel or stat.S_ISLNK(e.external_attr >> 16): continue
            if e.flag_bits & 1: raise Fail('is password-protected: extract it with the password yourself, then install the folder.')
            out = dest / rel
            if e.is_dir():
                out.mkdir(parents=True, exist_ok=True); continue
            out.parent.mkdir(parents=True, exist_ok=True)
            try:
                with z.open(e) as r, open(out, 'wb') as w:
                    shutil.copyfileobj(r, w, 1 << 20)
            except NotImplementedError:
                return False      # a compression Python does not read (Deflate64, ...): try tar / 7-Zip
            except (zipfile.BadZipFile, EOFError, OSError, ValueError) as x:
                if isinstance(x, OSError) and getattr(x, 'errno', 0) == 28:
                    raise Fail('ran out of disk space while extracting.')
                raise Fail(f'is damaged or only partly downloaded ({e.filename}: {x}). Download it again.')
            done += e.file_size
            say(pct=10 + 60 * done / total, msg=f'Extracting {rel}')
    return True


def extract_tool(src, dest, say):
    tar, sz = find_tool('tar'), find_tool('7z')
    if not tar and not sz:
        raise Fail('needs Windows\' tar.exe (Windows 10 1803 or newer) or 7-Zip to open. Install 7-Zip, or extract it '
                   'yourself and install the folder.')
    errs = []
    for tool in [t for t in (tar, sz) if t]:
        for p in list(dest.iterdir()): rmtree(p) if p.is_dir() else p.unlink()
        if tool == tar:
            rc, out, err = run([tar, '-t', '-f', str(src)])
            names = out.splitlines()
        else:
            rc, out, err = run([sz, 'l', '-slt', '-ba', str(src)])
            names = [l[7:] for l in out.splitlines() if l.startswith('Path = ')]
        if rc:
            errs.append(last_line(err) or last_line(out) or f'exit {rc}'); continue
        bad = [n for n in names if safe_rel(n) is None]
        if bad: raise Fail(f'has a file that would land outside the mod folder ({bad[0]}): not installed.')
        say(pct=20, msg=f'Extracting with {pathlib.Path(tool).name} ({len(names)} entries)')
        cmd = [tar, '-x', '-f', str(src), '-C', str(dest)] if tool == tar else [sz, 'x', '-y', '-bd', '-snl-', f'-o{dest}', str(src)]
        rc, out, err = run(cmd)
        if rc == 0: return
        errs.append(last_line(err) or last_line(out) or f'exit {rc}')
    msg = '; '.join(dict.fromkeys(errs))
    if re.search(r'passw|encrypt', msg, re.I):
        raise Fail('is password-protected: extract it with the password yourself, then install the folder.')
    raise Fail(f'could not be opened: it is damaged, only partly downloaded, or a kind this PC cannot read ({msg}). '
               'Download it again, or extract it with 7-Zip and install the folder.')


def check_space(dest, need):
    try:
        free = shutil.disk_usage(dest).free
    except OSError:
        return
    if free < need + (64 << 20):
        raise Fail(f'needs {human(need)} of free space and the drive with the mods folder has {human(free)}.')


def sanitize_tree(top):
    """No links out of the staging folder (an archive can hold symlinks)."""
    for d, ds, fs in os.walk(top):
        for n in ds + fs:
            p = os.path.join(d, n)
            if os.path.islink(p): os.unlink(p)


def extract(src, dest, say):
    say('extract', 10, f'Opening {src.name}')
    if src.name.lower().endswith('.zip'):
        if extract_zip(src, dest, say): return
        say(msg='Python cannot read this zip\'s compression: trying tar / 7-Zip')
    extract_tool(src, dest, say)
    sanitize_tree(dest)


# ---------------------------------------------------------------- finding the mods inside
def kids(d):
    try:
        return [p for p in d.iterdir() if not junk(p.name)]
    except OSError:
        return []


def find_roots(top):
    """[(folder, kind)] kind 'romfs' (has romfs/), 'bare' (the game's folders directly), 'code' (exefs/cheats only).
    A romfs folder anywhere wins over game-looking names: a mod folder may itself be called Pack or UI."""
    def walk(d, depth, test, out):
        ks = kids(d)
        dirs = {p.name.lower(): p for p in ks if p.is_dir()}
        k = test(dirs)
        if k: out.append((d, k)); return
        if depth < MAX_DEPTH:
            for p in sorted(dirs.values(), key=lambda p: p.name.lower()): walk(p, depth + 1, test, out)
    for test in (lambda ds: 'romfs' in ds and 'romfs', lambda ds: any(n in GAME_DIRS for n in ds) and 'bare',
                 lambda ds: ('exefs' in ds or 'cheats' in ds) and 'code'):
        out = []
        walk(top, 0, test, out)
        if out: return out
    return []


def root_name(root, top, fallback):
    parts = list(root.relative_to(top).parts)
    for p in reversed(parts):
        if p.lower() not in GENERIC and not TITLE_ID.match(p) and not VERSION_DIR.match(p): return clean_name(p)
    return clean_name(fallback)


def count(romfs):
    n = b = 0
    for d, _, fs in os.walk(romfs):
        for f in fs:
            if junk(f): continue
            n += 1
            try: b += os.path.getsize(os.path.join(d, f))
            except OSError: pass
    return n, b


def romfs_of(folder):
    """A mod folder's romfs folder in any letter case (modfs.c accepts any case; Linux and macOS file systems care)."""
    return next((p for p in kids(folder) if p.is_dir() and p.name.lower() == 'romfs'), folder / 'romfs')


def code_kinds(top):
    """What Switch code patches / cheats a folder holds, in words (empty: none)."""
    found = set()
    for d, ds, fs in os.walk(top):
        for x in ds:
            if x.lower() in CODE_DIRS: found.add('exefs code patches')
            if x.lower() == 'cheats': found.add('cheat codes')
        for f in fs:
            n = f.lower()
            if n.endswith('.ips'): found.add('.ips patches')
            elif n.endswith('.pchtxt'): found.add('.pchtxt patches')
            elif n.endswith('.nso') or n in CODE_NAMES: found.add('Switch program code (NSO)')
            elif n.endswith('.txt') and os.path.basename(d).lower() == 'cheats': found.add('cheat codes')
    order = ['exefs code patches', '.ips patches', '.pchtxt patches', 'Switch program code (NSO)', 'cheat codes']
    return [k for k in order if k in found]


CODE_WHY = ('Code patches and cheats change the Switch program\'s machine code. Ultra Instinct Tears runs the game recompiled '
            'for PC, so that code is not there and they cannot apply. A frame rate or resolution patch is not needed: '
            'those are built in (Performance > Frame rate > Frame rate target, and Graphics > Image quality). Only RomFS '
            'mods (a romfs folder, or the game\'s own folders such as Actor, Pack and UI) load.')


# ---------------------------------------------------------------- loose files: the game's own RomFS list
class GameFiles:
    """The game's RomFS file list (data\\romfs\\runtime-index.bin from setup.py, or a folder of plain files), by file
    name: a mod shipped as a bare file (no folders) goes where the game keeps the file of that name. Read only when
    needed, never changed. UIT_MODS_ROMFS overrides where it is (tests)."""
    def __init__(self):
        self.by_name, self.where = None, None

    def folder(self):
        r = os.environ.get('UIT_MODS_ROMFS') or cfg_value('TOTK_ROMFS') or 'data/romfs'
        r = pathlib.Path(r)
        return r if r.is_absolute() else ROOT / r

    def load(self):
        if self.by_name is not None: return self.by_name
        self.by_name, d = {}, self.folder()
        paths = []
        idx = d / 'runtime-index.bin'
        try:
            if idx.is_file():
                b = idx.read_bytes()
                if b[:8] == b'TOTKRFS1':
                    import struct
                    n, so = struct.unpack_from('<QQ', b, 8)
                    for i in range(n):
                        _, _, no, nl = struct.unpack_from('<QQII', b, 24 + 24 * i)
                        paths.append(b[so + no:so + no + nl - 1].decode('utf-8', 'replace'))
                    self.where = str(idx)
            elif d.is_dir():
                for dp, _, fs in os.walk(d):
                    rel = os.path.relpath(dp, d).replace(os.sep, '/')
                    for f in fs: paths.append(f if rel == '.' else f'{rel}/{f}')
                self.where = str(d)
        except (OSError, ValueError):
            paths = []
        for p in paths:
            self.by_name.setdefault(p.rsplit('/', 1)[-1].lower(), []).append(p)
        return self.by_name


GAME = GameFiles()


def loose_files(top):
    """Files that look like game files (not readmes, pictures, code) anywhere in top: [(path, name)]."""
    out = []
    for d, ds, fs in os.walk(top):
        ds[:] = [x for x in ds if not junk(x) and x.lower() not in CODE_DIRS and x.lower() != 'cheats']
        for f in fs:
            n = f.lower()
            if junk(f) or n.endswith(NOT_ROMFS) or n.endswith(CODE_EXT) or n in CODE_NAMES or is_archive(pathlib.Path(d) / f):
                continue
            out.append((pathlib.Path(d) / f, f))
    return out


def loose_name(fname, placed):
    """A name for a mod that is one bare game file: "Title.Product.110.Nin_NX_NVN.blarc.zs" -> "Title (LayoutArchive)"."""
    n = re.sub(r'\.zs$', '', fname, flags=re.I)
    n = re.sub(r'\.(Product\.\d+|Nin_NX_NVN|Nin_NX)\b', '', n, flags=re.I)
    n = n.split('.')[0] or fname
    where = placed[0].rsplit('/', 2)[-2] if placed and '/' in placed[0] else ''
    return f'{n} ({where})' if where else n


PLACED = 'uit-placed.txt'          # in a mod placed by file name: one game path per line (beside romfs, so it never loads)


class Undo:
    """The last install that placed files by name can be undone (the launcher's Undo): mods/.undo keeps the load order
    as it was, the installed copies that were replaced, and inbox items that were moved. A folder starting with a dot is
    skipped by the game and the launcher, like any other."""
    def __init__(self):
        self.md, self.new, self.old, self.moved, self.loose = None, [], [], [], False
    def start(self, md):
        self.md, self.new, self.old, self.moved, self.loose = md, [], [], [], False
        self.dir = md / '.undo'
        if self.dir.exists(): rmtree(self.dir, ignore=True)
        lo = md / 'load-order.txt'
        self.order = lo.read_text(encoding='utf-8', errors='replace') if lo.is_file() else None
        self.readme = (md / 'README.txt').exists()
        self.had_md = md.is_dir()
    def keep_old(self, dest):          # an installed copy about to be replaced: kept here instead of deleted
        if not self.md: self.start(dest.parent)
        to = self.dir / 'old' / dest.name
        to.parent.mkdir(parents=True, exist_ok=True)
        if to.exists(): rmtree(to, ignore=True)
        os.replace(dest, to)
        self.old.append(dest.name)
        return to
    def finish(self):
        if not self.md: return
        if not self.loose or not (self.new or self.old):
            if self.dir.exists(): rmtree(self.dir, ignore=True)
            return
        self.dir.mkdir(parents=True, exist_ok=True)
        (self.dir / 'undo.json').write_text(json.dumps({'new': self.new, 'old': self.old, 'moved': self.moved,
            'order': self.order, 'readme': self.readme, 'had_md': self.had_md}, indent=1), encoding='utf-8')


UNDO = Undo()


def undo(md):
    """Puts the mods folder back as it was before the last install that placed files by name."""
    u = md / '.undo'
    try:
        j = json.loads((u / 'undo.json').read_text(encoding='utf-8'))
    except (OSError, ValueError):
        tsv('error', 'undo', 'There is nothing to undo: the last install has no undo record (it is kept only for the '
            'last install that placed files by name, until the next install).'); return 1
    rc = 0
    back = {pathlib.Path(b).name for a, b in j['moved'] if pathlib.Path(b).parent == md}
    for a, b in j['moved']:          # inbox items back where they were (a folder moved in whole goes back as it is now)
        try:
            if pathlib.Path(b).exists() and not pathlib.Path(a).exists(): shutil.move(b, a)
        except OSError:
            rc = 1
    for n in j['new'] + j['old']:
        p = md / n
        if n in back and not p.exists(): continue
        if p.exists():
            try:
                rmtree(p)
            except OSError as e:
                tsv('error', n, f'could not remove {n} ({e}). Is the game running?'); return 1
    for n in j['old']:
        if (u / 'old' / n).exists(): os.replace(u / 'old' / n, md / n)
    lo = md / 'load-order.txt'
    if j['order'] is None:
        if lo.exists(): lo.unlink()
    else:
        tmp = md / 'load-order.txt.new'
        tmp.write_text(j['order'], encoding='utf-8'); os.replace(tmp, lo)
    if not j['readme'] and (md / 'README.txt').exists(): (md / 'README.txt').unlink()
    rmtree(u, ignore=True)
    if not j['had_md']:
        try: md.rmdir()
        except OSError: pass
    for n in j['new']: tsv('undone', n, 'removed')
    for n in j['old']: tsv('undone', n, 'restored')
    return rc


def place_loose(top, stage, label):
    """Loose files whose names the game has exactly once -> stage/romfs/<the game's path>. Returns (root, placed, left)
    or raises Fail with what to do."""
    files = loose_files(top)
    if not files: return None
    names = GAME.load()
    if not names:
        ex = files[0][1]
        raise Fail(f'has only loose files ({ex}{" and more" if len(files) > 1 else ""}) and no folders that say where in '
                   'the game they go, and the game\'s file list was not found to look it up (run tools/setup.py first). '
                   'Put the files in a folder named romfs with the game\'s folders, e.g. romfs\\UI\\LayoutArchive\\, then '
                   'install that folder.')
    root = stage / 'loose'
    placed, left = [], []
    for p, f in files:
        hit = names.get(f.lower(), [])
        if len(hit) != 1: left.append((f, len(hit))); continue
        dest = root / 'romfs' / hit[0]
        if dest.exists(): left.append((f, 2)); continue
        dest.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(p, dest)
        placed.append(hit[0])
    if not placed:
        f, n = left[0]
        why = (f'the game has {n} files named {f}, so it cannot tell which one it replaces' if n > 1 else
               f'the game has no file named {f}: it may be for another game version, or another game')
        raise Fail(f'has only loose files and no folders that say where in the game they go; {why}. Put the file in a '
                   'romfs folder with the game\'s folders (as the mod\'s readme says), then install that folder.')
    try:
        home = pathlib.Path(os.path.commonpath([str(p.parent) for p, _ in files]))
    except ValueError:
        home = top
    (root / PLACED).write_text(''.join(x + '\n' for x in placed), encoding='utf-8')   # the launcher shows where each went
    for x in kids(home):          # the mod's readme / info.json go along
        if x.is_file() and x.name.lower().endswith(('.txt', '.md', '.json', '.png', '.jpg')) and not (root / x.name).exists():
            shutil.copy2(x, root / x.name)
    return root, placed, left, home


def romfs_digest(root, kind):
    """The files a root brings and their bytes, as one value (variants that are the very same mod compare equal)."""
    import hashlib
    base = romfs_of(root) if kind == 'romfs' else root
    h = hashlib.sha1()
    for p in sorted((x for x in base.rglob('*') if x.is_file() and not junk(x.name)), key=lambda x: str(x).lower()):
        h.update(str(p.relative_to(base)).lower().encode() + b'\0')
        with open(p, 'rb') as f:
            for c in iter(lambda: f.read(1 << 20), b''): h.update(c)
    return h.hexdigest()


def romfs_files(root, kind):
    base = next((p for p in kids(root) if p.is_dir() and p.name.lower() == 'romfs'), None) if kind == 'romfs' else root
    out = set()
    if not base: return out
    for d, ds, fs in os.walk(base):
        rel = os.path.relpath(d, base).replace(os.sep, '/')
        if kind == 'bare' and rel == '.':
            ds[:] = [x for x in ds if x.lower() in GAME_DIRS]; continue
        for f in fs:
            if not junk(f): out.add(f'{rel}/{f}'.lower())
    return out


def place(root, kind, target, copy):
    """root -> target/ (target/romfs/... for a bare layout). copy: the source is the player's folder."""
    mv = (lambda a, b: shutil.copytree(a, b) if a.is_dir() else shutil.copy2(a, b)) if copy else (lambda a, b: shutil.move(str(a), str(b)))
    target.mkdir(parents=True)
    if kind == 'bare': (target / 'romfs').mkdir()
    for p in kids(root):
        if kind == 'bare' and p.is_dir() and p.name.lower() in GAME_DIRS: mv(p, target / 'romfs' / p.name)
        else: mv(p, target / p.name)


# ---------------------------------------------------------------- install
def install_one(src, md, name, say, results, take=False):
    src = pathlib.Path(src)
    label = src.name
    if not src.exists(): raise Fail(f'{label}: not found.')
    single = src.is_file() and not is_archive(src)   # one bare game file (dropped or picked): placed by its name
    if single and (src.name.lower().endswith(CODE_EXT) or src.name.lower() in CODE_NAMES):
        raise Fail(f'{label} is a code patch, not a file mod, so it was not installed. ' + CODE_WHY)
    if single and src.name.lower().endswith(NOT_ROMFS):
        raise Fail(f'{label}: not a mod archive (.zip, .7z, .rar), a mod folder, or a game file.')
    md.mkdir(parents=True, exist_ok=True)
    try:
        if src.resolve() == md.resolve() or md.resolve() in src.resolve().parents:
            raise Fail(f'{label} is already in the mods folder: it is installed (see the list).')
    except OSError:
        pass
    stage = md / f'.inst-{os.getpid() % 10000}{int(time.time() * 1000) % 1000}'   # short: Windows' 260-character paths
    stage.mkdir()
    try:
        if single:
            top, copy = stage / 'x', False
            top.mkdir()
            shutil.copy2(src, top / src.name)
        elif src.is_dir():
            top, copy = src, not take    # take: a folder in the inbox is moved, not copied
            n, b = count(src)
            check_space(md, b)
        else:
            top, copy = stage / 'x', False
            top.mkdir()
            extract(src, top, say)
            for _ in range(2):           # an archive inside the archive (a common way mods are re-shared)
                if find_roots(top): break
                inner = [p for p in top.rglob('*') if is_archive(p) and not junk(p.name)]
                if len(inner) != 1: break
                nxt = stage / f'x{len(list(stage.iterdir()))}'
                nxt.mkdir()
                extract(inner[0], nxt, say)
                top = nxt
        say('detect', 75, 'Looking for the mod\'s files')
        roots = find_roots(top)
        good = [(r, k) for r, k in roots if k != 'code']
        loose = None
        if not good:
            code = code_kinds(top)
            if not code:
                loose = place_loose(top, stage, label)
                if loose: good, copy = [(loose[0], 'romfs')], False
            if not good and code:
                raise Fail(f'{label} is a code mod ({", ".join(code)}), not a file mod, so it was not installed. ' + CODE_WHY)
            if not good:
                raise Fail(f'{label} has no mod files for this game: no romfs folder and none of the game\'s folders '
                           '(Actor, Pack, UI, ...) in it.')
        if len(good) > 1:                # the same files for each emulator ("Mod/Yuzu/romfs", "Mod/Ryujinx/romfs"): one copy
            seen, uniq = set(), []
            for r, k in good:
                d = romfs_digest(r, k)
                if d not in seen: seen.add(d), uniq.append((r, k))
            good = uniq
        fb = loose_name(src.name, loose[1]) if single else stem(src) if src.is_file() else src.name
        raw = []
        for r, k in good:
            nm = clean_name(name) if name and len(good) == 1 else (root_name(loose[3], top, fb) if loose[3] != top else clean_name(fb)) if loose else root_name(r, top, fb)
            if len(good) > 1:            # variants: "Archive - 60fps", not a bare "60fps"
                f = clean_name(fb)
                nm = f if nm.lower() == f.lower() else nm if nm.lower().startswith(f.lower()) else clean_name(f'{f} - {nm}')
            raw.append(nm)
        names, taken = [], set()
        for (r, k), nm in zip(good, raw):
            if sum(x.lower() == nm.lower() for x in raw) > 1:   # "Sky (Yuzu)", "Sky (Ryujinx)": the folder that tells them apart
                diff = [p for p in r.relative_to(top).parts if p.lower() != 'romfs' and not TITLE_ID.match(p)]
                tag = next((p for p in reversed(diff) if clean_name(p).lower() not in nm.lower()), None)
                if tag: nm = clean_name(f'{nm} ({tag})')
            base, i = nm, 2
            while nm.lower() in taken: nm, i = f'{base} {i}', i + 1
            taken.add(nm.lower()), names.append(nm)
        # variants that replace the same files: only the first goes on
        on, files_on = [], set()
        for r, k in good:
            fs = romfs_files(r, k)
            clash = bool(fs & files_on)
            on.append(not clash)
            if not clash: files_on |= fs
        order = read_order(md)
        existing = {n.lower(): n for n, _ in order}
        say('place', 85, 'Putting the files in place')
        placed = []
        for (r, k), nm, en in zip(good, names, on):
            old = existing.get(nm.lower())
            dest = md / (old or nm)
            new = stage / ('m-' + nm)
            place(r, k, new, copy)
            if dest.exists():
                bak = UNDO.keep_old(dest)          # kept until this run ends (or for Undo)
                try:
                    os.replace(new, dest)
                except OSError:
                    os.replace(bak, dest); UNDO.old.remove(dest.name)
                    raise
            else:
                os.replace(new, dest)
                UNDO.new.append(dest.name)
            if loose: UNDO.loose = True
            placed.append((dest.name, en, bool(old)))
        # order: new mods on top in archive order, updated ones keep their place and state
        say('order', 95, 'Load order')
        order = [(n, o) for n, o in read_order(md) if n not in {p for p, _, up in placed if not up}]
        order = [(p, en) for p, en, up in placed if not up] + order
        write_order(md, order)
        state = dict(order)
        for nm, en, up in placed:
            n, b = count(romfs_of(md / nm))
            extra = [x for x in ('exefs', 'cheats') if (md / nm / x).is_dir()]
            note = []
            if loose:
                note.append('placed by file name: ' + ', '.join(loose[1][:3]) + (' and more' if len(loose[1]) > 3 else ''))
                if loose[2]: note.append(f'{len(loose[2])} loose file(s) left out (the game has none, or several, of that name)')
            if extra: note.append(f'its {" and ".join(extra)} cannot load here (shown in the list)')
            if not en: note.append('off: it replaces the same files as another variant from this archive; turn on the one you want')
            if up: note.append('replaced the installed copy; its place in the load order kept')
            tsv('installed', nm, n, b, 'updated' if up else 'new' if state.get(nm, True) else 'off', '; '.join(note))
            if loose:
                for g in loose[1]: tsv('placed', nm, g)
            results.append(nm)
    finally:
        rmtree(stage, ignore=True)


def install(sources, md, name, say):
    ok = bad = 0
    for i, s in enumerate(sources):
        say('read', 2, f'{pathlib.Path(s).name} ({i + 1} of {len(sources)})')
        try:
            res = []
            install_one(s, md, name if len(sources) == 1 else None, say, res)
            ok += 1
        except Fail as e:
            m = str(e)
            tsv('error', pathlib.Path(s).name, m if m.startswith(pathlib.Path(s).name) else f'{pathlib.Path(s).name} {m}')
            bad += 1
        except OSError as e:
            tsv('error', pathlib.Path(s).name, f'{pathlib.Path(s).name}: could not write the mod files ({e}). Is the game running, or the folder read-only?')
            bad += 1
    say('done', 100, f'{ok} installed, {bad} failed', state='ok' if not bad else 'error' if not ok else 'partial')
    return 0 if not bad else 1


def inbox(ib, md, say):
    ib.mkdir(parents=True, exist_ok=True)
    readme = ib / 'README.txt'
    if not readme.exists():
        readme.write_text('Put mod downloads here (.zip, .7z, .rar or a folder), then press Install from the inbox in the '
                          'launcher\'s Mods tab.\nInstalled ones move to installed\\, ones that could not be installed to '
                          'failed\\ with a note that says why.\n')
    items = sorted((p for p in kids(ib) if p.name.lower() not in ('installed', 'failed', 'readme.txt') and
                    (p.is_dir() or is_archive(p) or p.name.lower().endswith(('.zs',) + CODE_EXT))), key=lambda p: p.stat().st_mtime)
    if not items:
        say('done', 100, 'The inbox is empty', state='ok'); tsv('empty', str(ib)); return 0
    ok = bad = 0
    for i, p in enumerate(items):
        say('read', 2, f'{p.name} ({i + 1} of {len(items)})')
        try:
            res = []
            install_one(p, md, None, say, res, take=p.is_dir())
            if not p.exists() and len(res) == 1: UNDO.moved.append((str(p), str(md / res[0])))   # a folder moved in whole
            dest, ok = ib / 'installed', ok + 1
            why = None
        except (Fail, OSError) as e:
            m = str(e)
            m = m if m.startswith(p.name) else f'{p.name} {m}'
            tsv('error', p.name, m)
            dest, bad, why = ib / 'failed', bad + 1, m
        dest.mkdir(exist_ok=True)
        to, k = dest / p.name, 2
        if not p.exists(): continue      # a folder moved in whole
        while to.exists(): to, k = dest / f'{stem(p)} ({k}){p.name[len(stem(p)):]}', k + 1
        try:
            shutil.move(str(p), str(to))
            if not why: UNDO.moved.append((str(p), str(to)))
            if why: (dest / (to.name + '.txt')).write_text(why + '\n')
        except OSError:
            pass
    say('done', 100, f'{ok} installed, {bad} failed', state='ok' if not bad else 'error' if not ok else 'partial')
    return 0 if not bad else 1


def remove(names, md):
    rc = 0
    have = mod_folders(md)
    for n in names:
        if n not in have or '/' in n or '\\' in n or junk(n):
            tsv('error', n, f'{n} is not an installed mod.'); rc = 1; continue
        p = md / n
        try:
            rmtree(p)
        except OSError as e:
            tsv('error', n, f'could not remove {n} ({e}). Is the game running?'); rc = 1; continue
        tsv('removed', n)
    write_order(md, read_order(md))
    return rc


MODS_README = r"""Ultra Instinct Tears mods folder
================================

Easiest: install and manage mods in the launcher, Mods tab. It works with a controller, or with mouse and keyboard.
  - Install from a file...      a .zip, .7z or .rar download, or one game file (you can also drop it on the window)
  - Install a mod folder...     an unpacked mod; it is copied, the original stays where it is
  - Install from mods-inbox     for a controller: copy downloads into the mods-inbox folder next to this one first
Each mod in the list: turn it on or off, move it up or down (a higher mod wins a file that both change), or uninstall it.
The right side shows its files, which mods it wins or loses files to, and anything in it that cannot load.

By hand (the launcher reads it the same way):
  - Each mod is one folder here: mods\<Mod name>\romfs\<the game's own paths>, for example
    mods\Some Fix\romfs\UI\LayoutArchive\Common.Product.110.Nin_NX_NVN.blarc.zs
    A folder with the game's folders directly in it (mods\Some Fix\UI\...) works too, and so do extra folders
    around one romfs folder (mods\Some Fix\Some Fix\romfs, or ...\atmosphere\contents\<title id>\romfs).
  - load-order.txt: the top line wins. +Name is on, -Name is off. A folder it does not list is on and goes on top.
  - Changes apply the next time you press Play.

What works: RomFS mods, the ones that replace or add game files (models, textures, UI, text, actor packs).
A mod's ResourceSizeTable (System\Resource\...rsizetable.zs) is merged with the game's and other mods' tables at start
(Mods > Mod settings > Fit the resource size table to mods).

What cannot work: exefs code patches (.ips, .pchtxt, NSO files), cheats, and .dll or .exe mods. They change the Switch
program's machine code, and Ultra Instinct Tears runs the game recompiled for PC, so that code is not there. The launcher
says so instead of installing them. Frame rate and resolution mods are not needed: Performance > Frame rate > Frame rate
target, and Graphics > Image quality > Scene resolution.

Mods are for game version 1.4.3. A mod made for another version may show "files the game does not have" in the launcher.
To rule mods out when something looks wrong: Mods > Mod settings > Load mods: Off (keeps each mod's own on/off).
"""


def ensure_readme(md):
    try:
        if md.is_dir() and not (md / 'README.txt').exists(): (md / 'README.txt').write_text(MODS_README, encoding='utf-8', newline='\r\n')
    except OSError:
        pass


def main():
    ap = argparse.ArgumentParser(description='Install and remove Ultra Instinct Tears mods.')
    sub = ap.add_subparsers(dest='cmd', required=True)
    i = sub.add_parser('install'); i.add_argument('source', nargs='+'); i.add_argument('--name')
    b = sub.add_parser('inbox'); b.add_argument('--inbox')
    r = sub.add_parser('remove'); r.add_argument('name', nargs='+')
    u = sub.add_parser('undo', help='undo the last install that placed files by name')
    for p in (i, b, r, u): p.add_argument('--mods'); p.add_argument('--progress')
    a = ap.parse_args()
    md = mods_dir(a.mods)
    say = Progress(a.progress)
    if a.cmd == 'undo': return undo(md)
    if a.cmd == 'remove' and (md / '.undo').exists(): rmtree(md / '.undo', ignore=True)   # undo only right after the install
    if a.cmd in ('install', 'inbox'): UNDO.start(md)
    try:
        if a.cmd == 'install': return install(a.source, md, a.name, say)
        if a.cmd == 'inbox': return inbox(pathlib.Path(a.inbox) if a.inbox else md.parent / 'mods-inbox', md, say)
        return remove(a.name, md)
    finally:
        ensure_readme(md)
        if a.cmd in ('install', 'inbox'): UNDO.finish()


if __name__ == '__main__':
    sys.exit(main())
