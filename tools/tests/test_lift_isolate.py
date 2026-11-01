"""Box test for UIT 0126 (tools/lifter/gen.py chunk_files): with no function marked to stand alone the files are
exactly the ones the old packing loop made (random sizes, chunk / byte limits and big functions); a marked function
(an unresolved computed branch: built at -O0) is always alone in its file, and every function is in exactly one file,
in address order. TOTK_LIFT_ISOLATE_O0=0 turns the marking off.
   py -3 tools/tests/test_lift_isolate.py"""
import importlib, os, pathlib, random, sys
HERE = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(HERE / 'tools' / 'lifter'))
import gen

ok = True
def check(c, w):
    global ok; print(('ok   ' if c else 'FAIL ') + w); ok &= bool(c)


def old_packing(fs, chunk, chunk_bytes, big_fn):
    """The loop gen.py had before UIT 0126 (pending = a big function that starts the next file)."""
    out, ci, pending = [], 0, None
    while ci < len(fs) or pending:
        fns, size = [], 0
        while (pending or ci < len(fs)) and len(fns) < chunk and size < chunk_bytes:
            if pending:
                f, pending = pending, None
            else:
                f = fs[ci]; ci += 1
            n = sum(len(l) + 1 for l in f[1])
            if n > big_fn and fns:
                pending = f
                break
            fns.append(f); size += n
            if n > big_fn:
                break
        out.append([f[0] for f in fns])
    return out


rng = random.Random(130)
same, trials = 0, 400
for t in range(trials):
    fs = [(i, ['x' * rng.choice((10, 50, 200, 900, 5000))] * rng.randint(1, 6), set(), False) for i in range(rng.randint(1, 300))]
    chunk, cb, big = rng.choice((1, 3, 20, 1500)), rng.choice((500, 3000, 20000)), rng.choice((1000, 8000, 100000))
    got = [[f[0] for f in g] for g in gen.chunk_files(iter(fs), chunk, cb, big)]
    same += got == old_packing(fs, chunk, cb, big)
check(same == trials, f'nothing marked: the same files as the old packing ({same}/{trials} random layouts)')

alone_ok = order_ok = True
for t in range(trials):
    fs = [(i, ['x' * rng.choice((10, 50, 200, 900))] * rng.randint(1, 6), set(), rng.random() < 0.05) for i in range(rng.randint(1, 300))]
    groups = list(gen.chunk_files(iter(fs), rng.choice((3, 20, 1500)), rng.choice((3000, 20000)), 8000))
    flat = [f[0] for g in groups for f in g]
    order_ok &= flat == [f[0] for f in fs] and all(groups)
    alone_ok &= all(len(g) == 1 for g in groups for f in g if f[3])
check(order_ok, 'every function in exactly one file, in address order, no empty file')
check(alone_ok, 'a function with unresolved jumps is alone in its file')

# the 2026-10-30 shape: one small all-labels function among 1499 ordinary ones went to -O0 with its whole file
fs = [(i, ['x' * 40] * 30, set(), i == 700) for i in range(1500)]
groups = [[f[0] for f in g] for g in gen.chunk_files(iter(fs), 1500, 10 ** 9, 100000)]
check(groups == [list(range(700)), [700], list(range(701, 1500))], 'one marked function: 3 files, the other 1499 functions outside its file')

os.environ.pop('TOTK_LIFT_ISOLATE_O0', None)
check(not importlib.reload(gen).ISOLATE_ALL_LABELS, 'off by default (UIT 0134: packed as before)')
os.environ['TOTK_LIFT_ISOLATE_O0'] = '0'
check(not importlib.reload(gen).ISOLATE_ALL_LABELS, 'TOTK_LIFT_ISOLATE_O0=0: off (packed as before)')
os.environ['TOTK_LIFT_ISOLATE_O0'] = '1'
check(importlib.reload(gen).ISOLATE_ALL_LABELS, 'TOTK_LIFT_ISOLATE_O0=1: on')
print('ALL OK' if ok else 'FAILURES')
sys.exit(0 if ok else 1)
