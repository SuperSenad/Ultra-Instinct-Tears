// FSR 1 at the window present (see present_fsr.h). Two compute passes into images of the window rectangle's size:
// EASU (the game's frame -> mid) and RCAS (mid -> fin), then fin is blitted 1:1 into the swapchain image (the blit
// converts RGBA -> the swapchain's BGRA/RGBA, so the swapchain needs no storage usage). The shaders are
// src/vk/shaders/fsr1_*.comp, compiled into fsr1_spv.h by tools/gen_fsr_spv.py.
// The design follows SkyboundTOTK's present pass (a64-kit/gpu/render/rx_present_fx.cpp, MIT): FSR only when the
// picture is enlarged, sharpness in stops, F8 / F9; it is a new Vulkan implementation, not a copy of that wgpu code.
#include "present_fsr.h"
#include "fsr1_spv.h"
#include "dlss.h"
#include "../host/uit_live.h"

#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>

#include "../runtime/logline.h"   // [UIT 0362] uit_logline: one write per line
#define LOG(...) uit_logline("[totk-fsr] " __VA_ARGS__)

static const float k_sharpness[] = {0.0f, 0.25f, 0.5f, 1.0f, 2.0f};
static _Atomic int g_want = -1;           // -1: not read yet
static _Atomic float g_sharpness = -1.0f;

static void read_settings(void) {
    if (atomic_load(&g_want) >= 0) return;
    // TOTK_UPSCALER=fsr|dlss: FSR 1 at the present (with dlss: DLSS makes the 1080p canvas, FSR takes it to the display;
    // TOTK_FSR=0 still turns it off); off: bilinear. Unset: TOTK_FSR alone, as before.
    const char *v = getenv("TOTK_FSR");
    int fsr = v && *v && *v != '0';
    switch (dlss_upscaler()) {
    case UPSCALER_FSR: fsr = 1; break;
    case UPSCALER_OFF: fsr = 0; break;
    case UPSCALER_DLSS: fsr = !(v && *v == '0'); break;
    default: break;
    }
    atomic_store(&g_want, fsr);
    float s = 0.25f;
    if ((v = getenv("TOTK_FSR_SHARPNESS"))) s = (float)atof(v);
    if (s < 0) s = 0;
    if (s > 2) s = 2;
    atomic_store(&g_sharpness, s);
}

int vkp_fsr_wanted(void) { read_settings(); return atomic_load(&g_want); }
void vkp_fsr_toggle(void) {
    read_settings();
    int on = !atomic_load(&g_want);
    atomic_store(&g_want, on);
    LOG("FSR %s", on ? "on" : "off (bilinear)");
}
// [UIT 0170] the in-game overlay: on / off and any sharpness in 0..2 (read by the next present's RCAS pass)
void vkp_fsr_set(int on) {
    read_settings();
    atomic_store(&g_want, on ? 1 : 0);
    LOG("FSR %s (overlay)", on ? "on" : "off (bilinear)");
}
float vkp_fsr_sharpness(void) { read_settings(); return atomic_load(&g_sharpness); }
void vkp_fsr_set_sharpness(float s) {
    read_settings();
    if (!(s >= 0)) s = 0;
    if (s > 2) s = 2;
    atomic_store(&g_sharpness, s);
}
void vkp_fsr_sharpness_next(void) {
    read_settings();
    float s = atomic_load(&g_sharpness), next = k_sharpness[0];
    for (unsigned i = 0; i < sizeof k_sharpness / sizeof *k_sharpness; i++)
        if (k_sharpness[i] > s + 0.001f) { next = k_sharpness[i]; break; }
    atomic_store(&g_sharpness, next);
    LOG("FSR sharpness %.2f stops (0 the sharpest)", next);
}

// ---- Vulkan objects (present thread only)
typedef struct { float con[4]; int32_t crop[4]; int32_t size[2]; int32_t pad[2]; } push_t;   // the shaders' push block
static int g_state;                       // 0 not tried, 1 ready, -1 failed (FSR off for the run)
static VkDescriptorSetLayout g_dsl;
static VkPipelineLayout g_layout;
static VkPipeline g_easu, g_rcas;
static VkSampler g_sampler;
static VkDescriptorPool g_pool;
typedef struct { VkImage img; VkDeviceMemory mem; VkImageView view; } img_t;
static img_t g_mid, g_fin;                // the rectangle's size
static uint32_t g_w, g_h;
static VkImageView g_src_view;            // of the last call's source (destroyed by the next call)

static VkPipeline make_pipeline(const uint32_t *code, size_t bytes) {
    VkShaderModuleCreateInfo mi = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = bytes, .pCode = code};
    VkShaderModule m;
    if (!vk_check(vk.vkCreateShaderModule(vkc.device, &mi, NULL, &m), "FSR shader module")) return VK_NULL_HANDLE;
    VkComputePipelineCreateInfo ci = {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = m,
                  .pName = "main"},
        .layout = g_layout};
    VkPipeline p = VK_NULL_HANDLE;
    if (!vk_check(vk.vkCreateComputePipelines(vkc.device, VK_NULL_HANDLE, 1, &ci, NULL, &p), "FSR pipeline")) p = VK_NULL_HANDLE;
    vk.vkDestroyShaderModule(vkc.device, m, NULL);
    return p;
}

