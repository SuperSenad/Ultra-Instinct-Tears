/* [UIT 0041] Mods: a folder of RomFS mods (Yuzu / Ryujinx layout, <mods>/<Name>/romfs/...) laid over the game's RomFS.
 * Shared by the game (src/host/fs.c) and the launcher (built as C++), so both read the same load order and agree on
 * which mod wins each file.
 *
 *  - <mods>/load-order.txt (written by the launcher): one mod per line, top = highest priority, "+Name" on, "-Name" off,
 *    '#' comments. A folder the file does not list is on and goes above the listed ones (new mods win), by name.
 *  - A mod's files: <mod>/romfs/ (any case), else the mod folder itself when it holds the game's own top-level folders
 *    (Actor, Pack, UI, ...): a mod unpacked without its romfs folder.
 *  - What cannot load is reported, never skipped silently: exefs/ code patches (.ips, .pchtxt, NSO), cheats/, and a mod
 *    with no RomFS files at all.
 * No mods (or TOTK_MODS=0): nothing is read and the game's file lookups are as before. */
#ifndef UIT_MODFS_H
#define UIT_MODFS_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

enum {
    MODFS_U_EXEFS = 1,     /* exefs/ folder (code patches) */
    MODFS_U_IPS = 2,       /* .ips patches */
    MODFS_U_PCHTXT = 4,    /* .pchtxt patches */
    MODFS_U_NSO = 8,       /* main / subsdk / .nso code */
    MODFS_U_CHEATS = 16,   /* cheats/ (Atmosphere cheat codes) */
    MODFS_U_EMPTY = 32,    /* no RomFS files at all */
    MODFS_U_FPSRES = 64,   /* a code patch whose name says frame rate / resolution: built into Ultra Instinct Tears */
    MODFS_U_DLL = 128,     /* .dll / .exe (PC tools, not mods for this game) */
};
#define MODFS_CODE (MODFS_U_EXEFS | MODFS_U_IPS | MODFS_U_PCHTXT | MODFS_U_NSO)

typedef struct {
    char name[256];          /* folder name */
    char root[4096];         /* host path of its RomFS files ("" when none) */
    int enabled, listed;     /* listed: named in load-order.txt */
    unsigned unsupported;    /* MODFS_U_* */
    char title[128], version[64], author[128], description[600];   /* info.json, optional */
    int nfiles;              /* RomFS files */
    uint64_t bytes;
    int nwon, nlost;         /* files it wins / files a higher mod wins (enabled mods only) */
    int nnew;                /* files the game does not have (canon callback; -1 unknown) */
    int nrestbl;             /* ships a resource size table (System/Resource/ResourceSizeTable...) */
} ModfsMod;

typedef struct {
    char *path;              /* RomFS path, '/' separated, the game's spelling when canon found it */
    char *host;              /* its file */
    uint64_t size;
    int mod;                 /* winner (index into mods) */
    int nshadow;             /* lower mods with the same file */
    int vanilla;             /* the game has this file (canon callback; -1 unknown) */
} ModfsFile;

typedef struct {
    char dir[4096];
    ModfsMod *mods;          /* priority order: 0 = highest */
    int nmods;
    ModfsFile *files;        /* winners, sorted by path (strcmp) */
    size_t nfiles;
    int *pairs;              /* nmods * nmods: pairs[w * nmods + l] files mod w takes from mod l */
    char **restbls;          /* every enabled mod's resource size tables (host paths), highest mod first */
    int nrestbls;
} Modfs;

/* canon: the game's own spelling of a RomFS path (any case), NULL when the game has no such file; may be NULL.
   is_game_dir: whether a top-level folder name is one of the game's (NULL: a built-in list). */
typedef const char *(*modfs_canon_fn)(const char *path, void *ud);
typedef int (*modfs_gamedir_fn)(const char *name, void *ud);

/* Reads dir: the mods, their order, files and conflicts. walk_disabled: also count a disabled mod's files (the
   launcher's info); they never enter files. Returns the number of mods (0: none or no folder). */
int modfs_load(Modfs *m, const char *dir, int walk_disabled, modfs_canon_fn canon, modfs_gamedir_fn is_game_dir, void *ud);
void modfs_free(Modfs *m);
/* The winning file for a RomFS path, NULL when no enabled mod has it. */
const ModfsFile *modfs_find(const Modfs *m, const char *path);
/* Whether some enabled mod has a file under folder ("a/b/", with the slash). */
int modfs_has_folder(const Modfs *m, const char *folder);
/* Writes load-order.txt: names[0] highest. Returns 0 on success. */
int modfs_write_order(const char *dir, const char *const *names, const int *enabled, int n);
/* A built-in list of the game's top-level RomFS folders (case-insensitive). */
int modfs_known_game_dir(const char *name);
/* One line about what a mod's unsupported flags mean (empty string when none). */
const char *modfs_unsupported_text(unsigned u);
/* RomFS path -> its resource size table name (no .zs / .zstd / .mc; .ta.zs kept); returns out. */
char *modfs_restbl_name(const char *path, char *out, size_t cap);
int modfs_is_restbl(const char *path);

#ifdef __cplusplus
}
#endif
#endif
