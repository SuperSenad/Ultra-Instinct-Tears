// [UIT 0087] The per-thread wait of threads.c: a condition variable, or (TOTK_WAIT_ON_ADDRESS=1) WaitOnAddress.
//
// threads.c keeps every guest wait under its global lock G and gives each thread one wait object; signals are always
// given with G held, and every wait is a loop that rechecks its condition with G held. With the setting on, the wait
// object is a 32-bit counter: a signal adds one and wakes the waiter (WakeByAddressSingle); a wait reads the counter
// with G held, lets G go, and sleeps until the counter differs (WaitOnAddress). A signal given after the read changes
// the counter, so it can never be missed; an early return just makes the caller's loop look again. The mode is read
// once (the first wait or signal) and never changes, so a signal and its wait always use the same kind.
// Linux (box tests): the same with a futex.
#ifndef UIT_THR_WAIT_H
#define UIT_THR_WAIT_H
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>   // WaitOnAddress, WakeByAddressSingle (synchronization.lib, linked by tools/build_game.py)
#else
#include <linux/futex.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

// [UIT 0246] How the WaitOnAddress path takes the lock back after waking (threads.c: its g_lock, which retries a busy
// lock briefly before sleeping; a waker usually still holds it for a moment after the signal). Default: plain lock.
#ifndef TW_RELOCK
#define TW_RELOCK(m) pthread_mutex_lock(m)
#endif
typedef struct { pthread_cond_t cv; uint32_t seq, waiters; } thr_wait_t;   // waiters: [UIT 0361] threads inside tw_wait

// [UIT 0361] TOTK_SCHED_DEFER_WAKE (with TOTK_WAIT_ON_ADDRESS=1; [UIT 0366] default on, =0 off): fewer trips through the lock and the
// kernel per wake-up. (1) A signal still changes the counter with the lock held, but the wake-up call is made once the
// signalling thread has let the lock go (tw_flush, from the caller's unlock and from tw_wait): a thread woken with the
// lock still held ran straight into it and had to retry or sleep again in it (the [UIT 0246] retries, ~200 a frame).
// (2) A signal to a wait object nobody is waiting on makes no wake-up call at all (more wakes than waits a frame in
// the logs). No wake-up can be lost: a waiter counts itself in (waiters) and reads the counter with the lock held, so
// a signal after that, given with the lock held, sees it and its wake-up follows; one before it changed the condition
// the waiter's loop checks with the lock held, as before. The idea of counting waiters and waking after the lock
// comes from tpcr (valentderah/totk-pc-recomp, MIT license), pull request 326; written anew for these waits.
// The caller must call tw_flush() after every unlock of the lock its signals were given under (threads.c: g_unlock).
enum { TW_DEFER_MAX = 32 };
static _Thread_local uint32_t *tw_q[TW_DEFER_MAX];
static _Thread_local int tw_nq;
static uint64_t tw_n_skip, tw_n_defer;   // statistics (relaxed adds): wake-ups not needed, wake-ups made after the lock
static int tw_defer_mode = -1;

// [UIT 0257] Activity of the WaitOnAddress path (counted only with it on), for threads.c's [sched] log line.
static uint64_t tw_n_wait, tw_n_wake, tw_n_timeout;

