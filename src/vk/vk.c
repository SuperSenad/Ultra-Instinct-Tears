// The shared Vulkan instance and device (see vk.h).
#include "vk.h"
#include "dlss.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#include <pthread.h>
#include <windows.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../runtime/logline.h"   // [UIT 0362] uit_logline: one write per line
#define LOG(...) uit_logline("[totk-vk] " __VA_ARGS__)

VkFunctions vk;
VkContext vkc;
static pthread_mutex_t queue_mu = PTHREAD_MUTEX_INITIALIZER;
// [UIT 0248] the time threads waited for the queue lock (the replay's submits behind the present thread's
// vkQueueSubmit2 + vkQueuePresentKHR, which can block in the driver), for the per-frame "submit thread" log line
static _Atomic uint64_t g_qlock_wait_ns, g_qlock_waits;
static uint64_t qlock_now_ns(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec; }
static void qlock(pthread_mutex_t *m) {
    if (!pthread_mutex_trylock(m)) return;
    uint64_t t0 = qlock_now_ns();
    pthread_mutex_lock(m);
    atomic_fetch_add_explicit(&g_qlock_wait_ns, qlock_now_ns() - t0, memory_order_relaxed);
    atomic_fetch_add_explicit(&g_qlock_waits, 1, memory_order_relaxed);
}
void vk_queue_lock(void) { qlock(&queue_mu); }
void vk_queue_unlock(void) { pthread_mutex_unlock(&queue_mu); }
void vk_queue_lock_stats(uint64_t *wait_ns, uint64_t *waits) {
    *wait_ns = atomic_load_explicit(&g_qlock_wait_ns, memory_order_relaxed);
    *waits = atomic_load_explicit(&g_qlock_waits, memory_order_relaxed);
}
// [UIT 0248] TOTK_VK_PRESENT_QUEUE (on unless 0; launcher: Performance > CPU+): the present thread's submit and
// vkQueuePresentKHR on a second queue of the same family (when the family has two), under a lock of its own, so the
// replay's vkQueueSubmit2 never waits behind a present that blocks in the driver. Same family: no ownership transfer;
// the present's submit already waits on the frame's timeline value (a semaphore works across queues). =0, or a family
// with one queue: one queue and one lock for both, as before.
static pthread_mutex_t pqueue_mu = PTHREAD_MUTEX_INITIALIZER;
void vk_pqueue_lock(void) { qlock(vkc.pqueue_own ? &pqueue_mu : &queue_mu); }
void vk_pqueue_unlock(void) { pthread_mutex_unlock(vkc.pqueue_own ? &pqueue_mu : &queue_mu); }

static void device_fault_report(void);
int vk_check(VkResult r, const char *what) {
    if (r == VK_SUCCESS) return 1;
    LOG("%s failed (%d)", what, (int)r);
    // [UIT 0355] the first lost device any call sees: the driver's own report of it (VK_EXT_device_fault)
    static atomic_int lost_reported;
    if (r == VK_ERROR_DEVICE_LOST && !atomic_exchange(&lost_reported, 1)) device_fault_report();
    return 0;
}

static int has_layer(const char *name) {
    uint32_t n = 0;
    vk.vkEnumerateInstanceLayerProperties(&n, NULL);
    VkLayerProperties *p = calloc(n ? n : 1, sizeof *p);
    vk.vkEnumerateInstanceLayerProperties(&n, p);
    int found = 0;
    for (uint32_t i = 0; i < n; i++) if (!strcmp(p[i].layerName, name)) found = 1;
    free(p);
    return found;
}

