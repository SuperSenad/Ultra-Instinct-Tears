// [UIT 0294] src/host/game_exit.h: the launcher's reading of the game's exit code (Quit game / Restart game).
// build: cc -O2 -Isrc/host tools/tests/test_game_exit.c -o /tmp/test_game_exit && /tmp/test_game_exit
#include "game_exit.h"

#include <stdio.h>

static int fails, checks;
#define CHECK(c) do { checks++; if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

int main(void) {
    CHECK(uit_game_end(0, 0) == UIT_END_NORMAL);
    CHECK(uit_game_end(0, 3600) == UIT_END_NORMAL);
    CHECK(uit_game_end(UIT_EXIT_RESTART, 60) == UIT_END_RESTART);
    CHECK(uit_game_end(UIT_EXIT_RESTART, UIT_RESTART_MIN_S) == UIT_END_RESTART);
    CHECK(uit_game_end(UIT_EXIT_RESTART, 0.5) == UIT_END_RESTART_SKIPPED);   // no restart loop
    CHECK(uit_game_end(1, 60) == UIT_END_ERROR);
    CHECK(uit_game_end(86, 60) == UIT_END_ERROR);                    // the memory watchdog stays an error
    CHECK(uit_game_end((int)0xC0000005u, 60) == UIT_END_ERROR);      // a crash
    CHECK(uit_game_end(-1, 60) == UIT_END_ERROR);
    CHECK(UIT_EXIT_RESTART != 0 && UIT_EXIT_RESTART != 86 && UIT_EXIT_RESTART < 256);   // fits a POSIX exit status too
    if (fails) { printf("%d of %d checks failed\n", fails, checks); return 1; }
    printf("ok: %d checks\n", checks);
    return 0;
}
