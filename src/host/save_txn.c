/* [UIT 0080] Save writes for nn::fs on the save mount (src/host/fs.c).
 *
 * Before: nn::fs::Commit ran fsync on every open file handle (RomFS-free, but read handles too) on the game's thread,
 * and files the game had already closed were never flushed at all; a crash while the game was writing a save left a
 * half-written file in place of the old one.
 *
 * TOTK_SAVE_WRITES:
 *   "-"    (default) exactly as before.
 *   "bg"   every save file or folder the game changes is remembered; Commit hands that list to a background thread,
 *          which flushes each of them (and only them) to disk; the game goes on at once.
 *   "safe" "bg", and before the first change of a file in a transaction (the time between two Commits) its old
 *          contents are copied to a journal (<profile>/save-journal/<n>/, written and flushed before the change is
 *          made). Commit marks the journal committed; when the flush is done the journal is deleted. At the next start,
 *          a journal that is not marked committed (the game or the PC stopped while the game was writing, before it
 *          committed) is rolled back: the old files come back, as on the Switch, whose save data only changes at
 *          Commit. Nothing is thrown away: the files rolled back are moved to <profile>/save-rolled-back/<time>/.
 *          A journal marked committed is kept as written (the files were complete in the PC's cache when the game
 *          committed). The one case not covered: the PC losing power in the few milliseconds between Commit and the
 *          end of the flush (as before this change, where it was not covered at all).
 */
#include "save_txn.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "../runtime/platform.h"   /* (Windows: fsync = _commit) */

#ifndef O_BINARY
#define O_BINARY 0
#endif
#define mkdir1(p) mkdir(p, 0700)   /* (Windows: platform.h's two-argument mkdir) */

uint64_t runtime_now_ns(void);

typedef struct { char **v; int n, cap; } List;
static void list_add(List *l, const char *s) {
    for (int i = 0; i < l->n; i++) if (!strcmp(l->v[i], s)) return;
    if (l->n == l->cap) { l->cap = l->cap ? l->cap * 2 : 16; l->v = realloc(l->v, (size_t)l->cap * sizeof *l->v); }
    l->v[l->n++] = strdup(s);
}
static int list_has(const List *l, const char *s) { for (int i = 0; i < l->n; i++) if (!strcmp(l->v[i], s)) return 1; return 0; }
static void list_free(List *l) { for (int i = 0; i < l->n; i++) free(l->v[i]); free(l->v); *l = (List){0}; }

static int mode;                      /* 0 as before, 1 bg, 2 safe */
static char root[1024], jroot[1100], rbroot[1100];
static size_t rootn;
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv = PTHREAD_COND_INITIALIZER;
static List changed;                  /* this transaction: host paths changed (bg, safe) */
static List journaled;                /* this transaction: relative paths already in the journal (safe) */
static unsigned gen = 1;              /* this transaction's journal folder number */
static int jcount, jopen;             /* entries in it; folder made */
static List queue_paths; static unsigned queue_gen; static int queued; /* the commit waiting for the flusher */
static _Atomic int flushing;
static int thread_started;
static uint64_t stat_commits, stat_files, stat_ns, stat_jbytes;
static _Atomic int sj_hold;   /* (tools/tests/test_save_txn.c: holds the flush back) */

int uit_save_mode(void) { return mode; }
int uit_save_busy(void) { return atomic_load(&flushing); }