static int create_instance(SDL_Window *window) {
    PFN_vkGetInstanceProcAddr gipa = NULL;
    if (window) gipa = (PFN_vkGetInstanceProcAddr)SDL_Vulkan_GetVkGetInstanceProcAddr();
    else {   // no window (TOTK_HEADLESS): SDL's video subsystem is not up, so the loader is loaded here
        HMODULE m = LoadLibraryA("vulkan-1.dll");
        if (m) gipa = (PFN_vkGetInstanceProcAddr)(void *)GetProcAddress(m, "vkGetInstanceProcAddr");
    }
    if (!gipa) { LOG("no Vulkan loader (vulkan-1.dll)"); return 0; }
    gipa = dlss_vk_loader(gipa);   // TOTK_UPSCALER=dlss: NVIDIA Streamline's interposer when it is there, else unchanged
    vkc.gipa = gipa;   // [UIT 0075] the memory budget query (src/gfx/vkb_mem.c)
#define LOAD(f) vk.f = (PFN_##f)gipa(NULL, #f);
    VK_FN_GLOBAL(LOAD)
#undef LOAD
    Uint32 next = 0;
    const char *const *exts = window ? SDL_Vulkan_GetInstanceExtensions(&next) : NULL;
    // TOTK_VK_VALIDATION=1: the Khronos validation layer (Vulkan SDK), for development
    const char *layer = "VK_LAYER_KHRONOS_validation";
    const char *v = getenv("TOTK_VK_VALIDATION");
    int validate = v && strcmp(v, "0") && has_layer(layer);
    if (v && strcmp(v, "0") && !validate) LOG("TOTK_VK_VALIDATION: %s not installed", layer);
    VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "Ultra Instinct Tears",
                             .apiVersion = VK_API_VERSION_1_3};
    VkInstanceCreateInfo ci = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app,
                               .enabledExtensionCount = next, .ppEnabledExtensionNames = exts,
                               .enabledLayerCount = validate ? 1u : 0u, .ppEnabledLayerNames = &layer};
    if (!vk_check(vk.vkCreateInstance(&ci, NULL, &vkc.instance), "vkCreateInstance")) return 0;
#define LOAD(f) vk.f = (PFN_##f)gipa(vkc.instance, #f);
    VK_FN_INSTANCE(LOAD)
#undef LOAD
    if (validate) LOG("validation layer on");
    if (window && !SDL_Vulkan_CreateSurface(window, vkc.instance, NULL, &vkc.surface)) {
        LOG("window surface: %s", SDL_GetError());
        vkc.surface = VK_NULL_HANDLE;
    }
    return 1;
}

typedef struct { VkExtensionProperties *e; uint32_t n; } ExtList;
static ExtList device_extensions(VkPhysicalDevice p) {
    ExtList l = {0};
    vk.vkEnumerateDeviceExtensionProperties(p, NULL, &l.n, NULL);
    l.e = calloc(l.n ? l.n : 1, sizeof *l.e);
    vk.vkEnumerateDeviceExtensionProperties(p, NULL, &l.n, l.e);
    return l;
}
static int ext_in(const ExtList *l, const char *name) {
    for (uint32_t i = 0; i < l->n; i++) if (!strcmp(l->e[i].extensionName, name)) return 1;
    return 0;
}

