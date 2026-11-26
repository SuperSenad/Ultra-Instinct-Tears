"""[UIT 0452] Box test of the button font edit of src/host/prompts.c on made-up fonts (no game data).

    python tools/tests/test_prompts.py /tmp/t/prompts        (the harness built from tools/tests/test_prompts.c; needs fontTools)

Builds OpenType CFF fonts like the game's button font with fontTools (round icons with letters at U+E040.., other
glyphs around them, a nominal width that is not 0, cmap format 4 or 12), wraps each as a .bfotf inside a SARC like a
Font/*.bfarc, runs the harness's "font" mode for each style and checks the result with fontTools:
  - the archive and the font keep their sizes and every byte outside the button font's CharStrings data, offsets,
    hmtx and checksums;
  - every glyph still decodes; the glyphs not asked for draw exactly as before;
  - Xbox: A/B and X/Y trade the game's own outlines and advances; L R ZL ZR Plus Minus are our icons;
  - PlayStation: the face buttons are our symbols, inside the glyph's advance;
  - fonts whose glyphs leave too little room keep their face buttons (or everything) as they are;
  - padded charstrings ("glyphs" mode) draw exactly what the unpadded ones draw.
"""
import io
import os
import random
import struct
import subprocess
import sys
import tempfile
import types

from fontTools.fontBuilder import FontBuilder
from fontTools.misc.psCharStrings import T2CharString
from fontTools.pens.boundsPen import BoundsPen
from fontTools.pens.recordingPen import RecordingPen
from fontTools.pens.t2CharStringPen import T2CharStringPen
from fontTools.ttLib import TTFont
from fontTools.ttLib.tables._c_m_a_p import CmapSubtable

HARNESS = sys.argv[1] if len(sys.argv) > 1 else '/tmp/t/prompts'
fails = 0


def check(cond, what):
    global fails
    if not cond:
        fails += 1
        print('FAIL', what)


CPS = [0xE040, 0xE041, 0xE042, 0xE043, 0xE044, 0xE045, 0xE046, 0xE047, 0xE048, 0xE049, 0xE04A, 0xE04B, 0xE04C, 0xE04D]
NOMINAL = 300


def circle(pen, cx, cy, r, ccw=True):
    k = 0.5523 * r
    pts = [((cx + r, cy + k), (cx + k, cy + r), (cx, cy + r)), ((cx - k, cy + r), (cx - r, cy + k), (cx - r, cy)),
           ((cx - r, cy - k), (cx - k, cy - r), (cx, cy - r)), ((cx + k, cy - r), (cx + r, cy - k), (cx + r, cy))]
    if ccw:
        pen.moveTo((cx + r, cy))
        for a, b, c in pts:
            pen.curveTo(a, b, c)
    else:
        pen.moveTo((cx + r, cy))
        for a, b, c in reversed(pts):
            pass
        rev = [(cx + r, cy - k), (cx + k, cy - r), (cx, cy - r), (cx - k, cy - r), (cx - r, cy - k), (cx - r, cy),
               (cx - r, cy + k), (cx - k, cy + r), (cx, cy + r), (cx + k, cy + r), (cx + r, cy + k), (cx + r, cy)]
        for i in range(0, 12, 3):
            pen.curveTo(rev[i], rev[i + 1], rev[i + 2])
    pen.closePath()


