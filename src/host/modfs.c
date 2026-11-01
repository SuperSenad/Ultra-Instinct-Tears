/* [UIT 0041] Mods: see modfs.h. Plain C that also builds as C++ (the launcher). */
#include "modfs.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <dirent.h>
#include <sys/stat.h>
#endif

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic ignored "-Wformat-truncation"   /* paths past 4 KB are cut, not overrun */
#endif

#define MF_ALLOC(T, n) ((T *)malloc(sizeof(T) * (size_t)(n) + 1))

static char *mf_strdup(const char *s) {
    size_t n = strlen(s) + 1;
    char *p = MF_ALLOC(char, n);
    if (p) memcpy(p, s, n);
    return p;
}

static int mf_ieq(const char *a, const char *b) {
    for (; *a && *b; a++, b++)
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return 0;
    return *a == *b;
}

static int mf_iends(const char *s, const char *suf) {
    size_t n = strlen(s), k = strlen(suf);
    return n >= k && mf_ieq(s + n - k, suf);
}

/* ---------------------------------------------------------------- directory listing */
typedef struct {
    char *name;
    int dir;
    uint64_t size;
} MfEnt;

typedef struct {
    MfEnt *v;
    int n, cap;
} MfList;

static void mf_list_free(MfList *l) {
    for (int i = 0; i < l->n; i++) free(l->v[i].name);
    free(l->v);
    l->v = NULL, l->n = l->cap = 0;
}

static void mf_list_add(MfList *l, const char *name, int dir, uint64_t size) {
    if (l->n == l->cap) {
        int cap = l->cap ? l->cap * 2 : 16;
        MfEnt *v = (MfEnt *)realloc(l->v, sizeof(MfEnt) * (size_t)cap);
        if (!v) return;
        l->v = v, l->cap = cap;
    }
    l->v[l->n].name = mf_strdup(name);
    l->v[l->n].dir = dir, l->v[l->n].size = size;
    if (l->v[l->n].name) l->n++;
}

/* The entries of folder path (no "." / ".."). Returns 0 when it cannot be read. */
static int mf_list(const char *path, MfList *out) {
    out->v = NULL, out->n = out->cap = 0;
#ifdef _WIN32
    char pat[4200];
    snprintf(pat, sizeof pat, "%s\\*", path);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileExA(pat, FindExInfoBasic, &fd, FindExSearchNameMatch, NULL, FIND_FIRST_EX_LARGE_FETCH);
    if (h == INVALID_HANDLE_VALUE) return 0;
    do {
        if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, "..")) continue;
        int dir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        mf_list_add(out, fd.cFileName, dir, ((uint64_t)fd.nFileSizeHigh << 32) | fd.nFileSizeLow);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
#else
    DIR *d = opendir(path);
    if (!d) return 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char full[4400];
        snprintf(full, sizeof full, "%s/%s", path, e->d_name);
        struct stat st;
        if (stat(full, &st)) continue;
        mf_list_add(out, e->d_name, S_ISDIR(st.st_mode) ? 1 : 0, (uint64_t)st.st_size);
    }
    closedir(d);
#endif
    return 1;
}

/* Files the OS or an archive tool leaves behind; never game data. */
static int mf_junk(const char *name) {
    return name[0] == '.' || !strncmp(name, "._", 2) || mf_ieq(name, "desktop.ini") || mf_ieq(name, "Thumbs.db") ||
           mf_ieq(name, "__MACOSX");
}

/* ---------------------------------------------------------------- the game's folders */
static const char *const kGameDirs[] = {
    "Actor", "AI", "AnimationEvent", "AS", "Banc", "Cave", "Collision", "Cooking", "Effect", "Event", "EventFlow", "Font",
    "GameData", "Gyml", "Layout", "LevelSensor", "Logic", "Mals", "Material", "Model", "Movie", "Pack", "Phive", "Physics",
    "Preset", "RSDB", "Sequence", "Shader", "Sound", "Speech", "System", "Tex", "TexToGo", "UI", "Voice", "XLink",
};

int modfs_known_game_dir(const char *name) {
    for (size_t i = 0; i < sizeof kGameDirs / sizeof *kGameDirs; i++)
        if (mf_ieq(name, kGameDirs[i])) return 1;
    return 0;
}

