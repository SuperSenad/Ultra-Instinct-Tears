"""[UIT 0078] Register cache for the lifted C: the hottest guest registers and the flags live in C locals.

The plain lifter reads and writes every guest register through the Cpu struct (c->x[19], c->nzcv). Guest stores go
through untyped pointers that may alias that struct, so the C compiler must store and reload every register around
every guest memory access: a loop that walks an array spends most of its instructions moving registers in and out of
memory. Here, per function, up to NREGS of the most used general registers (x0..x30, sp) and the flags (when they pay
for themselves) are copied into locals at entry (rc_x19, rc_sp, rc_nz), used there, and written back to the Cpu struct
before anything that may read or change them (a call to another guest function or to the dispatcher, an SVC, BRK/UDF,
an indirect branch, a return, a patch's host call, inline asm), and read back after it.

Safety:
  * A line that calls anything not defined in runtime/cpu.h, simd.h, simd_fp.h or regcache.h (pure inline helpers:
    checked below, none of them touches c->x / c->sp, and only cpu.h's flag helpers touch c->nzcv) is a barrier: the
    locals are written back before it, the line runs on the Cpu struct exactly as before, and they are read back after
    it (and before any goto out of it).
  * Every function's new C is checked against the old: undoing the edits must give the old text back character for
    character; if not, that function keeps the old C (STATS['verify_failed']).
  * TOTK_LIFT_REGCACHE=0 (default, launcher: Performance > CPU+ > Faster game code translation: Off) keeps the old
    code generation exactly; it is only used with --simd c (the asm path reads registers from memory itself).
  * [UIT 0084] TOTK_LIFT_REGCACHE_FLAGS=1 (sub-setting, default Off): conditions from the compared values; see
    flags_pass at the end of this file.
  * [UIT 0085] TOTK_LIFT_FPCR_HOST=1 (sub-setting, default Off): MSR FPCR also sets the host rounding mode and the FP
    helpers drop their per-op FPCR.RMode check (runtime/simd.h SIMD_FPCR_HOST).
Register state is only in the locals between barriers: host code that looks at a running thread's registers from the
outside (src/host/watch.c debugging, a crash report after a host fault in game code) sees them as of the last barrier.
"""
import os
import pathlib
import re
from collections import Counter

NREGS = 6
# [UIT 0192] TOTK_LIFT_REGCACHE_CLEAN=0: write every cached register back at each call, as 0078 did
CLEAN = os.environ.get('TOTK_LIFT_REGCACHE_CLEAN', '1') != '0'
STATS = Counter()
ROOT = pathlib.Path(os.environ.get('TOTK_LIFT_ROOT') or pathlib.Path(__file__).resolve().parents[2])
RUNTIME = ROOT / 'src' / 'runtime'
HEADERS = ('cpu.h', 'simd.h', 'simd_fp.h', 'regcache.h')

FLAGFN = ['adds64', 'adds32', 'subs64', 'subs32', 'logic64', 'logic32', 'adc64', 'adc32', 'sbc64', 'sbc32',
          'adcs64', 'adcs32', 'sbcs64', 'sbcs32']
CONDS = ['EQ', 'NE', 'CS', 'CC', 'MI', 'PL', 'VS', 'VC', 'HI', 'LS', 'GE', 'LT', 'GT', 'LE']
KEYWORDS = {'if', 'else', 'switch', 'case', 'while', 'for', 'do', 'return', 'sizeof', 'goto', '__attribute__',
            'musttail', 'default', 'break', '_Alignof', '__typeof__', 'typeof'}
# host functions known not to look at or change the calling thread's guest registers
HOST_SAFE = {'rt_spin_gate'}


