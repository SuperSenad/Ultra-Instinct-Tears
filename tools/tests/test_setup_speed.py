"""Box test for the speed options of tools/setup.py (ThinLTO default, one-time migration, chosen options, the
ThinLTO-failed fallback, --pgo-guide) and tools/build_game.py's compile-then-link with its LTO retry."""
import pathlib, sys, tempfile, types
HERE = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(HERE / 'tools'))
import setup, build_game  # noqa: E402

ok = True
def check(c, what):
    global ok
    print(('ok   ' if c else 'FAIL ') + what); ok &= bool(c)

R = setup.resolve_speed
def flags(**kw):
    a = types.SimpleNamespace(pgo_profile=False, pgo=False, no_pgo=False, lto=False, no_lto=False, O3=False, O2=False)
    for k, v in kw.items(): setattr(a, k, v)
    return setup.speed_flags(a)

# new folder: the new default
st = {}; sp, notes = R(st, {})
check(sp == {'pgo': 'off', 'lto': 'on', 'opt': 'O2'} and not notes, 'new folder: ThinLTO on, O2, no PGO, no note')
check(st['speed-v'] == 3 and st['speed-chosen'] == [], 'new folder: state marked migrated, nothing chosen')
sp, notes = R(st, {}); check(sp['lto'] == 'on' and not notes, 'new folder, second run: same, no note')

# old folder that never chose anything (old setup saved the old default every run): migrated once, with a note
st = {'speed': {'pgo': 'off', 'lto': 'off', 'opt': 'O2'}}
sp, notes = R(st, {})
check(sp['lto'] == 'on' and len(notes) == 1 and 'now on by default' in notes[0], 'old folder: ThinLTO turned on once, note says so')
sp, notes = R(st, {}); check(sp['lto'] == 'on' and not notes, 'old folder: migration only once')

# old folder with choices that differ from the old default: kept
st = {'speed': {'pgo': 'use', 'lto': 'on', 'opt': 'O3'}}
sp, notes = R(st, {})
check(sp == {'pgo': 'use', 'lto': 'on', 'opt': 'O3'} and not notes, 'old folder with O3/PGO/LTO: all kept, no note')
check(st['speed-chosen'] == ['lto', 'opt', 'pgo'], 'old folder: those count as chosen')

# explicit --no-lto: kept forever, also against the migration on the same run
st = {'speed': {'pgo': 'off', 'lto': 'off', 'opt': 'O2'}}
sp, notes = R(st, flags(no_lto=True))
check(sp['lto'] == 'off' and not notes, 'old folder + --no-lto on the migrating run: stays off, no note')
sp, notes = R(st, {}); check(sp['lto'] == 'off', '--no-lto remembered on later runs')
check(len(notes) == 1 and 'earlier run' in notes[0] and '--lto' in notes[0], '[0142] later runs with LTO off by choice: a note says so and how to turn it on')
sp, notes = R(st, flags(no_lto=True)); check(sp['lto'] == 'off' and not notes, '[0142] --no-lto given again on this run: no note')
sp, _ = R(st, flags(lto=True)); check(sp['lto'] == 'on', '--lto turns it on again')
st = {}; R(st, flags(no_lto=True)); sp, _ = R(st, {}); check(sp['lto'] == 'off', 'new folder + --no-lto remembered')
st = {}; R(st, flags(O2=True)); sp, _ = R(st, {}); check(st['speed-chosen'] == ['opt'] and sp['opt'] == 'O2', '--O2 counts as a choice')

# [UIT 0148] speed-v 2 folder with a remembered --no-lto: reset once, with a note; a new --no-lto then sticks
st = {'speed': {'pgo': 'off', 'lto': 'off', 'opt': 'O2'}, 'speed-chosen': ['lto'], 'speed-v': 2}
sp, notes = R(st, {})
check(sp['lto'] == 'on' and len(notes) == 1 and 'reset once' in notes[0] and st['speed-v'] == 3, '[0148] v2 --no-lto: reset once to on, note')
sp, notes = R(st, {}); check(sp['lto'] == 'on' and not notes, '[0148] reset only once')
R(st, flags(no_lto=True)); sp, _ = R(st, {}); check(sp['lto'] == 'off', '[0148] --no-lto after the reset is remembered')
st = {'speed': {'pgo': 'off', 'lto': 'off', 'opt': 'O2'}, 'speed-chosen': ['lto'], 'speed-v': 2}
sp, notes = R(st, flags(no_lto=True)); check(sp['lto'] == 'off' and not notes, '[0148] v2 + --no-lto on this run: stays off')
st = {'speed': {'pgo': 'off', 'lto': 'off', 'opt': 'O2'}, 'speed-chosen': ['lto'], 'speed-v': 2, 'lto-failed': 'x'}
sp, _ = R(st, {}); check(sp['lto'] == 'off', '[0148] v2 after a failed ThinLTO link: stays off')
st = {'speed': {'pgo': 'use', 'lto': 'on', 'opt': 'O3'}, 'speed-chosen': ['lto', 'opt', 'pgo'], 'speed-v': 2}
sp, notes = R(st, {}); check(sp == {'pgo': 'use', 'lto': 'on', 'opt': 'O3'} and not notes, '[0148] v2 other choices kept')