/* ---------------------------------------------------------------- info.json (a few string fields) */
static int mf_json_str(const char *js, const char *key, char *out, size_t cap) {
    size_t kl = strlen(key);
    for (const char *p = js; (p = strchr(p, '"'));) {
        const char *k = p + 1;
        const char *q = strchr(k, '"');
        if (!q) return 0;
        int match = (size_t)(q - k) == kl;
        for (size_t i = 0; match && i < kl; i++)
            if (tolower((unsigned char)k[i]) != tolower((unsigned char)key[i])) match = 0;
        p = q + 1;
        if (!match) continue;
        while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
        if (*p != ':') continue;
        p++;
        while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
        size_t n = 0;
        if (*p != '"') {   /* a number (a version like 1.2 written bare) */
            while (*p && *p != ',' && *p != '}' && *p != '\n' && n + 1 < cap) out[n++] = *p++;
            while (n && (out[n - 1] == ' ' || out[n - 1] == '\r')) n--;
            out[n] = 0;
            return n > 0;
        }
        for (p++; *p && *p != '"'; p++) {
            char c = *p;
            if (c == '\\' && p[1]) {
                p++;
                c = *p == 'n' ? '\n' : *p == 't' ? ' ' : *p == 'r' ? ' ' : *p;
                if (*p == 'u') {   /* \uXXXX: keep ASCII, otherwise a '?' */
                    unsigned v = 0;
                    int k2 = 0;
                    for (; k2 < 4 && isxdigit((unsigned char)p[1]); k2++, p++) v = v * 16 + (unsigned)(isdigit((unsigned char)*(p + 1)) ? *(p + 1) - '0' : (tolower((unsigned char)*(p + 1)) - 'a' + 10));
                    c = v && v < 128 ? (char)v : '?';
                }
            }
            if (n + 1 < cap) out[n++] = c;
        }
        out[n] = 0;
        return 1;
    }
    return 0;
}

static void mf_read_info(ModfsMod *mod, const char *folder) {
    char path[4400];
    snprintf(path, sizeof path, "%s/info.json", folder);
    FILE *f = fopen(path, "rb");
    if (!f) return;
    char buf[16384];
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[n] = 0;
    mf_json_str(buf, "name", mod->title, sizeof mod->title);
    mf_json_str(buf, "version", mod->version, sizeof mod->version);
    mf_json_str(buf, "author", mod->author, sizeof mod->author);
    if (!mod->author[0]) mf_json_str(buf, "authors", mod->author, sizeof mod->author);
    mf_json_str(buf, "description", mod->description, sizeof mod->description);
}

/* ---------------------------------------------------------------- one mod's files */
typedef struct {
    char *path, *host;
    uint64_t size;
    int mod, vanilla;
} MfFile;

typedef struct {
    MfFile *v;
    size_t n, cap;
} MfFiles;

static void mf_files_add(MfFiles *f, const char *path, const char *host, uint64_t size, int mod) {
    if (f->n == f->cap) {
        size_t cap = f->cap ? f->cap * 2 : 256;
        MfFile *v = (MfFile *)realloc(f->v, sizeof(MfFile) * cap);
        if (!v) return;
        f->v = v, f->cap = cap;
    }
    f->v[f->n].path = mf_strdup(path);
    f->v[f->n].host = mf_strdup(host);
    f->v[f->n].size = size, f->v[f->n].mod = mod, f->v[f->n].vanilla = -1;
    if (f->v[f->n].path && f->v[f->n].host) f->n++;
    else free(f->v[f->n].path), free(f->v[f->n].host);
}

