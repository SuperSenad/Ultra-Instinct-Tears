"""[UIT 0357] TOTK_CORE_SPIN_US: a game thread waiting for a virtual core (threads.c core_take, with asynchronous
preemption on) spins only this long, then sleeps on its wait object until core_grant hands it a core or the 250 us
before the first interruption are over. Cuts that block out of src/host/threads.c and runs it against thr_wait.h
(futex path and condition-variable path) with a granting thread: a core handed over during the spin or during the
sleep ends the wait (the sleep is woken by the signal, not by its timeout); with no grant the block returns no sooner
than 250 us; 250 never sleeps (the old spin exactly). Wiring: the value is read once, clamped to 0..250, the launcher key.
usage: python tools/tests/test_core_spin.py
"""
import os
import pathlib
import shutil
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parent.parent.parent
fails = []


def check(cond, msg):
    if not cond:
        fails.append(msg)


thr = (ROOT / 'src/host/threads.c').read_text(errors='replace').replace('\r\n', '\n')
a = thr.find('    if (g_ap) {   // most waits end within microseconds: spin before interrupting anyone\n')
b = thr.find('\n    }\n', thr.find('// [UIT 0357] then sleep the rest of the 250 us', a)) + len('\n    }\n')
check(a >= 0 and b > a, 'core_take spin block not found in threads.c')
block = thr[a:b]
check('g_core_spin_ns' in block and 'tw_wait(&t->cv, &G, &dl)' in block, 'the block spins g_core_spin_ns, then sleeps on t->cv')
check('static uint64_t g_core_spin_ns = 50000;' in thr, 'default 50 us')
check('v < 0 ? 0 : v > 250 ? 250 : v' in thr and 'getenv("TOTK_CORE_SPIN_US")' in thr, 'TOTK_CORE_SPIN_US read and clamped')
check('#define AP_FIRST_NS 250000ull' in thr, 'first interruption still at 250 us')
ini = (ROOT / 'launcher/settings.ini').read_text()
check('[TOTK_CORE_SPIN_US]' in ini, 'launcher key')

