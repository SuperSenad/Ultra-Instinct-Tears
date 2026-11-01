"""[UIT 0077] Spin-wait gates: the back-edges of spin_waits_v143.txt are lifted as `rt_spin_gate(c, head); goto head`.

A spin wait is a guest loop that polls memory until another thread changes it (found by spin_waits.py). On the
Switch it only fills its own core; here it also holds one of the three virtual cores, so the thread it waits for may
be unable to run. rt_spin_gate (src/host/threads.c) keeps every register and only adds time: it pauses the host core
once per pass and, once a wait has lasted a while and another game thread waits for a virtual core, lends the core
out for a moment (TOTK_SPIN_GATE, off unless 1).

Every back-edge is checked against the user's module (a direct branch to the head, inside the same lifted
function); one that is not is left as it was and counted. TOTK_LIFT_SPIN_GATES=0 at lift time emits no gates.
"""
import os
from pathlib import Path

TABLE = Path(__file__).with_name('spin_waits_v143.txt')
STATS = {'gates': 0, 'skipped': 0}


def load(name, base, path=TABLE):
    """{back-edge address: head address} of module `name` loaded at `base`."""
    if os.environ.get('TOTK_LIFT_SPIN_GATES', '') == '0' or not path.exists():
        return {}
    out = {}
    for raw in path.read_text().splitlines():
        f = raw.split('#', 1)[0].split()
        if len(f) < 4 or f[0] != 'spin' or f[1] != name:
            continue
        head = base + int(f[2], 16)
        for b in f[3:]:
            out[base + int(b, 16)] = head
    return out


def gate(spins, a, target, entry, end):
    """C to put before the jump of the branch at `a` to `target`, or ''."""
    head = spins.get(a)
    if head is None:
        return ''
    if head != target or not entry <= target < end:
        STATS['skipped'] += 1
        return ''
    STATS['gates'] += 1
    return f'rt_spin_gate(c, {target:#x}); '
