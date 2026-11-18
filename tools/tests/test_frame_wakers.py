"""[UIT 0369] TOTK_CRITPATH: which game threads the frame thread waited on. Cuts wk_note and runtime_frame_wakers out
of src/host/threads.c and runs them against a small model: waits ended by known threads, by a thread that has since
exited, by timeouts, and by more threads than the table has slots ("others"); checks the top 3 per kind, their ms a
frame and shares, the clearing after each report, and that TOTK_CRITPATH=0 records and reports nothing. A negative
probe (a table that ignores the handle) must be caught.
Wiring: the waker is set by wake() and core_grant(), cleared before each wait; both wait kinds note it; the
[critical path] report prints the line on the frame thread.
usage: python tools/tests/test_frame_wakers.py
"""
import os
import pathlib
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parent.parent.parent
fails = []


def check(cond, msg):
    if not cond:
        fails.append(msg)


thr = (ROOT / 'src/host/threads.c').read_text(errors='replace').replace('\r\n', '\n')
prof = (ROOT / 'src/gfx/uit_prof.c').read_text(errors='replace')
a = thr.find('enum { WK_SLOTS = 8 };'); b = thr.find('\n}\n', thr.find('static void wk_note(')) + 3
note = thr[a:b] if a >= 0 and b > a else ''
# [UIT 0388] wk_format (the top 3, shared), runtime_frame_wakers, runtime_thread_publish and runtime_thread_report
a = thr.find('static int wk_format('); b = thr.find('\n}\n', thr.find('int runtime_thread_report(')) + 3
rep = thr[a:b] if a >= 0 and b > a else ''
check(note and rep and 'int runtime_frame_wakers(double frames' in rep, '0369 / 0388 code not found')
check('t->waker = t_self ? t_self->handle : 0; tw_signal(&t->cv); }' in thr, 'wake() records the waker')
check('b->waker = t_self ? t_self->handle : 0;' in thr, 'core_grant() records the thread that let the core go')
check('g_core_wait = t; t->waker = 0;' in thr and '    t->waker = 0;\n    core_put(t); int64_t r = block_inner(t, kind, ns);' in thr,
      'the waker is cleared before each wait')
check(thr.count('wk_note(0, t->waker, d)') == 2 and 'wk_note(1, t->woke ? t->waker : 0, d)' in thr, 'both wait kinds noted')
check('runtime_frame_wakers(nf, wb, (int)sizeof wb)' in prof and '"[critical path]   frame thread waited on: %s"' in prof,
      'the [critical path] report prints it')
check('__attribute__((weak)) int runtime_frame_wakers(' in prof, 'weak stub for builds without threads.c')
check('__attribute__((weak)) int runtime_thread_report(' in prof and '"[critical path]   main thread %s"' in prof
      and '"[critical path]   main thread\'s top waker %s"' in prof, '[UIT 0388] the main thread lines and their weak stub')
check(thr.count('runtime_thread_publish(t);') == 2 and 't->tw = NULL; t->wk = NULL;' in thr and 'g_main_handle = t->handle;' in thr,
      '[UIT 0388] each game thread publishes its tables, the main thread its handle, an exiting thread withdraws them')

