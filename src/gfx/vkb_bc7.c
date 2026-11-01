// ASTC textures as BC7 (decided 2026-10-24): desktop GPUs and the Steam Deck have no ASTC, so
// ASTC was decoded on the CPU to RGBA8 at every upload (the hitches when new parts of the world stream in, 4x the video
// memory). BC7 is what these GPUs sample natively, at 8 bits per texel - not fewer bits than TOTK's ASTC blocks.
// A cache of BC7 data by the contents of the ASTC blocks: the key of a surface (one level of one layer; every slice of a
// 3D level) is a 128-bit hash of its ASTC blocks in rows (as deswizzled for an upload) with its size and block size
// (tex/bc7key.c) - the same bytes whichever way the game lays the texture out in memory, so the build can fill the cache
// from the RomFS (next step) and the game finds the entries. Until then the cache fills as textures are met: a miss is
// decoded as before and encoded to BC7 in the background for the next time.
// Files (TOTK_BC7_DIR, default data/textures): bc7.pack (the blocks, appended) and bc7.idx (32-byte records: key,
// offset, length, format - 0 BC7, 1 BC1; appended after the blocks are written, so an entry always has its data).
// BC1: the interface's 12x12 ASTC maps when opaque (tools/bc7build: a texture is BC1 only if none of its texels is
// transparent; half the bytes of BC7, and 12x12 ASTC holds less detail than BC1 keeps). Nothing from the game is
// in the repository or a build: the cache is made on the player's machine from the player's copy (README.md, Legal).
// 2026-10-29 (texture hitches): bc7.pack is mapped into memory (TOTK_TEX_MMAP) and read outside the lock; the surfaces
// used are listed in bc7.used (16-byte keys, appended at the first use in a run) and their blocks read into memory in
// the background from the start (TOTK_TEX_PREFETCH): before the title and during loading, not when a surface is drawn.
#include <direct.h>
#include <io.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#include "compat/gfx_compat.h"
#include "host/hle.h"
#include "vkb.h"
#include "tex/bc7.h"
#include "tex/bc7key.h"

#define VLOG(...) LOG("[vk] " __VA_ARGS__)

int vkb_bc7_on(void) {   // TOTK_BC7=0: decode ASTC to RGBA8 as before (A/B)
    static int on = -1;
    if (on < 0) { const char *e = getenv("TOTK_BC7"); on = !(e && *e == '0'); }
    return on;
}

// ---- the key (tex/bc7key.c: the build computes the same)
vkb_bc7_key vkb_bc7_key_of(const uint8_t *blocks, size_t n, uint32_t w, uint32_t h, uint32_t depth, uint32_t bw, uint32_t bh, int srgb) {
    (void)srgb;   // not part of the key: the BC7 bytes serve both views
    bc7_key k = bc7_key_of(blocks, n, w, h, depth, bw, bh);
    return (vkb_bc7_key){k.a, k.b};
}

// ---- the index in memory (open addressing) and the files
typedef struct { uint64_t a, b, off; uint32_t len, fmt; } rec_t;   // also the record in bc7.idx (fmt: VKB_BC_*)
#define USED 0x80000000u   // in memory only (fmt): in the used-textures list (bc7.used)
static rec_t *g_tab;
static size_t g_cap, g_n;
static FILE *g_pack, *g_idx, *g_used;
static const uint8_t *g_map;   // bc7.pack mapped (its size at the opening: later entries are read with fread)
static uint64_t g_map_size, g_nused;
static uint64_t g_read_bytes;
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static int g_open;   // 1 open, -1 cannot be used
static uint64_t g_hits, g_misses, g_encoded, g_dropped, g_encode_ns;

