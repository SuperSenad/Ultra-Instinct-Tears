"""[UIT 0070] Build-time search of the user's own main (their 1.4.3 dump, lifted on their PC) for three community
patch sites that were published for older game versions only, or as a bare address:

  quality   "remove quality reduction": the b.gt after the FXAA / FSR flag copies (public 1.0-1.2.1 patches: b.gt -> b)
  fxaa      the FXAA flag load of the same trio (1.1.x comments, the 1.2.1 FXAA ExeFS patch: ldrb -> mov w, #0)
  lod       theboy181's Increased LOD, ported to 1.4.3 (GameBanana 639965: main+0x67F01C -> b +0x90)
  shadow    the shadow map size (public 1.0-1.2.1 patches: movz w11, #size; b +0x5C at the site)

Nothing here comes from a game file: only instruction *shapes* (opcodes with register / offset wildcards, constants,
distances) described by those public patches. The user's binary is read at build time on their PC, as the lift does.

A feature is switched on only when exactly one place matches with confidence; otherwise it stays off, the launcher
greys its setting out with the reason (data/generated/lift-features.ini) and data/generated/lift-scan.txt lists what was
found (addresses, the instruction words of the window, scores) so a next round can narrow it down.

The shadow site has no published 1.4.3 address and no published original code, so a static match alone is never
enough: candidates get a run-time probe (the value of w11 where the patch would rejoin, S + 0x60) and the host matches it
against the square depth textures the game creates (src/host/uit_lift.c). A candidate confirmed that way is written to
diag/lift-confirm.txt; the next tools/setup.py run lifts again and switches the setting on.

The pure functions (find_trio, check_lod, shadow_candidates) take a word list and its base address, so tests can feed
them synthetic code (tools/tests/test_lift_scan.py)."""
import bisect
import os
import re
import sys

MAIN = 0x800000000
LOD_BUILD = '277178b7dba1b6d4949a84778d4abc58b31b34f5'   # 1.4.3 main (theboy181's port: @nsobid-277178B7...)
LOD_SITE = MAIN + 0x67F01C                                 # 0067F11C with offset_shift 0x100
LOD_SITES = {LOD_BUILD[:16]: (LOD_SITE, '1.4.3'),          # build id -> (site, version); the same port's 1.4.2 file:
             '5cb42b1cf25469fb': (MAIN + 0x896548, '1.4.2')}   # @nsobid-5CB42B1C... 00896648 24000014
LOD_SKIP = 0x90                                            # 24000014 = b +0x90
SHADOW_SKIP = 0x60                                         # movz w11 at S, b +0x5C at S + 4 -> S + 0x60
SHADOW_SIZES = (256, 512, 1024, 2048, 4096)
PROBE_MAX = 512   # run-time probes at most (a disabled probe is one predicted branch)
COND_NAMES = {0x0: 'eq', 0x1: 'ne', 0x2: 'hs', 0x3: 'lo', 0x8: 'hi', 0x9: 'ls', 0xA: 'ge', 0xB: 'lt', 0xC: 'gt', 0xD: 'le'}


# ---------------------------------------------------------------- instruction shapes
def sext(v, bits):
    return v - (1 << bits) if v & (1 << (bits - 1)) else v


def ldrb(w):
    """ldrb wt, [xn, #imm] -> (rt, rn, imm)"""
    return (w & 31, (w >> 5) & 31, (w >> 10) & 0xFFF) if (w & 0xFFC00000) == 0x39400000 else None


def strb(w):
    return (w & 31, (w >> 5) & 31, (w >> 10) & 0xFFF) if (w & 0xFFC00000) == 0x39000000 else None


def mov_x(w):
    """mov xd, xm (orr xd, xzr, xm) -> (rd, rm)"""
    return (w & 31, (w >> 16) & 31) if (w & 0xFFE0FFE0) == 0xAA0003E0 else None


def is_bl(w):
    return (w & 0xFC000000) == 0x94000000


def cmp_w_imm(w):
    """cmp wn, #imm (subs wzr, wn, #imm{, lsl 12}) -> (rn, imm)"""
    if (w & 0xFF80001F) != 0x7100001F:
        return None
    imm = (w >> 10) & 0xFFF
    return ((w >> 5) & 31, imm << 12 if (w >> 22) & 1 else imm)


def bcond(w, pc):
    """b.cond -> (cond, target)"""
    if (w & 0xFF000010) != 0x54000000:
        return None
    return (w & 0xF, pc + sext((w >> 5) & 0x7FFFF, 19) * 4)


