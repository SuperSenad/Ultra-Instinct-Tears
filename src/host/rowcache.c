/* [UIT 0356] Row lookups by name, answered from a per-thread cache (TOTK_ROWCACHE; tools/lifter/rowcache.py).
 * Idea from WotE (MIT): the game looks the same actor-table rows up by name every frame (binary search through
 * virtual calls); this is an independent implementation for 1.4.3 whose functions setup finds in the player's main.
 *
 * Stage 1 (main_rowcache_stage 1, probe): the lift found getRow-shaped functions it cannot yet tell apart. Their
 * front calls uit_rc_probe, which every 256th call notes the table's vtable (the first word at x0) when x0 and the
 * name are readable and the vtable lies in main's image. 8 samples of one (function, vtable) pair and
 * diag/lift-confirm.txt gets "rowcache <G> vtable <V> build <id16> samples <n>"; the next setup run uses it.
 * Nothing is cached; the probe only reads.
 *
 * Stage 2 (cache): for the confirmed functions, uit_rc_getrow looks the (function, table, name bytes) up in a table
 * of this thread's. An entry is used only if it was made in the current generation; every function of the table's
 * vtable that may write (uit_rc_mutate: setup fronted all of them) moves the generation before and after it runs,
 * and an answer is kept only if no such function ran or was running while it was worked out. So a hit returns what
 * the table's own search returned for the same name since its last change through its own functions. A table whose
 * vtable is not one of the confirmed ones, a name of 64 bytes or more, or x0 / the name pointer 0: the real search.
 * TOTK_ROWCACHE_VERIFY=1 does the real search on every hit too, logs any difference and returns the real answer.
 *
 * The hit path: one atomic load of the generation, a hash of the name, one compare; no lock (each guest thread runs
 * on its own host thread, the tables are _Thread_local). */
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../runtime/cpu.h"
#ifdef _WIN32
#include <windows.h>
#endif

#undef LOG
#include "../runtime/logline.h"   // [UIT 0362] uit_logline: one write per line
#define LOG(...) uit_logline("[totk] rowcache: " __VA_ARGS__)

typedef void (*GuestRoutine)(Cpu *c);
extern const int main_rowcache_stage, main_rowcache_n;
extern const unsigned long long main_rowcache_g[], main_rowcache_v[], main_rowcache_lo, main_rowcache_hi;
extern const unsigned main_rowcache_name_off[];
extern const char main_lift_build[];
void uit_rc_probe(Cpu *c, int k);
void uit_rc_getrow(Cpu *c, int k, GuestRoutine fn);
void uit_rc_mutate(Cpu *c, GuestRoutine fn);

#define RC_V 4            /* vtables per function (rowcache.py MAX_V) */
#define RC_G 64           /* functions at most (rowcache.py MAX_G) */
#define RC_NAME 64        /* names this long or longer are not cached */
#define RC_SLOTS 1024     /* per thread, direct-mapped */
#define RC_STATS (1u << 22)

int uit_rc_on = -1;       /* 0 off, 1 on, 2 verify (the fronts test it before calling in) */
static _Atomic uint64_t g_gen = 2;
static _Atomic int g_inmut;
static _Atomic uint64_t g_calls, g_hits, g_misses, g_bypass, g_stores, g_mut, g_mismatch, g_xdiff;

static int env_on(const char *n) {
    const char *e = getenv(n);
    return e && *e && strcmp(e, "0") && strcmp(e, "-") && strcmp(e, "off") && strcmp(e, "OFF") && strcmp(e, "no");
}

static int resolve(void) {
    int on = uit_rc_on;
    if (on >= 0) return on;
    on = 0;
    if (main_rowcache_stage && main_rowcache_n > 0 && env_on("TOTK_ROWCACHE"))
        on = main_rowcache_stage == 1 ? 1 : env_on("TOTK_ROWCACHE_VERIFY") ? 2 : 1;
    if (main_rowcache_stage == 2)
        LOG("%s (%d look-up function(s) of this build)%s", on ? "on" : "off", main_rowcache_n,
            on == 2 ? ", checking every hit against the real search" : "");
    else if (main_rowcache_stage == 1 && on)
        LOG("probing %d candidate(s): play a minute, close the game, then About > Install update", main_rowcache_n);
    uit_rc_on = on;
    return on;
}