/* The RomFS files under host folder dir, as paths rel/... (depth-limited: a link loop cannot hang the scan). */
static void mf_walk(const char *dir, const char *rel, int depth, int mod, MfFiles *out, int top_skip) {
    if (depth > 24) return;
    MfList l;
    if (!mf_list(dir, &l)) return;
    for (int i = 0; i < l.n; i++) {
        const char *nm = l.v[i].name;
        if (mf_junk(nm)) continue;
        /* a mod without a romfs folder: its own exefs/, cheats/ and info file are not RomFS */
        if (top_skip && !depth && (mf_ieq(nm, "exefs") || mf_ieq(nm, "cheats") || mf_ieq(nm, "info.json") ||
                                   mf_iends(nm, ".txt") || mf_iends(nm, ".md") || mf_iends(nm, ".png") || mf_iends(nm, ".jpg")))
            continue;
        char host[4400], r[1100];
        snprintf(host, sizeof host, "%s/%s", dir, nm);
        if (rel[0]) snprintf(r, sizeof r, "%s/%s", rel, nm);
        else snprintf(r, sizeof r, "%s", nm);
        if (l.v[i].dir) mf_walk(host, r, depth + 1, mod, out, top_skip);
        else mf_files_add(out, r, host, l.v[i].size, mod);
    }
    mf_list_free(&l);
}

/* What in the mod folder (outside its RomFS) cannot load: exefs/, patches, code, cheats. */
static unsigned mf_unsupported(const char *dir, int depth, const char *skip) {
    if (depth > 4) return 0;
    MfList l;
    if (!mf_list(dir, &l)) return 0;
    unsigned u = 0;
    for (int i = 0; i < l.n; i++) {
        const char *nm = l.v[i].name;
        char host[4400];
        snprintf(host, sizeof host, "%s/%s", dir, nm);
        if (skip && !strcmp(host, skip)) continue;
        if (l.v[i].dir) {
            if (mf_ieq(nm, "exefs")) u |= MODFS_U_EXEFS;
            if (mf_ieq(nm, "cheats")) u |= MODFS_U_CHEATS;
            if (depth < 1 && modfs_known_game_dir(nm) && !skip) continue;   /* bare layout: these are RomFS */
            u |= mf_unsupported(host, depth + 1, skip);
            continue;
        }
        if (mf_iends(nm, ".ips")) u |= MODFS_U_IPS;
        else if (mf_iends(nm, ".pchtxt")) u |= MODFS_U_PCHTXT;
        else if (mf_iends(nm, ".nso") || mf_ieq(nm, "main") || mf_ieq(nm, "main.npdm") || mf_ieq(nm, "rtld") ||
                 (!strncmp(nm, "subsdk", 6) && strlen(nm) <= 7))
            u |= MODFS_U_NSO;
        else if (mf_iends(nm, ".dll") || mf_iends(nm, ".exe"))
            u |= MODFS_U_DLL;
    }
    mf_list_free(&l);
    return u;
}

static int mf_name_says_fpsres(const ModfsMod *mod) {
    static const char *const words[] = {"fps", "60", "frame", "resolution", "1080p", "1440p", "2160p", "4k", "720p",
                                        "dynamic res", "dynamicres", "res fix", "aspect", "ultrawide", "21:9", "32:9"};
    char s[512];
    snprintf(s, sizeof s, "%s %s", mod->name, mod->title);
    for (char *p = s; *p; p++) *p = (char)tolower((unsigned char)*p);
    for (size_t i = 0; i < sizeof words / sizeof *words; i++)
        if (strstr(s, words[i])) return 1;
    return 0;
}

const char *modfs_unsupported_text(unsigned u) {
    if (u & MODFS_U_FPSRES)
        return "A frame rate / resolution code patch. Ultra Instinct Tears has these built in (Performance > Frame rate > "
               "Frame rate target, and Graphics > Image quality > Scene resolution), so the patch is not needed and cannot apply.";
    if (u & MODFS_CODE)
        return "Has code patches (exefs: .ips, .pchtxt or NSO). They change the Switch program's machine code; Ultra Instinct "
               "Tears runs the game recompiled for PC, so that code is not there and they cannot apply. Only its RomFS files load.";
    if (u & MODFS_U_CHEATS) return "Has cheat codes (cheats/). Cheats patch the Switch program's memory, which the recompiled PC game does "
                                        "not have, so they cannot apply. Only its RomFS files load.";
    if (u & MODFS_U_DLL) return "Has a .dll or .exe: a PC tool or another game's mod, not something this game loads.";
    if (u & MODFS_U_EMPTY) return "No RomFS files found (no romfs folder and none of the game's folders), so nothing loads.";
    return "";
}

/* ---------------------------------------------------------------- load order */
typedef struct {
    char name[256];
    int on;
} MfOrder;

