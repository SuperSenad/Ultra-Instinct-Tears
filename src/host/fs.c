/* nn::fs for the recompiled game: the read-only RomFS and writable save, temporary and cache storage.
 * Ported from MacNativeGames games/totk/runtime/fs_host.c and romfs.c @b75be8c (MIT). Changes:
 *  - guest-register binding through HLE_NATIVE (every nn::fs entry point takes integers and pointers only);
 *  - Windows file calls (mkdir, directory types via stat, rename that refuses to replace, _commit);
 *  - RomFS: the runtime index (tools/make_romfs_index.py, MacNativeGames' TOTKRFS1 layout) and the extracted
 *    romfs.bin, both already verified at extraction, or (the player's dump) a folder of plain files, indexed by
 *    walking it at start (tools/setup.py checks it against the expected version); split images and the startup
 *    re-hash are not ported yet. [UIT 0041] Mods (src/host/modfs.c: <mods>/<Name>/romfs/...) are laid over it: a file
 *    a mod has is read from the mod (the highest one in the launcher's order wins), the rest from the game. RomFS directory listing stays unsupported, as in MacNativeGames.
 * Save, temporary and cache storage are host directories under the profile directory (TOTK_PROFILE_DIR, default
 * userdata); application save data is created on first start, as the system launcher does. */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef _WIN32
#include <windows.h>
#endif

#include "host.h"
#include "modfs.h"
#include "restbl.h"
#include "romfix.h"
#include "save_txn.h"
#include "uit_display.h"
#include "../runtime/hle.h"

#define R_PATH_NOT_FOUND 0x202u
#define R_ALREADY_EXISTS 0x402u
#define R_NOT_EMPTY 0x1002u
#define R_TARGET_NOT_FOUND 0x7d402u
#define R_INVALID_ARGUMENT 0x2ee202u
#define R_INVALID_PATH 0x2ee602u
#define R_OPEN_MODE 0x307402u
#define R_APPEND_REQUIRED 0x2f5e02u
#define R_ROM_UNSUPPORTED 0x6402u
#define R_ROM_INVALID 0xe402u
#define TOTK_APPLICATION_ID 0x0100f2c0115b6000ULL
enum { M_FREE, M_SAVE, M_TEMP, M_CACHE };
enum { OPEN_READ = 1, OPEN_WRITE = 2, OPEN_APPEND = 4 };
#define ROM_BASE 0x100000000ULL
#define NROM 65536
#define FILE_BASE 0x200000000ULL
#define DIR_BASE 0x300000000ULL
#define NFILES 1024
#define NDIRS 256

void runtime_account_uid(uint64_t out[2]);
const char *runtime_profile_dir(void);

static void need(int c, const char *m) { if (!c) runtime_fail(m); }
static int make_dir_p(const char *p) { return mkdir(p, 0700); }

/* ======================================== RomFS ======================================== */
static uint8_t *index_bytes;
static uint64_t file_count, strings_offset;
static int image_fd = -1;
static char mount_name[32];
static uint64_t rom_handles[NROM];   /* file index + 1; 0 is free; ROM_MOD | k: mods.files[k] */
#define ROM_MOD (1ULL << 62)
#define ROM_MEM (1ULL << 61)   /* [UIT 0042] a file served from memory: ROM_MEM | slot (mem_file) */
static pthread_mutex_t rom_lock = PTHREAD_MUTEX_INITIALIZER;
typedef struct { uint64_t offset, size; uint32_t name, len; } Entry;
_Static_assert(sizeof(Entry) == 24, "index entry layout");
static Entry entry(uint64_t i) { Entry e; memcpy(&e, index_bytes + 24 + i * 24, 24); return e; }
static const char *entry_name(Entry e) { return (const char *)index_bytes + strings_offset + e.name; }

/* A RomFS folder (the player's extraction: plain files): the index is built by walking it, in the same layout as the
   runtime index, with each entry's offset unused; a file is opened by its path when the game opens it. */
static char rom_dir[4096];
static int rom_folder;
static int rom_fds[NROM];
typedef struct { char *path; uint64_t size; } Walked;
static Walked *walked;
static size_t nwalked, walked_cap;
static void walk_add(const char *path, uint64_t size) {
    if (nwalked == walked_cap) {
        walked_cap = walked_cap ? walked_cap * 2 : 65536;
        walked = realloc(walked, walked_cap * sizeof *walked);
        need(walked != NULL, "RomFS folder index memory");
    }
    walked[nwalked].path = strdup(path);
    walked[nwalked++].size = size;
}
static void walk(const char *root, const char *rel) {
    char pattern[4096];
    snprintf(pattern, sizeof pattern, "%s/%s%s*", root, rel, *rel ? "/" : "");
#ifdef _WIN32
    WIN32_FIND_DATAA d;
    HANDLE h = FindFirstFileExA(pattern, FindExInfoBasic, &d, FindExSearchNameMatch, NULL, FIND_FIRST_EX_LARGE_FETCH);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (!strcmp(d.cFileName, ".") || !strcmp(d.cFileName, "..")) continue;
        char sub[4096];
        snprintf(sub, sizeof sub, "%s%s%s", rel, *rel ? "/" : "", d.cFileName);
        if (d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) walk(root, sub);
        else walk_add(sub, (uint64_t)d.nFileSizeHigh << 32 | d.nFileSizeLow);
    } while (FindNextFileA(h, &d));
    FindClose(h);
#else
    pattern[strlen(pattern) - 1] = 0;
    DIR *dir = opendir(pattern);
    if (!dir) return;
    for (struct dirent *e; (e = readdir(dir));) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char sub[4096], full[8192];
        snprintf(sub, sizeof sub, "%s%s%s", rel, *rel ? "/" : "", e->d_name);
        snprintf(full, sizeof full, "%s/%s", root, sub);
        struct stat st;
        if (stat(full, &st)) continue;
        if (S_ISDIR(st.st_mode)) walk(root, sub);
        else walk_add(sub, (uint64_t)st.st_size);
    }
    closedir(dir);
#endif
}
static int walked_cmp(const void *a, const void *b) { return strcmp(((const Walked *)a)->path, ((const Walked *)b)->path); }
static void index_folder(const char *dir) {
    uint64_t t0 = runtime_now_ns();
    walk(dir, "");
    need(nwalked > 0, "RomFS folder is empty");
    qsort(walked, nwalked, sizeof *walked, walked_cmp);
    size_t names = 0;
    for (size_t i = 0; i < nwalked; i++) names += strlen(walked[i].path) + 1;
    file_count = nwalked;
    strings_offset = 24 + file_count * 24;
    index_bytes = malloc(strings_offset + names);
    need(index_bytes != NULL, "RomFS folder index memory");
    memcpy(index_bytes, "TOTKRFS1", 8);
    memcpy(index_bytes + 8, &file_count, 8);
    memcpy(index_bytes + 16, &strings_offset, 8);
    uint32_t at = 0;
    for (size_t i = 0; i < nwalked; i++) {
        uint32_t len = (uint32_t)strlen(walked[i].path) + 1;
        Entry e = {0, walked[i].size, at, len};
        memcpy(index_bytes + 24 + i * 24, &e, 24);
        memcpy(index_bytes + strings_offset + at, walked[i].path, len);
        at += len;
        free(walked[i].path);
    }
    free(walked);
    walked = NULL;
    fprintf(stderr, "[totk] RomFS: %llu files in the folder %s (indexed in %.0f ms)\n", (unsigned long long)file_count, dir,
            (runtime_now_ns() - t0) / 1e6);
}