// A Vulkan 1.3 GPU with a queue family for graphics and compute (and presentation, with a window); discrete first.
static int pick_device(void) {
    uint32_t n = 0;
    vk.vkEnumeratePhysicalDevices(vkc.instance, &n, NULL);
    if (!n) { LOG("no Vulkan GPU"); return 0; }
    VkPhysicalDevice *all = calloc(n, sizeof *all);
    vk.vkEnumeratePhysicalDevices(vkc.instance, &n, all);
    int best = -1;
    for (uint32_t i = 0; i < n; i++) {
        VkPhysicalDeviceProperties pr;
        vk.vkGetPhysicalDeviceProperties(all[i], &pr);
        ExtList ex = device_extensions(all[i]);
        int ok = pr.apiVersion >= VK_API_VERSION_1_3 && ext_in(&ex, VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME) &&
                 (!vkc.surface || ext_in(&ex, VK_KHR_SWAPCHAIN_EXTENSION_NAME));
        free(ex.e);
        if (!ok) continue;
        uint32_t nq = 0;
        vk.vkGetPhysicalDeviceQueueFamilyProperties(all[i], &nq, NULL);
        VkQueueFamilyProperties *q = calloc(nq, sizeof *q);
        vk.vkGetPhysicalDeviceQueueFamilyProperties(all[i], &nq, q);
        for (uint32_t k = 0; k < nq; k++) {
            VkBool32 present = VK_TRUE;
            if (vkc.surface) vk.vkGetPhysicalDeviceSurfaceSupportKHR(all[i], k, vkc.surface, &present);
            if ((q[k].queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) != (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT) || !present)
                continue;
            int score = pr.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 2 : 1;
            if (best < score) { best = score; vkc.gpu = all[i]; vkc.family = k; }
            break;
        }
        free(q);
    }
    free(all);
    if (best < 0) { LOG("no Vulkan 1.3 GPU with VK_EXT_external_memory_host%s", vkc.surface ? " that presents to the window" : ""); return 0; }
    vk.vkGetPhysicalDeviceProperties(vkc.gpu, &vkc.props);
    vk.vkGetPhysicalDeviceMemoryProperties(vkc.gpu, &vkc.memory);
    vkc.db = (VkPhysicalDeviceDescriptorBufferPropertiesEXT){VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_BUFFER_PROPERTIES_EXT};
    VkPhysicalDeviceExternalMemoryHostPropertiesEXT host = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_MEMORY_HOST_PROPERTIES_EXT, &vkc.db};
    VkPhysicalDeviceProperties2 p2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &host};
    vk.vkGetPhysicalDeviceProperties2(vkc.gpu, &p2);
    vkc.host_pointer_alignment = host.minImportedHostPointerAlignment;
    { ExtList ex = device_extensions(vkc.gpu); vkc.has_memory_budget = ext_in(&ex, VK_EXT_MEMORY_BUDGET_EXTENSION_NAME); free(ex.e); }
    LOG("GPU: %s (Vulkan %u.%u.%u, driver %#x)", vkc.props.deviceName, VK_API_VERSION_MAJOR(vkc.props.apiVersion),
        VK_API_VERSION_MINOR(vkc.props.apiVersion), VK_API_VERSION_PATCH(vkc.props.apiVersion), vkc.props.driverVersion);
    return 1;
}

// Feature structs: queried with everything the GPU supports and passed back unchanged to vkCreateDevice.
static VkPhysicalDeviceVulkan11Features f11 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};
static VkPhysicalDeviceVulkan12Features f12 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
static VkPhysicalDeviceVulkan13Features f13 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
static VkPhysicalDeviceExtendedDynamicState3FeaturesEXT feds3 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_3_FEATURES_EXT};
static VkPhysicalDeviceVertexInputDynamicStateFeaturesEXT fvids = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_INPUT_DYNAMIC_STATE_FEATURES_EXT};
static VkPhysicalDeviceDescriptorBufferFeaturesEXT fdb = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_BUFFER_FEATURES_EXT};
static VkPhysicalDeviceGraphicsPipelineLibraryFeaturesEXT fgpl = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_GRAPHICS_PIPELINE_LIBRARY_FEATURES_EXT};
static VkPhysicalDeviceDepthClipControlFeaturesEXT fdcc = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_CLIP_CONTROL_FEATURES_EXT};
static VkPhysicalDeviceDepthClipEnableFeaturesEXT fdce = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_CLIP_ENABLE_FEATURES_EXT};
#ifdef VK_EXT_device_fault
static VkPhysicalDeviceFaultFeaturesEXT ffault = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FAULT_FEATURES_EXT};
static int has_fault;
#endif
static VkPhysicalDeviceCustomBorderColorFeaturesEXT fcbc = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CUSTOM_BORDER_COLOR_FEATURES_EXT};
static VkPhysicalDeviceProvokingVertexFeaturesEXT fpv = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROVOKING_VERTEX_FEATURES_EXT};
static VkPhysicalDeviceConditionalRenderingFeaturesEXT fcr = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CONDITIONAL_RENDERING_FEATURES_EXT};
static VkPhysicalDeviceIndexTypeUint8FeaturesEXT fu8 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_INDEX_TYPE_UINT8_FEATURES_EXT};
static VkPhysicalDevicePrimitiveTopologyListRestartFeaturesEXT fplr = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRIMITIVE_TOPOLOGY_LIST_RESTART_FEATURES_EXT};
static VkPhysicalDeviceShaderObjectFeaturesEXT fso = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_OBJECT_FEATURES_EXT};
static VkPhysicalDeviceFragmentShaderInterlockFeaturesEXT ffsi = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADER_INTERLOCK_FEATURES_EXT};
static VkPhysicalDeviceAttachmentFeedbackLoopLayoutFeaturesEXT ffl = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ATTACHMENT_FEEDBACK_LOOP_LAYOUT_FEATURES_EXT};
static VkPhysicalDeviceAttachmentFeedbackLoopDynamicStateFeaturesEXT ffld = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ATTACHMENT_FEEDBACK_LOOP_DYNAMIC_STATE_FEATURES_EXT};
static int has_fl, has_fld;
static VkPhysicalDeviceDynamicRenderingLocalReadFeaturesKHR flr = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_LOCAL_READ_FEATURES_KHR};
static int has_lr;
#ifdef VK_EXT_depth_bias_control
static VkPhysicalDeviceDepthBiasControlFeaturesEXT fdbc = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_BIAS_CONTROL_FEATURES_EXT};
static int has_dbc;
#endif

