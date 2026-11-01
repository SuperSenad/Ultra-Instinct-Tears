"""Generate C for a whole NSO module.

From LASRecomp tools/gen.py @da35b83 (MIT, LICENSE-LASRecomp); TOTK changes: no randomizer variants,
TOTK patch table (patches.py).

Usage: gen.py <name> <nso> <base> <outdir> [--hle hle_symbols.txt] [--chunk N] [--simd asm|c]

--simd: FP/SIMD/CRC32 as AArch64 inline asm (default on arm64 hosts) or portable C (default elsewhere).
"""
import argparse
import bisect
import os
import platform
import re
import struct
import sys
import time
from collections import Counter

from a64emu import Emu, Stop as EmuStop   # [UIT 0083] replaces Unicorn (GPL-2.0)
from analyze import eh_frame_functions
from lifter import Insn, Lifter, Unsupported, bits, emit_asm_batch, hx, sext
from nso import Nso, R_AARCH64_RELATIVE

SCRATCH_MEM = 0x7300000000


def sext26(w):
    v = w & 0x03FFFFFF
    return v - 0x04000000 if v & 0x02000000 else v


def fname(a):
    return f'f_{a:x}'


def sanitize(sym):
    return 'hle_' + re.sub(r'[^A-Za-z0-9_]', '_', sym)


