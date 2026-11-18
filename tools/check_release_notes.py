#!/usr/bin/env python3
"""[UIT 0377] Lint release notes before they go on GitHub (the launcher's "Update available" dialog shows their top).

usage: python3 tools/check_release_notes.py NOTES.md [--allow-unicode] [--preview]
Fails (exit 1) on:
  * a hard-wrapped paragraph or list item: a line that runs on into the next non-blank line. Write each paragraph and
    each list item as one long line (GitHub and the launcher wrap it). A line ending in two spaces or a backslash is a
    deliberate break and is allowed; so are code fences and tables.
  * a character outside ASCII: launchers before 0377 read the notes through the Windows code page, so an en dash shows
    as '?' and a character the code page lacks (an arrow) stops the update check. --allow-unicode makes it a warning.
Warns when there is no "SHA-256 (<zip>): <hex>" line (the updater checks the download against it) and when the first
block is not a "# " title. --preview prints what the launcher shows (both the 0377 blocks and the older plain form).
See RELEASING.md."""
import argparse
import pathlib
import re
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))

BLOCK_START = re.compile(r'\s*(#{1,6}\s|[-*+]\s|\d{1,3}[.)]\s|>|\||```|~~~|<!--|([-*_])(\s*\2){2,}\s*$)')


def lint(text, allow_unicode=False):
    """(errors, warnings) as lists of strings, each naming a line."""
    errors, warnings = [], []
    lines = text.replace('\r\n', '\n').split('\n')
    fence = comment = False
    for i, line in enumerate(lines):
        n = i + 1
        t = line.strip()
        bad = sorted({c for c in line if ord(c) > 127})
        if bad:
            msg = f'line {n}: non-ASCII {" ".join(repr(c) for c in bad)} (older launchers show "?" or fail the update check)'
            (warnings if allow_unicode else errors).append(msg)
        if t.startswith('```') or t.startswith('~~~'):
            fence = not fence
            continue
        if '<!--' in t and '-->' not in t:
            comment = True
        if comment:
            comment = '-->' not in t or t.endswith('<!--')
            continue
        if fence or not t or t.startswith('|') or re.match(r'#{1,6}\s', t):
            continue
        if re.fullmatch(r'([-*_])(\s*\1){2,}', t):
            continue
        nxt = lines[i + 1] if i + 1 < len(lines) else ''
        if not nxt.strip() or BLOCK_START.match(nxt):
            continue
        if line.endswith('  ') or line.endswith('\\'):
            continue   # a deliberate line break
        errors.append(f'line {n}: hard-wrapped: it runs on into line {n + 1} ("{nxt.strip()[:40]}..."); join them into one line')
    if not re.search(r'(?im)^.*sha-?256.*\b[0-9a-f]{64}\b', text):
        warnings.append('no "SHA-256 (UltraInstinctTears-src.zip): <hex>" line: the updater then checks only the .sha256 asset')
    first = next((ln.strip() for ln in lines if ln.strip() and not ln.strip().startswith('<!--')), '')
    if first and not first.startswith('# '):
        warnings.append('the first line is not a "# " title: the dialog shows the top of the notes first')
    return errors, warnings


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('notes', type=pathlib.Path)
    ap.add_argument('--allow-unicode', action='store_true')
    ap.add_argument('--preview', action='store_true')
    a = ap.parse_args()
    text = a.notes.read_text(encoding='utf-8')
    errors, warnings = lint(text, a.allow_unicode)
    for w in warnings:
        print(f'{a.notes}: warning: {w}')
    for e in errors:
        print(f'{a.notes}: error: {e}')
    if a.preview:
        import update
        md, cut = update.notes_text(text, markdown=True)
        print('---- the launcher (0377 and later) shows:\n' + md + ('\n[Full notes on the release page]' if cut else ''))
        print('---- older launchers show (as plain text):\n' + update.notes_text(text))
    if errors:
        print(f'{a.notes}: {len(errors)} problem(s); see RELEASING.md')
        return 1
    print(f'{a.notes}: ok')
    return 0


if __name__ == '__main__':
    sys.exit(main())
