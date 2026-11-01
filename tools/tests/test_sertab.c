// [UIT 0233] vkb_sertab.h: the images table with a lookup that takes no lock (TOTK_VK_IMAGE_LOOKUP_LOCKFREE).
//   cc -O2 -Wall -Isrc/gfx tools/tests/test_sertab.c -lpthread -o /tmp/t/sertab && /tmp/t/sertab
//   (also with -fsanitize=address and -fsanitize=thread)
// 1. Randomized differential test: the table before 0233 (copied from vkb_tex.c: slot / grow / insert / release) and
//    vst_* run the same random operations; after each, every slot, the counts and the capacity are the same, and the
//    lock-free lookup gives exactly the locked lookup's answer (one thread).
// 2. Contention replay: a writer (holding the lock) inserts, removes and grows while reader threads look up without
//    the lock; every answer must be an entry that was live for that serial at some instant during the call
//    (per-serial version counters), and NULL answers are checked against the locked lookup. Old tables are freed
//    only between rounds (as vkb_mt_submit does), under ASan a use after free would show.
// 3. The recording pattern: threads look up a working set (all present, inserts of new serials meanwhile), with the
//    lock as before and without: same answers; the time both ways (a microbenchmark of this machine, not the game).
// 4. The interleavings that matter, run step by step (a hook inside the lock-free read runs writer steps between
//    its loading the table and probing it): grown then removed, removed then put back, put while probing.
static void (*g_hook)(void *);
static void *g_hook_arg;
#define VST_READ_HOOK(t) do { if (g_hook) { void (*h_)(void *) = g_hook; g_hook = NULL; h_(g_hook_arg); } } while (0)
#include "vkb_sertab.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

static int fails;
#define CHECK(c, ...) do { if (!(c)) { if (fails++ < 20) { printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } } while (0)

// ---- the table before 0233 (vkb_tex.c, verbatim but for the entry type and die)
typedef struct { uint64_t serial; void *e; } tslot;
static tslot *g_tab;
static uint32_t g_cap, g_n, g_tomb;
static tslot *slot(uint64_t serial) {
    uint32_t h = (uint32_t)(serial * 0x9E3779B97F4A7C15ull >> 40) & (g_cap - 1);
    while (g_tab[h].serial && g_tab[h].serial != serial) h = (h + 1) & (g_cap - 1);
    return &g_tab[h];
}
static void grow(void) {
    tslot *old = g_tab;
    uint32_t n = g_cap;
    g_cap = g_cap ? g_cap * 2 : 1024;
    g_tab = calloc(g_cap, sizeof *g_tab);
    if (!g_tab) abort();
    for (uint32_t i = 0; i < n; i++) if (old[i].serial && old[i].e) *slot(old[i].serial) = old[i];
    g_tomb = 0;
    free(old);
}
static void *ref_lookup_or_put(uint64_t serial, void *e_new, int *made) {   // vkb_image_of
    if ((g_n + g_tomb + 1) * 2 >= g_cap) grow();
    tslot *s = slot(serial);
    if (s->serial == serial && s->e) { *made = 0; return s->e; }
    if (s->serial == serial) g_tomb--; else s->serial = serial;
    s->e = e_new; g_n++; *made = 1;
    return e_new;
}
static void *ref_release(uint64_t serial) {   // vkb_tex_release
    tslot *s = g_cap ? slot(serial) : NULL;
    if (s && s->serial == serial && s->e) { void *e = s->e; s->e = NULL; g_n--; g_tomb++; return e; }
    return NULL;
}
static void *ref_find(uint64_t serial) {
    if (!g_cap) return NULL;
    tslot *s = slot(serial);
    return s->serial == serial ? s->e : NULL;
}
// ---- the same with vst
static void *vst_lookup_or_put(vst *t, uint64_t serial, void *e_new, int *made) {
    if (!vst_reserve(t)) abort();
    vst_slot *s = vst_slot_of(t, serial);
    if (s->serial == serial && s->e) { *made = 0; return s->e; }
    vst_put(t, s, serial, e_new); *made = 1;
    return e_new;
}
static void *vst_release(vst *t, uint64_t serial) {
    vst_slot *s = vst_cap(t) ? vst_slot_of(t, serial) : NULL;
    if (s && s->serial == serial && s->e) { void *e = s->e; vst_remove(t, s); return e; }
    return NULL;
}
static void *vst_find(vst *t, uint64_t serial) {
    if (!vst_cap(t)) return NULL;
    vst_slot *s = vst_slot_of(t, serial);
    return s->serial == serial ? s->e : NULL;
}

