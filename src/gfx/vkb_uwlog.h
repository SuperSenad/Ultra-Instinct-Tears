// [UIT 0335] What the GPU wrote into uniform memory earlier in a submission, for the CPU reads of pointers kept there.
// The game rewrites some uniform buffers between draws from the command buffer (nvnCommandBufferUpdateUniformBuffer,
// nvnCommandBufferCopyBufferToBuffer). The GPU sees each draw's bytes; the CPU-side reads of the {address, size} pairs
// behind storage buffers (vkb_ucap_read) saw the bytes as of the submit call instead (or game memory, which the replay
// never writes while the pools have device copies). In the Depths c6 of the ground shaders is rewritten 48 bytes
// before every ground draw (frame capture 2026-11-01 12:51: 441 draws, one GPU write each 4 commands), and every one of
// those draws was bound the storage buffer of the address the copy held: the ground's vertex data of another draw
// (missing floors, the Gloom gone, spikes and stretched faces).
// The log keeps the bytes of each such write with its position in the submission. One thread appends, in position
// order (the walk of the submission with several recording threads, else the replay itself); readers, each at its own
// position, apply the writes at or before it to a private page map (each write once per reader and submission) and
// lay those bytes over the base read. Storage is allocated once and never moves, so readers need no lock.
#pragma once
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

enum { UWL_ENTS = 1 << 16, UWL_BYTES = 8 << 20, UWL_WMAX = 16384, UWL_PAGE = 256 };

typedef struct { uint64_t at, lo; uint32_t n, off; } uwl_ent;
typedef struct {
    uwl_ent *e;
    uint8_t *b;
    atomic_int ne;            // entries published (release after the entry and its bytes are written)
    atomic_uint gen;          // bumped by each reset
    size_t nb;
    uint64_t logged, over;    // writes kept; writes not kept (too big, or the log full)
} uwlog;

// a reader's private view: pages of written bytes with a mask of the ones written
typedef struct { uint64_t page; uint8_t mask[UWL_PAGE / 8]; uint8_t d[UWL_PAGE]; } uwl_pg;
typedef struct { unsigned gen; int cur, used; uint64_t seq; int cap; uwl_pg *pg; int *slot; int nslot; } uwl_view;

