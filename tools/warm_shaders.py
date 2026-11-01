"""[UIT 0081] Re-warm the shaders after an update or a rebuild: tools/setup.py --warm-shaders (and, with
Performance > Stutter > Shaders+ > "Warm up shaders after an update" on, the end of every setup that changed the game
or its shaders) starts UltraInstinctTears.exe with TOTK_WARM_ONLY=1. The game makes the shaders you have seen
(<shaders>/used.txt, the same list and settings as at a normal start), which fills the graphics driver's own shader
cache, and exits before the title screen (src/gfx/vkb_shader.c prewarm_start). This module reads the game's progress
lines and shows them; the game's whole output goes to build/warm-shaders.log.

Nothing is downloaded and no game file is changed: only the driver's cache (and the binary cache, when that setting
is on) fills, as it would during the first minutes of play."""
import hashlib
import os
import pathlib
import re
import subprocess
import threading
import time

PROGRESS = re.compile(r'shader warm-up: (\d+) of (\d+) shaders')
LISTED = re.compile(r'shader list: (\d+) shaders to make ahead')
DONE = re.compile(r'shader warm-up only \(TOTK_WARM_ONLY\): done, (\d+) shaders and (\d+) variants made in ([\d.]+) s')
OFF = 'shader warm-up: off'
BOOT_S = 300      # no shader list this long after the start: something else holds the game (a message box, no GPU)
STALL_S = 300     # no progress this long: the driver hangs; the game is closed and the warm-up happens at the next start


def shader_dir(root):
    """The shader folder the game uses: ultrainstincttears.cfg "shaders=" (relative to the game folder), else the
    TOTK_SHADER_DIR setting, else setup.py's layout."""
    root = pathlib.Path(root)
    d = 'data/shaders/ryujinx'
    cfg = root / 'ultrainstincttears.cfg'
    if cfg.is_file():
        for line in cfg.read_text(errors='replace').splitlines():
            k, sep, v = line.strip().partition('=')
            if sep and k.strip() in ('shaders', 'TOTK_SHADER_DIR') and v.strip():
                d = v.strip()
    p = pathlib.Path(d)
    return p if p.is_absolute() else root / p


def setting(root, key):
    """A TOTK_* value: the environment, else ultrainstincttears.cfg, else None."""
    if os.environ.get(key):
        return os.environ[key].strip()
    cfg = pathlib.Path(root) / 'ultrainstincttears.cfg'
    if cfg.is_file():
        for line in cfg.read_text(errors='replace').splitlines():
            k, sep, v = line.strip().partition('=')
            if sep and k.strip() == key:
                return v.strip()
    return None


def stamp(root, exe_name, state):
    """What the driver's shader cache depends on that setup changes: the game exe (a rebuild can change the shader
    layout) and the translated shaders. A different stamp than at the last warm-up: the shaders are made again."""
    exe = pathlib.Path(root) / exe_name
    h = hashlib.sha256()
    if exe.is_file():
        st = exe.stat()
        h.update(f'{st.st_size} {st.st_mtime_ns}'.encode())
    h.update(str(state.get('shaders-xlate', '')).encode())
    return h.hexdigest()[:16]


def listed(root):
    """How many shaders the list of the ones you have seen holds (0: nothing to warm up yet)."""
    used = shader_dir(root) / 'used.txt'
    try:
        return sum(1 for line in used.read_text(errors='replace').splitlines() if len(line) >= 64)
    except OSError:
        return 0


def run(cmd, cwd, say, log_path, env=None, boot_s=BOOT_S, stall_s=STALL_S):
    """Run the game warm-up only; say() the progress about every 10 %. Returns (ok, message)."""
    env = dict(os.environ if env is None else env, TOTK_WARM_ONLY='1')
    log_path = pathlib.Path(log_path)
    log_path.parent.mkdir(parents=True, exist_ok=True)
    try:
        p = subprocess.Popen([str(c) for c in cmd], cwd=cwd, env=env, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                             stderr=subprocess.STDOUT)
    except OSError as e:
        return False, f'cannot start the game: {e}'
    st = {'t': time.time(), 'listed': None, 'done': None, 'off': False, 'n': (0, 0), 'shown': -1}
    lock = threading.Lock()

    def reader():
        with open(log_path, 'wb') as log:
            for raw in p.stdout:
                log.write(raw)
                line = raw.decode('utf-8', 'replace')
                with lock:
                    if m := PROGRESS.search(line):
                        n = (int(m[1]), int(m[2]))
                        if n != st['n']:
                            st['n'], st['t'] = n, time.time()
                        tenth = 10 * n[0] // max(n[1], 1)
                        if tenth > st['shown']:
                            st['shown'] = tenth
                            say(f'  shaders: {n[0]} of {n[1]} ({100 * n[0] // max(n[1], 1)} %)')
                    elif m := LISTED.search(line):
                        st['listed'], st['t'] = int(m[1]), time.time()
                        say(f'  shaders: {m[1]} to make (the ones you have seen)')
                    elif m := DONE.search(line):
                        st['done'] = (int(m[1]), int(m[2]), float(m[3]))
                    elif OFF in line:
                        st['off'] = True

    t = threading.Thread(target=reader, daemon=True)
    t.start()
    why = None
    while p.poll() is None:
        time.sleep(0.2)
        with lock:
            idle = time.time() - st['t']
            if st['listed'] is None and not st['off'] and idle > boot_s:
                why = f'the game did not start its warm-up within {boot_s} s'
            elif st['listed'] is not None and idle > stall_s:
                why = f'no progress for {stall_s} s'
        if why:
            p.kill()
            break
    p.wait()
    t.join(5)
    if st['off']:
        return True, 'shader warm-up is off (Performance > Stutter > Shaders > Shader warm-up): nothing to do'
    if st['done']:
        made, variants, secs = st['done']
        return True, f'{made} shaders and {variants} variants made in {secs:.0f} s'
    if why:
        return False, f'{why}: stopped (see {log_path}); the warm-up happens at the next start instead'
    return False, f'the game exited (code {p.returncode}) before the warm-up was done (see {log_path})'