// [UIT 0136] The device as it is about to be created, for the shader binary cache key (vkc.dev_fp): the GPU's vendor,
// device and driver version, the enabled extensions in order, the core features, and the body of every feature struct
// in the chain (without sType / pNext: pointers differ from run to run). A binary made on a device with another
// extension or feature set (TOTK_VK_DEPTH_BIAS_FIX, TOTK_VK_CHECKPOINTS, TOTK_VK_ROBUST, a driver update the binary
// UUID / version do not show) is never loaded on this one.
static uint64_t fp_mix(uint64_t h, const void *p, size_t n) {
    const uint8_t *b = p;
    for (size_t i = 0; i < n; i++) { h ^= b[i]; h *= 0x100000001b3ull; }
    return h;
}
static int fp_skip_name(const char *name) {
#ifdef VK_EXT_device_fault
    return !strcmp(name, VK_EXT_DEVICE_FAULT_EXTENSION_NAME);
#else
    (void)name; return 0;
#endif
}
static uint64_t device_fingerprint(const VkPhysicalDeviceFeatures2 *f2, const char *const *names, uint32_t nnames) {
    static const struct { const void *p; size_t n; } sz[] = {
#define S(x) {&x, sizeof x}
        S(f11), S(f12), S(f13), S(feds3), S(fvids), S(fdb), S(fgpl), S(fdcc), S(fdce), S(fcbc), S(fpv), S(fcr), S(fu8), S(fplr),
        S(fso), S(ffsi), S(ffl), S(ffld), S(flr),
#ifdef VK_EXT_depth_bias_control
        S(fdbc),
#endif
#undef S
    };
    uint64_t h = fp_mix(0xcbf29ce484222325ull, "UIT-dev-1", 9);
    h = fp_mix(h, &vkc.props.vendorID, sizeof vkc.props.vendorID);
    h = fp_mix(h, &vkc.props.deviceID, sizeof vkc.props.deviceID);
    h = fp_mix(h, &vkc.props.driverVersion, sizeof vkc.props.driverVersion);
    h = fp_mix(h, &vkc.props.apiVersion, sizeof vkc.props.apiVersion);
    // [UIT 0355] VK_EXT_device_fault changes no compiled shader: left out, so enabling it keeps the cached binaries
    uint32_t nkey = 0;
    for (uint32_t i = 0; i < nnames; i++) nkey += !fp_skip_name(names[i]);
    h = fp_mix(h, &nkey, sizeof nkey);
    for (uint32_t i = 0; i < nnames; i++) if (!fp_skip_name(names[i])) h = fp_mix(h, names[i], strlen(names[i]) + 1);
    h = fp_mix(h, &f2->features, sizeof f2->features);
    for (const VkBaseOutStructure *s = f2->pNext; s; s = s->pNext) {
#ifdef VK_EXT_device_fault
        if ((const void *)s == (const void *)&ffault) continue;
#endif
        size_t n = 0;
        for (unsigned i = 0; i < sizeof sz / sizeof *sz && !n; i++) if (sz[i].p == (const void *)s) n = sz[i].n;
        h = fp_mix(h, &s->sType, sizeof s->sType);
        if (n > sizeof *s) h = fp_mix(h, (const uint8_t *)s + sizeof *s, n - sizeof *s);
    }
    return h;
}

