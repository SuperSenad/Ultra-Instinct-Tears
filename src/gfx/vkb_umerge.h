// [UIT 0141] The uniform capture's merge (vkb_ucap.c, game thread, in the submit call), here so the box test
// (tools/tests/test_umerge.c) runs the same code: sorted ranges (vkb_rsort.h) with overlapping / touching ones merged
// (up to merge_max bytes) and those outside every imported pool dropped, in place; returns how many are left.
// The pool lookups run under one hold of the pool table's read lock per hold_max of them (hold / release / in, the
// backend's): before 0141 each lookup took and released the lock itself (mtl_addr), thousands of shared acquires per
// submission on the lock the recording threads take for their draws too. The answers are the same: the table may
// change between two holds as it could between two mtl_addr calls.
#pragma once
#include <stdint.h>
#include "vkb_rsort.h"

static inline int ucap_merge(ucap_rng *v, int nv, uint64_t merge_max, int hold_max, int (*hold)(void), void (*release)(void),
                             int (*in)(uint64_t lo, uint64_t len)) {
    int k = -1, looks = 0;
    if (!nv || !hold()) return 0;   // not running: every range is outside
    for (int i = 0; i < nv; i++) {
        if (looks >= hold_max) { release(); hold(); looks = 0; }
        if (k >= 0 && v[i].lo <= v[k].hi) {
            if (v[i].hi <= v[k].hi) { v[k].geo |= v[i].geo; continue; }
            if (v[i].hi - v[k].lo <= merge_max && (looks++, in(v[k].lo, v[i].hi - v[k].lo))) { v[k].hi = v[i].hi; v[k].geo |= v[i].geo; continue; }
        }
        looks++;
        if (!in(v[i].lo, v[i].hi - v[i].lo)) continue;
        v[++k] = v[i];
    }
    release();
    return k + 1;
}
