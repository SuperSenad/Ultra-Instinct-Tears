// [UIT 0172] The in-game overlay's Vulkan pass (see overlay_vk.h). Our own renderer, no UI library: one pipeline
// (src/vk/shaders/overlay.vert / .frag, compiled into overlay_spv.h by tools/gen_fsr_spv.py), the quads as instances
// of a 4-vertex strip from a host-visible buffer per frame in flight, one R8 font atlas (made once by the main
// thread at the window's scale, uploaded here once per change). Nothing is made until the overlay is first shown.
#include "overlay_vk.h"
#include "overlay_spv.h"
#include "../host/overlay_share.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "../runtime/logline.h"   // [UIT 0362] uit_logline: one write per line
#define LOG(...) uit_logline("[overlay-vk] " __VA_ARGS__)
#define SLOTS 2
#define VIEWS 8

static int g_state;                  // 0 not tried, 1 ready, -1 failed (the overlay is not drawn this run)
static VkFormat g_format;            // the pipeline's colour format
static VkDescriptorSetLayout g_dsl;
static VkPipelineLayout g_layout;
static VkPipeline g_pipe;
static VkSampler g_sampler;
static VkDescriptorPool g_pool;
static VkDescriptorSet g_set;
typedef struct { VkBuffer buf; VkDeviceMemory mem; void *map; VkDeviceSize size; } buf_t;
static buf_t g_inst[SLOTS];          // the quads, one buffer per frame in flight
static uint64_t g_inst_seq[SLOTS];   // the share's seq each holds (0: none)
static int g_inst_n[SLOTS];
static buf_t g_staging;              // the atlas upload's, freed when its slot comes round again
static int g_staging_slot = -1;
static VkImage g_atlas;
static VkDeviceMemory g_atlas_mem;
static VkImageView g_atlas_view;
static uint64_t g_atlas_gen;         // the share's atlas_gen uploaded (0: none)
static struct { VkImage img; VkImageView view; } g_views[VIEWS];

static uint32_t memory_type(uint32_t bits, VkMemoryPropertyFlags want) {
    for (uint32_t i = 0; i < vkc.memory.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (vkc.memory.memoryTypes[i].propertyFlags & want) == want) return i;
    return UINT32_MAX;
}

static void free_buf(buf_t *b) {
    if (b->map) vk.vkUnmapMemory(vkc.device, b->mem);
    if (b->buf) vk.vkDestroyBuffer(vkc.device, b->buf, NULL);
    if (b->mem) vk.vkFreeMemory(vkc.device, b->mem, NULL);
    *b = (buf_t){0};
}

static int make_buf(buf_t *b, VkDeviceSize size, VkBufferUsageFlags usage) {
    VkBufferCreateInfo ci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size, .usage = usage,
                             .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    if (!vk_check(vk.vkCreateBuffer(vkc.device, &ci, NULL, &b->buf), "overlay buffer")) return 0;
    VkMemoryRequirements req;
    vk.vkGetBufferMemoryRequirements(vkc.device, b->buf, &req);
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = req.size,
        .memoryTypeIndex = memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)};
    if (ai.memoryTypeIndex == UINT32_MAX || !vk_check(vk.vkAllocateMemory(vkc.device, &ai, NULL, &b->mem), "overlay memory") ||
        !vk_check(vk.vkBindBufferMemory(vkc.device, b->buf, b->mem, 0), "overlay bind") ||
        !vk_check(vk.vkMapMemory(vkc.device, b->mem, 0, VK_WHOLE_SIZE, 0, &b->map), "overlay map")) {
        free_buf(b);
        return 0;
    }
    b->size = size;
    return 1;
}

static VkImageView make_view(VkImage img, VkFormat f) {
    VkImageViewCreateInfo vi = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = img, .viewType = VK_IMAGE_VIEW_TYPE_2D,
                                .format = f, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    VkImageView v = VK_NULL_HANDLE;
    if (!vk_check(vk.vkCreateImageView(vkc.device, &vi, NULL, &v), "overlay image view")) v = VK_NULL_HANDLE;
    return v;
}

