#!/usr/bin/env python3
"""Ultra Instinct Tears icon generator: writes SVG sources per detail tier and renders PNGs + icon.ico."""
import io, os, struct
import resvg_py
from PIL import Image

OUT = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(OUT, 'src')
BASE_TOP, BASE_BOT, EDGE = '#1b232b', '#12171c', '#2a343e'
CYAN, CORE_LIGHT, GOLD, GOLD_DIM = '#3fe0ff', '#e8fdff', '#c9a45a', '#8a7344'

def rot(elems):
    """Repeat a group 4x around the centre (128,128)."""
    return ''.join(f'<g transform="rotate({a} 128 128)">{elems}</g>' for a in (0, 90, 180, 270))

def full_svg(weight=1.0):
    """weight>1 thickens strokes for the 64 px render."""
    import re
    W = lambda s: re.sub(r'stroke-width="([\d.]+)"', lambda m: f'stroke-width="{float(m.group(1))*weight:g}"', s)
    # one arm (pointing up); rotated 4x
    arm = f'''
    <path d="M128 92 V40" stroke="{CYAN}" stroke-width="6" stroke-linecap="square" fill="none"/>
    <rect x="120" y="26" width="16" height="16" rx="2" fill="none" stroke="{CYAN}" stroke-width="4"/>
    <path d="M104 74 V58 L88 42 H62" stroke="{GOLD}" stroke-width="5" fill="none" stroke-linejoin="miter" stroke-linecap="butt"/>
    <path d="M152 74 V58 L168 42 H194" stroke="{GOLD}" stroke-width="5" fill="none" stroke-linejoin="miter" stroke-linecap="butt"/>
    <circle cx="57" cy="42" r="6" fill="none" stroke="{GOLD}" stroke-width="4"/>
    <circle cx="199" cy="42" r="6" fill="none" stroke="{GOLD}" stroke-width="4"/>
    '''
    return f'''<svg xmlns="http://www.w3.org/2000/svg" width="256" height="256" viewBox="0 0 256 256">
  <defs>
    <linearGradient id="base" x1="0" y1="0" x2="0" y2="1">
      <stop offset="0" stop-color="{BASE_TOP}"/><stop offset="1" stop-color="{BASE_BOT}"/>
    </linearGradient>
    <linearGradient id="rim" x1="0" y1="0" x2="0" y2="1">
      <stop offset="0" stop-color="#4a5866"/><stop offset="0.5" stop-color="{EDGE}"/><stop offset="1" stop-color="#0b0f12"/>
    </linearGradient>
    <linearGradient id="core" x1="0" y1="0" x2="1" y2="1">
      <stop offset="0" stop-color="#8af0ff"/><stop offset="1" stop-color="#22b8e0"/>
    </linearGradient>
    <radialGradient id="light"><stop offset="0" stop-color="#ffffff"/><stop offset="0.6" stop-color="{CORE_LIGHT}"/><stop offset="1" stop-color="{CORE_LIGHT}" stop-opacity="0"/></radialGradient>
    <filter id="glow" x="-1" y="-1" width="3" height="3"><feGaussianBlur stdDeviation="14"/></filter>
    <filter id="soft" x="-1" y="-1" width="3" height="3"><feGaussianBlur stdDeviation="3"/></filter>
    <clipPath id="clip"><rect x="8" y="8" width="240" height="240" rx="46"/></clipPath>
  </defs>
  <rect x="8" y="8" width="240" height="240" rx="46" fill="url(#rim)"/>
  <rect x="14" y="14" width="228" height="228" rx="41" fill="url(#base)"/>
  <rect x="22" y="22" width="212" height="212" rx="34" fill="none" stroke="#0c1014" stroke-width="2" opacity="0.9"/>
  <g clip-path="url(#clip)">
    <rect x="88" y="88" width="80" height="80" rx="10" fill="{CYAN}" opacity="0.55" filter="url(#glow)"/>
    {W(rot(arm))}
    <rect x="80" y="80" width="96" height="96" rx="6" fill="none" stroke="{GOLD}" stroke-width="{5*weight:g}"/>
    <rect x="100" y="100" width="56" height="56" rx="5" fill="url(#core)"/>
    <rect x="108" y="108" width="40" height="40" rx="3" fill="none" stroke="#0f6f88" stroke-width="2.5" opacity="0.8"/>
    <circle cx="128" cy="128" r="16" fill="url(#light)" filter="url(#soft)"/>
    <rect x="122" y="122" width="12" height="12" rx="2" fill="{CORE_LIGHT}"/>
  </g>
</svg>'''

