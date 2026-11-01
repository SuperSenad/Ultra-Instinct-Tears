"""The port's version: one source, the git tag v<version> on the release commit.

  - a release: the tag on HEAD (tools/make_package.py and tools/public_tree.py refuse an untagged HEAD);
  - a builder package or an output folder made by tools/setup.py has no .git: VERSION.txt at its root says it
    ("Ultra Instinct Tears <version> (commit <hash>)", written by make_package.py / setup.py; also read: the
    "UltraTears ..." form from before the rename);
  - a clone: git describe, e.g. 0.1.0-3-gabc1234-dirty between releases.
tools/build_game.py puts it into the exe (build/version.h: the first log line, the file's properties in Windows).
"""
import os
import pathlib
import re
import subprocess

ROOT = pathlib.Path(__file__).resolve().parent.parent
TAG = re.compile(r'^v(\d+\.\d+\.\d+)$')


def _git(*args):
    r = subprocess.run(['git', '-C', str(ROOT)] + list(args), capture_output=True, text=True, stdin=subprocess.DEVNULL)
    return r.stdout.strip() if r.returncode == 0 else None


def release():
    """The version of the tag v<x.y.z> on HEAD, or None."""
    for t in (_git('tag', '--points-at', 'HEAD') or '').split():
        m = TAG.match(t)
        if m:
            return m.group(1)
    return None


def current():
    """(version, commit) of this tree."""
    # [UIT 0129] tools/update.py writes the package's VERSION.txt only after the build (so a failed build leaves the
    # folder saying what it really has), and hands the package's version to the build in these two variables: the exe
    # and the launcher were built with the VERSION.txt still there, the previous version's (an update to 9c231f1 logged
    # "commit 863d9b6" as its first line).
    ev, ec = os.environ.get('UIT_BUILD_VERSION', '').strip(), os.environ.get('UIT_BUILD_COMMIT', '').strip()
    if re.fullmatch(r'[\w.+-]+', ev) and re.fullmatch(r'[0-9a-f]{4,40}', ec):
        return ev, ec
    f = ROOT / 'VERSION.txt'
    if f.is_file():
        m = re.match(r'(?:Ultra Instinct Tears|UltraTears) (\S+) \(commit (\w+)\)', f.read_text(encoding='utf-8'))
        if m:
            return m.group(1), m.group(2)
    d = _git('describe', '--tags', '--match', 'v[0-9]*', '--dirty')
    commit = _git('rev-parse', '--short', 'HEAD') or 'unknown'
    if d:
        return d[1:], commit
    if _git('rev-parse', 'HEAD'):   # no release tag yet
        dirty = '-dirty' if _git('status', '--porcelain', '--untracked-files=no') else ''
        return f'0.0.0-dev{dirty}', commit
    return 'unknown', 'unknown'


def version_txt(version, commit):
    return f'Ultra Instinct Tears {version} (commit {commit})\n'


def numbers(version):
    """Four numbers for the Windows version resource: x.y.z and the commits since the tag (0.1.0-3-g... -> 0,1,0,3)."""
    m = re.match(r'(\d+)\.(\d+)\.(\d+)(?:-(\d+)-g)?', version)
    return tuple(int(x or 0) for x in m.groups()) if m else (0, 0, 0, 0)