static int create_device(void) {
    ExtList ex = device_extensions(vkc.gpu);
    const char *names[32];
    uint32_t nnames = 0;
    VkPhysicalDeviceFeatures2 f2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &f11};
    f11.pNext = &f12;
    f12.pNext = &f13;
    void **tail = &f13.pNext;
    // extension name, its feature struct (NULL: none), the flag in vkc
    struct { const char *name; void *features; int *flag; } want[] = {
        {VK_KHR_SWAPCHAIN_EXTENSION_NAME, NULL, NULL},
        {VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME, NULL, NULL},
        {VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME, NULL, NULL},
        {VK_EXT_DESCRIPTOR_BUFFER_EXTENSION_NAME, &fdb, &vkc.has_descriptor_buffer},
        {VK_KHR_PIPELINE_LIBRARY_EXTENSION_NAME, NULL, NULL},
        {VK_EXT_GRAPHICS_PIPELINE_LIBRARY_EXTENSION_NAME, &fgpl, &vkc.has_gpl},
        {VK_EXT_EXTENDED_DYNAMIC_STATE_3_EXTENSION_NAME, &feds3, &vkc.has_eds3},
        {VK_EXT_VERTEX_INPUT_DYNAMIC_STATE_EXTENSION_NAME, &fvids, &vkc.has_vertex_input_ds},
        {VK_EXT_DEPTH_CLIP_CONTROL_EXTENSION_NAME, &fdcc, &vkc.has_depth_clip_control},
        {VK_EXT_DEPTH_CLIP_ENABLE_EXTENSION_NAME, &fdce, NULL},
        {VK_EXT_CUSTOM_BORDER_COLOR_EXTENSION_NAME, &fcbc, NULL},
        {VK_EXT_PROVOKING_VERTEX_EXTENSION_NAME, &fpv, NULL},
        {VK_EXT_CONDITIONAL_RENDERING_EXTENSION_NAME, &fcr, NULL},
        {VK_EXT_INDEX_TYPE_UINT8_EXTENSION_NAME, &fu8, NULL},
        {VK_EXT_PRIMITIVE_TOPOLOGY_LIST_RESTART_EXTENSION_NAME, &fplr, NULL},
        {VK_EXT_SHADER_OBJECT_EXTENSION_NAME, &fso, &vkc.has_shader_object},
        {VK_EXT_FRAGMENT_SHADER_INTERLOCK_EXTENSION_NAME, &ffsi, &vkc.has_interlock},
        // TOTK samples textures that are attachments of the same pass (G-buffer decals, depth-reading effects)
        {VK_EXT_ATTACHMENT_FEEDBACK_LOOP_LAYOUT_EXTENSION_NAME, &ffl, &has_fl},
        {VK_EXT_ATTACHMENT_FEEDBACK_LOOP_DYNAMIC_STATE_EXTENSION_NAME, &ffld, &has_fld},
        // ...and a draw that samples a target earlier draws of the pass wrote waits for them inside the pass
        {VK_KHR_DYNAMIC_RENDERING_LOCAL_READ_EXTENSION_NAME, &flr, &has_lr},
        // development (TOTK_VK_CHECKPOINTS=1): the markers a lost device reached
        {VK_NV_DEVICE_DIAGNOSTIC_CHECKPOINTS_EXTENSION_NAME, NULL, &vkc.has_checkpoints},
        // [UIT 0075] the VRAM budget (a query; src/gfx/vkb_mem.c keeps its background blocks under 85% of it)
        {VK_EXT_MEMORY_BUDGET_EXTENSION_NAME, NULL, &vkc.has_memory_budget},
#ifdef VK_EXT_depth_bias_control
        // [UIT 0123] depth bias in depth units: D16 / D24 shadow maps are float buffers here (src/gfx/vkb_tex.c)
        {VK_EXT_DEPTH_BIAS_CONTROL_EXTENSION_NAME, &fdbc, &has_dbc},
#endif
#ifdef VK_EXT_device_fault
        // [UIT 0355] the driver's report of a lost device: what failed, at which GPU addresses (read only after a loss)
        {VK_EXT_DEVICE_FAULT_EXTENSION_NAME, &ffault, &has_fault},
#endif
    };
    for (unsigned i = 0; i < sizeof want / sizeof *want; i++) {
        if (!strcmp(want[i].name, VK_KHR_SWAPCHAIN_EXTENSION_NAME) && !vkc.surface) continue;
#ifdef VK_EXT_depth_bias_control
        // [UIT 0134] TOTK_VK_DEPTH_BIAS_FIX=0: the extension is not enabled either, so the device is created exactly as
        // before 0123 (until 0134 it was enabled whatever the setting: 22 extensions instead of 21 in the 2026-10-31 logs)
        if (want[i].flag == &has_dbc) { const char *e = getenv("TOTK_VK_DEPTH_BIAS_FIX"); if (e && e[0] == '0' && !e[1]) continue; }
#endif
        if (want[i].flag == &vkc.has_checkpoints && !getenv("TOTK_VK_CHECKPOINTS")) continue;
#ifdef VK_EXT_device_fault
        if (want[i].flag == &has_fault) { const char *e = getenv("TOTK_VK_DEVICE_FAULT"); if (e && e[0] == '0' && !e[1]) continue; }
#endif
        if (!ext_in(&ex, want[i].name)) { LOG("extension %s not available", want[i].name); continue; }
        names[nnames++] = want[i].name;
        if (want[i].flag) *want[i].flag = 1;
        if (want[i].features) { *tail = want[i].features; tail = (void **)&((VkBaseOutStructure *)want[i].features)->pNext; }
    }
    free(ex.e);
    vk.vkGetPhysicalDeviceFeatures2(vkc.gpu, &f2);
    vkc.has_aniso = f2.features.samplerAnisotropy;   // [UIT 0066] (all supported core features are enabled below)
    vkc.has_wide_lines = f2.features.wideLines;   // [UIT 0203]
    vkc.has_pv_last = fpv.provokingVertexLast;   // [UIT 0205] (stays 0 when the extension is not available: not chained)
    // [UIT 0352] translated compute shaders that store to an image of unknown format declare this capability
    if (!f2.features.shaderStorageImageWriteWithoutFormat) LOG("no shaderStorageImageWriteWithoutFormat: shaders storing to images of unknown format may fail");
    vkc.has_shader_object = vkc.has_shader_object && fso.shaderObject;
