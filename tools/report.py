"""Collect what a bug report needs into one zip: the newest game log(s), lasrecomp.log (a run started without the
launcher), ultrainstincttears.cfg, VERSION.txt, crash files, the newest benchmarks, the newest update log and setup's state.
Nothing of the game itself (data/, userdata/, saves, the dump) is ever added.

    py -3 tools/report.py [--out FOLDER] [--logs N]

The zip goes to --out, else TOTK_LAUNCHER_REPORT_DIR from ultrainstincttears.cfg, else the Desktop, else this folder's
reports/. It prints the zip's path on the last line. The cfg holds your folder paths; read the zip before sharing it.
"""
import argparse, datetime, os, pathlib, sys, zipfile

ROOT = pathlib.Path(__file__).resolve().parent.parent
CAP = 64 << 20            # a log bigger than this: its first and last 32 MB


def cfg_value(key):
    cfg = ROOT / 'ultrainstincttears.cfg'
    if cfg.is_file():
        for line in cfg.read_text(errors='replace').splitlines():
            k, _, v = line.strip().partition('=')
            if k.strip() == key and v.strip() not in ('', '-'):   # '-' = left at the default
                return v.strip()
    return ''


def desktop():
    if os.name == 'nt':
        try:
            import ctypes, ctypes.wintypes, uuid
            guid = uuid.UUID('{B4BFCC3A-DB2C-424C-B029-7FE99A87C641}')   # FOLDERID_Desktop (follows OneDrive moves)
            buf = ctypes.c_wchar_p()
            g = (ctypes.c_byte * 16).from_buffer_copy(guid.bytes_le)
            if ctypes.windll.shell32.SHGetKnownFolderPath(ctypes.byref(g), 0, None, ctypes.byref(buf)) == 0:
                p = pathlib.Path(buf.value); ctypes.windll.ole32.CoTaskMemFree(buf)
                if p.is_dir(): return p
        except Exception:   # noqa: BLE001 - fall back below
            pass
    p = pathlib.Path.home() / 'Desktop'
    return p if p.is_dir() else None


def newest(paths, n):
    return sorted((p for p in paths if p.is_file()), key=lambda p: p.stat().st_mtime, reverse=True)[:n]


