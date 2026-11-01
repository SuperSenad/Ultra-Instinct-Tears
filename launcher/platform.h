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
void open_path(const mini::string &path);           // Explorer / the default editor
bool file_exists(const mini::string &path);
// [UIT 0046] The whole file, opened so another process may rename over it or write it meanwhile ("" if missing).
mini::string read_shared(const mini::string &path);
mini::string timestamp();                            // 20261029-181502
mini::string join(const mini::string &dir, const mini::string &name);
// Writes launcher-crash.txt (exception, address, module+offset backtrace) next to the launcher if it crashes.
void install_crash_handler();

}  // namespace ut