static rec_t *slot(uint64_t a, uint64_t b) {   // g_mu held, g_cap a power of two
    size_t h = (size_t)(a ^ (b * 0x9E3779B97F4A7C15ull)) & (g_cap - 1);
    while ((g_tab[h].a || g_tab[h].b) && !(g_tab[h].a == a && g_tab[h].b == b)) h = (h + 1) & (g_cap - 1);
    return &g_tab[h];
}
int vkb_cpu_count(void) {
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    return si.dwNumberOfProcessors ? (int)si.dwNumberOfProcessors : 4;
}
void vkb_thread_below_normal(void) { SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL); }
static int env_on(const char *name) { const char *e = getenv(name); return !(e && *e == '0'); }
static void put(const rec_t *r) {   // g_mu held
    if ((g_n + 1) * 2 > g_cap) {
        rec_t *old = g_tab;
        size_t n = g_cap;
        g_cap = g_cap ? g_cap * 2 : 1 << 16;
        if (!(g_tab = calloc(g_cap, sizeof *g_tab))) die("BC7 index");
        for (size_t i = 0; i < n; i++) if (old[i].a || old[i].b) *slot(old[i].a, old[i].b) = old[i];
        free(old);
    }
    rec_t *s = slot(r->a, r->b);
    if (!s->a && !s->b) g_n++;
    *s = *r;
}
static const char *dir(void) {
    const char *d = getenv("TOTK_BC7_DIR");
    return d && *d ? d : "data/textures";
}
static int open_files(void) {   // g_mu held
    if (g_open) return g_open > 0;
    g_open = -1;
    char path[512];
    const char *d = dir();
    for (char *p = strcpy(path, d) + 1; *p; p++) if (*p == '/' || *p == '\\') { char c = *p; *p = 0; _mkdir(path); *p = c; }
    _mkdir(path);
    snprintf(path, sizeof path, "%s/bc7.pack", d);
    if (!(g_pack = fopen(path, "r+b")) && !(g_pack = fopen(path, "w+b"))) { VLOG("BC7 cache: cannot open %s", path); return 0; }
    _fseeki64(g_pack, 0, SEEK_END);
    int64_t pack_size = _ftelli64(g_pack);
    snprintf(path, sizeof path, "%s/bc7.idx", d);
    if (!(g_idx = fopen(path, "r+b")) && !(g_idx = fopen(path, "w+b"))) { VLOG("BC7 cache: cannot open %s", path); return 0; }
    rec_t r;
    int64_t good = 0;
    while (fread(&r, sizeof r, 1, g_idx) == 1) {
        if ((int64_t)(r.off + r.len) > pack_size) break;   // the blocks of an interrupted write: dropped
        put(&r);
        good += sizeof r;
    }
    _fseeki64(g_idx, good, SEEK_SET);
    VLOG("BC7 cache %s: %zu surfaces, %.1f MiB", d, g_n, pack_size / 1048576.0);
    if (pack_size > 0 && env_on("TOTK_TEX_MMAP")) {   // the pack in memory: reads are copies, the OS caches the pages
        HANDLE fh = (HANDLE)_get_osfhandle(_fileno(g_pack));
        HANDLE m = fh != INVALID_HANDLE_VALUE ? CreateFileMappingA(fh, NULL, PAGE_READONLY, (DWORD)((uint64_t)pack_size >> 32), (DWORD)pack_size, NULL) : NULL;
        g_map = m ? MapViewOfFile(m, FILE_MAP_READ, 0, 0, (SIZE_T)pack_size) : NULL;
        if (m) CloseHandle(m);   // the view keeps the mapping
        if (g_map) g_map_size = (uint64_t)pack_size;
        VLOG("BC7 cache: %s", g_map ? "bc7.pack mapped into memory (TOTK_TEX_MMAP)" : "bc7.pack could not be mapped: read from the file");
    }
    snprintf(path, sizeof path, "%s/bc7.used", d);   // the used-textures list (keys; for the prefetch)
    if ((g_used = fopen(path, "r+b")) || (g_used = fopen(path, "w+b"))) {
        uint64_t key[2], listed = 0;
        while (fread(key, sizeof key, 1, g_used) == 1) {
            listed++;
            if (!g_cap) continue;
            rec_t *s = slot(key[0], key[1]);
            if ((s->a || s->b) && !(s->fmt & USED)) { s->fmt |= USED; g_nused++; }
        }
        _fseeki64(g_used, (int64_t)(listed * sizeof key), SEEK_SET);
        VLOG("BC7 cache: used-textures list: %llu surfaces (%llu in the cache)", (unsigned long long)listed, (unsigned long long)g_nused);
    }
    g_open = 1;
    return 1;
}
static void mark_used(rec_t *s) {   // g_mu held: the first use (this run or any before)
    if (s->fmt & USED) return;
    s->fmt |= USED;
    g_nused++;
    uint64_t key[2] = {s->a, s->b};
    if (g_used && fwrite(key, sizeof key, 1, g_used) == 1) fflush(g_used);
}

