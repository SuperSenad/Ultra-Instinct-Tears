// [UIT 0029] Pool copies (backend_vk.c sync_gather): how far apart two runs of CPU-written pages may be and still be
// copied as one (pages in between copied from game memory too). Shared with tools/tests/test_shadow_pending.c.
#pragma once
#include <stddef.h>

// wb_pending: the pool has GPU writes in its device copy that are not in game memory yet (a submission not retired,
// so wb_merge has not run). Then the pages in between hold the GPU's bytes only in the copy: copied from game memory
// they would go back to what was there before, and wb_merge then writes the GPU's bytes into game memory as GPU writes
// (no generation): the copy stays stale for good. So only the CPU-written pages themselves (gap 0: adjacent ones
// merged). TOTK_VK_POOL_GAP_OLD=1 (development): the 0028 behaviour, 64 KiB always.
static inline size_t vkb_shadow_gap(int wb_pending, int old) { return wb_pending && !old ? 0 : 65536; }
