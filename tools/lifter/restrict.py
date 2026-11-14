"""[UIT 0190] `Cpu *__restrict c` in the lifted functions (TOTK_LIFT_RESTRICT=1; off by default), and a memory
clobber at every loop header whenever it or the register cache is on.

The lifted code reads and writes the game's memory through plain pointers made from guest addresses (cpu.h LD/ST, may_alias
types). Without more information the compiler must assume any of those stores may land in the Cpu structure, so it
writes every guest register back to `c` before each guest store and reads it again after: loops that load and store
guest memory spend most of their time on that traffic. The Cpu structure is never in guest memory (guest.c
guest_cpu_new: host heap, outside the reserved guest range; the game has no pointer to it), which is exactly what
`restrict` on the parameter states: within the function, only pointers based on `c` reach *c. The register cache
(regcache.py, UIT 0078) rests on the same fact.

What else reaches a thread's Cpu, and why it stays correct:
- every lifted call, rt_dispatch / rt_svc / rt_spin_gate / rt_brk / rt_udf, the zstd sites and the native libc front
  take `c` (or `c->x`), so the compiler treats them as reading and writing *c, as before;
- the other calls lifted code makes (rt_ticks, uit_cheat_*, fnprof_hit) do not touch any Cpu;
- host code that reaches the Cpu through the thread's own pointer (guest.c t_cpu: guest_call_args / GCALL, guest_cpu())
  runs inside an HLE function. An HLE stub (`f_<addr>(Cpu *c) { hle_<name>(c); }`) keeps `Cpu *c` and passes `c`
  through an empty asm statement first: should the HLE be inlined into lifted code (ThinLTO), `c` is still handed to
  code the compiler cannot see, with a memory clobber, before the HLE runs, so nothing is kept in registers across it;
- fault handlers (platform/windows.c, watch.c) only read the Cpu (crash reports, the debug watch).

The function's control flow is unchanged (the instrumentation hash of each function stays the same), so a PGO profile
made without it still applies. Applied to the final lines (after regcache.apply and native_libc.front).

Loops that wait on memory another thread writes. The game waits on a flag with a plain load in a loop (a relaxed
std::atomic load is `ldr; cbz` on AArch64; the hardware reads memory again each pass). The C of that loop only
re-read memory because it also stored the loaded value into c->x[n], which "might" have been the flag. Once the
compiler knows that store does not touch guest memory (restrict here; the register cache's locals, UIT 0078, do the
same), the load is loop-invariant and is hoisted: `L: jmp L`, the thread never sees the flag (box test:
tools/tests/test_lift_restrict.py). So with either on, every loop header (a label some later goto jumps back to,
L_dispatch, the function entry) gets an empty asm statement with a memory clobber: guest memory is read again each
pass, as on the Switch, while the registers kept in locals or behind restrict stay in PC registers. The spin waits of
spin_waits_v143.txt (UIT 0077) already call rt_spin_gate on their back-edge, which the compiler cannot see into, so
they were safe; the barrier covers every other loop (waits the search does not recognise, and loops whose only call is
to a lifted function the compiler can see into after ThinLTO). It adds no instruction, and the instrumentation hash
stays the same."""
import os
import re
from collections import Counter

ON = os.environ.get('TOTK_LIFT_RESTRICT', '') == '1'
# [UIT 0192] TOTK_LIFT_RESTRICT_ALL=1: every function definition, as 0190 did (default: only functions with a loop
# barrier or register-cache locals)
ALL = os.environ.get('TOTK_LIFT_RESTRICT_ALL', '') == '1'
STATS = Counter()
# [UIT 0192] 1: the 0190 barrier at every function entry instead of before each tail call (for comparison only)
ENTRY_BARRIER = os.environ.get('TOTK_LIFT_ENTRY_BARRIER', '') == '1'
DEF = re.compile(r'^void (f_[0-9a-f]+(?:_lifted|_any|_r[0-9a-f]+)?)\(Cpu \*c\) \{')   # [UIT 0370, 0371] _any, _r<start>: gen.py split
ESCAPE = '    __asm__ volatile("" : : "r"(c) : "memory");   /* [UIT 0190] restrict.py: c escapes before the HLE */'


