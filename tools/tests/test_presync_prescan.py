#!/usr/bin/env python3
# [UIT 0249] TOTK_VK_PRESYNC_PARALLEL: cuts the job hand-out from src/gfx/backend_vk.c, runs it with the real
# write-watch tracking (test_presync_prescan.c), on and off, and checks the wiring. Negative probes: broken variants of
# the tracking or the hand-out must fail.
import os, re, subprocess, sys, tempfile
R = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..'))
CC = os.environ.get('CC', 'cc')
LL = os.environ.get('LL', '')   # folder with clang for the TSAN build; empty = clang on PATH
src = open(os.path.join(R, 'src/gfx/backend_vk.c')).read()
dirty = open(os.path.join(R, 'src/gfx/nvn/nvn_dirty_win.c')).read()
fails = []
def check(c, m):
    if not c: fails.append(m)

def cut(s):
    a = s.index('enum { PS_MIN = 24 };')
    b = s.index('static void *mt_thread(void *arg)')
    return s[a:b]

def build(tmp, job, dirty_src, tsan=False):
    open(os.path.join(tmp, 'ps_job.inc'), 'w').write(job)
    dp = os.path.join(tmp, 'nvn_dirty_win.c')
    open(dp, 'w').write(dirty_src)
    t = os.path.join(R, 'tools/tests/test_presync_prescan.c')
    out = os.path.join(tmp, 'tps_t' if tsan else 'tps')
    cc = [os.path.join(LL, 'clang'), '-O1', '-g', '-fsanitize=thread'] if tsan else [CC, '-O2']
    cmd = cc + ['-pthread', '-I' + tmp, '-I' + os.path.join(R, 'src/gfx/nvn'), t, dp, '-o', out]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode:
        print(r.stderr[-3000:]); return None
    return out

def run(exe, mode, on, rounds):
    env = dict(os.environ, TOTK_VK_PRESYNC_PARALLEL='1' if on else '0', TSAN_OPTIONS='halt_on_error=1')
    r = subprocess.run([exe, mode, str(rounds)], capture_output=True, text=True, env=env, timeout=600)
    m = dict(kv.split('=') for kv in r.stdout.split()) if r.stdout.startswith('hash=') else {}
    return r.returncode, m, r.stdout + r.stderr[-2000:]

