// The process's one Vulkan device, shared by the NVN replayer (src/gfx/backend_vk.c) and the window presentation
// (present.c). Functions are loaded at run time through SDL's vkGetInstanceProcAddr (no vulkan-1 link), into the table
// below; a function is called as vk.Name(...).
// The device enables every feature the GPU supports except robustBufferAccess (bounds checks the game does not need,
// at a cost on every buffer access), and the extensions of the replayer's design (2026-10-21).
#pragma once
#define VK_NO_PROTOTYPES
#include <stdint.h>
#include <vulkan/vulkan.h>

#define VK_FN_GLOBAL(X) X(vkCreateInstance) X(vkEnumerateInstanceLayerProperties) X(vkEnumerateInstanceExtensionProperties)
#define VK_FN_INSTANCE(X) X(vkDestroyInstance) X(vkEnumeratePhysicalDevices) X(vkGetPhysicalDeviceProperties) \
    X(vkGetPhysicalDeviceProperties2) X(vkGetPhysicalDeviceFeatures2) X(vkGetPhysicalDeviceMemoryProperties) X(vkGetPhysicalDeviceMemoryProperties2) \
    X(vkGetPhysicalDeviceQueueFamilyProperties) X(vkGetPhysicalDeviceFormatProperties) X(vkGetPhysicalDeviceImageFormatProperties) \
    X(vkGetPhysicalDeviceSurfaceSupportKHR) X(vkGetPhysicalDeviceSurfaceCapabilitiesKHR) \
    X(vkGetPhysicalDeviceSurfaceFormatsKHR) X(vkGetPhysicalDeviceSurfacePresentModesKHR) \
    X(vkEnumerateDeviceExtensionProperties) X(vkCreateDevice) X(vkGetDeviceProcAddr) X(vkDestroySurfaceKHR)