#ifdef VK_EXT_device_fault
    has_fault = has_fault && ffault.deviceFault;
    ffault.deviceFaultVendorBinary = VK_FALSE;   // the description, addresses and vendor codes are enough for the log
    LOG("[UIT 0355] driver reports of a lost device (VK_EXT_device_fault): %s", has_fault ? "on" :
        getenv("TOTK_VK_DEVICE_FAULT") && !strcmp(getenv("TOTK_VK_DEVICE_FAULT"), "0") ? "off (TOTK_VK_DEVICE_FAULT=0)" : "not supported");
#endif
    if (!vkc.has_shader_object) {
        LOG("VK_EXT_shader_object is required but is not supported by this GPU or driver");
        return 0;
    }
    vkc.has_feedback_loop = has_fl && has_fld && ffl.attachmentFeedbackLoopLayout && ffld.attachmentFeedbackLoopDynamicState;
    if (!vkc.has_feedback_loop) LOG("no attachment feedback loops: textures that are attachments of their pass read empty");
    vkc.has_local_read = has_lr && flr.dynamicRenderingLocalRead;
#ifdef VK_EXT_depth_bias_control
    vkc.has_depth_bias_float = has_dbc && fdbc.depthBiasControl && fdbc.floatRepresentation;
#endif
    if (!vkc.has_depth_bias_float) LOG("no float depth bias (VK_EXT_depth_bias_control): D16 shadow bias scaled by 256 instead");
    if (vkc.has_feedback_loop && !vkc.has_local_read) LOG("no barriers inside a pass: a draw sampling its own target may miss earlier draws' writes");
    // TOTK_VK_ROBUST=1 (development): bounds-checked buffer access, to tell a shader's out-of-range access from other
    // causes of a lost device
    if (!getenv("TOTK_VK_ROBUST")) f2.features.robustBufferAccess = VK_FALSE;
    else LOG("robust buffer access %s (TOTK_VK_ROBUST)", f2.features.robustBufferAccess ? "on" : "not supported");
    vkc.robust = getenv("TOTK_VK_ROBUST") && f2.features.robustBufferAccess;
    vkc.has_descriptor_buffer = vkc.has_descriptor_buffer && fdb.descriptorBuffer;
    if (vkc.has_descriptor_buffer) fdb.descriptorBufferCaptureReplay = VK_FALSE;   // tools only, costs driver work
    if (vkc.has_descriptor_buffer)
        LOG("descriptor buffer: uniform %zu, storage %zu, combined image sampler %zu, storage image %zu bytes; offset alignment %llu; "
            "ranges resource %llu / sampler %llu", vkc.db.uniformBufferDescriptorSize, vkc.db.storageBufferDescriptorSize,
            vkc.db.combinedImageSamplerDescriptorSize, vkc.db.storageImageDescriptorSize, (unsigned long long)vkc.db.descriptorBufferOffsetAlignment,
            (unsigned long long)vkc.db.maxResourceDescriptorBufferRange, (unsigned long long)vkc.db.maxSamplerDescriptorBufferRange);
    fdb.descriptorBufferImageLayoutIgnored = fdb.descriptorBufferImageLayoutIgnored && vkc.has_descriptor_buffer;
    vkc.dev_fp = device_fingerprint(&f2, names, nnames);
    LOG("device fingerprint %016llx (shader binary cache key)", (unsigned long long)vkc.dev_fp);
    float prio[2] = {1.0f, 1.0f};
    uint32_t fam_queues = 1;   // [UIT 0248] the chosen family's queue count
    {
        uint32_t nq = 0;
        vk.vkGetPhysicalDeviceQueueFamilyProperties(vkc.gpu, &nq, NULL);
        VkQueueFamilyProperties *q = calloc(nq ? nq : 1, sizeof *q);
        if (q) { vk.vkGetPhysicalDeviceQueueFamilyProperties(vkc.gpu, &nq, q); if (vkc.family < nq) fam_queues = q[vkc.family].queueCount; free(q); }
    }
    const char *pqe = getenv("TOTK_VK_PRESENT_QUEUE");
    int pq_want = !(pqe && pqe[0] == '0' && !pqe[1]);
    int pq_two = pq_want && vkc.surface && fam_queues >= 2;
    VkDeviceQueueCreateInfo qi = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = vkc.family,
                                  .queueCount = pq_two ? 2u : 1u, .pQueuePriorities = prio};
    VkDeviceCreateInfo ci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .pNext = &f2, .queueCreateInfoCount = 1,
                             .pQueueCreateInfos = &qi, .enabledExtensionCount = nnames, .ppEnabledExtensionNames = names};
    if (!vk_check(vk.vkCreateDevice(vkc.gpu, &ci, NULL, &vkc.device), "vkCreateDevice")) return 0;
