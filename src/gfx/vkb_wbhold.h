// [UIT 0439] TOTK_VK_WB_HOLD (backend_vk.c): GPU-only copy backs held until a fixed present. The list and its rules,
// without Vulkan or locks (the caller holds its lock), so that tools/tests/test_wbhold.c can check them on the box.
//
// Frames: a submission committed after the present of frame L was handled (mtl_set_frame(L)) is held under label
// L + 1 and goes into game memory at the present of frame L + 3, which first makes sure the GPU has finished frame
// L + 1 (wbh_present_due: the present of frame n applies labels <= n - WBH_FRAMES).
#pragma once
#include <stdint.h>
#include <stdlib.h>

enum { WBH_FRAMES = 2, WBH_RECENT = 120 };
#define WBH_MAX_BYTES (64ull << 20)   // held bytes at most; past that, GPU-only ranges go at retirement as before

typedef struct wb_held { uint64_t frame, lo, len, stamp; struct wb_held *next; uint8_t data[]; } wb_held;
typedef struct { wb_held *head, **tail; uint64_t bytes; } wbh_list;
#define WBH_LIST_INIT(l) {NULL, &(l).head, 0}

static inline int wbh_overlaps(const wbh_list *l, uint64_t lo, uint64_t len) {   // a held range overlaps [lo, +len)
    for (const wb_held *h = l->head; h; h = h->next) if (h->lo < lo + len && lo < h->lo + h->len) return 1;
    return 0;
}
// Held: a range over a held one always (order); a GPU-only one while the list has room. Else it goes now.
static inline int wbh_should_hold(const wbh_list *l, int overlaps, int gpu_only, uint64_t len) {
    return overlaps || (gpu_only && l->bytes + len <= WBH_MAX_BYTES);
}
static inline void wbh_push(wbh_list *l, wb_held *h) {
    h->next = NULL; *l->tail = h; l->tail = &h->next; l->bytes += h->len;
}
// The oldest held range if its label is <= upto (or all), out of the list; NULL: none due.
static inline wb_held *wbh_pop_due(wbh_list *l, uint64_t upto, int all) {
    wb_held *h = l->head;
    if (!h || (!all && h->frame > upto)) return NULL;
    l->head = h->next;
    if (!l->head) l->tail = &l->head;
    l->bytes -= h->len;
    return h;
}
// At the present of frame present: 1 and *upto = the labels due now, when ranges were held lately (last: the label
// of the last one held, 0 = never); 0: nothing to wait for or apply.
static inline int wbh_present_due(uint64_t present, uint64_t last, uint64_t *upto) {
    if (present <= WBH_FRAMES || !last) return 0;
    *upto = present - WBH_FRAMES;
    return last + WBH_RECENT >= *upto;
}