class Module:
    def __init__(self, name, path, base, simd='asm'):
        self.name = name
        self.nso = Nso(path)
        self.base = base
        n = self.nso
        self.lo = base + n.text[0]
        self.hi = base + n.text[0] + n.text[1]
        self.words = struct.unpack(f'<{n.text[1] // 4}I', bytes(n.image[n.text[0]:n.text[0] + n.text[1]]))
        self.lifter = Lifter(base, simd)
        self.cache = {}
        self.stats = Counter()
        self.mu = None
        self.spins = spins.load(name, base)   # [UIT 0077] spin-wait back-edges (spins.py)

    def in_text(self, a):
        return self.lo <= a < self.hi and a % 4 == 0

    def word(self, a):
        return self.words[(a - self.lo) // 4]

    def lift(self, a):
        ins = self.cache.get(a)
        if ins is None:
            try:
                ins = self.lifter.lift(self.word(a), a)
            except Unsupported as e:
                self.stats['unsupported'] += 1
                ins = Insn('trap', f'rt_udf(c, {hx(a)}); /* {e} */')
            self.cache[a] = ins
        return ins

    def boundaries(self):
        n, b = self.nso, self.base
        starts = set()
        for a in eh_frame_functions(n):
            starts.add(b + a)
        for name, info, shndx, value, size in n.syms:
            if shndx and (info & 0xF) == 2:
                starts.add(b + value)
        for off, typ, sym, add in n.relocs:
            if typ == R_AARCH64_RELATIVE:
                starts.add(b + add)
        starts = sorted(a for a in starts if self.in_text(a))
        return starts

    # ------------------------------------------------------------ jump tables

    def emu(self):
        """[UIT 0083] The jump-table interpreter (a64emu.py): the image mapped at its base, writable, zeros
        elsewhere below 2^48. It used to be Unicorn; [TOTK] its unmapped pages already read as zeros (loads unrelated
        to the table between the bounds check and the br, e.g. `ldr q29, [x8, x13]` with both registers holding the
        scratch address, used to fault and drop the whole table: 798 functions in main)."""
        if self.mu is None:
            self.mu = Emu(self.nso.image, self.base)
        return self.mu

    def jump_table(self, br_pc, entry, end):
        """Resolve `br xN` targets from a bounds-checked jump table, or None."""
        self._jt_run = None   # [UIT 0353] how the emulation below ran: (start, set_at, index registers, count)
        r = self._jump_table_fallthrough(br_pc, entry, end)
        if r is None:
            r = self._jump_table_taken(br_pc, entry, end)
        # [TOTK] The emulation sets the register the bounds check compares; when the load indexes the table with
        # another register holding the same value (main 0x80209aa08: `cmp w22, #0xb` ... `ldrh w10, [x8, x28, lsl #1]`)
        # every index gives the same target (a table read at a fixed scratch address). Such a single-target result is
        # replaced by the entries read from the image. (Adding the scanned entries to every table instead put
        # trailing data that looks like entries into 1619 of main's 2367 tables.)
        if r is not None and len(set(r)) > 1:
            more = self._jump_table_other_bases(br_pc, entry, end) - set(r)
            if more:
                self.stats['jumptables_other_bases'] += 1
                return sorted(set(r) | more)
            return r
        s = self._jump_table_scan(br_pc, entry, end)
        if r is not None and s is not None:
            self.stats['jumptables_single_target_rescanned'] += 1
            return sorted(set(r) | set(s))
        # A single target the image scan cannot confirm is a guess (main 0x8006eb84c: a byte table of 7 targets whose
        # register is reused before the br, so the scan misses its adrp/add and the emulation reads one entry at the
        # scratch address; loading a save then ran into "no function at 8006eb850", GitHub issue #18). No table: the
        # br takes the computed-target fallback over all of the function's labels.
        if s is None and r is not None and len(set(r)) <= 1:
            self.stats['jumptables_single_unconfirmed'] += 1
            return None
        return r if r is not None else s

    def _jump_table_scan(self, br_pc, entry, end):
        """[TOTK] A table whose bounds check the compiler hoisted far from the br (main: functions with thousands of
        cases): `adr xB, base; adrp/add xT, table; ldr{b,h,sw} wO, [xT, xI, ...]; add xD, xB, wO, {lsl,sxt*} #k; br xD`.
        The entries are read from the image until one points outside the function or is misaligned; trailing bytes
        that happen to look like entries only add labels (an index the table never had is never taken)."""
        import capstone
        if not hasattr(self, '_cs'):
            self._cs = capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_ARM)
            self._cs.detail = True
        cs = self._cs
        window = []
        a = br_pc
        while a >= entry and br_pc - a <= 4 * 24:
            window.append(a)
            a -= 4
        insns = {}
        for x in window:
            got = list(cs.disasm(struct.pack('<I', self.word(x)), x))
            if not got:
                return None
            insns[x] = got[0]
        rn = bits(self.word(br_pc), 5, 5)
        # add xD(=rn), xB, wO, <ext|lsl> #k
        add = load = None
        for x in window[1:]:
            i = insns[x]
            if i.mnemonic == 'add' and len(i.operands) == 3 and i.reg_name(i.operands[0].reg) == f'x{rn}':
                add = i
                break
            if self.lift(x).kind not in ('op', 'asm'):
                return None
        if add is None or add.operands[2].type != capstone.arm64.ARM64_OP_REG:
            return None
        base_reg = add.reg_name(add.operands[1].reg)
        off_reg = add.reg_name(add.operands[2].reg)
        shift = add.operands[2].shift.value if add.operands[2].shift.type else 0
        values = {}   # register -> constant from adr / adrp+add before the add

        def scan_back(reg, before):
            for x in window:
                if x >= before:
                    continue
                i = insns[x]
                if not i.operands or i.operands[0].type != capstone.arm64.ARM64_OP_REG:
                    continue
                if i.reg_name(i.operands[0].reg) != reg:
                    continue
                return x, i
            return None, None
        x, i = scan_back(base_reg, add.address)
        if i is None or i.mnemonic != 'adr':
            return None
        base = i.operands[1].imm
        # the load of the offset
        x, i = scan_back(off_reg.replace('x', 'w'), add.address)
        if i is None:
            x, i = scan_back(off_reg.replace('w', 'x'), add.address)
        if i is None or i.mnemonic not in ('ldrb', 'ldrh', 'ldrsb', 'ldrsh', 'ldrsw', 'ldr'):
            return None
        mem = i.operands[1]
        if mem.type != capstone.arm64.ARM64_OP_MEM or not mem.mem.index:
            return None
        size = {'ldrb': 1, 'ldrsb': 1, 'ldrh': 2, 'ldrsh': 2, 'ldrsw': 4, 'ldr': 4}[i.mnemonic]
        signed = i.mnemonic.startswith('ldrs')
        table_reg = i.reg_name(mem.mem.base)
        # table address: adrp + add on table_reg, near the br or (hoisted out of a loop) anywhere earlier in the
        # function: the closest earlier write of table_reg must be that add, and the write before it the adrp.
        def writer_before(reg, before):
            for y in range(before - 4, entry - 4, -4):
                got = list(cs.disasm(struct.pack('<I', self.word(y)), y))
                if not got:
                    return None
                j = got[0]
                if j.operands and j.operands[0].type == capstone.arm64.ARM64_OP_REG and j.reg_name(j.operands[0].reg) == reg \
                        and j.mnemonic not in ('str', 'stp', 'stur', 'strb', 'strh', 'cmp', 'cmn', 'tst'):
                    return j
            return None
        ti = writer_before(table_reg, x)
        if ti is None or ti.mnemonic != 'add' or len(ti.operands) != 3 or ti.operands[2].type != capstone.arm64.ARM64_OP_IMM \
                or ti.reg_name(ti.operands[1].reg) != table_reg:
            return None
        pi = writer_before(table_reg, ti.address)
        if pi is None or pi.mnemonic != 'adrp':
            return None
        table = pi.operands[1].imm + ti.operands[2].imm
        lo, hi = self.base, self.base + len(self.nso.image)
        targets = set()
        for k in range(4096):
            at = table + k * size
            if at + size > hi or at < lo:
                break
            raw = int.from_bytes(self.nso.image[at - self.base:at - self.base + size], 'little', signed=signed)
            t = base + (raw << shift)
            if not (entry <= t < end) or t % 4:
                break
            targets.add(t)
        if len(targets) < 2:
            return None
        self.stats['jumptables_scanned'] += 1
        return sorted(targets)

    @staticmethod
    def _table_load(w):
        """[UIT 0353] `ldr{b,h,sb,sh,sw} / ldr wT, [xN, xM{, ext/lsl}]` (load, register offset): (Rt, Rn), else None."""
        if (w & 0x3F200C00) != 0x38200800 or bits(w, 22, 2) == 0 or bits(w, 26, 1):
            return None
        size, opc = bits(w, 30, 2), bits(w, 22, 2)
        if size == 3 or (size == 2 and opc == 3):   # 64-bit loads and prefetches are not table loads
            return None
        return w & 31, bits(w, 5, 5)

    def _jump_table_other_bases(self, br_pc, entry, end):
        """[UIT 0353] Targets of the other tables the table register can hold when the br is reached. A function may
        pick its table on one of several paths before a loop (adrp/add of one table or another into the same register,
        sometimes plus a fixed step); the emulation replays every address materialisation from the entry in order, so
        it reads only the table written last, and the game taking another path reached a target the lift did not have
        ("no function at ..."). Every constant the table register gets in the function is a candidate, and it is
        checked by the same emulation as the table itself, with the register holding the candidate: it is kept only if
        every index the bounds check lets through gives an aligned target in the function. A table whose register is
        set again between the bounds check and the br gives the targets already found (nothing is added).
        TOTK_LIFT_OTHER_TABLES=0 at lift time turns this off (to compare the generated code)."""
        run = self._jt_run
        if run is None or os.environ.get('TOTK_LIFT_OTHER_TABLES', '1') == '0':
            return set()
        start, set_at, idx_regs, count = run
        rn = bits(self.word(br_pc), 5, 5)
        # add xD(=rn), xB, xM / wM{, lsl|ext #k} near the br, then the load of xM from [xT, xI...]
        off = None
        lo_win = max(entry, br_pc - 4 * 24)
        a = br_pc - 4
        while a >= lo_win:
            w = self.word(a)
            if (w & 0xFF200000) == 0x8B000000 or (w & 0xFFE00000) == 0x8B200000:   # ADD (shifted / extended), 64-bit
                if (w & 31) == rn:
                    off = bits(w, 16, 5)
                    break
            a -= 4
        if off is None:
            return set()
        treg = None
        a -= 4
        while a >= lo_win:
            ld = self._table_load(self.word(a))
            if ld is not None and ld[0] == off:
                treg, load_at = ld[1], a
                break
            a -= 4
        if treg is None or treg >= 29 or treg in idx_regs:
            return set()
        # the constants written to xT in the function before the load: adrp (+ add #lo12 right after it), and adds of
        # an immediate to xT itself (a step from one of them)
        consts, steps, paired = set(), [], set()
        for a in range(entry, load_at, 4):
            w = self.word(a)
            if (w & 31) != treg:
                continue
            if (w & 0x9F000000) == 0x90000000:   # ADRP
                imm = sext(bits(w, 5, 19) << 2 | bits(w, 29, 2), 21)
                page = (a & ~0xFFF) + (imm << 12)
                nxt = self.word(a + 4) if a + 4 < load_at else 0
                if (nxt & 0xFFC00000) == 0x91000000 and (nxt & 31) == treg and bits(nxt, 5, 5) == treg:
                    consts.add(page + bits(nxt, 10, 12))
                    paired.add(a + 4)
                else:
                    consts.add(page)
            elif (w & 0x9F000000) == 0x10000000:   # ADR
                consts.add(a + sext(bits(w, 5, 19) << 2 | bits(w, 29, 2), 21))
            elif (w & 0xFF800000) == 0x91000000 and bits(w, 5, 5) == treg and a not in paired:   # ADD xT, xT, #imm{, lsl 12}
                steps.append(bits(w, 10, 12) << (12 if bits(w, 22, 1) else 0))
        cands = consts | {c + k for c in consts for k in steps}
        if len(cands) < 2 or len(cands) > 64:
            return set()
        lo, hi = self.base, self.base + len(self.nso.image)
        mu = self.emu()
        out = set()
        for table in sorted(cands):
            if not (lo <= table < hi):
                continue
            got = []
            try:
                for i in range(count):
                    self._emu_reset(mu)
                    self._emu_replay(mu, entry, start)
                    mu.x[treg] = table
                    if set_at != start and not self._emu_steps(mu, start, set_at):
                        break
                    mu.x[treg] = table   # (again: the run up to the bounds check may have set it)
                    self._set_index(mu, idx_regs, i)
                    if not self._emu_steps(mu, set_at, br_pc):
                        break
                    t = self._emu_reg(mu, rn)
                    if t is None or not (entry <= t < end) or t % 4:
                        break
                    got.append(t)
            except EmuStop:
                continue
            except Exception as e:
                self.stats['jumptables_emu_error'] += 1
                print(f'  jump table at {br_pc:x}: interpreter error {e!r} (other tables)', file=sys.stderr)
                continue
            if len(got) == count:
                out |= set(got)
        return out

    def _index_regs(self, start, at, idx):
        """The index register plus registers holding the same value in start..at: sources it was
        copied/masked from, and copies of those (the table lookup sometimes recomputes the index
        from another copy after the bounds check)."""
        def copy_src(w):
            if (w & 0x7F800000) == 0x12000000:  # AND (imm)
                return bits(w, 5, 5)
            if (w & 0x7FE0FFE0) == 0x2A0003E0:  # MOV (ORR with zr)
                return bits(w, 16, 5)
            if (w & 0x7F800000) in (0x53000000, 0x13000000):  # UBFM/SBFM (uxtb, uxth, sxtw...)
                return bits(w, 5, 5)
            return None
        regs = {idx}
        changed = True
        while changed:
            changed = False
            for a in range(start, at, 4):
                w = self.word(a)
                rd, src = w & 31, copy_src(w)
                if src is None or src >= 31 or rd >= 31:
                    continue
                for x, y in ((rd, src), (src, rd)):
                    if x in regs and y not in regs:
                        regs.add(y)
                        changed = True
        return sorted(regs)

    def _set_index(self, mu, regs, i):
        for r in regs:
            mu.x[r if r < 29 else 29] = i   # (as the Unicorn version did: x29 for any register above x28)

    @staticmethod
    def _emu_reg(mu, rn):
        return mu.x[rn if rn < 30 else 30]

    @staticmethod
    def _emu_steps(mu, frm, to):
        # single-step, at most 64 instructions, branches followed
        mu.pc = frm
        for _ in range(64):
            if mu.pc == to:
                return True
            mu.step()
        return False

    def _emu_replay(self, mu, entry, start):
        # replay address materialization (ADRP/ADR/ADD-imm) from the function entry, so table bases computed
        # before loops or branches are known
        for a2 in range(entry, start, 4):
            w2 = self.word(a2)
            if (w2 & 0x1F000000) == 0x10000000 or (w2 & 0xFF800000) == 0x91000000:
                mu.pc = a2
                mu.step()

    @staticmethod
    def _emu_reset(mu):
        for r in range(29):
            mu.x[r] = SCRATCH_MEM + 0x80000
        mu.sp = SCRATCH_MEM + 0x80000

    def _emulate_table(self, br_pc, entry, end, start, idx, count, set_at):
        """Run start..br for each index value (set at address set_at), collect targets."""
        self._jt_run = (start, set_at, [idx], count)
        mu = self.emu()
        rn = bits(self.word(br_pc), 5, 5)
        targets = []
        try:
            for i in range(count):
                self._emu_reset(mu)
                self._emu_replay(mu, entry, start)
                if set_at != start and not self._emu_steps(mu, start, set_at):
                    return None
                self._set_index(mu, [idx], i)
                if not self._emu_steps(mu, set_at, br_pc):
                    return None
                t = self._emu_reg(mu, rn)
                if t is None or not (entry <= t < end) or t % 4:
                    return None
                targets.append(t)
        except EmuStop:
            return None
        except Exception as e:   # an interpreter bug: drop the table (the safe all-labels fallback), count it
            self.stats['jumptables_emu_error'] += 1
            print(f'  jump table at {br_pc:x}: interpreter error {e!r}', file=sys.stderr)
            return None
        return sorted(set(targets))

    def _jump_table_taken(self, br_pc, entry, end):
        """Form: cmp idx, #N ; b.ls/b.lo table ... table: <address math> ; br"""
        # start of the block holding the br
        s = br_pc
        while s - 4 >= entry and br_pc - (s - 4) <= 4 * 12 and self.lift(s - 4).kind in ('op', 'asm'):
            s -= 4
        for g in range(entry, end, 4):
            w = self.word(g)
            if (w & 0xFF000010) != 0x54000000 or bits(w, 0, 4) not in (9, 3):
                continue
            t = g + sext(bits(w, 5, 19), 19) * 4
            if t != s:
                continue
            cw = self.word(g - 4) if g - 4 >= entry else 0
            if (cw & 0x7F80001F) != 0x7100001F or bits(cw, 22, 1):
                continue
            limit = bits(cw, 10, 12)
            count = limit + 1 if bits(w, 0, 4) == 9 else limit
            if count <= 0 or count > 4096:
                continue
            return self._emulate_table(br_pc, entry, end, s, bits(cw, 5, 5), count, s)
        return None

    def _jump_table_fallthrough(self, br_pc, entry, end):
        rn = bits(self.word(br_pc), 5, 5)
        # find the guarding b.hi / b.hs and its cmp, scanning backwards
        a = br_pc - 4
        guard = cmp = None
        while a >= entry and br_pc - a <= 4 * 16:
            w = self.word(a)
            if (w & 0xFF000010) == 0x54000000 and bits(w, 0, 4) in (8, 2):
                guard = a
                break
            if self.lift(a).kind in ('b', 'bl', 'blr', 'br', 'ret', 'trap'):
                return None
            a -= 4
        if guard is None:
            return None
        a = guard - 4
        while a >= entry and guard - a <= 4 * 8:
            w = self.word(a)
            # SUBS (imm) with Rd = zr, i.e. CMP #imm
            if (w & 0x7F80001F) == 0x7100001F:
                cmp = a
                break
            if self.lift(a).kind != 'op':
                return None
            a -= 4
        if cmp is None:
            return None
        w = self.word(cmp)
        if bits(w, 22, 1):
            return None
        limit = bits(w, 10, 12)
        count = limit + 1 if bits(self.word(guard), 0, 4) == 8 else limit
        if count <= 0 or count > 4096:
            return None
        idx = bits(w, 5, 5)
        # window start: back from cmp to the first control-flow instruction
        start = cmp
        while start - 4 >= entry and cmp - (start - 4) <= 4 * 12 and self.lift(start - 4).kind in ('op', 'asm'):
            start -= 4
        idx_regs = self._index_regs(start, cmp, idx)
        self._jt_run = (start, cmp, idx_regs, count)
        mu = self.emu()
        targets = []
        try:
            for i in range(count):
                self._emu_reset(mu)
                self._emu_replay(mu, entry, start)
                if not self._emu_steps(mu, start, cmp):
                    return None
                self._set_index(mu, idx_regs, i)
                if not self._emu_steps(mu, cmp, br_pc):
                    return None
                t = self._emu_reg(mu, rn)
                if t is None or not (entry <= t < end) or t % 4:
                    return None
                targets.append(t)
        except EmuStop:
            return None
        except Exception as e:
            self.stats['jumptables_emu_error'] += 1
            print(f'  jump table at {br_pc:x}: interpreter error {e!r}', file=sys.stderr)
            return None
        return sorted(set(targets))

    # ------------------------------------------------------------ functions

    def computed_target(self, br_pc, entry):
        """True if the br register comes from address arithmetic (a table) rather than a load."""
        rn = bits(self.word(br_pc), 5, 5)
        a = br_pc - 4
        while a >= entry and br_pc - a <= 4 * 16:
            w = self.word(a)
            if self.lift(a).kind not in ('op', 'asm'):
                return False
            if (w & 31) == rn:
                if (w & 0x1F000000) == 0x0B000000:  # ADD/SUB (shifted or extended register)
                    return True
                if (w & 0x9F000000) == 0x10000000:  # ADR
                    return True
                return False
            a -= 4
        return False

    def scan(self, entry, end):
        """Collect labels and external entries referenced by a function."""
        labels, ext, tables = {entry}, set(), {}
        terminated = False
        for a in range(entry, end, 4):
            ins = self.lift(a)
            if ins.kind in ('b', 'bcond'):
                t = ins.target
                if entry <= t < end:
                    labels.add(t)
                elif self.in_text(t):
                    ext.add(t)
            elif ins.kind == 'bl':
                if self.in_text(ins.target):
                    ext.add(ins.target)
            elif ins.kind == 'br':
                jt = self.jump_table(a, entry, end)
                if jt:
                    tables[a] = jt
                    labels.update(jt)
                    self.stats['jumptables'] += 1
                elif self.computed_target(a, entry):
                    # unresolved table jump: allow jumping to any instruction of the function
                    tables[a] = 'all'
                    self.stats['br_all_labels'] += 1
                else:
                    self.stats['br_dispatch'] += 1
            terminated = ins.kind in ('b', 'br', 'ret', 'trap')
        if not terminated and end < self.hi:
            ext.add(end)
        return labels, ext, tables

    def render_single(self, ins, a, jump, refs):
        """C for one lifted instruction (no batching); used for patch variants."""
        k = ins.kind
        if k == 'asm':
            return [emit_asm_batch([ins.asm])]
        if k in ('op', 'trap'):
            return [ins.code]
        if k == 'b':
            return [jump(ins.target)]
        if k == 'bcond':
            return [f'if ({ins.cond}) {{ {jump(ins.target)} }}']
        if k == 'bl':
            t = ins.target
            if self.in_text(t):
                refs.add(t)
                return [f'c->x[30] = {hx(a + 4)}; {fname(t)}(c);']
            return [f'c->x[30] = {hx(a + 4)}; c->pc = {hx(t)}; rt_dispatch(c);']
        if k == 'blr':
            return [f'c->pc = {ins.target}; c->x[30] = {hx(a + 4)}; rt_dispatch(c);']
        if k == 'br':
            return [f'c->pc = {ins.target}; c->last_br = {hx(a)}; __attribute__((musttail)) return rt_dispatch(c);']
        if k == 'ret':
            return ['return;']
        raise Unsupported(k)

    def emit_function(self, entry, end, labels, tables, hle, aliases=()):
        out = [f'void {fname(entry)}(Cpu *c) {{']
        if entry in hle:
            out.append(f'    {sanitize(hle[entry])}(c);')
            out.append('}')
            return out
        out.append(f'    FN_ENTER({hx(entry)});')
        if aliases:  # [TOTK] seeds.py: f_<alias> sets entry_at and tail-calls this function, which goes on from there
            cases = ' '.join(f'case {hx(t)}: goto L_{t:x};' for t in sorted(aliases))
            out.append(f'    if (__builtin_expect(c->entry_at != 0, 0)) {{ uint64_t at_ = c->entry_at; c->entry_at = 0; '
                       f'switch (at_) {{ {cases} default: rt_bad_entry(c, at_, {hx(entry)}); }} }}')
        batch = []
        refs = set()
        self._refs_now = refs   # [UIT 0070] scan.py's patches render the original instruction through these

        def flush():
            if batch:
                out.append('    ' + emit_asm_batch(batch))
                batch.clear()

        def jump(t):
            if entry <= t < end:
                return f'goto L_{t:x};'
            if not self.in_text(t):
                return f'c->pc = {hx(t)}; __attribute__((musttail)) return rt_dispatch(c);'
            refs.add(t)
            return f'__attribute__((musttail)) return {fname(t)}(c);'
        self._jump = jump

        # [TOTK] Only functions with an unresolved computed branch ('all') get the all-instruction fallback. LASRecomp
        # gave it to every function with a jump table: thousands of labels made clang take hours on single functions
        # (main_127.c: 15 880 labels, >2.5 h at -O2) and kept it from optimizing them. A resolved table's targets are
        # complete (each index was emulated); an index outside them reaches rt_dispatch and fails loudly.
        all_labels = any(v == 'all' for v in tables.values())
        if all_labels:
            labels = set(range(entry, end, 4))
        last = None
        for a in range(entry, end, 4):
            ins = self.lift(a)
            if a in labels:
                flush()
                out.append(f'L_{a:x}:;')
            if a in PATCHES:  # hand-written replacement for one instruction (tools/patches.py)
                flush()
                out.append('    ' + PATCHES[a](self, a))
                last = 'op'
                continue
            k = ins.kind
            if k == 'asm':
                if len(batch) >= 24:
                    flush()
                batch.append(ins.asm)
                try:
                    emit_asm_batch(batch)
                except Unsupported:
                    batch.pop()
                    flush()
                    batch.append(ins.asm)
                last = k
                continue
            flush()
            if k == 'op' or k == 'trap':
                out.append('    ' + ins.code)
            elif k == 'b':
                out.append('    ' + spins.gate(self.spins, a, ins.target, entry, end) + jump(ins.target))
            elif k == 'bcond':
                out.append(f'    if ({ins.cond}) {{ {spins.gate(self.spins, a, ins.target, entry, end)}{jump(ins.target)} }}')
            elif k == 'bl':
                t = ins.target
                zs = zstd_sites.call(self, a, t)   # [UIT 0079] the game's zstd decoder on the PC (zstd_sites.py)
                if zs:
                    refs.add(t)   # the lifted decoder stays: the host function falls back to it
                    out.append('    ' + zs)
                elif self.in_text(t):
                    refs.add(t)
                    out.append(f'    c->x[30] = {hx(a + 4)}; {fname(t)}(c);')
                else:
                    out.append(f'    c->x[30] = {hx(a + 4)}; c->pc = {hx(t)}; rt_dispatch(c);')
            elif k == 'blr':
                out.append(f'    c->pc = {ins.target}; c->x[30] = {hx(a + 4)}; rt_dispatch(c);')
            elif k == 'br':
                # [TOTK] c->last_br: the br a failed dispatch came from ("no function at": runtime/dispatch.c)
                lb = f'c->last_br = {hx(a)}; '
                jt = tables.get(a)
                if jt == 'all':
                    out.append(f'    c->pc = {ins.target}; {lb}goto L_dispatch;')
                    continue
                if jt:
                    cases = ' '.join(f'case {hx(t)}: goto L_{t:x};' for t in jt)
                    out.append(f'    switch ({ins.target}) {{ {cases} default: break; }}')
                    if all_labels:
                        out.append(f'    c->pc = {ins.target}; {lb}goto L_dispatch;')
                    else:
                        out.append(f'    c->pc = {ins.target}; {lb}__attribute__((musttail)) return rt_dispatch(c);')
                    continue
                out.append(f'    c->pc = {ins.target}; {lb}__attribute__((musttail)) return rt_dispatch(c);')
            elif k == 'ret':
                out.append('    return;')
            last = k
        flush()
        if last not in ('b', 'br', 'ret', 'trap'):
            out.append('    ' + jump(end) if end < self.hi else f'    rt_udf(c, {hx(end)});')
        if all_labels:
            out.append('L_dispatch:')
            cases = ' '.join(f'case {hx(t)}: goto L_{t:x};' for t in range(entry, end, 4))
            out.append(f'    switch (c->pc) {{ {cases} default: break; }}')
            out.append('    __attribute__((musttail)) return rt_dispatch(c);')
        out.append('}')
        for t in sorted(aliases):
            out.append(f'void {fname(t)}(Cpu *c) {{ c->entry_at = {hx(t)}; __attribute__((musttail)) return {fname(entry)}(c); }}')
        self._refs = refs
        return regcache.apply(out) if self.lifter.sc else out   # [UIT 0078] TOTK_LIFT_REGCACHE=1 only


