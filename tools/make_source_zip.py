"""Make the source package the launcher's updater installs: <out>/UltraInstinctTears-src.zip of HEAD with VERSION.txt.

usage: python tools/make_source_zip.py [--out ..] [--version 0.0.0-fps] [--name UltraInstinctTears-src.zip]
[UIT 0157] A plain "git archive" has no VERSION.txt (it is not tracked): installed over a folder, the folder's old
VERSION.txt stayed and the folder, launcher and exe went on naming the previous version (the 843a8f7 zip said cc3add4).
This adds "UltraInstinctTears/VERSION.txt" with HEAD's commit. Refuses a dirty tree: the package must be the commit it names.
[UIT 0282] For a GitHub release (RELEASING.md): the version defaults to HEAD's tag vX.Y.Z (else 0.0.0-fps), and
<zip>.sha256 is written next to the zip ("<sha256>  UltraInstinctTears-src.zip", the sha256sum format): attach both to
the release; the launcher's updater checks the download against it.
"""
import argparse
import hashlib
import pathlib
import subprocess
import sys
import zipfile

ROOT = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'tools'))
import version  # noqa: E402


def git(*a):
    return subprocess.run(['git', *a], cwd=ROOT, capture_output=True, text=True, check=True).stdout.strip()


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('--out', default=str(ROOT.parent))
    ap.add_argument('--version', default=None, help='default: the tag vX.Y.Z on HEAD, else 0.0.0-fps')
    ap.add_argument('--name', default='UltraInstinctTears-src.zip')
    a = ap.parse_args()
    a.version = a.version or version.release() or '0.0.0-fps'
    if git('status', '--porcelain', '--untracked-files=no'):
        sys.exit('make_source_zip: the working tree has changes: commit them first')
    commit = git('rev-parse', '--short=7', 'HEAD')
    out = pathlib.Path(a.out) / a.name
    out.parent.mkdir(parents=True, exist_ok=True)   # --out dist (RELEASING.md)
    tmp = out.with_name(out.name + '.tmp')
    subprocess.run(['git', 'archive', '--format=zip', '--prefix=UltraInstinctTears/', 'HEAD', '-o', str(tmp)], cwd=ROOT, check=True)
    with zipfile.ZipFile(tmp, 'a', zipfile.ZIP_DEFLATED) as z:
        if 'UltraInstinctTears/VERSION.txt' not in z.namelist():
            z.writestr('UltraInstinctTears/VERSION.txt', version.version_txt(a.version, commit))
    tmp.replace(out)
    h = hashlib.sha256()
    with open(out, 'rb') as f:
        for b in iter(lambda: f.read(1 << 20), b''):
            h.update(b)
    side = out.with_name(out.name + '.sha256')
    side.write_text(f'{h.hexdigest()}  {out.name}\n', encoding='ascii', newline='\n')
    print(f'{out}: {version.version_txt(a.version, commit).strip()}')
    print(f'{side}: {h.hexdigest()}')


if __name__ == '__main__':
    main()
