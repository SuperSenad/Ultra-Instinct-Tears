// [UIT 0234] Buffer reads already known to change nothing (vkb_iwb_buf_read), per thread.
//
// vkb_iwb_buf_read takes the images lock and walks the pending GPU drawings over the range; for each one it may mark
// it copied-back (wb_alias), clear wb_rel, or log (the first 32). When a walk did none of these, the same range gives
// the same "nothing" again as long as nothing that could create an effect changed. Everything that can is counted in
// one generation (g_bidx_gen, bumped under the images lock):
//   - a drawing becoming pending (iwb_stamp -> bidx_mod): a new image in the walk;
//   - wb_alias 1 -> 0 / wb_rel 0 -> 1 (taken back from the side / let go by the game);
// the rest only remove effects (pending -> not, wb_alias 0 -> 1, wb_rel 1 -> 0, an entry dropped), and the log
// counter only grows (a "nothing" with it under 32 means no pending drawing overlapped at all).
// So a range found here with the current generation is exactly a walk that would do nothing: skipped, no lock.
// Only results without effects are stored; the generation is read before the walk (a later bump makes it stale).
#pragma once
#include <stdint.h>

enum { BQM_N = 128 };   // direct-mapped, per thread
typedef struct { uint64_t addr, len, gen; } bqm_ent;   // len 0: empty
typedef struct { bqm_ent e[BQM_N]; } bqm;

static inline unsigned bqm_h(uint64_t addr, uint64_t len) {
    return (unsigned)(((addr ^ (len * 0x9E3779B97F4A7C15ull)) * 0xBF58476D1CE4E5B9ull) >> 57) & (BQM_N - 1);
}
static inline int bqm_hit(const bqm *m, uint64_t addr, uint64_t len, uint64_t gen) {
    const bqm_ent *x = &m->e[bqm_h(addr, len)];
    return x->len && x->addr == addr && x->len == len && x->gen == gen;
}
static inline void bqm_put(bqm *m, uint64_t addr, uint64_t len, uint64_t gen) {   // len != 0
    bqm_ent *x = &m->e[bqm_h(addr, len)];
    x->addr = addr; x->len = len; x->gen = gen;
}