static void mods_init(void);
static void rom_init(void) {
    const char *dir = getenv("TOTK_ROMFS");
    if (!dir) dir = "data/romfs";
    char path[4096];
    snprintf(path, sizeof path, "%s/romfs.bin", dir);
    struct stat st;
    if (stat(path, &st)) {   // no image: a folder of plain files
        need(snprintf(rom_dir, sizeof rom_dir, "%s", dir) < (int)sizeof rom_dir, "RomFS folder path");
        rom_folder = 1;
        index_folder(rom_dir);
        return;
    }
    snprintf(path, sizeof path, "%s/runtime-index.bin", dir);
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "[totk] %s missing: run tools/make_romfs_index.py\n", path); runtime_fail("RomFS index missing"); }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    need(size >= 24 && size < 128L * 1024 * 1024, "RomFS index size");
    index_bytes = malloc((size_t)size);
    need(index_bytes && fread(index_bytes, 1, (size_t)size, f) == (size_t)size, "read RomFS index");
    fclose(f);
    need(!memcmp(index_bytes, "TOTKRFS1", 8), "RomFS index format");
    memcpy(&file_count, index_bytes + 8, 8);
    memcpy(&strings_offset, index_bytes + 16, 8);
    need(file_count < 1000000 && strings_offset == 24 + file_count * 24 && strings_offset < (uint64_t)size, "RomFS index layout");
    snprintf(path, sizeof path, "%s/romfs.bin", dir);
    image_fd = open(path, O_RDONLY | O_BINARY);
    need(image_fd >= 0, "RomFS image missing");
    fprintf(stderr, "[totk] RomFS: %llu files from %s\n", (unsigned long long)file_count, dir);
}
void runtime_fs_init(void) {
    if (image_fd >= 0 || rom_folder) return;
    rom_init();
    mods_init();
}
/* First entry whose path is >= p */
static uint64_t lower(const char *p) {
    uint64_t lo = 0, hi = file_count;
    while (lo < hi) { uint64_t m = lo + (hi - lo) / 2; if (strcmp(entry_name(entry(m)), p) < 0) lo = m + 1; else hi = m; }
    return lo;
}
static uint64_t file_index(const char *p) {
    uint64_t i = lower(p);
    return i < file_count && !strcmp(p, entry_name(entry(i))) ? i : UINT64_MAX;
}
static int folder_exists(const char *folder) {
    size_t n = strlen(folder);
    uint64_t i = lower(folder);
    return i < file_count && !strncmp(entry_name(entry(i)), folder, n);
}

/* ---------------------------------------- [UIT 0041] mods */
static Modfs mods;
static size_t mods_n;                /* override files; 0: every lookup is the game's own, as before */
static uint32_t *lower_order;        /* game file indices sorted by lowercase path, built on the first case miss */
static int mods_case_fixed;
static int restbl_on;                /* TOTK_MODS_RESTBL (on unless =0) */
static int icmp_lower(const char *a, const char *b) {
    for (;; a++, b++) {
        int x = (unsigned char)*a, y = (unsigned char)*b;
        if (x >= 'A' && x <= 'Z') x += 32;
        if (y >= 'A' && y <= 'Z') y += 32;
        if (x != y || !x) return x - y;
    }
}
static int lower_cmp(const void *a, const void *b) {
    return icmp_lower(entry_name(entry(*(const uint32_t *)a)), entry_name(entry(*(const uint32_t *)b)));
}
/* The game's spelling of a path a mod has (any case), NULL when the game has no such file. */
static const char *mods_canon(const char *path, void *ud) {
    (void)ud;
    uint64_t i = file_index(path);
    if (i != UINT64_MAX) return entry_name(entry(i));
    if (!lower_order) {
        lower_order = malloc(sizeof *lower_order * (file_count + 1));
        need(lower_order != NULL, "mods: case index memory");
        for (uint64_t k = 0; k < file_count; k++) lower_order[k] = (uint32_t)k;
        qsort(lower_order, (size_t)file_count, sizeof *lower_order, lower_cmp);
    }
    size_t lo = 0, hi = (size_t)file_count;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        int c = icmp_lower(entry_name(entry(lower_order[mid])), path);
        if (!c) { mods_case_fixed++; return entry_name(entry(lower_order[mid])); }
        if (c < 0) lo = mid + 1;
        else hi = mid;
    }
    return NULL;
}
static int mods_game_dir(const char *name, void *ud) {
    (void)ud;
    char d[300];
    snprintf(d, sizeof d, "%s/", name);
    return modfs_known_game_dir(name) || folder_exists(d);
}
static void mods_init(void) {
    const char *on = getenv("TOTK_MODS"), *dir = getenv("TOTK_MODS_DIR");
    if (!dir || !*dir) dir = "mods";
    /* [UIT 0062] Every way to "no mods" ends the same: mods_n = 0 and restbl_on = 0, so each lookup is the game's own
     * file and the resource size table is the game's own bytes (nothing merged, nothing cached: the merge only ever
     * lives in memory, and only when a mod that is on has files). The log says so in plain words. */
    mods_n = 0, restbl_on = 0;
    if (on && !strcmp(on, "0")) {
        fprintf(stderr, "[totk] mods: none active, playing the unmodified game (Load mods is Off: TOTK_MODS=0)\n");
        return;
    }
    uint64_t t0 = runtime_now_ns();
    int n = modfs_load(&mods, dir, 0, mods_canon, mods_game_dir, NULL);
    free(lower_order), lower_order = NULL;
    if (!n) {   /* no mods folder or an empty one */
        modfs_free(&mods);
        fprintf(stderr, "[totk] mods: none active, playing the unmodified game (no mods installed in %s)\n", dir);
        return;
    }
    int non = 0;
    for (int i = 0; i < n; i++) non += mods.mods[i].enabled;
    if (!non || !mods.nfiles) {   /* all off (Disable Mods), or the ones on have nothing the game reads */
        for (int i = 0; i < n; i++)
            if (mods.mods[i].enabled && mods.mods[i].unsupported)
                fprintf(stderr, "[totk] mods:   %s is on but NOT LOADED: %s\n", mods.mods[i].name, modfs_unsupported_text(mods.mods[i].unsupported));
        fprintf(stderr, "[totk] mods: none active, playing the unmodified game (%d installed, %s)\n", n,
                !non ? "all off" : "the ones on have no game files");
        modfs_free(&mods);
        return;
    }
    {   /* one line that names what is active, highest first */
        char list[1024];
        size_t at = 0;
        list[0] = 0;
        for (int i = 0; i < n && at < sizeof list - 8; i++)
            if (mods.mods[i].enabled) {
                int w = snprintf(list + at, sizeof list - at, "%s%s", at ? ", " : "", mods.mods[i].name);
                if (w < 0 || (size_t)w >= sizeof list - at) { snprintf(list + (sizeof list - 8), 8, "..."); break; }
                at += (size_t)w;
            }
        fprintf(stderr, "[totk] mods: %d active (highest first): %s\n", non, list);
    }
    fprintf(stderr, "[totk] mods: %d of %d on, %llu files from %s (indexed in %.0f ms; the highest wins a shared file)\n", non, n,
            (unsigned long long)mods.nfiles, dir, (runtime_now_ns() - t0) / 1e6);
    for (int i = 0; i < n; i++) {
        const ModfsMod *m = &mods.mods[i];
        if (!m->enabled) { fprintf(stderr, "[totk] mods:   -  %s (off)\n", m->name); continue; }
        int won = 0;
        for (size_t k = 0; k < mods.nfiles; k++) won += mods.files[k].mod == i;
        fprintf(stderr, "[totk] mods: %2d. %s%s%s: %d files (%.1f MB), %d used, %d new to the game, %d taken by a higher mod\n", i + 1, m->name,
                m->version[0] ? " " : "", m->version, m->nfiles, m->bytes / 1048576.0, won, m->nnew, m->nlost);
        if (m->unsupported) fprintf(stderr, "[totk] mods:     NOT LOADED from it: %s\n", modfs_unsupported_text(m->unsupported));
        if (m->nnew > 0) {
            int shown = 0;
            for (size_t k = 0; k < mods.nfiles && shown < 5; k++)
                if (mods.files[k].mod == i && !mods.files[k].vanilla)
                    fprintf(stderr, "[totk] mods:     new: %s%s\n", mods.files[k].path, ++shown == 5 && m->nnew > 5 ? " (and more)" : "");
            if (m->nnew == m->nfiles)
                fprintf(stderr, "[totk] mods:     none of its files are in this game: made for another game version, or a wrong folder layout?\n");
        }
        for (int l = 0; l < n; l++)
            if (mods.pairs[i * n + l]) fprintf(stderr, "[totk] mods:     wins %d file%s over %s\n", mods.pairs[i * n + l], mods.pairs[i * n + l] == 1 ? "" : "s", mods.mods[l].name);
    }
    if (mods_case_fixed) fprintf(stderr, "[totk] mods: %d paths matched the game's file names in another letter case\n", mods_case_fixed);
    mods_n = mods.nfiles;
    const char *rt = getenv("TOTK_MODS_RESTBL");
    restbl_on = !(rt && !strcmp(rt, "0"));
    if (!restbl_on) fprintf(stderr, "[totk] mods: resource size table merge off (TOTK_MODS_RESTBL=0)\n");
}

