"""[UIT 0452] Write src/host/prompt_icons.h: the outlines of the controller button icons the game shows when
Game > Controller > Button prompts is Xbox, PlayStation 4 or PlayStation 5 (src/host/prompts.c).

    python tools/gen_prompt_icons.py            (plain Python; the header is in git, so a build never runs this)

Every shape is drawn here from circles, rounded rectangles, polygons and a few curves: our own art, letters and digits
included (the L B R T 1 2 of LB, RT, L1, R2 ...: simple bold shapes with few points, as the font's room is small). No
game file and no font file is read.

Units: the game's button font (1000 units to the em; its round A / B / X / Y icons fill y 20..585, y up). prompts.c
fits each icon into the advance of the glyph it replaces and, for the HUD's button images, into the image's square.
Outer contours run counter-clockwise and holes clockwise, so the nonzero and the even-odd rule fill them the same.
"""
import math
import pathlib


ROOT = pathlib.Path(__file__).resolve().parent.parent
OUT = ROOT / 'src/host/prompt_icons.h'

Y0, Y1 = 20, 585
CY, R = (Y0 + Y1) / 2, (Y1 - Y0) / 2
K = 0.5523  # quarter circle as one cubic: control distance per unit of radius


def end_point(contour):
    last = contour[-1]
    if len(contour) == 1:
        return last
    return last[-1]


def area(contour):
    """Signed area (y up: counter-clockwise > 0), curves by their control polygon (enough for the sign)."""
    pts = [contour[0]] + [p for seg in contour[1:] for p in seg[1:]]
    return sum(pts[i][0] * pts[(i + 1) % len(pts)][1] - pts[(i + 1) % len(pts)][0] * pts[i][1] for i in range(len(pts))) / 2


def reverse(contour):
    """The same contour drawn the other way round (closed by a line where it does not end at its start)."""
    c, start = list(contour), contour[0]
    if end_point(c) != start:
        c.append(('L', start))
    segs, cur = [], start
    for seg in c[1:]:
        segs.append((cur, seg))
        cur = seg[-1]
    out = [start]
    for frm, seg in reversed(segs):
        out.append(('L', frm) if seg[0] == 'L' else ('C', seg[2], seg[1], frm))
    if len(out) > 1 and out[-1][0] == 'L' and out[-1][1] == start:
        out.pop()   # the closing line is implicit
    return out


def oriented(contour, ccw):
    return contour if (area(contour) > 0) == ccw else reverse(contour)


def circle(cx, cy, r, ccw=True):
    k = K * r
    c = [(cx + r, cy),
         ('C', (cx + r, cy + k), (cx + k, cy + r), (cx, cy + r)),
         ('C', (cx - k, cy + r), (cx - r, cy + k), (cx - r, cy)),
         ('C', (cx - r, cy - k), (cx - k, cy - r), (cx, cy - r)),
         ('C', (cx + k, cy - r), (cx + r, cy - k), (cx + r, cy))]
    return oriented(c, ccw)


def rounded(x0, y0, x1, y1, bottom, top=None, ccw=True):
    top = bottom if top is None else top
    kb, kt = K * bottom, K * top
    c = [(x0 + bottom, y0), ('L', (x1 - bottom, y0)),
         ('C', (x1 - bottom + kb, y0), (x1, y0 + bottom - kb), (x1, y0 + bottom)), ('L', (x1, y1 - top)),
         ('C', (x1, y1 - top + kt), (x1 - top + kt, y1), (x1 - top, y1)), ('L', (x0 + top, y1)),
         ('C', (x0 + top - kt, y1), (x0, y1 - top + kt), (x0, y1 - top)), ('L', (x0, y0 + bottom)),
         ('C', (x0, y0 + bottom - kb), (x0 + bottom - kb, y0), (x0 + bottom, y0))]
    return oriented(c, ccw)


def polygon(points, ccw=True):
    return oriented([points[0]] + [('L', p) for p in points[1:]], ccw)


# ---- our own letters: bold, simple shapes in a 1000-unit cap height (few points: the font's room is small)
S = 230   # stroke
HC = 1000


def half(x, y0, y1, ccw):
    """a half disc bulging right from (x, y0) to (x, y1) as one cubic (y1 > y0 when ccw)"""
    r = abs(y1 - y0) / 2
    k = 4 / 3 * r
    return ('C', (x + k, y0), (x + k, y1), (x, y1))


