"""Write build/build.ninja for the runtime + the generated game code, and run ninja (MSYS2 CLANG64 clang/ninja).

usage: python tools/build_game.py [--generated data/generated] [--target probe_cpu] [-j N] [--cpus N] [--no-dlss]
                                 [--opt O2|O3] [--lto off|thin] [--pgo off|gen|use] [--profile build/pgo/uit.profdata]
--opt, --lto, --pgo (tools/setup.py --O3, --lto, --pgo-profile / --pgo): compiler options for speed, everything else
the same. --pgo gen builds the profiling exe (counters in build/pgo/*.profraw, src/host/pgo.c), --pgo use builds with
the merged profile (llvm-profdata merge, which setup.py runs).
Every object is compiled first, then the exe is linked. A failed ThinLTO link is tried once more with a quarter of the
link jobs; if it fails again the exit code is 3 (tools/setup.py then builds again with the normal link).
The ultratears target links UltraInstinctTears.exe in the game folder itself (its debug symbols:
build/UltraInstinctTears.pdb).
Run in the game folder (tools/setup.py does): the generated C is in data/ (derived from the game), objects and
executables go to build/; neither is ever in git.
"""
import argparse
import os
import pathlib
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'tools'))
from toolchain import CLANG64, env as tool_env  # noqa: E402
import version  # noqa: E402

RUNTIME = ['src/runtime/dispatch.c', 'src/runtime/loader.c', 'src/runtime/platform/windows.c']
TARGETS = {'probe_cpu': ['src/probe/probe_cpu.c'],
           'ultratears': ['src/host/boot.c', 'src/host/guest.c', 'src/host/svc.c', 'src/host/threads.c', 'src/host/preempt_win.c', 'src/host/ipc.c',
                    'src/host/services_lm.c', 'src/host/overrides_oe.c',
                    'src/host/overrides_time.c',
                    'src/host/fs.c', 'src/host/modfs.c', 'src/host/restbl.c', 'src/host/uit_display.c', 'src/host/uit_lift.c', 'src/host/romfix.c', 'src/host/zstd_native.c', 'src/host/native_libc.c', 'src/host/save_txn.c', 'src/host/account.c',
                    'src/host/overrides_applet.c', 'src/host/overrides_input.c',
                    'src/host/audio_renderer.c', 'src/host/audio_mixer.c', 'src/host/audio_effects.c',
                    'src/host/audio_reverb.c', 'src/host/opus_codec.c', 'src/host/movie_host.c', 'src/host/hid_feedback.c',
                    'src/host/shared_font.c',
                    'src/host/window_sdl.c', 'src/vk/vk.c', 'src/vk/present.c', 'src/vk/present_fsr.c', 'src/vk/dlss.c', 'src/host/motion_fusion.c',
                    'src/host/profiler.c', 'src/host/watch.c', 'src/host/fps_unlock.c', 'src/host/pgo.c',
                    'src/host/uit_overlay.c', 'src/host/overlay_live.c', 'src/vk/overlay_vk.c',
                    'src/host/input_bind.c',
                    'src/host/amiibo.c', 'src/host/nfp.c', 'src/host/cheats.c',
                    'src/host/swkbd.c', 'src/host/swkbd_host.c']}   # [UIT 0171/0172] the in-game overlay; [UIT 0174] its Controls tab; [UIT 0175] amiibo from files, cheats; [UIT 0177] the software keyboard
