// [UIT 0243] Buffer reads over GPU drawings that could still change something (vkb_iwb_buf_read), without the lock.
//
// vkb_iwb_buf_read walks the pending GPU drawings over a buffer range with the images lock held. Once the first 32
// log lines are used up, the walk does something to a pending image only when TOTK_VK_GPU_MEM_COHERENT is on and
//   - it is not marked copied-back yet (wb_alias 0) and not over IWB_EXPLICIT_MAX bytes: it gets marked; or
//   - it was marked because the game let it go (wb_rel 1): that is cleared.
// Such an image is "actionable" (bact_want). Its 1 MiB chunks of game memory are counted here (hashed, like g_bidx);
// a range with no actionable image in its chunks would walk and change nothing, so it is not walked.
// The counts change only with the images lock held (bact_fix in vkb_tex.c after every write of wb_pending, wb_alias,
// wb_rel, and when an entry goes); readers look without it, as they already did at g_bidx (a drawing made pending
// at that moment is seen by the next read, as before).
#pragma once
#include <stdatomic.h>
#include <stdint.h>

enum { BACT_N = 4096 };
typedef struct { _Atomic uint32_t c[BACT_N]; } bact_tab;

static inline int bact_want(int on, int pending, int alias, int rel, uint64_t size, uint64_t max) {
    return on && pending && ((!alias && size <= max) || rel);
}
static inline unsigned bact_h(uint64_t chunk) { return (unsigned)((chunk * 0x9E3779B97F4A7C15ull) >> 52) & (BACT_N - 1); }
// d = +1 / -1 for an image over [lo, lo + sz) (images lock held)
static inline void bact_mod(bact_tab *t, uint64_t lo, uint64_t sz, int d) {
    if (!lo || !sz) return;
    for (uint64_t c = lo >> 20; c <= (lo + sz - 1) >> 20; c++) atomic_fetch_add_explicit(&t->c[bact_h(c)], (uint32_t)d, memory_order_relaxed);
}
// Any actionable image in the chunks of [addr, addr + len)? The same first GiB as the g_bidx test (len != 0).
static inline int bact_any(const bact_tab *t, uint64_t addr, uint64_t len) {
    uint64_t c0 = addr >> 20, c1 = (addr + len - 1) >> 20;
    if (c1 - c0 > 1024) c1 = c0 + 1024;
    for (uint64_t c = c0; c <= c1; c++)
        if (atomic_load_explicit(&t->c[bact_h(c)], memory_order_relaxed)) return 1;
    return 0;
}
// One image's state: its current share (*cur, 0/1) brought to want (images lock held)
static inline void bact_set(bact_tab *t, int *cur, int want, uint64_t lo, uint64_t sz) {
    if (want == *cur) return;
    *cur = want;
    bact_mod(t, lo, sz, want ? 1 : -1);
}