/* ---------------------------------------- [UIT 0042] the resource size table with mods (restbl.h) */
static uint8_t *restbl_buf;          /* the merged table served for restbl_index, NULL: none (the usual file) */
static size_t restbl_len;
static uint64_t restbl_index = UINT64_MAX;
static pthread_mutex_t restbl_lock = PTHREAD_MUTEX_INITIALIZER;
/* n bytes at off of the game's file i (folder or image); bytes read, <0 on error */
static int64_t game_pread(uint64_t i, uint64_t off, void *buf, uint64_t n) {
    Entry e = entry(i);
    if (off >= e.size) return 0;
    if (n > e.size - off) n = e.size - off;
    int fd = image_fd, own = 0;
    uint64_t base = e.offset;
    if (rom_folder) {
        char full[8192];
        snprintf(full, sizeof full, "%s/%s", rom_dir, entry_name(e));
        if ((fd = open(full, O_RDONLY | O_BINARY)) < 0) return -1;
        own = 1, base = 0;
    }
    uint64_t done = 0;
    while (done < n) {
        ssize_t r = pread(fd, (uint8_t *)buf + done, (size_t)(n - done), (off_t)(base + off + done));
        if (r <= 0) break;
        done += (uint64_t)r;
    }
    if (own) close(fd);
    return done == n ? (int64_t)n : -1;
}
static uint8_t *host_slurp(const char *path, uint64_t max, size_t *n) {
    int fd = open(path, O_RDONLY | O_BINARY);
    if (fd < 0) return NULL;
    struct stat st;
    uint8_t *d = NULL;
    if (!fstat(fd, &st) && st.st_size > 0 && (uint64_t)st.st_size <= max && (d = malloc((size_t)st.st_size))) {
        size_t done = 0;
        while (done < (size_t)st.st_size) {
            ssize_t r = pread(fd, d + done, (size_t)st.st_size - done, (off_t)done);
            if (r <= 0) break;
            done += (size_t)r;
        }
        if (done != (size_t)st.st_size) free(d), d = NULL;
        else *n = done;
    }
    close(fd);
    return d;
}
static int head_needed(const char *path) {
    size_t n = strlen(path);
    return (n > 3 && !strcmp(path + n - 3, ".zs")) || (n > 5 && !strcmp(path + n - 5, ".zstd")) || (n > 3 && !strcmp(path + n - 3, ".mc"));
}
/* Once, the first time the game opens its table i: the merge (or nothing: the usual file). */
static void restbl_build(uint64_t i) {
    uint64_t t0 = runtime_now_ns();
    Entry ge = entry(i);
    uint8_t *game = ge.size && ge.size < (64u << 20) ? malloc((size_t)ge.size) : NULL;
    if (!game || game_pread(i, 0, game, ge.size) != (int64_t)ge.size) {
        fprintf(stderr, "[totk] mods: resource size table %s unreadable: the game's own is used\n", entry_name(ge));
        free(game);
        return;
    }
    int nt = 0;
    uint8_t **tabs = calloc((size_t)mods.nrestbls + 1, sizeof *tabs);
    size_t *tabn = calloc((size_t)mods.nrestbls + 1, sizeof *tabn);
    for (int k = 0; tabs && tabn && k < mods.nrestbls; k++)
        if ((tabs[nt] = host_slurp(mods.restbls[k], 64u << 20, &tabn[nt]))) nt++;
        else fprintf(stderr, "[totk] mods: cannot read %s\n", mods.restbls[k]);
    RestblFile *rf = calloc(mods.nfiles + 1, sizeof *rf);
    char (*names)[512] = malloc(512 * (mods.nfiles + 1));
    size_t nf = 0;
    for (size_t k = 0; rf && names && k < mods.nfiles; k++) {
        const ModfsFile *f = &mods.files[k];
        if (modfs_is_restbl(f->path)) continue;
        uint8_t head[32];
        size_t hn = 0;
        if (head_needed(f->path)) {
            int fd = open(f->host, O_RDONLY | O_BINARY);
            ssize_t r = fd >= 0 ? pread(fd, head, sizeof head, 0) : -1;
            if (fd >= 0) close(fd);
            hn = r > 0 ? (size_t)r : 0;
        }
        uint64_t md = restbl_decoded_size(f->path, head, hn, f->size), gd = 0;
        if (!md) md = f->size * 8;   /* a zstd frame without its size: a generous guess */
        uint64_t gi = f->vanilla ? file_index(f->path) : UINT64_MAX;
        if (gi != UINT64_MAX) {
            Entry e = entry(gi);
            int64_t gr = head_needed(f->path) ? game_pread(gi, 0, head, sizeof head) : 0;
            gd = restbl_decoded_size(f->path, head, gr > 0 ? (size_t)gr : 0, e.size);
            if (!gd) gd = e.size * 8;
        }
        modfs_restbl_name(f->path, names[nf], sizeof names[nf]);
        rf[nf].name = names[nf], rf[nf].mod_decoded = md, rf[nf].game_decoded = gd;
        nf++;
    }
    RestblStats st;
    uint8_t *out = NULL;
    size_t on = 0;
    int r = restbl_merge(game, (size_t)ge.size, (const uint8_t *const *)tabs, tabn, nt, rf, nf, &out, &on, &st);
    for (int k = 0; k < nt; k++) free(tabs[k]);
    free(tabs), free(tabn), free(rf), free(names), free(game);
    double ms = (runtime_now_ns() - t0) / 1e6;
    if (r < 0) {
        fprintf(stderr, "[totk] mods: resource size table %s: not a table this build reads (%d): the usual file is used\n", entry_name(ge), r);
        return;
    }
    if (r == 1) {
        fprintf(stderr, "[totk] mods: resource size table: no entry needed a change (%d mod tables, %zu files checked, %.0f ms)\n", st.tables, nf, ms);
        return;
    }
    restbl_buf = out, restbl_len = on;
    fprintf(stderr, "[totk] mods: resource size table merged: %d mod table%s (%d entries from them%s), %d raised and %d added for mod files, "
            "%u entries (%.0f ms)\n", st.tables, st.tables == 1 ? "" : "s", st.from_tables,
            st.tables_bad ? ", some tables unreadable" : "", st.raised, st.added, st.entries, ms);
}