def _safe_names():
    names = set()
    for h in HEADERS:
        p = RUNTIME / h
        if not p.is_file():
            continue
        t = p.read_text(errors='replace')
        names |= set(re.findall(r'^\s*#\s*define\s+(\w+)\(', t, re.M))
        names |= set(re.findall(r'^(?:static\s+inline|SI|SN|static)\b[^;={(]*?\b(\w+)\s*\(', t, re.M))
        for fn in re.findall(r'\bFN\((\w+)\)\s*\(', t):
            names |= {fn + '_s', fn + '_d'}
        for mac, pfx in (('EXCL', ('ldx', 'stx', 'lda', 'stl')), ('ADC_IMPL', ('adc', 'sbc', 'adcs', 'sbcs'))):
            for n in re.findall(r'^' + mac + r'\((\d+)', t, re.M):
                names |= {p_ + n for p_ in pfx}
    return names


def _headers_ok():
    """None of the inline helpers may touch the cached state except cpu.h's flag helpers (rewritten) and FN_ENTER."""
    bad = []
    for h in HEADERS[:3]:
        p = RUNTIME / h
        if not p.is_file():
            return False
        for i, l in enumerate(p.read_text(errors='replace').splitlines()):
            if re.search(r'->\s*(x\s*\[|sp\b|nzcv\b)', l):
                ok = h == 'cpu.h' and (l.startswith('#define FN_ENTER') or l.startswith('#define FLAG_')
                                        or re.search(r'c->nzcv = MKFLAGS', l) or l.lstrip().startswith('uint64_t nzcv')
                                        or l.lstrip().startswith('//'))
                if not ok:
                    bad.append(f'{h}:{i + 1}')
    if bad:
        print('regcache: off: these runtime header lines touch guest registers: ' + ', '.join(bad))
    return not bad


ON = os.environ.get('TOTK_LIFT_REGCACHE', '0') == '1'
if ON:
    ON = _headers_ok()
# [UIT 0084] Performance > CPU+ > Faster game code translation > Flags from the compared values (needs the above)
FLAGS = ON and os.environ.get('TOTK_LIFT_REGCACHE_FLAGS', '0') == '1'
# [UIT 0085] Performance > CPU+ > Faster game code translation > FP rounding in the PC's FP unit (needs the above):
# gen.py defines SIMD_FPCR_HOST in every chunk (runtime/simd.h; tested by tools/tests/test_fpcr_host.c)
FPCR_HOST = ON and os.environ.get('TOTK_LIFT_FPCR_HOST', '0') == '1'
SAFE = _safe_names() | HOST_SAFE if ON else set()

R_CALL = re.compile(r'\b([A-Za-z_]\w*)\s*\(')
R_X = re.compile(r'\bc->x\[(\d+)\]')
R_SP = re.compile(r'\bc->sp\b')
R_FLAGS = re.compile(r'\bc->nzcv\b|\b(?:COND_[A-Z]{2}|FLAG_[NZCV])\(c\)|\b(?:' + '|'.join(FLAGFN) + r')\(c, ')
R_LABEL = re.compile(r'\s*L_\w+:;?\s*')
R_GOTO = re.compile(r'goto (L_\w+);')
R_RET_END = re.compile(r'(?:^|[;{}]\s*)(?:__attribute__\(\(musttail\)\)\s*)?return\b[^;{}]*;\s*$')
R_SIMPLE_IF = re.compile(r'(\s*)if \((.*)\) \{ ([^{}]*) \}')


def _barrier(text):
    if '__asm__' in text or re.search(r'\breturn\b', text):
        return True
    return any(n not in SAFE and n not in KEYWORDS for n in R_CALL.findall(text))


def _writes(v, t):
    """[UIT 0192] True if the C text t can change the local v: an assignment (= += <<= ...), ++ / --, or its address
    taken. Function-like macros in the runtime headers never assign to a register argument (only to flags locals and
    through store pointers), and the inline helpers get values, not addresses."""
    return bool(re.search(r'\b' + v + r'\b\s*(?:=(?!=)|[-+*/%&|^]=|<<=|>>=|\+\+|--)', t)
                or re.search(r'(?:\+\+|--)\s*' + v + r'\b|[(,=]\s*&\s*' + v + r'\b', t))