static int tw_mode = -1;   // -1 not read yet, 0 condition variables, 1 WaitOnAddress
static inline int tw_on(void) {
    int m = __atomic_load_n(&tw_mode, __ATOMIC_ACQUIRE);
    if (m < 0) {
        const char *e = getenv("TOTK_WAIT_ON_ADDRESS");
        m = e && !strcmp(e, "1");
        int exp = -1;
        if (!__atomic_compare_exchange_n(&tw_mode, &exp, m, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) m = exp;
    }
    return m;
}
static inline int tw_defer_on(void) {   // read once, like tw_mode
    int m = __atomic_load_n(&tw_defer_mode, __ATOMIC_ACQUIRE);
    if (m < 0) {
        const char *e = getenv("TOTK_SCHED_DEFER_WAKE");
        m = !(e && !strcmp(e, "0"));   // [UIT 0366] on unless TOTK_SCHED_DEFER_WAKE=0 (was: only with =1)
        int exp = -1;
        if (!__atomic_compare_exchange_n(&tw_defer_mode, &exp, m, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) m = exp;
    }
    return m;
}
static inline void tw_init(thr_wait_t *w) { pthread_cond_init(&w->cv, NULL); w->seq = 0; w->waiters = 0; }
static inline void tw_destroy(thr_wait_t *w) { pthread_cond_destroy(&w->cv); }

static inline void tw_wake_addr(uint32_t *a) {
#ifdef _WIN32
    WakeByAddressSingle(a);
#else
    syscall(SYS_futex, a, FUTEX_WAKE_PRIVATE, 1, NULL, NULL, 0);
#endif
}
// [UIT 0361] the wake-ups this thread's signals left for after the unlock (cheap when there are none)
static inline void tw_flush(void) {
    int n = tw_nq;
    if (__builtin_expect(!n, 1)) return;
    tw_nq = 0;
    for (int i = 0; i < n; i++) tw_wake_addr(tw_q[i]);
}
static inline void tw_signal(thr_wait_t *w) {   // with the lock held
    if (!tw_on()) { pthread_cond_signal(&w->cv); return; }
    __atomic_add_fetch(&w->seq, 1, __ATOMIC_SEQ_CST);
    __atomic_add_fetch(&tw_n_wake, 1, __ATOMIC_RELAXED);
    if (tw_defer_on()) {   // [UIT 0361]
        if (!__atomic_load_n(&w->waiters, __ATOMIC_SEQ_CST)) { __atomic_add_fetch(&tw_n_skip, 1, __ATOMIC_RELAXED); return; }
        for (int i = 0; i < tw_nq; i++) if (tw_q[i] == &w->seq) return;   // already queued: one wake-up covers both
        if (tw_nq < TW_DEFER_MAX) { tw_q[tw_nq++] = &w->seq; __atomic_add_fetch(&tw_n_defer, 1, __ATOMIC_RELAXED); return; }
    }
    tw_wake_addr(&w->seq);
}

// With m held; returns with m held. dl: a CLOCK_REALTIME deadline (as pthread_cond_timedwait), or NULL for none.
// Returns 0 or ETIMEDOUT (either may also be an early return: the caller looks again, as with a condition variable).
static inline int tw_wait(thr_wait_t *w, pthread_mutex_t *m, const struct timespec *dl) {
    if (!tw_on()) return dl ? pthread_cond_timedwait(&w->cv, m, dl) : pthread_cond_wait(&w->cv, m);
    uint32_t seen = __atomic_load_n(&w->seq, __ATOMIC_SEQ_CST);
    int64_t left_ns = -1;
    if (dl) {
        struct timespec now; clock_gettime(CLOCK_REALTIME, &now);
        left_ns = (int64_t)(dl->tv_sec - now.tv_sec) * 1000000000 + (dl->tv_nsec - now.tv_nsec);
        if (left_ns <= 0) return ETIMEDOUT;
    }
    int dw = tw_defer_on();
    if (dw) __atomic_add_fetch(&w->waiters, 1, __ATOMIC_SEQ_CST);   // [UIT 0361] before the lock goes: signals see it
    pthread_mutex_unlock(m);
    if (dw) tw_flush();   // [UIT 0361] this thread's own signals, before it sleeps
    __atomic_add_fetch(&tw_n_wait, 1, __ATOMIC_RELAXED);
    int rc = 0;
#ifdef _WIN32
    DWORD ms = INFINITE;
    if (dl) { int64_t v = (left_ns + 999999) / 1000000; ms = v >= (int64_t)INFINITE ? INFINITE - 1 : (DWORD)v; }
    if (!WaitOnAddress(&w->seq, &seen, sizeof seen, ms) && GetLastError() == ERROR_TIMEOUT) rc = ETIMEDOUT;
#else
    struct timespec rel, *rp = NULL;
    if (dl) { rel.tv_sec = left_ns / 1000000000; rel.tv_nsec = left_ns % 1000000000; rp = &rel; }
    if (syscall(SYS_futex, &w->seq, FUTEX_WAIT_PRIVATE, seen, rp, NULL, 0) && errno == ETIMEDOUT) rc = ETIMEDOUT;
#endif
    if (rc == ETIMEDOUT && dl) {   // a Windows wait may end a little before the deadline: say so only once it has passed
        struct timespec now; clock_gettime(CLOCK_REALTIME, &now);
        if (now.tv_sec < dl->tv_sec || (now.tv_sec == dl->tv_sec && now.tv_nsec < dl->tv_nsec)) rc = 0;
    }
    if (rc == ETIMEDOUT) __atomic_add_fetch(&tw_n_timeout, 1, __ATOMIC_RELAXED);
    if (dw) __atomic_sub_fetch(&w->waiters, 1, __ATOMIC_SEQ_CST);
    TW_RELOCK(m);
    return rc;
}
#endif
