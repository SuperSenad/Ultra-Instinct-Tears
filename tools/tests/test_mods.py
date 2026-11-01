"""Box tests for tools/mods.py: layouts, names, load order, updates, variants, broken and hostile archives, the inbox,
removal, and that the game's own index (modfs.c) reads back what the installer wrote.

    python3 tools/tests/test_mods.py            (7z cases need py7zr: PY7ZR_PYTHON=/path/to/python with it installed)
"""
import io, json, os, pathlib, shutil, subprocess, sys, tempfile, time, zipfile

HERE = pathlib.Path(__file__).resolve().parent
REPO = HERE.parent.parent
MODS = REPO / 'tools' / 'mods.py'
fails = 0


def check(cond, what):
    global fails
    if not cond:
        fails += 1
        print('FAIL:', what)


def mkzip(path, files):
    with zipfile.ZipFile(path, 'w', zipfile.ZIP_DEFLATED) as z:
        for n, d in files.items():
            z.writestr(n, d if isinstance(d, bytes) else d.encode())
    return path


def run(*args, env=None):
    e = dict(os.environ, **(env or {}))
    r = subprocess.run([sys.executable, str(MODS), *args], capture_output=True, text=True, env=e)
    rows = [l.split('\t') for l in r.stdout.splitlines() if l]
    return r.returncode, rows, r.stderr


def order(md):
    t = (md / 'load-order.txt').read_text()
    return [l for l in t.splitlines() if l and not l.startswith('#')]


def lister():
    exe = pathlib.Path(tempfile.gettempdir()) / 'uit_modfs_list'
    cc = shutil.which('cc') or shutil.which('gcc') or shutil.which('clang')
    if not cc: return None
    r = subprocess.run([cc, '-O1', f'-I{REPO}/src/host', str(HERE / 'modfs_list.c'), str(REPO / 'src/host/modfs.c'), '-o', str(exe)],
                       capture_output=True, text=True)
    return str(exe) if r.returncode == 0 else None