int vkb_bc7_lookup(vkb_bc7_key k, int *fmt, size_t *len) {
    pthread_mutex_lock(&g_mu);
    int ok = open_files() && g_cap;
    if (ok) { rec_t *s = slot(k.a, k.b); ok = (s->a || s->b) && s->len; if (ok) { *fmt = (int)(s->fmt & ~USED); *len = s->len; } }
    pthread_mutex_unlock(&g_mu);
    return ok;
}
int vkb_bc7_has(vkb_bc7_key k, size_t n) {
    int fmt; size_t len;
    return vkb_bc7_lookup(k, &fmt, &len) && fmt == VKB_BC_7 && len == n;
}
int vkb_bc7_read(vkb_bc7_key k, uint8_t *out, size_t n) {
    pthread_mutex_lock(&g_mu);
    int ok = open_files() && g_cap;
    rec_t r = {0};
    if (ok) { rec_t *s = slot(k.a, k.b); r = *s; ok = (r.a || r.b) && r.len == n; if (ok) mark_used(s); }
    int mapped = ok && g_map && r.off + n <= g_map_size;
    if (ok && !mapped) ok = !_fseeki64(g_pack, (int64_t)r.off, SEEK_SET) && fread(out, 1, n, g_pack) == n;
    if (ok) g_hits++; else g_misses++;
    pthread_mutex_unlock(&g_mu);
    if (mapped) memcpy(out, g_map + r.off, n);   // outside the lock: several threads read at once
    if (ok) __atomic_fetch_add(&g_read_bytes, n, __ATOMIC_RELAXED);
    return ok;
}
uint64_t vkb_bc7_read_bytes(void) { return __atomic_load_n(&g_read_bytes, __ATOMIC_RELAXED); }

