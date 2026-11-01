// 客体内存写保护脏页跟踪（nvn_dirty.c）
// Write-protect dirty-page tracking for guest memory (nvn_dirty.c)
#pragma once
#include <stddef.h>
#include <stdint.h>

void nvn_dirty_add_range(const void *mem, size_t size);           // 登记一个内存池为跟踪区间（首次调用时注册缺页钩子与宿主写前钩子） | Register a memory pool as a tracked range (the first call registers the page-fault hook and the host pre-write hook)
void nvn_dirty_stat(int *live, int *peak, int *cap);                // 在册的内存池数、峰值、跟踪表容量（内存读数行用） | number of registered memory pools, peak, tracking table capacity (for the memory readout line)
void nvn_dirty_remove_range(const void *mem);                     // 池销毁：受保护页还原可写并标失效 | Pool destruction: restore protected pages to writable and mark invalid
int nvn_dirty_tracked(const void *addr, size_t len);
void nvn_dirty_protect(const void *addr, size_t len);              // 覆盖页设只读 | Set the covered pages read-only
uint64_t nvn_dirty_now(void);                                      // 当前代号：上传前取，作为该份内容的戳 | Current generation: taken before upload, as the stamp of that content
int nvn_dirty_since(const void *addr, size_t len, uint64_t stamp);
int nvn_dirty_written_after(const void *addr, size_t len, uint64_t stamp);   // [UIT 0018] generations so far above stamp (no scan); untracked: 0
// Runs of pages written since stamp (merged when closer than gap bytes): fn(ctx, offset, bytes); -1 = untracked
void nvn_dirty_epoch_begin(void);   // [Windows port] a new submission: one write-watch scan per range on this thread until the next
uint64_t nvn_dirty_epoch_now(void);   // [UIT 0249] the current epoch (nvn_dirty_epoch_prescan)
int nvn_dirty_epoch_prescan(const void *addr, uint64_t e);   // [UIT 0249] the epoch thread's whole-range scan for epoch e, on any thread
int nvn_dirty_runs(const void *addr, size_t len, uint64_t stamp, size_t gap, void (*fn)(void *ctx, size_t off, size_t n), void *ctx);
// [UIT 0025] the same, and the stamp for what fn copied (the generation taken with the range locked after its scan)
int nvn_dirty_runs_stamp(const void *addr, size_t len, uint64_t stamp, size_t gap, void (*fn)(void *ctx, size_t off, size_t n), void *ctx, uint64_t *now);
// [UIT 0310] the same, the span scanned now even when this thread's epoch scan of the range came earlier
int nvn_dirty_runs_fresh(const void *addr, size_t len, uint64_t stamp, size_t gap, void (*fn)(void *ctx, size_t off, size_t n), void *ctx, uint64_t *now);
uint64_t nvn_dirty_last_stamp(void);   // [UIT 0025] that stamp for the calling thread's last nvn_dirty_since / nvn_dirty_runs
// [UIT 0028] write guest memory on behalf of the GPU (copy n bytes, only where they differ): not counted as a CPU
// write, but a CPU write elsewhere in those pages meanwhile still is. 1: some byte changed
int nvn_dirty_gpu_write(void *dst, const void *src, size_t n);
uint64_t nvn_dirty_gpu_raced(void);   // pages where a CPU write was found during that (kept as the CPU's)
// [Windows port] GPU writes of guest memory (through nvn_dirty_gpu_write_*): a sequence number, and whether a range had
// one since a stamp (1 / 0, exact per 4 KiB page). Textures uploaded from
// game memory check it: the write watch does not count these writes as the CPU's.
uint64_t nvn_gpu_write_now(void);
void nvn_gpu_write_mark(const void *addr, size_t len);   // the GPU's bytes are in place there (writers take turns)
int nvn_gpu_written_since(const void *addr, size_t len, uint64_t stamp);
typedef struct { uintptr_t page; uint64_t pc, lr; uint32_t src; } nvn_dirty_rec;   // src 0: guest write (fault), 1: host write
int nvn_dirty_recent(uintptr_t lo, uintptr_t hi, nvn_dirty_rec *out, int max);   // latest unprotections in [lo, hi) (DM_NVN_TILELOG) // 覆盖页在戳之后被写过 → 1 | Covered pages were written after the stamp -> 1
void nvn_dirty_stats(uint64_t *faults, uint64_t *preps);           // 累计：游戏写触发的缺页次数、宿主写前解保护的页数 | Cumulative: number of page faults triggered by game writes, number of pages unprotected before host writes
void nvn_dirty_mp_stats(uint64_t *calls, uint64_t *pages, uint64_t *ns);   // 累计：本模块 mprotect 调用次数、覆盖页数、耗时 | Cumulative: this module's mprotect call count, pages covered, time spent
void nvn_dirty_slot_stats(int *live, int *used_slots, uint64_t *adds_total); // 当前存活槽、槽表高水位、登记总次数 | Currently live slots, slot table high-water mark, total number of registrations
void nvn_dirty_index_stats(uint64_t *hits, uint64_t *none, uint64_t *slow);   // [UIT 0240] TOTK_DIRTY_INDEX: lookups the index answered (a range / none) and those it left to the slots