HEAD = r'''
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef struct thr { uint32_t handle; uint64_t entry; struct thr *all; uint64_t *tw; void *wk; } thr_t;
static thr_t *g_all;
static __thread uint64_t t_tw[8];
static uint32_t g_main_handle;
static void g_lock(void) {}
static void g_unlock(void) {}
'''
MAIN = r'''
static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)
int main(void) {
    static thr_t t1 = {0x101, 0x7100001000, 0}, t2 = {0x102, 0x7100002000, 0}, t3 = {0x103, 0x7100003000, 0};
    t1.all = &t2; t2.all = &t3; g_all = &t1;
    char b[2048];
    int off = getenv("TOTK_CRITPATH") && *getenv("TOTK_CRITPATH") == '0';
    // core waits: 0x102 3 ms, 0x101 1 ms; blocking: 0x103 6 ms, timeouts 2 ms, 0x1ff (exited) 1 ms, 0x101 1 ms
    for (int i = 0; i < 3; i++) wk_note(0, 0x102, 1000000);
    wk_note(0, 0x101, 1000000);
    for (int i = 0; i < 6; i++) wk_note(1, 0x103, 1000000);
    wk_note(1, 0, 2000000); wk_note(1, 0x1ff, 1000000); wk_note(1, 0x101, 1000000);
    int r = runtime_frame_wakers(10, b, (int)sizeof b);
    printf("%s\n", b);
    if (off) { CHECK(r == 0 && b[0] == 0); printf(fails ? "FAILED\n" : "PASS off\n"); return fails != 0; }
    CHECK(r == 1);
    const char *c = strstr(b, "core waits ended by"), *k = strstr(b, "blocking waits ended by");
    CHECK(c && k && c < k);
    CHECK(c && strstr(c, " h=0x102 entry=0x7100002000 0.300 ms (75%, 0.3 waits), h=0x101 entry=0x7100001000 0.100 ms (25%, 0.1 waits)") == c + strlen("core waits ended by"));
    CHECK(k && strstr(k, " h=0x103 entry=0x7100003000 0.600 ms (60%, 0.6 waits), timeouts / host threads 0.200 ms (20%, 0.1 waits),") == k + strlen("blocking waits ended by"));
    CHECK(strstr(b, "(exited)") || strstr(b, "h=0x101 entry=0x7100001000 0.100 ms (10%"));   // third place: a 1 ms tie
    CHECK(runtime_frame_wakers(10, b, (int)sizeof b) == 0 && b[0] == 0);                  // cleared
    // more distinct threads than slots: the rest under "others", the top ones still right
    for (uint32_t h = 0x200; h < 0x20c; h++) wk_note(1, h, 1000000);
    wk_note(1, 0x200, 50000000);
    r = runtime_frame_wakers(1, b, (int)sizeof b);
    printf("%s\n", b);
    CHECK(r == 1 && strstr(b, "blocking waits ended by h=0x200 (exited) 51.000 ms") && strstr(b, ", others 4.000 ms (6%, 4.0 waits)"));
    CHECK(!strstr(b, "core waits"));
    // a small buffer is cut, never overrun
    wk_note(0, 0x102, 1000); char sm[16]; memset(sm, 'x', sizeof sm);
    runtime_frame_wakers(1, sm, 12); CHECK(strlen(sm) == 11 && sm[12] == 'x');
    // [UIT 0388] another thread's report: the main thread (handle 0), modelled with its own tables
    static uint64_t mtw[8]; static wk_slot mwk[2][WK_SLOTS + 1];
    static thr_t m = {0x1001, 0, 0}; m.all = g_all; g_all = &m; g_main_handle = 0x1001;
    uint32_t nx = 7;
    CHECK(runtime_thread_report(0, 10, 12, b, (int)sizeof b, &nx) == 0 && nx == 0);   // not running yet: nothing
    m.tw = mtw; m.wk = mwk;
    CHECK(runtime_thread_report(0, 10, 12, b, (int)sizeof b, &nx) == 0);   // the first call only takes the starting values
    mtw[0] = 10000000; mtw[4] = 20; mtw[1] = 80000000; mtw[5] = 300; mtw[3] = 5000000; mtw[7] = 40;
    mwk[1][0] = (wk_slot){0x103, 250, 70000000}; mwk[1][1] = (wk_slot){0, 50, 10000000}; mwk[0][0] = (wk_slot){0x102, 20, 10000000};
    r = runtime_thread_report(0, 10, 12, b, (int)sizeof b, &nx);
    printf("%s\n", b);
    CHECK(r == 1 && strstr(b, "h=0x1001 entry=0: core waits 1.000 ms (2.0), blocking waits 8.000 ms (30.0), sleeps 0.000 ms (0.0), yields 0.500 ms (4.0) a frame"));
    CHECK(strstr(b, "; the rest (running the game's code, or waiting for Windows) 2.500 ms"));
    CHECK(strstr(b, "blocking waits ended by h=0x103 entry=0x7100003000 7.000 ms (88%, 25.0 waits)") && nx == 0x103);
    CHECK(mwk[1][0].n == 0 && mwk[1][0].ns == 0);   // emptied
    r = runtime_thread_report(0, 10, 12, b, (int)sizeof b, &nx);   // nothing since: zeros, no wakers
    CHECK(r == 1 && strstr(b, "blocking waits 0.000 ms (0.0)") && !strstr(b, "ended by") && nx == 0);
    m.tw = t_tw;   // the calling (frame) thread itself: its own lines, not this one
    CHECK(runtime_thread_report(0x1001, 10, 12, b, (int)sizeof b, &nx) == 0);
    printf(fails ? "FAILED\n" : "PASS\n");
    return fails != 0;
}
'''


def build_run(code, env_val, tag):
    with tempfile.TemporaryDirectory() as d:
        src = pathlib.Path(d) / 't.c'; exe = pathlib.Path(d) / 't'
        src.write_text(HEAD + code + MAIN)
        r = subprocess.run(['cc', '-O1', '-Wall', '-Wno-unused-function', '-o', str(exe), str(src)], capture_output=True, text=True)
        if r.returncode:
            return 'BUILD FAIL ' + tag + ': ' + r.stderr[-2000:]
        env = dict(os.environ)
        env.pop('TOTK_CRITPATH', None)
        if env_val is not None:
            env['TOTK_CRITPATH'] = env_val
        r = subprocess.run([str(exe)], capture_output=True, text=True, env=env, timeout=60)
        return r.stdout + r.stderr


if note and rep:
    code = note + '\n' + rep
    for v in (None, '1', '0'):
        res = build_run(code, v, str(v))
        print(f'TOTK_CRITPATH={v}:', res.strip().replace('\n', '\n  '))
        check(res.rstrip().endswith('PASS off' if v == '0' else 'PASS'), f'model run TOTK_CRITPATH={v}')
    probe = code.replace('if (s[i].h == h || (!s[i].n && !s[i].ns)) break;', 'if (1) break;')
    if probe == code:
        check(False, 'probe did not apply')
    else:
        res = build_run(probe, None, 'probe')
        print('probe handle ignored ->', 'caught' if 'FAIL' in res else 'NOT caught')
        check('FAIL' in res, 'negative probe not caught')

if fails:
    print('FAIL:\n  ' + '\n  '.join(fails))
    sys.exit(1)
print('PASS test_frame_wakers')