static int sj_is_dir(const char *p) { struct stat st; return !stat(p, &st) && S_ISDIR(st.st_mode); }
static int exists(const char *p) { struct stat st; return !stat(p, &st); }
static void make_dirs(const char *path) {   /* the folders above path */
    char t[1100];
    snprintf(t, sizeof t, "%s", path);
    for (char *s = t + 1; *s; s++)
        if (*s == '/' || *s == '\\') { char c = *s; *s = 0; if (!sj_is_dir(t)) mkdir1(t); *s = c; }
}
static int sync_path(const char *p) {   /* flush one file to disk (FlushFileBuffers needs a handle with write access) */
    int fd = open(p, O_RDWR | O_BINARY);
    if (fd < 0) return errno == ENOENT ? 0 : -1;   /* deleted again since: nothing to flush */
    int r = fsync(fd);
    close(fd);
    return r;
}
static int copy_file(const char *from, const char *to, uint64_t *bytes) {
    int a = open(from, O_RDONLY | O_BINARY);
    if (a < 0) return -1;
    make_dirs(to);
    int b = open(to, O_WRONLY | O_CREAT | O_TRUNC | O_BINARY, 0600);
    if (b < 0) { close(a); return -1; }
    static _Thread_local char buf[1 << 20];
    ssize_t n;
    int bad = 0;
    while ((n = read(a, buf, sizeof buf)) > 0) {
        for (ssize_t d = 0; d < n;) { ssize_t w = write(b, buf + d, (size_t)(n - d)); if (w <= 0) { bad = 1; break; } d += w; }
        if (bad) break;
        if (bytes) *bytes += (uint64_t)n;
    }
    if (n < 0) bad = 1;
    if (!bad && fsync(b)) bad = 1;
    close(a);
    close(b);
    return bad ? -1 : 0;
}
static int sj_remove_tree(const char *path) {
    if (!sj_is_dir(path)) return unlink(path);
    DIR *d = opendir(path);
    if (!d) return -1;
    struct dirent *e;
    char child[2048];
    int fail = 0;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        snprintf(child, sizeof child, "%s/%s", path, e->d_name);
        fail |= sj_remove_tree(child);
    }
    closedir(d);
    return fail | rmdir(path);
}
static int write_line(const char *file, const char *line) {   /* append + flush: the journal's write-ahead step */
    int fd = open(file, O_WRONLY | O_CREAT | O_APPEND | O_BINARY, 0600);
    if (fd < 0) return -1;
    size_t n = strlen(line);
    int bad = write(fd, line, n) != (ssize_t)n || fsync(fd);
    close(fd);
    return bad ? -1 : 0;
}

/* ---------------------------------------------------------------- journal (safe) */
static void jdir(unsigned g, char *out, size_t cap) { snprintf(out, cap, "%s/%u", jroot, g); }
/* under mu. Record rel's state before its first change in this transaction: "F <n> <rel>" with the old contents in
 * <n>.bak, or "A <rel>" (it did not exist). A folder that exists is recorded file by file (it may be deleted). */
