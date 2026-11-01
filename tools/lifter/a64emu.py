"""[UIT 0083] A small AArch64 integer interpreter for the jump-table search in gen.py (replaces Unicorn, GPL-2.0).

gen.py finds the targets of a bounds-checked `br` by running the few instructions between the bounds check and the
`br` once for every index the check lets through. It used to run them in Unicorn; this runs them here, with the same
machine model, so the tables found are the same:
  * the module image is mapped (and writable) at its base, everything else below 2^48 reads as zeros (a page that was
    written keeps what was written; Unicorn mapped a zero page there on first touch); 2^48 and above faults;
  * registers, flags and memory persist from one run to the next (Unicorn's one emulator per module did the same);
  * one instruction per step, branches followed.
Only integer instructions are modelled: ADR/ADRP, add/subtract (immediate, shifted, extended, with carry), logical,
moves, bitfield/EXTR, shifts, multiply/divide, CSEL/CCMP, CLZ/RBIT/REV, branches, loads and stores of every width
(general and vector registers: vector values are bytes moved, not computed). A vector arithmetic instruction makes
the vector registers unknown; one that writes a general register (UMOV, FMOV, FCVTZS...) makes that register unknown;
FCMP makes the flags unknown. Anything else (system registers, atomics, exclusives, SVC, unknown encodings) stops the
run: `Stop` is raised and the caller drops the table, exactly as an emulator fault did (the br then takes the safe
all-labels fallback). An unknown value that reaches an address, a branch decision or the br target also stops it.
tools/tests/test_a64emu.py checks this interpreter against the lifter's own C for the same instructions.
"""
M64 = (1 << 64) - 1
M32 = (1 << 32) - 1
LIMIT = 1 << 48


class Stop(Exception):
    pass


def _bits(w, lo, n):
    return (w >> lo) & ((1 << n) - 1)


def _sext(v, n):
    return v - (1 << n) if v & (1 << (n - 1)) else v


def _ones(n):
    return (1 << n) - 1


def _ror(v, r, size):
    r %= size
    return ((v >> r) | (v << (size - r))) & _ones(size)


def _bitmask(N, imms, immr, size, immediate):
    x = (N << 6) | (~imms & 0x3F)
    if x == 0:
        raise Stop('reserved bitmask')
    length = x.bit_length() - 1
    if length < 1:
        raise Stop('reserved bitmask')
    levels = _ones(length)
    if immediate and (imms & levels) == levels:
        raise Stop('reserved bitmask')
    S, R = imms & levels, immr & levels
    esize = 1 << length
    d = (S - R) & levels
    welem, telem = _ones(S + 1), _ones(d + 1)
    w = _ror(welem, R, esize)
    wmask = tmask = 0
    for i in range(0, size, esize):
        wmask |= w << i
        tmask |= telem << i
    return wmask, tmask


def _u(v):
    if v is None:
        raise Stop('unknown value')
    return v


