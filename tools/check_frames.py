#!/usr/bin/env python3
"""[UIT 0121] Stack frame sizes of x64 Windows code, read from its unwind data: every function's prologue
allocation (UWOP_ALLOC_SMALL / UWOP_ALLOC_LARGE) plus its pushes, from .pdata / .xdata.

    python tools/check_frames.py [--limit 1MiB] [--top 10] [--warn] PATH...

PATH: COFF objects (.o / .obj, also /bigobj), folders of them (searched for *.o and *.obj), or a PE image (.exe /
.dll). Objects give function names; an image gives exe+RVA, the form the crash report prints. LLVM bitcode objects
(ThinLTO) and ELF objects are skipped (counted): check the linked exe for those.
Exit 1 when a function's frame is above --limit (--warn: report only, exit 0); 2 on a bad argument.

Why: a lifted function compiled at -O0 keeps every temporary in its own stack slot. One in the sdk module needed
8.4 MiB, more than the 8 MiB committed thread stack, and ___chkstk_ms faulted below it (the 0x80559cc20 crash,
UIT 0121). tools/build_game.py runs this after each build; tools/tests/check_windows_toolchain.py runs it on
lifter-shaped stand-ins with the builder's real toolchain and fails above 1 MiB.
"""
import argparse
import pathlib
import struct
import sys

BIGOBJ_CLASSID = bytes.fromhex('c7a1bad1eebaa94baf20faf66aa4dcb8')
AMD64 = 0x8664
ADDR32NB = 3


def parse_size(s):
    s = s.strip().lower()
    for suf, mul in (('kib', 1 << 10), ('mib', 1 << 20), ('gib', 1 << 30), ('k', 1 << 10), ('m', 1 << 20)):
        if s.endswith(suf):
            return int(float(s[:-len(suf)]) * mul)
    return int(s, 0)


def unwind_frame(data, off):
    """(bytes the prologue allocates incl. pushes, chained?) from an UNWIND_INFO at data[off:]."""
    if off + 4 > len(data):
        return None
    flags = data[off] >> 3
    n = data[off + 2]
    codes = data[off + 4: off + 4 + 2 * n]
    size, i = 0, 0
    while i < n:
        op, info = codes[2 * i + 1] & 15, codes[2 * i + 1] >> 4
        if op == 0:                          # UWOP_PUSH_NONVOL
            size += 8; i += 1
        elif op == 1:                        # UWOP_ALLOC_LARGE
            if info == 0:
                size += struct.unpack_from('<H', codes, 2 * i + 2)[0] * 8; i += 2
            else:
                size += struct.unpack_from('<I', codes, 2 * i + 2)[0]; i += 3
        elif op == 2:                        # UWOP_ALLOC_SMALL
            size += info * 8 + 8; i += 1
        elif op in (3, 10):                  # UWOP_SET_FPREG, UWOP_PUSH_MACHFRAME
            i += 1
        elif op in (4, 8):                   # UWOP_SAVE_NONVOL, UWOP_SAVE_XMM128
            i += 2
        elif op in (5, 9):                   # UWOP_SAVE_NONVOL_FAR, UWOP_SAVE_XMM128_FAR
            i += 3
        elif op == 6:                        # UWOP_EPILOG (version 2)
            i += 2 if data[off] & 7 >= 2 else 1
        else:
            i += 1
    return size, bool(flags & 4)