/* ------------------------------------------------------------- probe */
static int readable(uint64_t a, size_t n) {
    if (a < 0x10000) return 0;
#ifdef _WIN32
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery((void *)(uintptr_t)a, &mbi, sizeof mbi) || mbi.State != MEM_COMMIT) return 0;
    if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return 0;
    return a + n <= (uint64_t)(uintptr_t)mbi.BaseAddress + mbi.RegionSize;
#else
    (void)n;
    return 1;
#endif
}

static int name_ok(uint64_t p) {   /* a readable, printable, NUL-terminated name */
    if (!readable(p, 1)) return 0;
    for (int i = 0; i < 256; i++) {
        if (((p + i) & 0xFFF) == 0 && i && !readable(p + i, 1)) return 0;
        uint8_t ch = LD8(p + i);
        if (!ch) return i > 0;
        if (ch < 0x20 || ch > 0x7E) return 0;
    }
    return 0;
}

typedef struct { _Atomic uint64_t calls, v[RC_V]; _Atomic uint32_t n[RC_V], bad; _Atomic int written[RC_V]; } Probe;
static Probe g_probe[RC_G];

void uit_rc_probe(Cpu *c, int k) {
    if (resolve() == 0 || k < 0 || k >= RC_G || k >= main_rowcache_n) return;
    Probe *p = &g_probe[k];
    uint64_t n = atomic_fetch_add_explicit(&p->calls, 1, memory_order_relaxed);
    if (n & 255) return;
    uint64_t t = c->x[0], o = c->x[1] + main_rowcache_name_off[k];
    if (!readable(t, 8) || !readable(o, 8)) { atomic_fetch_add(&p->bad, 1); return; }
    uint64_t v = LD64(t), name = LD64(o);
    if (v < main_rowcache_lo || v >= main_rowcache_hi || (v & 7) || !name_ok(name)) { atomic_fetch_add(&p->bad, 1); return; }
    for (int i = 0; i < RC_V; i++) {
        uint64_t cur = atomic_load(&p->v[i]);
        if (cur == 0) {
            uint64_t z = 0;
            if (!atomic_compare_exchange_strong(&p->v[i], &z, v) && z != v) continue;
        } else if (cur != v) continue;
        uint32_t s = atomic_fetch_add(&p->n[i], 1) + 1;
        if (s >= 8 && atomic_load(&p->bad) == 0 && !atomic_exchange(&p->written[i], 1)) {
            FILE *f = fopen("diag/lift-confirm.txt", "a");
            if (f) {
                fprintf(f, "rowcache %llx vtable %llx build %.16s samples %u\n", main_rowcache_g[k],
                        (unsigned long long)v, main_lift_build, s);
                fclose(f);
            }
            LOG("confirmed look-up function %llx on a table with vtable %llx (%u samples)%s", main_rowcache_g[k],
                (unsigned long long)v, s, f ? "; About > Install update uses it" : "; could not write diag/lift-confirm.txt");
        }
        return;
    }
}

/* ------------------------------------------------------------- cache */
typedef struct {
    uint64_t gen, table, x0, x1;
    uint32_t hash;
    uint16_t k, len;
    char name[RC_NAME];
} Entry;
typedef struct { Entry e[RC_SLOTS]; uint32_t calls, hits, misses, bypass, stores; } Tls;
static _Thread_local Tls *t_rc;

static void flush_stats(Tls *t) {
    atomic_fetch_add_explicit(&g_hits, t->hits, memory_order_relaxed);
    atomic_fetch_add_explicit(&g_misses, t->misses, memory_order_relaxed);
    atomic_fetch_add_explicit(&g_bypass, t->bypass, memory_order_relaxed);
    atomic_fetch_add_explicit(&g_stores, t->stores, memory_order_relaxed);
    uint64_t prev = atomic_fetch_add_explicit(&g_calls, t->calls, memory_order_relaxed), all = prev + t->calls;
    t->calls = t->hits = t->misses = t->bypass = t->stores = 0;
    if (prev / RC_STATS != all / RC_STATS)
        LOG("%llu calls: %llu hits, %llu misses, %llu not cacheable, %llu stored, %llu table changes, %llu mismatches",
            (unsigned long long)all, (unsigned long long)atomic_load(&g_hits), (unsigned long long)atomic_load(&g_misses),
            (unsigned long long)atomic_load(&g_bypass), (unsigned long long)atomic_load(&g_stores),
            (unsigned long long)atomic_load(&g_mut), (unsigned long long)atomic_load(&g_mismatch));
}