/* [UIT 0065] Files served from memory, by slot: 0 the merged size table, 1.. the romfix edits (romfix.h: the quick-menu
   fix, the UI layouts, the texture heaps), each made the first time the game opens the file. */
enum { MEM_RESTBL, MEM_FIX0 };
static const char *const FIX_PATHS[] = {ROMFIX_COMMON, ROMFIX_HEAPDEF};
#define NFIX (sizeof FIX_PATHS / sizeof *FIX_PATHS)
static struct { int built; uint8_t *buf; size_t len; } fixes[NFIX];
static pthread_mutex_t fix_lock = PTHREAD_MUTEX_INITIALIZER;
static const uint8_t *mem_file(uint64_t i, size_t *len) {
    uint64_t slot = i & 0xffff;
    if (slot == MEM_RESTBL) { *len = restbl_len; return restbl_buf; }
    *len = fixes[slot - MEM_FIX0].len;
    return fixes[slot - MEM_FIX0].buf;
}
/* a whole RomFS file, the mod's when one replaces it (romfix's dictionaries) */
static uint8_t *rom_slurp(const char *p, size_t *n) {
    const ModfsFile *mf = mods_n ? modfs_find(&mods, p) : NULL;
    uint8_t *d = mf ? host_slurp(mf->host, 512u << 20, n) : NULL;
    if (d) return d;
    uint64_t gi = file_index(p);
    if (gi == UINT64_MAX) return NULL;
    Entry e = entry(gi);
    if (!e.size || e.size > (512u << 20) || !(d = malloc((size_t)e.size))) return NULL;
    if (game_pread(gi, 0, d, e.size) != (int64_t)e.size) { free(d); return NULL; }
    *n = (size_t)e.size;
    return d;
}
/* the slot of path's edited file (made the first time), -1: the usual file */
static int fix_slot(const char *p, const ModfsFile *mf) {
    unsigned k = 0;
    while (k < NFIX && strcmp(p, FIX_PATHS[k])) k++;
    if (k == NFIX) return -1;
    pthread_mutex_lock(&fix_lock);
    if (!fixes[k].built) {
        fixes[k].built = 1;
        if (romfix_wants(p)) {
            uint64_t t0 = runtime_now_ns();
            size_t n = 0;
            uint8_t *in = mf ? host_slurp(mf->host, 512u << 20, &n) : NULL;
            int from_mod = in != NULL;
            if (!in) {
                uint64_t gi = file_index(p);
                Entry e = gi != UINT64_MAX ? entry(gi) : (Entry){0};
                if (e.size && e.size <= (512u << 20) && (in = malloc((size_t)e.size)) && game_pread(gi, 0, in, e.size) != (int64_t)e.size)
                    free(in), in = NULL;
                n = (size_t)e.size;
            }
            char cache[4096] = "";
            const char *prof = runtime_profile_dir();
            if (prof && *prof && snprintf(cache, sizeof cache, "%s/romfix-cache", prof) < (int)sizeof cache) make_dir_p(cache);
            if (in && romfix_apply(p, in, n, from_mod, rom_slurp, *cache ? cache : NULL, &fixes[k].buf, &fixes[k].len) > 0)
                fprintf(stderr, "[totk] romfix: %s served edited%s (%.0f ms)\n", p, from_mod ? " (on top of the mod's file)" : "",
                        (runtime_now_ns() - t0) / 1e6);
            else if (!in) fprintf(stderr, "[totk] romfix: %s unreadable: the usual file is used\n", p);
            if (!fixes[k].buf && k == 1) uit_heap_refused();   /* the canvas must not outgrow the heaps it got */
        }
    }
    pthread_mutex_unlock(&fix_lock);
    return fixes[k].buf ? (int)(MEM_FIX0 + k) : -1;
}