def is_b(w):
    return (w & 0xFC000000) == 0x14000000


def b_target(w, pc):
    return pc + sext(w & 0x3FFFFFF, 26) * 4


def ends_flow(w):
    """bl / blr / br / ret / eret / svc / brk / hlt / udf: the end of straight-line code for the shadow search"""
    return (is_bl(w) or (w & 0xFE000000) == 0xD6000000 or (w & 0xFF000000) == 0xD4000000 or w == 0 or
            (w & 0xFFFF0000) == 0)


def any_branch_target(w, pc):
    """the target of b / b.cond / cbz / cbnz / tbz / tbnz (None for anything else)"""
    if is_b(w):
        return b_target(w, pc)
    c = bcond(w, pc)
    if c:
        return c[1]
    if (w & 0x7E000000) == 0x34000000:   # cbz / cbnz
        return pc + sext((w >> 5) & 0x7FFFF, 19) * 4
    if (w & 0x7E000000) == 0x36000000:   # tbz / tbnz
        return pc + sext((w >> 5) & 0x3FFF, 14) * 4
    return None


def movz_imm(w):
    """mov wd, #imm (movz, any shift) / orr wd, wzr, #bitmask (power of two only) -> (rd, value)"""
    if (w & 0x7F800000) == 0x52800000:
        return (w & 31, ((w >> 5) & 0xFFFF) << (16 * ((w >> 21) & 3)))
    if (w & 0xFFC003E0) == 0x320003E0:   # orr wd, wzr, #imm, N=0: a single set bit when imms == 0
        immr, imms = (w >> 16) & 63, (w >> 10) & 63
        if imms == 0:
            return (w & 31, 1 << ((32 - immr) % 32))
    return None


# ---------------------------------------------------------------- the FXAA / FSR / quality-reduction trio
def find_trio(words, base):
    """Tier A: the exact layout of 1.1.1 / 1.1.2 (0xcc1c10 / 0xc76fe0), registers and offsets as wildcards:
        X      ldrb wA, [x0]           (FXAA flag)
        X+4    mov  x0, xP
        X+8    strb wA, [xS, #k]
        X+12   bl   ...
        X+16   ldrb wB, [x0]           (FSR flag)
        X+20   cmp  wR, #8
        X+24   strb wB, [xS, #k+1]
        X+28   b.gt T                  (quality reduction; T forward)
    Tier B (a relaxed window, for a compiler that scheduled it differently): two "ldrb w, [x0]" stored to [xS, #k] and
    [xS, #k+1] with a bl between the loads, then within 3 instructions a cmp w, #imm and a forward b.gt/ge/hi/hs
    jumping at least 0x40. Each match: dict(fxaa, fsr, branch, target, cond, k, tier)."""
    n = len(words)
    tier_a, tier_b = [], []
    for i in range(n - 8):
        a = ldrb(words[i])
        if not a or a[1] != 0 or a[2] != 0:
            continue
        mv, s1, b2, cm, s2 = mov_x(words[i + 1]), strb(words[i + 2]), ldrb(words[i + 4]), cmp_w_imm(words[i + 5]), strb(words[i + 6])
        pc7 = base + (i + 7) * 4
        bc = bcond(words[i + 7], pc7)
        if (mv and mv[0] == 0 and s1 and s1[0] == a[0] and is_bl(words[i + 3]) and b2 and b2[1] == 0 and b2[2] == 0 and
                cm and cm[1] == 8 and s2 and s2[0] == b2[0] and s2[1] == s1[1] and s2[2] == s1[2] + 1 and
                bc and bc[0] == 0xC and bc[1] > pc7):
            tier_a.append(dict(fxaa=base + i * 4, fsr=base + (i + 4) * 4, branch=pc7, target=bc[1], cond=bc[0],
                               k=s1[2], tier='A'))
    seen = {m['branch'] for m in tier_a}
    for j in range(n):
        pcj = base + j * 4
        bc = bcond(words[j], pcj)
        if not bc or bc[0] not in (0xC, 0xA, 0x8, 0x2) or not 0x40 <= bc[1] - pcj <= 0x20000 or pcj in seen:
            continue
        cmpi = None
        for q in range(j - 1, max(j - 4, -1), -1):   # the flags come from a cmp w, #imm within 3 instructions
            c = cmp_w_imm(words[q])
            if c:
                cmpi = c
                break
            if (words[q] & 0x1F) == 0x1F and (words[q] >> 29) & 1:   # another flag setter (subs/adds/ands with Rd = zr)
                break
        if not cmpi or not 1 <= cmpi[1] <= 64:
            continue
        lo = max(0, j - 14)
        win = range(lo, j)
        loads = {q: ldrb(words[q]) for q in win}
        stores = {q: strb(words[q]) for q in win}
        found = None
        for q1 in win:
            s1 = stores[q1]
            if not s1 or s1[2] == 0:
                continue
            for q2 in win:
                s2 = stores[q2]
                if q2 <= q1 or not s2 or s2[1] != s1[1] or s2[2] != s1[2] + 1:
                    continue
                l1 = next((q for q in range(q1 - 1, max(lo, q1 - 4) - 1, -1) if loads[q] and loads[q][0] == s1[0]), None)
                l2 = next((q for q in range(q2 - 1, max(lo, q2 - 4) - 1, -1) if loads[q] and loads[q][0] == s2[0]), None)
                if l1 is None or l2 is None or l2 <= l1:
                    continue
                if loads[l1][1:] != (0, 0) or loads[l2][1:] != (0, 0):
                    continue
                if not any(is_bl(words[q]) for q in range(l1 + 1, l2)):
                    continue
                found = (l1, l2, s1[2])
        if found:
            tier_b.append(dict(fxaa=base + found[0] * 4, fsr=base + found[1] * 4, branch=pcj, target=bc[1], cond=bc[0],
                               k=found[2], tier='B'))
    return tier_a, tier_b


