"""[UIT 0259] Play and Play with log start the game the same way, and the game never has a console window.

    python3 tools/tests/test_play_console.py [UltraInstinctTears.exe ...]

Source checks (the box cannot run Windows programs): build_game.py links the game, and only the game, for the Windows
GUI subsystem; the launcher's start_game gives the game the same flags and inherited handles in both modes, NUL for
Play; the game's own log (lasrecomp.log, started without the launcher) is never opened in append mode and is buffered
and written by its own thread, as is the disk file Play with log gives it [UIT 0362], whose LOG lines are each one write;
the ways out that skip the CRT's flush flush it first. Every exe named on the command
line must have PE subsystem 2 (Windows GUI); tools/tests/check_windows_toolchain.sh checks the linked exe the same way.
"""
import pathlib, re, sys
ROOT = pathlib.Path(__file__).resolve().parents[2]
ok = True


def check(c, what):
    global ok
    print(('ok   ' if c else 'FAIL ') + what)
    ok &= bool(c)


def pe_subsystem(exe):
    d = pathlib.Path(exe).read_bytes()
    pe = int.from_bytes(d[0x3c:0x40], 'little')
    assert d[pe:pe + 4] == b'PE\0\0', f'{exe}: not a PE file'
    return int.from_bytes(d[pe + 24 + 68:pe + 24 + 70], 'little')


bg = (ROOT / 'tools/build_game.py').read_text()
check("GUI_SUBSYSTEM = '-Wl,--subsystem,windows'" in bg, 'build_game.py: GUI_SUBSYSTEM is -Wl,--subsystem,windows')
check(re.search(r"rule link',\n\s+f'  command = .*\$subsystem'", bg) is not None, 'build_game.py: the link rule takes $subsystem')
check(re.search(r"if a\.target == 'ultratears':.*\n\s+lines\.append\(f'  subsystem = \{GUI_SUBSYSTEM\}'\)", bg) is not None,
      'build_game.py: only the game (--target ultratears) is a GUI program; probes and tests stay console programs')
check('--subsystem' not in bg.split('LIBS = ', 1)[1].split('\n\n', 1)[0].split('GUI_SUBSYSTEM')[0],
      'build_game.py: LIBS itself has no subsystem (shared by every --target)')

pc = (ROOT / 'launcher/platform.cpp').read_text()
sg = pc[pc.index('bool start_game('):]
sg = sg[:sg.index('\n}\n')]
check('CreateFileW(L"NUL", GENERIC_WRITE' in sg and 'OPEN_EXISTING' in sg, 'start_game: Play opens NUL for the game')
check(sg.count('SECURITY_ATTRIBUTES sa = {sizeof sa, nullptr, TRUE}') == 1 and sg.index('SECURITY_ATTRIBUTES') < sg.index('if (!log.empty())'),
      'start_game: NUL and the log file are both inheritable (one SECURITY_ATTRIBUTES)')
flags = re.search(r'CreateProcessW\(nullptr, &cmd\[0\], nullptr, nullptr, (\w+), (\w+),', sg)
check(flags is not None and flags.groups() == ('TRUE', 'CREATE_NO_WINDOW'),
      'start_game: one CreateProcessW, handles inherited and CREATE_NO_WINDOW in both modes')
tail = sg[sg.index('} else {'):]
check('si.dwFlags = STARTF_USESTDHANDLES;' in tail and 'si.hStdOutput = logh;' in tail and 'si.hStdError = logh;' in tail,
      'start_game: standard output and error given in both modes (after the if / else)')
check('CloseHandle(logh);' in sg and 'if (logh != INVALID_HANDLE_VALUE) CloseHandle' not in sg, 'start_game: the handle is closed after the start')