#define VK_FN_DEVICE(X) X(vkGetDeviceQueue) X(vkDeviceWaitIdle) X(vkQueueSubmit) X(vkQueueSubmit2) X(vkQueueWaitIdle) \
    X(vkCreateSwapchainKHR) X(vkDestroySwapchainKHR) X(vkGetSwapchainImagesKHR) X(vkAcquireNextImageKHR) X(vkQueuePresentKHR) \
    X(vkCreateCommandPool) X(vkResetCommandPool) X(vkAllocateCommandBuffers) X(vkBeginCommandBuffer) \
    X(vkEndCommandBuffer) X(vkResetCommandBuffer) X(vkCmdPipelineBarrier) X(vkCmdPipelineBarrier2) \
    X(vkCmdClearColorImage) X(vkCmdCopyBuffer) X(vkCmdUpdateBuffer) X(vkCmdBlitImage) X(vkCmdFillBuffer) \
    X(vkCreateSemaphore) X(vkDestroySemaphore) X(vkCreateFence) X(vkWaitForFences) X(vkResetFences) X(vkDestroyFence) \
    X(vkWaitSemaphores) X(vkGetSemaphoreCounterValue) X(vkMapMemory) X(vkUnmapMemory) \
    X(vkAllocateMemory) X(vkFreeMemory) X(vkCreateBuffer) X(vkDestroyBuffer) X(vkBindBufferMemory) \
    X(vkGetBufferMemoryRequirements) X(vkGetMemoryHostPointerPropertiesEXT) X(vkGetBufferDeviceAddress) \
    X(vkCreateImage) X(vkDestroyImage) X(vkGetImageMemoryRequirements) X(vkBindImageMemory) X(vkCreateImageView) \
    X(vkDestroyImageView) X(vkCmdBeginRendering) X(vkCmdEndRendering) X(vkCmdClearAttachments) X(vkCmdCopyImage) \
    X(vkCmdClearDepthStencilImage) X(vkCmdCopyBufferToImage) X(vkCmdCopyImageToBuffer) \
    X(vkCreateDescriptorSetLayout) X(vkCreatePipelineLayout) X(vkCreateDescriptorPool) X(vkResetDescriptorPool) \
    X(vkAllocateDescriptorSets) X(vkUpdateDescriptorSets) X(vkCmdBindDescriptorSets) X(vkCreateSampler) X(vkDestroySampler) \
    X(vkCreateShadersEXT) X(vkDestroyShaderEXT) X(vkGetShaderBinaryDataEXT) X(vkCmdBindShadersEXT) X(vkCmdDraw) X(vkCmdDrawIndexed) X(vkCmdDispatch) \
    X(vkCmdBindVertexBuffers2) X(vkCmdBindIndexBuffer) X(vkCmdSetVertexInputEXT) X(vkCmdSetViewportWithCount) \
    X(vkCmdSetScissorWithCount) X(vkCmdSetPrimitiveTopology) X(vkCmdSetPrimitiveRestartEnable) X(vkCmdSetRasterizerDiscardEnable) \
    X(vkCmdSetCullMode) X(vkCmdSetFrontFace) X(vkCmdSetPolygonModeEXT) X(vkCmdSetDepthBiasEnable) X(vkCmdSetDepthBias) \
    X(vkCmdSetDepthTestEnable) X(vkCmdSetDepthWriteEnable) X(vkCmdSetDepthCompareOp) X(vkCmdSetDepthBoundsTestEnable) \
    X(vkCmdSetStencilTestEnable) X(vkCmdSetStencilOp) X(vkCmdSetStencilCompareMask) X(vkCmdSetStencilWriteMask) \
    X(vkCmdSetStencilReference) X(vkCmdSetRasterizationSamplesEXT) X(vkCmdSetSampleMaskEXT) X(vkCmdSetAlphaToCoverageEnableEXT) \
    X(vkCmdSetAlphaToOneEnableEXT) X(vkCmdSetColorBlendEnableEXT) X(vkCmdSetColorBlendEquationEXT) X(vkCmdSetColorWriteMaskEXT) \
    X(vkCmdSetLogicOpEnableEXT) X(vkCmdSetLogicOpEXT) X(vkCmdSetBlendConstants) X(vkCmdSetDepthClampEnableEXT) \
    X(vkCmdSetLineWidth) X(vkCmdSetDepthClipNegativeOneToOneEXT) X(vkCmdSetViewport) X(vkCmdSetScissor) \
    X(vkCmdSetProvokingVertexModeEXT) X(vkCmdSetDepthClipEnableEXT) X(vkCmdSetDepthBounds) \
    X(vkCreateQueryPool) X(vkResetQueryPool) X(vkCmdResetQueryPool) X(vkCmdWriteTimestamp2) X(vkGetQueryPoolResults) \
    X(vkCmdBeginQuery) X(vkCmdEndQuery) \
    X(vkCmdSetAttachmentFeedbackLoopEnableEXT) X(vkCmdSetCheckpointNV) X(vkGetQueueCheckpointDataNV) \
    X(vkGetDescriptorSetLayoutSizeEXT) X(vkGetDescriptorSetLayoutBindingOffsetEXT) X(vkGetDescriptorEXT) \
    X(vkCmdBindDescriptorBuffersEXT) X(vkCmdSetDescriptorBufferOffsetsEXT) \
    X(vkCreateShaderModule) X(vkDestroyShaderModule) X(vkCreateComputePipelines) X(vkCreateGraphicsPipelines) X(vkDestroyPipeline) \
    X(vkDestroyPipelineLayout) X(vkDestroyDescriptorSetLayout) X(vkDestroyDescriptorPool) X(vkCmdBindPipeline) \
    X(vkCmdPushConstants)
#define VK_FN_DECLARE(f) PFN_##f f;
typedef struct { VK_FN_GLOBAL(VK_FN_DECLARE) VK_FN_INSTANCE(VK_FN_DECLARE) VK_FN_DEVICE(VK_FN_DECLARE) } VkFunctions;
#undef VK_FN_DECLARE
extern VkFunctions vk;

