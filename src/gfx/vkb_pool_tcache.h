// [UIT 0033] Per recording thread: pools already current in this submission, so vkb_pool_use / vkb_pool_addr skip
// the pool table's lock (g_pool_mu, backend_vk.c) for them. Every draw asks for each of its constant, storage, vertex
// and index buffers (~20 per draw, 6 threads): the shared lock's waits were 0.5..5.7 ms per frame (logs, 2026-10-29),
// mostly behind the exclusive lock a storing draw takes for its GPU write. An entry is valid only for the submission
// (sub) and the pool table generation (gen: bumped under the lock by every pool added, deleted or given up) it was
// made in; any other value of either misses, so nothing is ever used across a pool change or a submission.
//   read entries: [lo, hi) of a pool current (or never copied, no_shadow) in sub, with its device copy's address
//   write entries: a GPU-written range already added (wb_add) in sub: the same or a contained write adds nothing new
// Pure logic (no locks, no Vulkan): tools/tests/test_pool_tcache.c.
#ifndef VKB_POOL_TCACHE_H
#define VKB_POOL_TCACHE_H
#include <stdint.h>

#define PTC_R 8
#define PTC_W 16
typedef struct { uint64_t lo, hi, sub, gen, base, soff; void *sbuf; int copy, ns; } ptc_read;   // copy: sbuf/soff hold the current copy; ns: never copied
typedef struct { uint64_t lo, hi, sub, gen; } ptc_write;
typedef struct { ptc_read r[PTC_R]; ptc_write w[PTC_W]; unsigned nr, nw; uint64_t hits, misses; } ptc_t;

static inline const ptc_read *ptc_find(const ptc_t *c, uint64_t addr, uint64_t len, uint64_t sub, uint64_t gen) {
    for (int i = 0; i < PTC_R; i++) {
        const ptc_read *e = &c->r[i];
        if (e->sub == sub && e->gen == gen && addr >= e->lo && addr + len <= e->hi && addr + len >= addr) return e;
    }
    return 0;
}
static inline void ptc_put(ptc_t *c, const ptc_read *e) {
    for (int i = 0; i < PTC_R; i++)   // the same pool again (another submission): its slot
        if (c->r[i].lo == e->lo && c->r[i].hi == e->hi) { c->r[i] = *e; return; }
    c->r[c->nr++ % PTC_R] = *e;
}
static inline int ptc_wfind(const ptc_t *c, uint64_t addr, uint64_t len, uint64_t sub, uint64_t gen) {
    for (int i = 0; i < PTC_W; i++) {
        const ptc_write *e = &c->w[i];
        if (e->sub == sub && e->gen == gen && addr >= e->lo && addr + len <= e->hi && addr + len >= addr) return 1;
    }
    return 0;
}
static inline void ptc_wput(ptc_t *c, uint64_t addr, uint64_t len, uint64_t sub, uint64_t gen) {
    c->w[c->nw++ % PTC_W] = (ptc_write){addr, addr + len, sub, gen};
}
#endif