def pick_trio(tier_a, tier_b):
    """(match or None, why): one tier-A match; or no tier-A match and one tier-B match."""
    if len(tier_a) == 1:
        return tier_a[0], 'one exact match (tier A)' + (f'; {len(tier_b)} looser look-alike(s) ignored' if tier_b else '')
    if len(tier_a) > 1:
        return None, f'{len(tier_a)} exact matches (tier A): ambiguous'
    if len(tier_b) == 1:
        return tier_b[0], 'no exact match; one relaxed match (tier B)'
    if tier_b:
        return None, f'no exact match and {len(tier_b)} relaxed matches (tier B): ambiguous'
    return None, 'no match: the FXAA / FSR flag copies and the cmp #8 / b.gt after them were not found'


# ---------------------------------------------------------------- Increased LOD
def lod_site(build_id):
    return LOD_SITES.get(build_id.lower()[:16], (None, None))[0]


def check_lod(build_id, word_at, in_fn):
    """theboy181's 1.4.3 (and 1.4.2) site. word_at(addr) -> word or None; in_fn(a, b) -> a and b in one lifted
    function. Returns (ok, why)."""
    LOD_SITE, ver = LOD_SITES.get(build_id.lower()[:16], (None, None))
    if LOD_SITE is None:
        return False, (f'main build {build_id[:16]} is not 1.4.3 ({LOD_BUILD[:16]}) or 1.4.2: the published address '
                       'does not apply')
    w = word_at(LOD_SITE)
    if w is None:
        return False, f'{LOD_SITE:#x} is outside main'
    if ends_flow(w) and not is_bl(w):
        return False, f'{LOD_SITE:#x} holds {w:#010x} (a return / indirect branch / trap): not the published site'
    if not in_fn(LOD_SITE, LOD_SITE + LOD_SKIP):
        return False, f'{LOD_SITE:#x} and {LOD_SITE + LOD_SKIP:#x} are not in one function: not the published site'
    t = any_branch_target(w, LOD_SITE)
    note = f' (the original already branches to {t:#x})' if t is not None else ''
    return True, f'build {build_id[:16]} = {ver}; {LOD_SITE:#x} holds {w:#010x}{note}; skip target in the same function'


# ---------------------------------------------------------------- register effects (for the shadow search)
_XW = re.compile(r'c->x\[(\d+)\]\s*=(?!=)')
_XR = re.compile(r'c->x\[(\d+)\]')
_FW = re.compile(r'\b(?:subs|adds|logic|adcs|sbcs)\d*\(c\b|c->nzcv\s*=(?!=)')
_FR = re.compile(r'\bCOND_|\b(?:adc|sbc|adcs|sbcs)\d*\(c\b')
FLAGS = 64
CALLER_SAVED = set(range(0, 19)) | {30} | set(range(32, 40)) | set(range(48, 64)) | {FLAGS}
ARGS = set(range(0, 8)) | set(range(32, 40))
RETS = {0, 1} | set(range(32, 36)) | set(range(19, 30)) | set(range(40, 48))


