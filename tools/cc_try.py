"""[UIT 0359] Compile one of the game files that would be built unoptimized (-O0) with optimization first, under a
time limit, and fall back to -O0 when that takes too long or fails.

    python tools/cc_try.py [--stamp .split] SECONDS SOURCE OBJECT -- <optimized compile> -- <-O0 compile>

Those files hold a function that is one big switch over all its instructions; clang -O1 takes from a minute to hours
on such a file, -O0 seconds (tools/build_game.py). The outcome is kept in OBJECT.try with a key of the source, both
commands and the limit, so a file that timed out once goes straight to -O0 in later builds until one of them changes.
Exit code: the one of the compile that made OBJECT. Prints one line when it falls back.
[UIT 0370] --stamp .split: tools/build_game.py's rule for the fast part of a split function (OBJECT.split).
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


def run(cmd, seconds=None):
    """The compile's exit code; None when it ran past the limit (it is stopped). clang compiles in its own process
    (no separate cc1), so stopping it stops the compile."""
    p = subprocess.Popen(cmd)
    try:
        return p.wait(timeout=seconds)
    except subprocess.TimeoutExpired:
        p.kill()
        p.wait()
        return None


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
    if old.get('key') == key and old.get('level') == 'O0':
        why = old.get('why', '')   # known: the optimized build does not finish in time here
    else:
        t0 = time.monotonic()
        rc = run(opt, seconds)
        took = time.monotonic() - t0
        if rc == 0:
            stamp.write_text(json.dumps({'key': key, 'level': 'opt', 'seconds': round(took, 1)}))
            return 0
        why = f'took more than {seconds:g} s' if rc is None else f'failed (exit code {rc})'
        print(f'  note: {name} {why} optimized; built unoptimized (-O0) instead', flush=True)
    pathlib.Path(obj).unlink(missing_ok=True)
    rc = run(o0)
    if rc == 0:
        stamp.write_text(json.dumps({'key': key, 'level': 'O0', 'why': why}))
    else:
        stamp.unlink(missing_ok=True)
    return rc


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
