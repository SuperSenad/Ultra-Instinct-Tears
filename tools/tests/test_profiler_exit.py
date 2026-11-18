#!/usr/bin/env python3
"""[UIT 0363] The CPU profiler (src/host/profiler.c, Windows only, so checked in the source here; the toolchain
check compiles and links it):
  - closing the game writes a final report: window_exit stops sampling first and waits a bounded time for the
    report after the save drain, before the log flush and _Exit; the sampler waits on the stop event instead of
    sleeping; a game closed before sampling starts still signals done;
  - reports are written to <path>.tmp and moved over the file whole;
  - "passes a second" divides by the time actually sampled;
  - the game's main thread is picked by samples in recompiled game code (f_...) and kept once picked.
    python3 tools/tests/test_profiler_exit.py"""
import pathlib, re, sys

root = pathlib.Path(__file__).resolve().parents[2]
prof = (root / 'src/host/profiler.c').read_text()
win = (root / 'src/host/window_sdl.c').read_text()
host = (root / 'src/host/host.h').read_text()
fails = []
def check(cond, what):
    if not cond: fails.append(what)

check('void runtime_profiler_stop(void);' in host and 'void runtime_profiler_finish(unsigned max_ms);' in host, 'host.h declares stop/finish')
body = win[win.index('static _Noreturn void window_exit'):]
body = body[:body.index('\n}\n')]
order = ['SDL_CreateThread(exit_watchdog', 'runtime_profiler_stop();', 'uit_overlay_shutdown();', 'uit_save_drain();',
         'runtime_profiler_finish(', 'fflush(stderr);', '_Exit(exit_code);']
pos = [body.find(x) for x in order]
check(all(p >= 0 for p in pos) and pos == sorted(pos), 'window_exit order: watchdog, stop, ..., save drain, finish, flush, _Exit')
# [UIT 0444] the wait for the final report: until 1 s before the 12 s watchdog (at least 4 s), so it is bounded under it
check('runtime_profiler_finish(exit_ms + 5000 < EXIT_WATCHDOG_MS ? (unsigned)(EXIT_WATCHDOG_MS - 1000 - exit_ms) : 4000);' in body
      and 'uint64_t exit_t0 = SDL_GetTicks();' in body and '#define EXIT_WATCHDOG_MS 12000' in win,
      'finish is bounded under the exit watchdog (its time left minus 1 s, at least 4 s)')
# [UIT 0444] an earlier run's report never looks like this run's; the symbols load before sampling, not in the final report
start = prof[prof.index('void runtime_profiler_start(void)'):]; start = start[:start.index('\n}\n')]
check('-previous%s' in start and 'MoveFileExA(out_path, prev, MOVEFILE_REPLACE_EXISTING)' in start and 'DeleteFileA(out_path)' in start,
      'arming moves the previous run\'s report aside (or removes it)')
check(start.index('-previous%s') < start.index('CreateThread('), 'it is moved aside before sampling starts')
_s = prof[prof.index('static DWORD WINAPI profiler('):]
check(_s.index('sym_init(GetCurrentProcess())') < _s.index('"[prof] sampling for'), 'symbols load before sampling')
fin = prof[prof.index('void runtime_profiler_finish('):]
check('reports_written ?' not in fin and 'no report of this run' in fin and "this run's last 5-minute report" in fin,
      'a late final report: the log says what the file holds')
check('reports_written++;' in prof[prof.index('static void report('):prof.index('static double start_s')], 'reports of this run are counted')
sampler = prof[prof.index('static DWORD WINAPI profiler('):]
sampler = sampler[:sampler.index('\n}\n')]
check('Sleep(' not in sampler, 'the sampler waits on the stop event, never Sleep')
check(sampler.count('WaitForSingleObject(stop_ev') == 2, 'start delay and the 1 ms pacing wait on stop_ev')
check(sampler.count('SetEvent(done_ev)') == 2, 'done is signalled on both exits of the sampler')
check('passes / duration_s' not in sampler and 'passes / (sampled' in sampler, 'passes a second uses the sampled time')
rep = prof[prof.index('static void report('):prof.index('static double start_s')]
check('"%s.tmp"' in rep and 'fopen(tmp, "w")' in rep and 'MoveFileExA(tmp, path, MOVEFILE_REPLACE_EXISTING)' in rep,
      'report goes to .tmp and replaces the file whole')
check('fopen(path' not in rep, 'report never opens the final path directly')
check('if (!armed || !done_ev) return;' in prof and 'if (armed && stop_ev) SetEvent(stop_ev);' in prof,
      'stop/finish are no-ops with the profiler off')
check('main_tid = tids[main_th]' in rep and 'tids[th] == main_tid' in rep, 'the main thread is kept once picked')
check('!strncmp(name, "f_", 2)) game_samples[' in rep, 'the main thread is picked by recompiled game code samples')
check('thread_samples[th] - thread_wait[th] >\n' not in rep, 'the old busiest-thread pick is gone')

for f in fails: print('FAIL:', f)
print('all ok' if not fails else f'{len(fails)} failure(s)')
sys.exit(1 if fails else 0)