static uint64_t rng_s = 88172645463325252ull;
static uint64_t rnd(void) { rng_s ^= rng_s << 13; rng_s ^= rng_s >> 7; rng_s ^= rng_s << 17; return rng_s; }
static double now_s(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return ts.tv_sec + ts.tv_nsec / 1e9; }

static void differential(void) {
    long ops = 0, cmp_slots = 0;
    for (int run = 0; run < 40; run++) {
        vst t = {0};
        t.keep_old = run & 1;
        g_tab = NULL; g_cap = g_n = g_tomb = 0;
        uint64_t space = run < 10 ? 64 : run < 20 ? 3000 : run < 30 ? 40000 : 200000;   // serials reused (removed slots for the same serial) or not
        int nops = run < 30 ? 60000 : 400000;
        char *ent = malloc(16);   // entries: distinct pointers
        uintptr_t next = (uintptr_t)ent;
        for (int i = 0; i < nops; i++, ops++) {
            uint64_t serial = 1 + rnd() % space;
            unsigned r = (unsigned)(rnd() % 100);
            if (r < 55) {
                void *e = (void *)(next += 8);
                int m1, m2;
                void *a = ref_lookup_or_put(serial, e, &m1), *b = vst_lookup_or_put(&t, serial, e, &m2);
                CHECK(a == b && m1 == m2, "lookup %llu: %p/%p made %d/%d", (unsigned long long)serial, a, b, m1, m2);
            } else if (r < 85) {
                void *a = ref_release(serial), *b = vst_release(&t, serial);
                CHECK(a == b, "release %llu: %p/%p", (unsigned long long)serial, a, b);
            } else {
                void *a = ref_find(serial), *b = vst_find(&t, serial), *c = vst_get_lockfree(&t, serial);
                CHECK(a == b && a == c, "find %llu: %p/%p/%p", (unsigned long long)serial, a, b, c);
            }
            if (rnd() % 1000 == 0) vst_quiesce(&t);
            CHECK(g_n == t.n && g_tomb == t.tomb && g_cap == vst_cap(&t), "counts %u/%u %u/%u %u/%u", g_n, t.n, g_tomb, t.tomb, g_cap, vst_cap(&t));
            if ((i & 1023) == 0 || i == nops - 1) {
                for (uint32_t k = 0; k < g_cap; k++, cmp_slots++)
                    CHECK(g_tab[k].serial == t.tab->s[k].serial && g_tab[k].e == t.tab->s[k].e, "slot %u differs", k);
                for (int q = 0; q < 200; q++) {   // lock-free lookups agree with the locked table (absent ones too)
                    uint64_t s = 1 + rnd() % (space + 10);
                    CHECK(vst_get_lockfree(&t, s) == ref_find(s), "lockfree %llu", (unsigned long long)s);
                }
            }
        }
        vst_quiesce(&t); free(t.tab); free(g_tab); free(ent);
    }
    printf("differential: %ld random operations in 40 runs, %ld slot comparisons: %s\n", ops, cmp_slots, fails ? "FAILED" : "identical");
}