R_SPILL = re.compile(r'(?:(?:c->x\[(\d+)\] = rc_x\1|c->sp = rc_sp|c->nzcv = rc_nz); )+')


def _dirty(body, kinds, subst, vs):
    """[UIT 0192] For each line, the cached registers (of vs) that may differ from the Cpu struct when the line starts:
    a forward may-analysis over the function's lines. Entry: none (they were just read). A plain line adds the ones it
    assigns (_writes). A barrier line writes back, runs and reads them all again on every way out (its gotos are
    wrapped in the reload), so after it: none; for a simple if, the not-taken way keeps what came in (plus what the
    condition assigns). Every goto is an edge and every line falls through unless it is a plain goto or ends in a
    return, so the sets can only be too large (a write-back kept), never too small. None: an unknown label (all
    written back, as 0078)."""
    n = len(body)
    labels = {}
    for i, (l, k) in enumerate(zip(body, kinds)):
        if k == 'label':
            labels[l.strip().rstrip(';').rstrip(':')] = i
    succ, gen, kill = [], [], []
    for i, (l, k) in enumerate(zip(body, kinds)):
        tg = set()
        for t in R_GOTO.findall(l):
            if t not in labels:
                return None
            tg.add(labels[t])
        st = l.strip()
        falls = not (re.fullmatch(r'goto L_\w+;', st) or (R_RET_END.search(l) and not st.startswith(('if', 'switch'))))
        if falls and i + 1 < n:
            tg.add(i + 1)
        succ.append(tg)
        text = subst(l) if k == 'plain' else subst(R_SIMPLE_IF.fullmatch(l).group(2)) if k == 'if' else ''
        gen.append({v for v in vs if _writes(v, text)})
        kill.append(k == 'unit')
    din = [set() for _ in range(n)]
    seen = [False] * n
    work = [0] if n else []
    seen[0:1] = [True] if n else []
    while work:
        i = work.pop()
        out = set() if kill[i] else din[i] | gen[i]
        for j in succ[i]:
            if not seen[j] or not out <= din[j]:
                seen[j] = True
                din[j] |= out
                work.append(j)
    return [set(vs) if not seen[i] else din[i] | gen[i] if kinds[i] == 'if' else din[i] for i in range(n)]   # (unreached: all)


def _balanced(s):
    d = 0
    for ch in s:
        d += ch == '('
        d -= ch == ')'
        if d < 0:
            return False
    return d == 0