typedef struct {
    VkInstance instance;
    VkSurfaceKHR surface;       // the window's, VK_NULL_HANDLE without one
    VkPhysicalDevice gpu;
    VkDevice device;
    VkQueue queue;              // graphics + compute + transfer (+ present when there is a window); vk_queue_lock()
    uint32_t family;
    VkPhysicalDeviceProperties props;
    VkPhysicalDeviceMemoryProperties memory;
    uint64_t host_pointer_alignment;   // VK_EXT_external_memory_host
    int has_descriptor_buffer, has_gpl, has_eds3, has_vertex_input_ds, has_depth_clip_control, has_shader_object, has_interlock;
    int has_feedback_loop;   // VK_EXT_attachment_feedback_loop_layout + _dynamic_state: sampling a bound attachment
    int has_local_read;      // VK_KHR_dynamic_rendering_local_read: barriers inside a pass (draws reading what earlier draws wrote)
    VkPhysicalDeviceDescriptorBufferPropertiesEXT db;   // VK_EXT_descriptor_buffer: descriptor sizes, alignment, ranges
    int has_aniso;           // [UIT 0066] the samplerAnisotropy feature (enabled when supported)
    int has_wide_lines;      // [UIT 0203] the wideLines feature (enabled when supported); range: props.limits.lineWidthRange
    int has_pv_last;         // [UIT 0205] VK_EXT_provoking_vertex provokingVertexLast (enabled when supported)
    int has_checkpoints;     // TOTK_VK_CHECKPOINTS=1: VK_NV_device_diagnostic_checkpoints (where a lost device stopped)
    // [UIT 0075] the loader entry (src/gfx/vkb_mem.c loads the budget query with it) and VK_EXT_memory_budget (enabled when the GPU
    // has it: a query only; the VRAM budget the headroom and staging threads of src/gfx/vkb_mem.c stay under, and
    // [UIT native] TOTK_VK_MEMLOG's [memory] log line; set when the GPU is picked, enabled with the device)
    PFN_vkGetInstanceProcAddr gipa;
    int has_memory_budget;
    // [UIT 0123] VK_EXT_depth_bias_control with floatRepresentation: depth bias given in depth units (r = 1), so the
    // guest's D16 / D24 bias keeps its size on the float depth buffers that stand for them (src/gfx/vkb_draw.c)
    int has_depth_bias_float;
#ifdef VK_EXT_depth_bias_control
    PFN_vkCmdSetDepthBias2EXT cmd_set_depth_bias2;
#endif
    // [UIT 0135] robustBufferAccess enabled on the device (TOTK_VK_ROBUST=1): the driver builds the bounds checks into
    // the shaders' machine code, so the shader binary cache keeps such binaries apart (src/gfx/vkb_shader.c)
    int robust;
    // [UIT 0136] fingerprint of the device as created: the GPU (vendor, device, driver version), every enabled
    // extension and every enabled feature bit. The shader binary cache keys its binaries with it, so a binary is only
    // ever loaded on a device set up exactly like the one that made it (src/gfx/vkb_shader.c)
    uint64_t dev_fp;
    // [UIT 0248] TOTK_VK_PRESENT_QUEUE: the queue the present thread submits and presents on (vk_pqueue_lock); pqueue_own:
    // a second queue of the same family (else pqueue == queue, under vk_queue_lock's lock)
    VkQueue pqueue;
    int pqueue_own;
} VkContext;
extern VkContext vkc;

typedef struct SDL_Window SDL_Window;
// Creates the instance and device once (the first caller's window decides the instance extensions for presentation;
// NULL = no window, e.g. TOTK_HEADLESS). 1 = ready; 0 = no usable Vulkan 1.3 GPU (logged).
int vk_init(SDL_Window *window);
void vk_queue_lock(void);       // the queue is used by the replayer's submit thread and the present thread
void vk_queue_unlock(void);
void vk_pqueue_lock(void);      // [UIT 0248] the present thread's queue (vkc.pqueue): its own lock, or vk_queue_lock's
void vk_pqueue_unlock(void);
void vk_queue_lock_stats(uint64_t *wait_ns, uint64_t *waits);   // [UIT 0248] time spent waiting for either lock, and how often
int vk_check(VkResult r, const char *what);   // 1 on success, logs the failure otherwise
