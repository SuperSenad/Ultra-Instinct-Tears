/* [UIT 0080] Box test of the save writes (src/host/save_txn.c) through the game's own nn::fs entry points in
 * src/host/fs.c, on made-up save files (no game data): each run is a child process; a "crash" is the child ending
 * without Commit (or in the middle of the game's writes), and the next child's start shows what the player gets.
 *   zcc -O1 -Isrc -Isrc/host -I<zstd.h> tools/tests/test_save_txn.c src/host/modfs.c src/host/restbl.c \
 *       src/host/romfix.c src/host/uit_display.c -lzstd -lm -lpthread -o /tmp/t/savetx && /tmp/t/savetx */
#include <fcntl.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#define O_BINARY 0                                   /* Windows-only names fs.c uses */
#define _chsize_s(fd, size) ftruncate(fd, size)
#include "../../src/host/fs.c"
#include "../../src/host/save_txn.c"

_Noreturn void runtime_fail(const char *m) { fprintf(stderr, "runtime_fail: %s\n", m); exit(3); }
uint64_t runtime_now_ns(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec; }
void runtime_account_uid(uint64_t out[2]) { out[0] = 1, out[1] = 2; }
static char prof[256];
const char *runtime_profile_dir(void) { return prof; }
void hle_native_thunk(Cpu *c) { (void)c; }
void plat_commit(uint64_t a, uint64_t s) { (void)a, (void)s; }
int plat_thread_ecores(void) { return 0; }   // [UIT 0089] platform stubs (TOTK_HYBRID_PLACEMENT, Windows only)
int plat_thread_game(void) { return 0; }
const int main_output_res_patch = 0, main_aspect_patch = 0;
const int main_shadow_patch = 0;

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)
static const uint64_t UID[2] = {1, 2};
static char sroot[400];

static const char *host_read(const char *rel) {   /* a save file as it is on disk, "-" when missing */
    static char buf[4096]; char p[700];
    snprintf(p, sizeof p, "%s/%s", sroot, rel);
    FILE *f = fopen(p, "rb");
    if (!f) return "-";
    size_t n = fread(buf, 1, sizeof buf - 1, f); buf[n] = 0; fclose(f);
    return buf;
}
static void host_write(const char *rel, const char *text) {
    char p[700]; snprintf(p, sizeof p, "%s/%s", sroot, rel);
    FILE *f = fopen(p, "wb"); fputs(text, f); fclose(f);
}
static int count_dir(const char *p) {   /* entries in a folder, -1 when missing */
    DIR *d = opendir(p); if (!d) return -1;
    int n = 0; struct dirent *e;
    while ((e = readdir(d))) n += strcmp(e->d_name, ".") && strcmp(e->d_name, "..");
    closedir(d); return n;
}
/* the game's way of saving: open (or create) for writing, set the size, write, close */
static void game_save(const char *path, const char *text) {
    uint64_t h = 0;
    if (open_file(&h, path, OPEN_WRITE)) { CHECK(create_file(path, 0) == 0); CHECK(open_file(&h, path, OPEN_WRITE) == 0); }
    CHECK(set_file_size(h, (int64_t)strlen(text)) == 0);
    CHECK(write_file(h, 0, text, strlen(text), NULL) == 0);
    close_file(h);
}
static int fs_start(void) { ready(); return mount_save("save", UID) == 0; }

typedef void (*Step)(void);
static int run(const char *mode, Step step) {   /* one game run in a child process; its exit code */
    fflush(stdout);
    pid_t p = fork();
    if (!p) {
        if (mode) setenv("TOTK_SAVE_WRITES", mode, 1); else unsetenv("TOTK_SAVE_WRITES");
        fails = 0;
        if (!fs_start()) _exit(9);
        if (step) step();
        fflush(stdout);
        _exit(fails ? 1 : 0);   /* (no atexit: as a crash) */
    }
    int st = 0; waitpid(p, &st, 0);
    return WIFEXITED(st) ? WEXITSTATUS(st) : 99;
}

/* ---- steps */
static void s_commit_two(void) {
    game_save("save:/slot_00/progress.sav", "NEW-progress");
    game_save("save:/slot_00/caption.sav", "NEW-caption");
    CHECK(commit("save") == 0);
    uit_save_drain();
    CHECK(!uit_save_busy());
    CHECK(stat_commits == 1 && stat_files == 2);   /* just the two changed files */
}
static void s_crash_mid_save(void) {   /* the game is writing when the PC stops: no Commit */
    game_save("save:/slot_00/progress.sav", "HALF-WRITTEN");
    game_save("save:/slot_00/new.sav", "a new file");
    CHECK(delete_file("save:/slot_00/caption.sav") == 0);
    CHECK(rename_file("save:/option.sav", "save:/option2.sav") == 0);
}
static void s_crash_after_commit(void) {   /* committed, then the game crashes before the flush is done (drain skipped) */
    game_save("save:/slot_00/progress.sav", "COMMITTED");
    atomic_store(&sj_hold, 1);   /* hold the flusher back: the journal is still there when the run ends */
    CHECK(commit("save") == 0);
    _exit(fails ? 1 : 0);
}
static void s_crash_delete_tree(void) {
    CHECK(delete_recursive("save:/slot_00") == 0);
    CHECK(host_read("slot_00/progress.sav")[0] == '-');
}
static void s_check_none(void) {}
static void s_old_mode(void) {
    game_save("save:/slot_00/progress.sav", "OLD-MODE");
    CHECK(commit("save") == 0);
    CHECK(count_dir(jroot) == -1);   /* no journal folder at all */
}

