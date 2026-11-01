/* [UIT 0042] Resource size table merge for mods: see restbl.h. */
#include "restbl.h"

#include <stdlib.h>
#include <string.h>
#include <zstd.h>

static uint32_t crc_table[256];
static void crc_init(void) {
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        crc_table[i] = c;
    }
}
uint32_t restbl_crc32(const char *s) {
    if (!crc_table[1]) crc_init();
    uint32_t c = 0xFFFFFFFFu;
    for (; *s; s++) c = crc_table[(c ^ (uint8_t)*s) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}
static int ends(const char *s, const char *suf) {
    size_t a = strlen(s), b = strlen(suf);
    if (a < b) return 0;
    for (size_t i = 0; i < b; i++) {
        char x = s[a - b + i], y = suf[i];
        if (x >= 'A' && x <= 'Z') x += 32;
        if (x != y) return 0;
    }
    return 1;
}
static uint64_t align32(uint64_t v) { return (v + 31) & ~(uint64_t)31; }

uint64_t restbl_decoded_size(const char *path, const uint8_t *head, size_t n, uint64_t file_size) {
    if (n >= 4 && head[0] == 0x28 && head[1] == 0xB5 && head[2] == 0x2F && head[3] == 0xFD) {
        unsigned long long c = ZSTD_getFrameContentSize(head, n);
        if (c == ZSTD_CONTENTSIZE_ERROR || c == ZSTD_CONTENTSIZE_UNKNOWN) return 0;
        return (uint64_t)c;
    }
    if (ends(path, ".mc")) {
        /* mesh codec: "MCPK" and a u32 of flags at +8, the decoded size (flags >> 5) << (flags & 15). Anything else
           (the header not as expected): 4 times the file, a generous guess for a compressed model */
        if (n >= 12 && !memcmp(head, "MCPK", 4)) {
            uint32_t flags = (uint32_t)head[8] | (uint32_t)head[9] << 8 | (uint32_t)head[10] << 16 | (uint32_t)head[11] << 24;
            uint64_t d = (uint64_t)(flags >> 5) << (flags & 15);
            if (d && d < (1ull << 34)) return d;
        }
        return file_size * 4;
    }
    return file_size;
}
uint64_t restbl_estimate(const char *name, uint64_t decoded) {
    if (ends(name, ".bgyml")) return (decoded + 2048) * 8;   /* parsed into objects: several times its bytes */
    if (ends(name, ".ainb") || ends(name, ".asb") || ends(name, ".bstar") || ends(name, ".baev"))
        return align32(decoded * 5 / 4) + 0x1000;
    if (ends(name, ".bfres")) return align32(decoded) * 2;  /* from .mc: decoded once, then relocated */
    return align32(decoded) + 0x1000;
}

/* ---- the table */
typedef struct { uint32_t crc; uint32_t size; } Crc;
typedef struct { char *name; uint32_t size; } Named;
typedef struct {
    uint32_t version, namelen;
    Crc *crc; size_t ncrc, ccrc;
    Named *named; size_t nnamed, cnamed;
    int named_sorted;
} Table;
static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static void wr32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v, p[1] = (uint8_t)(v >> 8), p[2] = (uint8_t)(v >> 16), p[3] = (uint8_t)(v >> 24); }
static void table_free(Table *t) {
    for (size_t i = 0; i < t->nnamed; i++) free(t->named[i].name);
    free(t->crc), free(t->named);
    memset(t, 0, sizeof *t);
}
static uint8_t *unzstd(const uint8_t *src, size_t n, size_t *out_n) {
    unsigned long long c = ZSTD_getFrameContentSize(src, n);
    if (c != ZSTD_CONTENTSIZE_ERROR && c != ZSTD_CONTENTSIZE_UNKNOWN) {
        if (c > (64u << 20)) return NULL;
        uint8_t *d = (uint8_t *)malloc((size_t)c + 1);
        if (!d) return NULL;
        size_t r = ZSTD_decompress(d, (size_t)c, src, n);
        if (ZSTD_isError(r) || r != c) { free(d); return NULL; }
        *out_n = r;
        return d;
    }
    if (c == ZSTD_CONTENTSIZE_ERROR) return NULL;
    /* no content size in the frame: stream it */
    ZSTD_DStream *z = ZSTD_createDStream();
    size_t cap = 4u << 20, len = 0;
    uint8_t *d = (uint8_t *)malloc(cap);
    ZSTD_inBuffer in = {src, n, 0};
    int ok = z && d;
    while (ok) {
        if (len == cap) {
            if (cap >= (64u << 20)) { ok = 0; break; }
            uint8_t *g = (uint8_t *)realloc(d, cap *= 2);
            if (!g) { ok = 0; break; }
            d = g;
        }
        ZSTD_outBuffer o = {d + len, cap - len, 0};
        size_t r = ZSTD_decompressStream(z, &o, &in);
        len += o.pos;
        if (ZSTD_isError(r)) ok = 0;
        else if (!r) break;
        else if (in.pos == in.size && o.pos < o.size) ok = 0;   /* truncated */
    }
    ZSTD_freeDStream(z);
    if (!ok) { free(d); return NULL; }
    *out_n = len;
    return d;
}
static int table_parse(Table *t, const uint8_t *zs, size_t n) {
    memset(t, 0, sizeof *t);
    size_t len = 0;
    uint8_t *d = unzstd(zs, n, &len);
    if (!d) return -1;
    int ok = len >= 0x16 && !memcmp(d, "RESTBL", 6);
    if (ok) {
        t->version = rd32(d + 6), t->namelen = rd32(d + 10);
        uint64_t nc = rd32(d + 14), nn = rd32(d + 18);
        ok = t->namelen >= 8 && t->namelen <= 1024 && 0x16 + nc * 8 + nn * (t->namelen + 4) == len;
        if (ok) {
            t->crc = (Crc *)malloc((size_t)(nc + 1) * sizeof(Crc));
            t->named = (Named *)calloc((size_t)(nn + 1), sizeof(Named));
            ok = t->crc && t->named;
            t->ccrc = (size_t)nc + 1, t->cnamed = (size_t)nn + 1;
        }
        for (uint64_t i = 0; ok && i < nc; i++) t->crc[i].crc = rd32(d + 0x16 + i * 8), t->crc[i].size = rd32(d + 0x1A + i * 8);
        if (ok) t->ncrc = (size_t)nc;
        const uint8_t *p = d + 0x16 + nc * 8;
        t->named_sorted = 1;
        for (uint64_t i = 0; ok && i < nn; i++, p += t->namelen + 4) {
            char *s = (char *)malloc(t->namelen + 1);
            if (!s) { ok = 0; break; }
            memcpy(s, p, t->namelen), s[t->namelen] = 0;
            t->named[i].name = s, t->named[i].size = rd32(p + t->namelen);
            t->nnamed = (size_t)i + 1;
            if (i && strcmp(t->named[i - 1].name, s) > 0) t->named_sorted = 0;
        }
        for (size_t i = 1; ok && i < t->ncrc; i++) if (t->crc[i - 1].crc > t->crc[i].crc) ok = 0;   /* must be sorted */
    }
    free(d);
    if (!ok) table_free(t);
    return ok ? 0 : -1;
}
static uint32_t *find_named(Table *t, const char *name) {
    for (size_t i = 0; i < t->nnamed; i++) if (!strcmp(t->named[i].name, name)) return &t->named[i].size;
    return NULL;
}
static uint32_t *find_crc(Table *t, uint32_t crc) {
    size_t lo = 0, hi = t->ncrc;
    while (lo < hi) {
        size_t m = (lo + hi) / 2;
        if (t->crc[m].crc == crc) return &t->crc[m].size;
        if (t->crc[m].crc < crc) lo = m + 1;
        else hi = m;
    }
    return NULL;
}
/* new crc entries are collected apart and merged in at the end (the table stays sorted for lookups) */
typedef struct { Crc *v; size_t n, cap; } Adds;
static uint32_t *find_add(Adds *a, uint32_t crc) {
    for (size_t i = 0; i < a->n; i++) if (a->v[i].crc == crc) return &a->v[i].size;
    return NULL;
}
static int add_crc(Adds *a, uint32_t crc, uint32_t size) {
    if (a->n == a->cap) {
        Crc *g = (Crc *)realloc(a->v, (a->cap = a->cap ? a->cap * 2 : 256) * sizeof(Crc));
        if (!g) return -1;
        a->v = g;
    }
    a->v[a->n].crc = crc, a->v[a->n].size = size, a->n++;
    return 0;
}
static uint32_t clamp32(uint64_t v) { return v > 0xFFFFFFF0u ? 0xFFFFFFF0u : (uint32_t)v; }
/* raise (or add) the entry of a name: returns 1 when it went up, 2 when added, 0 unchanged */
static int raise_name(Table *t, Adds *a, const char *name, uint64_t want, int may_add) {
    uint32_t v = clamp32(want), *e = find_named(t, name);
    if (!e) e = find_crc(t, restbl_crc32(name));
    if (!e) e = find_add(a, restbl_crc32(name));
    if (e) {
        if (*e >= v) return 0;
        *e = v;
        return 1;
    }
    if (!may_add) return 0;
    return add_crc(a, restbl_crc32(name), v) ? 0 : 2;
}
static int cmp_crc(const void *x, const void *y) {
    uint32_t a = ((const Crc *)x)->crc, b = ((const Crc *)y)->crc;
    return a < b ? -1 : a > b;
}
static int cmp_named(const void *x, const void *y) { return strcmp(((const Named *)x)->name, ((const Named *)y)->name); }

