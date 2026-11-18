"""[UIT 0447] "Optimize the big functions with unresolved jumps" (TOTK_LIFT_SPLIT_HUGE) is on by default: for a new
install and for a settings file that never set it, setup translates with the split; the launcher writes a player's Off
as 0, which setup keeps (also from the environment); the build record says what the build used (1 or 0), not "not set".
usage: python tools/tests/test_split_default.py
"""
import os
import pathlib
import shutil
import sys
import tempfile

HERE = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(HERE / 'tools'))
import setup  # noqa: E402

fails = []


def check(c, what):
    print(('ok   ' if c else 'FAIL ') + what)
    if not c:
        fails.append(what)


# the launcher's side: default on, Off written as 0 (a value setup reads as off)
ini = (HERE / 'launcher/settings.ini').read_text(errors='replace').replace('\r\n', '\n')
a = ini.index('[TOTK_LIFT_SPLIT_HUGE]\n'); sec = ini[a:ini.index('\n\n', a)]
fields = dict(l.split(' = ', 1) for l in sec.splitlines()[1:] if ' = ' in l)
check(fields.get('default') == 'on' and fields.get('on') == '1' and fields.get('off') == '0', 'settings.ini: default on, On 1, Off 0')
check(fields.get('rebuild') == 'yes' and fields.get('type') == 'bool', 'settings.ini: still a rebuild toggle')
check('off by default' not in sec.lower(), 'settings.ini: the texts no longer say off by default')

t = pathlib.Path(tempfile.mkdtemp(prefix='uit-split-'))
saved_root, saved_env = setup.ROOT, os.environ.pop('TOTK_LIFT_SPLIT_HUGE', None)
try:
    setup.ROOT = t
    (t / 'launcher').mkdir()
    shutil.copy(HERE / 'launcher/settings.ini', t / 'launcher/settings.ini')
    cfg = t / 'ultrainstincttears.cfg'
    check(setup.lift_options().get('TOTK_LIFT_SPLIT_HUGE') == '1', 'new install (no settings file): on')
    for text, want, what in (('TOTK_FPS=60\n', '1', 'never set'), ('TOTK_LIFT_SPLIT_HUGE=\n', '1', 'empty'),
                             ('TOTK_LIFT_SPLIT_HUGE=-\n', '1', '"-" (left at the default)'),
                             ('TOTK_LIFT_SPLIT_HUGE=1\n', '1', 'On'), ('TOTK_LIFT_SPLIT_HUGE=0\n', '0', 'the player\'s Off')):
        cfg.write_text('# x\n' + text)
        got = setup.lift_options().get('TOTK_LIFT_SPLIT_HUGE')
        check(got == want, f'settings file {what}: {got!r} (want {want!r})')
    cfg.write_text('TOTK_FPS=60\n')
    os.environ['TOTK_LIFT_SPLIT_HUGE'] = '0'
    check(setup.lift_options().get('TOTK_LIFT_SPLIT_HUGE') == '0', 'the environment\'s 0 wins over the default')
    del os.environ['TOTK_LIFT_SPLIT_HUGE']
    check(setup.lift_options().get('TOTK_LIFT_REGCACHE') is None, 'other lift options stay unset when not set (old stamps kept)')
    # the record
    for text, want in (('TOTK_FPS=60\n', '1'), ('TOTK_LIFT_SPLIT_HUGE=0\n', '0')):
        cfg.write_text(text)
        setup.write_built_settings(t)
        rec = dict(l.split('=', 1) for l in (t / 'data/built-settings.txt').read_text().splitlines() if l and not l.startswith('#'))
        check(rec.get('TOTK_LIFT_SPLIT_HUGE') == want, f'record for {text.strip()}: {rec.get("TOTK_LIFT_SPLIT_HUGE")!r}')
    # the lifter reads 0 as off, 1 as on
    import importlib
    for v, want in (('0', False), ('1', True)):
        os.environ['TOTK_LIFT_SPLIT_HUGE'] = v
        sys.path.insert(0, str(HERE / 'tools/lifter'))
        import gen  # noqa: E402
        importlib.reload(gen)
        check(gen.SPLIT_HUGE is want, f'lifter: {v} -> {want}')
finally:
    setup.ROOT = saved_root
    os.environ.pop('TOTK_LIFT_SPLIT_HUGE', None)
    if saved_env is not None:
        os.environ['TOTK_LIFT_SPLIT_HUGE'] = saved_env
    shutil.rmtree(t, ignore_errors=True)

if fails:
    print(f'test_split_default: {len(fails)} FAILED')
    sys.exit(1)
print('test_split_default: all passed')
