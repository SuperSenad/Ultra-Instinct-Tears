#!/usr/bin/env python3
"""[UIT 0104] Compile every non-generated source exactly as tools/setup.py does on Windows, against the headers and
libraries the user's toolchain really has: the builder package's deps/clang64 (a trimmed MSYS2 CLANG64: only the
headers and import libraries upstream needed, e.g. no strings.h, no psapi.h, only some libc++ headers).

Why: 7378e8d compiled on the box (zig cc ships a full MinGW + libc++) and failed on the user's PC
('strings.h', 'cpu.h', 'psapi.h', 'cstdlib' not found). This check uses the real thing:
  * the sysroot: deps/clang64 from UltraTears-builder-*-windows-x86_64.zip (the zip setup.py downloads, or
    --builder DIR|ZIP, or --fetch to download it from the same GitHub release setup.py uses);
  * a host clang of the same major version (the builder's clang is MSYS2 23.x): --clang DIR (its bin folder), or
    UIT_LLVM_BIN, or clang on PATH. Run as clang --target=x86_64-w64-windows-gnu --sysroot=deps/clang64
    -resource-dir=deps/clang64/lib/clang/<N>, it searches exactly what MSYS2's clang.exe searches
    (include/c++/v1, lib/clang/<N>/include, include; lib/ for the link).
  * build/build.ninja written by tools/build_game.py itself (--target ultratears, setup's default speed options
    O2 + ThinLTO; --lto off also checked), every edge except the generated game code is run as written, with
    'clang'/'clang++'/'llvm-windres' standing for the builder's. Small stand-in game files (they include
    cpu.h, simd.h, regcache.h like the lifter's output) check the gamecc rule.
  * [UIT 0121] stack frames: one more stand-in is a 16,000-instruction all-labels function in the lifter's form (30%
    FP / vector), which build_game.py compiles at -O0 like the real ones; tools/check_frames.py reads every COFF
    game object's unwind data and fails above 1 MiB (as compiled before 0121, with the SIMD helpers expanded: 1.77
    MB; now ~0.14 MB). The linked exe must reserve 64 MiB of stack per thread (--stack). [UIT 0259] Its PE subsystem
    must be 2 (Windows GUI, never a console window), and the launcher's too.
  * the link: every runtime object + the stand-ins, linked with the build's own LIBS against the builder's import
    and static libraries, with stand-in game tables (tools/lifter/gen.py's form): no undefined symbol, and the exe
    may import only DLLs that are part of Windows.
  * the launcher: tools/build_launcher.py --target windows with the same wrappers (compile, icon/version resource, link).
Exit 0: everything compiles and links. A source tree is taken from git (tracked files plus local changes).
"""
import argparse
import concurrent.futures
import glob
import hashlib
import json
import os
import pathlib
import re
import shlex
import shutil
import subprocess
import sys
import struct
import tempfile
import urllib.request
import zipfile

ROOT = pathlib.Path(__file__).resolve().parents[2]
BUILDER_ZIP_SHA = {   # known builder packages (setup.py takes the newest UltraTears-builder-*-windows-x86_64.zip)
    'UltraTears-builder-0.1.0-windows-x86_64.zip': '94560d340b533106b6af864d8a2dddc1fa18d39a42efa057fafd2ddbbe9484b2',
}
RELEASES = 'https://api.github.com/repos/kuraeow/ultratears/releases?per_page=20'


def say(*a):
    print(*a, flush=True)