def apply(lines):
    """lines: one function from gen.Module.emit_function. Returns the lines with the register cache (or as they were)."""
    if not ON:
        return lines
    try:
        i0 = next(i for i, l in enumerate(lines) if l.startswith('    FN_ENTER('))
    except StopIteration:
        return lines   # (an HLE stub)
    try:
        i1 = next(i for i in range(i0 + 1, len(lines)) if lines[i] == '}')
    except StopIteration:
        return lines
    body = lines[i0 + 1:i1]
    if any(re.search(r'\brc_\w|\bRCOND_|\bRCF_', l) for l in body):
        STATS['skipped_names'] += 1
        return lines
    STATS['functions'] += 1
    # loop weight: lines inside a backward goto's range count 4x
    labels = {}
    for i, l in enumerate(body):
        m = R_LABEL.fullmatch(l)
        if m:
            labels[l.strip().rstrip(';').rstrip(':')] = i
    w = [1] * len(body)
    for i, l in enumerate(body):
        for t in R_GOTO.findall(l):
            j = labels.get(t)
            if j is not None and j <= i:
                for k in range(j, i + 1):
                    w[k] = 4
    kinds = []
    for l in body:
        if R_LABEL.fullmatch(l):
            kinds.append('label')
        elif _barrier(l):
            m = R_SIMPLE_IF.fullmatch(l)
            kinds.append('if' if m and _balanced(m.group(2)) and not _barrier(m.group(2)) else 'unit')
        else:
            kinds.append('plain')
    uses, fl, bar = Counter(), 0, 0
    for l, k, wt in zip(body, kinds, w):
        if k == 'unit' or k == 'if':
            bar += wt
        text = l if k == 'plain' else (R_SIMPLE_IF.fullmatch(l).group(2) if k == 'if' else '')
        for r in R_X.findall(text):
            uses[int(r)] += wt
        uses['sp'] += wt * len(R_SP.findall(text))
        fl += wt * len(R_FLAGS.findall(text))
    need = 2 * bar + 3
    regs = [r for r, n in sorted(uses.items(), key=lambda kv: -kv[1]) if n >= need][:NREGS]
    flags = fl >= need
    if not regs and not flags:
        STATS['not_worth'] += 1
        return lines

    def loc(r):
        return 'rc_sp' if r == 'sp' else f'rc_x{r}'

    def mem(r):
        return 'c->sp' if r == 'sp' else f'c->x[{r}]'
    cached = [(loc(r), mem(r)) for r in sorted(regs, key=lambda r: (r == 'sp', r if r != 'sp' else 0))]
    if flags:
        cached.append(('rc_nz', 'c->nzcv'))
    xs = {r for r in regs if r != 'sp'}
    def subst(t):
        t = R_X.sub(lambda m: f'rc_x{m.group(1)}' if int(m.group(1)) in xs else m.group(0), t)
        if 'sp' in regs:
            t = R_SP.sub('rc_sp', t)
        if flags:
            t = re.sub(r'\bCOND_([A-Z]{2})\(c\)', r'RCOND_\1(rc_nz)', t)
            t = re.sub(r'\bFLAG_([NZCV])\(c\)', r'RCF_\1(rc_nz)', t)
            t = re.sub(r'\b(' + '|'.join(FLAGFN) + r')\(c, ', r'rc_\1(rc_nz, ', t)
            t = R_FLAGS.sub(lambda m: 'rc_nz' if m.group(0) == 'c->nzcv' else m.group(0), t)
        return t

    reload = ' '.join(f'{v} = {m};' for v, m in cached)
    allv = [v for v, m in cached]
    dirty = _dirty(body, kinds, subst, [v for v in allv if v != 'rc_nz']) if CLEAN else None
    if dirty is not None and os.environ.get('TOTK_LIFT_REGCACHE_MUTANT') == '1':   # test only: no write-backs (must fail)
        dirty = [set() for _ in body]

    def spill_at(i):
        """[UIT 0192] the write-back before line i: every cached register that some path since the function entry or
        the last reload can have changed (the flags always; without the analysis, all)"""
        vs = allv if dirty is None else [v for v in allv if v == 'rc_nz' or v in dirty[i]]
        STATS['spill_kept'] += len(vs)
        STATS['spill_dropped'] += len(allv) - len(vs)
        return ''.join(f'{m} = {v}; ' for v, m in cached if v in vs)

    def wrap_gotos(t):
        return R_GOTO.sub(lambda m: '{ ' + reload + ' goto ' + m.group(1) + '; }', t)
    new = []
    for i, (l, k) in enumerate(zip(body, kinds)):
        if k == 'label':
            new.append(l)
        elif k == 'plain':
            new.append(subst(l))
        elif k == 'if':
            ind, cond, b = R_SIMPLE_IF.fullmatch(l).groups()
            tail = '' if R_RET_END.search(b) or re.search(r'\bgoto L_\w+;\s*$', b) else ' ' + reload
            new.append(f'{ind}if ({subst(cond)}) {{ {spill_at(i)}{wrap_gotos(b)}{tail} }}')
        else:
            ind = l[:len(l) - len(l.lstrip())]
            s = l[len(ind):]
            tail = '' if R_RET_END.search(s) else ' ' + reload
            new.append(ind + spill_at(i) + wrap_gotos(s) + tail)
    decl = '    uint64_t ' + ', '.join(f'{v} = {m}' for v, m in cached) + ';'
    out = lines[:i0 + 1] + [decl] + new + lines[i1:]
    if undo(out[i0 + 1:i0 + 2 + len(new)], cached) != body:
        STATS['verify_failed'] += 1
        return lines
    if flags and FLAGS:   # [UIT 0084]
        fnew = flags_pass(new, kinds)
        if fnew is not None:
            if flags_undo(fnew) != new:
                STATS['flags_verify_failed'] += 1
            else:
                out = lines[:i0 + 1] + [decl] + fnew + lines[i1:]
    STATS['cached'] += 1
    STATS['regs'] += len(regs)
    STATS['flags'] += flags
    return out