def glyph_L():
    return 560, [polygon([(0, 0), (560, 0), (560, S), (S, S), (S, HC), (0, HC)])]


def glyph_T():
    w, h = 640, S / 2
    return w, [polygon([(w / 2 - h, 0), (w / 2 + h, 0), (w / 2 + h, HC - S), (w, HC - S), (w, HC), (0, HC), (0, HC - S), (w / 2 - h, HC - S)])]


def glyph_1():
    w = 520
    x0, x1 = w - S - 40, w - 40
    return w, [polygon([(x0, 0), (x1, 0), (x1, HC), (x0 - 10, HC), (40, HC - 260), (40, HC - 260 - S * 0.95), (x0, HC - S * 1.55)])]


def glyph_B():
    w, m = 660, 540
    xb, xt = w - m / 2 * 4 / 3 * 0.75, w - (HC - m) / 2 * 4 / 3 * 0.75   # the bowls' straight ends
    outer = [(0, 0), ('L', (xb, 0)), half(xb, 0, m, True), ('L', (xt, m)), half(xt, m, HC, True), ('L', (0, HC))]
    lo = [(S, S), ('L', (xb, S)), half(xb, S, m - S / 2, True), ('L', (S, m - S / 2))]
    hi = [(S, m + S / 2 - 20), ('L', (xt, m + S / 2 - 20)), half(xt, m + S / 2 - 20, HC - S, True), ('L', (S, HC - S))]
    return w, [oriented(outer, True), oriented(lo, False), oriented(hi, False)]


def glyph_R():
    w, m = 660, 450
    xt = w - (HC - m) / 2 * 4 / 3 * 0.75
    outer = [(0, 0), ('L', (S, 0)), ('L', (S, m)), ('L', (S + 40, m)), ('L', (w - S - 20, 0)), ('L', (w, 0)),
             ('L', (S + 230, m + 30)), ('L', (xt, m + 30)), half(xt, m + 30, HC, True), ('L', (0, HC))]
    hole = [(S, m + S / 2 + 10), ('L', (xt, m + S / 2 + 10)), half(xt, m + S / 2 + 10, HC - S, True), ('L', (S, HC - S))]
    return w, [oriented(outer, True), oriented(hole, False)]


def glyph_2():
    w = 640
    outer = [(0, 0), ('L', (w, 0)), ('L', (w, S)), ('L', (S * 1.55, S)), ('L', (w - 10, 600)),
             ('C', (w + 40, 680), (w, HC), (w / 2, HC)), ('C', (110, HC), (10, 870), (10, 690)), ('L', (S + 20, 690)),
             ('C', (S + 20, 760), (w / 2 - 110, HC - S), (w / 2, HC - S)), ('C', (w - S - 10, HC - S), (w - S + 20, 700), (w - S - 30, 610)),
             ('L', (0, S * 0.9))]
    return w, [oriented(outer, True)]


GLYPHS = {'L': glyph_L, 'T': glyph_T, '1': glyph_1, 'B': glyph_B, 'R': glyph_R, '2': glyph_2}


def letters(text, cx, cy, cap, max_width):
    """our letters for text centred on (cx, cy), at most cap high and max_width wide, as holes (counters fill back)"""
    parts, x = [], 0.0
    for ch in text:
        w, cs = GLYPHS[ch]()
        parts.append((cs, x))
        x += w + 90
    width = x - 90
    s = min(cap / HC, max_width / width)
    ox, oy = cx - width / 2 * s, cy - HC / 2 * s
    out = []
    for cs, dx in parts:
        for c in cs:
            t = [((c[0][0] + dx) * s + ox, c[0][1] * s + oy)] + [
                (seg[0],) + tuple(((q[0] + dx) * s + ox, q[1] * s + oy) for q in seg[1:]) for seg in c[1:]]
            out.append(reverse(t))   # outsides become holes in the shape, counters fill back
    return out