def find_builder(arg, fetch):
    cands = [arg] if arg else []
    cands += [os.environ.get('UIT_BUILDER'), str(ROOT / 'deps/clang64')]
    cands += sorted(glob.glob(str(ROOT / 'dev-local/downloads/UltraTears-builder-*-windows-x86_64.zip')), reverse=True)
    for c in filter(None, cands):
        p = pathlib.Path(c)
        if p.is_dir():
            for d in (p, p / 'deps/clang64', *p.glob('*/deps/clang64')):
                if (d / 'include/_mingw.h').is_file():
                    return d.resolve()
        elif p.is_file() and p.suffix == '.zip':
            return unpack_builder(p)
    if not fetch:
        sys.exit('no builder sysroot: pass --builder (deps/clang64 or the UltraTears-builder zip) or --fetch')
    req = urllib.request.Request(RELEASES, headers={'User-Agent': 'UIT-check', 'Accept': 'application/vnd.github+json'})
    with urllib.request.urlopen(req, timeout=30) as r:
        rel = json.load(r)
    for release in rel:
        for a in release.get('assets', []):
            n = a.get('name', '')
            if n.startswith('UltraTears-builder-') and n.endswith('-windows-x86_64.zip'):
                dst = ROOT / 'dev-local/downloads' / n
                dst.parent.mkdir(parents=True, exist_ok=True)
                if not dst.is_file():
                    say(f'downloading {n} ...')
                    urllib.request.urlretrieve(a['browser_download_url'], dst)
                return unpack_builder(dst)
    sys.exit('no builder package in the GitHub releases')


def unpack_builder(z):
    digest = hashlib.sha256(z.read_bytes()).hexdigest()
    want = BUILDER_ZIP_SHA.get(z.name)
    say(f'builder package {z.name} sha256 {digest}' + ('' if want is None else (' (known)' if want == digest else f' (EXPECTED {want})')))
    out = ROOT / 'dev-local/check-toolchain' / z.stem
    if not (out / '.done').is_file():
        with zipfile.ZipFile(z) as zf:
            for info in zf.infolist():
                parts = pathlib.PurePosixPath(info.filename).parts
                if 'clang64' in parts and 'python' not in parts and not info.filename.endswith(('.exe', '.dll')):
                    zf.extract(info, out)
        (out / '.done').write_text(digest)
    return next(out.glob('*/deps/clang64')).resolve()


def find_clang(arg, major):
    for c in filter(None, [arg, os.environ.get('UIT_LLVM_BIN'), *[str(pathlib.Path(p)) for p in os.environ.get('PATH', '').split(os.pathsep)]]):
        cl = pathlib.Path(c) / 'clang'
        if cl.is_file() and (pathlib.Path(c) / 'ld.lld').exists():
            v = subprocess.run([str(cl), '-dumpversion'], capture_output=True, text=True).stdout.strip()
            if v.split('.')[0] == major:
                return pathlib.Path(c).resolve(), v
            say(f'  ({cl} is clang {v}, the builder is {major}: skipped)')
    sys.exit(f'no clang {major} with ld.lld found: pass --clang <LLVM {major} bin folder> or set UIT_LLVM_BIN '
             f'(e.g. LLVM-{major}.x-Linux-X64.tar.xz from github.com/llvm/llvm-project/releases)')


def wrappers(where, llvm, sysroot, major):
    where.mkdir(parents=True, exist_ok=True)
    common = (f'--target=x86_64-w64-windows-gnu --sysroot={sysroot} -resource-dir={sysroot}/lib/clang/{major} '
              f'-stdlib=libc++ -rtlib=compiler-rt -unwindlib=libunwind -fuse-ld=lld -Qunused-arguments')
    for name, real in (('clang', 'clang'), ('clang++', 'clang++')):
        (where / name).write_text(f'#!/bin/sh\nexec {llvm}/{real} {common} "$@"\n')
    # llvm-windres preprocesses with the clang it finds beside itself or on PATH: our wrapper (the builder's sysroot)
    (where / 'llvm-windres').write_text(f'#!/bin/sh\nexec {llvm}/llvm-windres --preprocessor={where}/clang '
                                        f'--preprocessor-arg=-E --preprocessor-arg=-xc --preprocessor-arg=-DRC_INVOKED "$@"\n')
    for f in where.iterdir():
        f.chmod(0o755)