// ---- contention replay
enum { NS = 4096, READERS = 6 };
typedef struct { uint64_t serial; uint64_t gen; } inc;   // an entry: one incarnation of a serial
static vst g_t;
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_rwlock_t g_round = PTHREAD_RWLOCK_INITIALIZER;   // readers hold it shared; old tables freed holding it exclusively
static _Atomic uint64_t g_ver[NS + 1];   // per serial: 2g-1 while incarnation g is live, 2g after its removal
static _Atomic uint64_t g_gen[NS + 1];
static _Atomic int g_stop;
static _Atomic long g_hits, g_nulls, g_null_present, g_checked;
static void *writer(void *arg) {
    (void)arg;
    uint64_t seed = 12345;
    long ops = 0;
    while (!atomic_load(&g_stop)) {
        seed ^= seed << 13; seed ^= seed >> 7; seed ^= seed << 17;
        uint64_t s = 1 + seed % NS;
        pthread_mutex_lock(&g_mu);
        if (seed >> 60 < 9) {   // insert when absent
            if (!vst_reserve(&g_t)) abort();
            vst_slot *sl = vst_slot_of(&g_t, s);
            if (!(sl->serial == s && sl->e)) {
                inc *e = malloc(sizeof *e);   // never freed during the test (the game frees entries later too, after the GPU)
                e->serial = s; e->gen = atomic_load(&g_gen[s]) + 1; atomic_store(&g_gen[s], e->gen);
                atomic_store(&g_ver[s], 2 * e->gen - 1);   // live from before it can be seen
                vst_put(&g_t, sl, s, e);
            }
        } else {
            vst_slot *sl = vst_cap(&g_t) ? vst_slot_of(&g_t, s) : NULL;
            if (sl && sl->serial == s && sl->e) {
                inc *e = sl->e;
                vst_remove(&g_t, sl);
                atomic_store(&g_ver[s], 2 * e->gen);   // dead after it can no longer be seen
            }
        }
        pthread_mutex_unlock(&g_mu);
        if (++ops % 5000 == 0) {   // "between submissions": no reader in the table, old tables go
            pthread_rwlock_wrlock(&g_round);
            pthread_mutex_lock(&g_mu); vst_quiesce(&g_t); pthread_mutex_unlock(&g_mu);
            pthread_rwlock_unlock(&g_round);
        }
    }
    return NULL;
}
static void *reader(void *arg) {
    uint64_t seed = 777 + (uintptr_t)arg * 1000003;
    while (!atomic_load(&g_stop)) {
        pthread_rwlock_rdlock(&g_round);
        for (int i = 0; i < 2000; i++) {
            seed ^= seed << 13; seed ^= seed >> 7; seed ^= seed << 17;
            uint64_t s = 1 + seed % NS;
            uint64_t v0 = atomic_load(&g_ver[s]);
            inc *e = vst_get_lockfree(&g_t, s);
            uint64_t v1 = atomic_load(&g_ver[s]);
            if (e) {
                atomic_fetch_add(&g_hits, 1);
                CHECK(e->serial == s, "serial %llu gave an entry of %llu", (unsigned long long)s, (unsigned long long)e->serial);
                CHECK(v0 <= 2 * e->gen - 1 && 2 * e->gen - 1 <= v1, "serial %llu: incarnation %llu not live during the call (versions %llu..%llu)",
                      (unsigned long long)s, (unsigned long long)e->gen, (unsigned long long)v0, (unsigned long long)v1);
            } else {
                atomic_fetch_add(&g_nulls, 1);
                pthread_mutex_lock(&g_mu);   // the caller's fallback: the locked lookup decides
                void *l = vst_find(&g_t, s);
                pthread_mutex_unlock(&g_mu);
                if (l) atomic_fetch_add(&g_null_present, 1);
            }
            atomic_fetch_add(&g_checked, 1);
        }
        pthread_rwlock_unlock(&g_round);
    }
    return NULL;
}
static void contention(void) {
    g_t.keep_old = 1;
    pthread_t w, r[READERS];
    pthread_create(&w, NULL, writer, NULL);
    for (int i = 0; i < READERS; i++) pthread_create(&r[i], NULL, reader, (void *)(uintptr_t)i);
    double t0 = now_s();
    while (now_s() - t0 < 3.0) { struct timespec ts = {0, 50000000}; nanosleep(&ts, NULL); }
    atomic_store(&g_stop, 1);
    pthread_join(w, NULL);
    for (int i = 0; i < READERS; i++) pthread_join(r[i], NULL);
    printf("contention replay: %ld lock-free lookups by %d threads while one thread inserted / removed / grew: %ld answered (each live during "
           "its call), %ld fell back to the lock (%ld of them present there)%s\n", (long)g_checked, READERS, (long)g_hits, (long)g_nulls,
           (long)g_null_present, fails ? " FAILED" : "");
    vst_quiesce(&g_t);
}