def vreg_effects(w):
    """(reads, writes, exact) of the SIMD & FP registers (32 + n) for the usual classes; exact False = a guess."""
    if (w >> 26) & 1 and not (w >> 25) & 1 and (w >> 27) & 1:   # load / store, V = 1
        rt, cls = w & 31, (w >> 27) & 7
        if cls == 0b011:                                       # ldr (literal)
            return set(), {32 + rt}, True
        if cls == 0b101:                                       # ldp / stp
            regs = {32 + rt, 32 + ((w >> 10) & 31)}
            return (set(), regs, True) if (w >> 22) & 1 else (regs, set(), True)
        if cls == 0b111:                                       # ldr / str
            return (set(), {32 + rt}, True) if (w >> 22) & 1 else ({32 + rt}, set(), True)
        regs = {32 + ((rt + k) & 31) for k in range(4)}        # structures (ld1..ld4 / st1..st4)
        return (regs if not (w >> 22) & 1 else set(), regs if (w >> 22) & 1 else set(), False)
    if (w >> 25) & 7 != 7:
        return set(), set(), True
    rd, rn, rm, ra = 32 + (w & 31), 32 + ((w >> 5) & 31), 32 + ((w >> 16) & 31), 32 + ((w >> 10) & 31)
    top = (w >> 24) & 0xFF
    if top in (0x1E, 0x9E):                                    # scalar FP (32 / 64-bit general registers)
        if (w >> 21) & 1:
            if (w >> 10) & 0x3F == 0:                          # FP <-> integer conversions
                op = (w >> 16) & 7
                if op in (2, 3, 7):
                    return set(), {rd}, True                   # scvtf / ucvtf / fmov from w/x
                return {rn}, set(), True                       # fcvt* / fmov to w/x
            if (w >> 10) & 0xF == 0b1000:                      # fcmp
                return {rn, rm}, set(), True
            if (w >> 10) & 3 == 1:                             # fccmp
                return {rn, rm}, set(), True
            if (w >> 10) & 3 == 3:                             # fcsel
                return {rn, rm}, {rd}, True
            if (w >> 10) & 0x1F == 0b10000:                    # one source
                return {rn}, {rd}, True
            if (w >> 10) & 7 == 0b100:                         # fmov #imm
                return set(), {rd}, True
            if (w >> 10) & 3 == 2:                             # two sources
                return {rn, rm}, {rd}, True
        else:                                                  # fixed-point conversions
            op = (w >> 16) & 7
            return (set(), {rd}, True) if op in (2, 3) else ({rn}, set(), True)
        return {rn, rm, rd}, {rd}, False
    if top in (0x1F, 0x9F):                                    # fmadd & co
        return {rn, rm, ra}, {rd}, True
    return {rn, rm, rd}, {rd}, False                           # Advanced SIMD (vector): a guess


def effects(ins, w):
    """(reads, writes, exact, stores, kind) of one lifted instruction (gen.Module.lift) and its word."""
    code = (ins.code or '') + ' ' + (ins.cond or '' if isinstance(ins.cond, str) else '')
    tgt = ins.target if isinstance(ins.target, str) else ''
    writes = {int(r) for r in _XW.findall(code)}
    stripped = _XW.sub('', code)
    reads = {int(r) for r in _XR.findall(stripped + ' ' + tgt)}
    if _FW.search(code):
        writes.add(FLAGS)
    if _FR.search(code):
        reads.add(FLAGS)
    vr, vw, exact = vreg_effects(w)
    reads |= vr
    writes |= vw
    stores = bool(re.search(r'\bST\d+\(', code))
    if 'rt_' in code:
        return reads, writes, exact, stores, 'special'
    return reads, writes, exact, stores, ins.kind


