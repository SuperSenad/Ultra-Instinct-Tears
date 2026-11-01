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
typedef struct { pthread_cond_t cv; uint32_t seq; } thr_wait_t;

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
static inline void tw_init(thr_wait_t *w) { pthread_cond_init(&w->cv, NULL); w->seq = 0; }
static inline void tw_destroy(thr_wait_t *w) { pthread_cond_destroy(&w->cv); }

static inline void tw_signal(thr_wait_t *w) {   // with the lock held
    if (!tw_on()) { pthread_cond_signal(&w->cv); return; }
    __atomic_add_fetch(&w->seq, 1, __ATOMIC_SEQ_CST);
    __atomic_add_fetch(&tw_n_wake, 1, __ATOMIC_RELAXED);
#ifdef _WIN32
    WakeByAddressSingle(&w->seq);
#else
    syscall(SYS_futex, &w->seq, FUTEX_WAKE_PRIVATE, 1, NULL, NULL, 0);
#endif
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
    pthread_mutex_unlock(m);
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
    TW_RELOCK(m);
    return rc;
}
#endif
