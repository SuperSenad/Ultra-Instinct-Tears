"""[UIT 0250] TOTK_VK_RETIRE_PCORES: the retire thread (GPU done -> copy GPU writes back -> fence complete -> the game's
fence wait returns) takes the shared performance cores (plat_thread_pcores), as the submit and recording threads.
Checks:
  - the switch block cut from backend_vk.c, compiled with a stand-in plat_thread_pcores: unset / "" / "1" / "00" call it
    once; exactly "0" does not (g_ret_pc 0, as before); the placement it returns is kept for the log line
  - identical work: the retire loop of backend_vk.c with the [UIT 0250] lines taken out is byte for byte the loop of the
    base commit 4b42a20 (nothing else moved; complete() still before dead_reap, the timing reads only)
  - wiring: the timing starts after vkWaitSemaphores and is added after complete(); the per-frame line; the launcher key
Negative probes (each must be caught): a switch that turns off on any string starting with 0, a placement called
unconditionally, and a retire loop with one extra statement.
usage: python tools/tests/test_retire_pcores.py
"""
import pathlib
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parent.parent.parent
fails = []


def check(cond, msg):
    if not cond:
        fails.append(msg)


back = (ROOT / 'src/gfx/backend_vk.c').read_text()


def cut_switch(text):
    a = text.find('    {\n        const char *e = getenv("TOTK_VK_RETIRE_PCORES");')
    b = text.find('\n    }\n', a)
    return text[a:b + 7] if a >= 0 and b > a else ''


def loop(text):
    a = text.find('static void *retire_thread(void *unused) {')
    b = text.find('void retire_start(void) {', a)
    return text[a:b] if a >= 0 and b > a else ''


def strip0250(t):
    out, skip = [], False
    lines = t.split('\n')
    i = 0
    while i < len(lines):
        ln = lines[i]
        if ln.startswith('    // [UIT 0250] TOTK_VK_RETIRE_PCORES'):   # the comment + the switch block
            while not lines[i].startswith('    {'):
                i += 1
            while lines[i] != '    }':
                i += 1
            i += 1
            continue
        if '[UIT 0250]' in ln or 'g_ret_n, 1' in ln:
            i += 1
            continue
        out.append(ln)
        i += 1
    return '\n'.join(out)


HARNESS = r'''
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define VLOG(...) ((void)0)
static int calls;
int plat_thread_pcores(void) { calls++; return 12; }
static int g_ret_pc = -1;
static void body(void) {
%s
}
int main(int argc, char **argv) {
    if (argc > 1) setenv("TOTK_VK_RETIRE_PCORES", argv[1], 1); else unsetenv("TOTK_VK_RETIRE_PCORES");
    body();
    printf("%%d %%d\n", calls, g_ret_pc);
    return 0;
}
'''


def run_switch(block):
    with tempfile.TemporaryDirectory() as d:
        c = pathlib.Path(d) / 'h.c'
        c.write_text(HARNESS % block)
        exe = pathlib.Path(d) / 'h'
        r = subprocess.run(['cc', '-O1', '-Wall', '-Werror', '-o', str(exe), str(c)], capture_output=True, text=True)
        if r.returncode:
            return ['compile: ' + r.stderr[:400]]
        bad = []
        for arg, want in ((None, '1 12'), ('', '1 12'), ('1', '1 12'), ('00', '1 12'), ('0', '0 0')):
            out = subprocess.run([str(exe)] + ([] if arg is None else [arg]), capture_output=True, text=True).stdout.strip()
            if out != want:
                bad.append(f'switch {arg!r}: got {out!r}, want {want!r}')
        return bad


def base_loop():
    r = subprocess.run(['git', '-C', str(ROOT), 'show', '4b42a20:src/gfx/backend_vk.c'], capture_output=True, text=True)
    return loop(r.stdout) if r.returncode == 0 else None


def same_loop(text, base):
    return strip0250(loop(text)) == base


def wiring(text):
    bad = []
    lp = loop(text)
    w = lp.find('vk.vkWaitSemaphores(vkc.device, &wi, UINT64_MAX);')
    t0 = lp.find('uint64_t rt0 = runtime_now_ns();')
    cp = lp.find('complete(j->fence);')
    ad = lp.find('atomic_fetch_add_explicit(&g_ret_ns, runtime_now_ns() - rt0')
    dr = lp.find('dead_reap(j->fence);')
    if not (0 <= w < t0 < cp < ad < dr): bad.append('timing order (wait < rt0 < complete < add < dead_reap)')
    if lp.find('getenv("TOTK_VK_RETIRE_PCORES")') > lp.find('for (;;)'): bad.append('switch not before the loop')
    if 'per frame: [UIT 0250] after the GPU finishes' not in text: bad.append('per-frame line')
    return bad


block = cut_switch(back)
check(block, 'switch block not found')
for m in run_switch(block):
    check(False, m)
base = base_loop()
check(base, 'base commit 4b42a20 not readable')
if base:
    check(same_loop(back, base), 'retire loop differs from 4b42a20 beyond the [UIT 0250] lines')
for m in wiring(back):
    check(False, 'wiring: ' + m)
ini = (ROOT / 'launcher/settings.ini').read_text()
check('[TOTK_VK_RETIRE_PCORES]' in ini and '(Full path: Performance > CPU+ > ' in ini.split('[TOTK_VK_RETIRE_PCORES]')[1].split('\n[')[0],
      'launcher key with full path')

# negative probes
probes = 0
p1 = block.replace("int on = !(e && e[0] == '0' && !e[1]);", "int on = !(e && e[0] == '0');")
check(p1 != block and run_switch(p1), 'probe: prefix-0 switch not caught'); probes += 1
p2 = block.replace('on ? plat_thread_pcores() : 0', 'plat_thread_pcores()')
check(p2 != block and run_switch(p2), 'probe: unconditional placement not caught'); probes += 1
if base:
    p3 = back.replace('        cpend_done(j->fence);\n', '        cpend_done(j->fence);\n        cpend_done(j->fence);\n', 1)
    check(p3 != back and not same_loop(p3, base), 'probe: extra statement in the retire loop not caught'); probes += 1
    p4 = back.replace('        complete(j->fence);\n        atomic_fetch_add_explicit(&g_ret_ns',
                      '        atomic_fetch_add_explicit(&g_ret_ns', 1)
    check(p4 != back and (not same_loop(p4, base) or wiring(p4)), 'probe: complete() dropped not caught'); probes += 1

if fails:
    print('FAIL test_retire_pcores:')
    for f in fails:
        print('  -', f)
    sys.exit(1)
print(f'PASS test_retire_pcores: switch cases 5, retire loop identical to 4b42a20 apart from the timing, {probes} negative probes caught')
