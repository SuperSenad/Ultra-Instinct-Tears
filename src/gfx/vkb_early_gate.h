// [UIT 0236] TOTK_VK_EARLY_STREAM: who may send an early part, and when (backend_vk.c early_try; box test
// tools/tests/test_early_stream.c, which runs it with Windows-like non-recursive locks).
// The gate is open from the end of the walk (every chunk cut, the submission's draws known) until the translation
// thread assembles the last part. Closing takes the lock, so it waits for a part being sent by a recording thread; after
// it no thread sends for this submission, and the next submission's walk opens it again. Recording threads only try
// (pthread_mutex_trylock): a thread that finds another sending goes back to recording, and its chunk goes with a later
// part. Lock order: gate -> g_mt_mu (a snapshot, released) -> g_pro_mu -> the submit queue / the Vulkan queue lock;
// nothing that holds one of those takes the gate.
#ifndef VKB_EARLY_GATE_H
#define VKB_EARLY_GATE_H
#include <pthread.h>
typedef struct { pthread_mutex_t mu; int open; } vkb_early_gate;
#define VKB_EARLY_GATE_INIT {PTHREAD_MUTEX_INITIALIZER, 0}
// 1: this thread holds the gate and it is open (call vkb_early_leave); 0: not now. wait = 0: try only.
static inline int vkb_early_enter(vkb_early_gate *g, int wait) {
    if (wait) pthread_mutex_lock(&g->mu);
    else if (pthread_mutex_trylock(&g->mu)) return 0;
    if (!g->open) { pthread_mutex_unlock(&g->mu); return 0; }
    return 1;
}
static inline void vkb_early_leave(vkb_early_gate *g) { pthread_mutex_unlock(&g->mu); }
static inline void vkb_early_set_open(vkb_early_gate *g, int open) {   // close: waits for a send in progress
    pthread_mutex_lock(&g->mu);
    g->open = open;
    pthread_mutex_unlock(&g->mu);
}
#endif
