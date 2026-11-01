// [UIT 0272] Ultra Instinct Tears launcher: one copy per folder. A second UltraInstinctTearsLauncher.exe started from the
// same folder brings the running one forward (restored if minimized, also while the game runs) and exits. Copies in other
// folders are separate installs and run side by side. No thread: the running copy checks a named event once per loop.
//   Local\UltraInstinctTearsLauncher-<hash>          the mutex the running copy holds (hash: si_dir_hash of its folder)
//   Local\UltraInstinctTearsLauncher-<hash>-raise    the auto-reset event a second copy sets
// About > Install update > Restart launcher: the old copy lets the mutex go before it starts the new one with --relaunch,
// and a --relaunch copy waits for it (up to wait_ms) instead of handing over, in case the old one is still closing.
#pragma once
#include "mini.h"
#include <stdint.h>

namespace ut {

// FNV-1a 32 over the folder as Windows compares it: '/' as '\', ASCII lower case, no trailing slashes
uint32_t si_dir_hash(const mini::string &dir);
// "Local\UltraInstinctTearsLauncher-1a2b3c4d" (+ suffix, e.g. "-raise")
mini::string si_name(const mini::string &dir, const char *suffix);

enum SiResult { SI_FIRST = 0, SI_HANDED_OVER = 1, SI_ALONE = 2 };
// SI_FIRST: this copy holds the mutex (go on); SI_HANDED_OVER: another copy runs and was asked to come forward (exit now);
// SI_ALONE: no single-instance check here (not Windows, or the mutex could not be made): go on.
// relaunch: wait up to wait_ms for the old copy to let go instead of handing over.
SiResult si_claim(const mini::string &dir, bool relaunch, unsigned wait_ms);
// The running copy: has a second copy asked it to come forward since the last call? (WaitForSingleObject(event, 0))
bool si_raise_requested();
// Lets the mutex go (before Restart launcher starts the new copy); si_claim(dir, false, 0) takes it again if that failed.
void si_release();

}  // namespace ut
