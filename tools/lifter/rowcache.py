"""[UIT 0356] Row lookups by name, answered from a cache (TOTK_ROWCACHE, Advanced > Compatibility switches).

The idea is from WotE (MIT): the game looks up rows of its actor tables by name (PouchActorInfoTable::getRow, reached
from hundreds of call sites, SensorMgr::updateTargetActor among them) with a binary search through virtual calls, and
the same names come back every frame. WotE measured 5-7 % of the game's CPU time in towns on 1.0.0. That analysis
was of 1.0.0 only; this file finds the 1.4.3 functions itself, in the user's own main at lift time, from their shape:

  search  a function that calls through the table's virtual functions at +0x60, +0x70, +0x78 and +0x80 (count, index,
          key, row; all four shifted by the same d when the class has more virtual functions in front), stores only
          to its own stack, and calls nothing else but such functions;
  getRow  a function with exactly one bl, to a search function, with the table (x0 as it came in) and a query built on
          its own stack (type 8: a movz #8); it reads the name pointer from its second argument at one fixed offset
          and nothing else from it, reads x2..x7 never, stores only to its stack, and calls through a stack
          object's virtual function at +0x18 at most twice (the query's clean-up, which never runs for a name).

Nothing comes from a game file and no address is hard-coded: on another build the shapes simply do not match and
nothing changes. Two steps, like the shadow-size probe (scan.py):

 1. probe  with TOTK_ROWCACHE on, each getRow candidate gets a front that notes the table's vtable while the game
           runs (src/host/rowcache.c); a candidate seen working on one table writes "rowcache <G> vtable <V> build
           <id16> samples <n>" to diag/lift-confirm.txt. Nothing is cached yet.
 2. cache  the next setup run reads those lines. For each confirmed (G, V): the vtable's words (relocations) give its
           virtual functions; the four the search calls must be pure (stores to the stack only, no calls but pure
           ones); every other slot that is not pure (stores elsewhere or calls anything) - the table's mutators,
           +0x28..+0x58 in WotE's 1.0.0 analysis, and its destructors - gets a front that moves a generation counter
           before and after it runs. getRow gets the cache front: a per-thread table keyed by (G, table, name
           bytes), an entry valid only for the generation it was made in, and stored only when no mutator ran or was
           running meanwhile. TOTK_ROWCACHE_VERIFY=1 runs the real lookup on every hit too and logs any difference.

Fail closed throughout: no candidate, more than MAX_PAIRS confirmed pairs, an impure search slot, a mutator with an
alias entry: that pair (or everything) stays as it was, and data/generated/lift-scan.txt says why ([rowcache])."""
import bisect
import os
import re
import sys

SEARCH = (0x60, 0x70, 0x78, 0x80)   # count, index, key, row (WotE's 1.0.0 layout; d shifts all four)
SHIFTS = range(-0x20, 0x48, 8)
MAX_G = 64          # probe fronts at most
MAX_PAIRS = 8       # confirmed (getRow, vtable) pairs at most; more = something is off, nothing is cached
MAX_V = 4           # vtables per getRow
GETROW_MAX = 0x300  # bytes
SEARCH_MAX = 0x800
PURE_MAX = 0x400
VT_SLOTS = 96

STATE = {'stage': 0, 'g': [], 'v': {}, 'off': {}, 'fronts': {}, 'report': [], 'emitted': set()}
STATS = {'getrow': 0, 'mutators': 0, 'skipped': 0}

_EA = re.compile(r'uint64_t ea = c->(sp|x\[(\d+)\])(?:\s*([+-])\s*(0x[0-9a-fA-F]+|\d+)(?:ull|u)?)?;')
_LD = re.compile(r'\bLD(8|16|32|64|128)\(')
_ST = re.compile(r'\bST(8|16|32|64|128)\(')
_IMPURE = re.compile(r'\bstx\d*\(|\bstl\d*\(|memset|memcpy|memmove|__atomic|\brt_|\bcas\w*\(|\bswp\w*\(|\bldadd|'
                     r'\bldclr|\bldeor|\bldset|\bldsm|\bldum')