# The graphics front end (MacNativeGames' NVN layer) with its macOS stand-ins: own include paths, own rule
GFX = ['src/gfx/nvn/hle_nvn.c', 'src/gfx/nvn/nvn_tex.c', 'src/gfx/nvn/totk_nvn_ext.c', 'src/gfx/nvn/nvn_registry.c',
       'src/gfx/nvn/nvn_thunks.gen.c', 'src/gfx/nvn/nvn_dirty_win.c', 'src/gfx/nvn/totk_timeline_win.c',
       'src/gfx/nvn/nvn_names.gen.c', 'src/gfx/backend_null.c', 'src/gfx/gfx_bridge.c',
       'src/gfx/front_win.c', 'src/gfx/nvn/totk_vpool_win.c', 'src/gfx/backend_vk.c', 'src/gfx/vkb_tex.c', 'src/gfx/vkb_shader.c', 'src/gfx/vkb_spv.c', 'src/gfx/vkb_audit.c', 'src/gfx/vkb_copy.c', 'src/gfx/vkb_desc.c', 'src/gfx/vkb_shadow.c', 'src/gfx/vkb_ucap.c', 'src/gfx/vkb_gpuw.c', 'src/gfx/uit_prof.c',
       'src/gfx/vkb_draw.c', 'src/gfx/tex/nvn_deswizzle.c', 'src/gfx/tex/astc.c', 'src/gfx/vkb_bc7.c',
       'src/gfx/tex/bc7.cpp', 'src/gfx/tex/bc7enc/bc7enc.cpp', 'src/gfx/tex/bc7enc/rgbcx.cpp', 'src/gfx/tex/bc7key.c',
       'src/gfx/vkb_dlss.c', 'src/vk/dlss_sl.c',
       'src/gfx/vkb_mem.c', 'src/gfx/vkb_pfor.c']   # [UIT 0075] texture memory ([UIT 0104] our own suballocator, src/gfx/vkb_suballoc.h)
GFX_INCLUDES = '-Isrc/gfx -Isrc/gfx/nvn -Isrc/gfx/nvn/include -Isrc/gfx/compat'
# [UIT 0235] the graphics code compiles without warnings (-Wno-everything, was -w), except the few that are always a bug:
# a function that calls itself on every path (0232's img_unlock did: black screen at boot) stops the build. (-w would
# silence these errors too.)
GFX_WARN = '-Wno-everything -Werror=infinite-recursion'


LIBS = '-static -Wl,--error-limit=0 -Wl,--disable-high-entropy-va -pthread -ldbghelp -lwinmm -lsynchronization -lshell32 -lopus -lvpx -lzstd -lSDL3 -liconv' \
       ' -luser32 -lgdi32 -limm32 -lole32 -loleaut32 -lversion -luuid -ladvapi32 -lsetupapi -ldinput8 -lbcrypt -lc++'
# [UIT 0121] every thread's default stack: 64 MiB reserved, committed only as it is used (the PE header's stack reserve;
# the commit stays the default page). The game's threads take this default (src/runtime/platform/windows.c
# plat_guest_stack_size reads it back and = PLAT_GUEST_STACK): room for any -O0 frame, with a guard page so an overflow
# is reported as one.
LIBS += ' -Wl,--stack,67108864'
# [UIT 0259] UltraInstinctTears.exe is linked for the Windows GUI subsystem: started without standard handles (the
# launcher's Play before 0259, a double-click) a console program gets a console window from Windows, and every line the
# game logged went to it one character at a time, stalling the threads that log (Play with log, which gives it a file,
# ran smoother). main() still runs: MinGW's WinMainCRTStartup calls it with argc / argv. The helper programs built by
# --target (probes, tests) stay console programs: their output is read from the console.
GUI_SUBSYSTEM = '-Wl,--subsystem,windows'
CFLAGS = '-O2 -march=x86-64-v3 -g -gcodeview -fno-strict-aliasing -D_FILE_OFFSET_BITS=64'
GAME_CFLAGS = CFLAGS + ' -w -Isrc/runtime'


