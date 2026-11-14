"""[UIT 0375] Box test for tools/setup.py's fallback when the split of the big functions (TOTK_LIFT_SPLIT_HUGE) does
not compile: the code is translated again without the split, the compile is retried, and the same translation (same
lifter, same options) does not try the split again; a changed lifter or option tries it again."""
import os, pathlib, sys, tempfile
HERE = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(HERE / 'tools'))
import setup  # noqa: E402

ok = True
def check(c, what):
    global ok
    print(('ok   ' if c else 'FAIL ') + what); ok &= bool(c)

with tempfile.TemporaryDirectory() as d:
    d = pathlib.Path(d)
    (d / 'data/generated/main').mkdir(parents=True)
    (d / 'dump/exefs').mkdir(parents=True)
    setup.DATA, setup.ROOT = d / 'data', d
    setup.EXEFS = {}
    lifts, said = [], []
    setup.run = lambda cmd, what: lifts.append((what, os.environ.get('TOTK_LIFT_SPLIT_HUGE'))) or 0.0
    setup.say = lambda s: said.append(s)
    setup.save_state = lambda st: None
    hash_ = ['lifter-a']
    setup.tree_hash = lambda files: hash_[0]
    opts = {'TOTK_LIFT_SPLIT_HUGE': '1'}
    setup.lift_options = lambda: dict(opts)

    st = {}
    check(setup.step_lift(d / 'dump', st, 4) == 'done' and lifts[-1][1] == '1', 'split on: lifted with the split')
    check(st['lift-split'] == st['lift'], 'the state notes that this translation has the split')
    check(setup.relift_unsplit(st) and lifts[-1] == ('translating the code (without the split)', '0'),
          'compile failed: translated again with TOTK_LIFT_SPLIT_HUGE=0')
    check(st['split-failed'] and st['lift'].endswith('|split-off') and not st['lift-split'],
          'the failed translation is remembered; the new one is marked split-off')
    check(not setup.relift_unsplit(st), 'a second failure is not retried (the split is already off)')
    n = len(lifts)
    check(setup.step_lift(d / 'dump', st, 4) == 'up to date' and len(lifts) == n,
          'next setup, same version: the split stays off and nothing is translated again')
    check(any('skipped' in s for s in said) and os.environ['TOTK_LIFT_SPLIT_HUGE'] == '0', 'a note says the split is skipped')
    hash_[0] = 'lifter-b'
    check(setup.step_lift(d / 'dump', st, 4) == 'done' and lifts[-1][1] == '1' and st['lift-split'],
          'a new lifter: the split is tried again')
    opts = {}
    os.environ.pop('TOTK_LIFT_SPLIT_HUGE', None)
    setup.step_lift(d / 'dump', st, 4)
    check(not st['lift-split'] and not setup.relift_unsplit(st), 'split off: a compile failure is not retried')
print('test_setup_split_fallback: ' + ('PASS' if ok else 'FAIL'))
sys.exit(0 if ok else 1)