// ---- the recording pattern: lookups of images that exist, a few new ones meanwhile
static vst g_rt;
static pthread_mutex_t g_rmu = PTHREAD_MUTEX_INITIALIZER;
static int g_mode;   // 0: with the lock (before), 1: lock-free first
static _Atomic long g_rec_ops, g_rec_bad, g_rec_lf;
static void *recorder(void *arg) {
    uint64_t seed = 99 + (uintptr_t)arg * 7919;
    long ops = 0, bad = 0, lf = 0;
    for (int i = 0; i < 400000; i++) {
        seed ^= seed << 13; seed ^= seed >> 7; seed ^= seed << 17;
        uint64_t s = 1 + seed % 20000;   // the working set: present from the start
        void *e = NULL;
        if (g_mode && (e = vst_get_lockfree(&g_rt, s))) lf++;
        else {
            pthread_mutex_lock(&g_rmu);
            int m;
            e = vst_lookup_or_put(&g_rt, s, (void *)(uintptr_t)(s * 16), &m);
            if ((seed & 255) == 0) { int m2; vst_lookup_or_put(&g_rt, 1000000 + (seed >> 8) % 1000000, (void *)(uintptr_t)16, &m2); }   // a new image
            pthread_mutex_unlock(&g_rmu);
        }
        if (e != (void *)(uintptr_t)(s * 16)) bad++;
        ops++;
    }
    atomic_fetch_add(&g_rec_ops, ops); atomic_fetch_add(&g_rec_bad, bad); atomic_fetch_add(&g_rec_lf, lf);
    return NULL;
}
static void recording(void) {
    double tm[2];
    for (g_mode = 0; g_mode < 2; g_mode++) {
        memset(&g_rt, 0, sizeof g_rt);
        g_rt.keep_old = g_mode;
        for (uint64_t s = 1; s <= 20000; s++) { int m; vst_lookup_or_put(&g_rt, s, (void *)(uintptr_t)(s * 16), &m); }
        g_rec_ops = g_rec_bad = g_rec_lf = 0;
        pthread_t r[READERS];
        double t0 = now_s();
        for (int i = 0; i < READERS; i++) pthread_create(&r[i], NULL, recorder, (void *)(uintptr_t)i);
        for (int i = 0; i < READERS; i++) pthread_join(r[i], NULL);
        tm[g_mode] = now_s() - t0;
        CHECK(g_rec_bad == 0, "recording pattern mode %d: %ld wrong answers", g_mode, (long)g_rec_bad);
        printf("recording pattern, %s: %ld lookups by %d threads in %.1f ms (%ld lock-free), wrong answers %ld\n", g_mode ? "lock-free first" : "with the lock (before)",
               (long)g_rec_ops, READERS, tm[g_mode] * 1e3, (long)g_rec_lf, (long)g_rec_bad);
        vst_quiesce(&g_rt); free(g_rt.tab);
    }
    printf("recording pattern: lock-free first %.1fx the speed (this machine's microbenchmark, not a game measurement)\n", tm[0] / tm[1]);
}

static vst g_it;
static void hook_grow_remove(void *a) {   // the reader holds the old table: grow, then remove its serial
    uint64_t s = (uint64_t)(uintptr_t)a;
    if (!vst_grow(&g_it)) abort();
    vst_release(&g_it, s);
}
static void hook_remove_put(void *a) {   // remove, put back as another entry (same slot)
    uint64_t s = (uint64_t)(uintptr_t)a; int m;
    vst_release(&g_it, s);
    vst_lookup_or_put(&g_it, s, (void *)(uintptr_t)0xBEEF0, &m);
}
static void hook_grow_put(void *a) {   // a new serial goes in the new table only
    uint64_t s = (uint64_t)(uintptr_t)a; int m;
    if (!vst_grow(&g_it)) abort();
    vst_lookup_or_put(&g_it, s, (void *)(uintptr_t)0xCAFE0, &m);
}
static void interleavings(void) {
    int n = 0;
    {
        memset(&g_it, 0, sizeof g_it); g_it.keep_old = 1;
        int m;
        for (uint64_t s = 1; s <= 300; s++) vst_lookup_or_put(&g_it, s, (void *)(uintptr_t)(s * 16), &m);
        g_hook = hook_grow_remove; g_hook_arg = (void *)(uintptr_t)7;
        void *e = vst_get_lockfree(&g_it, 7);
        CHECK(e == NULL, "grown then removed: the old table's entry %p was returned", e); n++;
        CHECK(vst_find(&g_it, 7) == NULL, "removed");
        g_hook = hook_remove_put; g_hook_arg = (void *)(uintptr_t)9;
        e = vst_get_lockfree(&g_it, 9);
        CHECK(e == NULL || e == (void *)(uintptr_t)0xBEEF0, "removed and put back: %p", e); n++;
        g_hook = hook_grow_put; g_hook_arg = (void *)(uintptr_t)5000;
        e = vst_get_lockfree(&g_it, 5000);
        CHECK(e == NULL, "put in the new table while probing the old: %p", e); n++;
        CHECK(vst_get_lockfree(&g_it, 5000) == (void *)(uintptr_t)0xCAFE0, "then found");
        CHECK(vst_get_lockfree(&g_it, 9) == (void *)(uintptr_t)0xBEEF0, "then found");
        CHECK(vst_get_lockfree(&g_it, 10) == (void *)(uintptr_t)160, "others found");
        vst_quiesce(&g_it); free(g_it.tab);
    }
    printf("interleavings: %d step-by-step cases (grown then removed, removed then put back, put in the new table): %s\n", n, fails ? "FAILED" : "ok");
}

int main(void) {
    interleavings();
    differential();
    contention();
    recording();
    printf(fails ? "test_sertab: FAIL (%d)\n" : "test_sertab: PASS\n", fails);
    return fails != 0;
}