def speed_flags(a):
    """[UIT 0019] --opt / --lto / --pgo: added to every compile (and the link); the huge -O0 game files stay as they
    are (no LTO, no instrumentation: their -O0 is about compile time, see below)."""
    global CFLAGS, GAME_CFLAGS, GAME_O0_FLAGS, LINK_EXTRA
    GAME_O0_FLAGS = GAME_CFLAGS.replace('-O2', '-O0') + O0_OUTLINE
    extra, link = [], []
    if a.opt == 'O3':
        CFLAGS = CFLAGS.replace('-O2', '-O3')
    if a.lto == 'thin':
        extra.append('-flto=thin')
        link += ['-flto=thin', '-Wl,--thinlto-cache-dir=build/lto-cache', '-Wl,--thinlto-cache-policy=cache_size_bytes=16g',
                 '-Wl,--thinlto-jobs=$ltojobs', f'-O{a.opt[1]}']
    if a.pgo == 'gen':
        extra += ['-fprofile-generate', '-DUIT_PGO_GEN']
        link.append('-fprofile-generate')
    elif a.pgo == 'use':
        prof = (ROOT / a.profile)
        if not prof.is_file():
            sys.exit(f'--pgo use: {a.profile} not found (tools/setup.py --pgo merges build/pgo/*.profraw into it)')
        # a stale or partial profile only loses speed where code changed: no warnings for each such function
        extra += [f'-fprofile-use={pathlib.Path(a.profile).as_posix()}', '-Wno-profile-instr-unprofiled',
                  '-Wno-profile-instr-out-of-date', '-Wno-profile-instr-missing', '-Wno-backend-plugin']
    CFLAGS = ' '.join([CFLAGS] + extra)
    GAME_CFLAGS = CFLAGS + ' -w -Isrc/runtime'
    LINK_EXTRA = ' '.join(link)
    if a.opt != 'O2' or a.lto != 'off' or a.pgo != 'off':
        print(f'speed options: {a.opt}, LTO {a.lto}, PGO {a.pgo}' + (f' ({a.profile})' if a.pgo == 'use' else ''), flush=True)


# [UIT 0121] The huge -O0 files (below) call runtime/simd.h's FP / vector helpers instead of expanding them: -O0 gives
# every always_inline expansion its own stack slots and never reuses one, so a function with tens of thousands of FP
# instructions needed an 8.4 MiB frame (the 0x80559cc20 crash: ___chkstk_ms ran off an 8 MiB thread stack). Outlined,
# a lifted stand-in's frame is 13-19x smaller (16K instructions, 30% SIMD: 1,765,544 -> 136,424 bytes) and the file
# compiles 6-7x faster; the helpers are the same code (tools/tests/test_fpcr_host.c built -O2 inline against -O0
# outlined: bit-identical). A define on the command line, not an edit of simd.h: simd.h stays byte-identical, so an
# update recompiles only these files, not every game file. tools/check_frames.py checks the frames after the build.
O0_OUTLINE = ' -Dalways_inline=noinline'
GAME_O0_FLAGS = GAME_CFLAGS.replace('-O2', '-O0') + O0_OUTLINE
LINK_EXTRA = ''


def rel(p):
    return pathlib.Path(os.path.relpath(p, ROOT)).as_posix()


def write_if_changed(path, text):   # an unchanged file keeps its time: nothing rebuilds
    if not path.is_file() or path.read_text(encoding='utf-8') != text:
        path.write_text(text, encoding='utf-8')


def icon_header(build):
    """build/uit_icon.h: assets/uit-256.png as bytes, so the window gets its icon at run time (SDL_LoadPNG_IO) even
    when the exe was linked without the resource (no resource compiler)."""
    data = (ROOT / 'assets/uit-256.png').read_bytes()
    rows = [','.join(str(b) for b in data[i:i + 32]) for i in range(0, len(data), 32)]
    write_if_changed(build / 'uit_icon.h', '// generated by tools/build_game.py from assets/uit-256.png: do not edit\n'
                     f'static const unsigned char kUitIconPng[] = {{\n' + ',\n'.join(rows) + '};\n')


def font_header(build):
    """[UIT 0171] build/uit_font.h: the launcher's fonts (third_party/inter, SIL OFL 1.1) as bytes for the in-game
    overlay's font atlas (src/host/uit_overlay.c, built the first time the overlay is shown)."""
    out = ['// generated by tools/build_game.py from third_party/inter: do not edit\n']
    for name, f in (('kInterRegular', 'Inter-Regular.ttf'), ('kInterSemiBold', 'Inter-SemiBold.ttf')):
        data = (ROOT / 'third_party/inter' / f).read_bytes()
        rows = [','.join(str(b) for b in data[i:i + 32]) for i in range(0, len(data), 32)]
        out.append(f'static const unsigned char {name}[] = {{\n' + ',\n'.join(rows) + '};\n')
    write_if_changed(build / 'uit_font.h', ''.join(out))