def coff_frames(path, data):
    """[(name, frame bytes)] of one COFF object, or None when it is not an x64 COFF object."""
    if len(data) < 20:
        return None
    sig1, sig2 = struct.unpack_from('<HH', data, 0)
    if sig1 == 0 and sig2 == 0xFFFF and data[12:28] == BIGOBJ_CLASSID:
        machine = struct.unpack_from('<H', data, 6)[0]
        nsec, symptr, nsym = struct.unpack_from('<III', data, 44)
        sech, symsz, secnum_fmt = 56, 20, '<i'
    else:
        machine, nsec = struct.unpack_from('<HH', data, 0)
        symptr, nsym, opt = struct.unpack_from('<IIH', data, 8)
        if opt:
            return None
        sech, symsz, secnum_fmt = 20, 18, '<h'
    if machine != AMD64:
        return None
    strtab = symptr + nsym * symsz

    def name8(raw):
        if raw[:4] == b'\0\0\0\0':
            o = strtab + struct.unpack_from('<I', raw, 4)[0]
            return data[o:data.index(b'\0', o)].decode('utf-8', 'replace')
        return raw.rstrip(b'\0').decode('utf-8', 'replace')

    secs = []
    for k in range(nsec):
        h = sech + 40 * k
        nm = data[h:h + 8]
        if nm.startswith(b'/'):
            o = strtab + int(nm[1:].rstrip(b'\0'))
            nmS = data[o:data.index(b'\0', o)].decode('utf-8', 'replace')
        else:
            nmS = nm.rstrip(b'\0').decode('utf-8', 'replace')
        rawsz, rawptr, relptr = struct.unpack_from('<III', data, h + 16)
        nrel = struct.unpack_from('<H', data, h + 32)[0]
        ch = struct.unpack_from('<I', data, h + 36)[0]
        if ch & 0x01000000 and nrel == 0xFFFF:   # IMAGE_SCN_LNK_NRELOC_OVFL: the real count is the first relocation
            nrel = struct.unpack_from('<I', data, relptr)[0]
            relptr += 10; nrel -= 1
        secs.append((nmS, rawptr, rawsz, relptr, nrel))
    syms, names_at = {}, {}
    k = 0
    while k < nsym:
        o = symptr + k * symsz
        nm = name8(data[o:o + 8])
        value = struct.unpack_from('<I', data, o + 8)[0]
        secnum = struct.unpack_from(secnum_fmt, data, o + 12)[0]
        typ = struct.unpack_from('<H', data, o + 12 + (4 if symsz == 20 else 2))[0]
        sclass = data[o + symsz - 2]
        naux = data[o + symsz - 1]
        syms[k] = (nm, secnum, value, sclass)
        if secnum > 0 and sclass in (2, 3) and (typ == 0x20 or sclass == 2) and not nm.startswith(('.', '$')):
            names_at.setdefault((secnum, value), nm)
        k += 1 + naux

    def relocs(si):
        nmS, rawptr, rawsz, relptr, nrel = secs[si]
        out = {}
        for r in range(nrel):
            va, symi, typ = struct.unpack_from('<IIH', data, relptr + 10 * r)
            if typ == ADDR32NB:
                out[va] = symi
        return out

    res = []
    for si, (nmS, rawptr, rawsz, relptr, nrel) in enumerate(secs):
        if not nmS.startswith('.pdata'):
            continue
        rel = relocs(si)
        pd = data[rawptr:rawptr + rawsz]
        for e in range(0, len(pd) - 11, 12):
            begin, _end, unw = struct.unpack_from('<III', pd, e)
            if e not in rel or e + 8 not in rel:
                continue
            fs = syms.get(rel[e]); us = syms.get(rel[e + 8])
            if not fs or not us or us[1] <= 0:
                continue
            fname = fs[0] if fs[3] == 2 or not fs[0].startswith('.') else None
            if fname is None or fs[0].startswith('.'):
                fname = names_at.get((fs[1], fs[2] + begin)) or f'{fs[0]}+{fs[2] + begin:#x}'
            xs = secs[us[1] - 1]
            xd = data[xs[1]:xs[1] + xs[2]]
            fr = unwind_frame(xd, us[2] + unw)
            if fr is None or fr[1]:
                continue
            res.append((fname, fr[0]))
    return res


