"""Translate the exported shaders with totk_xlate --serve (Ryujinx), in parallel on at most 8 logical CPUs.

usage: python tools/run_ryujinx_xlate.py data/shaders [--workers 8]
Writes <dir>/ryujinx/<key>.spv and <dir>/ryujinx-results.tsv (key, ok/fail, seconds, spv bytes, message).
"""
import argparse
import ctypes
import json
import os
import pathlib
import subprocess
import sys
import threading
import time

ROOT = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'tools'))
from toolchain import XLATE  # noqa: E402


def start_pinned(cmd, mask, env, err):
    CREATE_SUSPENDED, BELOW_NORMAL = 0x4, 0x4000
    p = subprocess.Popen(cmd, env=env, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=err,
                         text=True, creationflags=CREATE_SUSPENDED | BELOW_NORMAL)
    ctypes.windll.kernel32.SetProcessAffinityMask(int(p._handle), ctypes.c_size_t(mask))
    ctypes.windll.ntdll.NtResumeProcess(int(p._handle))
    return p


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('dir', type=pathlib.Path)
    ap.add_argument('--workers', type=int, default=8)
    ap.add_argument('--new-only', action='store_true', help='translate only keys without a .spv')
    ap.add_argument('--quiet', action='store_true')
    a = ap.parse_args()
    lines = (a.dir / 'jobs.jsonl').read_text().splitlines()
    if a.new_only:   # only shaders without a translation yet (the comparison's outputs stay)
        lines = [l for l in lines if not (a.dir / 'ryujinx' / (json.loads(l)['job']['key'] + '.spv')).exists()]
    (a.dir / 'ryujinx').mkdir(exist_ok=True)
    env = dict(os.environ, DOTNET_CLI_TELEMETRY_OPTOUT='1', DOTNET_NOLOGO='1',
               DOTNET_CLI_HOME=str(ROOT / 'third_party/dotnet-home'))
    if XLATE is None:
        sys.exit('the shader translator ultrs-shaders.exe is missing: python tools/build_xlate.py')
    cmd = [str(XLATE)]   # tools/build_xlate.py, or the builder package's deps/shaders
    results, lock = [], threading.Lock()
    shards = [lines[i::a.workers] for i in range(a.workers)]
    t0 = time.time()

    def worker(i, shard):
        if not shard: return
        # [UIT 0339] the translator's error output to a file: a translator that stops at once ('no reply' for every
        # shader) says why there, and the reason goes into the message
        errp = a.dir / f'ryujinx-stderr-{i}.txt'
        err = errp.open('w')
        p = start_pinned(cmd + ['--serve'], (1 << a.workers) - 1, env, err)

        def why():
            err.flush()
            code = p.poll()
            tail = [l.strip() for l in errp.read_text(errors='replace').splitlines() if l.strip()][-3:]
            return 'no reply' + (f' (translator exited with {code})' if code is not None else '') + (': ' + ' | '.join(tail) if tail else '')

        for line in shard:
            key = json.loads(line)['job']['key']
            s = time.perf_counter()
            try:
                p.stdin.write(line + '\n'); p.stdin.flush()
                reply = p.stdout.readline().strip()
            except OSError:
                reply = ''
            if not reply: reply = why()
            dt = time.perf_counter() - s
            spv = a.dir / 'ryujinx' / f'{key}.spv'
            ok = reply.startswith('ok ')
            size = spv.stat().st_size if ok and spv.exists() else 0
            msg = '' if ok else (reply[len('fail ' + key) + 1:] if reply.startswith('fail') else reply)
            with lock:
                results.append((key, 'ok' if ok else 'fail', dt, size, msg.replace('\t', ' ')[:300]))
                if len(results) % 2000 == 0 and not a.quiet:
                    print(f'{len(results)}/{len(lines)} {time.time() - t0:.0f}s', flush=True)
        try: p.stdin.close()
        except OSError: pass
        p.wait(); err.close()
        if errp.stat().st_size == 0: errp.unlink()

    threads = [threading.Thread(target=worker, args=(i, s)) for i, s in enumerate(shards)]
    for t in threads: t.start()
    for t in threads: t.join()
    with (a.dir / ('ryujinx-results-new.tsv' if a.new_only else 'ryujinx-results.tsv')).open('w') as f:   # --new-only keeps the full table
        f.write('key\tresult\tseconds\tspv_bytes\tmessage\n')
        for r in sorted(results):
            f.write(f'{r[0]}\t{r[1]}\t{r[2]:.4f}\t{r[3]}\t{r[4]}\n')
    ok = sum(r[1] == 'ok' for r in results)
    print(f'ryujinx: {ok}/{len(results)} ok, {time.time() - t0:.0f}s wall, {sum(r[2] for r in results):.0f}s busy')
    reasons = {}   # [UIT 0339] why the others failed: the most common messages with their counts
    for r in results:
        if r[1] != 'ok': reasons[r[4][:160]] = reasons.get(r[4][:160], 0) + 1
    for m, n in sorted(reasons.items(), key=lambda x: -x[1])[:5]:
        print(f'ryujinx: {n} failed: {m or "(no message)"}')


if __name__ == '__main__':
    main()
