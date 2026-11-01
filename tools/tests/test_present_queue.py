"""[UIT 0248] TOTK_VK_PRESENT_QUEUE: the present thread's vkQueueSubmit2 + vkQueuePresentKHR on a second queue of the
same family under a lock of its own, so the replay's submits never wait behind a present blocked in the driver.
Cuts the queue locks out of src/vk/vk.c and runs them in threads: with a second queue the present lock must not wait
for a held replay lock (and two present lockers still exclude each other); without one (=0, or a one-queue family)
the present lock must be the replay's lock exactly as before (it waits until the replay lock is released). The lock
wait statistics must count a contended wait and its time. Wiring: two queues are asked for only when wanted, a window
exists and the family has two; queue index 1 only then; present.c uses only vkc.pqueue / vk_pqueue_lock; the replay's
submits stay on vkc.queue; the launcher key. Negative probes: a present lock that ignores pqueue_own, a lock without
its statistics, and a present.c still on vkc.queue must each be caught.
usage: python tools/tests/test_present_queue.py
"""
import os
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


vkc_src = (ROOT / 'src/vk/vk.c').read_text()
pres = (ROOT / 'src/vk/present.c').read_text()
back = (ROOT / 'src/gfx/backend_vk.c').read_text()
a = vkc_src.find('static pthread_mutex_t queue_mu')
b = vkc_src.find('void vk_pqueue_unlock(void)')
b = vkc_src.find('\n', b) + 1
locks = vkc_src[a:b] if a >= 0 and b > a else ''
check('vk_pqueue_lock' in locks and 'qlock(' in locks, '0248 lock code not found in vk.c')


def wiring(vk_text, pres_text, back_text):
    bad = []
    if 'int pq_two = pq_want && vkc.surface && fam_queues >= 2;' not in vk_text: bad.append('pq_two condition')
    if '.queueCount = pq_two ? 2u : 1u' not in vk_text: bad.append('queue count not tied to pq_two')
    if 'if (pq_two) { vk.vkGetDeviceQueue(vkc.device, vkc.family, 1, &vkc.pqueue);' not in vk_text: bad.append('queue 1 fetched without pq_two')
    if 'vkc.pqueue = vkc.queue;' not in vk_text: bad.append('pqueue default to queue')
    if "int pq_want = !(pqe && pqe[0] == '0' && !pqe[1]);" not in vk_text: bad.append('switch parse')
    if 'vkc.queue' in pres_text or 'vk_queue_lock' in pres_text: bad.append('present.c still uses the replay queue / lock')
    if pres_text.count('vkc.pqueue') != 2 or 'vk_pqueue_lock();' not in pres_text or 'vk_pqueue_unlock();' not in pres_text:
        bad.append('present.c submit + present on pqueue under vk_pqueue_lock')
    if len(re.findall(r'vkQueueSubmit2\(vkc\.queue,', back_text)) != 4: bad.append('replay submits on vkc.queue (4 sites)')
    if 'pqueue' in back_text.replace('vkc.pqueue_own', ''): bad.append('backend touches pqueue')
    return bad


for m in wiring(vkc_src, pres, back):
    check(False, 'wiring: ' + m)
for p in (ROOT / 'src').rglob('*.[ch]'):
    if p.name in ('vk.c', 'vk.h', 'present.c', 'backend_vk.c'):
        continue
    check('vkc.pqueue' not in p.read_text(errors='replace'), f'{p} uses vkc.pqueue')
ini = (ROOT / 'launcher/settings.ini').read_text()
check('[TOTK_VK_PRESENT_QUEUE]' in ini, 'launcher key')

