"""[UIT 0081] tools/warm_shaders.py: the progress setup.py --warm-shaders shows, and how it ends, with a stand-in for
the game that prints the lines src/gfx/vkb_shader.c prints (no GPU, no game files).
    py -3 tools/tests/test_warm_shaders.py"""
import os
import pathlib
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parent.parent.parent
sys.path.insert(0, str(ROOT / 'tools'))
import warm_shaders  # noqa: E402

FAKE = r'''
import os, sys, time
mode = sys.argv[1]
assert os.environ.get('TOTK_WARM_ONLY') == '1'
p = lambda s: (sys.stderr.write('[vk] ' + s + '\n'), sys.stderr.flush())
p('Ultra Instinct Tears test')
if mode == 'off':
    p('shader warm-up: off (TOTK_SHADER_PREWARM=0)')
    p('shader warm-up only (TOTK_WARM_ONLY): done, 0 shaders and 0 variants made in 0.0 s; the game exits'); sys.exit(0)
if mode == 'noboot':
    time.sleep(30); sys.exit(0)
p('shader list: 100 shaders to make ahead (100 used before, 0 of the build\'s), 3 integer-input variants, 6 threads, waiting up to 1000000000 s')
for i in range(0, 101, 5):
    p(f'shader warm-up: {i} of 100 shaders, {i / 10:.0f} s, 20.0 GiB memory free (RAM + page file)')
    if mode == 'stall' and i == 50:
        time.sleep(30)
    if mode == 'crash' and i == 40:
        sys.exit(3)
p('shaders made ahead: 100 of 100 listed, 3 integer-input variants of 3')
p('shader warm-up only (TOTK_WARM_ONLY): done, 100 shaders and 3 variants made in 9.5 s; the game exits')
'''


def main():
    fails = []

    def check(ok, what):
        if not ok:
            fails.append(what)
            print('FAIL', what)

    with tempfile.TemporaryDirectory() as td:
        td = pathlib.Path(td)
        fake = td / 'fake_game.py'
        fake.write_text(FAKE)
        log = td / 'build/warm-shaders.log'

        def go(mode, **kw):
            said = []
            ok, msg = warm_shaders.run([sys.executable, fake, mode], td, said.append, log, **kw)
            return ok, msg, said

        ok, msg, said = go('normal')
        check(ok and '100 shaders and 3 variants' in msg, f'normal: {msg}')
        prog = [s for s in said if s.startswith('  shaders: ') and ' of ' in s]
        check(len(prog) == 11 and prog[-1].endswith('(100 %)'), f'normal: progress every 10 % {prog}')
        check(any('100 to make' in s for s in said), 'normal: the list size is shown')
        check(log.is_file() and b'done, 100 shaders' in log.read_bytes(), 'normal: the whole output in the log')

        ok, msg, _ = go('off')
        check(ok and 'is off' in msg, f'off: {msg}')
        ok, msg, _ = go('crash')
        check(not ok and 'code 3' in msg, f'crash: {msg}')
        ok, msg, _ = go('stall', stall_s=1)
        check(not ok and 'no progress for 1 s' in msg, f'stall: {msg}')
        ok, msg, _ = go('noboot', boot_s=1)
        check(not ok and 'did not start its warm-up' in msg, f'noboot: {msg}')
        ok, msg = warm_shaders.run([td / 'missing.exe'], td, print, log)
        check(not ok and 'cannot start' in msg, f'missing exe: {msg}')

        # the shader folder and the list, as the game finds them
        check(warm_shaders.shader_dir(td) == td / 'data/shaders/ryujinx', 'shader_dir default')
        (td / 'ultrainstincttears.cfg').write_text('exefs=data/exefs\nshaders=elsewhere/sh\nTOTK_SETUP_REWARM=1\n')
        check(warm_shaders.shader_dir(td) == td / 'elsewhere/sh', 'shader_dir from the cfg')
        check(warm_shaders.listed(td) == 0, 'listed: no list yet')
        (td / 'elsewhere/sh').mkdir(parents=True)
        (td / 'elsewhere/sh/used.txt').write_text(('a' * 64 + ' 0\n') * 7 + 'junk\n')
        check(warm_shaders.listed(td) == 7, 'listed: 7')
        os.environ.pop('TOTK_SETUP_REWARM', None)
        check(warm_shaders.setting(td, 'TOTK_SETUP_REWARM') == '1', 'setting from the cfg')
        os.environ['TOTK_SETUP_REWARM'] = '0'
        check(warm_shaders.setting(td, 'TOTK_SETUP_REWARM') == '0', 'the environment wins')
        del os.environ['TOTK_SETUP_REWARM']

        # the stamp: a rebuilt exe or newly translated shaders make it stale
        exe = td / 'Game.exe'
        exe.write_bytes(b'1')
        s0 = warm_shaders.stamp(td, 'Game.exe', {'shaders-xlate': 'a'})
        check(s0 == warm_shaders.stamp(td, 'Game.exe', {'shaders-xlate': 'a'}), 'stamp: same build, same stamp')
        check(s0 != warm_shaders.stamp(td, 'Game.exe', {'shaders-xlate': 'b'}), 'stamp: new shaders')
        exe.write_bytes(b'22')
        check(s0 != warm_shaders.stamp(td, 'Game.exe', {'shaders-xlate': 'a'}), 'stamp: rebuilt exe')
    print('FAIL' if fails else 'PASS')
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())