static int init(void) {
    VkDescriptorSetLayoutBinding b[2] = {
        {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
        {1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL}};
    VkDescriptorSetLayoutCreateInfo di = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 2, .pBindings = b};
    if (!vk_check(vk.vkCreateDescriptorSetLayout(vkc.device, &di, NULL, &g_dsl), "FSR set layout")) return 0;
    VkPushConstantRange pr = {VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push_t)};
    VkPipelineLayoutCreateInfo li = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &g_dsl,
                                     .pushConstantRangeCount = 1, .pPushConstantRanges = &pr};
    if (!vk_check(vk.vkCreatePipelineLayout(vkc.device, &li, NULL, &g_layout), "FSR pipeline layout")) return 0;
    g_easu = make_pipeline(fsr1_easu_spv, sizeof fsr1_easu_spv);
    g_rcas = make_pipeline(fsr1_rcas_spv, sizeof fsr1_rcas_spv);
    if (!g_easu || !g_rcas) return 0;
    VkSamplerCreateInfo si = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, .magFilter = VK_FILTER_NEAREST, .minFilter = VK_FILTER_NEAREST,
        .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST, .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE};
    if (!vk_check(vk.vkCreateSampler(vkc.device, &si, NULL, &g_sampler), "FSR sampler")) return 0;
    VkDescriptorPoolSize ps[2] = {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2}, {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 2}};
    VkDescriptorPoolCreateInfo pi = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 2, .poolSizeCount = 2, .pPoolSizes = ps};
    if (!vk_check(vk.vkCreateDescriptorPool(vkc.device, &pi, NULL, &g_pool), "FSR descriptor pool")) return 0;
    LOG("FSR 1 ready (EASU + RCAS)");
    return 1;
}

static uint32_t memory_type(uint32_t bits, VkMemoryPropertyFlags want) {
    for (uint32_t i = 0; i < vkc.memory.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (vkc.memory.memoryTypes[i].propertyFlags & want) == want) return i;
    return UINT32_MAX;
}

static VkImageView make_view(VkImage img, VkFormat f) {
    VkImageViewCreateInfo vi = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = img, .viewType = VK_IMAGE_VIEW_TYPE_2D,
                                .format = f, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    VkImageView v = VK_NULL_HANDLE;
    if (!vk_check(vk.vkCreateImageView(vkc.device, &vi, NULL, &v), "FSR image view")) v = VK_NULL_HANDLE;
    return v;
}

static void free_image(img_t *t) {
    if (t->view) vk.vkDestroyImageView(vkc.device, t->view, NULL);
    if (t->img) vk.vkDestroyImage(vkc.device, t->img, NULL);
    if (t->mem) vk.vkFreeMemory(vkc.device, t->mem, NULL);
    *t = (img_t){0};
}

static int make_image(img_t *t, uint32_t w, uint32_t h, VkImageUsageFlags usage) {
    VkImageCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_UNORM,
        .extent = {w, h, 1}, .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = usage, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
    if (!vk_check(vk.vkCreateImage(vkc.device, &ci, NULL, &t->img), "FSR image")) return 0;
    VkMemoryRequirements req;
    vk.vkGetImageMemoryRequirements(vkc.device, t->img, &req);
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = req.size,
                               .memoryTypeIndex = memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)};
    if (ai.memoryTypeIndex == UINT32_MAX || !vk_check(vk.vkAllocateMemory(vkc.device, &ai, NULL, &t->mem), "FSR memory")) return 0;
    vk.vkBindImageMemory(vkc.device, t->img, t->mem, 0);
    return (t->view = make_view(t->img, VK_FORMAT_R8G8B8A8_UNORM)) != VK_NULL_HANDLE;
}

static int size_images(uint32_t w, uint32_t h) {
    if (g_mid.img && g_w == w && g_h == h) return 1;
    free_image(&g_mid);   // the last call's work is done (the caller waited for its fence)
    free_image(&g_fin);
    g_w = g_h = 0;
    if (!make_image(&g_mid, w, h, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT) ||
        !make_image(&g_fin, w, h, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT)) {
        free_image(&g_mid);
        free_image(&g_fin);
        return 0;
    }
    g_w = w; g_h = h;
    return 1;
}

static void barrier(VkCommandBuffer cb, VkImage img, VkImageLayout from, VkImageLayout to, VkAccessFlags src, VkAccessFlags dst,
                    VkPipelineStageFlags s0, VkPipelineStageFlags s1) {
    VkImageMemoryBarrier b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .srcAccessMask = src, .dstAccessMask = dst,
        .oldLayout = from, .newLayout = to, .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = img, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    vk.vkCmdPipelineBarrier(cb, s0, s1, 0, 0, NULL, 0, NULL, 1, &b);
}