def make_font(roomy, fmt12=False, seed=1):
    rnd = random.Random(seed)
    order = ['.notdef', 'space', 'A', 'B'] + [f'uni{cp:04X}' for cp in CPS] + ['Z', 'zz.alt']
    fb = FontBuilder(1000, isTTF=False)
    fb.setupGlyphOrder(order)
    cmap = {0x20: 'space', 0x41: 'A', 0x42: 'B', 0x5A: 'Z'}
    cmap.update({cp: f'uni{cp:04X}' for cp in CPS})
    fb.setupCharacterMap(cmap)
    cs, metrics = {}, {}
    for name in order:
        adv = 640 if name.startswith('uni') else 500
        if name in ('uniE046', 'uniE047'):
            adv = 760
        pen = T2CharStringPen(adv - NOMINAL, None)
        if name == 'space':
            pass
        elif name.startswith('uni') and roomy:
            circle(pen, adv / 2, 302.5, 282.5)
            # a "letter": a jagged polygon hole with many points (the game's glyphs have letter outlines)
            n = 18 + rnd.randrange(10)
            pts = []
            for i in range(n):
                ang = -2 * 3.14159 * i / n
                rr = 120 + rnd.randrange(60)
                pts.append((round(adv / 2 + rr * __import__('math').cos(ang)), round(302 + rr * __import__('math').sin(ang))))
            pen.moveTo(pts[0])
            for p in pts[1:]:
                pen.lineTo(p)
            pen.closePath()
            circle(pen, adv / 2, 302.5, 40 + rnd.randrange(30))
        else:
            pen.moveTo((50, 0)); pen.lineTo((adv - 50, 0)); pen.lineTo((adv - 50, 300 + rnd.randrange(300))); pen.lineTo((50, 600)); pen.closePath()
        cs[name] = pen.getCharString()
        metrics[name] = (adv, 50)
    fb.setupCFF('TestButtons', {'FullName': 'Test Buttons'}, cs, {'nominalWidthX': NOMINAL, 'defaultWidthX': 500})
    fb.setupHorizontalMetrics(metrics)
    fb.setupHorizontalHeader(ascent=800, descent=-200)
    fb.setupNameTable({'familyName': 'Test Buttons', 'styleName': 'Regular'})
    fb.setupOS2()
    fb.setupPost()
    if fmt12:
        st = CmapSubtable.newSubtable(12)
        st.platformID, st.platEncID, st.language = 3, 10, 0
        st.cmap = dict(cmap)
        fb.font['cmap'].tables = [st]
    buf = io.BytesIO()
    fb.font.save(buf)
    data = buf.getvalue()
    if len(data) % 4:
        data += b'\0' * (-len(data) % 4)
    return data


def xor(data, key):
    return bytes(b ^ key[i % 4] for i, b in enumerate(data))


def bfotf(otf, key=b'\x5a\xc3\x11\x7e'):
    return b'\x01\x02\x03\x04' + xor(struct.pack('>I', len(otf)) + otf, key)


def unbfotf(blob):
    key = xor(blob[4:8], struct.pack('>I', len(blob) - 8))
    return xor(blob[8:], key)


def sarc(members):
    names, data, entries = b'', b'', []
    for name, body in members:
        noff = len(names) // 4
        names += name.encode() + b'\0'
        names += b'\0' * (-len(names) % 4)
        data += b'\0' * (-len(data) % 8)
        entries.append((noff, len(data), len(data) + len(body)))
        data += body
    head = bytearray(0x14 + 12 + 16 * len(members) + 8)
    head[0:4] = b'SARC'
    struct.pack_into('<H', head, 4, 0x14)
    head[6:8] = b'\xff\xfe'
    head[0x14:0x18] = b'SFAT'
    struct.pack_into('<HHI', head, 0x18, 12, len(members), 0x65)
    for i, (noff, lo, hi) in enumerate(entries):
        struct.pack_into('<IIII', head, 0x20 + 16 * i, i, 0x01000000 | noff, lo, hi)
    sf = 0x20 + 16 * len(members)
    head[sf:sf + 4] = b'SFNT'
    struct.pack_into('<H', head, sf + 4, 8)
    out = bytes(head) + names
    out += b'\0' * (-len(out) % 64)
    start = len(out)
    out += data
    out = bytearray(out)
    struct.pack_into('<II', out, 8, len(out), start)
    return bytes(out), start, entries


def outlines(font):
    gs = font.getGlyphSet()
    out = {}
    for g in font.getGlyphOrder():
        rec = RecordingPen()
        gs[g].draw(rec)
        out[g] = [v for v in rec.value if v[0] != 'endPath']
    return out


def drawn(rec):
    """the recording without moves that draw nothing (the padding moves)"""
    out, cur = [], []
    for op, args in rec:
        if op == 'moveTo':
            if any(o not in ('moveTo',) for o, _ in cur):
                out += cur
            cur = [(op, args)]
        else:
            cur.append((op, args))
    if any(o not in ('moveTo', 'closePath') for o, _ in cur):
        out += cur
    return out


def run(style, otf):
    blob = bfotf(otf)
    arc, start, entries = sarc([('scft/Other.bfotf', b'OTHERFONT' * 7), ('scft/External.bfotf', blob)])
    with tempfile.TemporaryDirectory() as td:
        i, o = os.path.join(td, 'in'), os.path.join(td, 'out')
        open(i, 'wb').write(arc)
        r = subprocess.run([HARNESS, 'font', style, i, o], capture_output=True, text=True)
        out = open(o, 'rb').read()
    res = int(r.stdout.split('result ')[1].split()[0])
    check(len(out) == len(arc), f'{style}: archive size kept')
    lo, hi = start + entries[1][1], start + entries[1][2]
    check(out[:lo] == arc[:lo] and out[hi:] == arc[hi:], f'{style}: only the button font changed')
    new = unbfotf(out[lo:hi])
    check(out[lo:lo + 8] == arc[lo:lo + 8], f'{style}: bfotf header kept')
    return res, new, r.stderr


