// [UIT 0172] The in-game overlay's Vulkan pass (overlay_vk.c): after the game picture is upscaled into the swapchain
// image, one dynamic-rendering pass draws the overlay's quads (src/host/uit_overlay.c lays them out) as one instanced
// draw. present.c calls it only while g_uit_ov_visible is set (its one check while the overlay is closed).
#pragma once
#include "vk.h"

// Present thread. img (the swapchain image idx, of the given format and size) is in TRANSFER_DST_OPTIMAL after its
// transfer writes; on 1 the pass is recorded and img left in PRESENT_SRC_KHR, on 0 nothing was recorded (the caller
// makes its own transition). slot is the frame in flight (its fence was waited for); fences are all the slots'.
int ovk_record(VkCommandBuffer cb, VkImage img, uint32_t idx, VkFormat format, VkExtent2D size, unsigned slot,
               const VkFence *fences, unsigned nfences);
// Present thread, with no work of the swapchain's in flight: the per-image views go with the swapchain.
void ovk_swapchain_gone(void);