class Emu:
    def __init__(self, image, base):
        self.image = image          # bytes-like, mapped at base
        self.base = base
        self.x = [0] * 31
        self.sp = 0
        self.pc = 0
        self.nzcv = 0               # packed in bits 31..28, None = unknown
        self.v = [bytes(16)] * 32   # None = unknown
        self.mem = {}               # address -> byte (or None = unknown) written since start

    # ---------------------------------------------------------------- state
    def get(self, r, sp=False, sf=1):
        if r == 31:
            v = self.sp if sp else 0
        else:
            v = self.x[r]
        if v is None:
            return None
        return v if sf else v & M32

    def put(self, r, v, sp=False, sf=1):
        if v is not None:
            v &= M64 if sf else M32
        if r == 31:
            if sp:
                self.sp = v
            return
        self.x[r] = v

    def read(self, a, n):
        a = _u(a) & M64
        if a >= LIMIT or a + n > LIMIT:
            raise Stop('fault')
        out = bytearray()
        for k in range(a, a + n):
            if k in self.mem:
                b = self.mem[k]
                if b is None:
                    return None
            elif self.base <= k < self.base + len(self.image):
                b = self.image[k - self.base]
            else:
                b = 0
            out.append(b)
        return bytes(out)

    def write(self, a, data, n):
        a = _u(a) & M64
        if a >= LIMIT or a + n > LIMIT:
            raise Stop('fault')
        for i in range(n):
            self.mem[a + i] = None if data is None else data[i]

    def fetch(self, a):
        if a % 4 or not (self.base <= a and a + 4 <= self.base + len(self.image)):
            raise Stop('fetch outside the image')
        d = self.read(a, 4)
        if d is None:
            raise Stop('fetch of unknown bytes')
        return int.from_bytes(d, 'little')

    def cond(self, c):
        f = _u(self.nzcv)
        n, z, cy, v = f >> 31 & 1, f >> 30 & 1, f >> 29 & 1, f >> 28 & 1
        base = c >> 1
        r = [z, cy, n, v, cy and not z, n == v, n == v and not z, True][base]
        if c & 1 and c != 15:
            r = not r
        return bool(r)

    def flags(self, n, z, c, v):
        self.nzcv = (n << 31) | (z << 30) | (c << 29) | (v << 28)

    def addc(self, a, b, cin, sz, setf):
        if a is None or b is None or cin is None:
            if setf:
                self.nzcv = None
            return None
        m = _ones(sz)
        a &= m
        b &= m
        r = a + b + cin
        res = r & m
        if setf:
            sa, sb, sr = _sext(a, sz), _sext(b, sz), _sext(res, sz)
            self.flags(res >> (sz - 1), int(res == 0), int(r > m), int(sa + sb + cin != sr))
        return res

    # ---------------------------------------------------------------- run
    def step(self):
        w = self.fetch(self.pc)
        nxt = self.pc + 4
        self.pc = self.exec(w, self.pc, nxt)

    def shift(self, v, typ, amt, sz):
        if v is None:
            return None
        m = _ones(sz)
        if typ == 0:
            return (v << amt) & m
        if typ == 1:
            return v >> amt
        if typ == 2:
            return (_sext(v, sz) >> amt) & m
        return _ror(v, amt, sz)

    def extend(self, v, option, sh, sz):
        if v is None:
            return None
        n = 8 << (option & 3)
        v &= _ones(n)
        if option & 4:
            v = _sext(v, n) & M64
        return (v << sh) & _ones(sz)

    def exec(self, w, pc, nxt):
        sf = w >> 31
        SZ = 64 if sf else 32
        op0 = _bits(w, 25, 4)
        if op0 in (8, 9):
            return self.dp_imm(w, pc, nxt, sf, SZ)
        if op0 in (10, 11):
            return self.branch(w, pc, nxt)
        if op0 in (5, 13):
            self.dp_reg(w, sf, SZ)
            return nxt
        if op0 & 5 == 4:
            self.ldst(w, pc)
            return nxt
        if op0 in (7, 15):
            self.simd_dp(w)
            return nxt
        raise Stop('unallocated')

    def dp_imm(self, w, pc, nxt, sf, SZ):
        rd, rn = w & 31, _bits(w, 5, 5)
        op = _bits(w, 23, 3)
        if op in (0, 1):   # ADR / ADRP
            imm = _sext(_bits(w, 5, 19) << 2 | _bits(w, 29, 2), 21)
            if w >> 31:
                self.put(rd, (pc & ~0xFFF) + (imm << 12))
            else:
                self.put(rd, pc + imm)
            return nxt
        if op == 2:        # add/sub immediate
            subop, S = _bits(w, 30, 1), _bits(w, 29, 1)
            imm = _bits(w, 10, 12) << (12 if _bits(w, 22, 1) else 0)
            a = self.get(rn, sp=True, sf=sf)
            if subop:
                r = self.addc(a, ~imm & _ones(SZ), 1, SZ, S)
            else:
                r = self.addc(a, imm, 0, SZ, S)
            self.put(rd, r, sp=not S, sf=sf)
            return nxt
        if op == 3:
            raise Stop('add/sub immediate with tags')
        if op == 4:        # logical immediate
            opc, N = _bits(w, 29, 2), _bits(w, 22, 1)
            if not sf and N:
                raise Stop('reserved')
            imm, _ = _bitmask(N, _bits(w, 10, 6), _bits(w, 16, 6), SZ, True)
            a = self.get(rn, sf=sf)
            r = None if a is None else [a & imm, a | imm, a ^ imm, a & imm][opc]
            if opc == 3:
                if r is None:
                    self.nzcv = None
                else:
                    self.flags(r >> (SZ - 1), int(r == 0), 0, 0)
            self.put(rd, r, sp=opc != 3, sf=sf)
            return nxt
        if op == 5:        # move wide
            opc, hw = _bits(w, 29, 2), _bits(w, 21, 2)
            if opc == 1 or (not sf and hw >= 2):
                raise Stop('reserved')
            imm = _bits(w, 5, 16) << (16 * hw)
            if opc == 0:
                r = ~imm & _ones(SZ)
            elif opc == 2:
                r = imm
            else:
                old = self.get(rd, sf=sf)
                r = None if old is None else (old & ~(0xFFFF << (16 * hw))) | imm
            self.put(rd, r, sf=sf)
            return nxt
        if op == 6:        # bitfield
            opc, N = _bits(w, 29, 2), _bits(w, 22, 1)
            if opc == 3 or N != sf:
                raise Stop('reserved')
            immr, imms = _bits(w, 16, 6), _bits(w, 10, 6)
            wmask, tmask = _bitmask(N, imms, immr, SZ, False)
            src = self.get(rn, sf=sf)
            dst = self.get(rd, sf=sf) if opc == 1 else 0
            if src is None or dst is None:
                self.put(rd, None, sf=sf)
                return nxt
            bot = (dst & ~wmask) | (_ror(src, immr, SZ) & wmask) if opc == 1 else _ror(src, immr, SZ) & wmask
            if opc == 0:
                top = _ones(SZ) if (src >> imms) & 1 else 0
            elif opc == 1:
                top = dst
            else:
                top = 0
            self.put(rd, (top & ~tmask) | (bot & tmask), sf=sf)
            return nxt
        # EXTR
        if _bits(w, 29, 2) or _bits(w, 22, 1) != sf or _bits(w, 21, 1):
            raise Stop('reserved')
        lsb = _bits(w, 10, 6)
        if not sf and lsb >= 32:
            raise Stop('reserved')
        hi, lo = self.get(rn, sf=sf), self.get(_bits(w, 16, 5), sf=sf)
        if hi is None or lo is None:
            self.put(rd, None, sf=sf)
        else:
            self.put(rd, ((hi << SZ | lo) >> lsb) & _ones(SZ), sf=sf)
        return nxt

    def branch(self, w, pc, nxt):
        if (w & 0x7C000000) == 0x14000000:      # B / BL
            if w >> 31:
                self.x[30] = nxt
            return (pc + (_sext(w & 0x3FFFFFF, 26) << 2)) & M64
        if (w & 0xFF000010) == 0x54000000:      # B.cond
            return (pc + (_sext(_bits(w, 5, 19), 19) << 2)) & M64 if self.cond(w & 15) else nxt
        if (w & 0x7E000000) == 0x34000000:      # CBZ / CBNZ
            v = _u(self.get(w & 31, sf=w >> 31))
            take = (v == 0) != bool(_bits(w, 24, 1))
            return (pc + (_sext(_bits(w, 5, 19), 19) << 2)) & M64 if take else nxt
        if (w & 0x7E000000) == 0x36000000:      # TBZ / TBNZ
            b = _bits(w, 19, 5) | (w >> 31) << 5
            v = _u(self.get(w & 31))
            take = ((v >> b) & 1) == _bits(w, 24, 1)
            return (pc + (_sext(_bits(w, 5, 14), 14) << 2)) & M64 if take else nxt
        m = w & 0xFFFFFC1F
        if m in (0xD61F0000, 0xD63F0000, 0xD65F0000):   # BR / BLR / RET
            t = _u(self.get(_bits(w, 5, 5)))
            if m == 0xD63F0000:
                self.x[30] = nxt
            return t
        if (w & 0xFFFFF01F) == 0xD503201F:       # hints (NOP, YIELD, BTI, ...)
            return nxt
        if (w & 0xFFFFF0FF) in (0xD503309F, 0xD50330BF, 0xD50330DF):   # DSB / DMB / ISB
            return nxt
        if (w & 0xFFFFFFE0) == 0xD53B4200:       # MRS xT, NZCV
            self.put(w & 31, self.nzcv)
            return nxt
        if (w & 0xFFFFFFE0) == 0xD51B4200:       # MSR NZCV, xT
            v = self.get(w & 31)
            self.nzcv = None if v is None else v & 0xF0000000
            return nxt
        raise Stop('system / exception instruction')

    def dp_reg(self, w, sf, SZ):
        rd, rn, rm = w & 31, _bits(w, 5, 5), _bits(w, 16, 5)
        m = _ones(SZ)
        if (w & 0x1F000000) == 0x0A000000:      # logical (shifted register)
            opc, N, typ, amt = _bits(w, 29, 2), _bits(w, 21, 1), _bits(w, 22, 2), _bits(w, 10, 6)
            if not sf and amt >= 32:
                raise Stop('reserved')
            a, b = self.get(rn, sf=sf), self.shift(self.get(rm, sf=sf), typ, amt, SZ)
            if a is None or b is None:
                r = None
            else:
                if N:
                    b = ~b & m
                r = [a & b, a | b, a ^ b, a & b][opc]
            if opc == 3:
                if r is None:
                    self.nzcv = None
                else:
                    self.flags(r >> (SZ - 1), int(r == 0), 0, 0)
            self.put(rd, r, sf=sf)
            return
        if (w & 0x1F200000) == 0x0B000000:      # add/sub (shifted register)
            typ, amt = _bits(w, 22, 2), _bits(w, 10, 6)
            if typ == 3 or (not sf and amt >= 32):
                raise Stop('reserved')
            a, b = self.get(rn, sf=sf), self.shift(self.get(rm, sf=sf), typ, amt, SZ)
            self._addsub(w, rd, a, b, SZ, sf, False)
            return
        if (w & 0x1F200000) == 0x0B200000:      # add/sub (extended register)
            opt, sh = _bits(w, 13, 3), _bits(w, 10, 3)
            if sh > 4 or _bits(w, 22, 2):
                raise Stop('reserved')
            a = self.get(rn, sp=True, sf=sf)
            b = self.extend(self.get(rm), opt, sh, SZ)
            self._addsub(w, rd, a, b, SZ, sf, True)
            return
        if (w & 0x1FE0FC00) == 0x1A000000:      # ADC / SBC (S)
            sub, S = _bits(w, 30, 1), _bits(w, 29, 1)
            a, b = self.get(rn, sf=sf), self.get(rm, sf=sf)
            cin = None if self.nzcv is None else self.nzcv >> 29 & 1
            if sub and b is not None:
                b = ~b & m
            self.put(rd, self.addc(a, b, cin, SZ, S), sf=sf)
            return
        if (w & 0x1FE00410) == 0x1A400000 and _bits(w, 29, 1):   # CCMN / CCMP
            sub = _bits(w, 30, 1)
            a = self.get(rn, sf=sf)
            b = _bits(w, 16, 5) if _bits(w, 11, 1) else self.get(rm, sf=sf)
            if self.cond(_bits(w, 12, 4)):
                if sub and b is not None:
                    self.addc(a, ~b & m, 1, SZ, True)
                else:
                    self.addc(a, b, 0, SZ, True)
            else:
                self.nzcv = (w & 15) << 28
            return
        if (w & 0x1FE00800) == 0x1A800000 and not _bits(w, 29, 1):   # CSEL / CSINC / CSINV / CSNEG
            op, o2 = _bits(w, 30, 1), _bits(w, 10, 1)
            if self.cond(_bits(w, 12, 4)):
                r = self.get(rn, sf=sf)
            else:
                r = self.get(rm, sf=sf)
                if r is not None:
                    if op == 0 and o2:
                        r = (r + 1) & m
                    elif op and not o2:
                        r = ~r & m
                    elif op and o2:
                        r = (-r) & m
            self.put(rd, r, sf=sf)
            return
        if (w & 0x5FE00000) == 0x1AC00000:      # data processing (2 source)
            opc = _bits(w, 10, 6)
            a, b = self.get(rn, sf=sf), self.get(rm, sf=sf)
            if _bits(w, 29, 1) or opc not in (2, 3, 8, 9, 10, 11):
                raise Stop('2-source op')
            if a is None or b is None:
                r = None
            elif opc == 2:
                r = a // b if b else 0
            elif opc == 3:
                if not b:
                    r = 0
                else:
                    sa, sb = _sext(a, SZ), _sext(b, SZ)
                    q = abs(sa) // abs(sb)
                    r = (q if (sa < 0) == (sb < 0) else -q) & m
            else:
                r = self.shift(a, opc - 8, b % SZ, SZ)
            self.put(rd, r, sf=sf)
            return
        if (w & 0x5FE00000) == 0x5AC00000:      # data processing (1 source)
            opc = _bits(w, 10, 6)
            if _bits(w, 16, 5) or _bits(w, 29, 1) or opc > 5 or (opc == 3 and not sf):
                raise Stop('1-source op')
            a = self.get(rn, sf=sf)
            if a is None:
                self.put(rd, None, sf=sf)
                return
            if opc == 0:
                r = int(format(a, f'0{SZ}b')[::-1], 2)
            elif opc in (1, 2, 3):
                cs = [16, 32, 64][opc - 1]
                b = a.to_bytes(SZ // 8, 'little')
                r = int.from_bytes(b''.join(b[i:i + cs // 8][::-1] for i in range(0, SZ // 8, cs // 8)), 'little')
            elif opc == 4:
                r = SZ - a.bit_length()
            else:
                x = (a ^ (_sext(a, SZ) >> 1)) & m      # cpu.h cls: clz(v ^ (v >> 1, arithmetic)) - 1
                r = SZ - x.bit_length() - 1
            self.put(rd, r, sf=sf)
            return
        if (w & 0x1F000000) == 0x1B000000:      # data processing (3 source)
            op31, o0, ra = _bits(w, 21, 3), _bits(w, 15, 1), _bits(w, 10, 5)
            a, b, c = self.get(rn), self.get(rm), self.get(ra)
            if _bits(w, 29, 2) or (not sf and op31):
                raise Stop('3-source op')
            if a is None or b is None or (c is None and op31 not in (2, 6)):
                self.put(rd, None)
                return
            if op31 == 0:
                p = (a & m) * (b & m)
                r = ((c - p) if o0 else (c + p)) & m
                self.put(rd, r, sf=sf)
                return
            if op31 in (1, 5):
                if op31 == 1:
                    p = _sext(a & M32, 32) * _sext(b & M32, 32)
                else:
                    p = (a & M32) * (b & M32)
                self.put(rd, ((c - p) if o0 else (c + p)) & M64)
                return
            if op31 in (2, 6) and not o0:
                p = _sext(a, 64) * _sext(b, 64) if op31 == 2 else a * b
                self.put(rd, (p >> 64) & M64)
                return
            raise Stop('3-source op')
        raise Stop('data processing (register) op')

    def _addsub(self, w, rd, a, b, SZ, sf, ext):
        sub, S = _bits(w, 30, 1), _bits(w, 29, 1)
        if sub:
            r = self.addc(a, None if b is None else ~b & _ones(SZ), 1, SZ, S)
        else:
            r = self.addc(a, b, 0, SZ, S)
        self.put(rd, r, sp=ext and not S, sf=sf)

    # ---------------------------------------------------------------- loads / stores
    def _vget(self, t, n):
        v = self.v[t]
        return None if v is None else v[:n]

    def _vput(self, t, data, n):
        self.v[t] = None if data is None else bytes(data[:n]) + bytes(16 - n)

    def ldst(self, w, pc):
        V = _bits(w, 26, 1)
        if (w & 0x3B000000) == 0x18000000:      # load literal
            opc, rt = _bits(w, 30, 2), w & 31
            a = pc + (_sext(_bits(w, 5, 19), 19) << 2)
            if V:
                if opc == 3:
                    raise Stop('reserved')
                n = 4 << opc
                self._vput(rt, self.read(a, n), n)
                return
            if opc == 3:
                return   # PRFM
            d = self.read(a, 8 if opc == 1 else 4)
            if d is None:
                self.put(rt, None)
            elif opc == 2:
                self.put(rt, _sext(int.from_bytes(d, 'little'), 32))
            else:
                self.put(rt, int.from_bytes(d, 'little'), sf=opc == 1)
            return
        if (w & 0x3A000000) == 0x28000000:      # load/store pair
            opc, mode, L = _bits(w, 30, 2), _bits(w, 23, 2), _bits(w, 22, 1)
            rt, rn, rt2 = w & 31, _bits(w, 5, 5), _bits(w, 10, 5)
            if V:
                if opc == 3:
                    raise Stop('reserved')
                n = 4 << opc
            else:
                if opc == 3 or (opc == 1 and not L):
                    raise Stop('pair op')
                n = 8 if opc == 2 else 4
            off = _sext(_bits(w, 15, 7), 7) * n
            base = _u(self.get(rn, sp=True))
            a = base if mode == 1 else (base + off) & M64
            if L:
                d1, d2 = self.read(a, n), self.read(a + n, n)
                if V:
                    self._vput(rt, d1, n)
                    self._vput(rt2, d2, n)
                else:
                    for t, d in ((rt, d1), (rt2, d2)):
                        if d is None:
                            self.put(t, None)
                        elif opc == 1:
                            self.put(t, _sext(int.from_bytes(d, 'little'), 32))
                        else:
                            self.put(t, int.from_bytes(d, 'little'), sf=n == 8)
            else:
                for k, t in enumerate((rt, rt2)):
                    if V:
                        d = self._vget(t, n)
                    else:
                        v = self.get(t)
                        d = None if v is None else (v & _ones(8 * n)).to_bytes(n, 'little')
                    self.write(a + k * n, d, n)
            if mode in (1, 3):
                self.put(rn, (base + off) & M64, sp=True)
            return
        if (w & 0x3B000000) in (0x38000000, 0x39000000):
            size, opc, rt, rn = _bits(w, 30, 2), _bits(w, 22, 2), w & 31, _bits(w, 5, 5)
            if V:
                scale = size | (opc >> 1) << 2
                if scale > 4:
                    raise Stop('reserved')
                load, n = opc & 1, 1 << scale
            else:
                scale, n = size, 1 << size
                if opc == 2 and size == 3:
                    return                                # PRFM / PRFUM: no effect
                if opc >= 2 and (size == 3 or (size == 2 and opc == 3)):
                    raise Stop('reserved')
                load = opc != 0
            base = _u(self.get(rn, sp=True))
            wb = None
            if (w & 0x3B000000) == 0x39000000:
                a = (base + (_bits(w, 10, 12) << scale)) & M64
            elif _bits(w, 21, 1):
                if _bits(w, 10, 2) != 2:
                    raise Stop('atomic / tagged op')
                opt, S = _bits(w, 13, 3), _bits(w, 12, 1)
                if not opt & 2:
                    raise Stop('reserved')
                a = (base + self.extend(_u(self.get(_bits(w, 16, 5))), opt, scale if S else 0, 64)) & M64
            else:
                imm9, m = _sext(_bits(w, 12, 9), 9), _bits(w, 10, 2)
                if m == 0 or m == 2:                 # unscaled / unprivileged
                    a = (base + imm9) & M64
                elif m == 1:                         # post-index
                    a, wb = base, (base + imm9) & M64
                else:                                # pre-index
                    a = wb = (base + imm9) & M64
            if V:
                if load:
                    self._vput(rt, self.read(a, n), n)
                else:
                    self.write(a, self._vget(rt, n), n)
            elif load:
                d = self.read(a, n)
                if d is None:
                    val = None
                else:
                    val = int.from_bytes(d, 'little')
                    if opc >= 2:                          # LDRS*: to 64 bits (opc 2) or 32 bits (opc 3)
                        val = _sext(val, 8 * n) & (M32 if opc == 3 else M64)
                self.put(rt, val)
            else:
                v = self.get(rt)
                self.write(a, None if v is None else (v & _ones(8 * n)).to_bytes(n, 'little'), n)
            if wb is not None:
                self.put(rn, wb, sp=True)
            return
        if V and (w & 0xBF000000) in (0x0C000000, 0x0D000000):   # vector structure loads / stores
            if _bits(w, 23, 1):
                raise Stop('vector structure op with write-back')
            if _bits(w, 22, 1):
                self.v = [None] * 32                 # loads: the registers become unknown
            else:
                raise Stop('vector structure store')
            return
        raise Stop('load/store op')

    def simd_dp(self, w):
        # FP / SIMD arithmetic: never an address or a table index; only its effects on the integer state matter
        if (w & 0x5F200C00) == 0x1E200400 or (w & 0x5F203C00) == 0x1E202000:   # FCCMP / FCMP
            self.nzcv = None
            return
        if (w & 0x5F20FC00) == 0x1E200000 and _bits(w, 29, 1) == 0:    # FP <-> integer conversions, FMOV general
            if _bits(w, 16, 3) in (0, 1, 4, 5, 6):    # FCVT*S/U, FCVTA*, FMOV / FJCVTZS: to a general register
                self.put(w & 31, None)
                return
            self.v = [None] * 32                     # SCVTF / UCVTF / FMOV from a general register
            return
        if (w & 0xBFE08400) == 0x0E000400 and _bits(w, 11, 4) in (5, 7):   # SMOV / UMOV
            self.put(w & 31, None)
            return
        self.v = [None] * 32
