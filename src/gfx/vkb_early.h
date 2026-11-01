// [native-1031] TOTK_VK_EARLY_PARTS: the GPU starts on a submission before all of it is recorded (backend_vk.c
// vkb_mt_submit, early_try; box test tools/tests/test_early.c).
// A submission recorded by several threads (backend_vk.c "recording threads") went to the GPU as one vkQueueSubmit2
// once every chunk was recorded: the game's own fence (nvnQueueFenceSync / nvnSyncWait) then waited for the whole
// recording AND the whole GPU time one after the other (2026-10-30 longplay: ~8.6 ms translation + ~9.6 ms GPU).
// With parts = 2 or 3, once the walk has cut every chunk, the first finished chunks are handed to the GPU early
// (in order, without signalling anything), and the last part signals the submission's fence as before. So:
//   - the game's fences mean exactly what they meant: the timeline value is signalled by the last part only, after
//     all earlier parts in queue order; event values, counters, copy-backs (pool writes, images) still land at retire
//   - every chunk is submitted once, in order, only once recorded; a part never holds a chunk not yet recorded
//   - the prologue (work recording threads put before every chunk: uploads, pool copies) is cut into segments: the
//     segment open when a part is sent goes in front of that part and a new one opens (backend_vk.c, under g_pro_mu)
//     for the chunks after it. A chunk in a part finished recording before that part was cut, so all the prologue
//     work it asked for is in a segment already in front of it; a pool is copied at most once per submission
//     (sync_sub), so a later segment never rewrites a pool an earlier part used
//   - the slot's resources (command pools, upload memory, descriptors, copy-back memory) are per submission as
//     before and reused only after the slot's fence: the last part, so after every part
// This header holds the plan (how far to send now); the protocol is modelled by the test.
#ifndef VKB_EARLY_H
#define VKB_EARLY_H
#include <stdint.h>

enum { VKB_EARLY_MIN_DRAWS = 200 };   // no part smaller than this (a submit costs ~0.05 ms)

// How many chunks [0, return) may be sent now. next: chunks already sent; nch: chunks cut (the walk is over);
// done[k]: chunk k is recorded; draws[k]: its draws; total: draws in the submission; parts: 1..3 (1 = as before);
// sent: parts already sent early. Returns next when nothing is to be sent now.
static inline int vkb_early_plan(int next, int nch, const unsigned char *done, const uint64_t *draws, uint64_t total,
                                 int parts, int sent, uint64_t min_draws) {
    if (parts <= 1 || sent >= parts - 1 || next >= nch) return next;
    uint64_t before = 0;
    for (int k = 0; k < next; k++) before += draws[k];
    int p = next;
    uint64_t cum = before;
    while (p < nch && done[p]) cum += draws[p++];
    if (p >= nch) return next;   // all recorded: the last part takes the rest (it signals the fence)
    uint64_t want = total * (uint64_t)(sent + 1) / (uint64_t)parts;
    if (cum < want || cum - before < min_draws) return next;
    return p;
}
static inline int vkb_early_parts_parse(const char *e) {   // TOTK_VK_EARLY_PARTS: 1 (unset, bad) .. 3
    if (!e || !*e || *e == '-') return 1;
    int v = 0;
    for (; *e >= '0' && *e <= '9'; e++) v = v * 10 + (*e - '0');
    if (*e) return 1;
    return v < 1 ? 1 : v > 3 ? 3 : v;
}

// ---- [UIT 0236] TOTK_VK_EARLY_STREAM (default on): finished chunks stream to the GPU as they are recorded
// Why: the 00e43fa user logs (2026-10-31 20:26, all of 0230-0234 on) spend 5-7 ms of a 17-22 ms frame in the game's
// fence wait in the heavy areas: the submission went to the GPU only once every chunk was recorded (~7.7 ms after the
// game handed it over), then the GPU took 9-11 ms; the two ran one after the other. TOTK_VK_EARLY_PARTS (2 or 3) was
// meant to overlap them but sent a part only once half (a third) of the draws were in a finished prefix while another
// chunk was still recording, which the 21:08 log shows happening 0.0-0.8 times a submission (chunks are taken by 6
// threads at once and finish together), and only the translation thread ever looked.
// With the stream:
//   - a part goes as soon as the finished prefix since the last part has min_draws draws (no fraction of the total);
//     at most VKB_EARLY_STREAM_MAX parts a submission, the last one (the fence) included
//   - any recording thread may send it, right after recording a chunk (a try-lock: a thread never waits for another's
//     send; a skipped chance goes with a later part), and the translation thread while it waits for the others
//   - with TOTK_VK_ASYNC_SUBMIT (default) the part is handed to the submit thread as an item of the fence the
//     submission is about to take (no signal, no retire job, no "submitted" step): the queue keeps items of the same
//     fence in push order, so parts go to the driver in order, after every earlier submission and before the last part
//     (vkb_subq.h); no thread waits for the driver
// Everything else is the TOTK_VK_EARLY_PARTS protocol above: in order, only recorded chunks, prologue segments in front,
// the fence on the last part only. TOTK_VK_EARLY_PARTS = 2 or 3 set explicitly keeps the old plan.
enum { VKB_EARLY_STREAM_MAX = 8 };
static inline int vkb_early_stream_plan(int next, int nch, const unsigned char *done, const uint64_t *draws, int maxparts,
                                        int sent, uint64_t min_draws) {
    if (maxparts <= 1 || sent >= maxparts - 1 || next >= nch) return next;
    int p = next;
    uint64_t cum = 0;
    while (p < nch && done[p]) cum += draws[p++];
    if (p >= nch) return next;   // all recorded: the last part takes the rest
    if (cum < min_draws) return next;
    return p;
}
static inline int vkb_early_stream_parse(const char *e) {   // TOTK_VK_EARLY_STREAM: on unless "0"
    return !(e && e[0] == '0' && !e[1]);
}
// The plan in use: parts (1 = one submission as before) and whether it is the stream. TOTK_VK_EARLY_PARTS 2 or 3 (set
// explicitly) wins; else TOTK_VK_EARLY_STREAM (on unless 0) decides.
static inline int vkb_early_mode(const char *parts_env, const char *stream_env, int *stream) {
    int parts = vkb_early_parts_parse(parts_env);
    *stream = 0;
    if (parts > 1) return parts;
    if (vkb_early_stream_parse(stream_env)) { *stream = 1; return VKB_EARLY_STREAM_MAX; }
    return 1;
}
#endif
