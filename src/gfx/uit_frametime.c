// [UIT 0301] the frame-time ring (src/gfx/uit_frametime.h)
#include "uit_frametime.h"

#include <stdlib.h>
#include <string.h>

_Atomic int g_uit_ft_users;
_Atomic uint64_t g_uit_ft_head;
float g_uit_ft_ring[UIT_FT_RING];
_Atomic uint64_t g_uit_bg_period_ns;   // [UIT 0303]

void uit_ft_use(int on) { atomic_fetch_add_explicit(&g_uit_ft_users, on ? 1 : -1, memory_order_relaxed); }

int uit_ft_take(uint64_t *since, float *out, int max, uint64_t *lost) {
    uint64_t h = atomic_load_explicit(&g_uit_ft_head, memory_order_acquire), from = *since;
    if (lost) *lost = 0;
    if (!from || from > h) from = h > (uint64_t)max ? h - (uint64_t)max : 0;
    if (h - from > UIT_FT_SAFE) {   // fell behind: what was overwritten (or is being written now) is gone
        if (lost && *since) *lost = h - from - UIT_FT_SAFE;
        from = h - UIT_FT_SAFE;
    }
    if (h - from > (uint64_t)max) {
        if (lost && *since) *lost += h - from - (uint64_t)max;
        from = h - (uint64_t)max;
    }
    int n = 0;
    for (uint64_t i = from; i < h; i++) out[n++] = g_uit_ft_ring[i & (UIT_FT_RING - 1)];
    // the game thread may have lapped the copy (only if this thread stalled for a whole ring): those are dropped
    atomic_thread_fence(memory_order_acquire);
    uint64_t h2 = atomic_load_explicit(&g_uit_ft_head, memory_order_relaxed);
    if (h2 - from > UIT_FT_SAFE) {
        uint64_t bad = h2 - from - UIT_FT_SAFE;
        if (bad > (uint64_t)n) bad = (uint64_t)n;
        memmove(out, out + bad, (size_t)(n - (int)bad) * sizeof *out);
        n -= (int)bad;
        if (lost && *since) *lost += bad;
    }
    *since = h;
    return n;
}

int uit_ft_recent(float span_ms, float *out, int max) {
    uint64_t h = atomic_load_explicit(&g_uit_ft_head, memory_order_acquire), avail = h < UIT_FT_SAFE ? h : UIT_FT_SAFE;
    int k = 0;
    float sum = 0;
    while ((uint64_t)k < avail && k < max && sum < span_ms) sum += g_uit_ft_ring[(h - 1 - (uint64_t)k++) & (UIT_FT_RING - 1)];
    for (int i = 0; i < k; i++) out[i] = g_uit_ft_ring[(h - (uint64_t)k + (uint64_t)i) & (UIT_FT_RING - 1)];
    return k;
}

// [UIT 0303] the background cap (window thread)
static int bg_fps = 30, bg_away;
static void bg_apply(void) {
    uint64_t p = bg_away && bg_fps > 0 ? 1000000000ull / (uint64_t)bg_fps : 0;
    atomic_store_explicit(&g_uit_bg_period_ns, p, memory_order_relaxed);
}
void uit_bg_set_fps(int fps) { bg_fps = fps < 0 ? 0 : fps > 240 ? 240 : fps; bg_apply(); }
int uit_bg_fps(void) { return bg_fps; }
void uit_bg_set_away(int away) { bg_away = !!away; bg_apply(); }
int uit_bg_away(void) { return bg_away; }
int uit_bg_parse(const char *v) {
    if (!v || !*v || v[0] == '-') return 30;
    int f = atoi(v);
    return f < 0 ? 0 : f > 240 ? 240 : f;
}