static VkPipeline make_pipeline(VkFormat format) {
    VkShaderModule m[2] = {0};
    VkShaderModuleCreateInfo mv = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = sizeof overlay_vert_spv, .pCode = overlay_vert_spv};
    VkShaderModuleCreateInfo mf = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = sizeof overlay_frag_spv, .pCode = overlay_frag_spv};
    VkPipeline p = VK_NULL_HANDLE;
    if (!vk_check(vk.vkCreateShaderModule(vkc.device, &mv, NULL, &m[0]), "overlay vertex shader") ||
        !vk_check(vk.vkCreateShaderModule(vkc.device, &mf, NULL, &m[1]), "overlay fragment shader")) goto done;
    VkPipelineShaderStageCreateInfo st[2] = {
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = m[0], .pName = "main"},
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = m[1], .pName = "main"}};
    VkVertexInputBindingDescription bind = {0, sizeof(UitOvQuad), VK_VERTEX_INPUT_RATE_INSTANCE};
    VkVertexInputAttributeDescription at[4] = {
        {0, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(UitOvQuad, x0)},
        {1, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(UitOvQuad, u0)},
        {2, 0, VK_FORMAT_R8G8B8A8_UNORM, offsetof(UitOvQuad, rgba)},
        {3, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(UitOvQuad, radius)}};
    VkPipelineVertexInputStateCreateInfo vi = {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        .vertexBindingDescriptionCount = 1, .pVertexBindingDescriptions = &bind, .vertexAttributeDescriptionCount = 4,
        .pVertexAttributeDescriptions = at};
    VkPipelineInputAssemblyStateCreateInfo ia = {VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
                                                 .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP};
    VkPipelineViewportStateCreateInfo vp = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount = 1, .scissorCount = 1};
    VkPipelineRasterizationStateCreateInfo rs = {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode = VK_POLYGON_MODE_FILL, .cullMode = VK_CULL_MODE_NONE, .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE, .lineWidth = 1};
    VkPipelineMultisampleStateCreateInfo ms = {VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
                                               .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT};
    VkPipelineColorBlendAttachmentState ba = {VK_TRUE, VK_BLEND_FACTOR_SRC_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, VK_BLEND_OP_ADD,
        VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, VK_BLEND_OP_ADD,
        VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT};
    VkPipelineColorBlendStateCreateInfo cb = {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, .attachmentCount = 1,
                                              .pAttachments = &ba};
    VkDynamicState dyn[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo ds = {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, .dynamicStateCount = 2, .pDynamicStates = dyn};
    VkPipelineRenderingCreateInfo ri = {VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO, .colorAttachmentCount = 1,
                                        .pColorAttachmentFormats = &format};
    VkGraphicsPipelineCreateInfo ci = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .pNext = &ri, .stageCount = 2, .pStages = st,
        .pVertexInputState = &vi, .pInputAssemblyState = &ia, .pViewportState = &vp, .pRasterizationState = &rs,
        .pMultisampleState = &ms, .pColorBlendState = &cb, .pDynamicState = &ds, .layout = g_layout};
    if (!vk_check(vk.vkCreateGraphicsPipelines(vkc.device, VK_NULL_HANDLE, 1, &ci, NULL, &p), "overlay pipeline")) p = VK_NULL_HANDLE;
done:
    for (int i = 0; i < 2; i++) if (m[i]) vk.vkDestroyShaderModule(vkc.device, m[i], NULL);
    return p;
}

static int init(void) {
    VkDescriptorSetLayoutBinding b = {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL};
    VkDescriptorSetLayoutCreateInfo di = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 1, .pBindings = &b};
    if (!vk_check(vk.vkCreateDescriptorSetLayout(vkc.device, &di, NULL, &g_dsl), "overlay set layout")) return 0;
    VkPushConstantRange pr = {VK_SHADER_STAGE_VERTEX_BIT, 0, 2 * sizeof(float)};
    VkPipelineLayoutCreateInfo li = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &g_dsl,
                                     .pushConstantRangeCount = 1, .pPushConstantRanges = &pr};
    if (!vk_check(vk.vkCreatePipelineLayout(vkc.device, &li, NULL, &g_layout), "overlay pipeline layout")) return 0;
    // the atlas is made at the drawn size (one texel per pixel); linear only smooths the sub-pixel placement
    VkSamplerCreateInfo si = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, .magFilter = VK_FILTER_LINEAR, .minFilter = VK_FILTER_LINEAR,
        .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST, .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE};
    if (!vk_check(vk.vkCreateSampler(vkc.device, &si, NULL, &g_sampler), "overlay sampler")) return 0;
    VkDescriptorPoolSize ps = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1};
    VkDescriptorPoolCreateInfo pi = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 1, .poolSizeCount = 1, .pPoolSizes = &ps};
    if (!vk_check(vk.vkCreateDescriptorPool(vkc.device, &pi, NULL, &g_pool), "overlay descriptor pool")) return 0;
    VkDescriptorSetAllocateInfo ai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = g_pool,
                                      .descriptorSetCount = 1, .pSetLayouts = &g_dsl};
    if (!vk_check(vk.vkAllocateDescriptorSets(vkc.device, &ai, &g_set), "overlay descriptor set")) return 0;
    for (int i = 0; i < SLOTS; i++)
        if (!make_buf(&g_inst[i], UIT_OV_MAX_QUADS * sizeof(UitOvQuad), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT)) return 0;
    LOG("ready (%u KiB of quad buffers)", (unsigned)(SLOTS * UIT_OV_MAX_QUADS * sizeof(UitOvQuad) / 1024));
    return 1;
}