def version_files(build):
    """build/version.h (the version and commit, tools/version.py) and the exe's resources: icon and version info."""
    v, commit = version.current()
    n = ','.join(str(x) for x in version.numbers(v))
    write_if_changed(build / 'version.h', f'#define ULTRS_VERSION "{v}"\n#define ULTRS_COMMIT "{commit}"\n')
    write_if_changed(build / 'uit.rc', f'''1 ICON "assets/uit.ico"
1 VERSIONINFO
FILEVERSION {n}
PRODUCTVERSION {n}
BEGIN
  BLOCK "StringFileInfo"
  BEGIN
    BLOCK "040904B0"
    BEGIN
      VALUE "CompanyName", "Ultra Instinct Tears contributors (based on UltraTears by kuraeow)"
      VALUE "FileDescription", "Ultra Instinct Tears"
      VALUE "FileVersion", "{v} ({commit})"
      VALUE "LegalCopyright", "GPL-3.0-or-later. Not affiliated with Nintendo."
      VALUE "OriginalFilename", "UltraInstinctTears.exe"
      VALUE "ProductName", "Ultra Instinct Tears"
      VALUE "ProductVersion", "{v}"
    END
  END
  BLOCK "VarFileInfo"
  BEGIN
    VALUE "Translation", 0x409, 1200
  END
END
''')
    return v


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--generated', default='data/generated')
    ap.add_argument('--target', default='probe_cpu')
    ap.add_argument('-j', type=int, default=0, help='parallel jobs (default: --cpus)')
    ap.add_argument('--cpus', type=int, default=8, help='logical CPUs the build may use (affinity); the rest stay free for the user')
    ap.add_argument('--no-dlss', action='store_true', help='build src/vk/dlss_sl.c as stubs (TOTK_UPSCALER=dlss then runs FSR)')
    ap.add_argument('--opt', choices=('O2', 'O3'), default='O2', help='optimization level (default O2)')
    ap.add_argument('--lto', choices=('off', 'thin'), default='off', help='ThinLTO: inlining across files at the link')
    ap.add_argument('--pgo', choices=('off', 'gen', 'use'), default='off',
                    help='profile-guided optimization: gen = the profiling build, use = optimize with --profile')
    ap.add_argument('--profile', default='build/pgo/uit.profdata', help='the merged profile for --pgo use')
    a = ap.parse_args()
    speed_flags(a)
    a.j = a.j or a.cpus
    gen = ROOT / a.generated
    build = ROOT / 'build'
    build.mkdir(exist_ok=True)
    lines = [
        'builddir = build',   # ninja's own state (.ninja_log, .ninja_deps) in build/, not beside the sources
        f'cflags = {CFLAGS} -Wall -Wno-unused-function -Werror=infinite-recursion -Ibuild',   # build/version.h (the game's code: not, so a new version rebuilds nothing of it)
        f'gameflags = {GAME_CFLAGS}',
        'rule cc',
        '  command = clang -MD -MF $out.d $cflags -c $in -o $out',
        '  depfile = $out.d',
        '  deps = gcc',
        f'gfxflags = {CFLAGS} {GFX_WARN} {GFX_INCLUDES}',
        'rule gfxcc',
        '  command = clang -MD -MF $out.d $gfxflags -c $in -o $out',
        '  depfile = $out.d',
        '  deps = gcc',
        'rule gfxcxx',   # C++ (the BC7 encoder): no exceptions or RTTI, static libc++ at the link
        '  command = clang++ -MD -MF $out.d $gfxflags -std=c++17 -fno-exceptions -fno-rtti -c $in -o $out',
        '  depfile = $out.d',
        '  deps = gcc',
        'rule gamecc',
        '  command = clang -MD -MF $out.d $gameflags -c $in -o $out',   # runtime headers (cpu.h, simd*.h) are inlined
        '  depfile = $out.d',
        '  deps = gcc',
        'rule rc',
        '  command = llvm-windres --target=pe-x86-64 -I . $in -o $out',
        'rule link',
        f'  command = clang -fuse-ld=lld -g {LINK_EXTRA + " " if LINK_EXTRA else ""}-Wl,--pdb=$pdb -o $out @$out.rsp {LIBS} $subsystem',
        '  rspfile = $out.rsp',
        '  rspfile_content = $in',
        '',
    ]
    objs = []
    for src in RUNTIME + TARGETS[a.target]:
        obj = f'build/obj/{src[:-2].replace("/", "_")}.o'
        lines.append(f'build {obj}: cc {src}')
        objs.append(obj)
    if a.target == 'ultratears':
        # the icon (assets/uit.ico, README.md, Legal) and version in the resources; optional: without a resource
        # compiler the exe links without them and the window still gets its icon at run time (build/uit_icon.h)
        print(f'Ultra Instinct Tears {version_files(build)}', flush=True)
        icon_header(build)
        font_header(build)
        import shutil
        if shutil.which('llvm-windres', path=tool_env().get('PATH')):
            lines.append('build build/obj/uit_res.o: rc build/uit.rc | assets/uit.ico')
            objs.append('build/obj/uit_res.o')
        else:
            print('  note: no llvm-windres found, so the exe is built without its icon resource and version info '
                  '(it works the same; the window still shows the icon)', flush=True)
        for src in GFX:
            cxx = src.endswith('.cpp')
            obj = f'build/obj/{src.rsplit(".", 1)[0].replace("/", "_")}.o'
            lines.append(f'build {obj}: {"gfxcxx" if cxx else "gfxcc"} {src}')
            if src == 'src/host/window_sdl.c':
                lines.append(f'  gfxflags = {CFLAGS} {GFX_WARN} {GFX_INCLUDES} -Ibuild')   # build/uit_icon.h
            if src == 'src/vk/dlss_sl.c':
                # plain C on src/vk/sl_abi.h: no Streamline SDK or C++ standard library needed to build; the SDK's DLLs
                # are loaded at run time (tools/setup.py copies them next to the exe)
                if a.no_dlss:
                    print('DLSS: off (--no-dlss): stubs, TOTK_UPSCALER=dlss runs FSR', flush=True)
                    lines.append(f'  gfxflags = {CFLAGS} {GFX_WARN} {GFX_INCLUDES} -DTOTK_NO_DLSS')
                else:
                    print('DLSS: built in (Streamline 2.14.16 ABI; its DLLs are loaded at run time from the exe\'s folder)', flush=True)
            objs.append(obj)
    game_objs, o0_objs = [], []
    for module in ('main', 'subsdk0', 'sdk'):
        for c in sorted((gen / module).glob('*.c')):
            obj = f'build/game/{module}/{c.stem}.o'
            lines.append(f'build {obj}: gamecc {rel(c)}')
            text = c.read_text(errors='replace')
            # [temporary] a function with an unresolved computed branch is one switch over every
            # instruction; with thousands of labels clang -O1/-O2 takes from minutes to hours per file (main_376:
            # 20+ min at -O1), -O0 15 s. Such functions run unoptimized until their tables resolve. [UIT 0121] With
            # the SIMD helpers called, not expanded (O0_OUTLINE above): -O0 does not reuse stack slots.
            if 'L_dispatch:' in text and text.count(chr(10) + 'L_') > 2000:
                lines.append('  gameflags = ' + GAME_O0_FLAGS)
                o0_objs.append(obj)
            game_objs.append(obj)
    if not game_objs:
        sys.exit(f'no generated C in {gen}: run tools/gen_game.py first')
    exe = 'UltraInstinctTears.exe' if a.target == 'ultratears' else f'build/{a.target}.exe'   # the game: in the game folder
    lines.append(f'build {exe}: link {" ".join(objs + game_objs)}')
    lines.append(f'  pdb = build/{pathlib.Path(exe).stem}.pdb')
    if a.target == 'ultratears':   # [UIT 0259] the game: a GUI program, never a console window (GUI_SUBSYSTEM)
        lines.append(f'  subsystem = {GUI_SUBSYSTEM}')
    lines.append(f'  ltojobs = {a.cpus}')   # ThinLTO's parallel jobs at the link (a retry after a failed link uses fewer)
    lines.append(f'build objs: phony {" ".join(objs + game_objs)}')   # [UIT] everything but the link: compiled first
    lines.append(f'default {exe}')
    text = '\n'.join(lines) + '\n'
    (build / 'build.ninja').write_text(text)
    env = tool_env()
    print(f'{len(game_objs)} generated files, {len(objs)} runtime files', flush=True)
    # The user works on this PC during builds: ninja starts suspended, gets the first --cpus logical CPUs and
    # below-normal priority, then runs; every clang it starts inherits both, so the other CPUs stay free.
    ninja = [str(CLANG64 / 'ninja.exe'), '-C', str(ROOT), '-f', 'build/build.ninja', f'-j{a.j}', '-k', '0']

    def retry_jobs():   # the link command changes (fewer ThinLTO jobs, less memory at once): ninja links again
        (build / 'build.ninja').write_text(text.replace(f'  ltojobs = {a.cpus}\n', f'  ltojobs = {lto_retry_jobs(a.cpus)}\n'))
    rc = compile_then_link(lambda target: run_limited(ninja + ([target] if target else []), env, (1 << a.cpus) - 1),
                           a.lto == 'thin', retry_jobs)
    if rc == 0:
        frame_report(exe, o0_objs)
    sys.exit(rc)


