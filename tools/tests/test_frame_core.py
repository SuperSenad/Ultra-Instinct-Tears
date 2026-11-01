"""[UIT 0237] Cuts plat_thread_frame() (and FRAME_CORE_STREAK) out of src/runtime/platform/windows.c, which only
builds for Windows, and runs it in tools/tests/test_frame_core.c with Windows-like shims, in every mode, plain and under
ThreadSanitizer (when the compiler has it). Also checks the wiring: hle_nvn.c calls plat_thread_frame() where it marks
the frame thread (acquire and present), plat_thread_pcores() uses the shared set when split, and the launcher key exists.
usage: python tools/tests/test_frame_core.py
"""
import pathlib
import re
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parent.parent.parent
fails = []


def check(cond, msg):
    if not cond:
        fails.append(msg)


win = (ROOT / 'src/runtime/platform/windows.c').read_text(errors='replace')
a = win.find('enum { FRAME_CORE_STREAK')
b = win.find('\n}\n', win.find('int plat_thread_frame(void) {'))
check(a >= 0 and b > a, 'plat_thread_frame not found in windows.c')
nvn = (ROOT / 'src/gfx/nvn/hle_nvn.c').read_text(errors='replace')
check(len(re.findall(r'uit_cp_frame_thread = 1;[^\n]*\n\s*plat_thread_frame\(\);', nvn)) == 2,
      'hle_nvn.c: plat_thread_frame() right after both uit_cp_frame_thread = 1 (acquire and present)')
check('__attribute__((weak)) int plat_thread_frame(void) { return 0; }' in nvn, 'hle_nvn.c: weak default')
pc = win[win.find('int plat_thread_pcores(void) {'):]
pc = pc[:pc.find('\n}\n')]
check('g_rids' in pc and 'g_nr' in pc, 'plat_thread_pcores: the shared set when split')
check('[TOTK_FRAME_CORE]' in (ROOT / 'launcher/settings.ini').read_text(), 'launcher key')
if a >= 0 and b > a:
    with tempfile.TemporaryDirectory() as d:
        src = pathlib.Path(d) / 'ptf.c'
        src.write_text(win[a:b + 3])
        for san in ([], ['-fsanitize=thread']):
            exe = pathlib.Path(d) / ('fc' + ('t' if san else ''))
            cmd = ['cc', '-O1', '-g', '-Wall', '-Werror', '-pthread', *san, f'-DFRAME_CORE_SRC="{src}"',
                   str(ROOT / 'tools/tests/test_frame_core.c'), '-o', str(exe)]
            r = subprocess.run(cmd, capture_output=True, text=True)
            if r.returncode:
                if san:
                    print('(no ThreadSanitizer here: skipped)'); continue
                fails.append('compile: ' + r.stderr[-2000:]); break
            for mode in ['', 'off', 'nosplit', 'fail', 'race']:
                for _ in range(20 if mode == 'race' else 1):
                    r = subprocess.run([str(exe), mode], capture_output=True, text=True, timeout=60)
                    if r.returncode or 'all ok' not in r.stdout:
                        fails.append(f'{"tsan " if san else ""}mode {mode or "default"}: {r.stdout[-800:]} {r.stderr[-1500:]}')
                        break
for f in fails:
    print('FAIL', f)
print('frame core wiring + behaviour: ' + ('FAILED' if fails else 'all ok'))
sys.exit(1 if fails else 0)