static int vt_ok(int k, uint64_t vt) {
    const unsigned long long *v = &main_rowcache_v[k * RC_V];
    for (int i = 0; i < RC_V; i++)
        if (v[i] && v[i] == vt) return 1;
    return 0;
}

void uit_rc_getrow(Cpu *c, int k, GuestRoutine fn) {
    int on = resolve();
    if (on == 0 || main_rowcache_stage != 2 || k < 0 || k >= main_rowcache_n) { fn(c); return; }
    Tls *t = t_rc;
    if (!t) {
        t = t_rc = calloc(1, sizeof *t);
        if (!t) { fn(c); return; }
    }
    if (++t->calls >= 65536) flush_stats(t);
    uint64_t table = c->x[0], np = c->x[1] ? LD64(c->x[1] + main_rowcache_name_off[k]) : 0;
    if (!table || !np || !c->x[1] || !vt_ok(k, LD64(table))) { t->bypass++; fn(c); return; }
    char name[RC_NAME];
    uint32_t h = 2166136261u;
    unsigned len = 0;
    for (;; len++) {
        if (len == RC_NAME) { t->bypass++; fn(c); return; }
        char ch = (char)LD8(np + len);
        if (!ch) break;
        name[len] = ch;
        h = (h ^ (uint8_t)ch) * 16777619u;
    }
    h = (h ^ (uint32_t)k) * 16777619u;
    h = (h ^ (uint32_t)(table >> 3)) * 16777619u;
    h = (h ^ (uint32_t)(table >> 35)) * 16777619u;
    Entry *e = &t->e[(h ^ (h >> 16)) & (RC_SLOTS - 1)];
    uint64_t gen = atomic_load_explicit(&g_gen, memory_order_acquire);
    int busy = atomic_load_explicit(&g_inmut, memory_order_acquire);
    if (!busy && e->gen == gen && e->hash == h && e->table == table && e->k == k && e->len == len &&
        !memcmp(e->name, name, len)) {
        if (on == 2) {   /* verify: the real search too */
            uint64_t x0 = e->x0, x1 = e->x1;
            fn(c);
            if (c->x[1] != x1) atomic_fetch_add_explicit(&g_xdiff, 1, memory_order_relaxed);
            if (c->x[0] != x0 && atomic_load(&g_gen) == gen && !atomic_load(&g_inmut)) {
                uint64_t m = atomic_fetch_add(&g_mismatch, 1);
                if (m < 20)
                    LOG("MISMATCH: function %llx table %llx name \"%.*s\": cached %llx, real %llx", main_rowcache_g[k],
                        (unsigned long long)table, (int)len, name, (unsigned long long)x0, (unsigned long long)c->x[0]);
                e->gen = 0;
            }
            t->hits++;
            return;
        }
        c->x[0] = e->x0;
        c->x[1] = e->x1;
        t->hits++;
        return;
    }
    t->misses++;
    fn(c);
    atomic_thread_fence(memory_order_seq_cst);
    if (!busy && atomic_load_explicit(&g_gen, memory_order_acquire) == gen &&
        !atomic_load_explicit(&g_inmut, memory_order_acquire)) {
        e->gen = gen; e->hash = h; e->table = table; e->k = (uint16_t)k; e->len = (uint16_t)len;
        memcpy(e->name, name, len);
        e->x0 = c->x[0]; e->x1 = c->x[1];
        t->stores++;
    }
}

void uit_rc_mutate(Cpu *c, GuestRoutine fn) {
    if (uit_rc_on == 0) { fn(c); return; }
    atomic_fetch_add(&g_inmut, 1);
    atomic_fetch_add(&g_gen, 1);
    atomic_thread_fence(memory_order_seq_cst);
    fn(c);
    atomic_thread_fence(memory_order_seq_cst);
    atomic_fetch_add(&g_gen, 1);
    atomic_fetch_sub(&g_inmut, 1);
    atomic_fetch_add_explicit(&g_mut, 1, memory_order_relaxed);
}