def undo(new, cached):
    """The inverse of apply's edits (decl line first): the old body back, for the check. [UIT 0192] a write-back is any
    run of `c->x[N] = rc_xN; ` / `c->sp = rc_sp; ` / `c->nzcv = rc_nz; ` (the old body has no rc_ names, and in the
    rewritten plain lines a cached register never appears as c->x[N], so nothing else can match)."""
    reload = ' '.join(f'{v} = {m};' for v, m in cached)
    out = []
    for l in new[1:]:
        l = re.sub(r'\{ ' + re.escape(reload) + r' goto (L_\w+); \}', r'goto \1;', l)
        l = R_SPILL.sub('', l).replace(' ' + reload, '')
        l = re.sub(r'\brc_x(\d+)\b', r'c->x[\1]', l)
        l = re.sub(r'\brc_sp\b', 'c->sp', l)
        l = re.sub(r'\bRCOND_([A-Z]{2})\(rc_nz\)', r'COND_\1(c)', l)
        l = re.sub(r'\bRCF_([NZCV])\(rc_nz\)', r'FLAG_\1(c)', l)
        l = re.sub(r'\brc_(' + '|'.join(FLAGFN) + r')\(rc_nz, ', r'\1(c, ', l)
        l = re.sub(r'\brc_nz\b', 'c->nzcv', l)
        out.append(l)
    return out


# ---------------------------------------------------------------- [UIT 0084] flags from the operands
# TOTK_LIFT_REGCACHE_FLAGS=1 (needs TOTK_LIFT_REGCACHE=1; launcher: Performance > CPU+ > Faster game code translation >
# Flags from the compared values). Runs on a function the register cache has just rewritten with its flags in rc_nz.
# A reaching-definitions pass over the function's lines finds, for every place the flags are read, which flag-setting
# lines can have set them. Where all of them are the same kind (subs64, subs32, adds64, adds32, logic64, logic32: the
# condition is a function of two operands, or of the result for a logical op), the condition is computed from the
# operands kept in rc_fa / rc_fb (regcache.h RCOS_* / RCOA_* / RCOL_*), so `cmp x0, x1; b.lt` becomes one signed
# compare instead of packing NZCV and testing N != V. Everything else still reads rc_nz, and every flag-setting line
# that can reach such a read (or a write-back that mixes kinds) also keeps writing rc_nz (rcob_*). A write-back that only
# one kind can reach packs the flags there, from the operands (RCPK_*), so a loop's subs / b.ne never packs them.
# The control-flow graph over-approximates (every goto is an edge, every line falls through unless it is a plain goto
# or ends in a return, a barrier's reload counts as a new definition, a conditional definition does not hide the older
# ones), which can only make more reads take the packed form. The barriers stay exactly as in the register cache.
# Checked like the rest: flags_undo() must give the register cache's text back exactly, or the function keeps it
# (STATS['flags_verify_failed']).
R_FTOK = re.compile(r'\brc_(adds|subs|logic|adcs|sbcs)(32|64)\(rc_nz, |\bRCOND_([A-Z]{2})\(rc_nz\)|\brc_nz\s*=(?!=)|\brc_nz\b')
FKINDS = ('subs64', 'subs32', 'adds64', 'adds32', 'logic64', 'logic32')
SPILL_NZ = 'c->nzcv = rc_nz;'
FDECL = '    uint64_t rc_fa = 0, rc_fb = 0;'