#define LOAD(f) vk.f = (PFN_##f)vk.vkGetDeviceProcAddr(vkc.device, #f);
    VK_FN_DEVICE(LOAD)
#undef LOAD
#ifdef VK_EXT_depth_bias_control
    if (vkc.has_depth_bias_float)
        vkc.cmd_set_depth_bias2 = (PFN_vkCmdSetDepthBias2EXT)vk.vkGetDeviceProcAddr(vkc.device, "vkCmdSetDepthBias2EXT");
    if (!vkc.cmd_set_depth_bias2) vkc.has_depth_bias_float = 0;
#endif
    vk.vkGetDeviceQueue(vkc.device, vkc.family, 0, &vkc.queue);
    vkc.pqueue = vkc.queue;
    if (pq_two) { vk.vkGetDeviceQueue(vkc.device, vkc.family, 1, &vkc.pqueue); vkc.pqueue_own = vkc.pqueue != VK_NULL_HANDLE; }
    if (!vkc.pqueue_own) vkc.pqueue = vkc.queue;
    LOG("[UIT 0248] presents: %s (TOTK_VK_PRESENT_QUEUE%s; the family has %u queue%s)",
        vkc.pqueue_own ? "on a second queue with a lock of its own" : "on the replay's queue, one lock as before",
        pq_want ? ", default" : "=0", fam_queues, fam_queues == 1 ? "" : "s");
    dlss_device_created(vkc.gpu);
    LOG("device: %u extensions; descriptor buffer %d, pipeline library %d, dynamic state 3 %d, vertex input %d, "
        "timeline semaphores %d, host pointer alignment %#llx", nnames, vkc.has_descriptor_buffer, vkc.has_gpl,
        vkc.has_eds3, vkc.has_vertex_input_ds, f12.timelineSemaphore, (unsigned long long)vkc.host_pointer_alignment);
    if (!f12.timelineSemaphore || !f13.dynamicRendering || !f13.synchronization2) { LOG("GPU lacks Vulkan 1.3 core features"); return 0; }
    return 1;
}

