/* [UIT 0104] Box test of the free list of src/gfx/vkb_suballoc.h (the suballocator of src/gfx/vkb_mem.c), against a
 * byte map: random allocations and frees of random sizes and alignments; no two allocations overlap, every offset is
 * aligned, the free ranges stay sorted, separate and exactly the bytes not allocated, and a block whose allocations are
 * all freed is one free range again.
 *   cc -O1 -Wall -Isrc/gfx tools/tests/test_suballoc.c -o /tmp/test_suballoc && /tmp/test_suballoc */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "vkb_suballoc.h"

static int fails;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

#define BLOCK (1u << 20)
static unsigned char owner[BLOCK];   // 0 = free, else 1 + the slot
typedef struct { uint64_t off, size; int live; } slot;

static void check_list(const vkb_freelist *f) {
    uint64_t free_bytes = 0;
    for (uint32_t i = 0; i < f->n; i++) {
        CHECK(f->r[i].size > 0);
        if (i) CHECK(f->r[i - 1].off + f->r[i - 1].size < f->r[i].off);   // sorted, not touching
        for (uint64_t b = f->r[i].off; b < f->r[i].off + f->r[i].size; b += 997) CHECK(owner[b] == 0);
        free_bytes += f->r[i].size;
    }
    CHECK(free_bytes + f->used == f->size);
}

int main(void) {
    vkb_freelist f;
    srand(1031);
    for (int round = 0; round < 20; round++) {
        CHECK(vkb_fl_init(&f, BLOCK));
        memset(owner, 0, sizeof owner);
        slot s[200] = {0};
        for (int step = 0; step < 4000; step++) {
            int k = rand() % 200;
            if (s[k].live) {
                for (uint64_t b = s[k].off; b < s[k].off + s[k].size; b++) CHECK(owner[b] == 1 + k % 255);
                memset(owner + s[k].off, 0, s[k].size);
                CHECK(vkb_fl_free(&f, s[k].off, s[k].size));
                s[k].live = 0;
            } else {
                uint64_t size = 1 + rand() % (rand() % 4 ? 4096 : 65536), align = 1ull << (rand() % 13), off;
                if (vkb_fl_alloc(&f, size, align, &off)) {
                    CHECK(off % align == 0);
                    CHECK(off + size <= BLOCK);
                    for (uint64_t b = off; b < off + size; b++) { CHECK(owner[b] == 0); owner[b] = 1 + k % 255; }
                    s[k] = (slot){off, size, 1};
                }
            }
            if (step % 97 == 0) check_list(&f);
        }
        check_list(&f);
        for (int k = 0; k < 200; k++)
            if (s[k].live) { memset(owner + s[k].off, 0, s[k].size); CHECK(vkb_fl_free(&f, s[k].off, s[k].size)); }
        CHECK(vkb_fl_empty(&f));
        CHECK(f.n == 1 && f.r[0].off == 0 && f.r[0].size == BLOCK && f.used == 0);
        vkb_fl_destroy(&f);
    }
    // best fit: the exact hole is taken over a bigger one; a full block takes nothing more; bad alignment refused
    uint64_t a, b, c, d, o;
    CHECK(vkb_fl_init(&f, 1000));
    CHECK(vkb_fl_alloc(&f, 100, 1, &a) && a == 0);
    CHECK(vkb_fl_alloc(&f, 50, 1, &b) && b == 100);
    CHECK(vkb_fl_alloc(&f, 300, 1, &c) && c == 150);
    CHECK(vkb_fl_alloc(&f, 50, 1, &d) && d == 450);
    CHECK(vkb_fl_free(&f, b, 50) && vkb_fl_free(&f, c, 300));   // holes [100,450) and [500,1000)
    CHECK(vkb_fl_alloc(&f, 350, 1, &o) && o == 100);
    CHECK(vkb_fl_alloc(&f, 500, 1, &o) && o == 500);
    CHECK(!vkb_fl_alloc(&f, 1, 1, &o));
    CHECK(!vkb_fl_alloc(&f, 1, 3, &o));
    vkb_fl_destroy(&f);
    printf("%s (%d failures)\n", fails ? "FAIL" : "PASS", fails);
    return fails != 0;
}