def pe_frames(path, data):
    """[(exe+RVA, frame bytes)] of a PE32+ image from its exception directory."""
    pe = struct.unpack_from('<I', data, 0x3c)[0]
    if data[pe:pe + 4] != b'PE\0\0':
        return None
    machine, nsec = struct.unpack_from('<HH', data, pe + 4)
    optsz = struct.unpack_from('<H', data, pe + 20)[0]
    opt = pe + 24
    if machine != AMD64 or struct.unpack_from('<H', data, opt)[0] != 0x20b:
        return None
    exc_rva, exc_size = struct.unpack_from('<II', data, opt + 112 + 3 * 8)
    secs = []
    for k in range(nsec):
        h = opt + optsz + 40 * k
        vsz, va, rawsz, rawptr = struct.unpack_from('<IIII', data, h + 8)
        secs.append((va, max(vsz, rawsz), rawptr, rawsz))

    def at(rva):
        for va, sz, rawptr, rawsz in secs:
            if va <= rva < va + sz:
                return rawptr + (rva - va) if rva - va < rawsz else None
        return None
    res = []
    base = at(exc_rva)
    if base is None:
        return res
    for e in range(0, exc_size - 11, 12):
        begin, _end, unw = struct.unpack_from('<III', data, base + e)
        o = at(unw)
        if o is None:
            continue
        fr = unwind_frame(data, o)
        if fr and not fr[1]:
            res.append((f'exe+{begin:x}', fr[0]))
    return res


def frames_of(path):
    """(kind, [(name, bytes)]) for one file; kind 'skipped:<why>' when it has no x64 COFF/PE unwind data."""
    data = pathlib.Path(path).read_bytes()
    if data[:4] == b'BC\xc0\xde' or data[:4] == b'\xde\xc0\x17\x0b':
        return 'skipped:bitcode', []
    if data[:4] == b'\x7fELF':
        return 'skipped:elf', []
    if data[:2] == b'MZ':
        r = pe_frames(path, data)
        return ('image', r) if r is not None else ('skipped:not x64', [])
    r = coff_frames(path, data)
    return ('object', r) if r is not None else ('skipped:not x64 COFF', [])


def collect(paths):
    files = []
    for p in map(pathlib.Path, paths):
        if p.is_dir():
            files += sorted(q for q in p.rglob('*') if q.suffix.lower() in ('.o', '.obj') and q.is_file())
        else:
            files.append(p)
    return files


def check(paths, limit, top=10, out=print):
    """Returns (functions over the limit [(bytes, name, file)], every [(bytes, name, file)], skipped {why: n})."""
    allf, skipped = [], {}
    for f in collect(paths):
        kind, fr = frames_of(f)
        if kind.startswith('skipped'):
            skipped[kind[8:]] = skipped.get(kind[8:], 0) + 1
            continue
        allf += [(b, n, str(f)) for n, b in fr]
    allf.sort(reverse=True)
    over = [x for x in allf if x[0] > limit]
    return over, allf, skipped


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('paths', nargs='+')
    ap.add_argument('--limit', default='1MiB', help='largest frame allowed (bytes, or 512KiB / 1MiB ...)')
    ap.add_argument('--top', type=int, default=10, help='list this many of the biggest frames')
    ap.add_argument('--warn', action='store_true', help='report only: exit 0 even above the limit')
    a = ap.parse_args(argv)
    try:
        limit = parse_size(a.limit)
    except ValueError:
        print(f'check_frames: bad --limit {a.limit!r}')
        return 2
    over, allf, skipped = check(a.paths, limit)
    sk = ', '.join(f'{n} {k}' for k, n in sorted(skipped.items()))
    print(f'check_frames: {len(allf)} functions, largest frame {allf[0][0] if allf else 0:,} bytes, limit {limit:,}'
          + (f' ({sk} file(s) skipped)' if sk else ''))
    for b, n, f in allf[:a.top]:
        print(f'  {b:>12,}  {n}  ({f})')
    if over:
        print(f'check_frames: {len(over)} function(s) above {limit:,} bytes'
              + (' (warning only)' if a.warn else ''))
        return 0 if a.warn else 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
