"""[UIT 0236] Boot-path checks of TOTK_VK_EARLY_STREAM's wiring in src/gfx/backend_vk.c, which only builds for Windows
(the protocol itself runs in tools/tests/test_early_stream.c with the real headers and Windows-like locks).

Checked in the source:
  - the mode is read (early_parts()) before the recording threads start: they read g_early_stream
  - vkb_mt_submit opens the gate after the walk and closes it after the wait loop, before the last part is assembled
  - in early_try, every return after vkb_early_enter releases the gate first (a held gate would hang the next close:
    the black-screen kind of bug of 0232)
  - early_try is never called while this thread holds g_mt_mu, g_pro_mu or the gate (non-recursive locks on Windows),
    and early_try takes the locks in the order gate -> g_mt_mu -> g_pro_mu -> submit queue
  - the submit thread's early-part path neither signals nor calls vkb_subq_done (the fence's last item does)
  - vkb_subq_push keeps items of one fence in push order (<=), so early parts go before their fence's last part
usage: python tools/tests/test_early_stream_wiring.py
"""
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent.parent
fails = []


def check(cond, msg):
    if not cond:
        fails.append(msg)


def func(src, name):
    """The body of a top-level C function `name` (from its definition line to the closing brace in column 0)."""
    m = re.search(r'^(?:static |void |uint64_t |int )[^\n;]*\b' + re.escape(name) + r'\([^;{]*\)\s*\{', src, re.M)
    if not m:
        return ''
    end = src.find('\n}\n', m.end())
    return src[m.start():end + 3]


be = (ROOT / 'src/gfx/backend_vk.c').read_text(errors='replace')
subq = (ROOT / 'src/gfx/vkb_subq.h').read_text()

mt = func(be, 'vkb_mt_submit')
check(mt, 'vkb_mt_submit not found')
i_mode = mt.find('early_parts();')
i_threads = mt.find('pthread_create(')
check(0 <= i_mode < i_threads, 'early_parts() must run before the recording threads are created')
i_open = mt.find('if (early_parts() > 1 && !g_pbc_walk_open) vkb_early_set_open(&g_early_gate, 1)')
i_close = mt.find('vkb_early_set_open(&g_early_gate, 0)')
i_total = mt.find('g_early_total += ')
i_wait = mt.find('while (g_mt_done < g_nch)')
i_last = mt.find('if (!g_slot_cb_sent) glue(')
check(0 <= i_total < i_open, 'the gate opens only after the walk counted the draws')
check(0 <= i_wait < i_close, 'the gate closes after the wait loop')
check(0 <= i_close < i_last, 'the gate closes before the last part is assembled')
check(mt.count('vkb_early_set_open(') == 3, 'vkb_mt_submit opens the gate once (after the walk, or before it with 0258) and closes it once')
# [UIT 0258] TOTK_VK_PRESYNC_BY_CHUNK: open before the walk only with the stream (its plan never sends the last cut chunk),
# with the part counters reset first; the walk's chunk_close may then send
i_wopen = mt.find('g_pbc_walk_open = g_pbc_live && g_early_stream && early_parts() > 1;')
i_wreset = mt.find('if (g_pbc_walk_open) { g_early_next = g_early_sent = 0; g_early_total = 0; vkb_early_set_open(&g_early_gate, 1); }')
i_walk = mt.find('EX_WALK')
i_noreset = mt.find('if (!g_pbc_walk_open) { g_early_next = g_early_sent = 0; g_early_total = 0; }')
check(0 <= i_wopen < i_wreset < i_walk, 'the gate opens before the walk only with the stream and a presync plan, counters reset first')
check(0 <= i_walk < i_noreset < i_total < i_open, 'after the walk: no reset of the parts already sent; the usual open otherwise')
check(mt.find('g_pbc_walk_open = 0;') > i_open, 'g_pbc_walk_open ends after the walk')
cc = func(be, 'chunk_close')
check(cc and cc.find('pbc_sync(g_nch, 1);') < cc.find('g_mt_avail = ++g_nch;') < cc.find('if (g_pbc_walk_open) early_try(0);'), 'chunk_close: pools before the chunk is published; a send try after the lock')
ep = (ROOT / 'src/gfx/vkb_early.h').read_text()
check('if (p >= nch) return next;' in ep, 'vkb_early_stream_plan never sends every cut chunk (the walk-time open relies on it)')

et = func(be, 'early_try')
check(et, 'early_try not found')
lines = et.splitlines()
entered = False
for ln in lines:
    if 'vkb_early_enter(' in ln:
        entered = True
        continue
    if entered and re.search(r'\breturn\b', ln):
        check('vkb_early_leave(' in ln, 'early_try returns without vkb_early_leave: ' + ln.strip())
check(entered, 'early_try does not use the gate')
check(lines and 'vkb_early_leave(&g_early_gate);' in lines[-2], 'early_try ends by releasing the gate')
order = [et.find(s) for s in ('vkb_early_enter(', 'pthread_mutex_lock(&g_mt_mu)', 'pthread_mutex_unlock(&g_mt_mu)',
                               'pthread_mutex_lock(&g_pro_mu)', 'pthread_mutex_unlock(&g_pro_mu)', 'vkb_subq_push(')]
check(all(x >= 0 for x in order) and order == sorted(order),
      'early_try lock order: gate, g_mt_mu (released), g_pro_mu (released), then the submit queue')


def calls_while_held(body, fname):
    """Lines that call early_try( while a lock taken in this body is still held (by line order)."""
    held, bad = set(), []
    for ln in body.splitlines():
        for m in re.finditer(r'pthread_mutex_(lock|unlock)\(&(\w+)\)|vkb_early_(enter|leave)\(|early_try\(', ln):
            if m.group(1) == 'lock':
                held.add(m.group(2))
            elif m.group(1) == 'unlock':
                held.discard(m.group(2))
            elif m.group(3) == 'enter':
                held.add('gate')
            elif m.group(3) == 'leave':
                held.discard('gate')
            elif held and not ln.lstrip().startswith('static void early_try'):
                bad.append(f'{fname}: early_try called holding {sorted(held)}: {ln.strip()}')
    return bad


for name in ('vkb_mt_submit', 'mt_thread'):
    body = func(be, name)
    check(body, name + ' not found')
    for b in calls_while_held(body, name):
        fails.append(b)
check('early_try(0)' in func(be, 'mt_thread'), 'the recording threads try to send (early_try(0))')

st = func(be, 'submit_thread')
m = re.search(r'if \(r->partial\) \{(.*?)\n        \}', st, re.S)
check(m, 'submit_thread has no early-part path')
if m:
    part = m.group(1)
    check('vkb_subq_done' not in part and 'jobs_push' not in part and 'out_ready' not in part,
          'an early part must not call vkb_subq_done, push a retire job or mark a frame ready')
    check('VK_NULL_HANDLE' in part and 'pSignalSemaphoreInfos' not in part, 'an early part signals nothing')
    check('continue;' in part and 'free(r)' in part, 'an early part is freed and the loop goes on')

check(re.search(r'\(\*p\)->fence <= it->fence', subq), 'vkb_subq_push must keep items of one fence in push order (<=)')
check('fence != g_early_fence' in func(be, 'mtl_sub_commit'), 'mtl_sub_commit checks the early parts\' fence')

if fails:
    for f in fails:
        print('FAIL:', f)
    sys.exit(1)
print('test_early_stream_wiring: PASS')