# ---------------------------------------------------------------- the icons
def ps_cross(adv=640):
    cx, a, w = adv / 2, R * 0.50, R * 0.105   # arm reach and half thickness, along the diagonals
    pts = []
    for k in range(4):   # each arm: two points at its tip, then the inner corner
        ang = math.pi / 4 + k * math.pi / 2
        ux, uy = math.cos(ang), math.sin(ang)
        px, py = -uy, ux
        pts.append((cx + ux * a - px * w, CY + uy * a - py * w))
        pts.append((cx + ux * a + px * w, CY + uy * a + py * w))
        nang = ang + math.pi / 2
        ix = (math.cos(ang) + math.cos(nang))
        iy = (math.sin(ang) + math.sin(nang))
        n = math.hypot(ix, iy)
        r = w * math.sqrt(2)
        pts.append((cx + ix / n * r, CY + iy / n * r))
    return adv, [circle(cx, CY, R), polygon(pts, ccw=False)]


def ps_circle(adv=640):
    cx = adv / 2
    return adv, [circle(cx, CY, R), circle(cx, CY, R * 0.50, ccw=False), circle(cx, CY, R * 0.30, ccw=True)]


def ps_square(adv=640):
    cx, s = adv / 2, R * 0.40
    return adv, [circle(cx, CY, R), polygon([(cx - s, CY - s), (cx + s, CY - s), (cx + s, CY + s), (cx - s, CY + s)], ccw=False),
                 polygon([(cx - s + 46, CY - s + 46), (cx + s - 46, CY - s + 46), (cx + s - 46, CY + s - 46), (cx - s + 46, CY + s - 46)], ccw=True)]


def ps_triangle(adv=640):
    cx, r = adv / 2, R * 0.56
    oy = CY - r * 0.12
    outer = [(cx + r * math.cos(a), oy + r * math.sin(a)) for a in (math.pi / 2, math.pi / 2 + 2 * math.pi / 3, math.pi / 2 + 4 * math.pi / 3)]
    ri = r - 46 * 2
    inner = [(cx + ri * math.cos(a), oy + ri * math.sin(a)) for a in (math.pi / 2, math.pi / 2 + 2 * math.pi / 3, math.pi / 2 + 4 * math.pi / 3)]
    return adv, [circle(cx, CY, R), polygon(outer, ccw=False), polygon(inner, ccw=True)]


def bumper(label, adv=820):
    shape = rounded(40, Y0 + 80, adv - 40, Y1 - 80, 60, 160)
    return adv, [shape] + letters(label, adv / 2, CY - 6, 220, adv - 230)


def trigger(label, adv=760):
    shape = rounded(70, Y0, adv - 70, Y1, 60, 240)
    return adv, [shape] + letters(label, adv / 2, CY - 30, 220, adv - 260)


def bars(adv=640, pill=False):
    cx = adv / 2
    base = circle(cx, CY, R) if not pill else rounded(cx - R * 0.80, Y0 + 60, cx + R * 0.80, Y1 - 60, 120)
    hw = R * (0.48 if not pill else 0.40)
    out = [base]
    for dy in (-R * 0.36, 0, R * 0.36):
        out.append(polygon([(cx - hw, CY + dy - 32), (cx + hw, CY + dy - 32), (cx + hw, CY + dy + 32), (cx - hw, CY + dy + 32)], ccw=False))
    return adv, out


def view(adv=640):
    cx, s, g = adv / 2, R * 0.30, R * 0.06
    back = (cx - s - g - s * 0.5, CY + g, cx - g + s * 0.5, CY + g + 2 * s)   # upper left window
    front = (cx + g - s * 0.5, CY - g - 2 * s, cx + s + g + s * 0.5, CY - g)  # lower right window
    def box(x0, y0, x1, y1):
        return polygon([(x0, y0), (x1, y0), (x1, y1), (x0, y1)], ccw=False)
    return adv, [circle(cx, CY, R), box(*back), box(*front)]


def share(adv=640):   # PlayStation 4 Share: a small key with an arrow pointing up
    cx = adv / 2
    base = rounded(cx - R * 0.62, Y0 + 30, cx + R * 0.62, Y1 - 30, 110)
    t, h = R * 0.30, R * 0.22
    arrow = [(cx, CY + R * 0.52), (cx + t, CY + R * 0.52 - h - 6), (cx + t * 0.36, CY + R * 0.52 - h - 6),
             (cx + t * 0.36, CY - R * 0.52), (cx - t * 0.36, CY - R * 0.52), (cx - t * 0.36, CY + R * 0.52 - h - 6),
             (cx - t, CY + R * 0.52 - h - 6)]
    return adv, [base, polygon(arrow, ccw=False)]