FRAME_WARN = 4 << 20   # [UIT 0121] the game's threads have 64 MiB stacks; a frame above this is reported


def frame_report(exe, o0_objs):
    """[UIT 0121] The largest stack frames after a build (tools/check_frames.py): the -O0 game files by name, everything
    else in the exe. A warning only, the build stands: the threads' 64 MiB stacks hold far more than FRAME_WARN."""
    try:
        import check_frames
        over, allf, _ = check_frames.check([ROOT / o for o in o0_objs] + [ROOT / exe], FRAME_WARN)
        if allf:
            b, n, f = allf[0]
            print(f'stack frames: the largest is {b:,} bytes ({n}, {pathlib.Path(f).name}); {len(o0_objs)} file(s) '
                  f'built at -O0', flush=True)
        for b, n, f in over[:10]:
            print(f'  WARNING: {n} ({pathlib.Path(f).name}) needs a {b:,}-byte stack frame (more than {FRAME_WARN >> 20} '
                  f'MiB; the game still runs, please report it)', flush=True)
    except Exception as e:   # noqa: BLE001 - a report, never a failed build
        print(f'stack frames: not checked ({e})', flush=True)


LTO_LINK_FAILED = 3   # [UIT] exit code: everything compiled, only the ThinLTO link failed (tools/setup.py then
                      # builds again with the normal link)


