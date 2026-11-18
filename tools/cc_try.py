"""[UIT 0359] Compile one of the game files that would be built unoptimized (-O0) with optimization first, under a
time limit, and fall back to -O0 when that takes too long or fails.

    python tools/cc_try.py [--stamp .split] SECONDS SOURCE OBJECT -- <optimized compile> -- <-O0 compile>

Those files hold a function that is one big switch over all its instructions; clang -O1 takes from a minute to hours
on such a file, -O0 seconds (tools/build_game.py). The outcome is kept in OBJECT.try with a key of the source, both
commands and the limit, so a file that timed out once goes straight to -O0 in later builds until one of them changes.
Exit code: the one of the compile that made OBJECT. Prints one line when it falls back.
[UIT 0370] --stamp .split: tools/build_game.py's rule for the fast part of a split function (OBJECT.split).
[UIT 0445] The limit is the compile's own CPU time, not the time on the clock: on a busy PC (the rest of the build, a
game, a virus scan) a compile that got only part of a core used to run past the limit and was built -O0, and that
object stayed in every later build (ninja does not run an up-to-date step again) without a line about it. Now a compile
that is still under the limit in CPU time gets up to twice the limit on the clock; one stopped there with less than
half a core is built -O0 for this build only: its stamp says "busy" and tools/build_game.py builds it again (optimized
first) in the next build. Only a compile that used its whole limit in CPU time, or failed, is remembered as -O0.
"""
import hashlib
import json
import os
import pathlib
import subprocess
import sys
import time


def key_of(source, opt, o0, seconds):
    h = hashlib.sha256()
    with open(source, 'rb') as f:
        for block in iter(lambda: f.read(1 << 20), b''):
            h.update(block)
    h.update(json.dumps([opt, o0, seconds]).encode())
    return h.hexdigest()


def cpu_seconds(p):
    """[UIT 0445] The CPU time (user + kernel) the process has used so far, or None when it cannot be read."""
    try:
        if os.name == 'nt':
            import ctypes
            from ctypes import wintypes
            ft = [wintypes.FILETIME() for _ in range(4)]
            if not ctypes.windll.kernel32.GetProcessTimes(wintypes.HANDLE(int(p._handle)), *[ctypes.byref(f) for f in ft]):
                return None
            return sum((f.dwHighDateTime << 32 | f.dwLowDateTime) for f in ft[2:]) / 1e7
        with open(f'/proc/{p.pid}/stat') as f:
            st = f.read().rsplit(')', 1)[1].split()
        return (int(st[11]) + int(st[12])) / os.sysconf('SC_CLK_TCK')
    except (OSError, ValueError, IndexError, AttributeError):
        return None


WALL_FACTOR = 2   # [UIT 0445] a compile under its limit in CPU time may take this many times the limit on the clock
BUSY_SHARE = 0.5  # stopped on the clock with less than this share of a core: the PC was busy, tried again next build


def run(cmd, seconds=None):
    """(exit code, stop) of the compile; exit code None when it was stopped, stop = (why, cpu s, clock s) with why 'cpu'
    (it used the limit in CPU time, or on the clock when its CPU time cannot be read) or 'busy' (it reached
    WALL_FACTOR x the limit on the clock with under BUSY_SHARE of a core). clang compiles in its own process (no
    separate cc1), so stopping it stops the compile."""
    p = subprocess.Popen(cmd)
    if seconds is None:
        return p.wait(), None
    t0 = time.monotonic()
    while True:
        try:
            return p.wait(timeout=min(1.0, seconds)), None
        except subprocess.TimeoutExpired:
            pass
        wall, cpu = time.monotonic() - t0, cpu_seconds(p)
        why = None
        if cpu is None:
            why = 'cpu' if wall > seconds else None
        elif cpu > seconds:
            why = 'cpu'
        elif wall > WALL_FACTOR * seconds:
            why = 'busy' if cpu < BUSY_SHARE * wall else 'cpu'
        if why:
            p.kill()
            p.wait()
            return None, (why, cpu, wall)


def main(argv):
    ext = '.try'
    if argv[:1] == ['--stamp'] and len(argv) > 1:   # [UIT 0370] the fast parts of split functions: OBJECT.split
        ext, argv = argv[1], argv[2:]
    if len(argv) < 6 or argv[3] != '--' or '--' not in argv[4:]:
        sys.exit(__doc__)
    seconds, source, obj = float(argv[0]), argv[1], argv[2]
    rest = argv[4:]
    cut = rest.index('--')
    opt, o0 = rest[:cut], rest[cut + 1:]
    stamp = pathlib.Path(obj + ext)
    key = key_of(source, opt, o0, seconds)
    old = {}
    try:
        old = json.loads(stamp.read_text())
    except (OSError, ValueError):
        pass
    name = pathlib.Path(source).stem
    busy = False
    if old.get('key') == key and old.get('level') == 'O0' and not old.get('busy'):
        why = old.get('why', '')   # known: the optimized build does not finish in time here
        print(f'  note: {name} built unoptimized (-O0) as before (optimized it {why})', flush=True)   # [UIT 0445]
    else:
        t0 = time.monotonic()
        rc, stop = run(opt, seconds)
        took = time.monotonic() - t0
        if rc == 0:
            stamp.write_text(json.dumps({'key': key, 'level': 'opt', 'seconds': round(took, 1)}))
            return 0
        if rc is not None:
            why = f'failed (exit code {rc})'
        elif stop[0] == 'busy':   # [UIT 0445]
            busy = True
            why = (f'got {stop[1]:.0f} s of CPU time in {stop[2]:.0f} s (the PC was busy)')
        else:
            why = f'took more than {seconds:g} s' + (' of CPU time' if stop[1] is not None else '')
        if busy:
            print(f'  note: {name}: its optimized build {why}; built unoptimized (-O0) for now, tried optimized again in '
                  f'the next build', flush=True)
        else:
            print(f'  note: {name} {why} optimized; built unoptimized (-O0) instead', flush=True)
    pathlib.Path(obj).unlink(missing_ok=True)
    rc, _ = run(o0)
    if rc == 0:
        stamp.write_text(json.dumps({'key': key, 'level': 'O0', 'why': why, **({'busy': True} if busy else {})}))
    else:
        stamp.unlink(missing_ok=True)
    return rc


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
