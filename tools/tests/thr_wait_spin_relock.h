// [UIT 0246] test helper: a TW_RELOCK that retries the lock briefly (as threads.c g_relock) for tools/tests/test_thr_wait.c
#include <pthread.h>
static inline void tw_spin_relock(pthread_mutex_t *m) { for (int i = 0; i < 40; i++) { for (int k = 0; k < 8; k++) __builtin_ia32_pause(); if (!pthread_mutex_trylock(m)) return; } pthread_mutex_lock(m); }
#define TW_RELOCK(m) tw_spin_relock(m)