# the ThinLTO link failed on this PC (set by step_compile): off until --lto/--no-lto
st = {}; R(st, {}); st['lto-failed'] = '2026-10-30 17:00'
sp, notes = R(st, {})
check(sp['lto'] == 'off' and any('failed' in n for n in notes), 'after a failed ThinLTO link: off, note says why')
st['speed-chosen'] = ['lto']; st['speed']['lto'] = 'on'
sp, _ = R(st, {}); check(sp['lto'] == 'off', 'failed link wins over an earlier --lto choice')
sp, notes = R(st, flags(lto=True)); check(sp['lto'] == 'on' and 'lto-failed' not in st and not notes, '--lto clears the failure and tries again')
st['lto-failed'] = 'x'; sp, _ = R(st, flags(no_lto=True)); check(sp['lto'] == 'off' and 'lto-failed' not in st, '--no-lto clears it too')

# PGO flags
st = {}; sp, _ = R(st, flags(pgo_profile=True)); check(sp['pgo'] == 'gen' and sp['lto'] == 'on', '--pgo-profile: gen, LTO default kept')
sp, _ = R(st, {}); check(sp['pgo'] == 'gen', 'gen remembered')
sp, _ = R(st, flags(pgo=True)); check(sp['pgo'] == 'use', '--pgo: use')
sp, _ = R(st, flags(no_pgo=True)); check(sp['pgo'] == 'off', '--no-pgo: off')

# [UIT 0149] pgo_state (the launcher's About > Faster game code (PGO))
with tempfile.TemporaryDirectory() as td:
    d = pathlib.Path(td)
    check(setup.pgo_state({}, d) == ('normal', 0), '[0149] pgo_state: new folder normal')
    (d / 'build/pgo').mkdir(parents=True); (d / 'build/pgo/a.profraw').write_text('x'); (d / 'build/pgo/uit.profdata').write_text('x')
    check(setup.pgo_state({'speed': {'pgo': 'gen'}}, d) == ('profiling', 1), '[0149] pgo_state: profiling with 1 session')
    check(setup.pgo_state({'speed': {'pgo': 'use'}, 'pgo-profile': 'build/pgo/uit.profdata'}, d) == ('fast', 1), '[0149] pgo_state: fast')
    check(setup.pgo_state({'speed': {'pgo': 'use'}, 'pgo-profile': 'gone'}, d)[0] == 'normal', '[0149] pgo_state: profile missing = normal')

# build_game: compile first, then link, retry the ThinLTO link once with fewer jobs, then exit code 3
def sim(results, lto):
    calls, retried = [], []
    it = iter(results)
    rc = build_game.compile_then_link(lambda t: (calls.append(t), next(it))[1], lto, lambda: retried.append(1))
    return rc, calls, len(retried)
check(sim([0, 0], True) == (0, ['objs', None], 0), 'build: compile then link, no retry')
check(sim([1], True) == (1, ['objs'], 0), 'compile failure: exit 1, no link, no LTO fallback')
check(sim([0, 1], False) == (1, ['objs', None], 0), 'normal link failure: exit 1, no retry')
check(sim([0, 1, 0], True) == (0, ['objs', None, None], 1), 'ThinLTO link failed, retry with fewer jobs worked: 0')
check(sim([0, 1, 1], True) == (build_game.LTO_LINK_FAILED, ['objs', None, None], 1), 'ThinLTO link failed twice: exit code 3')
check(build_game.LTO_LINK_FAILED == setup.LTO_LINK_FAILED, 'setup and build_game agree on the exit code')
check([build_game.lto_retry_jobs(n) for n in (1, 4, 24)] == [1, 1, 6], 'retry jobs: a quarter, at least 1')

# setup.step_compile: falls back to the normal link by itself when build_game says 3
cmds = []
def fake_rc(cmd):
    cmds.append([str(c) for c in cmd]); return setup.LTO_LINK_FAILED if len(cmds) == 1 else 0
saved = (setup.run_rc, setup.save_state, setup.subprocess.run, setup.DLSS_MODE)
class P: returncode = 0
setup.run_rc, setup.save_state = fake_rc, lambda s: None
setup.subprocess.run = lambda *a, **k: P()
setup.DLSS_MODE = 'off'
setup.SPEED.update({'pgo': 'off', 'lto': 'on', 'opt': 'O2'})
st = {'speed': {'pgo': 'off', 'lto': 'on', 'opt': 'O2'}}
res = setup.step_compile(None, st, 8)
setup.run_rc, setup.save_state, setup.subprocess.run, setup.DLSS_MODE = saved
lto_of = lambda c: c[c.index('--lto') + 1]
check(len(cmds) == 2 and lto_of(cmds[0]) == 'thin' and lto_of(cmds[1]) == 'off', 'step_compile: ThinLTO build, then the normal link')
check('lto-failed' in st and st['speed']['lto'] == 'off' and setup.SPEED['lto'] == 'off', 'step_compile: the failure remembered')
check('ThinLTO link failed' in res, 'step_compile: the result line says so plainly')
sp, notes = R(st, {}); check(sp['lto'] == 'off' and notes, 'next run: stays on the normal link, with a note')

# --pgo-guide
with tempfile.TemporaryDirectory() as d:
    h = pathlib.Path(d)
    g = setup.pgo_guide({}, h); check('Next: step 1' in g and '--pgo-profile' in g, 'guide: normal build -> step 1')
    (h / 'build/pgo').mkdir(parents=True); (h / 'build/pgo/a.profraw').write_bytes(b'x' * 10)
    g = setup.pgo_guide({'speed': {'pgo': 'gen'}}, h); check('PROFILING build' in g and '1 session' in g and 'step 3' in g, 'guide: profiling build with counts -> step 3')
    (h / 'build/pgo/uit-1.profdata').write_bytes(b'x')
    g = setup.pgo_guide({'speed': {'pgo': 'use'}, 'pgo-profile': 'build/pgo/uit-1.profdata'}, h); check('fast PGO build is in use' in g, 'guide: PGO in use')

print('all ok' if ok else 'FAILURES'); sys.exit(0 if ok else 1)
