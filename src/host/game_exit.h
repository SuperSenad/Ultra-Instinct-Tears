// [UIT 0294] How the game ends, shared by the game (window_sdl.c) and the launcher (launcher/main.cpp). Plain C, no
// includes, so both sides and tools/tests/test_game_exit.c use the same numbers and the same decision.
//   exit code 0                 closed: the window's close button, Alt+F4, or the overlay's Quit game
//   exit code UIT_EXIT_RESTART  the overlay's Restart game, when the launcher started the game (UIT_ENV_LAUNCHER set):
//                               the launcher starts it again the same way (Play or Play with log)
// Without the launcher (UltraInstinctTears.exe started by hand) the game starts itself again and exits 0; the new
// process waits for the old one first (UIT_ENV_RESTART_WAIT: its process id) so two never write the saves at once.
#pragma once

#define UIT_EXIT_RESTART 75                          // EX_TEMPFAIL; not 0, not 86 (the memory watchdog), not an NTSTATUS
#define UIT_ENV_LAUNCHER "UIT_LAUNCHER_RESTART"      // set to 1 by the launcher for the game it starts
#define UIT_ENV_RESTART_WAIT "UIT_RESTART_WAIT_PID"  // set by a game restarting itself, for the new process
#define UIT_RESTART_MIN_S 3.0                        // a restart asked sooner than this after the start is not followed

enum { UIT_END_NORMAL, UIT_END_ERROR, UIT_END_RESTART, UIT_END_RESTART_SKIPPED };

// The launcher's reading of a game's exit: code, and how long the game ran (seconds). A restart is only followed when
// the game ran long enough to have been asked from the overlay (a game that exits with the code at once is not started
// over and over).
static inline int uit_game_end(int code, double ran_s) {
    if (code == 0) return UIT_END_NORMAL;
    if (code == UIT_EXIT_RESTART) return ran_s >= UIT_RESTART_MIN_S ? UIT_END_RESTART : UIT_END_RESTART_SKIPPED;
    return UIT_END_ERROR;
}