static void journal(const char *host) {
    if (strncmp(host, root, rootn) || (host[rootn] && host[rootn] != '/')) return;   /* not save data */
    const char *rel = host[rootn] ? host + rootn + 1 : "";
    if (!*rel || list_has(&journaled, rel)) return;
    char d[1200], m[1300], line[1400];
    jdir(gen, d, sizeof d);
    if (!jopen) { mkdir1(jroot); mkdir1(d); jopen = 1; }
    snprintf(m, sizeof m, "%s/manifest.txt", d);
    list_add(&journaled, rel);
    if (sj_is_dir(host)) {
        DIR *dd = opendir(host);
        struct dirent *e;
        char child[2048];
        snprintf(line, sizeof line, "D %s\n", rel);   /* (folders come back with the files in them) */
        if (write_line(m, line)) fprintf(stderr, "[totk] save journal: cannot write %s (errno %d)\n", m, errno);
        while (dd && (e = readdir(dd))) {
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
            snprintf(child, sizeof child, "%s/%s", host, e->d_name);
            journal(child);
        }
        if (dd) closedir(dd);
        return;
    }
    if (exists(host)) {
        char bak[1300];
        int n = jcount++;
        snprintf(bak, sizeof bak, "%s/%d.bak", d, n);
        if (copy_file(host, bak, &stat_jbytes)) { fprintf(stderr, "[totk] save journal: cannot copy %s (errno %d)\n", host, errno); return; }
        snprintf(line, sizeof line, "F %d %s\n", n, rel);
    } else snprintf(line, sizeof line, "A %s\n", rel);
    if (write_line(m, line)) fprintf(stderr, "[totk] save journal: cannot write %s (errno %d)\n", m, errno);
}
/* Undo one uncommitted journal folder: the files the game was writing go to save-rolled-back, the old ones come back. */
static void rollback(const char *d) {
    char m[1300], stamp[64], rb[1300];
    snprintf(m, sizeof m, "%s/manifest.txt", d);
    FILE *f = fopen(m, "rb");
    if (!f) { sj_remove_tree(d); return; }
    time_t now = time(NULL);
    strftime(stamp, sizeof stamp, "%Y%m%d-%H%M%S", localtime(&now));
    snprintf(rb, sizeof rb, "%s/%s", rbroot, stamp);
    for (int k = 2; exists(rb); k++) snprintf(rb, sizeof rb, "%s/%s-%d", rbroot, stamp, k);   /* a folder of its own */
    char line[1400];
    List lines = {0};
    while (fgets(line, sizeof line, f)) { line[strcspn(line, "\r\n")] = 0; if (line[0] && line[1] == ' ') list_add(&lines, line); }
    fclose(f);
    int restored = 0, moved = 0;
    for (int i = 0; i < lines.n; i++) {   /* what the game wrote: moved aside (kept), first */
        const char *rel = lines.v[i] + 2;
        if (lines.v[i][0] == 'F') rel = strchr(rel, ' ') ? strchr(rel, ' ') + 1 : rel;
        if (lines.v[i][0] == 'D') continue;
        char host[1300], keep[2700];
        snprintf(host, sizeof host, "%s/%s", root, rel);
        if (!exists(host) || sj_is_dir(host)) continue;
        snprintf(keep, sizeof keep, "%s/%s", rb, rel);
        make_dirs(keep);
        if (!rename(host, keep) || (!copy_file(host, keep, NULL) && !unlink(host))) moved++;
    }
    for (int i = lines.n - 1; i >= 0; i--) {   /* then the old files back */
        const char *l = lines.v[i];
        char host[1300], bak[1400];
        if (l[0] == 'F') {
            int n = atoi(l + 2);
            const char *rel = strchr(l + 2, ' ') + 1;
            snprintf(host, sizeof host, "%s/%s", root, rel);
            snprintf(bak, sizeof bak, "%s/%d.bak", d, n);
            if (!copy_file(bak, host, NULL)) restored++;
            else fprintf(stderr, "[totk] save journal: could not restore %s (errno %d)\n", host, errno);
        } else if (l[0] == 'D') {
            snprintf(host, sizeof host, "%s/%s/x", root, l + 2);
            make_dirs(host);
        } else if (l[0] == 'A') {   /* new folders the game made: removed when empty (their files were moved aside) */
            snprintf(host, sizeof host, "%s/%s", root, l + 2);
            if (sj_is_dir(host)) rmdir(host);
        }
    }
    fprintf(stderr, "[totk] save: the game stopped while writing its save before committing it; the save from before "
            "is back (%d file(s)), the unfinished files are in %s (%d)\n", restored, rb, moved);
    list_free(&lines);
    sj_remove_tree(d);
}
static void recover(void) {
    DIR *d = opendir(jroot);
    if (!d) return;
    unsigned gens[256];
    int n = 0;
    struct dirent *e;
    while ((e = readdir(d)) && n < 256) {
        char *end;
        unsigned long v = strtoul(e->d_name, &end, 10);
        if (*e->d_name && !*end) gens[n++] = (unsigned)v;
    }
    closedir(d);
    for (int i = 0; i < n; i++) for (int j = i + 1; j < n; j++) if (gens[j] > gens[i]) { unsigned t = gens[i]; gens[i] = gens[j]; gens[j] = t; }
    for (int i = 0; i < n; i++) {   /* newest first: each rolls back to the state before it */
        char p[1200], c[1300];
        jdir(gens[i], p, sizeof p);
        snprintf(c, sizeof c, "%s/committed", p);
        if (exists(c)) { sj_remove_tree(p); continue; }   /* committed: the files stay as the game wrote them */
        rollback(p);
    }
}