static inline int uwl_init(uwlog *l) {
    if (l->e) return 1;
    l->e = malloc(sizeof *l->e * UWL_ENTS);
    l->b = malloc(UWL_BYTES);
    if (!l->e || !l->b) { free(l->e); free(l->b); l->e = NULL; l->b = NULL; return 0; }
    return 1;
}
// a new submission (the readers of the previous one have finished)
static inline void uwl_reset(uwlog *l) {
    atomic_store_explicit(&l->ne, 0, memory_order_relaxed);
    l->nb = 0;
    atomic_fetch_add_explicit(&l->gen, 1, memory_order_release);
}
// the writer: n bytes at lo written at position at (positions never go back between resets); 0: not kept
static inline int uwl_add(uwlog *l, uint64_t at, uint64_t lo, uint64_t n, const uint8_t *data) {
    if (!n) return 1;
    int k = atomic_load_explicit(&l->ne, memory_order_relaxed);
    if (!data || n > UWL_WMAX || !uwl_init(l) || k >= UWL_ENTS || l->nb + n > UWL_BYTES) { l->over++; return 0; }
    memcpy(l->b + l->nb, data, (size_t)n);
    l->e[k] = (uwl_ent){at, lo, (uint32_t)n, (uint32_t)l->nb};
    l->nb += (size_t)n;
    l->logged++;
    atomic_store_explicit(&l->ne, k + 1, memory_order_release);
    return 1;
}
static inline uwl_pg *uwl_page(uwl_view *v, uint64_t page, int make) {
    if (!v->nslot) {
        if (!make) return NULL;
        v->nslot = 1024;
        if (!(v->slot = malloc(sizeof *v->slot * (size_t)v->nslot))) { v->nslot = 0; return NULL; }
        for (int i = 0; i < v->nslot; i++) v->slot[i] = -1;
    }
    unsigned h = (unsigned)((page * 0x9E3779B97F4A7C15ull) >> 40) & (unsigned)(v->nslot - 1);
    for (;; h = (h + 1) & (unsigned)(v->nslot - 1)) {
        int s = v->slot[h];
        if (s < 0) break;
        if (v->pg[s].page == page) return &v->pg[s];
    }
    if (!make) return NULL;
    if (v->used * 2 >= v->nslot) {   // grow the slots (and look again)
        int ns = v->nslot * 2, *n = malloc(sizeof *n * (size_t)ns);
        if (!n) return NULL;
        for (int i = 0; i < ns; i++) n[i] = -1;
        for (int s = 0; s < v->used; s++) {
            unsigned g = (unsigned)((v->pg[s].page * 0x9E3779B97F4A7C15ull) >> 40) & (unsigned)(ns - 1);
            while (n[g] >= 0) g = (g + 1) & (unsigned)(ns - 1);
            n[g] = s;
        }
        free(v->slot); v->slot = n; v->nslot = ns;
        return uwl_page(v, page, make);
    }
    if (v->used == v->cap) {
        int nc = v->cap ? v->cap * 2 : 256;
        uwl_pg *p = realloc(v->pg, sizeof *p * (size_t)nc);
        if (!p) return NULL;
        v->pg = p; v->cap = nc;
    }
    int s = v->used++;
    memset(&v->pg[s], 0, sizeof v->pg[s]);
    v->pg[s].page = page;
    v->slot[h] = s;
    return &v->pg[s];
}
static inline void uwl_view_clear(uwl_view *v) {
    for (int i = 0; i < v->nslot; i++) v->slot[i] = -1;
    v->used = 0; v->cur = 0; v->seq = 0;
}
static inline void uwl_view_free(uwl_view *v) { free(v->pg); free(v->slot); memset(v, 0, sizeof *v); }
// bring the view to position seq
static inline void uwl_sync(const uwlog *l, uwl_view *v, uint64_t seq) {
    unsigned gen = atomic_load_explicit(&((uwlog *)l)->gen, memory_order_acquire);
    if (v->gen != gen || seq < v->seq) { uwl_view_clear(v); v->gen = gen; }
    v->seq = seq;
    int ne = atomic_load_explicit(&((uwlog *)l)->ne, memory_order_acquire);
    for (; v->cur < ne && l->e[v->cur].at <= seq; v->cur++) {
        const uwl_ent *e = &l->e[v->cur];
        const uint8_t *src = l->b + e->off;
        for (uint64_t a = e->lo, end = e->lo + e->n; a < end;) {
            uint64_t page = a / UWL_PAGE, pe = (page + 1) * UWL_PAGE, stop = pe < end ? pe : end;
            uwl_pg *p = uwl_page(v, page, 1);
            if (p) for (uint64_t x = a; x < stop; x++) {
                unsigned o = (unsigned)(x - page * UWL_PAGE);
                p->d[o] = src[x - e->lo];
                p->mask[o / 8] |= (uint8_t)(1u << (o % 8));
            }
            a = stop;
        }
    }
}
// lay the bytes written at or before seq over out (n bytes read at addr); the number of bytes replaced
static inline size_t uwl_read(const uwlog *l, uwl_view *v, uint64_t seq, uint64_t addr, void *out, size_t n) {
    if (!l->e || !n) return 0;
    uwl_sync(l, v, seq);
    if (!v->used) return 0;
    size_t got = 0;
    uint8_t *o = out;
    for (uint64_t a = addr, end = addr + n; a < end;) {
        uint64_t page = a / UWL_PAGE, pe = (page + 1) * UWL_PAGE, stop = pe < end ? pe : end;
        const uwl_pg *p = uwl_page(v, page, 0);
        if (p) for (uint64_t x = a; x < stop; x++) {
            unsigned b = (unsigned)(x - page * UWL_PAGE);
            if (p->mask[b / 8] & (1u << (b % 8))) { o[x - addr] = p->d[b]; got++; }
        }
        a = stop;
    }
    return got;
}