static int mf_read_order(const char *dir, MfOrder **out) {
    *out = NULL;
    char path[4400];
    snprintf(path, sizeof path, "%s/load-order.txt", dir);
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    int n = 0, cap = 0;
    char line[600];
    while (fgets(line, sizeof line, f)) {
        size_t k = strcspn(line, "\r\n");
        line[k] = 0;
        while (k && (line[k - 1] == ' ' || line[k - 1] == '\t')) line[--k] = 0;
        char *s = line;
        if (!strncmp(s, "\xEF\xBB\xBF", 3)) s += 3;   /* a UTF-8 mark from an editor */
        while (*s == ' ' || *s == '\t') s++;
        if (!*s || *s == '#') continue;
        int on = 1;
        if (*s == '+' || *s == '-') on = *s == '+', s++;
        while (*s == ' ' || *s == '\t') s++;
        if (!*s) continue;
        if (n == cap) {
            cap = cap ? cap * 2 : 32;
            MfOrder *v = (MfOrder *)realloc(*out, sizeof(MfOrder) * (size_t)cap);
            if (!v) break;
            *out = v;
        }
        snprintf((*out)[n].name, sizeof(*out)[n].name, "%s", s);
        (*out)[n].on = on;
        n++;
    }
    fclose(f);
    return n;
}

int modfs_write_order(const char *dir, const char *const *names, const int *enabled, int n) {
    char path[4400], tmp[4400];
    snprintf(path, sizeof path, "%s/load-order.txt", dir);
    snprintf(tmp, sizeof tmp, "%s/load-order.txt.new", dir);
    FILE *f = fopen(tmp, "wb");
    if (!f) return -1;
    fprintf(f, "# Ultra Instinct Tears mods, highest priority first: a file in a mod higher up wins.\n"
               "# + on, - off. The launcher writes this file; a mod folder not listed here is on and goes on top.\n");
    for (int i = 0; i < n; i++) fprintf(f, "%c%s\n", enabled[i] ? '+' : '-', names[i]);
    int bad = ferror(f);
    if (fclose(f) || bad) { remove(tmp); return -1; }
#ifdef _WIN32
    if (!MoveFileExA(tmp, path, MOVEFILE_REPLACE_EXISTING)) { remove(tmp); return -1; }
#else
    if (rename(tmp, path)) { remove(tmp); return -1; }
#endif
    return 0;
}

/* ---------------------------------------------------------------- the index */
static int mf_cmp_name(const void *a, const void *b) {
    return strcmp(((const ModfsMod *)a)->name, ((const ModfsMod *)b)->name);
}

static int mf_cmp_file(const void *a, const void *b) {
    const MfFile *x = (const MfFile *)a, *y = (const MfFile *)b;
    int c = strcmp(x->path, y->path);
    return c ? c : x->mod - y->mod;
}

/* Where a mod's RomFS files are: 1 <folder>/romfs, 2 the folder itself (the game's own folders), 0 none. A mod copied in
   by hand with extra folders around it (mods/X/X/romfs, mods/X/atmosphere/contents/<title>/romfs) is followed down
   while each level holds just one folder, so it loads as it is instead of "no RomFS files". */
static int mf_root_at(ModfsMod *mod, const char *folder, modfs_gamedir_fn is_game_dir, void *ud, int depth) {
    MfList l;
    mod->root[0] = 0;
    if (!mf_list(folder, &l)) return 0;
    int bare = 0, ndirs = 0, only = -1;
    for (int i = 0; i < l.n; i++) {
        if (!l.v[i].dir || mf_junk(l.v[i].name)) continue;
        ndirs++, only = i;
        if (mf_ieq(l.v[i].name, "exefs") || mf_ieq(l.v[i].name, "cheats")) ndirs++;   /* a code mod: not followed */
        if (mf_ieq(l.v[i].name, "romfs")) {
            snprintf(mod->root, sizeof mod->root, "%s/%s", folder, l.v[i].name);
            mf_list_free(&l);
            return 1;
        }
        if (is_game_dir ? is_game_dir(l.v[i].name, ud) : modfs_known_game_dir(l.v[i].name)) bare = 1;
    }
    if (!bare && ndirs == 1 && depth < 5) {
        char sub[4400];
        snprintf(sub, sizeof sub, "%s/%s", folder, l.v[only].name);
        mf_list_free(&l);
        return mf_root_at(mod, sub, is_game_dir, ud, depth + 1);
    }
    mf_list_free(&l);
    if (bare) snprintf(mod->root, sizeof mod->root, "%s", folder);
    return bare ? 2 : 0;
}
static int mf_root(ModfsMod *mod, const char *folder, modfs_gamedir_fn is_game_dir, void *ud) {
    return mf_root_at(mod, folder, is_game_dir, ud, 0);
}

