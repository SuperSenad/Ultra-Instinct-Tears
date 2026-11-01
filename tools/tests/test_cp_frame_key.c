// [UIT t263] uit_crit.h uit_cp_is_frame: only the thread that acquired or presented last counts as the frame thread;
// a thread that presented once before (boot, loading) and later waits on fences counts as another game thread.
// cc -O1 -Wall -Werror -pthread -Isrc/gfx tools/tests/test_cp_frame_key.c -o /tmp/t/cpk && /tmp/t/cpk
#include <pthread.h>
#include "uit_crit.h"
_Atomic uint64_t uit_cp_ns[CPT_N][CP_K], uit_cp_n[CPT_N][CP_K], uit_dr_ns[DR_N], uit_dr_n[DR_N], uit_cx[CX_N];
__thread int uit_cp_frame_thread;
_Atomic uintptr_t uit_cp_frame_key;
int uit_cp_level = 1;
int uit_cp_init(void) { return 1; }
static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)
static void *boot(void *a) {          // presents once, then (after the frame thread takes over) waits on a fence
    pthread_barrier_t *b = a;
    uit_cp_frame_thread = 1; uit_cp_frame_mark();
    uit_cp_add(CP_SYNC_GPU, 100);   // still the frame thread: FRAME
    pthread_barrier_wait(b);        // the frame thread presents
    pthread_barrier_wait(b);
    uit_cp_add(CP_SYNC_GPU, 1000);  // no longer the frame thread: OTHER (was FRAME: the flag stayed set)
    return 0;
}
int main(void) {
    pthread_barrier_t b; pthread_barrier_init(&b, 0, 2);
    pthread_t t; pthread_create(&t, 0, boot, &b);
    pthread_barrier_wait(&b);
    uit_cp_frame_thread = 1; uit_cp_frame_mark();
    pthread_barrier_wait(&b);
    pthread_join(t, 0);
    uit_cp_add(CP_ACQ_PACE, 7);
    CHECK(uit_cp_ns[CPT_FRAME][CP_SYNC_GPU] == 100 && uit_cp_ns[CPT_OTHER][CP_SYNC_GPU] == 1000);
    CHECK(uit_cp_ns[CPT_FRAME][CP_ACQ_PACE] == 7);
    uit_cp_frame_thread = 0;                       // a thread that never acquired or presented: OTHER
    uit_cp_add(CP_DRAIN, 5);
    CHECK(uit_cp_ns[CPT_OTHER][CP_DRAIN] == 5);
    printf("%s\n", fails ? "FAILED" : "cp frame key: all ok");
    return fails != 0;
}