def lto_retry_jobs(cpus):
    return max(1, cpus // 4)


def compile_then_link(run_ninja, lto, retry_jobs):
    """[UIT] Compile every object first (ninja target objs), then link, so a failed link can be told apart from a
    failed compile. A failed ThinLTO link (most often: not enough memory with many link jobs) is tried once more with
    a quarter of the jobs; when that fails too the exit code is LTO_LINK_FAILED. Returns the exit code."""
    if run_ninja('objs'):
        print('Compiling failed: the lines above say which file and why.', flush=True)
        return 1
    if not run_ninja(None):
        return 0
    if not lto:
        print('Linking failed: the lines above say why.', flush=True)
        return 1
    print('The ThinLTO link failed (the lines above say why; most often it ran out of memory). '
          'Trying the link once more with fewer parallel jobs...', flush=True)
    retry_jobs()
    if not run_ninja(None):
        print('The ThinLTO link worked with fewer parallel jobs.', flush=True)
        return 0
    print('The ThinLTO link failed again.', flush=True)
    return LTO_LINK_FAILED


def run_limited(cmd, env, mask):
    import ctypes
    CREATE_SUSPENDED, BELOW_NORMAL_PRIORITY_CLASS = 0x4, 0x4000
    p = subprocess.Popen(cmd, env=env, creationflags=CREATE_SUSPENDED | BELOW_NORMAL_PRIORITY_CLASS)
    handle = int(p._handle)
    if not ctypes.windll.kernel32.SetProcessAffinityMask(handle, ctypes.c_size_t(mask)):
        p.kill()
        sys.exit('SetProcessAffinityMask failed')
    ctypes.windll.ntdll.NtResumeProcess(handle)
    return p.wait()


if __name__ == '__main__':
    main()