int modfs_load(Modfs *m, const char *dir, int walk_disabled, modfs_canon_fn canon, modfs_gamedir_fn is_game_dir, void *ud) {
    memset(m, 0, sizeof *m);
    snprintf(m->dir, sizeof m->dir, "%s", dir);
    MfList l;
    if (!mf_list(dir, &l)) return 0;
    int nfolders = 0;
    for (int i = 0; i < l.n; i++) nfolders += l.v[i].dir && !mf_junk(l.v[i].name);
    if (!nfolders) { mf_list_free(&l); return 0; }
    ModfsMod *found = (ModfsMod *)calloc((size_t)nfolders, sizeof(ModfsMod));
    m->mods = (ModfsMod *)calloc((size_t)nfolders, sizeof(ModfsMod));
    if (!found || !m->mods) { free(found); free(m->mods); m->mods = NULL; mf_list_free(&l); return 0; }
    int nf = 0;
    for (int i = 0; i < l.n; i++)
        if (l.v[i].dir && !mf_junk(l.v[i].name) && strlen(l.v[i].name) < sizeof found[0].name)
            snprintf(found[nf++].name, sizeof found[0].name, "%s", l.v[i].name);
    mf_list_free(&l);
    qsort(found, (size_t)nf, sizeof *found, mf_cmp_name);

    /* order: unlisted folders first (by name, on), then the file's order */
    MfOrder *ord = NULL;
    int no = mf_read_order(dir, &ord);
    char *used = (char *)calloc((size_t)nf + 1, 1);
    for (int i = 0; i < nf; i++) {
        int listed = 0;
        for (int k = 0; k < no && !listed; k++) listed = !strcmp(ord[k].name, found[i].name);
        if (!listed) { m->mods[m->nmods] = found[i]; m->mods[m->nmods].enabled = 1; m->nmods++; used[i] = 1; }
    }
    for (int k = 0; k < no; k++)
        for (int i = 0; i < nf; i++)
            if (!used[i] && !strcmp(ord[k].name, found[i].name)) {
                m->mods[m->nmods] = found[i];
                m->mods[m->nmods].enabled = ord[k].on, m->mods[m->nmods].listed = 1;
                m->nmods++, used[i] = 1;
                break;
            }
    free(used), free(ord), free(found);

    MfFiles all = {NULL, 0, 0};
    for (int i = 0; i < m->nmods; i++) {
        ModfsMod *mod = &m->mods[i];
        char folder[4400];
        snprintf(folder, sizeof folder, "%s/%s", dir, mod->name);
        mod->nnew = canon ? 0 : -1;
        mf_read_info(mod, folder);
        int kind = mf_root(mod, folder, is_game_dir, ud);
        mod->unsupported = mf_unsupported(folder, 0, kind == 1 ? mod->root : NULL);
        if ((mod->unsupported & MODFS_CODE) && mf_name_says_fpsres(mod)) mod->unsupported |= MODFS_U_FPSRES;
        if (!kind) { mod->unsupported |= MODFS_U_EMPTY; continue; }
        if (!mod->enabled && !walk_disabled) continue;
        size_t before = all.n;
        mf_walk(mod->root, "", 0, i, &all, kind == 2);
        mod->nfiles = (int)(all.n - before);
        for (size_t k = before; k < all.n; k++) {
            mod->bytes += all.v[k].size;
            if (!modfs_is_restbl(all.v[k].path)) continue;
            mod->nrestbl++;
            char **g = mod->enabled ? (char **)realloc(m->restbls, (size_t)(m->nrestbls + 1) * sizeof(char *)) : NULL;
            if (g && (g[m->nrestbls] = mf_strdup(all.v[k].host))) m->restbls = g, m->nrestbls++;
            else if (g) m->restbls = g;
        }
        if (!mod->nfiles) mod->unsupported |= MODFS_U_EMPTY;
        if (!mod->enabled) {   /* counted only */
            for (size_t k = before; k < all.n; k++) free(all.v[k].path), free(all.v[k].host);
            all.n = before;
        }
    }

    /* the game's spelling of each path, then sort: the first of each path (the highest mod) wins */
    if (all.n) {
        for (size_t k = 0; canon && k < all.n; k++) {
            const char *c = canon(all.v[k].path, ud);
            all.v[k].vanilla = c != NULL;
            if (c && strcmp(c, all.v[k].path)) {   /* "actor/..." in a mod, "Actor/..." in the game */
                char *p = mf_strdup(c);
                if (p) free(all.v[k].path), all.v[k].path = p;
            }
        }
        qsort(all.v, all.n, sizeof *all.v, mf_cmp_file);
    }
    m->pairs = (int *)calloc((size_t)m->nmods * (size_t)m->nmods + 1, sizeof(int));
    m->files = MF_ALLOC(ModfsFile, all.n);
    size_t nw = 0;
    for (size_t k = 0; k < all.n;) {
        size_t e = k + 1;
        while (e < all.n && !strcmp(all.v[e].path, all.v[k].path)) e++;
        ModfsFile *f = &m->files[nw++];
        int w = all.v[k].mod;
        f->path = all.v[k].path, f->host = all.v[k].host;
        f->size = all.v[k].size, f->vanilla = all.v[k].vanilla;
        f->mod = w, f->nshadow = 0;
        if (f->vanilla == 0) m->mods[w].nnew++;
        int counted_win = 0;
        for (size_t j = k + 1; j < e; j++) {
            int lo = all.v[j].mod;
            free(all.v[j].path), free(all.v[j].host);
            if (lo == w) continue;   /* the same mod twice (two spellings of one path) */
            f->nshadow++;
            m->pairs[w * m->nmods + lo]++;
            m->mods[lo].nlost++;
            if (!counted_win) m->mods[w].nwon++, counted_win = 1;
        }
        k = e;
    }
    m->nfiles = nw;
    free(all.v);
    return m->nmods;
}