int main(void) {
    snprintf(prof, sizeof prof, "/tmp/t/savetx-%d", (int)getpid());
    char cmd[600]; snprintf(cmd, sizeof cmd, "rm -rf '%s' && mkdir -p '%s'", prof, prof); if (system(cmd)) return 2;
    snprintf(sroot, sizeof sroot, "%s/save/%016llx%016llx", prof, 2ull, 1ull);
    char jr[400], rb[400]; snprintf(jr, sizeof jr, "%s/save-journal", prof); snprintf(rb, sizeof rb, "%s/save-rolled-back", prof);
    char mk[900]; snprintf(mk, sizeof mk, "mkdir -p '%s/slot_00'", sroot); if (system(mk)) return 2;
    host_write("slot_00/progress.sav", "OLD-progress"); host_write("slot_00/caption.sav", "OLD-caption"); host_write("option.sav", "OLD-option");

    /* 1: safe: a committed save is on disk, the journal is gone */
    CHECK(run("safe", s_commit_two) == 0);
    CHECK(!strcmp(host_read("slot_00/progress.sav"), "NEW-progress") && !strcmp(host_read("slot_00/caption.sav"), "NEW-caption"));
    CHECK(count_dir(jr) == 0);

    /* 2: safe: a crash while saving: the next start brings the committed save back, keeps the unfinished files aside */
    CHECK(run("safe", s_crash_mid_save) == 0);
    CHECK(!strcmp(host_read("slot_00/progress.sav"), "HALF-WRITTEN") && count_dir(jr) == 1);   /* (before the restart) */
    CHECK(run("safe", s_check_none) == 0);
    CHECK(!strcmp(host_read("slot_00/progress.sav"), "NEW-progress"));
    CHECK(!strcmp(host_read("slot_00/caption.sav"), "NEW-caption"));   /* deleted: back */
    CHECK(!strcmp(host_read("option.sav"), "OLD-option") && host_read("option2.sav")[0] == '-');   /* renamed: back */
    CHECK(host_read("slot_00/new.sav")[0] == '-');   /* new: moved aside */
    CHECK(count_dir(jr) == 0 && count_dir(rb) == 1);
    CHECK(run("safe", s_crash_mid_save) == 0 && run("safe", s_check_none) == 0 && count_dir(rb) == 2);   /* the same second: a folder of its own */
    { char f[700]; snprintf(cmd, sizeof cmd, "grep -rl HALF-WRITTEN '%s' >/dev/null && grep -rl 'a new file' '%s' >/dev/null", rb, rb);
      CHECK(system(cmd) == 0); (void)f; }   /* nothing thrown away */

    /* 3: rolled back also when the setting is off now (a journal left from a run with it on) */
    CHECK(run("safe", s_crash_mid_save) == 0);
    CHECK(run(NULL, s_check_none) == 0);
    CHECK(!strcmp(host_read("slot_00/progress.sav"), "NEW-progress") && count_dir(jr) == 0);

    /* 4: committed but not yet flushed when the game ended: kept as committed */
    CHECK(run("safe", s_crash_after_commit) == 0);
    CHECK(count_dir(jr) == 1);
    CHECK(run("safe", s_check_none) == 0);
    CHECK(!strcmp(host_read("slot_00/progress.sav"), "COMMITTED") && count_dir(jr) == 0);

    /* 5: a whole folder deleted, then a crash: the folder and its files come back */
    CHECK(run("safe", s_crash_delete_tree) == 0);
    CHECK(run("safe", s_check_none) == 0);
    CHECK(!strcmp(host_read("slot_00/progress.sav"), "COMMITTED") && !strcmp(host_read("slot_00/caption.sav"), "NEW-caption"));

    /* 6: bg: changed files only, no journal; "-": as before */
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", jr); if (system(cmd)) return 2;
    host_write("slot_00/caption.sav", "OLD-caption");
    CHECK(run("bg", s_commit_two) == 0);
    CHECK(!strcmp(host_read("slot_00/progress.sav"), "NEW-progress") && count_dir(jr) == -1);
    CHECK(run("-", s_old_mode) == 0);
    CHECK(!strcmp(host_read("slot_00/progress.sav"), "OLD-MODE"));

    snprintf(cmd, sizeof cmd, "rm -rf '%s'", prof); if (system(cmd)) return 2;
    printf(fails ? "FAIL (%d)\n" : "PASS\n", fails);
    return fails != 0;
}