/* "mount:/a/b" (or "mount://a" from audio stream paths) -> "a/b"; NULL on traversal or a foreign mount */
static const char *rom_path(const char *path) {
    size_t n = strlen(mount_name);
    if (!n || !path || strncmp(path, mount_name, n) || path[n] != ':' || path[n + 1] != '/') return NULL;
    path += n + 2;
    while (*path == '/') ++path;
    if (!*path || strchr(path, '\\')) return NULL;
    for (const char *part = path; *part;) {
        const char *end = strchr(part, '/');
        size_t size = end ? (size_t)(end - part) : strlen(part);
        if (!size || (size == 1 && part[0] == '.') || (size == 2 && part[0] == '.' && part[1] == '.')) return NULL;
        if (!end) break;
        part = end + 1;
    }
    return path;
}
static int fs_log_on = -1;
static uint64_t fs_log_t0;
void runtime_fs_log(const char *what, const char *path) {
    if (fs_log_on < 0) { fs_log_on = getenv("TOTK_FS_LOG") != NULL; fs_log_t0 = runtime_now_ns(); }
    if (fs_log_on) fprintf(stderr, "[totk-fs] %8.3f %s %s\n", (runtime_now_ns() - fs_log_t0) / 1e9, what, path);
}
static int romfs_owns(const char *path) {
    size_t n = strlen(mount_name);
    return n && path && !strncmp(path, mount_name, n) && path[n] == ':';
}
static int romfs_handle(uint64_t h) { return h > ROM_BASE && h < ROM_BASE + NROM; }
static unsigned romfs_open(uint64_t *out, const char *path, int mode) {
    runtime_fs_log("rom", path ? path : "(null)");
    if (!out || mode != OPEN_READ) return R_ROM_UNSUPPORTED;
    const char *p = rom_path(path);
    if (!p) return R_PATH_NOT_FOUND;
    const ModfsFile *mf = mods_n ? modfs_find(&mods, p) : NULL;
    uint64_t i = mf ? ROM_MOD | (uint64_t)(mf - mods.files) : file_index(p);
    if (mods_n && restbl_on && modfs_is_restbl(p)) {   /* [UIT 0042] the merged table, made the first time */
        uint64_t gi = file_index(p);
        if (gi != UINT64_MAX) {
            pthread_mutex_lock(&restbl_lock);
            if (restbl_index == UINT64_MAX) restbl_index = gi, restbl_build(gi);
            pthread_mutex_unlock(&restbl_lock);
            if (restbl_buf && restbl_index == gi) i = ROM_MEM | MEM_RESTBL, mf = NULL;
        }
    }
    if (i != UINT64_MAX && !(i & ROM_MEM)) {   /* [UIT 0065] romfix: the game's file or the mod's, edited */
        int slot = fix_slot(p, mf);
        if (slot >= 0) i = ROM_MEM | (uint64_t)slot, mf = NULL;
    }
    if (i == UINT64_MAX) return R_PATH_NOT_FOUND;
    int fd = -1;
    if (i & ROM_MEM) runtime_fs_log((i & 0xffff) == MEM_RESTBL ? "rom (merged size table)" : "rom (romfix)", p);
    else if (mf) {
        if ((fd = open(mf->host, O_RDONLY | O_BINARY)) < 0) {
            fprintf(stderr, "[totk] mods: cannot open %s (%s): the game's own file is used\n", mf->host, strerror(errno));
            if ((i = file_index(p)) == UINT64_MAX) return R_PATH_NOT_FOUND;
            mf = NULL;
        } else runtime_fs_log("rom (mod)", mf->host);
    }
    if (!mf && rom_folder && !(i & ROM_MEM)) {
        char full[8192];
        snprintf(full, sizeof full, "%s/%s", rom_dir, p);
        if ((fd = open(full, O_RDONLY | O_BINARY)) < 0) return R_PATH_NOT_FOUND;
    }
    pthread_mutex_lock(&rom_lock);
    for (uint64_t h = 1; h < NROM; h++)
        if (!rom_handles[h]) { rom_handles[h] = i & (ROM_MOD | ROM_MEM) ? i : i + 1; rom_fds[h] = fd; *out = ROM_BASE + h; pthread_mutex_unlock(&rom_lock); return 0; }
    pthread_mutex_unlock(&rom_lock);
    runtime_fail("RomFS handle table full");
}
static unsigned romfs_read(uint64_t *out, uint64_t h, int64_t offset, void *buffer, uint64_t size) {
    if (!out || offset < 0 || (!buffer && size)) return R_ROM_INVALID;
    uint64_t i = rom_handles[h - ROM_BASE];
    if (!i) return R_ROM_INVALID;
    Entry e;
    int fd = rom_folder ? rom_fds[h - ROM_BASE] : image_fd;
    if (i & ROM_MEM) {
        size_t ml;
        const uint8_t *mb = mem_file(i, &ml);
        if ((uint64_t)offset > ml) return R_ROM_INVALID;
        uint64_t n = size < ml - (uint64_t)offset ? size : ml - (uint64_t)offset;
        guest_prepare_write(buffer, n);
        memcpy(buffer, mb + offset, (size_t)n);
        *out = n;
        return 0;
    }
    if (i & ROM_MOD) e.offset = 0, e.size = mods.files[i & ~ROM_MOD].size, fd = rom_fds[h - ROM_BASE];
    else e = entry(i - 1);
    if ((uint64_t)offset > e.size) return R_ROM_INVALID;
    uint64_t n = size < e.size - (uint64_t)offset ? size : e.size - (uint64_t)offset, done = 0;
    guest_prepare_write(buffer, n);
    while (done < n) {
        ssize_t r = pread(fd, (uint8_t *)buffer + done, (size_t)(n - done), (off_t)(e.offset + (uint64_t)offset + done));
        need(r > 0, "RomFS image read");
        done += (uint64_t)r;
    }
    *out = n;
    return 0;
}
static unsigned romfs_size(int64_t *out, uint64_t h) {
    uint64_t i = rom_handles[h - ROM_BASE];
    if (!out || !i) return R_ROM_INVALID;
    size_t ml = 0;
    if (i & ROM_MEM) mem_file(i, &ml);
    *out = (int64_t)(i & ROM_MEM ? ml : i & ROM_MOD ? mods.files[i & ~ROM_MOD].size : entry(i - 1).size);
    return 0;
}
static void romfs_close(uint64_t h) {
    pthread_mutex_lock(&rom_lock);
    uint64_t i = rom_handles[h - ROM_BASE];
    need(i != 0, "close of a closed RomFS handle");
    rom_handles[h - ROM_BASE] = 0;
    if (!(i & ROM_MEM) && (rom_folder || (i & ROM_MOD))) close(rom_fds[h - ROM_BASE]);
    pthread_mutex_unlock(&rom_lock);
}
static unsigned romfs_entry_type(int *out, const char *path) {
    if (!out) return R_ROM_INVALID;
    const char *p = rom_path(path);
    if (!p) return R_PATH_NOT_FOUND;
    if (file_index(p) != UINT64_MAX || (mods_n && modfs_find(&mods, p))) { *out = 1; return 0; }
    char dir[4096];
    if (snprintf(dir, sizeof dir, "%s/", p) >= (int)sizeof dir) return R_ROM_INVALID;
    if (folder_exists(dir) || (mods_n && modfs_has_folder(&mods, dir))) { *out = 0; return 0; }
    return R_PATH_NOT_FOUND;
}
static unsigned mount_rom(const char *name, void *cache, uint64_t size) {
    (void)cache; (void)size;
    runtime_fs_init();
    if (!name || !*name || strlen(name) >= sizeof mount_name || strchr(name, '/') || strchr(name, ':')) return R_ROM_INVALID;
    if (*mount_name && strcmp(name, mount_name)) runtime_fail("second RomFS mount not implemented");
    strcpy(mount_name, name);
    fprintf(stderr, "[totk] fs mount %s: -> RomFS\n", name);
    /* [UIT 0068] the HeapDef decided now, before any game code can read the canvas size: a refused HeapDef must
       leave the canvas at 1080p from the start, never a canvas the heaps were not grown for */
    if (romfix_wants(ROMFIX_HEAPDEF)) fix_slot(ROMFIX_HEAPDEF, mods_n ? modfs_find(&mods, ROMFIX_HEAPDEF) : NULL);
    return 0;
}
static unsigned rom_cache_size(uint64_t *out) { if (!out) return R_ROM_INVALID; *out = 0; return 0; }

/* =============================== save, temporary and cache storage =============================== */
typedef struct { int kind; char name[32]; char root[1024]; } Mount;
typedef struct { int fd, mode, save; } File;   /* save: on a save-data mount (counts for runtime_fs_save_busy) */
typedef struct { DIR *dir; char path[1024]; int mode; } Dir;
static Mount mounts[16];
static File files[NFILES];
static Dir dirs[NDIRS];
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static char profile[1024], save_root[1024];
static pthread_once_t once = PTHREAD_ONCE_INIT;

/* Save in progress: a save-data file open for writing, or save data changed in the last 3 s and not committed yet. */
static _Atomic int save_writers, save_dirty;
static _Atomic uint64_t save_changed_ns;
static void save_touched(void) { atomic_store(&save_changed_ns, runtime_now_ns()); atomic_store(&save_dirty, 1); }
int runtime_fs_save_busy(void) {
    if (atomic_load(&save_writers) > 0 || uit_save_busy()) return 1;   /* [UIT 0080] or a commit still being flushed */
    return atomic_load(&save_dirty) && runtime_now_ns() - atomic_load(&save_changed_ns) < 3000000000ull;
}
static void make_dir(const char *p) { if (make_dir_p(p) && errno != EEXIST) runtime_fail("create profile storage directory"); }
static int is_dir(const char *p) { struct stat st; return !stat(p, &st) && S_ISDIR(st.st_mode); }
static int remove_tree(const char *path) {
    DIR *d = opendir(path);
    if (!d) return errno == ENOENT ? 0 : -1;
    struct dirent *e;
    int fail = 0;
    char child[2048];
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        snprintf(child, sizeof child, "%s/%s", path, e->d_name);
        fail |= is_dir(child) ? remove_tree(child) : unlink(child);
    }
    closedir(d);
    return fail | rmdir(path);
}
static void init(void) {
    need(snprintf(profile, sizeof profile, "%s", runtime_profile_dir()) < (int)sizeof profile, "profile path");
    uint64_t uid[2];
    runtime_account_uid(uid);
    char p[1100];
    make_dir(profile);
    snprintf(p, sizeof p, "%s/save", profile); make_dir(p);
    need(snprintf(save_root, sizeof save_root, "%s/save/%016llx%016llx", profile, (unsigned long long)uid[1], (unsigned long long)uid[0]) < (int)sizeof save_root, "save path");
    make_dir(save_root);                                   /* launcher-style EnsureSaveData */
    uit_save_init(save_root, profile);                     /* [UIT 0080] TOTK_SAVE_WRITES; an unfinished save rolled back */
    snprintf(p, sizeof p, "%s/temp", profile); remove_tree(p); make_dir(p);
    snprintf(p, sizeof p, "%s/cache", profile); make_dir(p);
    fprintf(stderr, "[totk] host save storage: %s\n", save_root);
}
static void ready(void) { pthread_once(&once, init); }

