// [UIT 0047] Which thread is recording GPU work (from mtl_sub_begin until mtl_sub_commit). One thread at a time: the
// current command buffer, its slot, the post ops and the upload ring are shared, unlocked state (backend_vk.c). With the
// translation thread (TOTK_ASYNC_REPLAY, the default) every recording runs there; other threads hand their GPU work to it
// (nvn_x_call). This only checks that, for the log: one compare-and-swap per submission, never a wait.
#pragma once
#include <stdatomic.h>
#include <stdint.h>

typedef struct { _Atomic uintptr_t owner; _Atomic int clashes; } vkb_recown;
static __thread char vkb_recown_tag;   // its address names the thread

// Returns 0, or the number of this clash (1 for the first) when another thread is recording already.
static inline int vkb_recown_enter(vkb_recown *o) {
    uintptr_t me = (uintptr_t)&vkb_recown_tag, cur = 0;
    if (atomic_compare_exchange_strong_explicit(&o->owner, &cur, me, memory_order_acquire, memory_order_relaxed) || cur == me)
        return 0;
    return atomic_fetch_add_explicit(&o->clashes, 1, memory_order_relaxed) + 1;
}
static inline void vkb_recown_leave(vkb_recown *o) {
    uintptr_t me = (uintptr_t)&vkb_recown_tag;
    atomic_compare_exchange_strong_explicit(&o->owner, &me, 0, memory_order_release, memory_order_relaxed);
}
