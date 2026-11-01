// [UIT 0237] TOTK_FRAME_CORE: the real plat_thread_frame() (and FRAME_CORE_STREAK) from src/runtime/platform/windows.c,
// cut out by tools/tests/test_frame_core.py into FRAME_CORE_SRC, run on pthreads with Windows-like shims: thread ids,
// a non-recursive SRWLOCK (a recursive take aborts), OpenThread / CloseHandle bookkeeping (a leaked or double-closed
// handle fails) and SetThreadSelectedCpuSets recording each thread's CPU set.
// Checked: a boot thread that presents first does not keep the core once the real frame thread has presented 60 times
// in a row, and it goes back to the shared cores; two threads presenting in turn never take it; a failed CPU-set call
// leaves the thread on the shared cores for good; placement off or no split: nothing done; 8 threads racing: exactly
// one owner, all others on the shared set. Build and run: python tools/tests/test_frame_core.py (also under TSAN).
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
typedef long LONG; typedef unsigned long ULONG, DWORD; typedef int BOOL; typedef void *HANDLE;
#define FALSE 0
#define THREAD_SET_LIMITED_INFORMATION 0x400
#define THREAD_QUERY_LIMITED_INFORMATION 0x800
typedef struct { pthread_mutex_t m; pthread_t holder; int held; } SRWLOCK;
#define SRWLOCK_INIT { PTHREAD_MUTEX_INITIALIZER, 0, 0 }
static void AcquireSRWLockExclusive(SRWLOCK *l) {
    if (__atomic_load_n(&l->held, __ATOMIC_ACQUIRE) && __atomic_load_n(&l->holder, __ATOMIC_RELAXED) == pthread_self()) { fprintf(stderr, "recursive SRW take\n"); abort(); }
    pthread_mutex_lock(&l->m); __atomic_store_n(&l->holder, pthread_self(), __ATOMIC_RELAXED); __atomic_store_n(&l->held, 1, __ATOMIC_RELEASE);
}
static void ReleaseSRWLockExclusive(SRWLOCK *l) { __atomic_store_n(&l->held, 0, __ATOMIC_RELEASE); __atomic_store_n(&l->holder, (pthread_t)0, __ATOMIC_RELAXED); pthread_mutex_unlock(&l->m); }
static __thread LONG t_id;               // thread id (set by each test thread)
static LONG GetCurrentThreadId(void) { return t_id; }
#define PSEUDO ((HANDLE)(intptr_t)-2)
static HANDLE GetCurrentThread(void) { return PSEUDO; }
static ULONG GetLastError(void) { return 5; }
enum { MAXT = 64 };
static pthread_mutex_t g_tm = PTHREAD_MUTEX_INITIALIZER;
static int g_open[MAXT];                 // open handles per thread id
static int g_set[MAXT];                  // 0 none, 1 shared (g_rids), 2 frame core (g_fids), 3 other
static int g_fail_set;                   // SetThreadSelectedCpuSets fails for this thread id
typedef struct { LONG tid; int open; } H;
static HANDLE OpenThread(DWORD acc, BOOL inh, DWORD tid) {
    (void)acc; (void)inh;
    H *h = malloc(sizeof *h); h->tid = (LONG)tid; h->open = 1;
    pthread_mutex_lock(&g_tm); g_open[tid]++; pthread_mutex_unlock(&g_tm);
    return h;
}
static int g_bad;
static BOOL CloseHandle(HANDLE x) {
    H *h = x;
    pthread_mutex_lock(&g_tm);
    if (!h->open) g_bad++; else { h->open = 0; g_open[h->tid]--; }
    pthread_mutex_unlock(&g_tm);
    free(h); return 1;
}
static int g_nf = 2, g_nr = 14, g_np = 16, g_place = 1;
static ULONG g_fids[8] = {270, 271}, g_rids[256];
static int place_mode(void) { return g_place; }
static int cpu_sets(void) { return g_np; }
static BOOL set_cpus(HANDLE x, const ULONG *ids, ULONG n) {
    LONG tid = x == PSEUDO ? t_id : ((H *)x)->tid;
    if (x != PSEUDO && !((H *)x)->open) { g_bad++; return 0; }
    if (tid == g_fail_set) return 0;
    pthread_mutex_lock(&g_tm);
    g_set[tid] = ids == g_fids && n == (ULONG)g_nf ? 2 : ids == g_rids && n == (ULONG)g_nr ? 1 : 3;
    pthread_mutex_unlock(&g_tm);
    return 1;
}
static BOOL (*g_cpuset_set)(HANDLE, const ULONG *, ULONG) = set_cpus;
#include FRAME_CORE_SRC

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)
static int setof(LONG t) { pthread_mutex_lock(&g_tm); int v = g_set[t]; pthread_mutex_unlock(&g_tm); return v; }