def _events(text):
    """flag events of a line, in order: ('read', cond, span) / ('use', span) / ('def', kind, span) / ('nz', span)"""
    ev = []
    for m in R_FTOK.finditer(text):
        if m.group(1):
            k = m.group(1) + m.group(2)
            if k in FKINDS:
                ev.append(('def', k, m.span()))
            else:   # adcs / sbcs: read the carry, then set all four
                ev.append(('use', m.span()))
                ev.append(('nz', m.span()))
        elif m.group(3):
            ev.append(('read', m.group(3), m.span()))
        elif m.group(0).rstrip().endswith('='):
            ev.append(('nz', m.span()))
        else:
            ev.append(('use', m.span()))
    return ev


def _if_cond(l):
    """start, end of the condition of a register-cached `if (cond) { ... }` barrier line"""
    a = l.index('if (') + 4
    d = 1
    for j in range(a, len(l)):
        d += (l[j] == '(') - (l[j] == ')')
        if d == 0:
            return a, j
    raise ValueError(l)


def flags_pass(new, kinds):
    n = len(new)
    labels = {}
    for i, (l, k) in enumerate(zip(new, kinds)):
        if k == 'label':
            labels[l.strip().rstrip(';').rstrip(':')] = i
    succ, ev, cond_text = [], [], []
    for i, (l, k) in enumerate(zip(new, kinds)):
        tg = set()
        for t in R_GOTO.findall(l):
            if t not in labels:
                STATS['flags_unknown_label'] += 1
                return None
            tg.add(labels[t])
        s = l.strip()
        falls = not (re.fullmatch(r'goto L_\w+;', s) or (R_RET_END.search(l) and not s.startswith(('if', 'switch'))))
        if falls and i + 1 < n:
            tg.add(i + 1)
        succ.append(tg)
        if k == 'plain':
            if re.search(r'\brc_nz\s*[-+*/%&|^<>]=', l):
                STATS['flags_compound'] += 1
                return None
            e = _events(l)
        elif k == 'if':
            a, b = _if_cond(l)
            e = _events(l[a:b])
            if any(x[0] != 'read' for x in e):
                return None
        else:
            e = []
        seen_def = False
        for x in e:
            if x[0] in ('def', 'nz'):
                seen_def = True
            elif seen_def:   # a read after a definition on the same line
                STATS['flags_order'] += 1
                return None
        ev.append(e)
    ENTRY = (-1, 0, 'nz')

    def transfer(i, inn):
        k = kinds[i]
        if k == 'unit':
            return frozenset([(i, 0, 'nz')])
        if k == 'if':
            return inn | {(i, 0, 'nz')}
        defs = [(i, j, x[1] if x[0] == 'def' else 'nz') for j, x in enumerate(ev[i]) if x[0] in ('def', 'nz')]
        if not defs:
            return inn
        l = new[i].lstrip()
        if not l.startswith('if'):
            return frozenset(defs[-1:]) if len(defs) == 1 or all(d[2] == 'nz' for d in defs) else None
        sp = [x[2][0] if x[0] == 'def' else x[1][0] for x in ev[i] if x[0] in ('def', 'nz')]
        e = new[i].find(' else ')
        if e > 0 and any(p < e for p in sp) and any(p > e for p in sp):
            return frozenset(defs)
        return inn | set(defs)
    inn = [frozenset() for _ in range(n)]
    out = [None] * n
    if n:
        inn[0] = frozenset([ENTRY])
    work = list(range(n))
    while work:
        i = work.pop()
        o = transfer(i, inn[i])
        if o is None:
            STATS['flags_multi_def'] += 1
            return None
        if o != out[i]:
            out[i] = o
            for j in succ[i]:
                u = inn[j] | o
                if u != inn[j]:
                    inn[j] = u
                    work.append(j)
    ops, packed = set(), set()
    plan = []   # (line, span, replacement)

    def one_kind(S):
        ks = {d[2] for d in S}
        return next(iter(ks)) if len(ks) == 1 and 'nz' not in ks else None
    for i in range(n):
        S = inn[i]
        if kinds[i] in ('unit', 'if') and SPILL_NZ in new[i]:
            k = one_kind(S)
            if k:
                ops.update(S)
                a = '(rc_fa)' if k.startswith('logic') else '(rc_fa, rc_fb)'
                plan.append((i, None, f'c->nzcv = RCPK_{k[:-2].upper()}{k[-2:]}{a};'))
            else:
                packed.update(S)
        for x in ev[i]:
            if x[0] == 'read':
                k = one_kind(S)
                if k:
                    ops.update(S)
                    pre = {'subs': 'RCOS', 'adds': 'RCOA', 'logic': 'RCOL'}[k[:-2]]
                    a = '(rc_fa)' if k.startswith('logic') else '(rc_fa, rc_fb)'
                    plan.append((i, x[2], f'{pre}_{x[1]}{k[-2:]}{a}'))
                elif S:
                    packed.update(S)
            elif x[0] == 'use':
                packed.update(S)
    if not ops:
        STATS['flags_no_gain'] += 1
        return None
    for i in range(n):
        for j, x in enumerate(ev[i]):
            if x[0] == 'def' and (i, j, x[1]) in ops:
                k = x[1]
                fa = 'rc_fa, ' if k.startswith('logic') else 'rc_fa, rc_fb, '
                if (i, j, k) in packed:
                    plan.append((i, x[2], f'rcob_{k}(rc_nz, {fa}'))
                else:
                    plan.append((i, x[2], f'rco_{k}({fa}'))
    res = list(new)
    for i in sorted({p[0] for p in plan}):
        l = res[i]
        reps = sorted([p for p in plan if p[0] == i and p[1] is not None], key=lambda p: -p[1][0])
        off = _if_cond(l)[0] if kinds[i] == 'if' else 0   # (an if line's spans are in its condition)
        for _, (a, b), t in reps:
            l = l[:a + off] + t + l[b + off:]
        for p in plan:
            if p[0] == i and p[1] is None:
                l = l.replace(SPILL_NZ, p[2], 1)
        res[i] = l
    STATS['flags_operands'] += 1
    STATS['flags_reads_direct'] += sum(1 for p in plan if p[2].startswith('RCO') and not p[2].startswith('RCOB'))
    STATS['flags_pack_at_spill'] += sum(1 for p in plan if p[1] is None)
    return [FDECL] + res


def flags_undo(fnew):
    """The inverse of flags_pass: the register cache's lines back."""
    if not fnew or fnew[0] != FDECL:
        return None
    out = []
    for l in fnew[1:]:
        l = re.sub(r'\bc->nzcv = RCPK_[A-Z]+(?:32|64)\((?:rc_fa, rc_fb|rc_fa)\);', SPILL_NZ, l)
        l = re.sub(r'\bRCO[SA]_([A-Z]{2})(?:32|64)\(rc_fa, rc_fb\)|\bRCOL_([A-Z]{2})(?:32|64)\(rc_fa\)',
                   lambda m: f'RCOND_{m.group(1) or m.group(2)}(rc_nz)', l)
        l = re.sub(r'\brcob_(\w+?)(32|64)\(rc_nz, rc_fa, (?:rc_fb, )?', r'rc_\1\2(rc_nz, ', l)
        l = re.sub(r'\brco_(\w+?)(32|64)\(rc_fa, (?:rc_fb, )?', r'rc_\1\2(rc_nz, ', l)
        out.append(l)
    return out