// ---- the prefetch (TOTK_TEX_PREFETCH: 1 the used-textures list (default), all: the whole pack, 0 off)
// One background thread from the first shader the game loads (vkb_shader.c, before the title): the ranges of the listed
// surfaces, sorted and joined, are asked of the OS (PrefetchVirtualMemory) and touched page by page, so a surface met in
// play is a copy from memory. The first run with this has no list yet: it is made as the game is played.
typedef struct { uint64_t off, len; } range_t;
static int range_cmp(const void *x, const void *y) {
    const range_t *a = x, *b = y;
    return a->off < b->off ? -1 : a->off > b->off;
}
static void *prefetcher(void *arg) {
    int all = (int)(intptr_t)arg;
    pthread_setname_np("totk-texprefetch");
    vkb_thread_below_normal();
    uint64_t t0 = runtime_now_ns();
    pthread_mutex_lock(&g_mu);
    int ok = open_files() && g_cap;
    size_t n = 0;
    range_t *rg = ok ? malloc((g_n ? g_n : 1) * sizeof *rg) : NULL;
    uint64_t limit = g_map ? g_map_size : 0;
    if (rg)
        for (size_t i = 0; i < g_cap; i++) {
            rec_t *s = &g_tab[i];
            if ((s->a || s->b) && s->len && (all || (s->fmt & USED)) && s->off + s->len <= limit) rg[n++] = (range_t){s->off, s->len};
        }
    pthread_mutex_unlock(&g_mu);
    if (!rg || !g_map) {
        VLOG("texture prefetch: off (%s)", !ok ? "no BC7 cache" : "bc7.pack not mapped");
        free(rg);
        return NULL;
    }
    if (!n) {
        VLOG("texture prefetch: no used-textures list yet (data/textures/bc7.used is made as the game is played; the next start reads it)");
        free(rg);
        return NULL;
    }
    qsort(rg, n, sizeof *rg, range_cmp);
    size_t m = 0;   // joined where the gaps are small (one read instead of many)
    uint64_t total = 0;
    for (size_t i = 0; i < n; i++) {
        if (m && rg[i].off <= rg[m - 1].off + rg[m - 1].len + (256u << 10)) {
            uint64_t end = rg[i].off + rg[i].len;
            if (end > rg[m - 1].off + rg[m - 1].len) rg[m - 1].len = end - rg[m - 1].off;
        } else rg[m++] = rg[i];
    }
    for (size_t i = 0; i < m; i++) total += rg[i].len;
    typedef BOOL(WINAPI * pvm_t)(HANDLE, ULONG_PTR, PWIN32_MEMORY_RANGE_ENTRY, ULONG);
    pvm_t pvm = (pvm_t)(void *)GetProcAddress(GetModuleHandleA("kernel32.dll"), "PrefetchVirtualMemory");
    VLOG("texture prefetch: %zu surfaces in %zu ranges, %.1f MiB (%s)", n, m, total / 1048576.0, all ? "the whole cache" : "the used-textures list");
    uint64_t done = 0, last = t0;
    volatile uint8_t sink = 0;
    for (size_t i = 0; i < m; i++) {
        for (uint64_t o = 0; o < rg[i].len; o += 8u << 20) {   // 8 MiB at a time
            uint64_t len = rg[i].len - o < (8u << 20) ? rg[i].len - o : (8u << 20);
            const uint8_t *p = g_map + rg[i].off + o;
            if (pvm) {
                WIN32_MEMORY_RANGE_ENTRY e = {(PVOID)p, (SIZE_T)len};
                pvm(GetCurrentProcess(), 1, &e, 0);
            }
            for (uint64_t b = 0; b < len; b += 4096) sink ^= p[b];   // in memory now
            done += len;
            uint64_t now = runtime_now_ns();
            if (now - last > 2000000000ull) {
                last = now;
                VLOG("texture prefetch: %.0f of %.0f MiB", done / 1048576.0, total / 1048576.0);
            }
        }
    }
    (void)sink;
    double s = (runtime_now_ns() - t0) / 1e9;
    VLOG("texture prefetch: done, %.1f MiB in %.1f s (%.0f MiB/s)", done / 1048576.0, s, s > 0 ? done / 1048576.0 / s : 0.0);
    free(rg);
    return NULL;
}
void vkb_bc7_prefetch_start(void) {
    static int started;
    if (__atomic_exchange_n(&started, 1, __ATOMIC_ACQ_REL)) return;
    const char *e = getenv("TOTK_TEX_PREFETCH");
    if (!vkb_bc7_on() || (e && *e == '0')) { VLOG("texture prefetch: off (TOTK_TEX_PREFETCH=0)"); return; }
    pthread_t t;
    if (pthread_create(&t, NULL, prefetcher, (void *)(intptr_t)(e && !strcmp(e, "all"))) == 0) pthread_detach(t);
}
void vkb_bc7_store(vkb_bc7_key k, const uint8_t *data, size_t n) { vkb_bc_store(k, data, n, VKB_BC_7); }
void vkb_bc_store(vkb_bc7_key k, const uint8_t *data, size_t n, int fmt) {
    pthread_mutex_lock(&g_mu);
    if (open_files() && !(g_cap && (slot(k.a, k.b)->a || slot(k.a, k.b)->b))) {
        _fseeki64(g_pack, 0, SEEK_END);
        rec_t r = {k.a, k.b, (uint64_t)_ftelli64(g_pack), (uint32_t)n, (uint32_t)fmt};
        if (fwrite(data, 1, n, g_pack) == n && !fflush(g_pack)) {
            _fseeki64(g_idx, 0, SEEK_END);
            if (fwrite(&r, sizeof r, 1, g_idx) == 1) { fflush(g_idx); put(&r); }
        }
    }
    pthread_mutex_unlock(&g_mu);
}