def create(adv=640):   # PlayStation 5 Create: a small key with three short rays
    cx = adv / 2
    base = rounded(cx - R * 0.62, Y0 + 30, cx + R * 0.62, Y1 - 30, 110)
    out = [base]
    w, l0, l1 = R * 0.075, R * 0.08, R * 0.50
    oy = CY - R * 0.24
    for ang in (math.pi / 2, math.pi / 2 + 0.85, math.pi / 2 - 0.85):
        ux, uy = math.cos(ang), math.sin(ang)
        px, py = -uy, ux
        out.append(polygon([(cx + ux * l0 - px * w, oy + uy * l0 - py * w), (cx + ux * l1 - px * w, oy + uy * l1 - py * w),
                            (cx + ux * l1 + px * w, oy + uy * l1 + py * w), (cx + ux * l0 + px * w, oy + uy * l0 + py * w)], ccw=False))
    return adv, out


ICONS = [
    ('PS_CROSS', ps_cross), ('PS_CIRCLE', ps_circle), ('PS_SQUARE', ps_square), ('PS_TRIANGLE', ps_triangle),
    ('XB_LB', lambda: bumper('LB')), ('XB_RB', lambda: bumper('RB')), ('XB_LT', lambda: trigger('LT')),
    ('XB_RT', lambda: trigger('RT')), ('XB_MENU', bars), ('XB_VIEW', view),
    ('PS_L1', lambda: bumper('L1')), ('PS_R1', lambda: bumper('R1')), ('PS_L2', lambda: trigger('L2')),
    ('PS_R2', lambda: trigger('R2')), ('PS_OPTIONS', lambda: bars(pill=True)), ('PS4_SHARE', share),
    ('PS5_CREATE', create),
]


def emit(contours):
    """int16 ops: 1 x y = move, 2 x y = line, 3 x1 y1 x2 y2 x y = curve, 4 = close; 0 = end."""
    out = []
    for c in contours:
        out += [1, round(c[0][0]), round(c[0][1])]
        for seg in c[1:]:
            if seg[0] == 'L':
                out += [2, round(seg[1][0]), round(seg[1][1])]
            else:
                out += [3] + [round(v) for p in seg[1:] for v in p]
        out.append(4)
    out.append(0)
    return out


def main():
    lines = ['/* [UIT 0452] Button icon outlines for src/host/prompts.c. Written by tools/gen_prompt_icons.py: do not edit.',
             ' * Our own drawings, letters and digits included (no font or game file is read).',
             ' * Units of the game\'s button font (1000 to the em, y up); ops: 1 x y move, 2 x y line, 3 x1 y1 x2 y2 x y curve,',
             ' * 4 close, 0 end. */',
             '#ifndef UIT_PROMPT_ICONS_H', '#define UIT_PROMPT_ICONS_H', '#include <stdint.h>', '']
    lines.append('enum {')
    for name, _ in ICONS:
        lines.append(f'    PI_{name},')
    lines.append('    PI_COUNT')
    lines.append('};')
    lines.append('')
    tables, advs = [], []
    for name, fn in ICONS:
        adv, cs = fn()
        ops = emit(cs)
        advs.append(adv)
        body = ', '.join(str(v) for v in ops)
        tables.append(f'static const int16_t pi_{name.lower()}[] = {{{body}}};')
    import textwrap
    for t in tables:
        lines += textwrap.wrap(t, 118, subsequent_indent='    ', break_long_words=False, break_on_hyphens=False)
    lines.append('')
    lines.append('static const struct { const char *name; const int16_t *ops; int16_t advance; } kPromptIcons[PI_COUNT] = {')
    for (name, _), adv in zip(ICONS, advs):
        lines.append(f'    {{"{name}", pi_{name.lower()}, {adv}}},')
    lines.append('};')
    lines.append('#endif')
    OUT.write_text('\n'.join(lines) + '\n')
    print(f'wrote {OUT.relative_to(ROOT)}: {len(ICONS)} icons')


if __name__ == '__main__':
    main()
