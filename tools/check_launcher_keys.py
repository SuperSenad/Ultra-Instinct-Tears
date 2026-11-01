#!/usr/bin/env python3
"""Check that the launcher can edit every setting the game reads from ultrainstincttears.cfg.

Every "TOTK_*" string literal in src/ is a key boot.c may pass through from ultrainstincttears.cfg (it applies each TOTK_ line
as an environment variable), so each one must have a [section] in launcher/settings.ini (directly or as an alias).
Also checks the schema itself: known fields and types, defaults inside ranges and choices, needs= pointing at keys.
[UIT 0304] And that every setting says when a change takes effect (applies = live | restart | setup | launcher), matching
the code: live = the in-game overlay changes it while the game runs (a cfg key of its table in src/host/uit_overlay.c, or a
button binding of its Controls tab); setup = read only by tools/setup.py (the next setup run); launcher = read only by the
launcher and its helpers; restart = everything else (the game reads it when it starts). The launcher tags restart settings.

    py -3 tools/check_launcher_keys.py        (exit 1 and a list on any problem; tools/build_launcher.py runs it)
"""
import pathlib, re, sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
SCHEMA = ROOT / 'launcher' / 'settings.ini'

# "TOTK_..." literals in src/ that are not ultrainstincttears.cfg keys.
NOT_KEYS = {
    'TOTK_APPLICATION_ID',     # a title-id macro
    'TOTK_AUDIO_REVERB_H',     # an include guard
    'TOTK_DUMP_AFTER',         # named in a comment only
    'TOTK_MFX',                # named in a comment only
    'TOTK_WARM_ONLY',          # set by tools/setup.py --warm-shaders for that run only (UIT 0081)
    'TOTK_VPOOL_PAGE',         # a macro
}
# Keys the launcher writes that src/ reads under a different spelling or that land with other work.
LAUNCHER_ONLY = {'TOTK_LAUNCHER_UPDATE_DIRS', 'TOTK_LAUNCHER_REPORT_DIR', 'TOTK_LAUNCHER_PYTHON',   # read by the launcher and tools/update.py, report.py
                 'TOTK_LAUNCHER_UPDATE_ONLINE', 'TOTK_LAUNCHER_UPDATE_PRERELEASE', 'TOTK_LAUNCHER_UPDATE_REPO',   # [UIT 0281] GitHub updates
                 'TOTK_LAUNCHER_UPDATE_ON_START',   # [UIT 0320] the launcher's start-up check
                 'TOTK_SAVE_BACKUPS'}   # [UIT 0347] the launcher zips the saves before Play
FIELDS = {'name', 'tab', 'group', 'type', 'default', 'on', 'off', 'choices', 'min', 'max', 'step', 'unit', 'auto', 'zero',
          'hint', 'summary', 'detail', 'tradeoff', 'recommended', 'needs', 'alias', 'tip', 'restart', 'section', 'lift',
          'ask', 'applies'}
TABS = {'graphics', 'performance', 'game', 'mods', 'advanced'}
TYPES = {'bool', 'choice', 'int', 'float', 'text', 'pad', 'key'}


def bind_tables():
    """src/host/input_map.h: the controller and key tokens, and each Switch button's cfg keys and defaults."""
    h = (ROOT / 'src' / 'host' / 'input_map.h').read_text()
    rows = re.findall(r'\{"(TOTK_PAD_\w+)", "(TOTK_KEY_\w+)", "\w+", "[^"]+", "([^"]+)", "([^"]+)", \d+\}', h)
    pad = set(re.findall(r'\{"(\w+)", (?:SDL_GAMEPAD_BUTTON_\w+|-1), (?:SDL_GAMEPAD_AXIS_\w+|-1)\}', h))
    keys = set(re.findall(r'\{"(\w+)", "[^"]*", SDL_SCANCODE_\w+', h))
    defaults = {}
    for pk, kk, pd, kd in rows:
        defaults[pk], defaults[kk] = pd, kd
    return pad, keys, defaults


APPLIES = ('live', 'restart', 'setup', 'launcher')


def overlay_live_keys():
    """[UIT 0304] The cfg keys the in-game overlay changes while the game runs: its settings table and its cfg edits."""
    t = (ROOT / 'src' / 'host' / 'uit_overlay.c').read_text(errors='replace')
    i = t.find('kUitOvSettings[OV_COUNT] = {')
    if i < 0:
        return set()
    table = t[i:t.index('\n};', i)]
    return set(re.findall(r'"(TOTK_[A-Z0-9_]+)"', table)) | set(re.findall(r'edit\(out, max, \w+, "(TOTK_[A-Z0-9_]+)"', t))


def parse(text):
    out, cur = {}, None
    for n, line in enumerate(text.splitlines(), 1):
        line = line.strip()
        if not line or line.startswith('#'):
            continue
        if line.startswith('[') and line.endswith(']'):
            cur = line[1:-1]
            if cur in out:
                raise SystemExit(f'settings.ini:{n}: [{cur}] twice')
            out[cur] = {'_line': n}
        elif '=' in line and cur:
            k, v = (x.strip() for x in line.split('=', 1))
            out[cur][k] = v
        else:
            raise SystemExit(f'settings.ini:{n}: cannot read {line!r}')
    return out


def runtime_keys():
    keys = set()
    for p in (ROOT / 'src').rglob('*'):
        if p.suffix in ('.c', '.h', '.cpp', '.hpp', '.m', '.mm', '.inc'):
            keys |= set(re.findall(r'"(TOTK_[A-Z0-9_]+)"', p.read_text(errors='replace')))
    return keys - NOT_KEYS


