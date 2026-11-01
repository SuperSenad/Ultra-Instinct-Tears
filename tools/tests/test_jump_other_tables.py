"""Box test for the lifter's jump tables whose register holds one of several tables (UIT 0353, tools/lifter/gen.py
_jump_table_other_bases): a function picks table A or table B on two paths, then a bounds-checked byte table and a br.
The emulation reads only the table written last (B); the other one's targets must be added, a candidate whose entries
leave the function must not, and TOTK_LIFT_OTHER_TABLES=0 gives the old result.
    py -3 tools/tests/test_jump_other_tables.py"""
import collections, os, pathlib, sys, types
HERE = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(HERE / 'tools' / 'lifter'))
import gen
from lifter import Lifter

ok = True
def check(c, w):
    global ok
    if not c:
        ok = False
        print('FAIL', w)

BASE = 0x8000000000
NOP, RET = 0xD503201F, 0xD65F03C0
def cmpw(rn, imm): return 0x7100001F | imm << 10 | rn << 5
def bcond(pc, to, cond): return 0x54000000 | (((to - pc) >> 2) & 0x7FFFF) << 5 | cond
def b(pc, to): return 0x14000000 | (((to - pc) >> 2) & 0x3FFFFFF)
def adrp(pc, rd, to):
    imm = ((to >> 12) - (pc >> 12)) & 0x1FFFFF
    return 0x90000000 | (imm & 3) << 29 | (imm >> 2) << 5 | rd
def adr(pc, rd, to):
    imm = (to - pc) & 0x1FFFFF
    return 0x10000000 | (imm & 3) << 29 | (imm >> 2) << 5 | rd
def addi(rd, rn, imm): return 0x91000000 | imm << 10 | rn << 5 | rd
def ldrb(rt, rn, rm): return 0x38606800 | rm << 16 | rn << 5 | rt
def addlsl(rd, rn, rm, k): return 0x8B000000 | rm << 16 | k << 10 | rn << 5 | rd
def br(rn): return 0xD61F0000 | rn << 5

def build(table_c):
    """entry: w0 == 0 ? x20 = A : x20 = B (B last in the code); 3 cases on w1; the cases are 8 ret blocks."""
    w = [NOP] * 64
    pc = lambda i: BASE + 4 * i
    CASES = 32                      # case k at word 32 + 2k
    TA, TB, TC = BASE + 0x1000, BASE + 0x1010, BASE + 0x1020
    w[0] = cmpw(0, 0)
    w[1] = bcond(pc(1), pc(5), 0)   # b.eq -> B path
    w[2] = adrp(pc(2), 20, TA); w[3] = addi(20, 20, TA & 0xFFF)
    w[4] = b(pc(4), pc(8))
    w[5] = adrp(pc(5), 20, TB); w[6] = addi(20, 20, TB & 0xFFF)
    w[7] = NOP
    if table_c:                     # a third constant into x20 whose entries leave the function: never used
        w[7] = adr(pc(7), 20, TC)
    w[8] = cmpw(1, 2)
    w[9] = bcond(pc(9), pc(30), 8)  # b.hi default
    w[10] = adr(pc(10), 9, pc(CASES))
    w[11] = ldrb(10, 20, 1)
    w[12] = addlsl(9, 9, 10, 2)
    w[13] = br(9)
    w[30] = RET; w[31] = RET
    for k in range(8):
        w[CASES + 2 * k] = 0x52800000 | k << 5   # movz w0, #k
        w[CASES + 2 * k + 1] = RET
    image = bytearray(0x2000)
    image[:len(w) * 4] = b''.join(x.to_bytes(4, 'little') for x in w)
    image[0x1000:0x1003] = bytes([0, 2, 4])          # A: cases 0, 1, 2 -> words 32, 34, 36
    image[0x1010:0x1013] = bytes([6, 8, 10])         # B: cases 3, 4, 5
    image[0x1020:0x1023] = bytes([200, 201, 202])    # C: outside the function
    m = gen.Module.__new__(gen.Module)
    m.name, m.base, m.lo, m.hi, m.words = 'main', BASE, BASE, BASE + len(w) * 4, tuple(w)
    m.lifter, m.cache, m.stats, m.mu, m.spins = Lifter(BASE, 'c'), {}, collections.Counter(), None, {}
    m.nso = types.SimpleNamespace(image=bytes(image))
    return m, pc(13), BASE, BASE + len(w) * 4, [pc(CASES + 2 * k) for k in range(8)]

for with_c in (False, True):
    m, brpc, entry, end, case = build(with_c)
    os.environ.pop('TOTK_LIFT_OTHER_TABLES', None)
    got = m.jump_table(brpc, entry, end)
    want = sorted(case[0:6]) if not with_c else None
    if not with_c:
        check(got == want, f'both tables: {got and [hex(x) for x in got]} != {[hex(x) for x in want]}')
        check(m.stats['jumptables_other_bases'] == 1, f'stats {dict(m.stats)}')
        os.environ['TOTK_LIFT_OTHER_TABLES'] = '0'
        m2, *_ = build(False)
        old = m2.jump_table(brpc, entry, end)
        check(old == sorted(case[3:6]), f'switch off: {old and [hex(x) for x in old]}')
        check(m2.stats['jumptables_other_bases'] == 0, 'switch off counted')
        os.environ.pop('TOTK_LIFT_OTHER_TABLES', None)
    else:
        # the emulation now reads C last (out of the function): the table is dropped as before, nothing invented
        check(got is None or all(entry <= t < end for t in got), f'with an invalid table: {got}')

print('OK' if ok else 'FAILED')
sys.exit(0 if ok else 1)
