"""[UIT 0432] Damaged build files: find them before the link and make them again.

A hard power cut (or a crash of the whole PC) while the game is compiled can leave object files that ninja thinks are
finished: clang writes each one to a temporary file and renames it into place, and ninja records it in build/.ninja_log,
but Windows keeps the written data in memory for a while. When the power goes, the name and the size can survive while
the data does not: the file reads as zeros. lld takes such a file as an empty object without an error, so the link then
fails with thousands of undefined symbols (main_fn_addrs, f_8...), and every later run fails the same way, because ninja
still has the file as up to date.

tools/build_game.py therefore, after every compile and before the link:
  * checks each object's header and layout (COFF, big COFF, LLVM bitcode for ThinLTO, ELF for the tests) unless
    build/obj-records.json already vouches for it (same size and time as when it was checked);
  * flushes each newly checked object to the disk (os.fsync) before it is recorded, so a recorded object survives a
    power cut;
  * deletes the bad ones: ninja then builds them again (a missing output is always out of date).
After a failed link, undefined_objects() names the objects that define the missing symbols (from the generated C), so
they can be deleted and built once more.

usage (by hand): python tools/obj_check.py OBJECT...   (prints each file's verdict)
"""
import bisect
import json
import os
import pathlib
import re
import struct

RECORDS = 'obj-records.json'
COFF_MACHINES = (0x8664, 0xAA64, 0x14C)
BIGOBJ_CLASS = bytes.fromhex('c7a1bad1eebaa94baf20faf66aa4dcb8')   # ANON_OBJECT_HEADER_BIGOBJ ClassID


def check(path):
    """None when the object looks whole, else why not (a short reason, for the log)."""
    try:
        with open(path, 'rb') as f:
            head = f.read(64)
            size = os.fstat(f.fileno()).st_size
            if size == 0:
                return 'empty'
            if not head.strip(b'\0'):
                f.seek(max(0, size - 4096))
                if not f.read().strip(b'\0'):
                    return 'only zeros'
            if head[:4] == b'BC\xc0\xde':   # LLVM bitcode (-flto=thin)
                return None if size % 4 == 0 and size >= 64 else 'bitcode cut short'
            if head[:4] == b'\xde\xc0\x17\x0b':   # bitcode wrapper
                off, n = struct.unpack_from('<II', head, 8)
                return None if off + n <= size and n else 'bitcode cut short'
            if head[:4] == b'\x7fELF':
                return _elf(f, head, size)
            if head[:4] == b'\0\0\xff\xff' and head[12:28] == BIGOBJ_CLASS:
                machine, = struct.unpack_from('<H', head, 6)
                nsec, symptr, nsym = struct.unpack_from('<III', head, 44)
                return _coff(f, size, machine, nsec, 56, symptr, nsym, 20)
            machine, nsec = struct.unpack_from('<HH', head, 0)
            symptr, nsym = struct.unpack_from('<II', head, 8)
            optsize, = struct.unpack_from('<H', head, 16)
            return _coff(f, size, machine, nsec, 20 + optsize, symptr, nsym, 18)
    except OSError as e:
        return f'unreadable ({e.strerror or e})'
    except struct.error:
        return 'cut short'


def _coff(f, size, machine, nsec, sect_at, symptr, nsym, symsize):
    if machine not in COFF_MACHINES:
        return 'not an object file'
    if not nsec or sect_at + 40 * nsec > size:
        return 'no sections'
    f.seek(sect_at)
    table = f.read(40 * nsec)
    for i in range(nsec):
        raw_size, raw_ptr = struct.unpack_from('<II', table, i * 40 + 16)
        if raw_ptr and raw_ptr + raw_size > size:
            return 'cut short'
    if not nsym or symptr + nsym * symsize + 4 > size:
        return 'no symbols' if not nsym else 'cut short'
    f.seek(symptr + nsym * symsize)
    strsize, = struct.unpack('<I', f.read(4))
    if strsize == 0 and symptr + nsym * symsize + 4 == size:   # no long names (llvm-windres writes 0)
        return None
    if strsize < 4 or symptr + nsym * symsize + strsize > size:
        return 'cut short'
    if symptr + nsym * symsize + strsize == size:   # clang writes the string table last: its end is the file's end
        f.seek(size - min(strsize, 64))
        tail = f.read()
        if strsize > 8 and not tail.strip(b'\0'):
            return 'end of the file is zeros'
    return None


def _elf(f, head, size):
    if head[4] != 2:
        return 'not a 64-bit object'
    shoff, = struct.unpack_from('<Q', head, 0x28)
    shentsize, shnum = struct.unpack_from('<HH', head, 0x3A)
    if not shnum or shoff + shentsize * shnum > size:
        return 'cut short'
    f.seek(shoff)
    table = f.read(shentsize * shnum)
    for i in range(shnum):
        typ, = struct.unpack_from('<I', table, i * shentsize + 4)
        off, sz = struct.unpack_from('<QQ', table, i * shentsize + 0x18)
        if typ != 8 and off + sz > size:   # (SHT_NOBITS has no data)
            return 'cut short'
    return None


