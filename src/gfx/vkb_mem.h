// [UIT 0075] Device memory for images and upload chunks without a vkAllocateMemory on the render thread (vkb_mem.c).
//   vkb_mem_image      allocate and bind an image's memory: a place in a shared block (the suballocator of vkb_mem.c,
//                      TOTK_VK_SUBALLOC=1) or, as before, one allocation of its own. *mem: the VkDeviceMemory it is in;
//                      *alloc: the suballocation (vkb_mem.c) (NULL: *mem is the image's own and is freed with it)
//                      [UIT 0130] `usage`: images the GPU draws into or writes (render targets, depth, storage) keep
//                      an allocation of their own unless TOTK_VK_SUBALLOC_TARGETS=1 (vkb_mem_shared_usage)
//   vkb_mem_free       gives the place back (after the image is destroyed; vkb_destroy_image_later does both)
//   vkb_mem_chunk      [TOTK_VK_STAGING_POOL] an upload chunk of `size` bytes in memory type `type`: a spare one made
//                      at boot or by the background thread when one is ready, else 0 (the caller allocates as before)
// The headroom thread (TOTK_VRAM_HEADROOM, with TOTK_VK_SUBALLOC) makes the next block of each memory type in use
// ahead of need, and the spare upload chunks; neither while usage + reserve passes 85% of the VRAM budget.
#ifndef VKB_MEM_H
#define VKB_MEM_H
#include <stdint.h>
#include <vulkan/vulkan.h>

int vkb_mem_suballoc(void);   // TOTK_VK_SUBALLOC on (and the allocator came up)
int vkb_mem_image(VkImage img, VkImageUsageFlags usage, VkMemoryPropertyFlags want, VkDeviceMemory *mem, void **alloc);
void vkb_mem_free(void *alloc);
// [UIT 0133] VkDeviceMemory objects alive now (memory pools imported, images' own memory, suballocator blocks), and the
// driver's maxMemoryAllocationCount (4096 on NVIDIA under Windows; TOTK alone imports 1500-2700 memory pools)
void vkb_mem_count(int delta);
uint64_t vkb_mem_live(void);
uint64_t vkb_mem_dedicated_live(void);   // of those, images' own memory
void vkb_mem_own_freed(void);            // an image's own memory (alloc NULL) was freed (backend_vk.c dead_reap)
uint64_t vkb_mem_crowded(void);          // GPU-written images the guard put in shared blocks
// Spare upload chunks (vkb_draw.c's upload ring): 1 and the chunk's buffer, memory and mapping when one was ready.
// `make` is the chunk maker (vkb_draw.c's, so the chunk is made exactly as before); vkb_mem_chunk_init starts the pool.
typedef int (*vkb_chunk_make_fn)(VkDeviceSize size, VkBuffer *buf, VkDeviceMemory *mem, uint8_t **map);
void vkb_mem_chunk_init(vkb_chunk_make_fn make, VkDeviceSize size, uint32_t type);
int vkb_mem_chunk(VkDeviceSize size, VkBuffer *buf, VkDeviceMemory *mem, uint8_t **map);
// The pure parts, for the box test (tools/tests/test_vkb_mem.c)
static inline int vkb_mem_room(uint64_t usage, uint64_t budget, uint64_t reserve) {   // a background block fits
    return budget && usage + reserve <= budget / 100 * 85;
}
// [UIT 0130] An image of this usage may take a place in a shared block. Images the GPU writes (render targets, depth,
// storage) are drawn into before, or without, any upload of the guest's bytes, so whatever they are not drawn over
// keeps the memory's earlier contents: zeros in a fresh allocation of its own (every allocation before 0104 and with
// TOTK_VK_SUBALLOC off), but the bytes of a freed texture in a recycled place of a block. They stay in memory of
// their own, as VMA advises for render targets; the sampled textures (the decoded ASTC / BC world textures, most of
// them) still share blocks. targets (TOTK_VK_SUBALLOC_TARGETS=1): every image may share, as 0104.
static inline int vkb_mem_shared_usage(uint32_t usage, int targets) {
    return targets || !(usage & (VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
                                 VK_IMAGE_USAGE_STORAGE_BIT));
}
// [UIT 0133] near the allocation count limit: GPU-written images too take a place in a shared block (as 0104) rather
// than a VkDeviceMemory of their own; `margin` kept free for pools, upload chunks and the rest (limit 0: unknown, 4096)
static inline int vkb_mem_near_limit(uint64_t live, uint64_t limit, uint64_t margin) {
    if (!limit) limit = 4096;
    return live + margin >= limit;
}
#endif