w = (ROOT / 'src/runtime/platform/windows.c').read_text()
boot = (ROOT / 'src/host/boot.c').read_text()
lg = w[w.index('static void log_to_file_without_console(void) {'):]
lg = lg[:lg.index('\n}\n')]
check('"ab"' not in lg and 'LASRECOMP_LOG_APPEND' not in w, 'lasrecomp.log: never append mode (UCRT seeks to the end before every write)')
check('freopen(g_log_path, "wb", stderr)' in lg, 'lasrecomp.log: made new each run')
check(lg.count('setvbuf(stderr, NULL, _IOFBF, 1 << 16)') == 2, 'NUL and lasrecomp.log: 64 KiB buffered, no system call per character')
check('is_nul_device(h)' in lg and 'GetConsoleMode' in w, 'NUL told apart from a console (a console or pipe stays unbuffered)')
fl = w[w.index('static bool start_log_flusher(void) {'):]
fl = fl[:fl.index('\n}\n')]
check('CreateThread(NULL, 64 << 10, log_flusher' in fl and 'THREAD_PRIORITY_BELOW_NORMAL' in fl and 'Sleep(200);' in w
      and lg.count('start_log_flusher()') == 2, 'lasrecomp.log and a disk file: written out by a below-normal thread every 200 ms')
# [UIT 0362] Play with log: the disk file the launcher gives is buffered (1 MiB) and written by that thread
check('GetFileType(h) == FILE_TYPE_DISK && !log_unbuffered_wanted()' in lg and 'setvbuf(stderr, NULL, _IOFBF, 1 << 20)' in lg,
      'a given disk file (Play with log): 1 MiB buffered unless TOTK_LOG_UNBUFFERED=1')
disk = lg[lg.index('FILE_TYPE_DISK'):lg.index('return;')]
check(disk.index('start_log_flusher()') < disk.index('g_log_buffered = 1') and 'setvbuf(stderr, NULL, _IONBF, 0)' in disk,
      'a given disk file: unbuffered again when the writer thread cannot start')
check(lg.count('signal(SIGABRT, log_flush_on_abort)') == 2 and 'FILE_TYPE_PIPE' not in lg,
      'abort() flushes a buffered log file; pipes stay unbuffered')
ls = w[w.index('void plat_log_settings(void) {'):]
ls = ls[:ls.index('\n}\n')]
check('fflush(stderr)' in ls and 'setvbuf(stderr, NULL, _IONBF, 0)' in ls and 'g_log_buffered = 0' in ls,
      'TOTK_LOG_UNBUFFERED=1 in the settings file: flushed, then unbuffered')
check(boot.index('default_exefs()') < boot.index('plat_log_settings();') < boot.index('runtime_profiler_start();'),
      'plat_log_settings after the settings file is read, before any other thread')
# [UIT 0362] whole lines: no LOG macro writes its line end as a second call
import re as _re
split = [str(f.relative_to(ROOT)) for f in (ROOT / 'src').rglob('*') if f.suffix in ('.c', '.h', '.cpp')
         and _re.search(r"#define \w+\(\.\.\.\).*fputc\('\\n', ?stderr\)", f.read_text(errors='replace'))]
check(not split, 'every LOG macro writes its line in one call (uit_logline)' + (': ' + ', '.join(split) if split else ''))
term = w[w.index('void plat_terminate(void) {'):]
check(term.index('fflush(stderr)') < term.index('TerminateProcess'), 'plat_terminate flushes before TerminateProcess')
ex = w[w.index('plat_exec('):]
ex = ex[:ex.index('CreateProcessW')]
check('STARTF_USESTDHANDLES' in ex and '_get_osfhandle(_fileno(stderr))' in ex, 'plat_exec passes its standard handles on')
check('_set_abort_behavior(0, _WRITE_ABORT_MSG)' in w, 'abort(): no second CRT message box after plat_fatal_notice')
check(re.search(r'fflush\(stderr\);\s*_exit\(86\)', (ROOT / 'src/gfx/nvn/hle_nvn.c').read_text()) is not None,
      'memory watchdog flushes the log before _exit')
check(re.search(r'fflush\(stderr\);\s*plat_fatal_notice\(\);\s*abort\(\);', (ROOT / 'src/runtime/dispatch.c').read_text()) is not None,
      'fatal errors and crash reports flush the log (rt_fatal_trace)')
check(boot.index('plat_init();') < boot.index('"Ultra Instinct Tears %s (commit %s)'), 'plat_init before the version line: lasrecomp.log starts with it')
check("(ROOT / 'lasrecomp.log')" in (ROOT / 'tools/report.py').read_text(), 'Report a problem adds lasrecomp.log')

for exe in sys.argv[1:]:
    sub = pe_subsystem(exe)
    check(sub == 2, f'{exe}: PE subsystem {sub} (2 = Windows GUI, 3 = console)')
print('all ok' if ok else 'FAILURES')
sys.exit(0 if ok else 1)
