/* [UIT 0080] Save writes: changed files only, flushed to disk on a background thread, and (optionally) crash-safe.
 * See save_txn.c. TOTK_SAVE_WRITES: "-" (default) as before, "bg" background flush, "safe" background + journal. */
#pragma once

void uit_save_init(const char *save_root, const char *profile);   /* fs.c init(): reads the setting, recovers */
int uit_save_mode(void);                                           /* 0 as before, 1 background, 2 crash-safe */
void uit_save_touch(const char *host_path);   /* before a save file or folder changes (open for writing, create, ...) */
void uit_save_commit(void);                   /* nn::fs::Commit on the save mount: returns at once */
int uit_save_busy(void);                      /* a commit's flush still running */
void uit_save_drain(void);                    /* wait (up to 10 s) for the flush: before the process ends */
