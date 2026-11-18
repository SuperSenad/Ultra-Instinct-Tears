"""[UIT 0433] Make UltraInstinctTears-Setup.exe: the first-time installer a new player downloads.

usage: python tools/build_installer.py --zip UltraInstinctTears-src.zip [--out DIR] [--launcher EXE]
                                       [--cxx CMD --rc CMD]   (as tools/build_launcher.py; a Linux cross build too)
       python tools/make_source_zip.py --out dist --setup     (RELEASING.md: the source package, then this)

The installer is the Windows launcher (launcher/installer.inc runs instead of the settings when it finds a package
appended to its own exe) followed by the source package and a 16-byte trailer: "UITSETUP" and the package's length
(8 bytes, little endian). One file, no installer framework. Nothing from the game is in it: the player points it at
their own dump; it gets the build tools from the same upstream builder release tools/setup.py uses. Writes
<out>/UltraInstinctTears-Setup.exe and <out>/UltraInstinctTears-Setup.exe.sha256 (the sha256sum format).
"""
import argparse
import hashlib
import pathlib
import shlex
import struct
import subprocess
import sys
import zipfile

ROOT = pathlib.Path(__file__).resolve().parent.parent
NAME = 'UltraInstinctTears-Setup.exe'
MAGIC = b'UITSETUP'


def append_package(launcher, package, out):
    """the launcher, the package, the trailer; checked by reading the package back the way the installer does"""
    data = pathlib.Path(package).read_bytes()
    with zipfile.ZipFile(package) as z:
        names = z.namelist()
    if 'UltraInstinctTears/VERSION.txt' not in names or 'UltraInstinctTears/tools/setup.py' not in names:
        sys.exit(f'build_installer: {package} is not a source package from tools/make_source_zip.py (no VERSION.txt or tools/setup.py)')
    exe = pathlib.Path(launcher).read_bytes()
    if exe[:2] != b'MZ':
        sys.exit(f'build_installer: {launcher} is not a Windows program')
    if exe[-16:-8] == MAGIC:
        sys.exit(f'build_installer: {launcher} is already an installer')
    out = pathlib.Path(out)
    out.parent.mkdir(parents=True, exist_ok=True)
    tmp = out.with_name(out.name + '.tmp')
    tmp.write_bytes(exe + data + MAGIC + struct.pack('<Q', len(data)))
    blob = tmp.read_bytes()
    n = struct.unpack('<Q', blob[-8:])[0]
    assert blob[-16:-8] == MAGIC and blob[-16 - n:-16] == data
    tmp.replace(out)
    h = hashlib.sha256(blob).hexdigest()
    side = out.with_name(out.name + '.sha256')
    side.write_text(f'{h}  {out.name}\n', encoding='ascii', newline='\n')
    return h


def build_launcher(dst, cxx=None, rc=None, sdl=None):
    cmd = [sys.executable, str(ROOT / 'tools' / 'build_launcher.py'), '--target', 'windows', '--out', str(dst)]
    if cxx:
        cmd += ['--cxx', cxx]
    if rc:
        cmd += ['--rc', rc]
    if sdl:
        cmd += ['--sdl', sdl]
    if subprocess.run(cmd, cwd=ROOT).returncode:
        sys.exit('build_installer: the launcher did not build')


def make(package, out_dir, launcher=None, cxx=None, rc=None, sdl=None):
    out = pathlib.Path(out_dir) / NAME
    if not launcher:
        launcher = ROOT / 'build' / 'launcher' / 'setup-launcher.exe'
        build_launcher(launcher, cxx, rc, sdl)
    h = append_package(launcher, package, out)
    mb = out.stat().st_size / (1 << 20)
    print(f'{out}: {mb:.1f} MB (the launcher with {pathlib.Path(package).name} inside)')
    print(f'{out.with_name(out.name + ".sha256")}: {h}')
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('--zip', required=True, help='the source package (tools/make_source_zip.py)')
    ap.add_argument('--out', help='where to write it (default: next to the zip)')
    ap.add_argument('--launcher', help='a Windows launcher already built (default: build it with tools/build_launcher.py)')
    ap.add_argument('--cxx', help='as tools/build_launcher.py --cxx')
    ap.add_argument('--rc', help='as tools/build_launcher.py --rc')
    ap.add_argument('--sdl', help='as tools/build_launcher.py --sdl')
    a = ap.parse_args()
    make(a.zip, a.out or pathlib.Path(a.zip).resolve().parent, a.launcher, a.cxx, a.rc, a.sdl)


if __name__ == '__main__':
    main()