typedef struct { LONG id; int n; int ret_last; pthread_barrier_t *b; } Job;
static void *present_n(void *p) {          // one thread presenting n times
    Job *j = p; t_id = j->id;
    if (j->b) pthread_barrier_wait(j->b);
    for (int i = 0; i < j->n; i++) j->ret_last = plat_thread_frame();
    return NULL;
}
static void run(LONG id, int n, int *ret) {
    Job j = {id, n, 0, NULL}; pthread_t t;
    pthread_create(&t, NULL, present_n, &j); pthread_join(t, NULL);
    if (ret) *ret = j.ret_last;
}
// two threads presenting in turn, strictly alternating (a semaphore-like handshake)
static pthread_mutex_t alt_m = PTHREAD_MUTEX_INITIALIZER; static pthread_cond_t alt_c = PTHREAD_COND_INITIALIZER;
static LONG alt_turn;
typedef struct { LONG id, other; int n; } Alt;
static void *alt(void *p) {
    Alt *a = p; t_id = a->id;
    for (int i = 0; i < a->n; i++) {
        pthread_mutex_lock(&alt_m);
        while (alt_turn != a->id) pthread_cond_wait(&alt_c, &alt_m);
        pthread_mutex_unlock(&alt_m);
        plat_thread_frame();
        pthread_mutex_lock(&alt_m); alt_turn = a->other; pthread_cond_broadcast(&alt_c); pthread_mutex_unlock(&alt_m);
    }
    return NULL;
}
int main(int argc, char **argv) {
    const char *mode = argc > 1 ? argv[1] : "";
    int r = -1;
    if (!strcmp(mode, "off")) {               // placement off: nothing, ever
        g_place = 0; unsetenv("TOTK_VK_PCORES");
        run(1, 500, &r); CHECK(r == 0 && setof(1) == 0 && g_open[1] == 0);
    } else if (!strcmp(mode, "nosplit")) {    // fewer than 4 P-cores (g_nr 0): nothing
        g_nr = 0;
        run(1, 500, &r); CHECK(r == 0 && setof(1) == 0);
    } else if (!strcmp(mode, "fail")) {       // the CPU-set call fails: shared for good, no handle leaked
        g_fail_set = 1;
        run(1, 500, &r); CHECK(r == 0 && setof(1) == 0 && g_open[1] == 0);
    } else if (!strcmp(mode, "race")) {       // 8 threads, each presenting 200 times at once: interleaved, so streaks
        pthread_barrier_t b; pthread_barrier_init(&b, NULL, 8);   // break; whoever gets one: one owner at the end
        Job j[8]; pthread_t t[8];
        for (int i = 0; i < 8; i++) { j[i] = (Job){10 + i, 200, 0, &b}; pthread_create(&t[i], NULL, present_n, &j[i]); }
        for (int i = 0; i < 8; i++) pthread_join(t[i], NULL);
        int owners = 0, open = 0;
        for (int i = 0; i < 8; i++) { owners += setof(10 + i) == 2; open += g_open[10 + i]; CHECK(setof(10 + i) != 3); }
        CHECK(owners <= 1 && open == owners);
        run(30, 100, &r);                     // afterwards a steady frame thread takes it; all of those back on shared
        CHECK(r == 2 && setof(30) == 2 && g_open[30] == 1);
        for (int i = 0; i < 8; i++) { CHECK(setof(10 + i) != 2); CHECK(g_open[10 + i] == 0); }
    } else {
        run(1, 59, &r);                       // the boot thread: 59 presents, not yet
        CHECK(r == 0 && setof(1) == 0);
        run(1, 1, &r);                        // a new OS thread reusing that id: its own streak (TLS) starts again
        run(2, 60, &r);                       // the boot / loading thread: 60 in a row: it takes the core
        CHECK(r == 2 && setof(2) == 2 && g_open[2] == 1);
        run(3, 59, &r);                       // the game's frame thread, 59 in a row: not yet
        CHECK(r == 0 && setof(2) == 2 && setof(3) == 0);
        run(3, 1, &r);                        // its 60th (new TLS: the streak restarts, so still 1 here)
        CHECK(r == 0);
        run(3, 60, &r);                       // 60 in a row: the core moves, the boot thread back on the shared cores
        CHECK(r == 2 && setof(3) == 2 && setof(2) == 1 && g_open[2] == 0 && g_open[3] == 1);
        // two threads presenting in turn (2 and 4), 500 each: the streak never reaches 60: the owner (3) keeps it
        Alt a = {4, 5, 500}, b = {5, 4, 500}; pthread_t ta, tb; alt_turn = 4;
        pthread_create(&ta, NULL, alt, &a); pthread_create(&tb, NULL, alt, &b);
        pthread_join(ta, NULL); pthread_join(tb, NULL);
        CHECK(setof(3) == 2 && setof(4) == 0 && setof(5) == 0 && g_open[4] == 0 && g_open[5] == 0);
    }
    CHECK(g_bad == 0);
    printf("%s\n", fails ? "FAILED" : "frame core: all ok");
    return fails != 0;
}
