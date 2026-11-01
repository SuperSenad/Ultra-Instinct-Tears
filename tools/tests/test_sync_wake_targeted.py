"""[UIT 0247] TOTK_SYNC_WAKE_TARGETED: an event signal (or a thread exit) wakes only the WaitSynchronization waiters that
wait on that object. Cuts sig_obj, sync_wake_targeted, sync_may_end and wake_sync_waiters out of src/host/threads.c and
runs them against a random model: 4096-slot handle table with events, threads, write ends, free and bad handles;
waiters with 0..64 handles each (H_CUR_THREAD among them), all blocked (none of their objects signaled). Each step
signals one event or exits one thread through the real code, then every waiter is re-checked with the real sig_obj:
a waiter left asleep must find exactly what it found before (nothing signaled, no bad handle) - so no wake-up the
game could see is lost - and with =0 every waiter must be woken (as before). Negative probes: a sync_may_end that
looks only at the first four handles, and one that skips handles that went bad, must both be caught.
Wiring: every signal site passes its object; whs is set and cleared around the wait; the launcher key.
usage: python tools/tests/test_sync_wake_targeted.py
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


def cut(start, end_marker='\n}\n'):
    a = thr.find(start)
    if a < 0:
        fails.append('not found: ' + start)
        return ''
    b = thr.find(end_marker, a)
    return thr[a:b + len(end_marker)]


sig = cut('static int sig_obj(uint32_t h, int *bad) {')
code = thr[thr.find('static int g_swt = -1;'):thr.find('void runtime_syncwake_stats(')]
check('sync_may_end' in code and 'static void wake_sync_waiters(const void *obj)' in code, '0247 code not found')
check('wake_sync_waiters()' not in thr, 'a signal site without its object')
check(thr.count('wake_sync_waiters(e);') == 4, 'four event signal sites pass the event')
check('wake_sync_waiters(t);' in thr, 'thread exit passes the thread')
check('if (w->wkind == 4) wake(w, -1)' not in thr, 'no wake-everyone loop left')
check('t->whs = hs;' in thr and 't->whs = NULL;' in thr, 'whs set and cleared around WaitSynchronization')
ini = (ROOT / 'launcher/settings.ini').read_text()
check('[TOTK_SYNC_WAKE_TARGETED]' in ini, 'launcher key')

HEAD = r'''
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define LOG(...) (fprintf(stderr, __VA_ARGS__), fputc('\n', stderr))
#define MAX_H 4096
#define H_CUR_THREAD 0xFFFF8000u
enum { O_FREE, O_THREAD, O_EVENT_R, O_EVENT_W, O_SESSION, O_TMEM };
typedef struct event { int signaled; int refs; } event_t;
typedef struct thr { int exited; int wkind, woke; const uint32_t *whs; int nwh; struct thr *next; } thr_t;
static struct { int type; void *obj; } g_h[MAX_H];
static thr_t *g_wait;
static void wake(thr_t *t, int64_t res) { (void)res; t->wkind = 0; t->woke = 1; }
'''
MAIN = r'''
static uint64_t rs = 88172645463325252ull;
static uint32_t rnd(uint32_t n) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return (uint32_t)(rs % n); }
enum { NE = 60, NT = 40, NW = 48 };
static event_t ev[NE]; static thr_t th[NT]; static thr_t w[NW]; static uint32_t hs[NW][64];
static int state(thr_t *t) {   /* 0 = nothing signaled, 1 = signaled, 2 = bad handle */
    int bad = 0, idx = -1;
    for (int i = 0; i < t->nwh && idx < 0; i++) if (sig_obj(t->whs[i], &bad)) idx = i;
    return bad ? 2 : idx >= 0 ? 1 : 0;
}
int main(int argc, char **argv) {
    g_swt = argc > 1 ? atoi(argv[1]) : -1;
    if (g_swt < 0) sync_wake_targeted();
    long woken = 0, skipped = 0, steps = 0;
    for (int round = 0; round < 400; round++) {
        memset(g_h, 0, sizeof g_h); memset(ev, 0, sizeof ev); memset(th, 0, sizeof th);
        int nh = 1 + (int)rnd(300);
        for (int i = 1; i < nh; i++) {
            int k = (int)rnd(10);
            if (k < 5) { g_h[i].type = O_EVENT_R; g_h[i].obj = &ev[rnd(NE)]; }
            else if (k < 8) { g_h[i].type = O_THREAD; g_h[i].obj = &th[rnd(NT)]; }
            else if (k < 9) { g_h[i].type = O_EVENT_W; g_h[i].obj = &ev[rnd(NE)]; }
        }
        g_wait = NULL;
        for (int j = 0; j < NW; j++) {
            w[j] = (thr_t){0};
            int n = rnd(4) == 0 ? (int)rnd(65) : (int)rnd(5);
            for (int tries = 0; tries < 1000; tries++) {
                for (int i = 0; i < n; i++) {
                    uint32_t r = rnd(20);
                    hs[j][i] = r == 0 ? H_CUR_THREAD : 0x1000u + 1u + rnd((uint32_t)(nh - 1 > 0 ? nh - 1 : 1));
                }
                w[j].whs = hs[j]; w[j].nwh = n;
                if (state(&w[j]) == 0) break;
                n = (int)rnd(3);
            }
            if (state(&w[j]) != 0) continue;
            w[j].wkind = 4; w[j].next = g_wait; g_wait = &w[j];
        }
        for (int s = 0; s < 40; s++, steps++) {
            int sb[NW];
            for (int j = 0; j < NW; j++) sb[j] = w[j].wkind == 4 ? state(&w[j]) : -1;
            for (int j = 0; j < NW; j++) w[j].woke = 0;
            int what = (int)rnd(4);
            if (what == 0) {   /* a handle goes bad (closed), as CloseHandle while another thread waits */
                int i = 1 + (int)rnd((uint32_t)(nh > 1 ? nh - 1 : 1)); g_h[i].type = O_FREE; g_h[i].obj = 0;
                event_t *e = &ev[rnd(NE)]; e->signaled = 1; wake_sync_waiters(e);
            } else if (what == 1) { thr_t *t = &th[rnd(NT)]; t->exited = 1; wake_sync_waiters(t); }
            else { event_t *e = &ev[rnd(NE)]; e->signaled = 1; wake_sync_waiters(e); }
            for (int j = 0; j < NW; j++) {
                if (sb[j] < 0) continue;
                if (w[j].woke) { woken++; continue; }
                skipped++;
                if (!g_swt) { printf("FAIL: =0 left a waiter asleep\n"); return 1; }
                if (state(&w[j]) != sb[j]) { printf("FAIL: waiter %d left asleep but its re-check changed (%d -> %d)\n", j, sb[j], state(&w[j])); return 1; }
            }
            /* re-blocked waiters (state 0 again) go back on the list, like the WaitSynchronization loop */
            g_wait = NULL;
            for (int j = 0; j < NW; j++) {
                if (sb[j] < 0 && w[j].wkind != 4) continue;
                if (state(&w[j]) == 0) { w[j].wkind = 4; w[j].next = g_wait; g_wait = &w[j]; } else w[j].wkind = 0;
            }
            for (int i = 0; i < NE; i++) if (rnd(3) == 0) ev[i].signaled = 0;   /* ClearEvent */
        }
    }
    printf("OK steps=%ld woken=%ld skipped=%ld\n", steps, woken, skipped);
    if (g_swt && skipped == 0) { printf("FAIL: nothing skipped\n"); return 1; }
    return 0;
}
'''


def build_run(body, args, tag):
    with tempfile.TemporaryDirectory() as d:
        src = pathlib.Path(d) / 't.c'
        src.write_text(HEAD + sig + body + MAIN)
        exe = pathlib.Path(d) / 't'
        cc = os.environ.get('CC', 'cc')
        r = subprocess.run([cc, '-O1', '-g', '-Wall', '-Werror', '-Wno-unused-function', '-fsanitize=address,undefined',
                            '-fno-sanitize-recover=all', '-o', str(exe), str(src)], capture_output=True, text=True)
        if r.returncode:
            return 'BUILD ' + r.stderr[:2000]
        out = subprocess.run([str(exe)] + args, capture_output=True, text=True, timeout=300)
        return f'{out.returncode} {out.stdout.strip()}'


if not fails:
    for args, want in (([], 'OK'), (['1'], 'OK'), (['0'], 'OK')):
        res = build_run(code, args, 'real')
        print('real', args or 'default', '->', res)
        check(res.startswith('0 OK'), f'real code {args}: {res}')
    probe1 = code.replace('for (int i = 0; i < w->nwh; i++) {', 'for (int i = 0; i < w->nwh && i < 4; i++) {')
    probe2 = code.replace('if (k == 0 || k >= MAX_H) return 1;\n        if (g_h[k].type != O_THREAD && g_h[k].type != O_EVENT_R) return 1;',
                          'if (k == 0 || k >= MAX_H) continue;\n        if (g_h[k].type != O_THREAD && g_h[k].type != O_EVENT_R) continue;')
    for name, p in (('first four handles only', probe1), ('bad handles skipped', probe2)):
        if p == code:
            check(False, f'probe {name} did not apply')
            continue
        res = build_run(p, ['1'], name)
        print('probe', name, '->', res)
        check('FAIL' in res, f'negative probe not caught: {name}')

if fails:
    print('FAIL:\n  ' + '\n  '.join(fails))
    sys.exit(1)
print('PASS test_sync_wake_targeted')