static void barrier(VkCommandBuffer cb, VkImage img, VkImageLayout from, VkImageLayout to, VkAccessFlags src, VkAccessFlags dst,
                    VkPipelineStageFlags s0, VkPipelineStageFlags s1) {
    VkImageMemoryBarrier b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .srcAccessMask = src, .dstAccessMask = dst,
        .oldLayout = from, .newLayout = to, .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = img, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    vk.vkCmdPipelineBarrier(cb, s0, s1, 0, 0, NULL, 0, NULL, 1, &b);
}

static void free_atlas(void) {
    if (g_atlas_view) vk.vkDestroyImageView(vkc.device, g_atlas_view, NULL);
    if (g_atlas) vk.vkDestroyImage(vkc.device, g_atlas, NULL);
    if (g_atlas_mem) vk.vkFreeMemory(vkc.device, g_atlas_mem, NULL);
    g_atlas_view = VK_NULL_HANDLE;
    g_atlas = VK_NULL_HANDLE;
    g_atlas_mem = VK_NULL_HANDLE;
    g_atlas_gen = 0;
}

// Under the share lock: a new atlas into a new image, its copy recorded into cb (before the pass). The old image may
// still be read by the other frame in flight, so that frame's fence is waited for first (this happens once per
// atlas: the first show and a large change of the window's size).
static int upload_atlas(VkCommandBuffer cb, const UitOvShared *s, unsigned slot, const VkFence *fences, unsigned nfences) {
    for (unsigned i = 0; i < nfences; i++)
        if (i != slot) vk.vkWaitForFences(vkc.device, 1, &fences[i], VK_TRUE, UINT64_MAX);
    free_atlas();
    free_buf(&g_staging);
    g_staging_slot = -1;
    VkDeviceSize bytes = (VkDeviceSize)s->aw * (VkDeviceSize)s->ah;
    if (!make_buf(&g_staging, bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT)) return 0;
    memcpy(g_staging.map, s->pix, (size_t)bytes);
    g_staging_slot = (int)slot;
    VkImageCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = VK_FORMAT_R8_UNORM,
        .extent = {(uint32_t)s->aw, (uint32_t)s->ah, 1}, .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
    if (!vk_check(vk.vkCreateImage(vkc.device, &ci, NULL, &g_atlas), "overlay atlas")) return 0;
    VkMemoryRequirements req;
    vk.vkGetImageMemoryRequirements(vkc.device, g_atlas, &req);
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = req.size,
                               .memoryTypeIndex = memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)};
    if (ai.memoryTypeIndex == UINT32_MAX || !vk_check(vk.vkAllocateMemory(vkc.device, &ai, NULL, &g_atlas_mem), "overlay atlas memory"))
        return 0;
    vk.vkBindImageMemory(vkc.device, g_atlas, g_atlas_mem, 0);
    if (!(g_atlas_view = make_view(g_atlas, VK_FORMAT_R8_UNORM))) return 0;
    barrier(cb, g_atlas, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkBufferImageCopy r = {0, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {(uint32_t)s->aw, (uint32_t)s->ah, 1}};
    vk.vkCmdCopyBufferToImage(cb, g_staging.buf, g_atlas, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r);
    barrier(cb, g_atlas, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
            VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    VkDescriptorImageInfo ii = {g_sampler, g_atlas_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet w = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = g_set, .dstBinding = 0, .descriptorCount = 1,
                              .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &ii};
    vk.vkUpdateDescriptorSets(vkc.device, 1, &w, 0, NULL);
    g_atlas_gen = s->atlas_gen;
    return 1;
}

static VkImageView view_of(VkImage img, uint32_t idx, VkFormat format) {
    if (idx >= VIEWS) return VK_NULL_HANDLE;
    if (g_views[idx].img != img) {
        if (g_views[idx].view) return VK_NULL_HANDLE;   // a new swapchain without ovk_swapchain_gone: not drawn
        g_views[idx].view = make_view(img, format);
        g_views[idx].img = g_views[idx].view ? img : VK_NULL_HANDLE;
    }
    return g_views[idx].view;
}

void ovk_swapchain_gone(void) {
    for (int i = 0; i < VIEWS; i++) {
        if (g_views[i].view) vk.vkDestroyImageView(vkc.device, g_views[i].view, NULL);
        g_views[i].view = VK_NULL_HANDLE;
        g_views[i].img = VK_NULL_HANDLE;
    }
}

int ovk_record(VkCommandBuffer cb, VkImage img, uint32_t idx, VkFormat format, VkExtent2D size, unsigned slot,
               const VkFence *fences, unsigned nfences) {
    if (g_state < 0 || slot >= SLOTS || !size.width || !size.height) return 0;
    if (!g_state) {
        g_state = init() ? 1 : -1;
        if (g_state < 0) { LOG("unavailable: the overlay is not drawn this run"); return 0; }
    }
    if ((int)slot == g_staging_slot) { free_buf(&g_staging); g_staging_slot = -1; }   // its copy is done (slot's fence)
    if (!g_pipe || g_format != format) {   // the first time, and a swapchain of another format (rare): nothing may use the old one
        if (g_pipe) {
            vk.vkWaitForFences(vkc.device, nfences, fences, VK_TRUE, UINT64_MAX);
            vk.vkDestroyPipeline(vkc.device, g_pipe, NULL);
        }
        g_format = format;
        if (!(g_pipe = make_pipeline(format))) { g_state = -1; LOG("no pipeline: the overlay is not drawn this run"); return 0; }
    }
    VkImageView view = view_of(img, idx, format);
    if (!view) return 0;

    // the hand-over: the quads (only when they changed since this slot's buffer was filled) and a new atlas
    const UitOvShared *s = uit_ov_share_lock();
    int n = s->n, ok = s->pix != NULL && n > 0;
    if (ok && s->atlas_gen != g_atlas_gen) ok = upload_atlas(cb, s, slot, fences, nfences);
    if (ok && g_inst_seq[slot] != s->seq) {
        if (n > UIT_OV_MAX_QUADS) n = UIT_OV_MAX_QUADS;
        memcpy(g_inst[slot].map, s->q, (size_t)n * sizeof *s->q);
        g_inst_seq[slot] = s->seq;
        g_inst_n[slot] = n;
    }
    uit_ov_share_unlock();
    if (!ok) return 0;   // nothing to draw, or the upload failed (g_atlas_gen is 0 again: the next frame retries)
    n = g_inst_n[slot];

    const VkPipelineStageFlags CA = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    barrier(cb, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
            VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, CA);
    VkRenderingAttachmentInfo ca = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = view,
        .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD,
        .storeOp = VK_ATTACHMENT_STORE_OP_STORE};
    VkRenderingInfo ri = {VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = {{0, 0}, size}, .layerCount = 1,
                          .colorAttachmentCount = 1, .pColorAttachments = &ca};
    vk.vkCmdBeginRendering(cb, &ri);
    VkViewport vp = {0, 0, (float)size.width, (float)size.height, 0, 1};
    VkRect2D sc = {{0, 0}, size};
    vk.vkCmdSetViewport(cb, 0, 1, &vp);
    vk.vkCmdSetScissor(cb, 0, 1, &sc);
    vk.vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, g_pipe);
    vk.vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, g_layout, 0, 1, &g_set, 0, NULL);
    float inv[2] = {2.0f / (float)size.width, 2.0f / (float)size.height};
    vk.vkCmdPushConstants(cb, g_layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof inv, inv);
    VkDeviceSize off = 0;
    vk.vkCmdBindVertexBuffers2(cb, 0, 1, &g_inst[slot].buf, &off, NULL, NULL);
    vk.vkCmdDraw(cb, 4, (uint32_t)n, 0, 0);   // every quad in one draw
    vk.vkCmdEndRendering(cb);
    barrier(cb, img, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
            VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, 0, CA, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
    return 1;
}
