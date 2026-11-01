// [UIT 0347] Ultra Instinct Tears launcher: a zip of the save folder before each Play (TOTK_SAVE_BACKUPS).
#pragma once
#include "mini.h"

namespace ut {

struct SaveBackup {
    enum Result { Off, Made, Unchanged, NoSaves, Skipped } result = Off;
    mini::string file;     // Made: the new zip
    mini::string why;      // Skipped: what went wrong (for the status line and the launcher log)
    int removed = 0;       // older backups deleted to keep the newest `keep`
};

// Zips every file under `saves` (but not its temp, cache and romfix-cache folders, which the game rebuilds) into
// dest/saves-YYYYMMDD-HHMMSS.zip, then deletes the oldest saves-*.zip in dest beyond the newest `keep`. Nothing under
// `saves` is ever written or deleted. When the files (names, sizes, change times) are the same as in the newest backup,
// no new one is made, so starting the game again and again does not push the older backups out. Gives up (and makes
// no backup) after `budget_s` seconds or past 2 GB, so Play is never held up for long. keep <= 0: off.
SaveBackup save_backup(const mini::string &saves, const mini::string &dest, int keep, double budget_s);

}  // namespace ut
