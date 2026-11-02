"""[UIT 0246] TOTK_GUEST_LOCK_SPIN: threads.c takes its global lock G with g_lock() (retry briefly, then sleep in it).
Cuts the g_lock code out of src/host/threads.c and hammers it from 8 threads (a plain counter and a two-word invariant
under G, plus pthread_cond_wait / signal on G the way thr_wait.h does), with the spin at its default, 0 (as before) and
a large value; plain, and under ThreadSanitizer and AddressSanitizer when the compiler has them. Every count must be
exact and no invariant broken. Negative probe: g_lock with its trylock result ignored (a lock taken without holding it)
must be caught. Wiring: every pthread_mutex_lock(&G) except the one inside g_lock_slow is g_lock(); the launcher key.
usage: python tools/tests/test_guest_lock_spin.py
"""
import os
import pathlib
import re
import shutil
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parent.parent.parent
LLVM = os.environ.get('UIT_LLVM_BIN') or os.path.dirname(shutil.which('clang') or '') or '.'   # folder with clang (TSAN/ASAN builds)
fails = []


def check(cond, msg):
    if not cond:
        fails.append(msg)


thr = (ROOT / 'src/host/threads.c').read_text(errors='replace').replace('\r\n', '\n')
a = thr.find('static int g_lk_spin = -1;')
b = thr.find('\n}\n', thr.find('void runtime_glock_stats(uint64_t v[3]) {'))
check(a >= 0 and b > a, 'g_lock code not found in threads.c')
check(thr.count('pthread_mutex_lock(&G)') == 1 and 'pthread_mutex_lock(&G);\n    __atomic_add_fetch(&g_lk_slept' in thr,
      'threads.c: pthread_mutex_lock(&G) only inside g_lock_slow')
check(thr.count('g_lock()') >= 40, 'threads.c: g_lock() at every former lock of G')
check('pthread_mutex_unlock(&G)' in thr, 'unlock unchanged')
check(thr.count('pthread_mutex_unlock(&G)') == 1 and 'static inline void g_unlock(void) { pthread_mutex_unlock(&G); tw_flush(); }' in thr
      and thr.count('g_unlock()') >= 40, '[UIT 0361] every unlock of G is g_unlock() (the wake-ups left for after the lock)')
tw = (ROOT / 'src/host/thr_wait.h').read_text()
check('#define TW_RELOCK(m) g_relock(m)' in thr and 'static void g_relock(pthread_mutex_t *m) { if (m == &G) g_lock();' in thr
      and '    TW_RELOCK(m);\n    return rc;' in tw and tw.count('pthread_mutex_lock(m)') == 1,
      'WaitOnAddress path takes G back with g_lock (TW_RELOCK)')
ini = (ROOT / 'launcher/settings.ini').read_text()
check('[TOTK_GUEST_LOCK_SPIN]' in ini, 'launcher key')
# [UIT 0361] the yield looks at the core waiters without G only to skip the lock; with waiters it takes G as before
check("if (yield_fast_on() && !__atomic_load_n(&g_core_wait, __ATOMIC_RELAXED))" in thr
      and "                g_lock();   // Horizon yield" in thr and '[TOTK_SCHED_YIELD_FAST]' in ini and '[TOTK_SCHED_DEFER_WAKE]' in ini,
      '[UIT 0361] yield fast path and its launcher keys')

HEAD = r'''
#define _GNU_SOURCE
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
static pthread_mutex_t G = PTHREAD_MUTEX_INITIALIZER;
static inline void tw_flush(void) {}   /* thr_wait.h [UIT 0361]: nothing queued here */
'''
MAIN = r'''
static pthread_cond_t cv = PTHREAD_COND_INITIALIZER;
static long counter, wa, wb, tokens, taken;
enum { T = 8, N = 60000 };
static void *worker(void *p) {
    long id = (long)(intptr_t)p;
    for (int i = 0; i < N; i++) {
        g_lock();
        long c = counter; wa++; if (wa != wb + 1) { printf("FAIL: invariant broken\n"); exit(1); }
        counter = c + 1; wb++;
        if (id & 1) { tokens++; pthread_cond_signal(&cv); }
        else if (i % 64 == 0) { while (!tokens) pthread_cond_wait(&cv, &G); tokens--; taken++; }
        pthread_mutex_unlock(&G);
    }
    return 0;
}
int main(void) {
    pthread_t t[T];
    for (long i = 0; i < T; i++) pthread_create(&t[i], 0, worker, (void *)(intptr_t)i);
    for (int i = 0; i < T; i++) pthread_join(t[i], 0);
    uint64_t v[3]; runtime_glock_stats(v);
    if (counter != (long)T * N || wa != wb) { printf("FAIL: counter %ld of %ld\n", counter, (long)T * N); return 1; }
    printf("ok: %ld locked increments exact; retried and won %llu, slept %llu (%.2f ms); cond waits %ld\n", counter,
           (unsigned long long)v[0], (unsigned long long)v[1], v[2] / 1e6, taken);
    return 0;
}
'''

if a >= 0 and b > a:
    code = thr[a:b + 3]
    with tempfile.TemporaryDirectory() as d:
        d = pathlib.Path(d)
        builds = [('plain', 'cc', ['-O2'])]
        for name, flags in (('tsan', ['-O1', '-g', '-fsanitize=thread']), ('asan', ['-O1', '-g', '-fsanitize=address,undefined', '-fno-sanitize-recover=undefined'])):
            if pathlib.Path(LLVM, 'clang').exists():
                builds.append((name, str(pathlib.Path(LLVM, 'clang')), flags))
        for probe in (False, True):
            body = code
            if probe:
                body = body.replace('static inline void g_lock(void) { if (__builtin_expect(pthread_mutex_trylock(&G) != 0, 0)) g_lock_slow(); }',
                                    'static inline void g_lock(void) { if (pthread_mutex_trylock(&G) != 0) { (void)g_lock_slow; } }')
                check(body != code, 'probe edit applied')
            src = d / ('g%d.c' % probe)
            src.write_text(HEAD + body + MAIN)
            for name, cc, flags in (builds if not probe else builds[:1]):
                exe = d / ('g%d%s' % (probe, name))
                r = subprocess.run([cc, *flags, '-Wall', '-Werror', '-Wno-unused-function', '-pthread', str(src), '-o', str(exe)], capture_output=True, text=True)
                check(r.returncode == 0, f'{name} build: ' + r.stderr[-600:])
                if r.returncode:
                    continue
                for spin in ([None, '0', '5000'] if not probe else [None]):
                    env = dict(os.environ, TSAN_OPTIONS='halt_on_error=1', ASAN_OPTIONS='detect_leaks=0')
                    env.pop('TOTK_GUEST_LOCK_SPIN', None)
                    if spin is not None:
                        env['TOTK_GUEST_LOCK_SPIN'] = spin
                    bad = False
                    for _ in range(3 if probe else 1):
                        r = subprocess.run([str(exe)], capture_output=True, text=True, env=env, timeout=300)
                        bad = bad or r.returncode != 0
                    tag = f'{name} spin={spin or "default"}'
                    if probe:
                        print(f'negative probe ({tag}): ' + ('caught' if bad else 'NOT caught'))
                        check(bad, 'negative probe (lock taken without holding it) not caught')
                    else:
                        print(f'{tag}: ' + (r.stdout.strip().splitlines() or ['?'])[-1])
                        check(r.returncode == 0, f'{tag}: ' + (r.stdout + r.stderr)[-600:])

if fails:
    print('FAIL:\n  ' + '\n  '.join(fails))
    sys.exit(1)
print('test_guest_lock_spin: all passed')
