"""[UIT 0235] The images lock as vkb_tex.c takes and releases it, run on the box with a non-recursive lock that reports
misuse, under a time limit. Also a scan for re-entry: no code holding the images lock calls a function that takes it.

6102d92 booted to a black screen: 0232's img_unlock() called itself instead of releasing the lock, so the first
release never returned (a spin holding the lock) and every later user of the lock waited forever. vkb_tex.c only
builds for Windows. This copies img_lock / img_unlock out of it and runs them here:
  - the lock is PTHREAD_MUTEX_ERRORCHECK, which is non-recursive like Windows' locks. Taking it again on the same thread
    (EDEADLK) or releasing a lock this thread does not hold (EPERM) is reported, not left to hang.
  - lock, unlock, then another thread must get it at once; 4 threads x 200000 lock/unlock pairs must keep the count
    exact (with the 0232 statistics on and off).
  - a 20 s limit: a release that never returns (the 0232 bug) fails instead of hanging the suite.
On 6102d92: "FAIL: timed out ... the images lock is never released". With 0235: PASS.

usage: python tools/tests/test_img_lock_run.py [--cc clang] [--src src/gfx/vkb_tex.c]
"""
import argparse
import os
import pathlib
import re
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parent.parent.parent

PRELUDE = r'''
#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
static pthread_mutex_t g_mu = PTHREAD_ERRORCHECK_MUTEX_INITIALIZER_NP;   /* non-recursive, reports misuse */
static int real_lock(pthread_mutex_t *m) {
    int r = pthread_mutex_lock(m);
    if (r == EDEADLK) { fprintf(stderr, "FAIL: the images lock taken again by the thread that holds it (deadlock on Windows)\n"); exit(3); }
    return r;
}
static int real_unlock(pthread_mutex_t *m) {
    int r = pthread_mutex_unlock(m);
    if (r) { fprintf(stderr, "FAIL: the images lock released by a thread that does not hold it (%d)\n", r); exit(4); }
    return r;
}
#define pthread_mutex_lock real_lock
#define pthread_mutex_unlock real_unlock
static uint64_t runtime_now_ns(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec; }
enum { LK_IMAGES, LK_N };
static _Atomic uint64_t vkb_lock_wait_ns[LK_N], vkb_lock_waits[LK_N];
typedef struct { uint64_t v; } vkb_cnt[1];
static void vkb_cnt_add(vkb_cnt c, uint64_t n) { __atomic_fetch_add(&c[0].v, n, __ATOMIC_RELAXED); }
static int g_cp_on;
static int uit_cp_on(void) { return g_cp_on; }
'''

MAIN = r'''
static long g_count;
static void *other(void *p) {
    (void)p;
    if (pthread_mutex_trylock(&g_mu)) { fprintf(stderr, "FAIL: after img_unlock another thread cannot take the images lock\n"); exit(5); }
    real_unlock(&g_mu);
    return NULL;
}
static void *hammer(void *p) {
    for (int i = 0; i < 200000; i++) { img_lock(i % IS_N); long c = g_count; g_count = c + 1; img_unlock(); }
    return p;
}
int main(void) {
    for (g_cp_on = 0; g_cp_on < 2; g_cp_on++) {
        img_lock(IS_LOOKUP);
        img_unlock();
        pthread_t t;
        pthread_create(&t, NULL, other, NULL); pthread_join(t, NULL);
        img_lock(IS_CREATE); img_unlock(); img_lock(IS_OTHER); img_unlock();
        g_count = 0;
        pthread_t h[4];
        for (int i = 0; i < 4; i++) pthread_create(&h[i], NULL, hammer, NULL);
        for (int i = 0; i < 4; i++) pthread_join(h[i], NULL);
        if (g_count != 800000) { fprintf(stderr, "FAIL: the images lock did not exclude: %ld of 800000\n", g_count); return 6; }
        printf("ok: statistics %s: lock/unlock, released to another thread, 4 threads x 200000 exact\n", g_cp_on ? "on" : "off");
    }
    return 0;
}
'''


def functions(code):
    """top-level functions (code without comments or strings): name -> (start, end), by matching braces."""
    out = {}
    for m in re.finditer(r'^(?:static\s+)?(?:inline\s+)?[A-Za-z_][\w\s\*]*?\b(\w+)\s*\([^;{]*\)\s*\{', code, re.M):
        depth, p = 0, m.end() - 1
        while p < len(code):
            depth += {'{': 1, '}': -1}.get(code[p], 0)
            p += 1
            if depth == 0:
                break
        out.setdefault(m.group(1), (m.start(), p))
    return out


def reentry(src):
    """calls, while the images lock is held, to functions that take it themselves (directly)."""
    code = re.sub(r'/\*.*?\*/|//[^\n]*|"(?:\\.|[^"\\\n])*"|\'(?:\\.|[^\'\\\n])+\'', lambda m: ' ' * len(m.group(0)), src, flags=re.S)
    fns = functions(code)
    takers = {n for n, (a, b) in fns.items() if n not in ('img_lock', 'img_unlock') and 'img_lock(' in code[a:b]}
    bad = []
    for name, (a, b) in fns.items():
        if name in ('img_lock', 'img_unlock'):
            continue
        body = code[a:b]
        for m in re.finditer(r'\bimg_lock\(', body):
            stop = body.find('img_unlock()', m.end())
            region = body[m.end():stop if stop > 0 else len(body)]
            for c in re.finditer(r'\b(\w+)\s*\(', region):
                if c.group(1) in takers:
                    bad.append(f'{name} calls {c.group(1)} (which takes the images lock) while holding it')
    return sorted(set(bad)), len(takers)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--cc', default=os.environ.get('CC', 'clang'))
    ap.add_argument('--src', default=str(ROOT / 'src/gfx/vkb_tex.c'))
    a = ap.parse_args()
    src = pathlib.Path(a.src).read_text(encoding='utf-8')
    bad = 0

    rs, ntake = reentry(src)
    for r in rs:
        print('FAIL: re-entry: ' + r)
    bad += len(rs)
    if not rs:
        print(f'ok: no code holding the images lock calls one of the {ntake} functions that take it')

    i = src.index('enum { IS_LOOKUP')
    k = src.index('static void img_unlock(void) {')
    j = src.index('\n}', k) + 2
    with tempfile.TemporaryDirectory() as d:
        c = pathlib.Path(d) / 'img_lock_run.c'
        c.write_text(PRELUDE + src[i:j] + '\n' + MAIN, encoding='utf-8')
        exe = pathlib.Path(d) / 'img_lock_run'
        r = subprocess.run([*a.cc.split(), '-O2', '-w', '-pthread', str(c), '-o', str(exe)], capture_output=True, text=True)
        if r.returncode:
            print('FAIL: the copied lock code does not compile:\n' + r.stderr[-3000:])
            return 1
        try:
            r = subprocess.run([str(exe)], capture_output=True, text=True, timeout=20)
            print(r.stdout, end='')
            if r.returncode:
                bad += 1
                print(r.stderr, end='')
                if not r.stderr.strip():
                    print(f'FAIL: the lock test stopped with {r.returncode} (a negative number: killed by that signal; '
                          f'-11 is a stack overflow, e.g. img_unlock calling itself)')
        except subprocess.TimeoutExpired:
            bad += 1
            print('FAIL: timed out after 20 s: the images lock is never released (img_lock / img_unlock hang)')
    print('test_img_lock_run: ' + ('PASS' if not bad else f'{bad} FAILED'))
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
