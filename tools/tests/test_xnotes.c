// [UIT 0330] vkb_xnotes.h: what a shader's reflection says the translation left out.
//   cc -O2 -Wall -Isrc/gfx tools/tests/test_xnotes.c -o /tmp/txn && /tmp/txn
#include <stdio.h>
#include "vkb_xnotes.h"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

int main(void) {
    // clean: two c2 textures (one with a packed sampler slot), no notes about removals
    vkb_xnotes a = vkb_xnotes_scan("{\"key\":\"k\",\"notes\":[\"translator: x\"],\"textures\":[{\"binding\":3,\"cbuf_slot\":2,"
                                   "\"handle\":90,\"array\":1},{\"binding\":4,\"cbuf_slot\":196610,\"handle\":91,\"array\":1}],\"images\":[]}");
    CHECK(!vkb_xnotes_any(a));
    // two removed texture reads, one global memory access, a c6 handle and an array of 32
    vkb_xnotes b = vkb_xnotes_scan("{\"notes\":[\"translator: Failed to find handle source for bindless access of type \\\"sampler2D\\\".\","
                                   "\"translator: Failed to find handle source for bindless access of type \\\"sampler2DArray\\\".\","
                                   "\"translator: Failed to find storage buffer for global memory operation \\\"Load\\\".\"],"
                                   "\"textures\":[{\"binding\":3,\"cbuf_slot\":6,\"handle\":4,\"array\":1},"
                                   "{\"binding\":4,\"cbuf_slot\":2,\"handle\":90,\"array\":32}],\"images\":[{\"cbuf_slot\":9,\"array\":4}]}");
    CHECK(b.tex_removed == 2 && b.gmem_zero == 1 && b.tex_other_cb == 1 && b.tex_arrays == 1);
    // no textures at all, a failed storage buffer reservation
    vkb_xnotes c = vkb_xnotes_scan("{\"notes\":[\"translator: Failed to reserve storage buffer for global memory operation \\\"Store\\\".\"]}");
    CHECK(c.gmem_zero == 1 && c.tex_removed == 0 && vkb_xnotes_any(c));
    // the default bound-texture slot (-1) is c2
    vkb_xnotes d = vkb_xnotes_scan("{\"textures\":[{\"binding\":3,\"cbuf_slot\":-1,\"handle\":90,\"array\":1}]}");
    CHECK(!vkb_xnotes_any(d));
    printf(fails ? "test_xnotes: %d FAILED\n" : "test_xnotes: ok\n", fails);
    return fails != 0;
}