int restbl_merge(const uint8_t *game_zs, size_t game_n, const uint8_t *const *tables, const size_t *table_n, int ntables,
                 const RestblFile *files, size_t nfiles, uint8_t **out, size_t *out_n, RestblStats *st) {
    RestblStats s0;
    if (!st) st = &s0;
    memset(st, 0, sizeof *st);
    *out = NULL, *out_n = 0;
    Table t;
    if (table_parse(&t, game_zs, game_n)) return -1;
    Adds adds = {NULL, 0, 0};
    int changed = 0;

    /* 1. the mods' own tables: every entry, the larger size */
    for (int k = 0; k < ntables; k++) {
        Table m;
        if (table_parse(&m, tables[k], table_n[k]) || m.namelen != t.namelen) { st->tables_bad++; table_free(&m); continue; }
        st->tables++;
        for (size_t i = 0; i < m.ncrc; i++) {
            uint32_t *e = find_crc(&t, m.crc[i].crc);
            if (!e) e = find_add(&adds, m.crc[i].crc);
            if (e) { if (*e < m.crc[i].size) *e = m.crc[i].size, st->from_tables++; }
            else if (!add_crc(&adds, m.crc[i].crc, m.crc[i].size)) st->from_tables++;
        }
        for (size_t i = 0; i < m.nnamed; i++) {
            uint32_t *e = find_named(&t, m.named[i].name);
            if (e) { if (*e < m.named[i].size) *e = m.named[i].size, st->from_tables++; continue; }
            if (t.nnamed == t.cnamed) {
                Named *g = (Named *)realloc(t.named, (t.cnamed *= 2) * sizeof(Named));
                if (!g) continue;
                t.named = g;
            }
            t.named[t.nnamed].name = m.named[i].name, t.named[t.nnamed].size = m.named[i].size, t.nnamed++;
            m.named[i].name = NULL;
            st->from_tables++;
        }
        table_free(&m);
    }
    changed = st->from_tables > 0;

    /* 2. each mod file: the game's entry plus the growth, or an estimate for a file the game does not have */
    for (size_t i = 0; i < nfiles; i++) {
        const RestblFile *f = &files[i];
        if (f->game_decoded) {
            uint32_t *e = find_named(&t, f->name);
            if (!e) e = find_crc(&t, restbl_crc32(f->name));
            if (!e) { st->no_entry++; continue; }
            uint64_t a = restbl_estimate(f->name, f->mod_decoded), b = restbl_estimate(f->name, f->game_decoded);
            if (a <= b) continue;
            /* the game's own value (from this run's merge, possibly raised by a mod table) plus the growth */
            uint64_t want = (uint64_t)*e + align32(a - b);
            int r = raise_name(&t, &adds, f->name, want, 0);
            st->raised += r == 1;
        } else {
            int r = raise_name(&t, &adds, f->name, restbl_estimate(f->name, f->mod_decoded), 1);
            st->raised += r == 1, st->added += r == 2;
        }
    }
    changed |= st->raised || st->added;
    if (!changed) { table_free(&t); free(adds.v); return 1; }

    /* 3. write it: crc entries sorted, names in order */
    size_t nc = t.ncrc + adds.n;
    Crc *all = (Crc *)malloc((nc + 1) * sizeof(Crc));
    if (!all) { table_free(&t); free(adds.v); return -2; }
    memcpy(all, t.crc, t.ncrc * sizeof(Crc));
    memcpy(all + t.ncrc, adds.v, adds.n * sizeof(Crc));
    free(adds.v);
    if (adds.n) qsort(all, nc, sizeof(Crc), cmp_crc);
    if (t.named_sorted) qsort(t.named, t.nnamed, sizeof(Named), cmp_named);
    size_t raw_n = 0x16 + nc * 8 + t.nnamed * (t.namelen + 4);
    uint8_t *raw = (uint8_t *)calloc(raw_n, 1);
    if (!raw) { free(all); table_free(&t); return -2; }
    memcpy(raw, "RESTBL", 6);
    wr32(raw + 6, t.version), wr32(raw + 10, t.namelen), wr32(raw + 14, (uint32_t)nc), wr32(raw + 18, (uint32_t)t.nnamed);
    uint8_t *p = raw + 0x16;
    for (size_t i = 0; i < nc; i++, p += 8) wr32(p, all[i].crc), wr32(p + 4, all[i].size);
    for (size_t i = 0; i < t.nnamed; i++, p += t.namelen + 4) {
        size_t l = strlen(t.named[i].name);
        memcpy(p, t.named[i].name, l < t.namelen ? l : t.namelen - 1);
        wr32(p + t.namelen, t.named[i].size);
    }
    st->entries = (uint32_t)(nc + t.nnamed);
    free(all);
    table_free(&t);
    size_t cap = ZSTD_compressBound(raw_n);
    uint8_t *z = (uint8_t *)malloc(cap);
    size_t zn = z ? ZSTD_compress(z, cap, raw, raw_n, 3) : 0;
    free(raw);
    if (!z || ZSTD_isError(zn)) { free(z); return -2; }
    *out = z, *out_n = zn;
    return 0;
}