def export_tree(dst):
    files = subprocess.run(['git', '-C', str(ROOT), 'ls-files', '-co', '--exclude-standard', '-z'],
                           capture_output=True, check=True).stdout.decode().split('\0')
    for f in filter(None, files):
        s = ROOT / f
        if s.is_file():
            d = dst / f
            d.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(s, d)


STUB = '''/* check_windows_toolchain: a stand-in for one lifted module: the lifter's includes, one lifted function and the
 * module's table in the form tools/lifter/gen.py writes it (optional patches off, as when a patch site is not found) */
#include "cpu.h"
#include "simd.h"
#include "regcache.h"
void f_check_%(m)s(Cpu *c) { (void)c; }
const uint64_t %(m)s_fn_addrs[] = {0x0};
const GuestFn %(m)s_fn_ptrs[] = {f_check_%(m)s};
const uint64_t %(m)s_fn_count = 1;
%(extra)s'''
MAIN_FLAGS = '''const int main_render_res_patch = 0;
const int main_output_res_patch = 0;
const int main_aspect_patch = 0;
const int main_cheat_durability_patch = 0;
const int main_cheat_amiibo_patch = 0;
const int main_quality_patch = 0;
const int main_fxaa_flag_patch = 0;
const int main_lod_patch = 0;
const int main_shadow_patch = 0;
const int main_shadow_probes = 0;
const unsigned long long main_shadow_probe_sites[] = { 0 };
const char main_lift_build[] = "check";
'''


def bigframe_standin(n=16000, seed=121):
    """[UIT 0121] A lifted function with n labelled instructions and a computed branch (L_dispatch over every label):
    tools/build_game.py's -O0 rule takes it. The statements are the lifter's (tools/lifter/gen.py) for integer ALU,
    loads / stores, branches and, every third one, FCMP / FADD / FMUL / FMADD / FCVTZS / SCVTF / vector FMUL / FMLA /
    ORR / LDR q - the mix of the sdk function behind the 0x80559cc20 crash (its -O0 frame was 8.4 MiB)."""
    import random
    r = random.Random(seed)
    base = 0x805000000
    fp = ['c->nzcv = cmp_s(c, VSF32({a}), VSF32({b}));', 'fp_op_s(c, FOP_MUL, {a}, {b}, {d});',
          'fp_op_s(c, FOP_ADD, {a}, {b}, {d});', 'SV_FOP_S(FOP_MUL, 1, {a}, VREG(vf32, {b}), VREG(vf32, {d}), (vf32){{}});',
          'SV_FOP_S(FOP_ADD, 1, {a}, VREG(vf32, {b}), VREG(vf32, {d}), (vf32){{}});',
          'SV_FOP_S(FOP_MADD, 1, {a}, VREG(vf32, {a}), VREG(vf32, {b}), VREG(vf32, {d}));',
          'c->x[{x}] = (uint32_t)(toint_s(c, VSF32({a}), 3, 0, 32, 0));',
          'FP_SETS({a}, fromint_s(c, (uint32_t)c->x[{y}], 32, 0, 0));',
          'VW(vu64, 1, {a}, VREG(vu64, {b}) | VREG(vu64, {d}));',
          '{{ uint64_t ea = c->x[{x}] + {o:#x}ull; c->v[{a}] = LD128(ea); }}']
    it = ['c->x[{x}] = (uint64_t)(c->x[{y}] + c->x[{z}]);', 'c->x[{x}] = (uint64_t)(c->x[{y}] | c->x[{z}]);',
          'c->x[{x}] = (uint64_t)(c->x[{y}] + {o:#x}ull);', '(void)(subs64(c, c->x[{x}], {o:#x}ull));',
          '{{ uint64_t ea = c->x[{x}] + {o:#x}ull; uint64_t v = LD64(ea); c->x[{y}] = (uint64_t)(v); }}',
          '{{ uint64_t ea = c->x[{x}] + {o:#x}ull; ST64(ea, c->x[{y}]); }}',
          'c->x[{x}] = (uint64_t)((COND_VS(c)) ? c->x[{y}] : (uint64_t)(c->x[{z}]));']
    out = ['/* check_windows_toolchain [UIT 0121]: a lifter-shaped all-labels function (tools/check_frames.py) */',
           '#include "cpu.h"', '#include "simd.h"', f'void f_check_bigframe(Cpu *c) {{', f'    FN_ENTER({base:#x}ull);']
    for i in range(n):
        out.append(f'L_{base + 4 * i:x}:;')
        k = dict(a=r.randrange(32), b=r.randrange(32), d=r.randrange(32), x=r.randrange(29), y=r.randrange(29),
                 z=r.randrange(29), o=r.randrange(0x1000) & ~15)
        if i == n // 2:
            line = f'c->pc = c->x[{k["x"]}]; c->last_br = {base + 4 * i:#x}ull; goto L_dispatch;'
        elif i % 10 == 9:
            line = f'if (COND_{r.choice(["EQ", "LT", "GT", "CC"])}(c)) {{ goto L_{base + 4 * r.randrange(n):x}; }}'
        else:
            line = (r.choice(fp) if i % 10 in (0, 3, 6) else r.choice(it)).format(**k)
        out.append('    ' + line)
    out += ['    return;', 'L_dispatch:', '    switch (c->pc) { '
            + ' '.join(f'case {base + 4 * i:#x}ull: goto L_{base + 4 * i:x};' for i in range(n)) + ' }',
            '    __attribute__((musttail)) return rt_dispatch(c);', '}', '']
    return '\n'.join(out)


