// Ultra Instinct Tears launcher: the few OS calls it needs (start the game, open a folder, GPU name).
#pragma once
#include "mini.h"

namespace ut {

struct GpuInfo { mini::string name; bool nvidia = false, rtx = false; };
GpuInfo detect_gpu();

struct Game {                          // a running UltraInstinctTears.exe
    void *process = nullptr;
    int exit_code = 0;
    bool running() ;                    // false once it has exited (exit_code is then set)
};

// Starts exe in dir; with a log path, stdout and stderr go to that file. Returns false and sets error on failure.
bool start_game(const mini::string &exe, const mini::string &dir, const mini::string &log, Game *game, mini::string *error);
// [UIT 0036] Runs a command line (the Python helpers tools/update.py and tools/report.py) in dir, without a console
// window, its output into log. The Game struct tracks it like the game.
bool start_process(const mini::string &cmdline, const mini::string &dir, const mini::string &log, Game *proc, mini::string *error);
// [UIT 0320] Ends a helper started by start_process (the start-up update check when Play is pressed); no-op once it exited.
void stop_process(Game *proc);
void open_path(const mini::string &path);           // Explorer / the default editor
bool file_exists(const mini::string &path);
// [UIT 0046] The whole file, opened so another process may rename over it or write it meanwhile ("" if missing).
mini::string read_shared(const mini::string &path);
// [UIT 0431] The whole PC's CPU use in percent since the last call (-1 on the first call or when unknown): the update
// screen shows it as proof that a quiet step is still working.
double cpu_busy();
// [UIT 0431] The CPU use of a helper and every process it started (python.exe, clang.exe, ...), since the last call:
// cores = CPU threads kept busy on average (1.0 = one thread all the time; -1 = unknown, e.g. the first call), procs =
// how many processes, names = the busy ones by name, busiest first ("clang.exe x12, python.exe").
struct TreeCpu { double cores = -1; int procs = 0; mini::string names; };
TreeCpu tree_cpu(Game *root);
int cpu_threads();                                    // logical CPUs
// [UIT 0431] While an update or a build runs the PC does not go to sleep (the screen may still turn off). on = false ends it.
void keep_awake(bool on);
// [UIT 0431] The system's notification sound (a helper finished while the launcher was in the background).
void notify_sound(bool ok);
// [UIT 0431] The last max_bytes of a file at most (the whole file when smaller), opened as read_shared does.
mini::string read_tail(const mini::string &path, long long max_bytes);
mini::string timestamp();                            // 20261029-181502
mini::string join(const mini::string &dir, const mini::string &name);
// Writes launcher-crash.txt (exception, address, module+offset backtrace) next to the launcher if it crashes.
void install_crash_handler();

}  // namespace ut
