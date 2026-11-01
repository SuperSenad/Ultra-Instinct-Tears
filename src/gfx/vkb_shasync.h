// [UIT 0039] Asynchronous shader compilation (TOTK_SHADER_ASYNC): the state machine of one shader's compile and the
// "rendered into every frame" streak, on their own so tools/tests/test_shader_async.c can run them without Vulkan.
//   pending: 1 queued, 2 being compiled, 0 done. Whoever moves it from 1 to 2 compiles it (exactly one thread);
//   publishing 0 (release, under mu, broadcasting cv) makes what the compiler wrote visible to whoever then reads 0.
#pragma once
#include <pthread.h>
#include <stdint.h>

static inline int shasync_claim(int *pending) {
    int expect = 1;
    return __atomic_compare_exchange_n(pending, &expect, 2, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
}
static inline void shasync_publish(int *pending, pthread_mutex_t *mu, pthread_cond_t *cv) {
    pthread_mutex_lock(mu);
    __atomic_store_n(pending, 0, __ATOMIC_RELEASE);
    pthread_cond_broadcast(cv);
    pthread_mutex_unlock(mu);
}
static inline void shasync_wait_done(int *pending, pthread_mutex_t *mu, pthread_cond_t *cv) {
    pthread_mutex_lock(mu);
    while (__atomic_load_n(pending, __ATOMIC_ACQUIRE)) pthread_cond_wait(cv, mu);
    pthread_mutex_unlock(mu);
}

// A target rendered into at frame f, last rendered into at frame `last` with streak `streak`: the new streak (frames in
// a row, a gap of one frame allowed; 0 = never before). Capped so it never wraps.
static inline uint32_t shasync_streak_next(uint64_t last, uint64_t f, uint32_t streak) {
    if (last == f) return streak;
    uint32_t k = last && f > last && f - last <= 2 ? streak + 1 : 1;
    return k > 1000000 ? 1000000 : k;
}
// Rendered into in each of the last `need` frames, the latest at most two frames before f.
static inline int shasync_every_frame(uint64_t last, uint32_t streak, uint64_t f, uint32_t need) {
    return last && streak >= need && last + 2 >= f;
}
// A draw with a shader still compiling may be skipped while f is within max_skip frames of the first skip (since).
static inline int shasync_within(uint64_t since, uint64_t f, uint64_t max_skip) { return !since || f <= since + max_skip; }