static unsigned mount_add(const char *name, int kind, const char *root) {
    if (!name || !*name || strlen(name) >= 32 || strchr(name, ':') || strchr(name, '/')) return R_INVALID_ARGUMENT;
    pthread_mutex_lock(&lock);
    int slot = -1;
    for (int i = 0; i < 16; i++) {
        if (mounts[i].kind && !strcmp(mounts[i].name, name)) { pthread_mutex_unlock(&lock); return R_ALREADY_EXISTS; }
        if (!mounts[i].kind && slot < 0) slot = i;
    }
    if (slot < 0) runtime_fail("fs mount table full");
    mounts[slot].kind = kind;
    strcpy(mounts[slot].name, name);
    snprintf(mounts[slot].root, sizeof mounts[slot].root, "%s", root);
    pthread_mutex_unlock(&lock);
    fprintf(stderr, "[totk] fs mount %s: -> %s\n", name, root);
    return 0;
}
static int component_ok(const char *c, size_t n) { return n && !(n == 1 && c[0] == '.') && !(n == 2 && c[0] == '.' && c[1] == '.'); }
/* "name:/a/b" -> host path; 0 on success; *kind = the mount's kind (M_SAVE ...) when kind is given */
static unsigned host_path_kind(const char *path, char *out, size_t cap, int *kind) {
    if (!path) return R_INVALID_PATH;
    const char *colon = strchr(path, ':');
    if (!colon || colon[1] != '/') return R_INVALID_PATH;
    size_t n = (size_t)(colon - path);
    pthread_mutex_lock(&lock);
    const Mount *m = NULL;
    for (int i = 0; i < 16; i++) if (mounts[i].kind && strlen(mounts[i].name) == n && !strncmp(mounts[i].name, path, n)) m = &mounts[i];
    char root[1024];
    if (m) strcpy(root, m->root);
    int k = m ? m->kind : M_FREE;
    pthread_mutex_unlock(&lock);
    if (!m) return R_PATH_NOT_FOUND;
    if (kind) *kind = k;
    const char *rest = colon + 1;
    for (const char *q = rest + 1; *q;) {
        const char *e = strchr(q, '/');
        size_t len = e ? (size_t)(e - q) : strlen(q);
        if (!e && !len) break;
        if (!component_ok(q, len) || memchr(q, '\\', len)) return R_INVALID_PATH;
        q = e ? e + 1 : q + len;
    }
    if (snprintf(out, cap, "%s%s", root, rest) >= (int)cap) return R_INVALID_PATH;
    size_t L = strlen(out);
    while (L > strlen(root) && out[L - 1] == '/') out[--L] = 0;
    return 0;
}
static unsigned host_path(const char *path, char *out, size_t cap) { return host_path_kind(path, out, cap, NULL); }
static unsigned save_change_path(const char *path, char *out, size_t cap) {
    int kind = M_FREE;
    unsigned r = host_path_kind(path, out, cap, &kind);
    if (!r && kind == M_SAVE) { uit_save_touch(out); save_touched(); }   /* [UIT 0080] before the change */
    return r;
}
static unsigned errno_result(void) {
    return errno == ENOENT || errno == ENOTDIR ? R_PATH_NOT_FOUND : errno == EEXIST ? R_ALREADY_EXISTS
         : errno == ENOTEMPTY ? R_NOT_EMPTY : R_INVALID_ARGUMENT;
}

static unsigned mount_save(const char *name, const uint64_t *uid) {
    ready();
    uint64_t me[2];
    runtime_account_uid(me);
    if (!uid || uid[0] != me[0] || uid[1] != me[1]) return R_TARGET_NOT_FOUND;
    return mount_add(name, M_SAVE, save_root);
}
static unsigned char save_exists(uint64_t application, const uint64_t *uid) {
    ready();
    uint64_t me[2];
    runtime_account_uid(me);
    return application == TOTK_APPLICATION_ID && uid && uid[0] == me[0] && uid[1] == me[1];
}
/* Other titles' save data (e.g. data-link bonuses) is not present on this host. */
static unsigned mount_save_ro(const char *name, uint64_t application, const uint64_t *uid) {
    if (!save_exists(application, uid)) return R_TARGET_NOT_FOUND;
    return mount_add(name, M_SAVE, save_root);
}
static unsigned mount_temp(const char *name) { ready(); char p[1100]; snprintf(p, sizeof p, "%s/temp", profile); return mount_add(name, M_TEMP, p); }
static void cache_dir(int index, char *out, size_t cap) { snprintf(out, cap, "%s/cache/%d", profile, index); }
static unsigned create_cache(int index, int64_t size, int64_t journal) {
    ready();
    if (index < 0 || size < 0 || journal < 0) return R_INVALID_ARGUMENT;
    char p[1100];
    cache_dir(index, p, sizeof p);
    if (make_dir_p(p)) return errno == EEXIST ? R_ALREADY_EXISTS : errno_result();
    return 0;
}
static unsigned delete_cache(int index) { ready(); char p[1100]; cache_dir(index, p, sizeof p); return remove_tree(p) ? R_TARGET_NOT_FOUND : 0; }
static unsigned mount_cache(const char *name, int index) {
    ready();
    char p[1100];
    cache_dir(index, p, sizeof p);
    if (!is_dir(p)) return R_TARGET_NOT_FOUND;
    return mount_add(name, M_CACHE, p);
}
typedef struct { int32_t index; uint8_t reserved[0x1c]; } CacheStorageInfo;
typedef struct { int list[64]; int count, next; } CacheList;
static unsigned open_cache_list(uint64_t *out) {
    ready();
    if (!out) return R_INVALID_ARGUMENT;
    CacheList *l = calloc(1, sizeof *l);
    need(l != NULL, "cache list allocation");
    char p[1100];
    snprintf(p, sizeof p, "%s/cache", profile);
    DIR *d = opendir(p);
    struct dirent *e;
    while (d && (e = readdir(d)) && l->count < 64) {
        char *end;
        long v = strtol(e->d_name, &end, 10);
        if (*e->d_name && !*end && v >= 0) l->list[l->count++] = (int)v;
    }
    if (d) closedir(d);
    *out = (uint64_t)(uintptr_t)l;
    return 0;
}
static unsigned read_cache_list(int *count, CacheStorageInfo *out, uint64_t h, int max) {
    CacheList *l = (CacheList *)(uintptr_t)h;
    if (!l || !count || max < 0 || (!out && max)) return R_INVALID_ARGUMENT;
    int n = 0;
    while (n < max && l->next < l->count) { memset(&out[n], 0, sizeof *out); out[n++].index = l->list[l->next++]; }
    *count = n;
    return 0;
}
static void close_cache_list(uint64_t h) { free((void *)(uintptr_t)h); }
static void unmount(const char *name) {
    pthread_mutex_lock(&lock);
    for (int i = 0; i < 16; i++)
        if (mounts[i].kind && !strcmp(mounts[i].name, name)) { mounts[i].kind = M_FREE; pthread_mutex_unlock(&lock); return; }
    pthread_mutex_unlock(&lock);
    if (!romfs_owns(name) && strcmp(name, mount_name)) runtime_fail("unmount of unknown fs mount");
}
static unsigned commit(const char *name) {
    pthread_mutex_lock(&lock);
    int found = 0, save = 0;
    for (int i = 0; i < 16; i++) if (mounts[i].kind && !strcmp(mounts[i].name, name)) { found = 1; save = mounts[i].kind == M_SAVE; }
    pthread_mutex_unlock(&lock);
    if (!found) return R_PATH_NOT_FOUND;
    if (save && uit_save_mode()) {   /* [UIT 0080] the changed files, flushed on a background thread */
        uit_save_commit();
        atomic_store(&save_dirty, 0);
        return 0;
    }
    for (int i = 0; i < NFILES; i++) {
        pthread_mutex_lock(&lock);
        int fd = files[i].fd;
        pthread_mutex_unlock(&lock);
        if (fd > 0) fsync(fd);
    }
    if (save) atomic_store(&save_dirty, 0);
    return 0;
}