int vk_init(SDL_Window *window) {
    static int state;   // 0 not tried, 1 ready, -1 failed
    static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_lock(&mu);
    if (!state) state = create_instance(window) && pick_device() && create_device() ? 1 : -1;
    pthread_mutex_unlock(&mu);
    return state == 1;
}

/* [UIT 0355] After the first VK_ERROR_DEVICE_LOST: the driver's description of the fault, the GPU addresses involved and
   the vendor's codes, into the log (VK_EXT_device_fault; NVIDIA, AMD and Mesa drivers have it). Nothing is read before
   a loss, so this costs nothing while the game runs. TOTK_VK_DEVICE_FAULT=0: the extension is not enabled. */
static void device_fault_report(void) {
#ifdef VK_EXT_device_fault
    static PFN_vkGetDeviceFaultInfoEXT get;
    if (!has_fault || !vkc.device) { LOG("device lost: no driver fault report (VK_EXT_device_fault %s)", has_fault ? "not ready" : "not enabled"); return; }
    if (!get) get = (PFN_vkGetDeviceFaultInfoEXT)vk.vkGetDeviceProcAddr(vkc.device, "vkGetDeviceFaultInfoEXT");
    if (!get) { LOG("device lost: vkGetDeviceFaultInfoEXT missing"); return; }
    VkDeviceFaultCountsEXT n = {VK_STRUCTURE_TYPE_DEVICE_FAULT_COUNTS_EXT};
    VkResult r = get(vkc.device, &n, NULL);
    if (r != VK_SUCCESS) { LOG("device lost: the driver gave no fault report (%d)", (int)r); return; }
    enum { MAXN = 16 };
    VkDeviceFaultAddressInfoEXT addr[MAXN];
    VkDeviceFaultVendorInfoEXT ven[MAXN];
    memset(addr, 0, sizeof addr);
    memset(ven, 0, sizeof ven);
    uint32_t na = n.addressInfoCount, nv = n.vendorInfoCount;
    if (n.addressInfoCount > MAXN) n.addressInfoCount = MAXN;
    if (n.vendorInfoCount > MAXN) n.vendorInfoCount = MAXN;
    n.vendorBinarySize = 0;
    VkDeviceFaultInfoEXT info = {VK_STRUCTURE_TYPE_DEVICE_FAULT_INFO_EXT, .pAddressInfos = addr, .pVendorInfos = ven};
    r = get(vkc.device, &n, &info);
    if (r != VK_SUCCESS && r != VK_INCOMPLETE) { LOG("device lost: the driver gave no fault report (%d)", (int)r); return; }
    LOG("device fault: %.*s (%u address%s, %u vendor record%s)", (int)sizeof info.description,
        info.description[0] ? info.description : "(no description)", na, na == 1 ? "" : "es", nv, nv == 1 ? "" : "s");
    static const char *const kind[] = {"none", "invalid read", "invalid write", "invalid execute", "instruction pointer unknown",
                                       "instruction pointer invalid", "instruction pointer fault"};
    for (uint32_t i = 0; i < n.addressInfoCount; i++)
        LOG("  fault address %#llx (precision %#llx): %s", (unsigned long long)addr[i].reportedAddress,
            (unsigned long long)addr[i].addressPrecision,
            (unsigned)addr[i].addressType < sizeof kind / sizeof *kind ? kind[addr[i].addressType] : "unknown type");
    for (uint32_t i = 0; i < n.vendorInfoCount; i++)
        LOG("  vendor record: %.*s (code %#llx, data %#llx)", (int)sizeof ven[i].description, ven[i].description,
            (unsigned long long)ven[i].vendorFaultCode, (unsigned long long)ven[i].vendorFaultData);
#else
    LOG("device lost: no driver fault report (built without VK_EXT_device_fault)");
#endif
}
