"""[UIT 0245] TOTK_PRECISE_SLEEP: guest sleeps (SleepThread ns > 0 and the yield back-off) through plat_sleep_ns_precise.
1. Cuts plat_sleep_ns_precise out of src/runtime/platform/windows.c (Windows only) and runs it against a fake
   high-resolution timer that wakes at random between half and one and a half times the asked time, over thousands of
   sleeps: it must never return before the end, and must not spin for long. Negative probe: the same function with its
   end check weakened (one timer wait and out) must be caught.
2. Cuts plat_sleep_ns_precise out of src/runtime/platform/linux.c and sleeps 50 us .. 3 ms for real: never early, and
   a sub-millisecond sleep must not be rounded up to whole milliseconds (the old winpthreads nanosleep did). A probe that
   rounds up to ms (the old way) must be caught.
3. Wiring: threads.c sends both guest sleeps through guest_sleep_ns, which falls back to nanosleep when the switch is
   =0 or the timer is missing; the launcher key exists (LF file).
usage: python tools/tests/test_precise_sleep.py
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


def cut(text, head):
    a = text.find(head)
    if a < 0:
        return None
    b = text.find('\n}\n', a)
    return text[a:b + 3] if b > a else None


win = (ROOT / 'src/runtime/platform/windows.c').read_text(errors='replace')
lin = (ROOT / 'src/runtime/platform/linux.c').read_text(errors='replace')
thr = (ROOT / 'src/host/threads.c').read_text(errors='replace').replace('\r\n', '\n')
wf = cut(win, 'int plat_sleep_ns_precise(uint64_t ns) {')
lf = cut(lin, 'int plat_sleep_ns_precise(uint64_t ns) {')
check(wf is not None, 'windows.c: plat_sleep_ns_precise')
check(lf is not None, 'linux.c: plat_sleep_ns_precise')

FAKE = r'''
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
typedef void *HANDLE; typedef long long LONGLONG; typedef union { LONGLONG QuadPart; } LARGE_INTEGER;
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 2
#define TIMER_ALL_ACCESS 0
#define INFINITE 0xffffffffu
#define FALSE 0
static uint64_t now_fake = 1000000000ull, armed_ns, n_wait, n_spin, n_set;
static unsigned rng = 12345u;
static unsigned rnd(void) { rng = rng * 1103515245u + 12345u; return rng >> 8; }
static uint64_t plat_now_ns(void) { return now_fake; }
static HANDLE CreateWaitableTimerExW(void *a, void *b, int f, int acc) { (void)a; (void)b; (void)f; (void)acc; return (HANDLE)1; }
static int SetWaitableTimer(HANDLE h, LARGE_INTEGER *due, int p, void *f, void *a, int r) {
    (void)h; (void)p; (void)f; (void)a; (void)r; n_set++;
    if (due->QuadPart >= 0) { printf("FAIL: not a relative due time\n"); exit(1); }
    armed_ns = (uint64_t)(-due->QuadPart) * 100; return 1;
}
static unsigned WaitForSingleObject(HANDLE h, unsigned ms) {   // wakes between 0.5x and 1.5x the asked time
    (void)h; (void)ms; n_wait++; now_fake += armed_ns / 2 + (armed_ns ? rnd() % (armed_ns + 1) : 0); return 0;
}
static void YieldProcessor(void) { n_spin++; now_fake += 300; }
static void Sleep(unsigned ms) { now_fake += (uint64_t)ms * 1000000; }
'''
MAIN = r'''
int main(void) {
    static const uint64_t asks[] = {1, 999, 5000, 20000, 20001, 50000, 100000, 333333, 999999, 1000000, 1500000, 16666667, 50000000};
    uint64_t worst_spin = 0;
    for (int r = 0; r < 4000; r++) {
        uint64_t ns = asks[r % 13] + (r >= 13 ? rnd() % 2000000 : 0), t0 = now_fake, s0 = n_spin;
        if (!plat_sleep_ns_precise(ns)) { printf("FAIL: no timer\n"); return 1; }
        if (now_fake - t0 < ns) { printf("FAIL: woke early: asked %llu ns, slept %llu\n", (unsigned long long)ns, (unsigned long long)(now_fake - t0)); return 1; }
        if (n_spin - s0 > worst_spin) worst_spin = n_spin - s0;
    }
    if (worst_spin * 300 > 25000 + 300) { printf("FAIL: spun %llu ns in one sleep\n", (unsigned long long)(worst_spin * 300)); return 1; }
    printf("ok: 4000 sleeps never early under a timer waking at 0.5x..1.5x; %llu timer waits, longest spin %llu ns\n",
           (unsigned long long)n_wait, (unsigned long long)(worst_spin * 300));
    return 0;
}
'''

LMAIN = r'''
#define _GNU_SOURCE
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
static uint64_t mono(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000000u + t.tv_nsec; }
#ifdef PROBE_MS_ROUND
static int plat_sleep_ns_precise(uint64_t ns) {   // the old winpthreads way: whole milliseconds, rounded up
    uint64_t ms = (ns + 999999) / 1000000; struct timespec ts = {(time_t)(ms / 1000), (long)(ms % 1000) * 1000000}; nanosleep(&ts, 0); return 1;
}
#else
LINUX_FN
#endif
static int cmp(const void *a, const void *b) { uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b; return x < y ? -1 : x > y; }
int main(void) {
    static const uint64_t asks[] = {50000, 100000, 250000, 500000, 1300000, 3000000};
    for (int k = 0; k < 6; k++) {
        uint64_t d[15];
        for (int r = 0; r < 15; r++) {
            uint64_t t0 = mono(); plat_sleep_ns_precise(asks[k]); d[r] = mono() - t0;
            if (d[r] < asks[k]) { printf("FAIL: early: asked %llu got %llu\n", (unsigned long long)asks[k], (unsigned long long)d[r]); return 1; }
        }
        qsort(d, 15, sizeof d[0], cmp);
        if (asks[k] < 1000000 && d[7] >= 1000000) { printf("FAIL: a %llu ns sleep took %llu ns (median): rounded up to ms\n", (unsigned long long)asks[k], (unsigned long long)d[7]); return 1; }
        printf("  %7llu ns asked: median %7llu ns\n", (unsigned long long)asks[k], (unsigned long long)d[7]);
    }
    printf("ok: real sleeps never early, sub-ms sleeps not rounded to ms\n");
    return 0;
}
'''


def run(cmd, **kw):
    return subprocess.run(cmd, capture_output=True, text=True, **kw)


with tempfile.TemporaryDirectory() as d:
    d = pathlib.Path(d)
    if wf:
        for probe in (False, True):
            body = wf
            if probe:   # weakened end check: one timer wait and out
                body = body.replace('WaitForSingleObject(timer, INFINITE);', 'WaitForSingleObject(timer, INFINITE); return 1;')
                check(body != wf, 'probe edit applied')
            src = d / ('w%d.c' % probe)
            src.write_text(FAKE + body + MAIN)
            exe = d / ('w%d' % probe)
            r = run(['cc', '-O1', '-Wall', '-Werror', '-Wno-unused-function', str(src), '-o', str(exe)])
            check(r.returncode == 0, 'windows fake build: ' + r.stderr[-800:])
            if r.returncode == 0:
                r = run([str(exe)])
                print(('negative probe: ' if probe else '') + (r.stdout.strip().splitlines() or ['?'])[-1])
                check((r.returncode != 0) == probe, ('negative probe not caught' if probe else 'windows fake: ' + r.stdout[-500:]))
    if lf:
        for probe in (False, True):
            src = d / ('l%d.c' % probe)
            src.write_text(LMAIN.replace('LINUX_FN', lf))
            exe = d / ('l%d' % probe)
            r = run(['cc', '-O1', '-Wall', '-Werror'] + (['-DPROBE_MS_ROUND'] if probe else []) + [str(src), '-o', str(exe)])
            check(r.returncode == 0, 'linux build: ' + r.stderr[-800:])
            if r.returncode == 0:
                r = run([str(exe)])
                print(('negative probe (ms rounding): ' if probe else '') + r.stdout.strip().replace('\n', '\n  ' if not probe else ' | '))
                check((r.returncode != 0) == probe, ('ms-rounding probe not caught' if probe else 'linux real sleeps: ' + r.stdout[-500:]))

# wiring
gs = cut(thr, 'static void guest_sleep_ns(uint64_t ns) {')
check(gs is not None and 'precise_sleep_on() && plat_sleep_ns_precise(ns)' in gs and 'nanosleep(&ts, 0)' in gs,
      'threads.c guest_sleep_ns: precise when on, nanosleep otherwise')
check('else if (ns > 0) guest_sleep_ns((uint64_t)ns);' in thr, 'threads.c: SleepThread(ns > 0) uses guest_sleep_ns')
check('guest_sleep_ns(backoff);' in thr, 'threads.c: the yield back-off uses guest_sleep_ns')
check('getenv("TOTK_PRECISE_SLEEP")' in thr and "e[0] == '0' && !e[1]" in thr, 'threads.c: =0 turns it off')
check('int plat_sleep_ns_precise(uint64_t ns);' in (ROOT / 'src/runtime/platform.h').read_text(), 'platform.h declaration')
ini = (ROOT / 'launcher/settings.ini').read_bytes()
check(b'[TOTK_PRECISE_SLEEP]' in ini and b'\r\n' not in ini, 'settings.ini key (LF)')
check(b'\r\n' in (ROOT / 'src/host/threads.c').read_bytes() and thr.count('\n') == (ROOT / 'src/host/threads.c').read_bytes().count(b'\r\n'),
      'threads.c stays CRLF throughout')

if fails:
    print('FAIL:\n  ' + '\n  '.join(fails))
    sys.exit(1)
print('test_precise_sleep: all passed')
