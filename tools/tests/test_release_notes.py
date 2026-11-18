#!/usr/bin/env python3
"""[UIT 0377] Release notes: update.py's blocks for the launcher (soft breaks joined, headings, lists, Unicode, a cut
at a block boundary) and tools/check_release_notes.py's lint (hard wraps, non-ASCII)."""
import pathlib
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent.parent))
import update as U  # noqa: E402
import check_release_notes as L  # noqa: E402

fails = 0


def check(ok, what):
    global fails
    print(('ok    ' if ok else 'FAIL  ') + what)
    fails += not ok


H = 'ab' * 32
body = ('<!-- draft -->\n# UIT 1.0.3 \u2013 test build 6\n\nTest build 5 plus\na fix, builds 1\u20135.\n\n## What changed\n'
        '- **[UIT 0374] Pieces** find\n  each other.\n- `TOTK_X` [docs](http://x) \u2192 on\n1. first\n2) second\n\n---\n\n'
        f'SHA-256 (UltraInstinctTears-src.zip): {H}\n')
b = U.notes_blocks(body)
check(b[0] == ('h', 1, 'UIT 1.0.3 \u2013 test build 6'), f'blocks: title, en dash kept ({b[0]!r})')
check(b[1] == ('p', '', 'Test build 5 plus a fix, builds 1\u20135.'), f'blocks: soft line breaks joined ({b[1]!r})')
check(b[2] == ('h', 2, 'What changed'), 'blocks: section heading')
check(b[3] == ('li', '\u2022', '[UIT 0374] Pieces find each other.'), f'blocks: list item with its continuation ({b[3]!r})')
check(b[4] == ('li', '\u2022', 'TOTK_X docs \u2192 on'), f'blocks: code and link reduced, arrow kept ({b[4]!r})')
check(b[5] == ('li', '1.', 'first') and b[6] == ('li', '2.', 'second'), 'blocks: numbered items')
check(len(b) == 7, 'blocks: comment, rule and checksum line left out')
safe = U.notes_safe('a\u2011b \U0001F389 \u2033x\u2033 \u00a0')
check(safe == 'a-b  "x"  ', f'unicode: unknown characters as ASCII or nothing ({safe!r})')
md, cut = U.notes_text(body, markdown=True)
check(not cut and md.startswith('# UIT 1.0.3') and '\n\n## What changed\n\n- [UIT 0374]' in md and '\n\n1. first' in md, f'markdown form ({md!r})')
plain = U.notes_text(body)
check(plain.startswith('UIT 1.0.3 \u2013 test build 6\n\nTest build 5 plus a fix') and '\u2022 [UIT 0374] Pieces find each other.\n\u2022 TOTK_X' in plain,
      f'plain form for older launchers ({plain!r})')
long = '# T\n\n' + '\n\n'.join(f'Paragraph {i} ' + 'word ' * 40 for i in range(20)) + '\n\n## Tail\n'
md, cut = U.notes_text(long, markdown=True)
check(cut and md.rstrip().endswith('word') and len(md) <= U.GitHubReleasesSource.MAX_NOTES + 40, 'excerpt: whole paragraphs, cut flagged')
plain = U.notes_text(long)
check(plain.endswith('\n\nFull notes on the release page.') and 'Paragraph 19' not in plain, 'excerpt: older launchers get the release page line')
md, cut = U.notes_text('# T\n\nSome text.\n\n## Install\n\n' + 'x ' * 900, markdown=True)
check(cut and not md.rstrip().endswith('Install'), f'excerpt: never ends on a heading ({md[-40:]!r})')
md, cut = U.notes_text('One. ' + 'Two words here. ' * 200, markdown=True)
check(cut and md.endswith('here.') and len(md) <= U.GitHubReleasesSource.MAX_NOTES, 'excerpt: a long first paragraph cut at a sentence end')

e, w = L.lint('# Title\n\nOne long line.\n\n- item\n  - nested\n- next\n\n1. a\n2. b\n\n```\ncode\nmore code\n```\n\n| a | b |\n|---|---|\n\n'
              f'line with break  \nafter\n\nSHA-256 (x.zip): {H}\n')
check(not e and not w, f'lint: a clean file passes ({e} {w})')
e, w = L.lint('# T\n\nThis paragraph is\nhard wrapped.\n\n- item that\n  runs on\n')
check(len(e) == 2 and 'line 3' in e[0] and 'line 6' in e[1], f'lint: hard wraps in a paragraph and a list item ({e})')
e, w = L.lint('# T \u2013 x\n')
check(len(e) == 1 and 'non-ASCII' in e[0] and any('SHA-256' in x for x in w), f'lint: non-ASCII, missing checksum ({e} {w})')
e, w = L.lint('# T \u2013 x\n', allow_unicode=True)
check(not e and any('non-ASCII' in x for x in w), 'lint: --allow-unicode warns only')
e, w = L.lint('Intro\n')
check(any('title' in x for x in w), 'lint: no "# " title warned')

print('all release notes checks passed' if not fails else f'{fails} release notes check(s) failed')
sys.exit(1 if fails else 0)