void modfs_free(Modfs *m) {
    for (size_t k = 0; k < m->nfiles; k++) free(m->files[k].path), free(m->files[k].host);
    for (int k = 0; k < m->nrestbls; k++) free(m->restbls[k]);
    free(m->files), free(m->mods), free(m->pairs), free(m->restbls);
    memset(m, 0, sizeof *m);
}

const ModfsFile *modfs_find(const Modfs *m, const char *path) {
    size_t lo = 0, hi = m->nfiles;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        int c = strcmp(m->files[mid].path, path);
        if (!c) return &m->files[mid];
        if (c < 0) lo = mid + 1;
        else hi = mid;
    }
    return NULL;
}

int modfs_has_folder(const Modfs *m, const char *folder) {
    size_t n = strlen(folder), lo = 0, hi = m->nfiles;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (strcmp(m->files[mid].path, folder) < 0) lo = mid + 1;
        else hi = mid;
    }
    return lo < m->nfiles && !strncmp(m->files[lo].path, folder, n);
}

int modfs_is_restbl(const char *path) {
    static const char pre[] = "System/Resource/ResourceSizeTable.Product.";
    size_t n = strlen(path);
    if (n < sizeof pre - 1) return 0;
    char head[sizeof pre];
    memcpy(head, path, sizeof pre - 1), head[sizeof pre - 1] = 0;
    return mf_ieq(head, pre) && mf_iends(path, ".rsizetable.zs");
}

char *modfs_restbl_name(const char *path, char *out, size_t cap) {
    snprintf(out, cap, "%s", path);
    size_t n = strlen(out);
    if (mf_iends(out, ".ta.zs")) return out;
    if (n > 3 && mf_iends(out, ".zs")) out[n - 3] = 0;
    else if (n > 5 && mf_iends(out, ".zstd")) out[n - 5] = 0;
    else if (n > 3 && mf_iends(out, ".mc")) out[n - 3] = 0;
    return out;
}
