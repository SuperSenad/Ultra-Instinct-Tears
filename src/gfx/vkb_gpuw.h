// [UIT 0027] GPU writes the copies taken at the submit call must know of (vkb_ucap.c); no Vulkan here, so that
// tools/tests/test_ucap_gpuw.c can run it on its own.
#pragma once
#include <stdint.h>

typedef struct { uint64_t lo, hi, fence; } gpuw_t;
// [UIT 0038] keep = GPUW_FOREVER: completed writes are never forgotten (no overflow in fence + keep)
#define GPUW_FOREVER UINT64_MAX
static inline int gpuw_kept(uint64_t fence, uint64_t keep, uint64_t cdone) { return cdone < fence || cdone - fence < keep; }
typedef struct {
    gpuw_t *pend; int npend, cappend;   // in submission order; not completed when the capture being replayed was taken
    gpuw_t *rec; int nrec, caprec;      // completed before it: sorted by lo, disjoint, fence the latest merged in
    gpuw_t *ix; uint64_t *ixhi; int nix, capix, ixok;   // pend sorted by lo with the running maximum of hi (built by retire)
} gpuw_set;

void gpuw_add(gpuw_set *s, uint64_t lo, uint64_t hi, uint64_t fence);
// GPU writes the capture predates: written by a submission not complete when it was taken (cdone)
int gpuw_inflight(const gpuw_set *s, uint64_t lo, uint64_t hi, uint64_t cdone);
// the same, or completed within the last keep submissions before it (their bytes may be only in the pool's device copy)
int gpuw_recent(const gpuw_set *s, uint64_t lo, uint64_t hi, uint64_t cdone, uint64_t keep);
// Before a capture's checks (call the checks right after it, before gpuw_add): what completed by its cdone moves to rec; rec older than keep submissions is forgotten.
void gpuw_retire(gpuw_set *s, uint64_t cdone, uint64_t keep);
// [UIT 0157] each GPU write overlapping [lo, hi) that gpuw_inflight (recent = 0) or gpuw_recent (recent = 1) would count,
// clipped to [lo, hi); returns how many (call after gpuw_retire, as those)
typedef void (*gpuw_fn)(void *ctx, uint64_t lo, uint64_t hi);
int gpuw_each(const gpuw_set *s, uint64_t lo, uint64_t hi, uint64_t cdone, int recent, uint64_t keep, gpuw_fn fn, void *ctx);