static File *file_of(uint64_t h, int *fd, int *mode) {
    if (h < FILE_BASE || h >= FILE_BASE + NFILES) return NULL;
    pthread_mutex_lock(&lock);
    File *f = &files[h - FILE_BASE];
    int v = f->fd;
    if (fd) *fd = f->fd;
    if (mode) *mode = f->mode;
    pthread_mutex_unlock(&lock);
    return v > 0 ? f : NULL;
}
static unsigned open_file(uint64_t *out, const char *path, int mode) {
    if (romfs_owns(path)) return romfs_open(out, path, mode);
    if (path) runtime_fs_log(mode & 2 ? "write" : "read", path);
    if (!out || !(mode & (OPEN_READ | OPEN_WRITE)) || (mode & ~7)) return R_INVALID_ARGUMENT;
    char p[1100];
    int kind = M_FREE;
    unsigned r = host_path_kind(path, p, sizeof p, &kind);
    if (r) return r;
    struct stat st;
    if (stat(p, &st) || !S_ISREG(st.st_mode)) return R_PATH_NOT_FOUND;
    int save_writer = kind == M_SAVE && (mode & OPEN_WRITE);
    if (save_writer) uit_save_touch(p);   /* [UIT 0080] before the first write */
    int fd = open(p, (mode & OPEN_WRITE ? (mode & OPEN_READ ? O_RDWR : O_WRONLY) : O_RDONLY) | O_BINARY);
    if (fd < 0) return errno_result();
    pthread_mutex_lock(&lock);
    for (int i = 0; i < NFILES; i++)
        if (files[i].fd <= 0) {
            files[i] = (File){fd, mode, save_writer};
            *out = FILE_BASE + (uint64_t)i;
            pthread_mutex_unlock(&lock);
            if (save_writer) { atomic_fetch_add(&save_writers, 1); save_touched(); }
            return 0;
        }
    pthread_mutex_unlock(&lock);
    runtime_fail("host file handle table full");
}
static unsigned read_file(uint64_t *out, uint64_t h, int64_t offset, void *buffer, uint64_t size) {
    if (romfs_handle(h)) return romfs_read(out, h, offset, buffer, size);
    int fd, mode;
    if (!out || offset < 0 || (!buffer && size) || !file_of(h, &fd, &mode)) return R_INVALID_ARGUMENT;
    if (!(mode & OPEN_READ)) return R_OPEN_MODE;
    guest_prepare_write(buffer, size);
    uint64_t done = 0;
    while (done < size) {
        ssize_t n = pread(fd, (uint8_t *)buffer + done, (size_t)(size - done), offset + (off_t)done);
        if (n < 0) runtime_fail("host save read failure");
        if (!n) break;
        done += (uint64_t)n;
    }
    *out = done;
    return 0;
}
static unsigned read_options(uint64_t *out, uint64_t h, int64_t off, void *buf, uint64_t size, const void *options) {
    (void)options;
    return read_file(out, h, off, buf, size);
}
static unsigned read_exact(uint64_t h, int64_t off, void *buf, uint64_t size) {
    uint64_t n = 0;
    unsigned r = read_file(&n, h, off, buf, size);
    return r ? r : n == size ? 0 : R_INVALID_ARGUMENT;
}
static unsigned write_file(uint64_t h, int64_t offset, const void *buffer, uint64_t size, const int32_t *option) {
    int fd, mode;
    File *f = offset < 0 || (!buffer && size) ? NULL : file_of(h, &fd, &mode);
    if (!f) return R_INVALID_ARGUMENT;
    if (!(mode & OPEN_WRITE)) return R_OPEN_MODE;
    struct stat st;
    if (fstat(fd, &st)) runtime_fail("host save stat");
    if ((uint64_t)offset + size > (uint64_t)st.st_size && !(mode & OPEN_APPEND)) return R_APPEND_REQUIRED;
    if (f->save) save_touched();
    uint64_t done = 0;
    while (done < size) {
        ssize_t n = pwrite(fd, (const uint8_t *)buffer + done, (size_t)(size - done), offset + (off_t)done);
        if (n <= 0) {
            fprintf(stderr, "[totk] save write at %#llx (%#llx bytes from %p) failed: errno %d\n",
                    (unsigned long long)(offset + (off_t)done), (unsigned long long)(size - done),
                    (const void *)((const uint8_t *)buffer + done), errno);
            runtime_fail("host save write failure");
        }
        done += (uint64_t)n;
    }
    if (option && (*option & 1)) fsync(fd);
    return 0;
}
static unsigned file_size(int64_t *out, uint64_t h) {
    if (romfs_handle(h)) return romfs_size(out, h);
    int fd;
    struct stat st;
    if (!out || !file_of(h, &fd, NULL) || fstat(fd, &st)) return R_INVALID_ARGUMENT;
    *out = st.st_size;
    return 0;
}
static unsigned set_file_size(uint64_t h, int64_t size) {
    int fd, mode;
    if (size < 0 || !file_of(h, &fd, &mode)) return R_INVALID_ARGUMENT;
    if (!(mode & OPEN_WRITE)) return R_OPEN_MODE;
    if (files[h - FILE_BASE].save) save_touched();
    return _chsize_s(fd, size) ? errno_result() : 0;
}
static unsigned flush_file(uint64_t h) { int fd; if (!file_of(h, &fd, NULL)) return R_INVALID_ARGUMENT; fsync(fd); return 0; }
static void close_file(uint64_t h) {
    if (romfs_handle(h)) { romfs_close(h); return; }
    if (!file_of(h, NULL, NULL)) runtime_fail("close invalid file handle");
    pthread_mutex_lock(&lock);
    int fd = files[h - FILE_BASE].fd, save = files[h - FILE_BASE].save;
    files[h - FILE_BASE].fd = 0;
    pthread_mutex_unlock(&lock);
    close(fd);
    if (save) { save_touched(); atomic_fetch_sub(&save_writers, 1); }
}
static unsigned create_file(const char *path, int64_t size) {
    char p[1100];
    unsigned r = save_change_path(path, p, sizeof p);
    if (r) return r;
    if (size < 0) return R_INVALID_ARGUMENT;
    int fd = open(p, O_WRONLY | O_CREAT | O_EXCL | O_BINARY, 0600);
    if (fd < 0) return errno_result();
    int bad = _chsize_s(fd, size);
    close(fd);
    return bad ? errno_result() : 0;
}
static unsigned delete_file(const char *path) {
    char p[1100];
    unsigned r = save_change_path(path, p, sizeof p);
    if (r) return r;
    return unlink(p) ? errno_result() : 0;
}
static unsigned rename_file(const char *from, const char *to) {
    char a[1100], b[1100];
    unsigned r = save_change_path(from, a, sizeof a);
    if (!r) r = save_change_path(to, b, sizeof b);   /* [UIT 0080] the new name changes too (was host_path) */
    if (r) return r;
    struct stat st;
    if (!stat(b, &st)) return R_ALREADY_EXISTS;
    return rename(a, b) ? errno_result() : 0;
}
static unsigned entry_type(int *out, const char *path) {
    if (romfs_owns(path)) return romfs_entry_type(out, path);
    char p[1100];
    unsigned r = host_path(path, p, sizeof p);
    if (r) return r;
    struct stat st;
    if (!out) return R_INVALID_ARGUMENT;
    if (stat(p, &st)) return R_PATH_NOT_FOUND;
    *out = S_ISDIR(st.st_mode) ? 0 : 1;
    return 0;
}