from patches import APPLIED, OPTIONAL, PATCHES, table_flags
import spins
import regcache
import zstd_sites
import restrict
import native_libc
from seeds import SEEDS


def table_base(m, i, reg, window=40):
    """True if the ADR at word i loads the base of a jump table: within a few instructions an ADD (register) uses
    `reg` and a BR jumps to the sum. [TOTK] The compiler schedules other address setup between them (main
    0x800d2aab0: the br is 17 instructions after the adr), so the window ends at an unconditional branch or return
    rather than after 16 instructions."""
    regs, added = {reg}, False
    for j in range(i + 1, min(i + 1 + window, len(m.words))):
        w = m.words[j]
        if (w & 0xFC000000) == 0x14000000 or w == 0xD65F03C0:  # B / RET: the block ends without the br
            return False
        if (w & 0xFFFFFC1F) == 0xD61F0000:  # BR Xn
            return added and ((w >> 5) & 31) in regs
        if (w & 0x7F200000) in (0x0B000000, 0x0B200000) and (((w >> 5) & 31) in regs or ((w >> 16) & 31) in regs):
            regs.add(w & 31)  # ADD (shifted/extended register) of the base
            added = True
    return False


# [UIT 0126] A function with an unresolved computed branch ('all' labels: a switch over every one of its instructions)
# is built at -O0 (tools/build_game.py: clang takes up to hours on it otherwise), and that decision is made per file.
# Packed with up to 1500 other functions, it took all of them to -O0 with it (9 of the 1436 files of the 2026-10-30
# build). Such a function now gets a file of its own, like a big one, so only it stays unoptimized and its neighbours
# are built like every other file (-O2, LTO / PGO when chosen). The C of every function is the same, only the file it
# is in changes. TOTK_LIFT_ISOLATE_O0=0 packs them as before (launcher: Performance > CPU+ > Optimize the code next to
# unresolved jumps).
# [UIT 0134] Off by default again (=1 turns it on). tools/build_game.py picks -O0 for a file only when it has more than
# 2000 labels, so a small function with unresolved jumps alone in its file is built at -O2, not -O0 as said above: the
# 2026-10-31 07:41 lift isolated 22 of them, and the build put only 7 files at -O0, so about 15 of these functions (a
# switch over every one of their instructions) and ~13,000 neighbours were optimized for the first time. That is the
# one change to the game's own code between the last clean play and the device losses of 2026-10-31, so the files are
# packed as before until a play with it on is clean.
ISOLATE_ALL_LABELS = os.environ.get('TOTK_LIFT_ISOLATE_O0', '0') not in ('', '0')


