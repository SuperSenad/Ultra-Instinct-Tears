// [UIT 0140] A parallel for over [0, n) on a small pool of worker threads: the work of one texture upload (the
// deswizzle + key of each surface, the cache reads) spread over the CPU's cores instead of the recording thread alone.
#pragma once

typedef void (*vkb_pfor_fn)(void *ctx, int i);
// fn(ctx, i) once for every i in [0, n), on the calling thread and the pool's workers; returns when all are done.
// Calls running at once from other threads (or from inside fn) are not waited for: they run on their caller alone,
// as before. Each i must only touch what is its own (results by index): the order the calls run in is not defined.
void vkb_pfor(int n, vkb_pfor_fn fn, void *ctx);
// threads one vkb_pfor uses, the calling one included (TOTK_TEX_THREADS; 1 = everything on the caller)
int vkb_pfor_threads(void);
// for tests and the auto value: threads for this many logical cores
int vkb_pfor_auto(int cpus);