typedef struct { char name[769]; uint8_t pad0[3]; int8_t type; uint8_t pad1[3]; int64_t size; } DirectoryEntry;
_Static_assert(sizeof(DirectoryEntry) == 0x310, "nn::fs::DirectoryEntry");
static unsigned create_directory(const char *path) {
    char p[1100];
    unsigned r = save_change_path(path, p, sizeof p);
    if (r) return r;
    return make_dir_p(p) ? errno_result() : 0;
}
static unsigned delete_recursive(const char *path) {
    char p[1100];
    unsigned r = save_change_path(path, p, sizeof p);
    if (r) return r;
    struct stat st;
    if (stat(p, &st)) return R_PATH_NOT_FOUND;
    return remove_tree(p) ? R_INVALID_ARGUMENT : 0;
}
static unsigned open_directory(uint64_t *out, const char *path, int mode) {
    if (romfs_owns(path)) { fprintf(stderr, "[totk] OpenDirectory on RomFS %s\n", path); runtime_fail("RomFS directory listing not implemented"); }
    char p[1100];
    unsigned r = host_path(path, p, sizeof p);
    if (r) return r;
    if (!out || mode < 1 || mode > 3) return R_INVALID_ARGUMENT;
    DIR *d = opendir(p);
    if (!d) return errno_result();
    pthread_mutex_lock(&lock);
    for (int i = 0; i < NDIRS; i++)
        if (!dirs[i].dir) {
            dirs[i].dir = d; dirs[i].mode = mode; strcpy(dirs[i].path, p);
            *out = DIR_BASE + (uint64_t)i;
            pthread_mutex_unlock(&lock);
            return 0;
        }
    pthread_mutex_unlock(&lock);
    runtime_fail("host directory handle table full");
}
static unsigned read_directory(int64_t *count, DirectoryEntry *out, uint64_t h, int64_t max) {
    if (h < DIR_BASE || h >= DIR_BASE + NDIRS || !count || max < 0 || (!out && max)) return R_INVALID_ARGUMENT;
    Dir *d = &dirs[h - DIR_BASE];
    if (!d->dir) return R_INVALID_ARGUMENT;
    int64_t n = 0;
    struct dirent *e;
    while (n < max && (e = readdir(d->dir))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..") || strlen(e->d_name) > 768) continue;
        char child[2048];
        snprintf(child, sizeof child, "%s/%s", d->path, e->d_name);
        struct stat st;
        if (stat(child, &st)) continue;
        int dir = S_ISDIR(st.st_mode);
        if (!(d->mode & (dir ? 1 : 2))) continue;
        memset(&out[n], 0, sizeof *out);
        strcpy(out[n].name, e->d_name);
        out[n].type = dir ? 0 : 1;
        out[n].size = dir ? 0 : st.st_size;
        n++;
    }
    *count = n;
    return 0;
}
static void close_directory(uint64_t h) {
    if (h < DIR_BASE || h >= DIR_BASE + NDIRS || !dirs[h - DIR_BASE].dir) runtime_fail("close invalid directory handle");
    pthread_mutex_lock(&lock);
    closedir(dirs[h - DIR_BASE].dir);
    dirs[h - DIR_BASE].dir = NULL;
    pthread_mutex_unlock(&lock);
}
static void set_priority(int priority) { (void)priority; }   /* fs priority orders I/O inside the system fs process */
static void fs_initialize(void) { runtime_fs_init(); }

HLE_NATIVE("_ZN2nn2fs24InitializeForApplicationEv", fs_initialize, 'v')
HLE_NATIVE("_ZN2nn2fs8MountRomEPKcPvm", mount_rom, 'w')
HLE_NATIVE("_ZN2nn2fs22QueryMountRomCacheSizeEPm", rom_cache_size, 'w')
HLE_NATIVE("_ZN2nn2fs8OpenFileEPNS0_10FileHandleEPKci", open_file, 'w')
HLE_NATIVE("_ZN2nn2fs11GetFileSizeEPlNS0_10FileHandleE", file_size, 'w')
HLE_NATIVE("_ZN2nn2fs9CloseFileENS0_10FileHandleE", close_file, 'v')
HLE_NATIVE("_ZN2nn2fs12GetEntryTypeEPNS0_18DirectoryEntryTypeEPKc", entry_type, 'w')
HLE_NATIVE("_ZN2nn2fs8ReadFileEPmNS0_10FileHandleElPvm", read_file, 'w')
HLE_NATIVE("_ZN2nn2fs8ReadFileEPmNS0_10FileHandleElPvmRKNS0_10ReadOptionE", read_options, 'w')
HLE_NATIVE("_ZN2nn2fs8ReadFileENS0_10FileHandleElPvm", read_exact, 'w')
HLE_NATIVE("_ZN2nn2fs9WriteFileENS0_10FileHandleElPKvmRKNS0_11WriteOptionE", write_file, 'w')
HLE_NATIVE("_ZN2nn2fs11SetFileSizeENS0_10FileHandleEl", set_file_size, 'w')
HLE_NATIVE("_ZN2nn2fs9FlushFileENS0_10FileHandleE", flush_file, 'w')
HLE_NATIVE("_ZN2nn2fs10CreateFileEPKcl", create_file, 'w')
HLE_NATIVE("_ZN2nn2fs10DeleteFileEPKc", delete_file, 'w')
HLE_NATIVE("_ZN2nn2fs10RenameFileEPKcS2_", rename_file, 'w')
HLE_NATIVE("_ZN2nn2fs15CreateDirectoryEPKc", create_directory, 'w')
HLE_NATIVE("_ZN2nn2fs26DeleteDirectoryRecursivelyEPKc", delete_recursive, 'w')
HLE_NATIVE("_ZN2nn2fs13OpenDirectoryEPNS0_15DirectoryHandleEPKci", open_directory, 'w')
HLE_NATIVE("_ZN2nn2fs13ReadDirectoryEPlPNS0_14DirectoryEntryENS0_15DirectoryHandleEl", read_directory, 'w')
HLE_NATIVE("_ZN2nn2fs14CloseDirectoryENS0_15DirectoryHandleE", close_directory, 'v')
HLE_NATIVE("_ZN2nn2fs13MountSaveDataEPKcRKNS_7account3UidE", mount_save, 'w')
HLE_NATIVE("_ZN2nn2fs21MountSaveDataReadOnlyEPKcNS_13ApplicationIdERKNS_7account3UidE", mount_save_ro, 'w')
HLE_NATIVE("_ZN2nn2fs18IsSaveDataExistingENS_13ApplicationIdERKNS_7account3UidE", save_exists, 'b')
HLE_NATIVE("_ZN2nn2fs21MountTemporaryStorageEPKc", mount_temp, 'w')
HLE_NATIVE("_ZN2nn2fs7UnmountEPKc", unmount, 'v')
HLE_NATIVE("_ZN2nn2fs6CommitEPKc", commit, 'w')
HLE_NATIVE("_ZN2nn2fs18CreateCacheStorageEill", create_cache, 'w')
HLE_NATIVE("_ZN2nn2fs18DeleteCacheStorageEi", delete_cache, 'w')
HLE_NATIVE("_ZN2nn2fs17MountCacheStorageEPKci", mount_cache, 'w')
HLE_NATIVE("_ZN2nn2fs20OpenCacheStorageListEPNS0_22CacheStorageListHandleE", open_cache_list, 'w')
HLE_NATIVE("_ZN2nn2fs20ReadCacheStorageListEPiPNS0_16CacheStorageInfoENS0_22CacheStorageListHandleEi", read_cache_list, 'w')
HLE_NATIVE("_ZN2nn2fs21CloseCacheStorageListENS0_22CacheStorageListHandleE", close_cache_list, 'v')
HLE_NATIVE("_ZN2nn2fs26SetPriorityOnCurrentThreadENS0_8PriorityE", set_priority, 'v')