def chunk_files(functions, chunk, chunk_bytes, big_fn):
    """Groups (address, lines, refs, alone) in order into files: at most `chunk` functions and about `chunk_bytes` of
    C each; a function above big_fn bytes or with alone set gets a file of its own (big ones compile in parallel)."""
    group, size = [], 0
    for f in functions:
        n = sum(len(l) + 1 for l in f[1])
        own = n > big_fn or f[3]
        if group and (own or len(group) >= chunk or size >= chunk_bytes):
            yield group
            group, size = [], 0
        group.append(f)
        size += n
        if own:
            yield group
            group, size = [], 0
    if group:
        yield group


def write_if_changed(path, text):
    """Leaves unchanged chunks untouched so make only recompiles what a patch affected."""
    try:
        with open(path) as f:
            if f.read() == text:
                return
    except FileNotFoundError:
        pass
    with open(path, 'w') as f:
        f.write(text)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('name')
    ap.add_argument('nso')
    ap.add_argument('base', type=lambda s: int(s, 0))
    ap.add_argument('outdir')
    ap.add_argument('--hle')
    ap.add_argument('--chunk', type=int, default=1500)  # functions per file at most
    ap.add_argument('--chunk-bytes', type=int, default=1500000)  # and about this much C (even compile times)
    ap.add_argument('--big-fn', type=int, default=100000)  # bytes of C from which a function gets its own file
    ap.add_argument('--limit', type=int, default=0)
    ap.add_argument('--simd', choices=('asm', 'c'),
                    default='asm' if platform.machine().lower() in ('arm64', 'aarch64') else 'c')
    args = ap.parse_args()

    t0 = time.time()
    m = Module(args.name, args.nso, args.base, args.simd)
    starts = m.boundaries()
    hle = {}
    if args.hle and os.path.exists(args.hle):
        want = {l.strip() for l in open(args.hle) if l.strip() and not l.startswith('#')}
        for name, info, shndx, value, size in m.nso.syms:
            if shndx and name in want:
                hle[args.base + value] = name

    # Partition text into functions; any branch/call target outside its function becomes a new
    # boundary (splitting the containing function) until a fixpoint is reached.
    start_set = set(starts)
    for i, w in enumerate(m.words):
        if (w & 0xFC000000) == 0x94000000:
            t = m.lo + i * 4 + sext26(w) * 4
            if m.in_text(t):
                start_set.add(t)
    # Code addresses materialized in registers (callbacks, vtables built at runtime): ADR, or
    # ADRP followed shortly by ADD on the same register.
    adrp_regs = {}
    for i, w in enumerate(m.words):
        pc = m.lo + i * 4
        if (w & 0x9F000000) == 0x10000000:  # ADR
            imm = ((w >> 5) & 0x7FFFF) << 2 | ((w >> 29) & 3)
            if imm & (1 << 20):
                imm -= 1 << 21
            # [TOTK] an ADR that is the base of a jump table (adr xT; ...; add xD, xT, wI, ...; br xD) is not a
            # function: as a start it split the switch function there and made the table's targets unreachable.
            if m.in_text(pc + imm) and not table_base(m, i, w & 31):
                start_set.add(pc + imm)
        elif (w & 0x9F000000) == 0x90000000:  # ADRP
            imm = ((w >> 5) & 0x7FFFF) << 2 | ((w >> 29) & 3)
            if imm & (1 << 20):
                imm -= 1 << 21
            adrp_regs[w & 31] = ((pc & ~0xFFF) + (imm << 12), i)
        elif (w & 0xFFC00000) == 0x91000000:  # ADD (imm, 64-bit, no shift)
            rn, rd = (w >> 5) & 31, w & 31
            if rn in adrp_regs and i - adrp_regs[rn][1] <= 8:
                t = adrp_regs[rn][0] + ((w >> 10) & 0xFFF)
                if m.in_text(t):
                    start_set.add(t)
    scanned = {}
    rounds = 0
    while True:
        rounds += 1
        order = sorted(start_set)
        new = set()
        for i, a in enumerate(order):
            end = order[i + 1] if i + 1 < len(order) else m.hi
            key = (a, end)
            if key not in scanned:
                scanned[key] = m.scan(a, end)
            new |= scanned[key][1] - start_set
        if not new:
            break
        start_set |= new
    entries = {}
    for i, a in enumerate(order):
        end = order[i + 1] if i + 1 < len(order) else m.hi
        labels, ext, tables = scanned[(a, end)]
        entries[a] = (end, labels, tables)
    print(f'{args.name}: {len(starts)} boundaries, {len(entries)} functions, {rounds} rounds, {dict(m.stats)}, '
          f'{time.time() - t0:.0f}s', file=sys.stderr)
    # [TOTK] seeds.py: addresses a real run reached that discovery missed get an alias entry into their function
    aliases = {}  # containing function -> [seed]
    for t, why in sorted(SEEDS.items()):
        if not (m.lo <= t < m.hi) or t % 4:
            continue
        if t in entries:
            print(f'{args.name}: seed {t:#x} ({why}): already a function, nothing to add', file=sys.stderr)
            continue
        i = bisect.bisect_right(order, t) - 1
        if i < 0 or order[i] in hle:
            print(f'{args.name}: seed {t:#x} ({why}): in no lifted function, skipped', file=sys.stderr)
            continue
        a = order[i]
        end, labels, tables = entries[a]
        was_label = t in labels
        labels.add(t)
        aliases.setdefault(a, []).append(t)
        brs = []
        for p in range(a, end, 4):
            if m.lift(p).kind == 'br':
                jt = tables.get(p)
                kind = 'all labels' if jt == 'all' else (f'table of {len(jt)}' + (' incl. seed' if t in jt else '')
                                                         if jt else 'dispatch')
                brs.append(f'{p:#x} {kind}')
        print(f'{args.name}: seed {t:#x} ({why}): inside f_{a:x} ({a:#x}..{end:#x}, offset {t - a:#x}), '
              f'{"already" if was_label else "not"} a label; alias entry added; brs: ' + (', '.join(brs) or 'none'),
              file=sys.stderr)

    os.makedirs(args.outdir, exist_ok=True)
    order = sorted(entries)
    if args.name == 'main' and not args.limit:   # [UIT 0070] find the community patch sites in the user's own main
        import scan
        gen_dir = os.path.dirname(os.path.abspath(args.outdir))
        root = os.environ.get('TOTK_LIFT_ROOT') or os.path.dirname(os.path.dirname(gen_dir))
        t1 = time.time()
        scan.run(m, entries, order, root, gen_dir, sys.modules['patches'])
        print(f'{args.name}: patch-site search {time.time() - t1:.0f}s', file=sys.stderr, flush=True)
    if args.limit:
        order = order[:args.limit]
    nlibc = native_libc.find(m.nso, args.base)   # [UIT 0086] memcpy, strlen, ... on the PC's (TOTK_NATIVE_LIBC)
    nchunks = 0
    ci = 0

    def functions():   # (address, lines, refs, own file) in address order, emitted as the files are written
        for a in order:
            end, labels, tables = entries[a]
            lines = m.emit_function(a, end, labels, tables, hle, aliases.get(a, ()))
            if a in nlibc and a not in hle and a not in aliases:   # [UIT 0086] native_libc.py
                lines = native_libc.front(lines, fname(a), nlibc[a])
            lines = restrict.apply(lines, a in hle, restrict.ON or (regcache.ON and m.lifter.sc))   # [UIT 0190] restrict.py
            r = getattr(m, '_refs', set())
            m._refs = set()
            yield a, lines, r, ISOLATE_ALL_LABELS and any(v == 'all' for v in tables.values())

    nalone = 0
    for group in chunk_files(functions(), args.chunk, args.chunk_bytes, args.big_fn):
        body, refs, fns = [], set(), []
        for a, lines, r, alone in group:
            body += lines
            fns.append(a)
            refs |= r
            nalone += alone and len(group) == 1
        ci += len(fns)
        path = os.path.join(args.outdir, f'{args.name}_{nchunks:03d}.c')
        text = [('#define SIMD_FPCR_HOST 1\n' if m.lifter.sc and regcache.FPCR_HOST else '')   # [UIT 0085] before cpu.h
                + '#include "cpu.h"\n' + ('#include "simd.h"\n' if m.lifter.sc else '')
                + ('#include "regcache.h"\n' if m.lifter.sc and regcache.ON else '')]   # [UIT 0078]
        text += [f'void {fname(r)}(Cpu *c);\n' for r in sorted(refs)]
        text += [f'void {sanitize(hle[a])}(Cpu *c);\n' for a in sorted(set(hle) & set(fns))]
        text += ['\n'.join(body), '\n']
        write_if_changed(path, ''.join(text))
        nchunks += 1
        # (the start screen's setup reads these for its progress bar: runtime/gamebuild.c)
        print(f'{args.name}: progress {ci}/{len(order)} files {nchunks}', file=sys.stderr, flush=True)
    for fn in os.listdir(args.outdir):  # an earlier run's extra chunks
        mm = re.fullmatch(re.escape(args.name) + r'_(\d+)\.c', fn)
        if mm and int(mm.group(1)) >= nchunks:
            os.remove(os.path.join(args.outdir, fn))
    # [TOTK] a replacement that was never emitted (its address in no lifted function) would leave the game unpatched
    # without a word: stop instead. PATCHES holds main's addresses only.
    missing = sorted(a for a in PATCHES if m.lo <= a < m.hi and a not in APPLIED) if not args.limit else []
    # (optional patches, off by default: one never emitted only turns its feature off, main_*_patch = 0 in table_flags)
    for a in [a for a in missing if a in OPTIONAL]:
        print(f'WARNING: optional patch at {a:#x} not emitted (in no lifted function): that feature stays off',
              file=sys.stderr, flush=True)
    missing = [a for a in missing if a not in OPTIONAL]
    if missing:
        raise SystemExit(f'{args.name}: patches not emitted at ' + ', '.join(f'{a:#x}' for a in missing))
    import io
    emitted = set(order)
    order = sorted(emitted | {t for a, ts in aliases.items() if a in emitted for t in ts})  # (alias entries too)
    with io.StringIO() as f:
        f.write('#include "cpu.h"\n')
        for a in order:
            f.write(f'void {fname(a)}(Cpu *c);\n')
        f.write(f'const uint64_t {args.name}_fn_addrs[] = {{\n')
        f.write(',\n'.join(hx(a) for a in order))
        f.write('};\n')
        f.write(f'const GuestFn {args.name}_fn_ptrs[] = {{\n')
        f.write(',\n'.join(fname(a) for a in order))
        f.write('};\n')
        f.write(f'const uint64_t {args.name}_fn_count = {len(order)};\n')
        f.write(table_flags(m.lo, m.hi))   # [TOTK] optional patches applied (main_render_res_patch)
        write_if_changed(os.path.join(args.outdir, f'{args.name}_table.c'), f.getvalue())
    if m.spins and not args.limit:   # [UIT 0077]
        print(f'{args.name}: spin-wait gates: {spins.STATS["gates"]} of {len(m.spins)} back-edges'
              + (f', {len(m.spins) - spins.STATS["gates"]} left as they were (not the expected branch)'
                 if spins.STATS['gates'] < len(m.spins) else ''), file=sys.stderr)
    if nlibc:   # [UIT 0086]
        print(f'{args.name}: C library routines with a PC version: {native_libc.STATS["fronts"]} '
              f'({", ".join(sorted(set(nlibc.values())))})'
              + (f', {native_libc.STATS["skipped"]} left as they were' if native_libc.STATS['skipped'] else ''), file=sys.stderr)
    if zstd_sites.STATS['sites'] or zstd_sites.STATS['skipped']:   # [UIT 0079]
        print(f'{args.name}: zstd calls to the PC decoder: {zstd_sites.STATS["sites"]} of {len(zstd_sites.SITES)}'
              + (f', {zstd_sites.STATS["skipped"]} left as they were (not the expected instruction)'
                 if zstd_sites.STATS['skipped'] else ''), file=sys.stderr)
    if restrict.ON:   # [UIT 0190]
        print(f'{args.name}: restrict: {restrict.STATS["functions"]} function definitions take Cpu *__restrict c, '
              f'{restrict.STATS["hle_stubs"]} HLE stubs keep Cpu *c, {restrict.STATS["plain"]} without a loop or cache keep it too', file=sys.stderr)
    if restrict.STATS['loop_barriers'] or restrict.STATS['tail_barriers'] or restrict.STATS['entry_barriers']:   # [UIT 0190/0192]
        print(f'{args.name}: memory read again at {restrict.STATS["loop_barriers"]} loop headers, '
              f'{restrict.STATS["tail_barriers"]} tail calls and {restrict.STATS["entry_barriers"]} function entries',
              file=sys.stderr)
    if regcache.ON and m.lifter.sc:   # [UIT 0078]
        st = regcache.STATS
        print(f'{args.name}: register cache: {st["cached"]} of {st["functions"]} functions keep registers in locals '
              f'({st["regs"]} registers, flags in {st["flags"]}); {st["verify_failed"]} kept the old C (check failed)',
              file=sys.stderr)
        if regcache.FPCR_HOST:   # [UIT 0085]
            print(f'{args.name}: FP rounding follows FPCR in the host FP unit (SIMD_FPCR_HOST)', file=sys.stderr)
        if regcache.FLAGS:   # [UIT 0084]
            print(f'{args.name}: operand flags: {st["flags_operands"]} functions, {st["flags_reads_direct"]} conditions from '
                  f'the compared values, {st["flags_pack_at_spill"]} packed at a write-back; '
                  f'{st["flags_verify_failed"]} kept the register-cache C (check failed)', file=sys.stderr)
    alone = f' ({nalone} of them a function with unresolved jumps on its own: TOTK_LIFT_ISOLATE_O0)' if nalone else ''  # [UIT 0126]
    print(f'{args.name}: wrote {nchunks} chunks{alone}, {time.time() - t0:.0f}s total', file=sys.stderr)


if __name__ == '__main__':
    main()