BARRIER = '__asm__ volatile("" : : : "memory");'
LABEL = re.compile(r'^(L_[0-9a-f]+:;|L_dispatch:)\s*$')
GOTO = re.compile(r'goto (L_[0-9a-f]+|L_dispatch);')


TAIL = '__attribute__((musttail)) return '
SPLIT_MARK = '/* [UIT 0370] fast part of a split function: optimized (tools/build_game.py) */'   # = gen.py's


def loop_barriers(lines):
    """[UIT 0190] A memory clobber at every loop header: L_dispatch and each label that a goto at or after it jumps
    to (the lines are in address order). [UIT 0192] and right before each tail call: a cycle of tail calls is the
    only other way the C can loop (the compiler turns a tail call to itself, also one it gets by inlining, into a
    jump back). 0190 put that barrier at each function's entry instead; once a small function is inlined into its
    caller, an entry barrier makes the caller write every register it holds back to the Cpu struct and read it again
    (a call-heavy kernel ran 39% slower: tools/tests/bench_lift.py), while a barrier before a tail call sits where the
    registers are written back anyway."""
    at = {}
    for i, l in enumerate(lines):
        m = LABEL.match(l)
        if m:
            at[m.group(1).rstrip(':;')] = i
    back = {'L_dispatch'} & set(at)
    # [UIT 0370] in the fast part of a split function (gen.py SPLIT_MARK) the dispatch switch's jumps are not loop
    # edges of their own: every cycle through them passes L_dispatch, which has the barrier. (Elsewhere, as before:
    # the switch comes after every label, so each of them got one.)
    split = any(l == SPLIT_MARK for l in lines)
    for i, l in enumerate(lines):
        if split and i and lines[i - 1] == 'L_dispatch:' and l.startswith('    switch (c->pc'):
            continue
        for t in GOTO.findall(l):
            if t in at and at[t] <= i:
                back.add(t)
    out = []
    for i, l in enumerate(lines):
        if ENTRY_BARRIER and DEF.match(l) and l.rstrip().endswith('{'):
            out.append(l)
            out.append(f'    {BARRIER}   /* [UIT 0190] guest memory is read again in each loop (restrict.py) */')
            STATS['entry_barriers'] += 1
            continue
        if TAIL in l and not ENTRY_BARRIER:
            n = l.count(TAIL)
            l = l.replace(TAIL, BARRIER + ' ' + TAIL)
            STATS['tail_barriers'] += n
        out.append(l)
        m = LABEL.match(l)
        if m and m.group(1).rstrip(':;') in back:
            out.append(f'    {BARRIER}')
            STATS['loop_barriers'] += 1
    return out


def apply(lines, hle_stub=False, loops=None):
    """The function's lines with `Cpu *__restrict c` in its definitions; an HLE stub keeps `Cpu *c` and gets the
    escape before the HLE call. loops (default: ON): the loop-header barriers (also wanted with the register cache
    alone)."""
    if loops is None:
        loops = ON
    if hle_stub:
        if ON:
            STATS['hle_stubs'] += 1
            return [lines[0], ESCAPE] + lines[1:]
        return lines
    if loops:
        lines = loop_barriers(lines)
    if not ON:
        return lines
    if not ALL and not any(l.strip() == BARRIER or 'rc_' in l for l in lines):
        # [UIT 0192] only where it pays: a function with a loop or the register cache. In a small straight-line function
        # restrict gains nothing and makes the compiler keep more values in callee-saved registers (a push / pop of
        # up to 7 around the cold FN_ENTER check), which made a call-heavy kernel 6% slower (tools/tests/bench_lift.py)
        STATS['plain'] += 1
        return lines
    out = []
    for l in lines:
        m = DEF.match(l)
        if m:
            l = f'void {m.group(1)}(Cpu *__restrict c) {{' + l[m.end():]
            STATS['functions'] += 1
        out.append(l)
    return out
