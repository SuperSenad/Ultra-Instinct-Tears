"""[UIT 0235] The graphics code built as tools/build_game.py builds it for Windows (its flags: GFX_WARN), checked for
functions that call themselves on every path, plus the 0232 bug put back to show the check catches it.

0232 replaced every "pthread_mutex_unlock(&g_mu)" in src/gfx/vkb_tex.c with img_unlock(), including the one inside
img_unlock: at -O2 a jump to itself. The first thread to let go of the images lock spun forever holding it and the game
booted to a black screen (6102d92, with the four 0230-0234 switches on or off). The graphics code was built with -w,
which hid clang's "all paths through this function will call itself". Nothing on the box runs vkb_tex.c (Windows only),
so this compiles it for Windows with the builder's headers.

usage: python tools/tests/test_gfx_selfcall.py --builder <builder>/deps/clang64 --clang <LLVM>/bin [-j N]
"""
import argparse
import concurrent.futures
import os
import pathlib
import re
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent.parent
sys.path.insert(0, str(ROOT / 'tools'))
import build_game as bg  # noqa: E402


def compile_cmd(clang, sysroot, src, extra=()):
    cxx = src.endswith('.cpp')
    major = next(iter(sorted((pathlib.Path(sysroot) / 'lib/clang').iterdir()))).name   # the builder's clang headers
    cmd = [str(pathlib.Path(clang) / ('clang++' if cxx else 'clang')), '--target=x86_64-w64-windows-gnu', f'--sysroot={sysroot}',
           f'-resource-dir={sysroot}/lib/clang/{major}', '-stdlib=libc++', '-fsyntax-only', *bg.CFLAGS.split(), *bg.GFX_WARN.split(), *bg.GFX_INCLUDES.split(), '-Ibuild', *extra]
    if cxx:
        cmd += ['-std=c++17', '-fno-exceptions', '-fno-rtti']
    return cmd


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--builder', required=True, help='the builder\'s deps/clang64 (Windows headers)')
    ap.add_argument('--clang', required=True, help='LLVM bin directory')
    ap.add_argument('-j', type=int, default=os.cpu_count() or 4)
    a = ap.parse_args()
    if '-Werror=infinite-recursion' not in bg.GFX_WARN.split() or '-w' in bg.GFX_WARN.split():
        sys.exit('FAIL: build_game.GFX_WARN does not stop the build on infinite recursion')
    bad = 0
    src = (ROOT / 'src/gfx/vkb_tex.c').read_text(encoding='utf-8')
    head = 'static void img_unlock(void) {'
    i = src.index(head)
    end = src.index('\n}', i) + 2
    func = src[i:end]

    # 1. the unlock really unlocks (and calls nothing named img_unlock)
    body = re.sub(r'/\*.*?\*/|//[^\n]*', '', func[len(head):], flags=re.S)   # code only
    if 'img_unlock(' in body or 'pthread_mutex_unlock(&g_mu);' not in body:
        bad += 1
        print('FAIL: img_unlock does not end in pthread_mutex_unlock(&g_mu):\n' + func)

    # 2. the 0232 bug put back (img_unlock calling itself): the build's flags must refuse it
    mutant = src[:i] + func.replace('pthread_mutex_unlock(&g_mu);', 'img_unlock();') + src[end:]
    cmd = compile_cmd(a.clang, a.builder, 'src/gfx/vkb_tex.c', ['-iquote', 'src/gfx', '-x', 'c', '-'])
    r = subprocess.run(cmd, cwd=ROOT, input=mutant, capture_output=True, text=True)
    if r.returncode != 0 and 'infinite-recursion' in r.stderr:
        line = next(l for l in r.stderr.splitlines() if 'call itself' in l)
        print('ok: the 0232 img_unlock (calling itself) stops the build: ' + line.split(': ', 1)[-1])
    else:
        bad += 1
        print(f'FAIL: the 0232 img_unlock was not refused (exit {r.returncode})\n{r.stderr[-2000:]}')

    # 3. every graphics source as built: no errors (so no function calling itself on every path)
    def one(s):
        rr = subprocess.run(compile_cmd(a.clang, a.builder, s, [s]), cwd=ROOT, capture_output=True, text=True)
        return s, rr.returncode, rr.stderr
    with concurrent.futures.ThreadPoolExecutor(a.j) as ex:
        res = list(ex.map(one, bg.GFX))
    for s, rc, err in res:
        if rc:
            bad += 1
            print(f'FAIL: {s}\n{err[-2000:]}')
    print(f'{len(res)} graphics sources: {sum(1 for _, rc, _ in res if rc == 0)} build clean')
    print('test_gfx_selfcall: ' + ('PASS' if not bad else f'{bad} FAILED'))
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
