"""A stand-in for Windows' tar.exe (bsdtar) on .7z files, for test_mods.py on a box without bsdtar: "-t -f F" lists,
"-x -f F -C D" extracts. Needs py7zr. Like bsdtar, it refuses to write outside D."""
import os, sys
import py7zr

a = sys.argv[1:]
f = a[a.index('-f') + 1]
try:
    with py7zr.SevenZipFile(f) as z:
        if '-t' in a:
            for n in z.getnames(): print(n)
        else:
            d = os.path.realpath(a[a.index('-C') + 1])
            for n in z.getnames():
                if not os.path.realpath(os.path.join(d, n)).startswith(d + os.sep):
                    print(f'tar: {n}: Path contains \'..\'', file=sys.stderr); sys.exit(1)
            z.extractall(d)
except Exception as e:
    print(f'tar: Error opening archive: {e}', file=sys.stderr); sys.exit(1)
