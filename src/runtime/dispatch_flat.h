// [UIT 0088] A flat table in front of dispatch_lookup's binary search (TOTK_DISPATCH_FLAT; default on since UIT 0127, =0 turns it off).
//
// One 32-bit slot per possible function start (every 4 bytes) between a module's first and last lifted function: 0 =
// not looked up yet, else the function's index + 1 in the module's sorted table. A slot is filled the first time its
// address is looked up (by the binary search, so the result is the same), and from then on the lookup is one load.
// Only found functions are stored (an address with no function stays 0 and is searched again: that is a fatal error
// anyway). Filling is a relaxed atomic store of a value every thread would compute the same way, so races are
// harmless. The memory is reserved zeroed and only the touched pages become real memory (4 bytes per 4 bytes of code
// at most: the game's three modules reserve about as much as their code).
#ifndef UIT_DISPATCH_FLAT_H
#define UIT_DISPATCH_FLAT_H
#include <stdint.h>
#include <stdlib.h>

typedef struct { uint64_t lo, n; uint32_t *slot; } FlatMap;

static inline int flat_init(FlatMap *m, uint64_t lo, uint64_t hi, uint64_t count) {
    m->lo = lo; m->n = 0; m->slot = NULL;
    if (!count || hi < lo || (hi - lo) / 4 + 1 > 0x7fffffffu || count > 0xfffffffeu) return 0;
    uint64_t n = (hi - lo) / 4 + 1;
    m->slot = (uint32_t *)calloc(n, sizeof(uint32_t));
    if (!m->slot) return 0;
    m->n = n;
    return 1;
}
// The slot of addr, or NULL (outside the module, or not a multiple of 4 from its start)
static inline uint32_t *flat_slot(const FlatMap *m, uint64_t addr) {
    uint64_t d = addr - m->lo;
    if (d & 3) return NULL;
    d >>= 2;
    return d < m->n ? &m->slot[d] : NULL;
}
// The index of addr in the sorted addrs[0..count), or -1: the slot if filled, else a binary search that fills it
static inline int64_t flat_find(const FlatMap *m, const uint64_t *addrs, uint64_t count, uint64_t addr) {
    uint32_t *s = m->slot ? flat_slot(m, addr) : NULL;
    if (s) {
        uint32_t v = __atomic_load_n(s, __ATOMIC_RELAXED);
        if (v) return (int64_t)v - 1;
    }
    uint64_t lo = 0, hi = count;
    while (lo < hi) {
        uint64_t mid = (lo + hi) / 2;
        if (addrs[mid] < addr) lo = mid + 1;
        else hi = mid;
    }
    if (lo < count && addrs[lo] == addr) {
        if (s) __atomic_store_n(s, (uint32_t)(lo + 1), __ATOMIC_RELAXED);
        return (int64_t)lo;
    }
    return -1;
}
#endif