def compare(style, otf, res, new, expect_faces, expect_others):
    f0, f1 = TTFont(io.BytesIO(otf)), TTFont(io.BytesIO(new))
    o0, o1 = outlines(f0), outlines(f1)   # decodes every glyph
    cmap = f0.getBestCmap()
    face = [cmap[cp] for cp in CPS[:4]]
    others = [cmap[cp] for cp in (0xE044, 0xE045, 0xE046, 0xE047, 0xE04B, 0xE04C)]
    changed = set()
    if expect_faces:
        changed |= set(face)
    if expect_others:
        changed |= set(others)
    for g in f0.getGlyphOrder():
        if g not in changed:
            check(drawn(o0[g]) == drawn(o1[g]), f'{style}: {g} untouched')
    hm0, hm1 = f0['hmtx'].metrics, f1['hmtx'].metrics
    if expect_faces and style == 'xbox':
        for a, b in ((0, 1), (1, 0), (2, 3), (3, 2)):
            check(drawn(o1[face[a]]) == drawn(o0[face[b]]), f'xbox: {face[a]} draws the old {face[b]}')
            check(hm1[face[a]] == hm0[face[b]], f'xbox: {face[a]} metrics of {face[b]}')
    gs = f1.getGlyphSet()
    for g in changed:
        b = BoundsPen(gs)
        gs[g].draw(b)
        adv = hm1[g][0]
        check(b.bounds is not None, f'{style}: {g} draws something')
        if b.bounds:
            x0, y0, x1, y1 = b.bounds
            if not (style == 'xbox' and g in face):   # Xbox face buttons: the game's own outlines
                check(x0 >= 15 and x1 <= adv - 15 and y0 >= 15 and y1 <= 590, f'{style}: {g} inside its advance {b.bounds} {adv}')
                check(abs(hm1[g][1] - round(x0)) <= 1, f'{style}: {g} lsb {hm1[g][1]} vs {x0}')
    if expect_faces and style != 'xbox':
        check(drawn(o1[face[0]]) != drawn(o0[face[0]]), f'{style}: face changed')
    # the font is still whole: fontTools writes it again
    buf = io.BytesIO()
    f1.save(buf)
    # checksums: head.checkSumAdjustment makes the whole font sum to 0xB1B0AFBA
    total = sum(struct.unpack('>%dI' % (len(new) // 4), new)) & 0xffffffff
    check(total == 0xB1B0AFBA, f'{style}: font checksum {total:#x}')


def main():
    roomy, roomy12, tight = make_font(True), make_font(True, fmt12=True, seed=7), make_font(False)
    for style in ('xbox', 'ps4', 'ps5'):
        res, new, err = run(style, roomy)
        check(res == 10, f'{style}: 10 glyphs changed (got {res}): {err}')
        compare(style, roomy, res, new, True, True)
        res, new, err = run(style, roomy12)
        check(res == 10, f'{style} format 12: 10 glyphs changed (got {res}): {err}')
        compare(style, roomy12, res, new, True, True)
        res, new, err = run(style, tight)
        if style == 'xbox':   # the face buttons trade places exactly; the drawn icons do not fit
            check(res == 4, f'xbox tight: 4 glyphs (got {res}): {err}')
            compare(style, tight, res, new, True, False)
        else:
            check(res == 0 and new == tight, f'{style} tight: unchanged (got {res}): {err}')
    # padding draws the same
    with tempfile.TemporaryDirectory() as td:
        subprocess.run([HARNESS, 'glyphs', td, '700', str(NOMINAL)], check=True)
        names = sorted({f.split('.')[0] for f in os.listdir(td)})
        for n in names:
            base = open(os.path.join(td, f'{n}.0.cs'), 'rb').read()
            ref = None
            for pad in range(10):
                b = open(os.path.join(td, f'{n}.{pad}.cs'), 'rb').read()
                check(len(b) == len(base) + pad, f'{n}: padded by {pad}')
                cs = T2CharString(bytecode=b, private=types.SimpleNamespace(nominalWidthX=NOMINAL, defaultWidthX=500, Subrs=[]), globalSubrs=[])
                rec = RecordingPen()
                cs.draw(rec)
                cs.decompile()
                check(cs.program[0] == 700 - NOMINAL, f'{n}: width operand')
                d = drawn(rec.value)
                if ref is None:
                    ref = d
                check(d == ref, f'{n}: pad {pad} draws the same')
    print('test_prompts.py:', f'{fails} FAILED' if fails else 'all ok')
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())
