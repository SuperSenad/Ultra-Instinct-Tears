"""[UIT 0443] Hitch attribution: the game-thread side and the wiring.
Cuts hf_note and runtime_frame_waits out of src/host/threads.c and runs them against a small model: blocking waits
ended by the main thread, by more threads than the table has slots ("others"), timeouts; the longest single wait with
its object and kind; deltas of the cumulative wait counters between calls; the first call only starts the values; the
table clears after each call; the main thread asked from itself gives nothing; TOTK_SYNC_LOG unset records and reports
nothing. A negative probe (a table that ignores the handle) must be caught.
Wiring: block() notes every blocking wait (with what it waited on) only with TOTK_SYNC_LOG; the thread publishes and
drops its table under G; hle_nvn.c counts the blocked fence waits and drains per frame and builds, counts and prints the
records only with TOTK_SYNC_LOG; weak stubs for builds without threads.c.
usage: python tools/tests/test_hitch_wiring.py
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
nvn = (ROOT / 'src/gfx/nvn/hle_nvn.c').read_text(errors='replace').replace('\r\n', '\n')
a = thr.find('typedef struct { uint32_t h; uint64_t ns; } hf_slot;'); b = thr.find('\n}\n', thr.find('static void hf_note(')) + 3
note = thr[a:b] if a >= 0 and b > a else ''
a = thr.find('int runtime_frame_waits(int main_thread, uit_tframe *o) {'); b = thr.find('\n}\n', a) + 3
rep = thr[a:b] if a >= 0 and b > a else ''
check(note and rep, '0443 code not found in threads.c')
check('uint64_t hobj = kind == 2 ? t->wkey : kind == 4 ? (t->nwh > 0 ? t->wh[0] : 0) : t->waddr;' in thr, 'block() takes what it waits on')
check('if (t == t_self && hf_on() > 0) hf_note(&t_hf, t->woke ? t->waker : 0, kind, hobj, now_ns() - b0);' in thr, 'block() notes the wait')
check('static void hf_publish(thr_t *t) { g_lock(); t->hf = &t_hf; g_unlock(); }' in thr and thr.count('    hf_publish(t);') == 2 and 't->hf = NULL;' in thr, 'the table is published and dropped')
check('    t->waker = 0;\n    core_put(t); int64_t r = block_inner(t, kind, ns);' in thr, 'UIT 0369 wiring unchanged')
check('hw_note(1, d, NULL); return; }' in nvn and 'hw_note(1, d, seg);' in nvn, 'fw_note counts every blocked fence wait')
check('if (synclog_env()) hw_note(0, dt, NULL);   // [UIT 0443]' in nvn, 'x_drain counts drains')
check('if (synclog_env()) hitch_take(f, fr, (unsigned)(dr - prev_draw));' in nvn, 'records taken at present, gated')
check('if (synclog_env() && r->f == f - 8) hitch_end(f - 8, g, r); }' in nvn, 'records ended 8 frames late, gated')
check('__attribute__((weak)) int runtime_frame_waits(' in nvn and '__attribute__((weak)) int runtime_is_main_thread(' in nvn, 'weak stubs')

HARNESS = r'''
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define UIT_HITCH_TFRAME_ONLY
#include "uit_hitch.h"
typedef struct thr { uint32_t handle; uint64_t *tw; void *hf; struct thr *all; } thr_t;
static thr_t *g_all, *t_self;
static uint32_t g_main_handle;
static void g_lock(void) {}
static void g_unlock(void) {}
%(note)s
%(rep)s
static int fails;
#define CHECK(c, m) do { if (!(c)) { fails++; printf("FAIL: %%s\n", m); } } while (0)
int main(void) {
    static uint64_t tw_m[8], tw_f[8]; static hf_acc hf_m, hf_f;
    thr_t mainth = {0x1001, tw_m, &hf_m, NULL}, frame = {0x1020, tw_f, &hf_f, &mainth}, w1 = {0x1037, 0, 0, 0};
    (void)w1;
    g_all = &frame; g_main_handle = 0x1001; t_self = &frame;
    uit_tframe o;
    g_hf_on = 0;
    hf_note(&hf_f, 0x1001, 2, 0x1234, 5);   // (hf_note itself is only called when on: block() checks)
    memset(&hf_f, 0, sizeof hf_f);
    CHECK(runtime_frame_waits(0, &o) == 0 && o.block_ns == 0, "off: nothing");
    g_hf_on = 1;
    CHECK(runtime_frame_waits(0, &o) == 0, "first call starts the values");
    CHECK(runtime_frame_waits(1, &o) == 0, "first call for the main thread too");
    // the frame thread: 9 ms ended by the main thread in 3 waits, 2 ms by 0x1037, a timeout of 1 ms; core 0.5 ms
    hf_note(&hf_f, 0x1001, 2, 0xA0, 3000000); hf_note(&hf_f, 0x1001, 2, 0xA0, 5000000); hf_note(&hf_f, 0x1001, 1, 0xB0, 1000000);
    hf_note(&hf_f, 0x1037, 4, 0x55, 2000000); hf_note(&hf_f, 0, 3, 0xC0, 1000000);
    tw_f[0] += 500000; tw_f[1] += 12000000; tw_f[2] += 300000; tw_f[3] += 100000;
    CHECK(runtime_frame_waits(0, &o) == 1, "second call reports");
    CHECK(o.core_ns == 500000 && o.block_ns == 12000000 && o.sleep_ns == 300000 && o.yield_ns == 100000, "deltas of the counters");
    CHECK(o.main_ns == 9000000, "ended by the main thread");
    CHECK(o.top_h == 0x1001 && o.top_ns == 9000000, "top waker");
    CHECK(o.big_ns == 5000000 && o.big_obj == 0xA0 && o.big_kind == 2 && o.big_h == 0x1001, "the longest wait, its object, kind, waker");
    CHECK(runtime_frame_waits(0, &o) == 1 && o.block_ns == 0 && o.main_ns == 0 && o.top_ns == 0 && o.big_ns == 0, "cleared after a call");
    // the main thread: more wakers than slots
    for (uint32_t h = 0x2000; h < 0x2008; h++) hf_note(&hf_m, h, 1, h, 1000000 + (h - 0x2000));
    hf_note(&hf_m, 0x2007, 1, 0x2007, 9000000);
    tw_m[1] += 17000000;
    CHECK(runtime_frame_waits(1, &o) == 1 && o.block_ns == 17000000, "main thread deltas");
    CHECK(o.main_ns == 0, "the main thread's own waits are not 'ended by the main thread'");
    CHECK(o.top_h == 0xffffffffu && o.top_ns == 4000022 + 9000000, "others take the wakers past the slots");
    CHECK(o.big_ns == 9000000 && o.big_h == 0xffffffffu && o.big_obj == 0x2007, "the longest: waker shown as others past the slots");
    // asked from the main thread itself: nothing
    t_self = &mainth;
    CHECK(runtime_frame_waits(1, &o) == 0, "main thread from itself");
    // a thread not published (exited)
    t_self = &frame; mainth.hf = NULL;
    CHECK(runtime_frame_waits(1, &o) == 0, "not running");
    printf("%%s\n", fails ? "FAILED" : "ok");
    return fails != 0;
}
'''


def run(note_src, label):
    d = tempfile.mkdtemp()
    src = os.path.join(d, 't.c'); exe = os.path.join(d, 't')
    with open(src, 'w') as f:
        f.write(HARNESS % {'note': note_src, 'rep': rep})
    cc = os.environ.get('CC', 'cc')
    p = subprocess.run([cc, '-O1', '-Wall', '-I', str(ROOT / 'src/gfx'), src, '-o', exe], capture_output=True, text=True)
    if p.returncode:
        return None, p.stderr
    r = subprocess.run([exe], capture_output=True, text=True)
    return r.returncode, r.stdout


if note and rep:
    note_c = note.replace('if (__builtin_expect(g_hf_on < 0, 0)) g_hf_on = getenv("TOTK_SYNC_LOG") != NULL; ', '')
    rc, out = run(note_c, 'real')
    check(rc == 0, 'model failed: %s' % out)
    bad = note_c.replace('if (a->w[i].h == h || !a->w[i].ns) break;', 'if (1) break;')
    check(bad != note_c, 'probe edit applies')
    rc2, out2 = run(bad, 'probe')
    check(rc2 not in (0, None), 'negative probe (table ignores the handle) not caught: %s' % out2)

if fails:
    print('test_hitch_wiring: FAILED'); [print(' -', f) for f in fails]; sys.exit(1)
print('test_hitch_wiring: all passed')