FRAME_LIMIT = 1 << 20   # [UIT 0121] tools/check_frames.py --limit for the stand-ins


def frame_check(tree, text, log):
    """[UIT 0121] The -O0 stand-in got build_game.py's -O0 flags (helpers outlined) and every COFF game object's
    largest frame is under FRAME_LIMIT. Returns (failure lines, summary)."""
    sys.path.insert(0, str(tree / 'tools'))
    import check_frames
    bad = []
    m = re.search(r'build build/game/sdk/sdk_bigframe\.o: gamecc \S+\n  gameflags = (.*)', text)
    if not m or '-O0' not in m.group(1) or '-Dalways_inline=noinline' not in m.group(1):
        bad.append('the all-labels stand-in was not given the -O0 flags with the SIMD helpers outlined '
                   f'(gameflags: {m.group(1) if m else "none"})')
    over, allf, skipped = check_frames.check([tree / 'build/game'], FRAME_LIMIT)
    big = [x for x in allf if x[1] == 'f_check_bigframe']
    log.write(f'[frames] {len(allf)} functions, skipped {skipped}: ' + ', '.join(f'{n} {b}' for b, n, _ in allf[:5]) + '\n')
    if not big:
        bad.append('no unwind data found for f_check_bigframe (build/game/sdk/sdk_bigframe.o)')
    for b, n, f in over:
        bad.append(f'{n}: a {b:,}-byte stack frame (limit {FRAME_LIMIT:,}) in {pathlib.Path(f).name}')
    return bad, (f'{big[0][0]:,} bytes' if big else 'not found')


def pe_stack_reserve(exe):
    d = pathlib.Path(exe).read_bytes()
    pe = int.from_bytes(d[0x3c:0x40], 'little')
    return int.from_bytes(d[pe + 24 + 72:pe + 24 + 80], 'little'), int.from_bytes(d[pe + 24 + 80:pe + 24 + 88], 'little')


def pe_subsystem(exe):   # [UIT 0259] 2 = IMAGE_SUBSYSTEM_WINDOWS_GUI, 3 = WINDOWS_CUI (a console program)
    d = pathlib.Path(exe).read_bytes()
    pe = int.from_bytes(d[0x3c:0x40], 'little')
    return int.from_bytes(d[pe + 24 + 68:pe + 24 + 70], 'little')


