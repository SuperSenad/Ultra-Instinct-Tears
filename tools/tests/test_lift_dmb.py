"""[UIT 0404] tools/lifter/lifter.py: DMB ...LD becomes an acquire fence and DMB ...ST a release fence (compiler
barriers on x86); the full DMB, every DSB and ISB keep the sequentially consistent fence; CLREX still clears the
monitor.     py -3 tools/tests/test_lift_dmb.py"""
import pathlib
import sys
HERE = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(HERE / 'tools' / 'lifter'))
from lifter import Lifter  # noqa: E402

ok = True


def check(c, w):
    global ok
    print(('ok   ' if c else 'FAIL ') + w)
    ok &= bool(c)


L = Lifter.__new__(Lifter)


def text(w):
    return L.system(w, 0x800000000).code


def barrier(op2, crm):   # DMB/DSB/ISB/CLREX: 1101 0101 0000 0011 0011 CRm op2 11111
    return 0xD503301F | crm << 8 | op2 << 5


ACQ, REL, SEQ = '__atomic_thread_fence(__ATOMIC_ACQUIRE);', '__atomic_thread_fence(__ATOMIC_RELEASE);', '__atomic_thread_fence(__ATOMIC_SEQ_CST);'
check(text(0xD50339BF) == ACQ, 'DMB ISHLD: acquire')
check(text(0xD5033ABF) == REL, 'DMB ISHST: release')
check(text(0xD5033BBF) == SEQ, 'DMB ISH: sequentially consistent')
for crm in range(16):
    want = ACQ if crm & 3 == 1 else REL if crm & 3 == 2 else SEQ
    check(text(barrier(5, crm)) == want, f'DMB CRm={crm:#x}')
    check(text(barrier(4, crm)) == SEQ, f'DSB CRm={crm:#x} stays sequentially consistent')
check(text(barrier(6, 15)) == SEQ, 'ISB stays sequentially consistent')
check(text(barrier(2, 15)) == 'c->excl_addr = ~0ull;', 'CLREX clears the monitor')
print('PASS' if ok else 'FAIL')
sys.exit(0 if ok else 1)