# ---------------------------------------------------------------- the shadow map size
def live_at(lift, word, start, fn_lo, fn_hi, want, budget=200):
    """Which registers of `want` code from `start` reads before writing (all paths within the function, up to `budget`
    instructions): (read, assumed). `assumed`: registers still pending where the walk had to stop (an indirect
    branch, leaving the function, the budget) - treated as live by the caller."""
    read, assumed = set(), set()
    work = [(start, frozenset(want))]
    seen = set()
    steps = 0
    while work:
        a, pend = work.pop()
        while pend:
            if (a, pend) in seen:
                break
            seen.add((a, pend))
            steps += 1
            if steps > budget or not fn_lo <= a < fn_hi:
                assumed |= pend
                break
            ins, w = lift(a), word(a)
            r, wr, exact, _, kind = effects(ins, w)
            if kind == 'ret':
                read |= pend & RETS
                break
            if kind == 'trap':
                break
            if kind in ('bl', 'blr'):
                read |= pend & (ARGS | r)
                pend = pend - CALLER_SAVED
                a += 4
                continue
            if kind in ('br', 'special'):
                read |= pend & r
                assumed |= pend
                break
            read |= pend & r
            if exact:
                pend = pend - wr
            if kind == 'b':
                t = ins.target
                if not fn_lo <= t < fn_hi:   # a tail call
                    read |= pend & (ARGS | RETS)
                    break
                a = t
                continue
            if kind == 'bcond':
                work.append((ins.target, pend))
            a += 4
    return read, assumed


