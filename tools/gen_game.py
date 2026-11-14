"""Lift the three TOTK code modules to C (tools/lifter/gen.py), in parallel.

usage: python tools/gen_game.py <exefs dir> <output dir>
Bases must match src/runtime/rt.h (MAIN_BASE, SUBSDK0_BASE, SDK_BASE).
"""
import pathlib
import re
import subprocess
import sys
import time

ROOT = pathlib.Path(__file__).resolve().parent.parent
PROGRESS = re.compile(r': progress (\d+)/(\d+) files')
MODULES = {'main': 0x0800000000, 'subsdk0': 0x0804000000, 'sdk': 0x0805000000}


PHASES = ('reading', 'finding', 'looking for the places', 'looking for table', 'writing')   # tools/lifter/gen.py phase()


def last_lines(path, n=32768):
    try:
        with open(path, 'rb') as f:
            f.seek(0, 2)
            f.seek(max(0, f.tell() - n))
            return f.read().decode('utf-8', 'replace').splitlines()
    except OSError:
        return []


def module_state(path, was):
    """The newest phase ("finding the functions (pass 2, 40%)") and "progress i/N" count in one module's log
    (tools/lifter/gen.py); was: the last state, kept when the tail no longer shows them."""
    ph, cnt = was
    got_ph = got_cnt = False
    for line in reversed(last_lines(path)):
        if not got_cnt:
            m = PROGRESS.search(line)
            if m:
                cnt, got_cnt = (int(m.group(1)), int(m.group(2))), True
        if not got_ph and ': phase ' in line:
            ph, got_ph = line.split(': phase ', 1)[1].strip(), True
        if got_ph and got_cnt:
            break
    return ph, cnt


def rank(ph):
    return next((k for k, p in enumerate(PHASES) if ph and ph.startswith(p)), -1)


def watch_progress(out, procs, every=2.0):
    """[UIT 0430] While the modules are lifted, for the update screen (tools/update.py COUNT_LINES): what the
    slowest module is doing ("translating the code: now finding the functions (pass 1, 40%) in main") and, while the
    C files are written, "translating the code: i of N functions". Only reads the logs, only prints changes."""
    states = {name: (None, None) for name, _, _ in procs}
    shown_ph = shown_cnt = None
    while any(p.poll() is None for _, _, p in procs):
        time.sleep(every)
        for name, _, p in procs:
            states[name] = module_state(out / f'{name}.log', states[name])
        running = [n for n, _, p in procs if p.poll() is None] or list(states)
        slow = min(running, key=lambda n: rank(states[n][0]))
        ph = states[slow][0]
        if ph and (ph, slow) != shown_ph:
            shown_ph = (ph, slow)
            print(f'  translating the code: now {ph} in {slow}', flush=True)
        known = [c for _, c in states.values() if c]
        if known:
            done, total = sum(c[0] for c in known), sum(c[1] for c in known)
            if total and (done, total) != shown_cnt:
                shown_cnt = (done, total)
                print(f'  translating the code: {done} of {total} functions', flush=True)


def main():
    exefs, out = pathlib.Path(sys.argv[1]), pathlib.Path(sys.argv[2])
    t0 = time.time()
    procs = []
    for name, base in MODULES.items():
        (out / name).mkdir(parents=True, exist_ok=True)
        log = open(out / f'{name}.log', 'w')
        procs.append((name, log, subprocess.Popen(
            [sys.executable, str(ROOT / 'tools/lifter/gen.py'), name, str(exefs / name), hex(base), str(out / name),
             '--simd', 'c'], stdin=subprocess.DEVNULL, stdout=log, stderr=subprocess.STDOUT)))
    watch_progress(out, procs)
    failed = False
    for name, log, p in procs:
        code = p.wait()
        log.close()
        summary = [l for l in (out / f'{name}.log').read_text().splitlines() if 'progress' not in l and ': phase ' not in l]
        print(f'{name}: exit {code}; ' + ' | '.join(summary[-2:]))
        for line in summary:   # [TOTK] an optional patch the lift skipped (tools/lifter/patches.py)
            if line.startswith('WARNING:'):
                print(f'{name}: {line}')
        failed |= code != 0
    print(f'{time.time() - t0:.0f}s')
    sys.exit(1 if failed else 0)


if __name__ == '__main__':
    main()
