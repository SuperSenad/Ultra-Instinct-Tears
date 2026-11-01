// [UIT 0241] TOTK_VK_CHUNK_RAMP (default 200; 0 or off = as before): with TOTK_VK_EARLY_STREAM the first chunks of a
// submission are cut smaller, so the GPU gets its first part sooner (backend_vk.c walk_op; box test
// tools/tests/test_chunk_ramp.c).
// Why: with the stream (UIT 0236) the GPU starts a submission once its first chunks are recorded, in order, and the walk
// is over. A chunk is cut inside a long pass at TOTK_VK_CHUNK_SPLIT (600) draws, and the first chunks of a frame are its
// heaviest: the 2026-10-31 20:48 log at Lookout Landing shows a largest chunk of 4.3-5.3 ms (561-599 draws, 8-9 us a
// draw against 4.6 on average: shadow and G-buffer draws, each with new descriptor sets). With such a chunk first, the
// GPU waited the prologue + that chunk (~1.2 + ~5 ms) before it began the frame's ~11 ms; the frame's fence then
// came at ~17 ms (the t242 "on" logs: 16.7-18.3 ms frames).
// With the ramp, the first 6 chunks of a submission (one for each recording thread) are cut at 200 draws, the next at
// 400, then 600 as before. The first part can go as soon as the first 200-draw chunk is recorded (~1-1.7 ms), and the
// next ones are recorded side by side, so they are ready about as fast as the GPU uses them. Costs: about 4 more
// chunks a submission (each a pass restart on the same targets, load + store, and one barrier: tens of microseconds of
// GPU in all), and slightly more command buffers to submit.
// Render target changes still cut at 400 draws as before; chunks are never cut smaller than 100 draws.
#ifndef VKB_CHUNK_RAMP_H
#define VKB_CHUNK_RAMP_H
#include <stdint.h>
#include <stdlib.h>

enum { VKB_RAMP_DEFAULT = 200, VKB_RAMP_MIN = 100 };

// TOTK_VK_CHUNK_RAMP: unset / empty / "-" -> the default; "0" / "off" -> 0 (off); a number -> clamped to >= 100
static inline int vkb_ramp_parse(const char *e) {
    if (!e || !*e || (*e == '-' && !e[1])) return VKB_RAMP_DEFAULT;
    if ((e[0] == 'o' || e[0] == 'O') && (e[1] == 'f' || e[1] == 'F')) return 0;
    char *end;
    long v = strtol(e, &end, 10);
    if (end == e || *end) return VKB_RAMP_DEFAULT;   // not a number: the default
    if (v <= 0) return 0;
    return v < VKB_RAMP_MIN ? VKB_RAMP_MIN : v > 100000 ? 100000 : (int)v;
}
// The draw count at which chunk k is cut inside a pass. split: TOTK_VK_CHUNK_SPLIT (0: never inside a pass, ramp too);
// ramp: as parsed (0: off, every chunk at split, as before). The first VKB_RAMP_FLAT chunks (one for each recording
// thread) are cut at ramp, then ramp doubles each chunk until split: 200 x 6, 400, 600, 600, ... The flat start matters:
// all chunks are recorded at once and sent in order, so a 400-draw second chunk (3.4 ms at 8.6 us a draw) would keep the
// GPU waiting after the first (the box model in test_chunk_ramp.c: 200, 400, 600 gave 0.3 ms; 200 x 6 gives ~1.7 ms).
enum { VKB_RAMP_FLAT = 6 };
static inline uint64_t vkb_chunk_limit(int k, int split, int ramp) {
    if (split <= 0) return 0;
    if (ramp <= 0 || ramp >= split || k < 0) return (uint64_t)split;
    int sh = k < VKB_RAMP_FLAT ? 0 : k - VKB_RAMP_FLAT + 1;
    uint64_t l = (uint64_t)ramp << (sh < 16 ? sh : 16);
    return l < (uint64_t)split ? l : (uint64_t)split;
}
// The stream's part limit with the ramp: each of the flat chunks may go as its own part, so the ramp gets that many more
// parts (8 + 6 = 14 vkQueueSubmit2 calls at most a submission, on the submit thread). Without them the small chunks use
// up the parts early and the rest of the frame waits to go as the last part once all of it is recorded (the box model:
// 0.2 ms slower on a frame with even draw costs; with them 0.9 ms faster). parts: early_parts(); stream: the stream is on.
static inline int vkb_ramp_parts(int parts, int stream, int ramp) {
    return stream && ramp > 0 && parts > 1 ? parts + VKB_RAMP_FLAT : parts;
}
#endif
