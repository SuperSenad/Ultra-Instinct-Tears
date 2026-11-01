// [UIT native] GPU time per game frame (backend_vk.c: mtl_gpu_ms / mtl_gpu_span; box test tools/tests/test_gpuframe.c).
// One writer (the retire thread, gpu_frame_put once per completed submission), any readers. An entry holds frame f as
// f + 1 (0: empty); a reader that sees another frame there gets "not measured" (-1).
#pragma once
#include <stdatomic.h>
#include <stdint.h>
#define GFN 512
typedef struct { _Atomic uint64_t f, ns, t0, t1, n; } gpu_frame;
static inline void gpu_frame_put(gpu_frame *g, uint64_t f, uint64_t beg, uint64_t end, uint64_t dur) {
    gpu_frame *e = &g[f % GFN];
    if (atomic_load_explicit(&e->f, memory_order_relaxed) != f + 1) {   // a new frame in this entry
        atomic_store_explicit(&e->f, 0, memory_order_relaxed);
        atomic_store_explicit(&e->ns, 0, memory_order_relaxed); atomic_store_explicit(&e->n, 0, memory_order_relaxed);
        atomic_store_explicit(&e->t0, beg, memory_order_relaxed); atomic_store_explicit(&e->t1, end, memory_order_relaxed);
        atomic_store_explicit(&e->f, f + 1, memory_order_release);
    }
    atomic_fetch_add_explicit(&e->ns, dur, memory_order_relaxed); atomic_fetch_add_explicit(&e->n, 1, memory_order_relaxed);
    if (beg < atomic_load_explicit(&e->t0, memory_order_relaxed)) atomic_store_explicit(&e->t0, beg, memory_order_relaxed);
    if (end > atomic_load_explicit(&e->t1, memory_order_relaxed)) atomic_store_explicit(&e->t1, end, memory_order_relaxed);
}
static inline double gpu_frame_ms(gpu_frame *g, uint64_t f) {
    gpu_frame *e = &g[f % GFN];
    if (atomic_load_explicit(&e->f, memory_order_acquire) != f + 1) return -1;
    return atomic_load_explicit(&e->ns, memory_order_relaxed) / 1e6;
}
static inline int gpu_frame_span(gpu_frame *g, uint64_t f, uint64_t *t0, uint64_t *t1) {
    gpu_frame *e = &g[f % GFN];
    *t0 = *t1 = 0;
    if (atomic_load_explicit(&e->f, memory_order_acquire) != f + 1) return 0;
    *t0 = atomic_load_explicit(&e->t0, memory_order_relaxed); *t1 = atomic_load_explicit(&e->t1, memory_order_relaxed);
    return *t1 > *t0;
}
