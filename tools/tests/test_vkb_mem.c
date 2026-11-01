/* [UIT 0075] The pure rules of the texture memory: the 85% budget check (vkb_mem.h) and when an aside texture goes
 * (vkb_aside.h, TOTK_TEX_RELEASE_SPREAD). A model of vkb_tex.c's aside list: releases and the purge after each one.
 *   cc -O2 -Isrc/gfx -I<Vulkan-Headers>/include tools/tests/test_vkb_mem.c -o /tmp/t/vmem && /tmp/t/vmem */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "vkb_mem.h"
#include "vkb_aside.h"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

enum { FRAMES = 120 };
static unsigned long long rel[VKB_ASIDE_SPREAD];
static int n;
// vkb_tex.c: purge (aside_purge) with this cap; returns how many went
static int purge(int cap, unsigned long long now) {
    int budget = cap == VKB_ASIDE_SPREAD ? VKB_ASIDE_PER_CALL : -1, went = 0;
    while (vkb_aside_go(n, cap, n && rel[0] + FRAMES < now, &budget)) { memmove(rel, rel + 1, (size_t)--n * sizeof rel[0]); went++; }
    return went;
}
static int release(int cap, unsigned long long now) {   // vkb_tex_release: one more aside, then the purge
    int went = 0;
    if (n >= cap) { memmove(rel, rel + 1, (size_t)--n * sizeof rel[0]); went++; }
    rel[n++] = now;
    return went + purge(cap, now);
}

int main(void) {
    const unsigned long long G = 1ull << 30;
    CHECK(vkb_mem_room(0, 8 * G, 512ull << 20));
    CHECK(vkb_mem_room(6 * G, 8 * G, 512ull << 20));            // 6.5 of 8 GiB: 81%
    CHECK(!vkb_mem_room(6 * G + (600ull << 20), 8 * G, 512ull << 20));   // past 85%
    CHECK(!vkb_mem_room(0, 0, 1));                               // no budget known: nothing in the background
    CHECK(vkb_mem_room(85, 100, 0) && !vkb_mem_room(86, 100, 0));

    // old rule: 600 textures released in one frame, then 121 frames on: every one goes in the same call
    n = 0;
    int most = 0;
    for (int i = 0; i < 600; i++) release(VKB_ASIDE_OLD, 1);
    CHECK(n == VKB_ASIDE_OLD - 1);   // as before: the purge after a release at the cap drops one more
    most = release(VKB_ASIDE_OLD, 1 + FRAMES + 1);
    CHECK(most == VKB_ASIDE_OLD - 1);   // the hitch: 511 images destroyed at once
    CHECK(n == 1);

    // spread: the same burst; at most one per release past the cap and two old ones per call
    n = 0; most = 0;
    for (int i = 0; i < 600; i++) { int w = release(VKB_ASIDE_SPREAD, 1); if (w > most) most = w; }
    CHECK(n == 600 && most == 0);
    for (int i = 0; i < 3000; i++) { int w = release(VKB_ASIDE_SPREAD, 2); if (w > most) most = w; }
    CHECK(n == VKB_ASIDE_SPREAD && most == 1);
    unsigned long long now = 1 + FRAMES + 1;
    int calls = 0, still_old = 1;
    while (still_old && calls < 10000) {
        int w = purge(VKB_ASIDE_SPREAD, now);
        if (w > most) most = w;
        calls++;
        still_old = n && rel[0] + FRAMES < now;
    }
    CHECK(most <= VKB_ASIDE_PER_CALL);
    CHECK(!still_old && n == VKB_ASIDE_SPREAD);   // the frame-1 ones were already pushed out by frame 2's
    // the rest stay until they age, then go two per call
    CHECK(rel[0] == 2);
    CHECK(purge(VKB_ASIDE_SPREAD, 2 + FRAMES) == 0);   // not past its frames yet
    CHECK(purge(VKB_ASIDE_SPREAD, 2 + FRAMES + 1) == 2);
    CHECK(n == VKB_ASIDE_SPREAD - 2);
    // a scene change: 1500 aside at once, all old 121 frames later: they go two per call over 750 calls, not at once
    n = 0; most = 0; calls = 0;
    for (int i = 0; i < 1500; i++) release(VKB_ASIDE_SPREAD, 10);
    while (n && calls < 10000) { int w = purge(VKB_ASIDE_SPREAD, 10 + FRAMES + 1); if (w > most) most = w; calls++; }
    CHECK(most == VKB_ASIDE_PER_CALL && calls == 750);
    // [UIT 0130] which images may share a block: sampled-only ones; render targets, depth and storage images only
    // with TOTK_VK_SUBALLOC_TARGETS=1
    {
        uint32_t sampled = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        CHECK(vkb_mem_shared_usage(sampled, 0));
        CHECK(!vkb_mem_shared_usage(sampled | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, 0));
        CHECK(!vkb_mem_shared_usage(sampled | VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, 0));
        CHECK(!vkb_mem_shared_usage(sampled | VK_IMAGE_USAGE_STORAGE_BIT, 0));
        CHECK(vkb_mem_shared_usage(sampled | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_STORAGE_BIT, 1));
    }
    // [UIT 0133] the allocation count guard: 512 kept free below the driver's limit (0: unknown, taken as 4096)
    CHECK(!vkb_mem_near_limit(3000, 4096, 512));
    CHECK(!vkb_mem_near_limit(3583, 4096, 512));
    CHECK(vkb_mem_near_limit(3584, 4096, 512));
    CHECK(vkb_mem_near_limit(3600, 0, 512));
    CHECK(!vkb_mem_near_limit(3600, 0xFFFFFFFFu, 512));   // drivers without a practical limit
    printf(fails ? "test_vkb_mem: %d FAILED\n" : "test_vkb_mem: all passed\n", fails);
    return fails != 0;
}