def mid_svg(n):
    """32/40/48 px: pixel-aligned, a few circuit traces."""
    S = {48: dict(inset=1, r=9, ring=(13, 35), core=(17, 31), light=(22, 26), p=3, node=4, diag=(10, 8.5)),
         40: dict(inset=1, r=8, ring=(11, 29), core=(14, 26), light=(18, 22), p=3, node=4, diag=(9, 7.5)),
         32: dict(inset=0, r=6, ring=(8, 24), core=(11, 21), light=(15, 17), p=1, node=4, diag=None)}[n]
    inset, r, p, node = S['inset'], S['r'], S['p'], S['node']
    a, b = S['ring']; k, l = S['core']; m, o = S['light']
    c = n // 2
    g = ''
    # cyan stubs + square nodes on the four axes
    e = p + node
    for (x, y, w, h) in [(c-1, e, 2, a-e), (c-1, b, 2, n-e-b), (e, c-1, a-e, 2), (b, c-1, n-e-b, 2)]:
        g += f'<rect x="{x}" y="{y}" width="{w}" height="{h}" fill="{CYAN}"/>'
    if n == 32:  # T-shaped terminals: lighter than solid squares at this size
        for (x, y, w, h) in [(c-2, p+2, 4, 2), (c-2, n-p-4, 4, 2), (p+2, c-2, 2, 4), (n-p-4, c-2, 2, 4)]:
            g += f'<rect x="{x}" y="{y}" width="{w}" height="{h}" fill="{CYAN}"/>'
    else:
        for (x, y) in [(c-node//2, p), (c-node//2, n-p-node), (p, c-node//2), (n-p-node, c-node//2)]:
            g += f'<rect x="{x}" y="{y}" width="{node}" height="{node}" fill="{CYAN}"/>'
    # gold diagonal traces from ring corners with ring nodes
    if S['diag']:
        t, q = S['diag']
        for sx, sy in [(1, 1), (-1, 1), (1, -1), (-1, -1)]:
            fx = lambda v: v if sx > 0 else n - v
            fy = lambda v: v if sy > 0 else n - v
            g += (f'<line x1="{fx(a+0.5)}" y1="{fy(a+0.5)}" x2="{fx(t+0.5)}" y2="{fy(t+0.5)}" stroke="{GOLD}" stroke-width="2"/>'
                  f'<circle cx="{fx(q)}" cy="{fy(q)}" r="2" fill="none" stroke="{GOLD}" stroke-width="1.5"/>')
    rg = (f'<rect x="{a}" y="{a}" width="{b-a}" height="{b-a}" fill="{GOLD}"/>'
          f'<rect x="{a+2}" y="{a+2}" width="{b-a-4}" height="{b-a-4}" fill="#161d24"/>')
    return f'''<svg xmlns="http://www.w3.org/2000/svg" width="{n}" height="{n}" viewBox="0 0 {n} {n}">
  <defs><linearGradient id="b" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="{BASE_TOP}"/><stop offset="1" stop-color="{BASE_BOT}"/></linearGradient></defs>
  <rect x="{inset}" y="{inset}" width="{n-2*inset}" height="{n-2*inset}" rx="{r}" fill="#3a4652"/>
  <rect x="{inset+1}" y="{inset+1}" width="{n-2*inset-2}" height="{n-2*inset-2}" rx="{r-1}" fill="url(#b)"/>
  {g}{rg}
  <rect x="{k}" y="{k}" width="{l-k}" height="{l-k}" fill="{CYAN}"/>
  <rect x="{m}" y="{m}" width="{o-m}" height="{o-m}" fill="{CORE_LIGHT}"/>
</svg>'''

def small_svg(n):
    """16/20/24: rounded dark square + bold cyan core + gold ring."""
    spec = {16: dict(r=3, ring=(3, 13), core=(5, 11), light=(7, 9), stub=True),
            20: dict(r=4, ring=(4, 16), core=(7, 13), light=(9, 11), stub=True),
            24: dict(r=4, ring=(5, 19), core=(8, 16), light=(11, 13), stub=True)}[n]
    a, b = spec['ring']; k, l = spec['core']; m, o = spec['light']
    gold = (f'<rect x="{a}" y="{a}" width="{b-a}" height="{b-a}" fill="{GOLD}"/>'
            f'<rect x="{a+1}" y="{a+1}" width="{b-a-2}" height="{b-a-2}" fill="{BASE_TOP}"/>')
    c = n // 2
    stubs = ''
    if spec['stub']:
        # cyan ticks from ring midpoints toward the rim (2px wide, centred)
        for (x, y, w, h) in [(c-1, 1, 2, a-1), (c-1, b, 2, n-1-b), (1, c-1, a-1, 2), (b, c-1, n-1-b, 2)]:
            stubs += f'<rect x="{x}" y="{y}" width="{w}" height="{h}" fill="{CYAN}" opacity="0.75"/>'
    return f'''<svg xmlns="http://www.w3.org/2000/svg" width="{n}" height="{n}" viewBox="0 0 {n} {n}" shape-rendering="crispEdges">
  <rect x="0" y="0" width="{n}" height="{n}" rx="{spec['r']}" fill="#3a4652" shape-rendering="geometricPrecision"/>
  <rect x="1" y="1" width="{n-2}" height="{n-2}" rx="{spec['r']-1}" fill="{BASE_TOP}" shape-rendering="geometricPrecision"/>
  {gold}{stubs}
  <rect x="{k}" y="{k}" width="{l-k}" height="{l-k}" fill="{CYAN}"/>
  <rect x="{m}" y="{m}" width="{o-m}" height="{o-m}" fill="{CORE_LIGHT}"/>
</svg>'''

def render(svg, size):
    png = bytes(resvg_py.svg_to_bytes(svg_string=svg, width=size, height=size))
    return Image.open(io.BytesIO(png)).convert('RGBA')

def svg_for(size):
    if size <= 24: return small_svg(size if size in (16, 20, 24) else 24)
    if size <= 40: return mid_svg(32) if size == 32 else None
    if size == 48: return mid_svg(48)
    return full_svg()

def main():
    os.makedirs(SRC, exist_ok=True)
    srcs = {'icon-full.svg': full_svg(), 'icon-64.svg': full_svg(1.45), 'icon-48.svg': mid_svg(48), 'icon-40.svg': mid_svg(40), 'icon-32.svg': mid_svg(32),
            'icon-24.svg': small_svg(24), 'icon-20.svg': small_svg(20), 'icon-16.svg': small_svg(16)}
    for name, s in srcs.items():
        open(os.path.join(SRC, name), 'w').write(s)
    imgs = {}
    for s in (16, 20, 24, 32, 40, 48, 64, 128, 256, 512, 1024):
        if s in (16, 20, 24): im = render(small_svg(s), s)
        elif s == 32: im = render(mid_svg(32), s)
        elif s == 40: im = render(mid_svg(40), s)
        elif s == 48: im = render(mid_svg(48), s)
        elif s == 64: im = render(full_svg(1.45), s)
        else: im = render(full_svg(), s)
        imgs[s] = im
        im.save(os.path.join(OUT, f'icon-{s}.png'))
    write_ico(os.path.join(OUT, 'icon.ico'), [imgs[s] for s in (16, 20, 24, 32, 40, 48, 64, 128, 256)])

def write_ico(path, images):
    """ICO with 32-bit BMP (DIB) entries for <256 and PNG for 256 (Vista+ style)."""
    entries, blobs = [], []
    for im in images:
        w, h = im.size
        if w >= 256:
            b = io.BytesIO(); im.save(b, 'PNG'); data = b.getvalue()
        else:
            px = im.tobytes('raw', 'BGRA')
            rows = [px[y*w*4:(y+1)*w*4] for y in range(h)][::-1]
            xor = b''.join(rows)
            mask_stride = ((w + 31) // 32) * 4
            alpha = im.getchannel('A').tobytes()
            mask = b''
            for y in reversed(range(h)):
                bits = bytearray(mask_stride)
                for x in range(w):
                    if alpha[y*w+x] == 0: bits[x//8] |= 0x80 >> (x % 8)
                mask += bytes(bits)
            hdr = struct.pack('<IiiHHIIiiII', 40, w, h*2, 1, 32, 0, len(xor)+len(mask), 0, 0, 0, 0)
            data = hdr + xor + mask
        entries.append((w, h, data)); blobs.append(data)
    off = 6 + 16*len(entries)
    out = struct.pack('<HHH', 0, 1, len(entries))
    for w, h, data in entries:
        out += struct.pack('<BBBBHHII', w % 256, h % 256, 0, 0, 1, 32, len(data), off)
        off += len(data)
    open(path, 'wb').write(out + b''.join(blobs))

if __name__ == '__main__':
    main()
