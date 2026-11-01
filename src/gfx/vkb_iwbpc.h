// [UIT native] TOTK_IWB_POOL_COPY=1: game memory a drawn image was copied back into (vkb_iwb_land) is also brought into
// the pool's device copy at the pool's next sync (backend_vk.c sync_gather). The copy back writes game memory as GPU
// writes (no dirty generation, so the image's own next copy back is not taken for a CPU write), so the sync never saw
// those pages: TOTK_VK_SHADOW_VERIFY found the device copy still zeros under GPU-drawn 260x260 probe images
// (0x103ec51000.., 121,123 stale pages over 86 minutes, 2026-10-30). The ranges wait here until their pool syncs.
// Plain list logic, no locking (the caller holds its mutex): shared with tools/tests/test_iwbpc.c.
#pragma once
#include <stdint.h>
#include <stdlib.h>

typedef struct { uintptr_t lo; uint64_t len, seq; } iwbpc_ent;
typedef struct { iwbpc_ent *e; int n, cap, max; uint64_t seq; } iwbpc_list;

// Adds [lo, lo+len). 0: full (max entries; the caller may purge and retry).
static inline int iwbpc_add(iwbpc_list *l, uintptr_t lo, uint64_t len) {
    for (int i = 0; i < l->n; i++)   // the same image again (every copy back of it): one entry, the newest sequence
        if (l->e[i].lo == lo && l->e[i].len == len) { l->e[i].seq = ++l->seq; return 1; }
    if (l->n == l->max) return 0;
    if (l->n == l->cap) {
        int c = l->cap ? l->cap * 2 : 64;
        iwbpc_ent *p = realloc(l->e, sizeof *p * (size_t)c);
        if (!p) return 0;
        l->e = p; l->cap = c;
    }
    l->e[l->n++] = (iwbpc_ent){lo, len, ++l->seq};
    return 1;
}
// Calls fn(ctx, offset from plo, bytes) for every entry's part inside [plo, phi); returns the sequence to drop up to.
static inline uint64_t iwbpc_take(const iwbpc_list *l, uintptr_t plo, uintptr_t phi, void (*fn)(void *, uint64_t, uint64_t), void *ctx) {
    for (int i = 0; i < l->n; i++) {
        uintptr_t a = l->e[i].lo, b = l->e[i].lo + l->e[i].len;
        if (a < plo) a = plo;
        if (b > phi) b = phi;
        if (a < b) fn(ctx, a - plo, b - a);
    }
    return l->seq;
}
// Removes the entries inside [plo, phi) not added again after seq (a copy back after the take waits for the next sync).
static inline void iwbpc_drop(iwbpc_list *l, uintptr_t plo, uintptr_t phi, uint64_t seq) {
    int k = 0;
    for (int i = 0; i < l->n; i++) {
        const iwbpc_ent *x = &l->e[i];
        int in = x->lo >= plo && x->lo + x->len <= phi;
        if (!(in && x->seq <= seq)) l->e[k++] = *x;
    }
    l->n = k;
}
// Keeps only the entries alive(ctx, lo, len) says are still in a pool (pools deleted since).
static inline void iwbpc_purge(iwbpc_list *l, int (*alive)(void *, uintptr_t, uint64_t), void *ctx) {
    int k = 0;
    for (int i = 0; i < l->n; i++) if (alive(ctx, l->e[i].lo, l->e[i].len)) l->e[k++] = l->e[i];
    l->n = k;
}