HEAD = r'''
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
static struct { int pqueue_own; } vkc;
'''
MAIN = r'''
static uint64_t now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec; }
static void *hold_replay(void *p) { vk_queue_lock(); atomic_store((_Atomic int *)p, 1); usleep(60000); vk_queue_unlock(); return NULL; }
static void *hold_present(void *p) { vk_pqueue_lock(); atomic_store((_Atomic int *)p, 1); usleep(60000); vk_pqueue_unlock(); return NULL; }
static uint64_t timed(void *(*holder)(void *), void (*lk)(void), void (*ul)(void)) {
    _Atomic int held = 0; pthread_t t;
    pthread_create(&t, NULL, holder, (void *)&held);
    while (!atomic_load(&held)) usleep(100);
    uint64_t t0 = now(); lk(); uint64_t d = now() - t0; ul();
    pthread_join(t, NULL);
    return d;
}
int main(void) {
    for (int own = 0; own < 2; own++) {
        vkc.pqueue_own = own;
        uint64_t w0, n0, w1, n1;
        vk_queue_lock_stats(&w0, &n0);
        uint64_t d = timed(hold_replay, vk_pqueue_lock, vk_pqueue_unlock);
        vk_queue_lock_stats(&w1, &n1);
        if (own && d > 20000000ull) { printf("FAIL: own queue: present waited %.1f ms for the replay lock\n", d / 1e6); return 1; }
        if (!own && d < 40000000ull) { printf("FAIL: one queue: present did not wait for the replay lock (%.1f ms)\n", d / 1e6); return 1; }
        if (!own && (n1 - n0 != 1 || w1 - w0 < 40000000ull)) { printf("FAIL: stats %llu waits %.1f ms\n", (unsigned long long)(n1 - n0), (w1 - w0) / 1e6); return 1; }
        if (own && n1 != n0) { printf("FAIL: stats counted an uncontended lock\n"); return 1; }
        d = timed(hold_present, vk_pqueue_lock, vk_pqueue_unlock);
        if (d < 40000000ull) { printf("FAIL: own=%d: two present lockers did not exclude each other\n", own); return 1; }
        d = timed(hold_present, vk_queue_lock, vk_queue_unlock);
        if (own && d > 20000000ull) { printf("FAIL: own queue: replay waited for the present lock\n"); return 1; }
        if (!own && d < 40000000ull) { printf("FAIL: one queue: replay did not wait for the present lock\n"); return 1; }
    }
    printf("OK\n");
    return 0;
}
'''


def build_run(body):
    with tempfile.TemporaryDirectory() as d:
        src = pathlib.Path(d) / 't.c'
        src.write_text(HEAD + body + MAIN)
        exe = pathlib.Path(d) / 't'
        cc = os.environ.get('CC', 'cc')
        r = subprocess.run([cc, '-O1', '-g', '-Wall', '-Werror', '-Wno-unused-function', '-pthread', '-fsanitize=thread',
                            '-o', str(exe), str(src)], capture_output=True, text=True)
        if r.returncode:
            return 'BUILD ' + r.stderr[:2000]
        out = subprocess.run([str(exe)], capture_output=True, text=True, timeout=120)
        return f'{out.returncode} {out.stdout.strip()} {out.stderr.strip()[:300]}'


if locks:
    res = build_run(locks)
    print('real ->', res)
    check(res.startswith('0 OK'), f'real code: {res}')
    probes = (('present lock ignores pqueue_own', locks.replace('qlock(vkc.pqueue_own ? &pqueue_mu : &queue_mu)', 'qlock(&pqueue_mu)')),
              ('lock without statistics', locks.replace('atomic_fetch_add_explicit(&g_qlock_waits, 1, memory_order_relaxed);', '')))
    for name, p in probes:
        if p == locks:
            check(False, f'probe {name} did not apply')
            continue
        res = build_run(p)
        print('probe', name, '->', res)
        check('FAIL' in res, f'negative probe not caught: {name}')
    bad = wiring(vkc_src, pres.replace('vkc.pqueue, 1, &si', 'vkc.queue, 1, &si'), back)
    print('probe present.c on vkc.queue ->', bad)
    check(bool(bad), 'negative probe not caught: present.c on vkc.queue')
    bad = wiring(vkc_src.replace('.queueCount = pq_two ? 2u : 1u', '.queueCount = 2u'), pres, back)
    print('probe two queues always ->', bad)
    check(bool(bad), 'negative probe not caught: two queues always')

if fails:
    print('FAIL:\n  ' + '\n  '.join(fails))
    sys.exit(1)
print('PASS test_present_queue')