def main():
    t = pathlib.Path(tempfile.mkdtemp(prefix='uit_mods_'))
    md, dl = t / 'mods', t / 'dl'
    dl.mkdir()
    inst = lambda *s, **k: run('install', *map(str, s), '--mods', str(md), **k)

    # 1. <Name>/romfs + info.json: the name from the folder, on top
    z = mkzip(dl / 'BSF_v2.zip', {'Black Screen Fix/romfs/UI/LayoutArchive/Common.Product.110.Nin_NX_NVN.blarc.zs': 'x' * 100,
                                 'Black Screen Fix/info.json': '{"name": "Black Screen Fix", "version": "2"}'})
    rc, rows, err = inst(z, '--progress', str(t / 'p.json'))
    check(rc == 0 and rows == [['installed', 'Black Screen Fix', '1', '100', 'new', '']], f'romfs layout: {rows} {err}')
    check((md / 'Black Screen Fix/romfs/UI/LayoutArchive/Common.Product.110.Nin_NX_NVN.blarc.zs').is_file(), 'romfs placed')
    check('cannot work' in (md / 'README.txt').read_text(), 'mods\\README.txt written')
    check((md / 'Black Screen Fix/info.json').is_file(), 'info.json kept')
    check(json.loads((t / 'p.json').read_text())['state'] == 'ok', 'progress file')

    # 2. bare game folders, a readme beside them; 3. romfs at the top (name from the zip); 4./5. emulator SD layouts
    mkzip(dl / 'Bare.zip', {'Actor/Link.bfres.zs': 'aa', 'readme.txt': 'hi'})
    mkzip(dl / 'Top_Romfs-v1.2.zip', {'romfs/Pack/Npc.pack.zs': 'b'})
    mkzip(dl / 'Atmo.zip', {'atmosphere/contents/0100F2C0115B6000/romfs/Pack/A.pack.zs': 'c'})
    mkzip(dl / 'ryu.zip', {'mods/contents/0100f2c0115b6000/Cool Mod/romfs/Pack/B.pack.zs': 'd'})
    rc, rows, err = inst(dl / 'Bare.zip', dl / 'Top_Romfs-v1.2.zip', dl / 'Atmo.zip', dl / 'ryu.zip')
    check(rc == 0 and [r[1] for r in rows] == ['Bare', 'Top Romfs-v1.2', 'Atmo', 'Cool Mod'], f'layouts: {rows} {err}')
    check((md / 'Bare/romfs/Actor/Link.bfres.zs').is_file() and (md / 'Bare/readme.txt').is_file(), 'bare -> romfs/')
    check(order(md) == ['+Cool Mod', '+Atmo', '+Top Romfs-v1.2', '+Bare', '+Black Screen Fix'], f'new on top: {order(md)}')

    # 6. variants in one archive: both installed, the second off (same file)
    mkzip(dl / 'Pack.zip', {'Pack/30fps/romfs/UI/a.zs': '1', 'Pack/60fps/romfs/UI/a.zs': '2', 'Pack/Extra/romfs/UI/b.zs': '3'})
    rc, rows, err = inst(dl / 'Pack.zip')
    check([(r[1], r[4]) for r in rows] == [('Pack - 30fps', 'new'), ('Pack - 60fps', 'off'), ('Pack - Extra', 'new')], f'variants: {rows}')
    check(order(md)[:3] == ['+Pack - 30fps', '-Pack - 60fps', '+Pack - Extra'], f'variants order: {order(md)}')

    # 7. an update: replaced in place, its place and its off state kept, old files gone
    lo = (md / 'load-order.txt').read_text().replace('+Bare\n', '-Bare\n')
    (md / 'load-order.txt').write_text(lo)
    before = order(md)
    rc, rows, err = inst(mkzip(dl / 'Bare.zip', {'Bare/romfs/Actor/Zelda.bfres.zs': 'zz'}))
    check(rc == 0 and rows[0][1] == 'Bare' and rows[0][4] == 'updated', f'update: {rows}')
    check(order(md) == before, f'update keeps order/state: {order(md)}')
    check(not (md / 'Bare/romfs/Actor/Link.bfres.zs').exists() and (md / 'Bare/romfs/Actor/Zelda.bfres.zs').is_file(), 'update replaced files')

    # 8. a folder: copied, the source left as it was; --name; a non-ASCII name made ASCII
    src = t / 'Mein Möd'
    (src / 'romfs/UI').mkdir(parents=True)
    (src / 'romfs/UI/c.zs').write_text('c')
    rc, rows, err = inst(src)
    check(rc == 0 and rows[0][1] == 'Mein Mod' and (src / 'romfs/UI/c.zs').is_file(), f'folder + ascii: {rows}')
    rc, rows, err = inst(mkzip(dl / 'x.zip', {'romfs/UI/d.zs': 'd'}), '--name', '+#CON')
    check(rows[0][1] == 'CON mod', f'name cleaning: {rows}')

    # 9. hostile and broken archives: nothing written, nothing left behind
    ev = mkzip(dl / 'evil.zip', {'romfs/UI/e.zs': 'e', '../../escaped.txt': 'x'})
    rc, rows, err = inst(ev)
    check(rc == 1 and rows[0][0] == 'error' and 'outside' in rows[0][2], f'zip slip: {rows}')
    check(not (t / 'escaped.txt').exists() and not (md.parent / 'escaped.txt').exists(), 'zip slip wrote nothing')
    data = (dl / 'BSF_v2.zip').read_bytes()
    (dl / 'cut.zip').write_bytes(data[:len(data) // 2])
    rc, rows, err = inst(dl / 'cut.zip')
    check(rc == 1 and 'damaged' in rows[0][2], f'truncated zip: {rows}')
    bad = bytearray(mkzip(dl / 'crc.zip', {'romfs/UI/f.zs': 'f' * 5000}).read_bytes())
    i = bad.find(b'romfs/UI/f.zs') + 30
    bad[i:i + 8] = b'\xff' * 8
    (dl / 'crc.zip').write_bytes(bytes(bad))
    rc, rows, err = inst(dl / 'crc.zip')
    check(rc == 1 and 'damaged' in rows[0][2] and not (md / 'crc').exists(), f'crc error: {rows}')
    rc, rows, err = inst(mkzip(dl / 'code.zip', {'60fps/exefs/main.pchtxt': 'p'}))
    check(rc == 1 and 'code mod' in rows[0][2] and 'exefs' in rows[0][2] and 'Frame rate target' in rows[0][2],
          f'code only refused: {rows}')
    rc, rows, err = inst(mkzip(dl / 'pics.zip', {'shots/a.png': 'p'}))
    check(rc == 1 and 'no mod files' in rows[0][2], f'no mod files: {rows}')
    rc, rows, err = inst(dl / 'nothing.zip')
    check(rc == 1 and 'not found' in rows[0][2], f'missing file: {rows}')
    check(not [p for p in md.iterdir() if p.name.startswith('.inst')], 'no staging folders left')

    # 10. a zip inside the zip
    buf = io.BytesIO()
    with zipfile.ZipFile(buf, 'w') as zz: zz.writestr('Inner Mod/romfs/UI/g.zs', 'g')
    rc, rows, err = inst(mkzip(dl / 'wrapped.zip', {'Inner Mod v3.zip': buf.getvalue(), 'credits.txt': 'x'}))
    check(rc == 0 and rows[0][1] == 'Inner Mod', f'nested zip: {rows} {err}')

    # 11. romfs + exefs: installed, the exefs noted
    rc, rows, err = inst(mkzip(dl / 'mixed.zip', {'Mixed/romfs/UI/h.zs': 'h', 'Mixed/exefs/x.ips': 'i'}))
    check(rc == 0 and 'exefs' in rows[0][5], f'mixed: {rows}')

    # 12. .tar.gz through tar; .7z through a bsdtar stand-in (py7zr); no tool at all
    tg = t / 'tg'
    (tg / 'Tarred/romfs/UI').mkdir(parents=True)
    (tg / 'Tarred/romfs/UI/j.zs').write_text('j')
    subprocess.run(['tar', '-czf', str(dl / 'tarred.tar.gz'), '-C', str(tg), 'Tarred'], check=True)
    rc, rows, err = inst(dl / 'tarred.tar.gz', env={'UIT_MODS_TAR': shutil.which('tar'), 'UIT_MODS_7Z': '-'})
    check(rc == 0 and rows[0][1] == 'Tarred', f'tar.gz: {rows} {err}')
    import tarfile
    with tarfile.open(dl / 'slip.tar', 'w') as tf:
        for nm in ('Slip/romfs/UI/s.zs', '../up.txt'):
            ti = tarfile.TarInfo(nm); ti.size = 1; tf.addfile(ti, io.BytesIO(b's'))
    rc, rows, err = inst(dl / 'slip.tar', env={'UIT_MODS_TAR': shutil.which('tar'), 'UIT_MODS_7Z': '-'})
    check(rc == 1 and 'outside' in rows[0][2] and not (t / 'up.txt').exists() and not (md / 'Slip').exists(), f'tar slip: {rows}')
    py7 = os.environ.get('PY7ZR_PYTHON')
    if py7:
        shim = t / 'fake-bsdtar'
        shim.write_text(f'#!/bin/sh\nexec "{py7}" "{HERE / "bsdtar_7z_shim.py"}" "$@"\n')
        shim.chmod(0o755)
        subprocess.run([py7, '-c', 'import py7zr,sys; z=py7zr.SevenZipFile(sys.argv[1],"w"); z.writestr(b"k","Seven/romfs/UI/k.zs"); '
                        'z.writestr(b"{}", "Seven/info.json"); z.close()', str(dl / 'seven.7z')], check=True)
        rc, rows, err = inst(dl / 'seven.7z', env={'UIT_MODS_TAR': str(shim), 'UIT_MODS_7Z': '-'})
        check(rc == 0 and rows[0][1] == 'Seven' and (md / 'Seven/romfs/UI/k.zs').read_text() == 'k', f'7z: {rows} {err}')
        (dl / 'broken.7z').write_bytes(b"7z\xbc\xaf'\x1c" + b'\0' * 40)
        rc, rows, err = inst(dl / 'broken.7z', env={'UIT_MODS_TAR': str(shim), 'UIT_MODS_7Z': '-'})
        check(rc == 1 and 'could not be opened' in rows[0][2], f'broken 7z: {rows}')
    else:
        print('(7z cases skipped: set PY7ZR_PYTHON)')
    (dl / 'r.rar').write_bytes(b'Rar!\x1a\x07\x00junk')
    rc, rows, err = inst(dl / 'r.rar', env={'UIT_MODS_TAR': '-', 'UIT_MODS_7Z': '-'})
    check(rc == 1 and '7-Zip' in rows[0][2], f'no tool: {rows}')

    # 13. an unlisted folder someone copied in by hand: kept, the new mod still goes above it
    (md / 'Hand Made/romfs/UI').mkdir(parents=True)
    (md / 'Hand Made/romfs/UI/l.zs').write_text('l')
    inst(mkzip(dl / 'Newest.zip', {'Newest/romfs/UI/m.zs': 'm'}))
    check(order(md)[:2] == ['+Newest', '+Hand Made'], f'unlisted kept below the new one: {order(md)[:3]}')

    # 14. the inbox: oldest first (newest ends on top); bad ones to failed/ with the reason; folders moved
    ib = t / 'mods-inbox'
    ib.mkdir()
    mkzip(ib / 'Old One.zip', {'romfs/UI/n.zs': 'n'})
    (ib / 'Folder Mod/romfs/UI').mkdir(parents=True)
    (ib / 'Folder Mod/romfs/UI/o.zs').write_text('o')
    (ib / 'junk.zip').write_bytes(b'PK\3\4 not really')
    now = time.time()
    os.utime(ib / 'Old One.zip', (now - 300, now - 300)), os.utime(ib / 'junk.zip', (now - 200, now - 200))
    rc, rows, err = run('inbox', '--mods', str(md), '--inbox', str(ib))
    check([r[0] for r in rows] == ['installed', 'error', 'installed'], f'inbox: {rows} {err}')
    check(order(md)[:2] == ['+Folder Mod', '+Old One'], f'inbox order: {order(md)[:3]}')
    check((ib / 'installed/Old One.zip').is_file() and (ib / 'failed/junk.zip').is_file() and (ib / 'failed/junk.zip.txt').is_file(),
          'inbox moved items')
    check(not (ib / 'Folder Mod').exists() and (md / 'Folder Mod/romfs/UI/o.zs').is_file(), 'inbox folder moved in')
    check((ib / 'README.txt').is_file(), 'inbox readme')
    rc, rows, err = run('inbox', '--mods', str(md), '--inbox', str(ib))
    check(rows == [['empty', str(ib)]], f'inbox empty: {rows}')

    # 15a. naming through emulator / version folders; RomFS in capitals counted; code patches anywhere explained
    rc, rows, err = inst(mkzip(dl / 'pack.zip', {'TOTK Mods/1.4.x/Fog Remover/Yuzu/romfs/UI/fog.zs': 'f',
                                                'TOTK Mods/1.4.x/Fog Remover/Ryujinx/romfs/UI/fog.zs': 'f'}))
    check(rc == 0 and [r[1] for r in rows] == ['Fog Remover'], f'emulator copies: one mod, named past them: {rows} {err}')
    rc, rows, err = inst(mkzip(dl / 'Sky.zip', {'Sky/Yuzu/romfs/UI/s.zs': 'a', 'Sky/Ryujinx/romfs/UI/s.zs': 'b'}))
    check(rc == 0 and [r[1] for r in rows] == ['Sky (Ryujinx)', 'Sky (Yuzu)'] and rows[1][4] == 'off',
          f'different emulator copies stay variants, named by the folder: {rows} {err}')
    caps = dl / 'Caps Mod'
    (caps / 'RomFS/UI').mkdir(parents=True)
    (caps / 'RomFS/UI/c.zs').write_text('cc')
    rc, rows, err = inst(caps)
    check(rc == 0 and rows[0][1:4] == ['Caps Mod', '1', '2'], f'RomFS in capitals counted: {rows} {err}')
    for nm, files, kind in (('loose pchtxt.zip', {'60fps.pchtxt': 'p'}, '.pchtxt'),
                            ('atmo ips.zip', {'atmosphere/exefs_patches/fps/ABCD.ips': 'i'}, 'exefs'),
                            ('cheat.zip', {'0100F2C0115B6000/cheats/ABCD.txt': 'c'}, 'cheat')):
        rc, rows, err = inst(mkzip(dl / nm, files))
        check(rc == 1 and 'code mod' in rows[0][2] and kind in rows[0][2] and 'recompiled' in rows[0][2], f'{nm}: {rows}')
    (dl / 'x.ips').write_text('i')
    rc, rows, err = inst(dl / 'x.ips')
    check(rc == 1 and 'code patch' in rows[0][2], f'a dropped .ips: {rows}')

    # 15b. loose game files (no folders): placed where the game keeps the file of that name, from the game's file list
    import struct
    game = t / 'game-romfs'
    game.mkdir()
    gp = sorted(['UI/LayoutArchive/Common.Product.110.Nin_NX_NVN.blarc.zs', 'UI/LayoutArchive/Title.Product.110.Nin_NX_NVN.blarc.zs',
                 'Model/A/Dup.bfres.zs', 'Model/B/Dup.bfres.zs'])
    st, en = bytearray(), bytearray()
    for p in gp:
        b = p.encode() + b'\0'
        en += struct.pack('<QQII', 0, 1, len(st), len(b)); st += b
    (game / 'runtime-index.bin').write_bytes(b'TOTKRFS1' + struct.pack('<QQ', len(gp), 24 + len(en)) + en + st)
    (game / 'romfs.bin').write_bytes(b'\0')
    genv = {'UIT_MODS_ROMFS': str(game)}
    def snap(d):   # [UIT 0062] every file under d with its bytes and times: the game dump must never change
        return sorted((str(p.relative_to(d)), p.read_bytes(), p.stat().st_mtime_ns) for p in d.rglob('*') if p.is_file())
    game_before = snap(game)
    rc, rows, err = inst(mkzip(dl / 'BSF loose.zip', {'Screen Fix/Common.Product.110.Nin_NX_NVN.blarc.zs': 'z',
                                                     'Screen Fix/readme.txt': 'r'}), env=genv)
    check(rc == 0 and rows[0][1] == 'Screen Fix' and 'placed by file name' in rows[0][5], f'loose file placed: {rows} {err}')
    check((md / 'Screen Fix/romfs/UI/LayoutArchive/Common.Product.110.Nin_NX_NVN.blarc.zs').is_file() and
          (md / 'Screen Fix/readme.txt').is_file(), 'loose file at the game path')
    (dl / 'Title.Product.110.Nin_NX_NVN.blarc.zs').write_text('t')
    rc, rows, err = inst(dl / 'Title.Product.110.Nin_NX_NVN.blarc.zs', env=genv)
    check(rc == 0 and rows[0][1] == 'Title (LayoutArchive)' and
          (md / rows[0][1] / 'romfs/UI/LayoutArchive/Title.Product.110.Nin_NX_NVN.blarc.zs').is_file(),
          f'one dropped game file: {rows} {err}')
    rc, rows, err = inst(mkzip(dl / 'dup.zip', {'Dup.bfres.zs': 'd'}), env=genv)
    check(rc == 1 and '2 files named' in rows[0][2], f'ambiguous loose file refused: {rows}')
    rc, rows, err = inst(mkzip(dl / 'other.zip', {'Nope.bfres.zs': 'd'}), env=genv)
    check(rc == 1 and 'no file named' in rows[0][2], f'unknown loose file refused: {rows}')
    rc, rows, err = inst(dl / 'BSF loose.zip', env={'UIT_MODS_ROMFS': str(t / 'none')})
    check(rc == 1 and 'file list was not found' in rows[0][2], f'loose file, no game list: {rows}')
    # 15c. what was placed: a 'placed' row and uit-placed.txt (beside romfs, never loaded); Undo puts everything back
    rc, rows, err = inst(dl / 'Title.Product.110.Nin_NX_NVN.blarc.zs', env=genv)   # an update of the same loose mod
    check(['placed', 'Title (LayoutArchive)', 'UI/LayoutArchive/Title.Product.110.Nin_NX_NVN.blarc.zs'] in rows, f'placed row: {rows}')
    check((md / 'Title (LayoutArchive)/uit-placed.txt').read_text().split() ==
          ['UI/LayoutArchive/Title.Product.110.Nin_NX_NVN.blarc.zs'], 'uit-placed.txt')
    (md / 'Title (LayoutArchive)/mine.txt').write_text('kept')         # the copy from before the update, as it was
    before = (md / 'load-order.txt').read_text()
    lines = before.splitlines()
    i = lines.index('+Title (LayoutArchive)')
    lines.insert(len(lines), lines.pop(i).replace('+', '-', 1))       # bottom and off: Undo must bring that back too
    (md / 'load-order.txt').write_text('\n'.join(lines) + '\n')
    before = (md / 'load-order.txt').read_text()
    rc, rows, err = inst(dl / 'Title.Product.110.Nin_NX_NVN.blarc.zs', env=genv)
    check(not (md / 'Title (LayoutArchive)/mine.txt').exists() and (md / '.undo/undo.json').is_file(), f'update replaced: {rows} {err}')
    rc, rows, err = run('undo', '--mods', str(md))
    check(rc == 0 and rows == [['undone', 'Title (LayoutArchive)', 'restored']], f'undo update: {rows} {err}')
    check((md / 'Title (LayoutArchive)/mine.txt').read_text() == 'kept' and (md / 'load-order.txt').read_text() == before and
          not (md / '.undo').exists(), 'undo restored the old copy and the load order')
    rc, rows, err = run('undo', '--mods', str(md))
    check(rc == 1 and rows[0][0] == 'error' and 'nothing to undo' in rows[0][2], f'undo twice: {rows}')
    # a new loose mod: Undo removes it; a normal (folder) install keeps no undo record
    (dl / 'Common.Product.110.Nin_NX_NVN.blarc.zs').write_text('c')
    rc, rows, err = inst(dl / 'Common.Product.110.Nin_NX_NVN.blarc.zs', env=genv)
    check(rc == 0 and rows[0][1] == 'Common (LayoutArchive)', f'second loose: {rows} {err}')
    rc, rows, err = run('undo', '--mods', str(md))
    check(rows == [['undone', 'Common (LayoutArchive)', 'removed']] and not (md / 'Common (LayoutArchive)').exists() and
          (md / 'load-order.txt').read_text() == before, f'undo new loose: {rows} {err}')
    rc, rows, err = inst(mkzip(dl / 'plain.zip', {'Plain/romfs/UI/p.zs': 'p'}))
    check(rc == 0 and not (md / '.undo').exists(), 'no undo record for a folder mod')
    run('remove', 'Plain', '--mods', str(md))
    # from the inbox, into a mods folder that did not exist: Undo puts the file back in the inbox and the folder goes
    md2, ib2 = t / 'mods-u', t / 'inbox-u'
    ib2.mkdir()
    (ib2 / 'Common.Product.110.Nin_NX_NVN.blarc.zs').write_text('c')
    rc, rows, err = run('inbox', '--mods', str(md2), '--inbox', str(ib2), env=genv)
    check(rc == 0 and ['placed', 'Common (LayoutArchive)', 'UI/LayoutArchive/Common.Product.110.Nin_NX_NVN.blarc.zs'] in rows and
          not (ib2 / 'Common.Product.110.Nin_NX_NVN.blarc.zs').exists(), f'inbox loose: {rows} {err}')
    rc, rows, err = run('undo', '--mods', str(md2))
    check(rc == 0 and (ib2 / 'Common.Product.110.Nin_NX_NVN.blarc.zs').read_text() == 'c' and not md2.exists(), f'inbox undo: {rows} {err}')
    for nm in ('Fog Remover', 'Sky (Ryujinx)', 'Sky (Yuzu)', 'Caps Mod', 'Screen Fix', 'Title (LayoutArchive)'):
        run('remove', nm, '--mods', str(md))

    check(snap(game) == game_before, 'loose placement, update and undo never touched the game dump')
    # 15. remove (a read-only file too)
    os.chmod(md / 'Atmo/romfs/Pack/A.pack.zs', 0o444)
    rc, rows, err = run('remove', 'Atmo', '..', 'Nope', '--mods', str(md))
    check(rows[0] == ['removed', 'Atmo'] and rows[1][0] == 'error' and rows[2][0] == 'error', f'remove: {rows}')
    check(not (md / 'Atmo').exists() and '+Atmo' not in order(md) and t.exists() and md.exists(), 'removed only that mod')

    # 16. the game's index reads the same order and on/off
    ls = lister()
    if ls:
        got = subprocess.run([ls, str(md)], capture_output=True, text=True).stdout.splitlines()
        check([g.split('\t')[0] for g in got] == order(md), f'modfs agrees:\n{got}\n{order(md)}')
        mixed = [g for g in got if g.split('\t')[0] == '+Mixed']
        check(mixed and int(mixed[0].split('\t')[2]) & 2, 'modfs flags the ips')
    else:
        print('(modfs cross-check skipped: no C compiler)')

    shutil.rmtree(t)
    print('mods.py: all ok' if not fails else f'mods.py: {fails} failure(s)')
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())