def add(z, p, name, notes):
    size = p.stat().st_size
    if size <= CAP:
        z.write(p, name)
        return
    with open(p, 'rb') as f:
        head = f.read(CAP // 2); f.seek(size - CAP // 2); tail = f.read()
    z.writestr(name, head + b'\n\n[... %d bytes left out by tools/report.py ...]\n\n' % (size - CAP) + tail)
    notes.append(f'{name}: {size >> 20} MB, first and last {CAP >> 21} MB kept')


def main():
    ap = argparse.ArgumentParser(description='Zip the logs and settings a bug report needs.')
    ap.add_argument('--out', type=pathlib.Path)
    ap.add_argument('--logs', type=int, default=2, help='how many of the newest game logs (default 2)')
    a = ap.parse_args()
    out = a.out or (pathlib.Path(os.path.expandvars(cfg_value('TOTK_LAUNCHER_REPORT_DIR'))) if cfg_value('TOTK_LAUNCHER_REPORT_DIR') else None) \
        or desktop() or ROOT / 'reports'
    out.mkdir(parents=True, exist_ok=True)
    logs = ROOT / 'logs'
    pick = []   # (path, name in the zip)
    for p in newest(logs.glob('ultrainstincttears-*.log'), a.logs): pick.append((p, f'logs/{p.name}'))
    for p in newest(logs.glob('update-*.log'), 1): pick.append((p, f'logs/{p.name}'))
    # [UIT 0070] what the build-time scan found (addresses and counts only: the instruction words stay in
    # lift-scan-words.txt, which is never added) and what the game's shadow-size check saw
    for n in ('ultrainstincttears.cfg', 'VERSION.txt', 'VERSION', 'launcher-crash.txt', 'data/setup.json',
              'data/generated/lift-scan.txt', 'data/generated/lift-features.ini', 'diag/lift-probe.txt',
              'diag/lift-confirm.txt', 'diag/amiibo.txt'):   # [UIT 0180] the amiibo reader and cheat, every run
        if (ROOT / n).is_file(): pick.append((ROOT / n, n))
    # [UIT 0259] the log of a run started without the launcher (UltraInstinctTears.exe by double-click), next to the exe
    if (ROOT / 'lasrecomp.log').is_file(): pick.append((ROOT / 'lasrecomp.log', 'lasrecomp.log'))
    # [UIT 0157] the newest frame captures (Shift+F7 / Overlay > Capture a frame for diagnosis)
    for p in newest((ROOT / 'diag').glob('frame-capture-*.txt'), 3): pick.append((p, f'diag/{p.name}'))
    # [UIT 0338] the audit a frame capture starts (what the replay substituted), [UIT 0339] the shader translator's
    # results of the last setup (why shaders failed) and its error output
    for p in newest((ROOT / 'diag').glob('audit-*.md'), 3): pick.append((p, f'diag/{p.name}'))
    # [UIT 0342] the image-write watch (TOTK_WATCH_IMAGE_WRITES)
    for p in newest((ROOT / 'diag').glob('image-writes-*.txt'), 2): pick.append((p, f'diag/{p.name}'))
    sh = ROOT / 'data' / 'shaders'
    for p in [sh / 'ryujinx-results-new.tsv'] + newest(sh.glob('ryujinx-stderr-*.txt'), 2):
        if p.is_file(): pick.append((p, p.relative_to(ROOT).as_posix()))
    # [UIT 0332] the newest benchmark (Shift+F8): its summary .txt and every frame's .csv
    for p in newest((ROOT / 'benchmarks').glob('*.txt'), 2) + newest((ROOT / 'benchmarks').glob('*.csv'), 2):
        pick.append((p, f'benchmarks/{p.name}'))
    for p in newest(list(ROOT.glob('*crash*.txt')) + list(logs.glob('*crash*.txt')) + list(ROOT.glob('*.dmp')) + list(logs.glob('*.dmp')), 4):
        if all(p != q for q, _ in pick): pick.append((p, p.relative_to(ROOT).as_posix()))
    notes = []
    # [UIT 0180] a game run newer than the newest log (its frame capture, amiibo log or crash file) was started with
    # Play, not Play with log: say so, it is what the report is usually about
    game_logs = [p for p, n in pick if n.startswith('logs/ultrainstincttears-') or n == 'lasrecomp.log']
    last_log = max((p.stat().st_mtime for p in game_logs), default=0)
    later = [(p.stat().st_mtime, n) for p, n in pick
             if (n.startswith('diag/frame-capture-') or n == 'diag/amiibo.txt' or 'crash' in n or n.endswith('.dmp'))
             and n != 'launcher-crash.txt' and p.stat().st_mtime > last_log + 120]
    if game_logs and later:
        t, n = max(later)
        notes.append('The newest game run (%s, %s) has no log: it was started with Play, not "Play with log". '
                     'For a problem in that run, play again with "Play with log" and make the report again.'
                     % (datetime.datetime.fromtimestamp(t).strftime('%Y-%m-%d %H:%M'), n))
    stamp = datetime.datetime.now().strftime('%Y%m%d-%H%M%S')
    zp = out / f'UltraInstinctTears-report-{stamp}.zip'
    with zipfile.ZipFile(zp, 'w', zipfile.ZIP_DEFLATED) as z:
        for p, name in pick:
            add(z, p, name, notes)
        z.writestr('README.txt', 'Ultra Instinct Tears problem report, made %s (local time).\nFiles: %s\n%s\n'
                   'No game files, saves or keys are in this zip. ultrainstincttears.cfg shows your folder paths.\n'
                   % (stamp, ', '.join(n for _, n in pick) or 'none found', '\n'.join(notes)))
    print(f'{len(pick)} files ({", ".join(n for _, n in pick)})')
    for n in notes:
        if n.startswith('The newest game run'): print(n)
    if not any(n.startswith('logs/ultrainstincttears-') or n == 'lasrecomp.log' for _, n in pick):
        print('no game log yet: use "Play with log", reproduce the problem, then make the report again')
    print(zp)
    return 0


if __name__ == '__main__':
    sys.exit(main())
