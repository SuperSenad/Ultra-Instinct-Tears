"""[UIT 0079] The seven call sites of the game's zstd decoder that go to the PC's libzstd (src/host/zstd_native.c).

The sites are valentderah/totk-pc-recomp's 1.4.3 patch set "zstd" (recomp/patches-v143.txt, reference commit c3b9a4e;
Copyright (c) 2026 Valiantsin Dzerakh, MIT, third_party/licenses/valentderah-totk-recomp.txt). There each `bl` is
rewritten to a host page; here the lifter emits a call of the host function instead of the lifted decoder, only when
the instruction at the site is exactly the original `bl` (so another game version, or a moved site, keeps the game's
code). Whether the PC decoder is used is still decided at run time (TOTK_ZSTD_NATIVE, default off): with it off, the
host function calls the lifted decoder straight away.

Never main+0x547a04: that caller (main+0x547964) resets a static DCtx of its own with which the game's zstd rejects
some valid frames (corruption_detected on a 473 KB frame libzstd decodes), and the game relies on that result.

TOTK_LIFT_ZSTD=0 (setup reads it from ultrainstincttears.cfg) lifts the calls as before."""
import os
from collections import Counter

MAIN = 0x800000000
MULTIFRAME, USING_DDICT = 0x3a1f0, 0x3a1e0   # main offsets of ZSTD_decompressMultiFrame / ZSTD_decompress_usingDDict
HOST = {MULTIFRAME: 'uit_zstd_multiframe', USING_DDICT: 'uit_zstd_using_ddict'}
SITES = {   # main offset -> (original word, callee offset)
    0x5f5358: (0x97e913a6, MULTIFRAME),
    0x6d7788: (0x97e58a9a, MULTIFRAME),
    0xbdc8e8: (0x97d17642, MULTIFRAME),
    0x27ce4dc: (0x9761af45, MULTIFRAME),
    0x5f574c: (0x97e912a5, USING_DDICT),
    0xbdc910: (0x97d17634, USING_DDICT),
    0x1852314: (0x979f9fb3, USING_DDICT),
}
NEVER = {0x547a04}   # see above: must keep the game's decoder
assert not (NEVER & set(SITES))
ON = os.environ.get('TOTK_LIFT_ZSTD', '1') != '0'
STATS = Counter()


def _bl_target(off, word):
    imm = word & 0x03FFFFFF
    if imm & 0x02000000:
        imm -= 0x04000000
    return off + 4 * imm


for _o, (_w, _t) in SITES.items():   # the table itself: every word is a bl to the callee it names
    assert _w >> 26 == 0x25 and _bl_target(_o, _w) == _t, hex(_o)


def call(gen, addr, target):
    """The C for the `bl` at addr (target: its callee), or None to lift it as before."""
    if not ON or gen.name != 'main' or gen.base != MAIN:
        return None
    off = addr - MAIN
    if off in NEVER or off not in SITES:
        return None
    word, callee = SITES[off]
    if gen.word(addr) != word or target != MAIN + callee:
        STATS['skipped'] += 1
        return None
    STATS['sites'] += 1
    fn = HOST[callee]
    g = f'f_{target:x}'   # the lifted decoder, passed along: the host function falls back to it
    return (f'c->x[30] = 0x{addr + 4:x}ull; {{ extern void {fn}(Cpu *, void (*)(Cpu *)); {fn}(c, {g}); }}'
            '   /* [UIT 0079] zstd on the PC */')