AP_HOST = ('uit_ap_stub', 'rt_dispatch', 'rt_svc', 'runtime_core_checkpoint', 'guest_call', 'uit_ap_yield',
           'uit_ap_rip', 'memcpy')   # src/host/preempt_win.c uit_ap_init's host functions


def ap_ranges_check(exe, mapf):
    """[UIT 0257] What src/host/preempt_win.c uit_ap_init finds in this exe: the old single range (first to last lifted
    function start) and the exact ranges (each lifted function's own unwind entry, as RtlLookupFunctionEntry gives it).
    Returns (host functions inside the old range, exact ranges, host functions inside them, lifted functions)."""
    d = pathlib.Path(exe).read_bytes()
    pe = int.from_bytes(d[0x3c:0x40], 'little')
    nsec = int.from_bytes(d[pe + 6:pe + 8], 'little')
    opt = pe + 24
    exc_rva, exc_size = struct.unpack_from('<II', d, opt + 112 + 3 * 8)   # data directory 3: exception (.pdata)
    so = opt + int.from_bytes(d[pe + 20:pe + 22], 'little')
    secs = [struct.unpack_from('<IIII', d, so + 40 * i + 8) for i in range(nsec)]   # VirtualSize, VA, raw size, raw ptr
    def off(rva):
        for vs, va, _rs, ra in secs:
            if va <= rva < va + vs:
                return ra + rva - va
        raise ValueError(f'rva {rva:#x} in no section')
    unwind = {}
    if exc_size:
        base = off(exc_rva)
        for i in range(exc_size // 12):
            b, e, _u = struct.unpack_from('<III', d, base + 12 * i)
            unwind[b] = e
    sym = {}
    for line in pathlib.Path(mapf).read_text(errors='replace').splitlines():
        p = line.split()   # Address Size Align Symbol (the map's addresses are RVAs)
        if len(p) == 4 and p[2] == '0' and re.fullmatch(r'[0-9a-f]{8,16}', p[0]):
            sym.setdefault(p[3], int(p[0], 16))
    lifted = {k: v for k, v in sym.items() if re.fullmatch(r'f_\w+', k)}
    if not lifted:
        return [], [], [], {}
    lo, hi = min(lifted.values()), max(lifted.values())
    host = {h: sym[h] for h in AP_HOST if h in sym}
    old = [h for h, a in host.items() if lo <= a <= hi]
    rng = sorted((a, unwind[a]) for a in set(lifted.values()) if a in unwind)
    inside = [h for h, a in host.items() if any(b <= a < e for b, e in rng)]
    return old, rng, inside, lifted


def parse_ninja(text):
    top, rules, builds, cur = {}, {}, [], None
    for line in text.splitlines():
        if not line.strip():
            continue
        if line.startswith('  '):
            k, v = line.strip().split(' = ', 1)
            cur[k] = v
            continue
        if line.startswith('rule '):
            cur = rules.setdefault(line[5:].strip(), {})
        elif line.startswith('build '):
            outs, rest = line[6:].split(': ', 1)
            parts = rest.split(' | ')[0].split()
            cur = {'_out': outs.strip(), '_rule': parts[0], '_in': ' '.join(parts[1:])}
            builds.append(cur)
        elif line.startswith('default '):
            cur = None
        else:
            k, v = line.split(' = ', 1)
            top[k.strip()] = v
            cur = None
    return top, rules, builds


def expand(s, scope):
    return re.sub(r'\$(\w+)', lambda m: scope.get(m.group(1), ''), s)


def gen_ninja(tree, sysroot, lto, no_dlss):
    gen = tree / 'data/generated'
    for m in ('main', 'subsdk0', 'sdk'):
        (gen / m).mkdir(parents=True, exist_ok=True)
        (gen / m / f'{m}_stub.c').write_text(STUB % {'m': m, 'extra': MAIN_FLAGS if m == 'main' else ''})
    if not (gen / 'sdk/sdk_bigframe.c').is_file():
        (gen / 'sdk/sdk_bigframe.c').write_text(bigframe_standin())
    shim = ('import sys; sys.path.insert(0, "tools"); import build_game as b; '
            'b.compile_then_link = lambda *a, **k: 0; b.main()')
    args = ['--generated', 'data/generated', '--target', 'ultratears', '--opt', 'O2', '--lto', lto, '--cpus', '4']
    env = dict(os.environ, ULTRS_CLANG64=str(sysroot / 'bin'))   # toolchain.py only checks that clang.exe is there
    if not (sysroot / 'bin/clang.exe').exists():
        (sysroot / 'bin').mkdir(exist_ok=True)
        (sysroot / 'bin/clang.exe').write_text('')
    r = subprocess.run([sys.executable, '-c', shim, *args] + (['--no-dlss'] if no_dlss else []), cwd=tree, env=env,
                       capture_output=True, text=True)
    if r.returncode:
        sys.exit(f'build_game.py failed:\n{r.stdout}\n{r.stderr}')
    return (tree / 'build/build.ninja').read_text()


def run_edges(tree, text, path_env, jobs, log):
    top, rules, builds = parse_ninja(text)
    cmds, link = [], None
    for b in builds:
        if b['_rule'] == 'phony':
            continue
        if b['_rule'] == 'link':
            link = b
            continue
        scope = dict(top)
        scope.update({k: expand(v, top) for k, v in b.items() if not k.startswith('_')})
        scope['in'], scope['out'] = b['_in'], b['_out']
        cmds.append((b['_out'], expand(rules[b['_rule']]['command'], scope)))
    env = dict(os.environ, PATH=path_env)

    def one(c):
        out, cmd = c
        (tree / out).parent.mkdir(parents=True, exist_ok=True)
        r = subprocess.run(cmd, shell=True, cwd=tree, env=env, capture_output=True, text=True)
        return out, cmd, r.returncode, r.stdout + r.stderr

    bad = []
    with concurrent.futures.ThreadPoolExecutor(jobs) as ex:
        for out, cmd, rc, text_ in ex.map(one, cmds):
            log.write(f'[{"FAILED" if rc else "ok"}] {cmd}\n{text_}')
            if rc:
                bad.append((out, cmd, text_))
    return cmds, bad, (top, rules, link)


SYSTEM_DLLS = {d.lower() for d in (   # DLLs every Windows 10/11 has (the exe is linked -static otherwise); also
    # api-ms-win-crt-* (the Universal CRT of CLANG64, part of Windows 10+) and api-ms-win-core-*
    'kernel32.dll', 'user32.dll', 'gdi32.dll', 'advapi32.dll', 'shell32.dll', 'ole32.dll', 'oleaut32.dll', 'imm32.dll',
    'winmm.dll', 'version.dll', 'setupapi.dll', 'dinput8.dll', 'bcrypt.dll', 'dbghelp.dll', 'msvcrt.dll', 'cfgmgr32.dll',
    'api-ms-win-core-synch-l1-2-0.dll', 'ntdll.dll', 'shlwapi.dll', 'uuid.dll', 'ws2_32.dll', 'hid.dll')}


def link_check(tree, link_info, path_env, log, llvm):
    top, rules, link = link_info
    scope = dict(top)
    scope.update({k: v for k, v in link.items() if not k.startswith('_')})
    scope['out'] = 'build/check-link.exe'
    rsp = tree / 'build/check-link.exe.rsp'
    rsp.write_text(link['_in'])
    cmd = expand(rules['link']['command'], scope).replace('@$out.rsp', f'@{rsp}')
    cmd = cmd.replace('@build/check-link.exe.rsp', f'@{rsp}')
    cmd += ' -Wl,--Map=build/check-link.map'   # [UIT 0257] symbol addresses for ap_ranges_check (layout unchanged)
    r = subprocess.run(cmd, shell=True, cwd=tree, env=dict(os.environ, PATH=path_env), capture_output=True, text=True)
    out = r.stdout + r.stderr
    log.write(f'[link rc={r.returncode}] {cmd}\n{out}')
    undefined = sorted(set(re.findall(r'undefined symbol: (\S+)', out)))
    other = [l for l in out.splitlines() if 'error:' in l and 'undefined symbol' not in l
             and 'errors omitted' not in l]
    dlls = []
    exe = tree / 'build/check-link.exe'
    if r.returncode == 0 and exe.is_file():
        ro = subprocess.run([str(llvm / 'llvm-readobj'), '--coff-imports', str(exe)], capture_output=True, text=True)
        dlls = sorted(set(re.findall(r'Name: (\S+\.dll)', ro.stdout, re.I)), key=str.lower)
        log.write('imports: ' + ' '.join(dlls) + '\n')
    return r.returncode, undefined, other, dlls


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--builder', help='deps/clang64 folder, the builder package folder, or its zip')
    ap.add_argument('--fetch', action='store_true', help='download the builder package if not found')
    ap.add_argument('--clang', help='bin folder of a host LLVM with the builder clang\'s major version')
    ap.add_argument('-j', type=int, default=os.cpu_count() or 4)
    ap.add_argument('--keep', action='store_true', help='keep the temporary tree')
    ap.add_argument('--no-link', action='store_true')
    a = ap.parse_args()

    sysroot = find_builder(a.builder, a.fetch)
    major = sorted(p.name for p in (sysroot / 'lib/clang').iterdir())[-1]
    llvm, ver = find_clang(a.clang, major)
    say(f'sysroot  {sysroot}\nclang    {llvm}/clang {ver} as x86_64-w64-windows-gnu (builder: clang {major})')
    tmp = pathlib.Path(tempfile.mkdtemp(prefix='uit-wincheck-'))
    failures = 0
    try:
        tree = tmp / 'tree'
        export_tree(tree)
        wrappers(tmp / 'bin', llvm, sysroot, major)
        path_env = f'{tmp / "bin"}{os.pathsep}{os.environ.get("PATH", "")}'
        logp = ROOT / 'dev-local/check-toolchain/last.log'
        logp.parent.mkdir(parents=True, exist_ok=True)
        with open(logp, 'w') as log:
            for lto, no_dlss in (('thin', False), ('off', False), ('off', True)):
                text = gen_ninja(tree, sysroot, lto, no_dlss)
                shutil.rmtree(tree / 'build/obj', ignore_errors=True)
                shutil.rmtree(tree / 'build/game', ignore_errors=True)
                cmds, bad, link_info = run_edges(tree, text, path_env, a.j, log)
                tag = f'LTO {lto}' + (', --no-dlss' if no_dlss else '')
                say(f'[{tag}] compiled {len(cmds) - len(bad)} of {len(cmds)} objects (every runtime source, the '
                    f'resource file, 4 stand-in game files) with the builder\'s headers')
                for out, cmd, t in bad:
                    failures += 1
                    say(f'  FAILED {out}\n    {cmd}\n' + ''.join(f'    {l}\n' for l in t.strip().splitlines()[:12]))
                if not bad:
                    fbad, fsum = frame_check(tree, text, log)
                    failures += len(fbad)
                    say(f'  stack frames (tools/check_frames.py, limit {FRAME_LIMIT >> 20} MiB): the 16,000-instruction '
                        f'-O0 stand-in needs {fsum}' + ('' if fbad else '; every COFF game object is under the limit'))
                    for l in fbad:
                        say(f'  FRAME CHECK FAILED: {l}')
                if bad or a.no_link:
                    continue
                rc, undefined, other, dlls = link_check(tree, link_info, path_env, log, llvm)
                odd = [d for d in dlls if d.lower() not in SYSTEM_DLLS and not d.lower().startswith(('api-ms-win-crt-', 'api-ms-win-core-'))]
                if rc or undefined or other or odd:
                    failures += 1
                    say(f'  LINK FAILED (rc {rc}): {len(undefined)} undefined symbol(s), {len(other)} other error(s)'
                        + (f', DLLs that are not part of Windows: {" ".join(odd)}' if odd else ''))
                    for s_ in undefined[:60]:
                        say(f'    undefined: {s_}')
                    for l in other[:30]:
                        say(f'    {l}')
                else:
                    reserve, commit = pe_stack_reserve(tree / 'build/check-link.exe')
                    if reserve != 64 << 20:
                        failures += 1
                        say(f'  STACK RESERVE FAILED: the exe reserves {reserve:,} bytes per thread, not 64 MiB (--stack)')
                    else:
                        say(f'  thread stacks: 64 MiB reserved per thread, {commit // 1024} KiB committed up front (--stack)')
                    sub = pe_subsystem(tree / 'build/check-link.exe')
                    if sub != 2:   # [UIT 0259] a console program gets a console window when started without handles
                        failures += 1
                        say(f'  PE SUBSYSTEM FAILED: {sub}, not 2 (Windows GUI): the game would open a console window')
                    else:
                        say('  PE subsystem 2 (Windows GUI): no console window, main() through WinMainCRTStartup')
                    old, rng, inside, lifted = ap_ranges_check(tree / 'build/check-link.exe', tree / 'build/check-link.map')
                    if inside or not rng:
                        failures += 1
                        say(f'  ASYNC PREEMPTION RANGES FAILED: {len(rng)} exact range(s) for {len(lifted)} lifted '
                            f'function(s); host code inside: {" ".join(inside) or "-"}')
                    else:
                        say(f'  async preemption (src/host/ap_ranges.h): {len(rng)} exact lifted range(s) from the unwind '
                            f'table, no host code inside; the old single range '
                            + (f'holds {len(old)} host functions ({" ".join(old)}), so it refused' if old
                               else 'holds no host code either'))
                    crt = [d for d in dlls if d.lower().startswith('api-ms-win-crt-')]
                    say(f'  linked ultratears.exe with the build\'s own link command and libraries: no undefined '
                        f'symbol; it imports only Windows DLLs: '
                        f'{" ".join(d for d in dlls if d not in crt)} + {len(crt)} api-ms-win-crt-* (UCRT)')
            # the launcher (setup.py builds it after the game with tools/build_launcher.py and the same clang.exe)
            out = tree / 'build/check-launcher.exe'
            r = subprocess.run([sys.executable, 'tools/build_launcher.py', '--target', 'windows', '-j', str(a.j),
                                '--cxx', f'{tmp / "bin" / "clang"} --driver-mode=g++',
                                '--rc', f'{tmp / "bin" / "llvm-windres"} --target=pe-x86-64', '--out', str(out)],
                               cwd=tree, env=dict(os.environ, PATH=path_env), capture_output=True, text=True)
            log.write(f'[launcher rc={r.returncode}]\n{r.stdout}{r.stderr}')
            if r.returncode or not out.is_file() or 'without its icon' in r.stdout:
                failures += 1
                say('[launcher] FAILED:\n' + ''.join(f'    {l}\n' for l in (r.stdout + r.stderr).strip().splitlines()[-25:]))
            else:
                sub = pe_subsystem(out)
                if sub != 2:
                    failures += 1
                    say(f'[launcher] PE SUBSYSTEM FAILED: {sub}, not 2 (Windows GUI)')
                say(f'[launcher] tools/build_launcher.py: compiled, resources and link ok ({out.stat().st_size // 1024} KB)')
        say(f'log: {logp}')
    finally:
        if a.keep:
            say(f'tree kept: {tmp}')
        else:
            shutil.rmtree(tmp, ignore_errors=True)
    say('RESULT: ' + ('PASS' if not failures else f'FAIL ({failures})'))
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