def main():
    s = parse(SCHEMA.read_text(encoding='utf-8'))
    global BINDS
    BINDS = bind_tables()
    problems = []
    if len(BINDS[0]) < 16 or len(BINDS[1]) < 40:
        problems.append(f'input_map.h: read only {len(BINDS[0])} controller and {len(BINDS[1])} key tokens')
    covered = set(s) | {e['alias'] for e in s.values() if 'alias' in e}
    rt = runtime_keys()
    for k in sorted(rt - covered):
        problems.append(f'{k}: read by src/ but missing from launcher/settings.ini')
    # lift-time keys are read by tools/setup.py (LIFT_OPTIONS) when the code is translated, not by the game (UIT 0078);
    # TOTK_SETUP_* by setup.py itself (UIT 0081)
    setup_keys = set(re.findall(r"'(TOTK_(?:LIFT|SETUP)_[A-Z0-9_]+)'", (ROOT / 'tools/setup.py').read_text(errors='replace')))
    for k in sorted(set(s) - rt - LAUNCHER_ONLY - setup_keys):
        if k.startswith('TOTK_') and s[k].get('alias') not in rt:
            problems.append(f'{k}: in settings.ini but nothing in src/ reads it')
    live = overlay_live_keys()
    if len(live) < 10:
        problems.append(f'src/host/uit_overlay.c: read only {len(live)} live keys from the overlay table')
    for k in sorted(live - set(s)):
        problems.append(f'{k}: the overlay changes it but it is not in launcher/settings.ini')
    for k, e in s.items():
        where = f'settings.ini:{e["_line"]} [{k}]'
        # [UIT 0304] when a change takes effect, as the code has it
        if k in live or e.get('type') in ('pad', 'key'):
            want = 'live'
        elif k in LAUNCHER_ONLY:
            want = 'launcher'
        elif k in setup_keys and k not in rt:
            want = 'setup'
        else:
            want = 'restart'
        have = e.get('applies', 'restart')
        if have not in APPLIES:
            problems.append(f'{where}: applies {have!r} is not one of {", ".join(APPLIES)}')
        elif have != want:
            problems.append(f'{where}: applies = {have}, but the code says {want}' + (' (omit the line)' if want == 'restart' else ''))
        for f in e:
            if f != '_line' and f not in FIELDS:
                problems.append(f'{where}: unknown field {f}')
        for f in ('name', 'tab', 'group', 'type', 'summary', 'detail'):
            if not e.get(f):
                problems.append(f'{where}: no {f}')
        if e.get('tab') not in TABS:
            problems.append(f'{where}: tab {e.get("tab")}')
        t = e.get('type')
        if t not in TYPES:
            problems.append(f'{where}: type {t}')
        d = e.get('default', '-')
        if t == 'choice':
            vals = [c.split(':', 1)[0] for c in e.get('choices', '').split('|')]
            if not all(':' in c for c in e.get('choices', '').split('|')):
                problems.append(f'{where}: choices need value:Label')
            # [UIT 0270] ask = 1 with default - : the key absent means "not asked yet" (shown as the first choice)
            if d not in vals and '-' not in vals and not (d == '-' and e.get('ask') == '1'):
                problems.append(f'{where}: default {d} is not a choice')
            if 'recommended' in e and e['recommended'] not in vals:
                problems.append(f'{where}: recommended {e["recommended"]} is not a choice')
        if t in ('pad', 'key'):
            pad_tokens, key_tokens, bind_defaults = BINDS
            ok = pad_tokens if t == 'pad' else key_tokens
            for tok in d.split(','):
                if tok not in ok:
                    problems.append(f'{where}: default {tok} is not a {t} token in src/host/input_map.h')
            if bind_defaults.get(k) != d:
                problems.append(f'{where}: default {d} differs from src/host/input_map.h ({bind_defaults.get(k)})')
        if t in ('int', 'float'):
            try:
                lo, hi, st = float(e['min']), float(e['max']), float(e.get('step', 1))
                if d != '-' and not lo <= float(d) <= hi:
                    problems.append(f'{where}: default {d} outside {lo}..{hi}')
                if d == '-' and 'auto' not in e:
                    problems.append(f'{where}: no default and no auto label')
                if st <= 0:
                    problems.append(f'{where}: step {st}')
            except (KeyError, ValueError) as ex:
                problems.append(f'{where}: range: {ex}')
        if t == 'bool' and d not in ('on', 'off'):
            problems.append(f'{where}: bool default must be on/off')
        if 'needs' in e:
            nk = e['needs'].split('=', 1)[0]
            if nk not in s:
                problems.append(f'{where}: needs unknown key {nk}')
        lifts = ('quality', 'fxaa', 'lod', 'shadow', 'cheat_durability', 'cheat_amiibo', 'cheat_stamina', 'cheat_arrows')
        if 'lift' in e and e['lift'] not in lifts:   # [UIT 0070, 0344] lift-features.ini names
            problems.append(f'{where}: lift {e["lift"]!r} is not one of {", ".join(lifts)}')
    for p in problems:
        print('check_launcher_keys:', p)
    print(f'check_launcher_keys: {len(s)} settings, {len(rt)} keys read by src/, {len(problems)} problem(s)')
    return 1 if problems else 0


if __name__ == '__main__':
    sys.exit(main())