_XW = re.compile(r'c->x\[(\d+)\]\s*=(?!=)')
_XR = re.compile(r'c->x\[(\d+)\]')
_SP_ADD = re.compile(r'^c->x\[(\d+)\] = \(uint64_t\)\(c->sp ([+-]) (0x[0-9a-fA-F]+)ull\);$')
_X_ADD = re.compile(r'^c->x\[(\d+)\] = \(uint64_t\)\(c->x\[(\d+)\] ([+-]) (0x[0-9a-fA-F]+)ull\);$')
_MOV = re.compile(r'^c->x\[(\d+)\] = \(uint64_t\)\(0ull \| c->x\[(\d+)\]\);$')
_SP_SET = re.compile(r'^c->sp = \(uint64_t\)\(c->sp ([+-]) (0x[0-9a-fA-F]+)ull\);$')
_SP_WB = re.compile(r'c->sp = ea(?: [+-] (?:0x[0-9a-fA-F]+|\d+))?;')
_MOVZ8 = re.compile(r'^c->x\[\d+\] = \(uint(?:32|64)_t\)\((?:0u \| )?0x8ull\);$')


def ea_of(code):
    """(base, offset) of a load / store's address: base 'sp' or a register number; None: not an immediate form."""
    mm = _EA.search(code)
    if not mm:
        return None
    base = 'sp' if mm.group(1) == 'sp' else int(mm.group(2))
    off = int(mm.group(4), 0) if mm.group(4) else 0
    return base, (-off if mm.group(3) == '-' else off)


class Fn:
    """One function's instructions, lifted: (address, kind, code, target, asm registers) - from a gen.Module."""
    def __init__(self, m, lo, hi):
        self.lo, self.hi, self.ins = lo, hi, []
        for a in range(lo, hi, 4):
            i = m.lift(a)
            code = i.code if isinstance(i.code, str) else ''
            regs = tuple(r for _, r in i.asm.gprs) if i.kind == 'asm' and i.asm is not None else ()
            self.ins.append((a, i.kind, code, i.target, regs))


class Reject(Exception):
    pass


TAINT = ('U', 'X1')   # classes that survive a merge: an argument register never written, the name object


def _succ(f, a, kind, tgt):
    if kind in ('ret', 'trap', 'br'):
        return []
    if kind == 'b':
        return [tgt] if isinstance(tgt, int) and f.lo <= tgt < f.hi else None   # None: leaves the function
    out = [a + 4] if a + 4 < f.hi else []
    if kind == 'bcond':
        if not (isinstance(tgt, int) and f.lo <= tgt < f.hi):
            return None
        out.append(tgt)
    return out


def _meet(old, new):
    out = {}
    for k in set(old) | set(new):
        x, y = old.get(k), new.get(k)
        if x == y:
            out[k] = x
        elif x in TAINT or y in TAINT:
            out[k] = x if x in TAINT else y
    return out