static void bind_set(VkDescriptorSet set, VkImageView in, VkImageView out) {
    VkDescriptorImageInfo ii = {g_sampler, in, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkDescriptorImageInfo oi = {VK_NULL_HANDLE, out, VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet w[2] = {
        {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .dstBinding = 0, .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &ii},
        {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .dstBinding = 1, .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .pImageInfo = &oi}};
    vk.vkUpdateDescriptorSets(vkc.device, 2, w, 0, NULL);
}

int vkp_fsr_record(VkCommandBuffer cb, VkImage src, const int crop[4], VkImage dst, int dx, int dy, int dw, int dh) {
    if (!vkp_fsr_wanted() || g_state < 0 || dw <= 0 || dh <= 0) return 0;
    if (dw <= crop[2] && dh <= crop[3]) return 0;   // not enlarged: the plain blit is as good
    if (dlss_upscaler() == UPSCALER_DLSS) {   // [native-1031] say once that FSR 1 is doing DLSS's last step, and how not to
        static int said;
        if (!said++) LOG("DLSS makes a %dx%d picture and FSR 1 enlarges it to %dx%d. For DLSS to make the full picture itself: "
                         "Graphics > Resolution and shape > Output resolution (experimental): The display's size", crop[2], crop[3], dw, dh);
    }
    if (!g_state) {
        g_state = init() ? 1 : -1;
        if (g_state < 0) { LOG("FSR unavailable; bilinear present"); return 0; }
    }
    // the last call's work is finished (present_one waits for the fence of a frame with a game picture)
    if (g_src_view) { vk.vkDestroyImageView(vkc.device, g_src_view, NULL); g_src_view = VK_NULL_HANDLE; }
    vk.vkResetDescriptorPool(vkc.device, g_pool, 0);
    if (!size_images((uint32_t)dw, (uint32_t)dh)) return 0;
    if (!(g_src_view = make_view(src, VK_FORMAT_B8G8R8A8_UNORM))) return 0;
    VkDescriptorSet sets[2];
    VkDescriptorSetLayout dsl[2] = {g_dsl, g_dsl};
    VkDescriptorSetAllocateInfo ai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = g_pool,
                                      .descriptorSetCount = 2, .pSetLayouts = dsl};
    if (!vk_check(vk.vkAllocateDescriptorSets(vkc.device, &ai, sets), "FSR descriptor sets")) return 0;
    bind_set(sets[0], g_src_view, g_mid.view);
    bind_set(sets[1], g_mid.view, g_fin.view);

    const VkPipelineStageFlags CS = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, TR = VK_PIPELINE_STAGE_TRANSFER_BIT;
    barrier(cb, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, TR, CS);
    barrier(cb, g_mid.img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, 0, VK_ACCESS_SHADER_WRITE_BIT, CS, CS);
    barrier(cb, g_fin.img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, 0, VK_ACCESS_SHADER_WRITE_BIT, TR, CS);
    uint32_t gx = ((uint32_t)dw + 7) / 8, gy = ((uint32_t)dh + 7) / 8;

    // EASU: output pixel p -> source position p * (src / dst) + (0.5 * src / dst - 0.5), as FsrEasuCon
    push_t pc = {{(float)crop[2] / dw, (float)crop[3] / dh, 0, 0}, {crop[0], crop[1], crop[2], crop[3]}, {dw, dh}, {0, 0}};
    pc.con[2] = 0.5f * pc.con[0] - 0.5f;
    pc.con[3] = 0.5f * pc.con[1] - 0.5f;
    vk.vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, g_easu);
    vk.vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, g_layout, 0, 1, &sets[0], 0, NULL);
    vk.vkCmdPushConstants(cb, g_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pc, &pc);
    vk.vkCmdDispatch(cb, gx, gy, 1);
    barrier(cb, g_mid.img, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, CS, CS);

    // RCAS: sharpness in stops -> 2^-stops, as FsrRcasCon (powf: the libm function the port already links)
    push_t rc = {{powf(2.0f, -atomic_load(&g_sharpness)), 0, 0, 0}, {0, 0, 0, 0}, {dw, dh}, {0, 0}};
    vk.vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, g_rcas);
    vk.vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, g_layout, 0, 1, &sets[1], 0, NULL);
    vk.vkCmdPushConstants(cb, g_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof rc, &rc);
    vk.vkCmdDispatch(cb, gx, gy, 1);
    barrier(cb, g_fin.img, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT, CS, TR);

    // 1:1 into the window rectangle (format conversion only)
    VkImageBlit b = {{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {{0, 0, 0}, {dw, dh, 1}},
                     {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {{dx, dy, 0}, {dx + dw, dy + dh, 1}}};
    vk.vkCmdBlitImage(cb, g_fin.img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &b,
                      VK_FILTER_NEAREST);
    barrier(cb, src, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            0, 0, CS, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
    return 1;
}