PROG = r'''
#define _GNU_SOURCE
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
static pthread_mutex_t G = PTHREAD_MUTEX_INITIALIZER;
#define TW_RELOCK(m) pthread_mutex_lock(m)
#include "thr_wait.h"
#define AP_FIRST_NS 250000ull
static uint64_t now_ns(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return ts.tv_sec * 1000000000ull + ts.tv_nsec; }
static void g_lock(void) { pthread_mutex_lock(&G); }
static void g_unlock(void) { pthread_mutex_unlock(&G); tw_flush(); }
static uint64_t g_core_spin_ns;
static int g_ap = 1;
typedef struct { int cur_core; thr_wait_t cv; } thr_t;
static thr_t T;
static uint64_t grant_after_ns;   // 0: never
static volatile uint64_t start_ns;   // set by the waiter once the granter runs
static volatile int ready;
static void *granter(void *p) {
    (void)p;
    ready = 1;
    while (!start_ns) ;
    while (now_ns() - start_ns < grant_after_ns) ;
    pthread_mutex_lock(&G); T.cur_core = 1; tw_signal(&T.cv); g_unlock();
    return NULL;
}
static double run(uint64_t spin_us, uint64_t grant_us, int *got) {
    g_core_spin_ns = spin_us * 1000; grant_after_ns = grant_us * 1000;
    T.cur_core = -1; tw_init(&T.cv);
    uint64_t w0 = __atomic_load_n(&tw_n_wait, __ATOMIC_RELAXED), o0 = __atomic_load_n(&tw_n_timeout, __ATOMIC_RELAXED);
    pthread_t h; ready = 0; start_ns = 0;
    if (grant_us) { pthread_create(&h, NULL, granter, NULL); while (!ready) ; }
    thr_t *t = &T;
    uint64_t s = now_ns(); start_ns = s;
    pthread_mutex_lock(&G);
BLOCK
    *got = t->cur_core;
    pthread_mutex_unlock(&G);
    double us = (now_ns() - s) / 1000.0;
    if (grant_us) pthread_join(h, NULL);
    if (tw_on()) got[1] = (int)(__atomic_load_n(&tw_n_wait, __ATOMIC_RELAXED) - w0);
    if (tw_on()) got[2] = (int)(__atomic_load_n(&tw_n_timeout, __ATOMIC_RELAXED) - o0);
    return us;
}
int main(void) {
    int bad = 0, g[3] = {0, 0, 0}, late = 0;
    for (int rep = 0; rep < 20; rep++) {
        g[1] = -1; double us = run(50, 10, g);       // granted during the spin
        if (g[0] != 1 || us > 5000) { printf("FAIL spin grant: core %d after %.0f us\n", g[0], us); bad++; }
        g[1] = -1; us = run(50, 150, g);             // granted during the sleep: woken by the signal
        if (g[0] != 1 || us < 140 || us > 240 + 2000) { printf("FAIL sleep grant: core %d after %.0f us\n", g[0], us); bad++; }
        if (tw_on() && g[1] < 1) { printf("FAIL sleep grant: never slept\n"); bad++; }
        if (tw_on() && g[2]) late++;   // the sleep ended by its timeout, not by the grant's wake
        g[1] = -1; us = run(50, 0, g);               // nobody grants: back after the 250 us
        if (g[0] != -1 || us < 250) { printf("FAIL no grant: core %d after %.0f us\n", g[0], us); bad++; }
        g[1] = -1; us = run(250, 200, g);            // 250: spins all of it (no sleep), as before 0357
        if (g[0] != 1 || (tw_on() && g[1] != 0)) { printf("FAIL 250: core %d, %d sleeps\n", g[0], g[1]); bad++; }
        g[1] = -1; us = run(0, 100, g);              // 0: sleeps at once, still woken by the grant
        if (g[0] != 1 || us < 90) { printf("FAIL 0: core %d after %.0f us\n", g[0], us); bad++; }
    }
    if (late > 2) { printf("FAIL sleep grant: %d of 20 sleeps ended by the timeout, not the wake\n", late); bad++; }
    printf("%s: %d failures (mode %s)\n", bad ? "FAIL" : "ok", bad, tw_on() ? "futex" : "condvar");
    return bad != 0;
}
'''

cc = shutil.which('clang') or shutil.which('gcc') or 'cc'
with tempfile.TemporaryDirectory() as d:
    src = pathlib.Path(d) / 't.c'
    src.write_text(PROG.replace('BLOCK', block))
    exe = pathlib.Path(d) / 't'
    r = subprocess.run([cc, '-O2', '-pthread', f'-I{ROOT / "src/host"}', str(src), '-o', str(exe)], capture_output=True, text=True)
    check(r.returncode == 0, 'compile: ' + r.stderr[-2000:])
    if r.returncode == 0:
        for mode in ('1', '0'):
            p = subprocess.run([str(exe)], capture_output=True, text=True, env=dict(os.environ, TOTK_WAIT_ON_ADDRESS=mode))
            print(p.stdout.strip())
            check(p.returncode == 0, f'TOTK_WAIT_ON_ADDRESS={mode}: ' + p.stdout[-1500:])

# negative probe: a sleep that ignores the grant (no signal reaches it) must be caught
if not fails:
    with tempfile.TemporaryDirectory() as d:
        src = pathlib.Path(d) / 't.c'
        src.write_text(PROG.replace('BLOCK', block).replace('tw_signal(&T.cv);', ''))
        exe = pathlib.Path(d) / 't'
        subprocess.run([cc, '-O2', '-pthread', f'-I{ROOT / "src/host"}', str(src), '-o', str(exe)], check=True)
        p = subprocess.run([str(exe)], capture_output=True, text=True, env=dict(os.environ, TOTK_WAIT_ON_ADDRESS='1'))
        check(p.returncode != 0, 'negative probe (grant without the signal) was not caught')
        print('negative probe (no signal at the grant):', 'caught' if p.returncode else 'NOT caught')

if fails:
    print('FAIL:\n  ' + '\n  '.join(fails))
    sys.exit(1)
print('test_core_spin: PASS')