def verdict(job, dsrc, tsan=False, rounds=300):   # list of problems found ([] = all good)
    bad = []
    with tempfile.TemporaryDirectory() as tmp:
        exe = build(tmp, job, dsrc, tsan)
        if not exe: return ['does not build']
        rc0, off, o0 = run(exe, 'same', 0, rounds)
        rc1, on, o1 = run(exe, 'same', 1, rounds)
        if rc0 or rc1 or not off or not on: return ['same: failed: ' + o0 + o1]
        if off['hash'] != on['hash']: bad.append('same: on copies other runs than off (%s vs %s)' % (on['hash'], off['hash']))
        if int(off['jobs']) != 0: bad.append('same: off handed out jobs')
        if int(on['jobs']) < rounds: bad.append('same: on handed out %s jobs' % on['jobs'])
        if int(on['loop_wcalls']) != 0: bad.append('same: on: the loop still made %s write-watch calls' % on['loop_wcalls'])
        if on['calls'] != on['items']: bad.append('same: on scanned %s times for %s items (an item taken twice or never)' % (on['calls'], on['items']))
        if int(on['helped']) == 0: bad.append('same: the recording threads never joined')
        if on['wcalls'] != off['wcalls']: bad.append('same: write-watch calls on %s, off %s' % (on['wcalls'], off['wcalls']))
        for v in (0, 1):
            rc, m, o = run(exe, 'race', v, rounds // 2 if tsan else rounds)
            if rc or not m or m.get('stale') != '0': bad.append('race %s: %s' % ('on' if v else 'off', o.strip()[-600:]))
        print('   on : %s' % ' '.join('%s=%s' % kv for kv in on.items()))
        print('   off: %s' % ' '.join('%s=%s' % kv for kv in off.items()))
    return bad

job = cut(src)
print('real code:')
b = verdict(job, dirty)
check(not b, 'real: %s' % b)
print('real code, thread sanitizer:')
b = verdict(job, dirty, tsan=True, rounds=120)
check(not b, 'tsan: %s' % b)

# negative probes
P = 'scan_pages(r, r->base, r->end - r->base, 1); atomic_store(&r->epoch, e); did = 1;'
assert P in dirty
probes = [
    ('prescan without counting generations', job, dirty.replace(P, 'scan_pages(r, r->base, r->end - r->base, 0); atomic_store(&r->epoch, e); did = 1;'), False),
    ('prescan does not mark the epoch', job, dirty.replace(P, 'scan_pages(r, r->base, r->end - r->base, 1); did = 1;'), False),
    ('prescan scans half of the pool', job, dirty.replace(P, 'scan_pages(r, r->base, (r->end - r->base) / 2, 1); atomic_store(&r->epoch, e); did = 1;'), False),
    ('prescan for another epoch', job.replace('atomic_store(&g_ps_epoch, nvn_dirty_epoch_now());', 'atomic_store(&g_ps_epoch, nvn_dirty_epoch_now() + 1);'), dirty, False),
    ('prescan picks other pools than the loop', job.replace('g_pools[i].used_sub + win >= sub) g_ps_addr[k++]', 'g_pools[i].used_sub + win > sub) g_ps_addr[k++]'), dirty, False),
    ('recording threads take items twice', job.replace('if (!atomic_compare_exchange_weak(&g_ps_word, &w, w + 1)) continue;', 'sched_yield(); atomic_store(&g_ps_word, w + 1);'), dirty, True),   # the yield opens the window so two threads take the same item every run, not only sometimes
]
for name, j, d, ts in probes:
    if j == job and d == dirty: fails.append('probe %s did not apply' % name); continue
    print('probe: %s' % name)
    b = verdict(j, d, tsan=ts, rounds=120 if ts else 300)
    print('   caught: %s' % (b[:2] if b else 'NOTHING'))
    check(b, 'probe not caught: ' + name)

# wiring
sp = src[src.index('static void shadow_presync(void) {'):]
sp = sp[:sp.index('\n}\n')]
check('if (t_ps_lead) presync_prescan(sub, win, NULL, 0);' in sp and sp.index('presync_prescan') > sp.index('__atomic_store_n(&g_presync_sub, sub') and sp.index('presync_prescan') < sp.index('for (int i = 0; i < g_npools'), 'shadow_presync: prescan after taking the presync, before the loop')
loopc = re.search(r'if \((g_pools\[i\]\.sh && g_pools\[i\]\.sync_sub != sub && g_pools\[i\]\.used_sub \+ win >= sub)\) \{ sync_gather', sp)
check(loopc is not None, 'shadow_presync loop condition')
if loopc: check(job.count(loopc.group(1)) == 2, 'prescan picks the same pools as the loop')
check(src.count('t_ps_lead = 1;') == 1 and re.search(r't_ps_lead = 1;   // \[UIT 0249\]\n    if \(pbc_on\(\)\) pbc_presync\(\);[^\n]*\n    else shadow_presync\(\);\n    t_ps_lead = 0;', src) is not None, 't_ps_lead only around the prologue presync')
check(src.count('presync_prescan(') == 4, 'presync_prescan: declared, defined, called by shadow_presync and pbc_presync')
# [UIT 0258] pbc_presync: the scans of chunk 0's pools only (the ordered plan's first n0 entries), after taking the presync
pp = src[src.index('static void pbc_presync(void) {'):]
pp = pp[:pp.index('\n}\n')]
check('if (t_ps_lead) presync_prescan(sub, win, g_pbc.addr, n0);' in pp and pp.index('presync_prescan') > pp.index('vkb_pbc_order(') > pp.index('__atomic_store_n(&g_presync_sub, sub'), 'pbc_presync: prescan of the ordered plan after taking the presync')
check('n0 += !g_pbc_tmp_c[n];' in pp and 'if (presync_due(&g_pools[i], sub, win))' in pp, 'pbc_presync: n0 counts the pools wanted by chunk 0, picked as the loop picks')
due = src[src.index('static int presync_due('):]
due = due[:due.index('\n}\n')]
check('return p->sh && p->sync_sub != sub && p->used_sub + win >= sub;' in due, 'presync_due is the loop condition')
check('if (only) for (; k < n; k++) g_ps_addr[k] = (const void *)only[k];' in job, 'presync_prescan: the list given')
mt = src[src.index('static void *mt_thread(void *arg)'):]
mt = mt[:mt.index('int k = g_mt_next++;')]
check('while (g_mt_next >= g_mt_avail && g_ps_wake == ps_seen) pthread_cond_wait(&g_mt_cv, &g_mt_mu);' in mt and 'if (ps_work(ps_seen))' in mt and 'ps_seen = g_ps_wake;' in mt, 'mt_thread joins prescan jobs')
check('if (g_mt_threads < 2 || !presync_par_on()) return;' in job and 'if (n < PS_MIN) return;' in job, 'off / one thread / few pools: as before')
check("v = !(e && e[0] == '0' && !e[1]);" in job, 'default on, =0 off')
check(job.index('| PS_CLOSED);') < job.index('g_ps_addr[k++] ='), 'job closed before its items are rewritten')
check('[UIT 0249] TOTK_VK_PRESYNC_PARALLEL' in src and 'TOTK_VK_PRESYNC_PARALLEL)",' in src, 'log lines')
ini = open(os.path.join(R, 'launcher/settings.ini')).read()
check('[TOTK_VK_PRESYNC_PARALLEL]' in ini, 'launcher key')
if fails:
    print('FAIL'); [print(' -', f) for f in fails]; sys.exit(1)
print('PASS')
