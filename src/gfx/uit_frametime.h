// [UIT 0301] The game's frame times (present to present, ms) for the in-game overlay's frame-time graph and
// [UIT 0302] the benchmark; [UIT 0303] the frame rate cap while the window is in the background.
// Game thread (nvnQueuePresentTexture): uit_ft_push. With nothing reading (graph off, no benchmark) that is one relaxed
// atomic load a frame. While on: one store into a fixed ring and one release store of the count. No locks, no
// allocation; the window thread copies what it needs (uit_ft_copy). The background cap is one relaxed load a frame at
// acquire (hle_nvn.c), 0 while the window has the focus.
#pragma once
#include <stdatomic.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define UIT_FT_RING 4096u   // a power of two: 34 s at 120 FPS (the readers take theirs every 100 ms at most)
#define UIT_FT_SAFE (UIT_FT_RING - 64u)   // what a reader copies at most: the slots the game thread is not about to write
extern _Atomic int g_uit_ft_users;           // readers (graph, benchmark): 0 = nothing is recorded
extern _Atomic uint64_t g_uit_ft_head;       // frames recorded so far; the newest is at (head - 1) % UIT_FT_RING
extern float g_uit_ft_ring[UIT_FT_RING];
extern _Atomic uint64_t g_uit_bg_period_ns;  // [UIT 0303] 0: no cap (the window has the focus, or the cap is Off)

static inline void uit_ft_push(float ms) {
    if (!atomic_load_explicit(&g_uit_ft_users, memory_order_relaxed)) return;
    uint64_t h = atomic_load_explicit(&g_uit_ft_head, memory_order_relaxed);
    g_uit_ft_ring[h & (UIT_FT_RING - 1)] = ms;
    atomic_store_explicit(&g_uit_ft_head, h + 1, memory_order_release);
}
// A reader starts / stops (the game thread records while there is at least one).
void uit_ft_use(int on);
// The frames after *since (oldest first), at most max of the newest: copied to out; *since moves to the current count.
// *since = 0 the first time takes the newest max. Returns how many were copied; *lost (may be NULL): frames that were
// overwritten before this read (more than UIT_FT_RING behind).
int uit_ft_take(uint64_t *since, float *out, int max, uint64_t *lost);
// The newest frames covering at least span_ms (at most max), oldest first: the graph's window.
int uit_ft_recent(float span_ms, float *out, int max);

// [UIT 0303] The frame rate cap in the background (window thread). TOTK_BG_FPS: unset / "-" = 30, 0 = Off (no cap),
// else that many FPS (at most 240). The cap is on only while away (the window lost the focus or is minimised); back
// in front it is 0 at once, so the next frame is paced as before.
int uit_bg_parse(const char *v);
void uit_bg_set_fps(int fps);
int uit_bg_fps(void);
void uit_bg_set_away(int away);
int uit_bg_away(void);

#ifdef __cplusplus
}
#endif