/* ---------------------------------------------------------------- flusher */
static void *flusher(void *arg) {
    (void)arg;
    { int plat_thread_ecores(void); plat_thread_ecores(); }   // [UIT 0089] TOTK_HYBRID_PLACEMENT=1: efficiency cores (hybrid CPUs)
    pthread_mutex_lock(&mu);
    for (;;) {
        while (!queued) pthread_cond_wait(&cv, &mu);
        List l = queue_paths; unsigned g = queue_gen;
        queue_paths = (List){0}; queued = 0; queue_gen = 0;
        pthread_mutex_unlock(&mu);
        while (atomic_load(&sj_hold)) { struct timespec ts = {0, 1000000}; nanosleep(&ts, NULL); }
        uint64_t t0 = runtime_now_ns();
        int bad = 0;
        for (int i = 0; i < l.n; i++) if (!sj_is_dir(l.v[i]) && sync_path(l.v[i])) { bad = 1; fprintf(stderr, "[totk] save: flush of %s failed (errno %d)\n", l.v[i], errno); }
        if (g) { char p[1200]; jdir(g, p, sizeof p); sj_remove_tree(p); }   /* durable now: the journal goes */
        uint64_t dt = runtime_now_ns() - t0;
        pthread_mutex_lock(&mu);
        stat_commits++, stat_files += (uint64_t)l.n, stat_ns += dt;
        fprintf(stderr, "[totk] save: commit %llu flushed %d changed file(s) in %.1f ms in the background%s (%llu KiB journaled so far)\n",
                (unsigned long long)stat_commits, l.n, dt / 1e6, bad ? ", with errors" : "", (unsigned long long)(stat_jbytes >> 10));
        list_free(&l);
        if (!queued) atomic_store(&flushing, 0);
        pthread_cond_broadcast(&cv);
    }
    return NULL;
}

void uit_save_init(const char *save_root, const char *profile) {
    const char *v = getenv("TOTK_SAVE_WRITES");
    mode = !v || !*v || !strcmp(v, "-") || !strcmp(v, "0") ? 0 : !strcmp(v, "safe") ? 2 : 1;
    snprintf(root, sizeof root, "%s", save_root);
    rootn = strlen(root);
    snprintf(jroot, sizeof jroot, "%s/save-journal", profile);
    snprintf(rbroot, sizeof rbroot, "%s/save-rolled-back", profile);
    recover();   /* also with the setting off: a journal left from a run with it on still comes back */
    if (!mode) return;
    if (mode == 2) mkdir1(jroot);
    pthread_t t;
    thread_started = !pthread_create(&t, NULL, flusher, NULL);
    if (thread_started) pthread_detach(t); else mode = 0;
    fprintf(stderr, "[totk] save writes: %s\n", !mode ? "as before (no flush thread)" : mode == 2 ? "background flush, crash-safe journal" : "background flush of changed files");
}
void uit_save_touch(const char *host_path) {
    if (!mode) return;
    pthread_mutex_lock(&mu);
    if (mode == 2) journal(host_path);
    list_add(&changed, host_path);
    pthread_mutex_unlock(&mu);
}
void uit_save_commit(void) {
    if (!mode) return;
    pthread_mutex_lock(&mu);
    if (mode == 2 && jopen) {   /* committed: from here a restart keeps the new files */
        char p[1200], c[1300];
        jdir(gen, p, sizeof p);
        snprintf(c, sizeof c, "%s/committed", p);
        FILE *f = fopen(c, "wb"); if (f) fclose(f);
    }
    for (int i = 0; i < changed.n; i++) list_add(&queue_paths, changed.v[i]);
    if (mode == 2 && jopen) {
        if (queue_gen) { char p[1200]; jdir(queue_gen, p, sizeof p); sj_remove_tree(p); }   /* (an older one not flushed yet: this flush covers it) */
        queue_gen = gen;
    }
    list_free(&changed);
    list_free(&journaled);
    if (jopen) gen++, jcount = 0, jopen = 0;
    queued = 1;
    atomic_store(&flushing, 1);
    pthread_cond_broadcast(&cv);
    pthread_mutex_unlock(&mu);
}
void uit_save_drain(void) {
    if (!mode || !thread_started) return;
    uint64_t end = runtime_now_ns() + 10000000000ull;
    pthread_mutex_lock(&mu);
    while ((queued || atomic_load(&flushing)) && runtime_now_ns() < end) {
        pthread_mutex_unlock(&mu);
        struct timespec ts = {0, 2000000};
        nanosleep(&ts, NULL);
        pthread_mutex_lock(&mu);
    }
    pthread_mutex_unlock(&mu);
}