def load_records(build):
    try:
        return json.loads((pathlib.Path(build) / RECORDS).read_text())
    except (OSError, ValueError):
        return {}


def save_records(build, rec):
    p = pathlib.Path(build) / RECORDS
    tmp = p.with_name(p.name + '.new')
    with open(tmp, 'w') as f:
        json.dump(rec, f, separators=(',', ':'))
        f.flush()
        os.fsync(f.fileno())
    os.replace(tmp, p)


def _stamp(st):
    return [st.st_size, st.st_mtime_ns]


def _flush(path):
    """Data on the disk, not only in Windows' cache (FlushFileBuffers needs a handle that may write)."""
    fd = os.open(path, os.O_RDWR | getattr(os, 'O_BINARY', 0))
    try:
        os.fsync(fd)
    finally:
        os.close(fd)


def verify(root, build, objs, jobs=8, keep=False):
    """Checks objs (paths relative to root) not vouched for by the records; flushes and records the good ones,
    deletes the bad ones. Returns [(object, reason)] of the bad ones. keep: the bad ones were just made again by the
    compiler itself, so they stay and are recorded (should this check ever be wrong about a file, the build goes on
    and the link decides; it never loops)."""
    from concurrent.futures import ThreadPoolExecutor
    root = pathlib.Path(root)
    rec = load_records(build)
    todo = []
    for o in objs:
        try:
            st = (root / o).stat()
        except OSError:
            rec.pop(o, None)   # missing: ninja builds it
            continue
        if rec.get(o) != _stamp(st):
            todo.append(o)

    def one(o):
        p = root / o
        why = check(p)
        if why and not keep:
            return o, why, None
        try:
            _flush(p)
            return o, None, _stamp(p.stat())
        except OSError as e:
            return o, None, None   # good, only not flushed: checked again next time
    bad = []
    with ThreadPoolExecutor(max(1, jobs)) as ex:
        for o, why, st in ex.map(one, todo):
            if why and keep:
                bad.append((o, why))
            if why and not keep:
                rec.pop(o, None)
                bad.append((o, why))
                try:
                    (root / o).unlink()
                except OSError:
                    pass
            if st:
                rec[o] = st
    save_records(build, rec)
    return bad


def forget(build, objs):
    rec = load_records(build)
    for o in objs:
        rec.pop(o, None)
    save_records(build, rec)


UNDEF = re.compile(r'undefined symbol: (\S+)')
OBJ_IN_ERROR = re.compile(r'((?:build/(?:game|obj)/)\S+?\.o)\b')
FIRST_DEF = re.compile(rb'^void f_([0-9a-f]+)\w*\(Cpu \*(?:__restrict )?c\) \{', re.M)
MODULE_PREFIX = re.compile(r'^(main|subsdk0|sdk)_')


def _first_address(c):
    with open(c, 'rb') as f:
        while True:
            block = f.read(1 << 18)
            if not block:
                return None
            m = FIRST_DEF.search(block)
            if m:
                return int(m.group(1), 16)
            if len(block) < (1 << 18):
                return None
            f.seek(-200, 1)


def undefined_objects(link_output, generated, modules=('main', 'subsdk0', 'sdk')):
    """The objects to make again after a failed link: build/game/<module>/<module>_table.o for a missing
    <module>_... symbol, the object of the generated file that holds a missing f_<address>, and any object an error
    line names as itself broken (not as the file that refers to a symbol). Returns (objects, unplaced symbols)."""
    generated = pathlib.Path(generated)
    syms, out = set(), set()
    for line in link_output.splitlines():
        m = UNDEF.search(line)
        if m:
            syms.add(m.group(1).strip('`\''))
        elif 'error' in line and 'referenced by' not in line:
            for p in OBJ_IN_ERROR.findall(line.replace('\\', '/')):
                out.add(p)
    index = {}
    unplaced = []
    for s in sorted(syms):
        m = MODULE_PREFIX.match(s)
        if m and m.group(1) in modules:
            out.add(f'build/game/{m.group(1)}/{m.group(1)}_table.o')
            continue
        f = re.match(r'f_([0-9a-f]+)', s)
        if not f:
            unplaced.append(s)
            continue
        a = int(f.group(1), 16)
        placed = False
        for mod in modules:
            if mod not in index:
                rows = []
                for c in sorted((generated / mod).glob(f'{mod}_[0-9]*.c')):
                    first = _first_address(c)
                    if first is not None:
                        rows.append((first, c.stem))
                rows.sort()
                index[mod] = rows
            rows = index[mod]
            i = bisect.bisect_right([r[0] for r in rows], a) - 1
            if i >= 0 and (i + 1 < len(rows) or a - rows[i][0] < 1 << 26):
                if rows and rows[0][0] >> 26 == a >> 26:   # the module's own address range
                    out.add(f'build/game/{mod}/{rows[i][1]}.o')
                    placed = True
                    break
        if not placed:
            unplaced.append(s)
    return sorted(out), unplaced


if __name__ == '__main__':
    import sys
    for p in sys.argv[1:]:
        print(f'{p}: {check(p) or "ok"}')
