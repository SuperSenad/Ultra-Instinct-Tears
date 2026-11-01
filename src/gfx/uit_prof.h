// [UIT 0019] TOTK_PROFILE=1: where a frame's CPU time goes, one log line every 300 frames ("[profile]").
// Cheap counters (a relaxed atomic add at a few points per submission / present; nothing when the key is not set).
#pragma once
#include <stdatomic.h>
#include <stdint.h>

enum {
    UP_G_SUBMIT,      // game thread: inside nvnQueueSubmitCommands (with the translation thread: reading handles, uniforms, queueing)
    UP_G_UCAP,        //   of which: copying the bound uniforms (and recently written vertex / index data) at the submit call
    UP_G_DEFER,       //   of which: reading the handles of deferred bindings
    UP_G_ACQ_PACE,    // game thread: acquire, sleeping to the frame-rate limit (TOTK_FPS)
    UP_G_ACQ_BEHIND,  // game thread: acquire, waiting for the translation thread / GPU to finish that swapchain image's last present
    UP_G_SYNC,        // game threads: blocked in nvnSyncWait (fences: "translated and GPU done")
    UP_G_SYNC_X,      //   [UIT 0232] of which: until the translation thread had replayed the fenced submission (the rest: the GPU)
    UP_G_DRAIN,       // game threads: waiting for the translation queue to empty (pool / texture changes, finish)
    UP_X_SUBMIT,      // translation thread: replaying submissions (all of submit_exec)
    UP_X_UCAP,        //   of which: applying the captured uniforms
    UP_X_PRESENT,     // translation thread: presents
    UP_X_IDLE,        // translation thread: waiting for work
    UP_X_PROLOGUE,    // recording (vkb_mt_submit): objects made on first use, pool copies
    UP_X_WALK,        //   the walk and the chunks the translation thread records itself
    UP_X_RECWAIT,     //   waiting for the other recording threads' chunks
    UP_X_COMMIT,      //   image moves between chunks + vkQueueSubmit
    UP_R_REC,         // chunk recording, all recording threads together
    UP_N
};
extern _Atomic uint64_t uit_prof_ns[UP_N];
extern int uit_prof_level;   // -1: not read yet
int uit_prof_init(void);
static inline int uit_prof_on(void) { int v = uit_prof_level; return v >= 0 ? v : uit_prof_init(); }
static inline void uit_prof_add(int k, uint64_t ns) {
    if (uit_prof_on()) atomic_fetch_add_explicit(&uit_prof_ns[k], ns, memory_order_relaxed);
}
void uit_prof_present(uint64_t now_ns);   // at each present call (game thread): writes the line every 300 frames
