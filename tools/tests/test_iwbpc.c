// tools/tests/test_iwbpc.c: the waiting ranges of TOTK_IWB_POOL_COPY (src/gfx/vkb_iwbpc.h).
//   cc -O1 -Wall -Isrc/gfx tools/tests/test_iwbpc.c -o /tmp/test_iwbpc && /tmp/test_iwbpc
#include <stdio.h>
#include "vkb_iwbpc.h"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)
static uint64_t got_off[16], got_len[16]; static int ngot;
static void rec(void *ctx, uint64_t off, uint64_t n) { (void)ctx; got_off[ngot] = off; got_len[ngot] = n; ngot++; }
static int alive_hi(void *ctx, uintptr_t lo, uint64_t len) { (void)len; return lo >= *(uintptr_t *)ctx; }

int main(void) {
    iwbpc_list l = {.max = 4};
    CHECK(iwbpc_add(&l, 0x10000, 0x3000));
    CHECK(iwbpc_add(&l, 0x10000, 0x3000) && l.n == 1);   // the same image again: one entry
    CHECK(iwbpc_add(&l, 0x50000, 0x1000));
    CHECK(iwbpc_add(&l, 0x90000, 0x2000));   // in another pool
    ngot = 0;
    uint64_t s = iwbpc_take(&l, 0x10000, 0x60000, rec, NULL);   // pool A
    CHECK(ngot == 2 && got_off[0] == 0 && got_len[0] == 0x3000 && got_off[1] == 0x40000 && got_len[1] == 0x1000);
    CHECK(iwbpc_add(&l, 0x50000, 0x1000));   // copied back again after the take: must stay for the next sync
    iwbpc_drop(&l, 0x10000, 0x60000, s);
    CHECK(l.n == 2);
    ngot = 0; iwbpc_take(&l, 0x10000, 0x60000, rec, NULL);
    CHECK(ngot == 1 && got_off[0] == 0x40000);
    ngot = 0; iwbpc_take(&l, 0x8f000, 0x91000, rec, NULL);   // clipped to the pool
    CHECK(ngot == 1 && got_off[0] == 0x1000 && got_len[0] == 0x1000);
    CHECK(iwbpc_add(&l, 0xa0000, 0x1000) && iwbpc_add(&l, 0xb0000, 0x1000));
    CHECK(!iwbpc_add(&l, 0xc0000, 0x1000));   // full
    uintptr_t keep = 0x90000;
    iwbpc_purge(&l, alive_hi, &keep);
    CHECK(l.n == 3);
    CHECK(iwbpc_add(&l, 0xc0000, 0x1000));
    free(l.e);
    printf(fails ? "test_iwbpc: %d failure(s)\n" : "test_iwbpc: all passed\n", fails);
    return fails != 0;
}