def flow(f, init, step):
    """Forward data flow over f: state = {register: class}, missing = other; a merge keeps equal classes and the
    taints. step(a, kind, code, target, asm registers, state) -> state, or raises Reject. {address: state before}."""
    before = {f.lo: dict(init)}
    work = [f.lo]
    n = 0
    while work:
        a = work.pop()
        n += 1
        if n > 50000:
            raise Reject('flow did not settle')
        _, kind, code, tgt, asm = f.ins[(a - f.lo) // 4]
        out = step(a, kind, code, tgt, asm, dict(before[a]))
        nxt = _succ(f, a, kind, tgt)
        if nxt is None:
            raise Reject('branches out of the function')
        for s in nxt:
            old = before.get(s)
            merged = dict(out) if old is None else _meet(old, out)
            if merged != old:
                before[s] = merged
                work.append(s)
    return before


CALL_CLOBBER = set(range(0, 19)) | {30}


def _stack(st, r):
    return r == 'sp' or st.get(r) == 'S'


def _alu(code, st):
    """the registers a plain instruction writes and their class: copies and stack-pointer arithmetic keep track"""
    c = code.strip()
    mm = _SP_ADD.match(c)
    if mm:
        st[int(mm.group(1))] = 'S'
        return
    mm = _X_ADD.match(c)
    if mm:
        st[int(mm.group(1))] = 'S' if st.get(int(mm.group(2))) == 'S' else 'O'
        return
    mm = _MOV.match(c)
    if mm:
        st[int(mm.group(1))] = st.get(int(mm.group(2)), 'O')
        return
    for r in _XW.findall(c):
        st[int(r)] = 'O'
    if 'c->sp = ' in c and not _SP_SET.match(c):
        ea = ea_of(c)   # pre- / post-index on sp only
        if not (ea and ea[0] == 'sp' and _SP_WB.search(c)):
            raise Reject('moves sp other than by a constant')


def _common(kind, code, asm, st):
    """what no checked function may do: unknown instructions, atomics, runtime calls, memory not on its stack"""
    if kind in ('trap', 'br'):
        raise Reject('a trap or an indirect jump')
    if _IMPURE.search(code):
        raise Reject('an atomic, exclusive, cache or runtime operation')
    if kind == 'asm' and not all(_stack(st, r) for r in asm):
        raise Reject('a SIMD instruction with a general register other than the stack')


def _store_ok(code, st):
    if _ST.search(code):
        ea = ea_of(code)
        if not ea or not _stack(st, ea[0]):
            raise Reject('stores outside its stack')


def _calls(st):
    for r in CALL_CLOBBER:
        st.pop(r, None)
    return st


def pure(m, entries, a, depth=2, memo=None, seen=None):
    """True if the function at a stores only to its own stack, has no indirect calls and calls (bl or a tail b) only
    pure functions (depth calls deep)."""
    memo = STATE.setdefault('_pure', {}) if memo is None else memo
    if (a, depth) in memo:
        return memo[(a, depth)]
    seen = set() if seen is None else seen
    if a in seen or depth < 0 or a not in entries:
        return False
    seen = seen | {a}
    end = entries[a][0]
    ok = end - a <= PURE_MAX
    if ok:
        calls = set()

        def step(x, kind, code, tgt, asm, st):
            _common(kind, code, asm, st)
            if kind == 'blr':
                raise Reject('an indirect call')
            _store_ok(code, st)
            if kind == 'bl':
                calls.add(tgt)
                return _calls(st)
            _alu(code, st)
            return st
        try:
            f = Fn(m, a, end)
            outs = {t for (_, k, _, t, _) in f.ins if k in ('b', 'bcond') and not (isinstance(t, int) and a <= t < end)}
            if outs:
                if any(k == 'bcond' and not (isinstance(t, int) and a <= t < end) for (_, k, _, t, _) in f.ins) \
                        or not all(isinstance(t, int) for t in outs):
                    raise Reject('a conditional branch out')
                calls |= outs
                f = _Clip(f)
            flow(f, {}, step)
            ok = all(isinstance(t, int) and pure(m, entries, t, depth - 1, memo, seen) for t in calls)
        except Reject:
            ok = False
    memo[(a, depth)] = ok
    return ok


class _Clip:
    """f with its branches out of the function (tail calls) turned into returns"""
    def __init__(self, f):
        self.lo, self.hi = f.lo, f.hi
        self.ins = [(a, 'ret' if k == 'b' and not (isinstance(t, int) and f.lo <= t < f.hi) else k, c, t, s)
                    for (a, k, c, t, s) in f.ins]


def _blr_reg(tgt):
    mm = re.match(r'c->x\[(\d+)\]$', str(tgt))
    return int(mm.group(1)) if mm else None


def search_fn(m, entries, a):
    """None or the shift d if the function at a is search-shaped (see the header)."""
    end = entries[a][0]
    if end - a > SEARCH_MAX:
        return None
    used, calls = set(), set()

    def step(x, kind, code, tgt, asm, st):
        _common(kind, code, asm, st)
        _store_ok(code, st)
        if kind == 'blr':
            r = _blr_reg(tgt)
            cls = st.get(r, '') if r is not None else ''
            if not cls.startswith('VS'):
                raise Reject("a call not through the table's vtable")
            used.add(int(cls[2:]))
            return _calls(st)
        if kind == 'bl':
            calls.add(tgt)
            return _calls(st)
        ld = _LD.search(code)
        if ld and not _ST.search(code):
            ea = ea_of(code)
            dst = [int(r) for r in _XW.findall(code)]
            cls = 'O'
            if ea and ld.group(1) == '64' and len(dst) == 1 and ea[0] != 'sp':
                b = st.get(ea[0])
                if b == 'T' and ea[1] == 0:
                    cls = 'VP'
                elif b == 'VP' and ea[1] % 8 == 0:
                    cls = f'VS{ea[1]}'
            for r in dst:
                st[r] = cls
            return st
        _alu(code, st)
        return st
    try:
        flow(Fn(m, a, end), {0: 'T'}, step)
    except Reject:
        return None
    for d in SHIFTS:
        if used == {o + d for o in SEARCH}:
            return d if all(isinstance(t, int) and pure(m, entries, t) for t in calls) else None
    return None


def getrow_fn(m, entries, a, searches, hle=(), aliases=()):
    """(None, why) or ({'s': search address, 'off': name offset in x1's object}, '') if a is getRow-shaped."""
    end = entries[a][0]
    if end - a > GETROW_MAX:
        return None, 'too long'
    if a in hle or a in aliases:
        return None, 'an HLE stub or with alias entries'
    bls, blrs, offs, movz8 = set(), set(), set(), []

    def step(x, kind, code, tgt, asm, st):
        _common(kind, code, asm, st)
        if _MOVZ8.match(code.strip()):
            movz8.append(x)
        if kind == 'blr':
            r = _blr_reg(tgt)
            if r is None or st.get(r) != 'M18':
                raise Reject("a call through something other than a stack object's +0x18")
            if any(st.get(k) in TAINT for k in range(0, 8)):
                raise Reject('passes the name object or an unset argument register on')
            blrs.add(x)
            return _calls(st)
        if kind == 'bl':
            if st.get(0) != 'X0' or not _stack(st, 1):
                raise Reject('the call is not (table, query on the stack)')
            if any(st.get(k) == 'X1' for k in range(0, 8)):
                raise Reject('passes the name object on')
            bls.add((x, tgt))
            return _calls(st)
        if kind == 'bcond':   # a test of a register reads nothing from memory
            if any(st.get(int(r)) == 'U' for r in _XR.findall(code)):
                raise Reject('reads x2..x7 (more than two arguments?)')
            return st
        if kind in ('ret', 'b'):
            return st
        rhs = _XW.sub('', code)
        reads = {int(r) for r in _XR.findall(rhs)}
        if any(st.get(r) == 'U' for r in reads):
            raise Reject('reads x2..x7 (more than two arguments?)')
        if kind == 'asm':
            return st
        ld, sto = _LD.search(code), _ST.search(code)
        if ld or sto:
            ea = ea_of(code)
            if not ea:
                raise Reject('an address that is not register + constant')
            b = 'S' if ea[0] == 'sp' else st.get(ea[0], 'O')
            if sto:
                if b != 'S':
                    raise Reject('stores outside its stack')
                data = reads - ({ea[0]} if ea[0] != 'sp' else set())
                if any(st.get(r) == 'X1' for r in data):
                    raise Reject('stores the name object')
                if ld:
                    raise Reject('loads and stores in one')
            else:
                dst = [int(r) for r in _XW.findall(code)]
                if ea[0] != 'sp' and re.search(r'c->x\[%d\] = ea\b' % ea[0], code):
                    raise Reject('a load with write-back')
                if b == 'X1':
                    if ld.group(1) != '64' or len(dst) != 1:
                        raise Reject('reads more than the name pointer from the name object')
                    offs.add(ea[1])
                    cls = 'N'
                elif b in ('S', 'M', 'M18'):
                    cls = 'M18' if b == 'M' and ea[1] == 0x18 and ld.group(1) == '64' else 'M'
                else:
                    raise Reject('reads memory other than its stack and the name object')
                for r in dst:
                    st[r] = cls
            if 'c->sp = ' in code and not (ea[0] == 'sp' and _SP_WB.search(code)):
                raise Reject('sp from another register')
            return st
        if any(st.get(r) == 'X1' for r in reads) and not _MOV.match(code.strip()):
            raise Reject('uses the name object other than to read the name pointer')
        _alu(code, st)
        return st
    init = {0: 'X0', 1: 'X1', **{r: 'U' for r in range(2, 8)}}
    try:
        flow(Fn(m, a, end), init, step)
    except Reject as e:
        return None, str(e)
    if len(bls) != 1 or next(iter(bls))[1] not in searches:
        return None, 'not exactly one call, to a search function'
    if len(offs) != 1:
        return None, 'the name pointer not read at exactly one offset'
    if len(blrs) > 2:
        return None, 'more than two clean-up calls'
    if not movz8:
        return None, 'no type-8 query (movz #8)'
    return {'s': next(iter(bls))[1], 'off': offs.pop()}, ''


def read_confirm(path, build_id):
    """diag/lift-confirm.txt: 'rowcache <G> vtable <V> build <id16> ...' lines of this build -> {G: {V}}"""
    got = {}
    try:
        for line in open(path, encoding='utf-8', errors='replace'):
            p = line.split()
            if len(p) >= 6 and p[0] == 'rowcache' and p[2] == 'vtable' and p[4] == 'build' and build_id and \
                    build_id.lower().startswith(p[5].lower()):
                try:
                    got.setdefault(int(p[1], 16), set()).add(int(p[3], 16))
                except ValueError:
                    pass
    except OSError:
        pass
    return got


def vtable_slots(nso, base, v, entries):
    """The vtable at guest address v: ([function per slot], clean). It ends at the first word with no relocation
    (the next object's offset-to-top or RTTI header); clean is False if a relocated word before that is not a
    lifted function's entry (an import, data): a slot no front could cover."""
    rel = STATE.get('_rel')
    if rel is None:
        rel = STATE['_rel'] = {}
        for off, typ, sym, add in nso.relocs:
            rel[off] = (typ, add)
    out = []
    for k in range(VT_SLOTS):
        r = rel.get(v - base + 8 * k)
        if r is None:
            return out, True
        if r[0] != 1027 or base + r[1] not in entries:
            return out, False
        out.append(base + r[1])
    return out, False


def _words_of(m, a, n=24):
    return ' '.join(f'{m.word(x):08x}' for x in range(a, min(a + 4 * n, m.hi), 4))


def find(m, entries, order, root, gen_dir, base, hle=(), aliases=()):
    """The fronts for this lift: {address: ('probe' | 'getrow' | 'mutator', index)}; also STATE for table_c()."""
    STATE.update(stage=0, g=[], v={}, off={}, fronts={}, report=[], words=[], emitted=set(), _pure={}, _rel=None,
                 img=(base, base + getattr(m.nso, 'size', 0)))
    rep = STATE['report']
    on = (os.environ.get('TOTK_ROWCACHE') or '').strip().lower() not in ('', '0', 'off', 'no', 'false', '-')
    if not on:
        rep.append('[rowcache] TOTK_ROWCACHE is off: nothing searched, no fronts')
        return {}
    build = (getattr(m.nso, 'build_id', b'') or b'')
    build = build.hex() if isinstance(build, (bytes, bytearray)) else str(build)
    words, lo = m.words, m.lo
    fn_of = lambda a: order[bisect.bisect_right(order, a) - 1] if order and a >= order[0] else None
    # 1. functions with 64-bit loads at all four search offsets (any shift), then the full check
    want = {}
    for d in SHIFTS:
        for o in SEARCH:
            want.setdefault((o + d) // 8, set()).add(d)
    hits = {}
    for i, w in enumerate(words):
        if (w & 0xFFC00000) == 0xF9400000:
            k = (w >> 10) & 0xFFF
            if k in want:
                f = fn_of(lo + 4 * i)
                if f is not None:
                    hits.setdefault(f, set()).add(k * 8)
    pre = [f for f, offs in hits.items() if any({o + d for o in SEARCH} <= offs for d in SHIFTS)]
    searches = {}
    for f in pre:
        if f in entries and f not in hle:
            d = search_fn(m, entries, f)
            if d is not None:
                searches[f] = d
    rep.append(f'[rowcache] search-shaped functions: {len(searches)} of {len(pre)} with the four loads')
    for s, d in sorted(searches.items())[:20]:
        rep.append(f'    search {s:#x} shift {d:+#x}')
    # 2. callers of a search function, then the getRow check
    callers = set()
    for i, w in enumerate(words):
        if (w >> 26) == 0x25:
            t = lo + 4 * i + (((w & 0x3FFFFFF) ^ 0x2000000) - 0x2000000) * 4
            if t in searches:
                f = fn_of(lo + 4 * i)
                if f is not None:
                    callers.add(f)
    gs, near = [], {}
    for f in sorted(callers):
        g, why = getrow_fn(m, entries, f, searches, hle, aliases)
        if g:
            gs.append((f, g))
        else:
            near[why] = near.get(why, 0) + 1
    rep.append(f'[rowcache] getRow-shaped functions: {len(gs)} of {len(callers)} callers of a search function')
    for why, n in sorted(near.items(), key=lambda x: -x[1])[:12]:
        rep.append(f'    not taken ({n}): {why}')
    if len(gs) > MAX_G:
        rep.append(f'[rowcache] more than {MAX_G} candidates: nothing done (the shape is not specific enough)')
        return {}
    confirm = read_confirm(os.path.join(root, 'diag', 'lift-confirm.txt'), build)
    fronts = {}
    if not confirm:
        STATE['stage'] = 1
        for k, (f, g) in enumerate(gs):
            STATE['g'].append(f)
            STATE['off'][f] = g['off']
            fronts[f] = ('probe', k)
            rep.append(f'    probe {k}: getRow candidate {f:#x} (search {g["s"]:#x}, name at +{g["off"]:#x})')
            STATE['words'].append(f'getrow {f:#x}: {_words_of(m, f)}')
        rep.append('[rowcache] probing: play a minute with Advanced > Compatibility switches > Row lookup cache on, close the '
                   'game, then About > Install update > Install anyway; the cache is used from the build after that')
        STATE['fronts'] = fronts
        return fronts
    # 3. confirmed pairs: vtable checks, mutators
    gmap = dict(gs)
    pairs, mut, bad = [], set(), []
    for f, vs in sorted(confirm.items()):
        if f not in gmap:
            bad.append(f'{f:#x}: not a getRow candidate of this lift')
            continue
        d = searches[gmap[f]['s']]
        for v in sorted(vs):
            slots, clean = vtable_slots(m.nso, base, v, entries)
            sl = {o + d for o in SEARCH}
            if not clean:
                bad.append(f'{f:#x} vtable {v:#x}: a slot that is not a lifted function, or no end found')
                continue
            if len(slots) * 8 <= max(sl):
                bad.append(f'{f:#x} vtable {v:#x}: {len(slots)} slots, too few')
                continue
            if not all(pure(m, entries, slots[o // 8]) for o in sl):
                bad.append(f'{f:#x} vtable {v:#x}: a search slot is not pure')
                continue
            ms = {t for k, t in enumerate(slots) if 8 * k not in sl and not pure(m, entries, t)}
            if any(t in hle or t in aliases for t in ms):
                bad.append(f'{f:#x} vtable {v:#x}: a mutator is an HLE stub or has alias entries')
                continue
            if any(t in gmap for t in ms):
                bad.append(f'{f:#x} vtable {v:#x}: a mutator is a getRow candidate')
                continue
            pairs.append((f, v, slots, ms))
    for b in bad:
        rep.append(f'    not used: {b}')
    if not pairs or len(pairs) > MAX_PAIRS or any(len([p for p in pairs if p[0] == f]) > MAX_V for f, *_ in pairs):
        rep.append(f'[rowcache] {len(pairs)} usable confirmed pair(s): nothing cached'
                   + (f' (more than {MAX_PAIRS})' if len(pairs) > MAX_PAIRS else ''))
        return {}
    STATE['stage'] = 2
    for f, v, slots, ms in pairs:
        if f not in STATE['v']:
            STATE['g'].append(f)
            STATE['off'][f] = gmap[f]['off']
            STATE['v'][f] = []
        STATE['v'][f].append(v)
        mut |= ms
        rep.append(f'    cache: getRow {f:#x} table vtable {v:#x} ({len(slots)} slots; search shift {searches[gmap[f]["s"]]:+#x}); '
                   f'generation bumped by {len(ms)} slot function(s): ' + ', '.join(f'{t:#x}' for t in sorted(ms)))
    for k, f in enumerate(STATE['g']):
        fronts[f] = ('getrow', k)
    for t in sorted(mut):
        fronts[t] = ('mutator', 0)
    STATE['fronts'] = fronts
    rep.append(f'[rowcache] cache stage: {len(STATE["g"])} getRow function(s), {len(mut)} mutator(s)')
    return fronts


def append_report(gen_dir):
    """the [rowcache] section of data/generated/lift-scan.txt (addresses only); instruction words to lift-scan-words.txt"""
    for name, lines in (('lift-scan.txt', STATE['report']), ('lift-scan-words.txt', STATE.get('words', []))):
        if not lines:
            continue
        try:
            with open(os.path.join(gen_dir, name), 'a', encoding='utf-8') as f:
                f.write('\n'.join(lines) + '\n')
        except OSError:
            pass


def front(lines, fn, kind, k):
    """The lifted function renamed <fn>_lifted and the front under the original name (as native_libc.front)."""
    head = f'void {fn}(Cpu *c) {{'
    if not lines or lines[0] != head:
        STATS['skipped'] += 1
        return lines, False
    body = [f'void {fn}_lifted(Cpu *c);', f'void {fn}_lifted(Cpu *c) {{'] + lines[1:]
    if kind == 'mutator':
        STATS['mutators'] += 1
        return body + [f'void {fn}(Cpu *c) {{   /* [UIT 0356] may change a row table: the row cache\'s generation moves */',
                       '    extern void uit_rc_mutate(Cpu *, void (*)(Cpu *));',
                       f'    uit_rc_mutate(c, {fn}_lifted);', '}'], True
    STATS['getrow'] += 1
    call = (f'uit_rc_getrow(c, {k}, {fn}_lifted); return;' if kind == 'getrow' else f'uit_rc_probe(c, {k});')
    return body + [f'void {fn}(Cpu *c) {{   /* [UIT 0356] row lookup by name: TOTK_ROWCACHE ({kind}) */',
                   '    extern int uit_rc_on; extern void uit_rc_getrow(Cpu *, int, void (*)(Cpu *)); '
                   'extern void uit_rc_probe(Cpu *, int);',
                   f'    if (__builtin_expect(uit_rc_on != 0, {1 if kind == "getrow" else 0})) {{ {call} }}',
                   f'    __attribute__((musttail)) return {fn}_lifted(c);', '}'], True


def table_c():
    """main_table.c: what the lift put in (src/host/rowcache.c reads it); lo..hi: the module's image (its vtables)"""
    st = STATE
    lo, hi = st.get('img', (0, 0))
    g = [a for a in st['g']]
    stage = st['stage']
    emitted = st.get('emitted', set())
    if stage and not all(a in emitted for a in g):
        stage = 0   # a front was not emitted: nothing used
    if stage == 2 and not all(a in emitted for a, (kd, _) in st['fronts'].items() if kd == 'mutator'):
        stage = 0
    vs = []
    for a in g:
        row = (st['v'].get(a, []) + [0] * MAX_V)[:MAX_V]
        vs += row
    return (f'const int main_rowcache_stage = {stage};\n'
            f'const int main_rowcache_n = {len(g)};\n'
            f'const unsigned long long main_rowcache_g[] = {{ {", ".join(f"{a:#x}ull" for a in g) or "0"} }};\n'
            f'const unsigned long long main_rowcache_v[] = {{ {", ".join(f"{v:#x}ull" for v in vs) or "0"} }};\n'
            f'const unsigned main_rowcache_name_off[] = {{ {", ".join(str(st["off"][a]) for a in g) or "0"} }};\n'
            f'const unsigned long long main_rowcache_lo = {lo:#x}ull, main_rowcache_hi = {hi:#x}ull;\n')