def shadow_candidates(words, base, lift, fn_of, labels_of=None, limit=None):
    """Blocks [S, S + 0x60) the public shadow patch could have skipped: straight-line code (no call, no indirect
    branch, branches only within the block) that writes w11, and after which (S + 0x60) w11 is read before it is
    written while every other register the block writes is dead. fn_of(a) -> (lo, hi) of the lifted function or None;
    labels_of(lo) -> that function's branch targets. Each candidate: dict(s, e, score, strict, cues, stores)."""
    n = len(words)
    nw = SHADOW_SKIP // 4
    # prefix counts (one pass; the window tests below are then O(1)): flow enders, Rd = 11, reads of register 11
    bad, rd11, rd = [0] * (n + 1), [0] * (n + 1), [0] * (n + 1)
    b_ = d_ = r_ = 0
    for i, w in enumerate(words):
        if ends_flow(w):
            b_ += 1
        if (w & 31) == 11:
            d_ += 1
        if ((w >> 5) & 31) == 11 or ((w >> 16) & 31) == 11 or ((w >> 10) & 31) == 11 or (w & 31) == 11:
            r_ += 1
        bad[i + 1], rd11[i + 1], rd[i + 1] = b_, d_, r_
    word = lambda a: words[(a - base) // 4]
    out = []
    for i in range(0, n - nw - 1):
        if bad[i + nw] != bad[i] or rd11[i + nw] == rd11[i] or rd[min(n, i + nw + 12)] == rd[i + nw]:
            continue   # a call / return / trap inside, no instruction with Rd = 11, or nothing near S + 0x60 names x11
        s, e = base + i * 4, base + (i + nw) * 4
        blk = words[i:i + nw]
        ok = True
        for q, w in enumerate(blk):
            t = any_branch_target(w, s + q * 4)
            if t is not None and not s < t <= e:
                ok = False
                break
        if not ok:
            continue
        fn = fn_of(s)
        if not fn or fn != fn_of(e):
            continue
        lo, hi = fn
        writes, stores, cues = set(), False, []
        for q in range(nw):
            a = s + q * 4
            r, wr, exact, st, kind = effects(lift(a), words[i + q])
            if kind in ('bl', 'blr', 'br', 'ret', 'trap', 'special'):
                ok = False
                break
            writes |= wr
            stores |= st
        if not ok or 11 not in writes:
            continue
        read, assumed = live_at(lift, word, e, lo, hi, writes | {11})
        if 11 not in read or (writes - {11}) & (read | assumed):
            continue
        score = 0
        pw = [v for w in blk for v in [movz_imm(w)] if v and v[1] in SHADOW_SIZES]
        if pw:
            score += 3
            cues.append('constant ' + '/'.join(str(v[1]) for v in pw))
        if any((w >> 25) & 7 == 7 for w in blk):
            score += 1
            cues.append('float maths')
        if not stores:
            score += 2
            cues.append('no stores')
        labels = labels_of(lo) if labels_of else set()
        prev = words[i - 1] if i else 0
        s_start = s == lo or s in labels or ends_flow(prev) or any_branch_target(prev, s - 4) is not None
        e_start = e in labels or any_branch_target(words[i + nw - 1], e - 4) is not None
        if s_start:
            score += 2
            cues.append('starts a block')
        if e_start:
            score += 2
            cues.append('rejoin is a branch target')
        strict = s_start and not stores
        out.append(dict(s=s, e=e, score=score, strict=strict, cues=cues, stores=stores))
        if limit and len(out) >= limit:
            break
    out.sort(key=lambda c: (-c['strict'], -c['score'], c['s']))
    return out


def read_confirm(path, build_id):
    """diag/lift-confirm.txt (src/host/uit_lift.c): 'shadow <S> build <id16> ...' lines for this build -> {S}"""
    got = set()
    try:
        for line in open(path, encoding='utf-8', errors='replace'):
            p = line.split()
            if len(p) >= 4 and p[0] == 'shadow' and p[2] == 'build' and build_id.lower().startswith(p[3].lower()):
                try:
                    got.add(int(p[1], 16))
                except ValueError:
                    pass
    except OSError:
        pass
    return got


# ---------------------------------------------------------------- the lift's side (gen.py)
STATE = {}   # what the scan decided (patches.table_flags reads it)


def _window(m, a, before=4, after=8):
    return ' '.join(f'{m.word(x):08x}' if m.lo <= x < m.hi else '--------' for x in range(a - before * 4, a + after * 4, 4))


def run(m, entries, order, root, outdir, patches):
    """Search main, add the patches (patches.PATCHES / OPTIONAL / extra labels), write lift-scan.txt and
    lift-features.ini next to the generated code. m: gen.Module; entries: {start: (end, labels, tables)}."""
    def fn_of(a):
        i = bisect.bisect_right(order, a) - 1
        if i < 0:
            return None
        lo = order[i]
        hi = entries[lo][0]
        return (lo, hi) if lo <= a < hi else None

    def in_fn(a, b):
        f = fn_of(a)
        return bool(f) and f == fn_of(b) and m.in_text(b)

    def labels_of(lo):
        return entries[lo][1]

    def add_label(a):
        f = fn_of(a)
        entries[f[0]][1].add(a)

    build = m.nso.build_id
    words = m.words
    rep = [f'# Ultra Instinct Tears: the lift\'s search for community patch sites (tools/lifter/scan.py, UIT 0070)',
           f'# main build {build}', '']
    feat = {}
    taken = set(patches.PATCHES)

    # -- the trio
    ta, tb = find_trio(words, m.lo)
    pick, why = pick_trio(ta, tb)
    if pick and not (in_fn(pick['branch'], pick['target']) and in_fn(pick['fxaa'], pick['branch'])):
        why += f'; rejected: {pick["branch"]:#x} and its target {pick["target"]:#x} are not in one lifted function'
        pick = None
    if pick and ({pick['branch'], pick['fxaa']} & taken):
        why += '; rejected: the site is already patched by another feature'
        pick = None
    rep.append(f'[quality + fxaa] {why}')
    for t in ta + tb:
        rep.append(f'  tier {t["tier"]}: flag loads {t["fxaa"]:#x} / {t["fsr"]:#x}, stores at +{t["k"]:#x}/+{t["k"] + 1:#x}, '
                   f'b.{COND_NAMES.get(t["cond"], t["cond"])} {t["branch"]:#x} -> {t["target"]:#x}')
        rep.append(f'    words from {t["fxaa"] - 16:#x}: {_window(m, t["fxaa"], 4, 12)}')
    STATE['quality'] = pick
    if pick:
        b, f = pick['branch'], pick['fxaa']
        patches.PATCHES[b] = lambda gen, a: _quality_patch(gen, a)
        patches.PATCHES[f] = lambda gen, a: _fxaa_patch(gen, a)
        patches.OPTIONAL.update({b, f})
        feat['quality'] = (1, f'b.{COND_NAMES.get(pick["cond"])} at {b:#x} ({why})')
        feat['fxaa'] = (1, f'FXAA flag load at {f:#x}')
        print(f'SCAN: quality reduction branch {b:#x}, FXAA flag {f:#x} ({why})', file=sys.stderr, flush=True)
    else:
        feat['quality'] = (0, why)
        feat['fxaa'] = (0, why + '; the GPU-side FXAA skip still works')
        print(f'WARNING: optional patch quality / fxaa: {why}: Remove quality reduction stays unavailable', file=sys.stderr,
              flush=True)
    rep.append('')

    # -- Increased LOD
    LOD_SITE = lod_site(build) or globals()['LOD_SITE']
    ok, why = check_lod(build, lambda a: m.word(a) if m.in_text(a) else None, in_fn)
    if ok and LOD_SITE in taken:
        ok, why = False, why + '; rejected: the site is already patched by another feature'
    rep.append(f'[lod] {"found" if ok else "not applied"}: {why}')
    if m.in_text(LOD_SITE):
        rep.append(f'    words from {LOD_SITE - 16:#x}: {_window(m, LOD_SITE, 4, 40)}')
    STATE['lod'] = LOD_SITE if ok else None
    if ok:
        add_label(LOD_SITE + LOD_SKIP)
        patches.PATCHES[LOD_SITE] = lambda gen, a: _lod_patch(gen, a)
        patches.OPTIONAL.add(LOD_SITE)
        feat['lod'] = (1, f'{LOD_SITE:#x} -> {LOD_SITE + LOD_SKIP:#x}')
        print(f'SCAN: Increased LOD site {LOD_SITE:#x} ({why})', file=sys.stderr, flush=True)
    else:
        feat['lod'] = (0, why)
        print(f'WARNING: optional patch lod: {why}: Improved LOD stays unavailable', file=sys.stderr, flush=True)
    rep.append('')

    # -- shadow size
    cands = shadow_candidates(words, m.lo, m.lift, fn_of, labels_of)
    confirm = read_confirm(os.path.join(root, 'diag', 'lift-confirm.txt'), build)
    strict = [c for c in cands if c['strict']]
    site = next((c for c in cands if c['s'] in confirm), None)
    if site and ({site['s'], site['e']} & taken):
        site = None
    if site:
        why = f'confirmed in game (diag/lift-confirm.txt): {site["s"]:#x}'
    elif confirm:
        why = ('diag/lift-confirm.txt names ' + ', '.join(f'{a:#x}' for a in sorted(confirm)) +
               ', which is not among this lift\'s candidates (other code?): not used')
    else:
        why = (f'{len(strict)} strict and {len(cands) - len(strict)} looser candidate(s); none confirmed in game yet '
               '(play about a minute outdoors, close the game, run tools/setup.py again)')
    rep.append(f'[shadow] {why}')
    probes = []
    if not site:
        for c in cands:
            if len(probes) >= PROBE_MAX:
                break
            if c['e'] in taken or any(p['e'] == c['e'] for p in probes):
                continue
            probes.append(c)
    for k, c in enumerate(cands[:200]):
        pk = next((j for j, p in enumerate(probes) if p is c), None)
        rep.append(f'  {"strict" if c["strict"] else "loose "} score {c["score"]:2d} S {c["s"]:#x} rejoin {c["e"]:#x}'
                   f'{f"  probe {pk}" if pk is not None else ""}  ({", ".join(c["cues"]) or "-"})')
        if k < 40:
            rep.append(f'    words from {c["s"] - 8:#x}: {_window(m, c["s"], 2, 26)}')
    STATE['shadow'] = site
    STATE['probes'] = probes
    if site:
        add_label(site['e'])
        patches.PATCHES[site['s']] = lambda gen, a: _shadow_patch(gen, a, site['e'])
        patches.OPTIONAL.add(site['s'])
        feat['shadow'] = (1, f'{site["s"]:#x} (confirmed in game)')
        print(f'SCAN: shadow size site {site["s"]:#x} ({why})', file=sys.stderr, flush=True)
    else:
        for k, c in enumerate(probes):
            patches.PATCHES[c['e']] = (lambda kk: lambda gen, a: _probe_patch(gen, a, kk))(k)
            patches.OPTIONAL.add(c['e'])
        feat['shadow'] = (0, why)
        print(f'WARNING: optional patch shadow: {why}; {len(probes)} run-time probe(s) added', file=sys.stderr, flush=True)
    rep.append('')
    rep.append('[probes] ' + (' '.join(f'{k}={c["s"]:#x}' for k, c in enumerate(probes)) or 'none'))

    # (the instruction words stay in a file of their own: tools/report.py adds lift-scan.txt, which has none)
    with open(os.path.join(outdir, 'lift-scan.txt'), 'w', encoding='utf-8') as f:
        f.write('\n'.join(l for l in rep if not l.startswith('    words from')) + '\n')
    with open(os.path.join(outdir, 'lift-scan-words.txt'), 'w', encoding='utf-8') as f:
        f.write('\n'.join(rep) + '\n')
    STATE['feat'], STATE['gen_dir'], STATE['build'] = feat, outdir, build
    write_features()   # (again from patches.table_flags once the code is written: a site never emitted reads 0)
    return feat


def write_features(applied=None):
    """data/generated/lift-features.ini for the launcher: name=1 when the feature's code is in this build."""
    feat = dict(STATE.get('feat', {}))
    if not STATE.get('gen_dir'):
        return
    if applied is not None:
        for k, v in applied.items():
            if feat.get(k, (0,))[0] and not v:
                feat[k] = (0, 'found, but the site was in no lifted function (not emitted)')
    lines = ['# written by tools/lifter/scan.py when the code is lifted: 1 = this build has the feature\'s code',
             f'build={STATE.get("build", "")}']
    for k in ('quality', 'fxaa', 'lod', 'shadow'):
        v, note = feat.get(k, (0, 'not searched'))
        lines += [f'{k}={v}', f'{k}_note={note}']
    for k, (v, note) in STATE.get('extra', {}).items():   # [UIT 0175] patches.table_flags: the cheats
        lines += [f'{k}={v}', f'{k}_note={note}']
    lines.append(f'shadow_probes={len(STATE.get("probes", ()))}')
    with open(os.path.join(STATE['gen_dir'], 'lift-features.ini'), 'w', encoding='utf-8') as f:
        f.write('\n'.join(lines) + '\n')


def table_c(applied):
    """the main_*_patch flags and the probe table for main_table.c (patches.table_flags)"""
    q = STATE.get('quality')
    sh = STATE.get('shadow')
    flags = dict(quality=int(bool(q and q['branch'] in applied)), fxaa=int(bool(q and q['fxaa'] in applied)),
                 lod=int(bool(STATE.get('lod') and STATE['lod'] in applied)), shadow=int(bool(sh and sh['s'] in applied)))
    write_features(flags)
    # (the count is the table's length: probe k indexes it even when another probe's site was not emitted)
    probes = STATE.get('probes', ()) if any(c['e'] in applied for c in STATE.get('probes', ())) else ()
    sites = ', '.join(f'{c["s"]:#x}ull' for c in STATE.get('probes', ())) or '0'
    return (f'const int main_quality_patch = {flags["quality"]};\n'
            f'const int main_fxaa_flag_patch = {flags["fxaa"]};\n'
            f'const int main_lod_patch = {flags["lod"]};\n'
            f'const int main_shadow_patch = {flags["shadow"]};\n'
            f'const int main_shadow_probes = {len(probes)};\n'
            f'const unsigned long long main_shadow_probe_sites[] = {{ {sites} }};\n'
            f'const char main_lift_build[] = "{STATE.get("build", "")[:16]}";\n')


# ---------------------------------------------------------------- the C the patches emit
def _render(gen, a):
    ins = gen.lift(a)
    return ' '.join(gen.render_single(ins, a, gen._jump, gen._refs_now))


def _quality_patch(gen, a):
    """b.gt T: also taken when TOTK_QUALITY_REDUCTION=off (the public patch made it b T)"""
    import patches
    patches.APPLIED.add(a)
    ins = gen.lift(a)
    return (f'{{ extern int totk_quality_reduction_off(void); if (totk_quality_reduction_off() || ({ins.cond})) '
            f'{{ {gen._jump(ins.target)} }} }}')


def _fxaa_patch(gen, a):
    """ldrb wA, [x0]: 0 when TOTK_GAME_FXAA turns the game's FXAA off (the public patch: mov wA, #0)"""
    import patches
    patches.APPLIED.add(a)
    rt = gen.word(a) & 31
    return (f'{{ extern int totk_game_fxaa_off(void); if (totk_game_fxaa_off()) c->x[{rt}] = 0; else {{ {_render(gen, a)} }} }}')


def _lod_patch(gen, a):
    import patches
    patches.APPLIED.add(a)
    return (f'{{ extern int totk_lod_improve(void); if (totk_lod_improve()) {{ {gen._jump(a + LOD_SKIP)} }} }} '
            + _render(gen, a))


def _shadow_patch(gen, a, e):
    import patches
    patches.APPLIED.add(a)
    return (f'{{ extern uint32_t totk_shadow_size(void); uint32_t sz_ = totk_shadow_size(); '
            f'if (sz_) {{ c->x[11] = sz_; {gen._jump(e)} }} }} ' + _render(gen, a))


def _probe_patch(gen, a, k):
    import patches
    patches.APPLIED.add(a)
    return (f'{{ extern volatile int totk_lift_probe; extern void totk_shadow_probe(int, uint32_t); '
            f'if (__builtin_expect(totk_lift_probe, 0)) totk_shadow_probe({k}, (uint32_t)c->x[11]); }} ' + _render(gen, a))