// ---- encoding in the background (a miss: next time it is there)
typedef struct job { vkb_bc7_key k; uint8_t *rgba; uint32_t w, h, d; int srgb; struct job *next; } job;
static job *g_jobs, **g_tail = &g_jobs;
static size_t g_queued_bytes;
static pthread_mutex_t g_jmu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_jcv = PTHREAD_COND_INITIALIZER;
enum { QUEUE_MAX = 512u << 20, ENCODERS = 2 };
static int quality(void) {   // TOTK_BC7_QUALITY=0..4 (bc7enc's uber level; background encoding)
    static int q = -1;
    // 0 by default: met in play this is a stopgap (one large surface took ~0.75 s at level 1); the build fills the cache
    if (q < 0) { const char *e = getenv("TOTK_BC7_QUALITY"); q = e && *e ? atoi(e) : 0; }
    return q;
}
static void *encoder(void *unused) {
    (void)unused;
    pthread_setname_np("totk-bc7");
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_LOWEST);   // the game's and the recording threads come first
    for (;;) {
        pthread_mutex_lock(&g_jmu);
        while (!g_jobs) pthread_cond_wait(&g_jcv, &g_jmu);
        job *j = g_jobs;
        g_jobs = j->next;
        if (!g_jobs) g_tail = &g_jobs;
        size_t in = (size_t)j->w * j->h * j->d * 4;
        pthread_mutex_unlock(&g_jmu);
        size_t n = bc7_size(j->w, j->h, j->d);
        if (!vkb_bc7_has(j->k, n)) {
            uint8_t *out = malloc(n);
            if (out) {
                uint64_t t0 = runtime_now_ns();
                bc7_encode(j->rgba, j->w, j->h, j->d, j->srgb, quality(), out);
                vkb_bc7_store(j->k, out, n);
                __atomic_fetch_add(&g_encode_ns, runtime_now_ns() - t0, __ATOMIC_RELAXED);
                __atomic_fetch_add(&g_encoded, 1, __ATOMIC_RELAXED);
                free(out);
            }
        }
        free(j->rgba);
        free(j);
        pthread_mutex_lock(&g_jmu);
        g_queued_bytes -= in;
        pthread_mutex_unlock(&g_jmu);
    }
    return NULL;
}
void vkb_bc7_queue(vkb_bc7_key k, const uint8_t *rgba, uint32_t w, uint32_t h, uint32_t d, int srgb) {
    size_t in = (size_t)w * h * (d ? d : 1) * 4;
    pthread_mutex_lock(&g_jmu);
    if (g_queued_bytes + in > QUEUE_MAX) { g_dropped++; pthread_mutex_unlock(&g_jmu); return; }   // met again later
    g_queued_bytes += in;
    pthread_mutex_unlock(&g_jmu);
    job *j = malloc(sizeof *j);
    uint8_t *copy = malloc(in);
    if (!j || !copy) { free(j); free(copy); pthread_mutex_lock(&g_jmu); g_queued_bytes -= in; pthread_mutex_unlock(&g_jmu); return; }
    memcpy(copy, rgba, in);
    *j = (job){k, copy, w, h, d ? d : 1, srgb, NULL};
    static int started;
    pthread_mutex_lock(&g_jmu);
    if (!started) {
        started = 1;
        for (int i = 0; i < ENCODERS; i++) {
            pthread_t t;
            if (pthread_create(&t, NULL, encoder, NULL) == 0) pthread_detach(t);
        }
    }
    *g_tail = j;
    g_tail = &j->next;
    pthread_cond_signal(&g_jcv);
    pthread_mutex_unlock(&g_jmu);
}
// A miss on an image that is BC7 / BC1 already (its contents changed after it became one): encoded now, quickly.
void vkb_bc7_encode_now(vkb_bc7_key k, const uint8_t *rgba, uint32_t w, uint32_t h, uint32_t d, int srgb, int fmt, uint8_t *out) {
    if (fmt == VKB_BC_1) { bc1_encode(rgba, w, h, d ? d : 1, 4, out); return; }   // (not kept: the cache keys BC1 by the build's choice)
    bc7_encode(rgba, w, h, d ? d : 1, srgb, 0, out);
    vkb_bc7_store(k, out, bc7_size(w, h, d ? d : 1));
}
void vkb_bc7_stats(uint64_t *hits, uint64_t *misses, uint64_t *encoded, uint64_t *dropped, uint64_t *encode_ns, size_t *queued) {
    pthread_mutex_lock(&g_mu);
    *hits = g_hits; *misses = g_misses;
    pthread_mutex_unlock(&g_mu);
    *encoded = __atomic_load_n(&g_encoded, __ATOMIC_RELAXED); *encode_ns = __atomic_load_n(&g_encode_ns, __ATOMIC_RELAXED);
    pthread_mutex_lock(&g_jmu);
    *dropped = g_dropped; *queued = g_queued_bytes;
    pthread_mutex_unlock(&g_jmu);
}
