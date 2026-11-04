// NVIDIA Streamline glue for DLSS (see dlss.h). Plain C on Streamline's C-compatible ABI (sl_abi.h): it builds with
// the builder package's toolchain as it is, with no Streamline headers and no C++ standard library, so every build has
// DLSS unless it is built with TOTK_NO_DLSS (tools/setup.py --no-dlss), which leaves stubs that say "not built".
// Streamline is never linked: every entry point comes from sl.interposer.dll with GetProcAddress, so the exe starts
// the same with or without the DLLs (the user downloads the SDK; tools/setup.py copies its DLLs next to the exe).
#include "dlss.h"

#ifndef TOTK_NO_DLSS
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "sl_abi.h"

#include "../runtime/logline.h"   // [UIT 0362] uit_logline: one write per line
#define LOG(...) uit_logline("[totk-dlss] " __VA_ARGS__)

static HMODULE g_dll;
static slc_PFN_slInit *p_init;
static slc_PFN_slIsFeatureSupported *p_supported;
static slc_PFN_slEvaluateFeature *p_evaluate;
static slc_PFN_slSetTagForFrame *p_tag;
static slc_PFN_slSetConstants *p_constants;
static slc_PFN_slGetFeatureFunction *p_feature_fn;
static slc_PFN_slGetNewFrameToken *p_token;
static slc_PFN_slDLSSGetOptimalSettings *p_optimal;
static slc_PFN_slDLSSSetOptions *p_options;
static int g_up;                          // slInit succeeded
static wchar_t g_dir[MAX_PATH];

// [native-1031] Streamline messages that are expected with this integration, shown as one plain line (TOTK_DLSS_LOG=2
// shows them as they are):
//  - sl.common asks for vkCmdBindPipeline / vkCmdBindDescriptorSets / vkBeginCommandBuffer hooks that Streamline 2.x's
//    Vulkan interposer has no entry for (pluginManager.cpp processPluginHooks; sl_hooks.h lists only the present /
//    swapchain / device ones). sl.common uses them only for command-buffer state tracking, which is turned off here
//    (SLC_PREF_DISABLE_CL_STATE_TRACKING: the replay restores its own state after the evaluate), so DLSS is unaffected;
//  - VK_EXT_debug_utils not enabled: only Streamline's debug names and profiler markers; the extension is not needed
//    to play and is left off.
static int sl_expected(const char *msg) {
    static const char *const known[] = {"Hook sl.common:Vulkan:CmdBindPipeline is NOT supported",
                                        "Hook sl.common:Vulkan:CmdBindDescriptorSets is NOT supported",
                                        "Hook sl.common:Vulkan:BeginCommandBuffer is NOT supported",
                                        "VK_EXT_debug_utils extension not enabled"};
    for (unsigned i = 0; i < sizeof known / sizeof *known; i++) if (strstr(msg, known[i])) return 1;
    return 0;
}

static void sl_log(uint32_t type, const char *msg) {
    if (type == SLC_LOGTYPE_INFO || !msg) return;
    static int verbose = -1;
    if (verbose < 0) { const char *lv = getenv("TOTK_DLSS_LOG"); verbose = lv && *lv == '2'; }
    if (!verbose && sl_expected(msg)) {
        static volatile long said;
        if (!InterlockedExchange(&said, 1))
            LOG("streamline: its usual notes about Vulkan hooks it does not need here and about debug names are expected "
                "and harmless (DLSS is unaffected); TOTK_DLSS_LOG=2 shows them");
        return;
    }
    size_t n = strlen(msg);
    while (n && (msg[n - 1] == '\n' || msg[n - 1] == '\r')) n--;
    fprintf(stderr, "[totk-dlss] streamline: %.*s\n", (int)n, msg);
}

static void *sym(const char *name) {
    void *p = (void *)GetProcAddress(g_dll, name);
    if (!p) LOG("sl.interposer.dll has no %s (an older or newer Streamline than 2.x?)", name);
    return p;
}

int dlss_sl_built(void) { return 1; }

PFN_vkGetInstanceProcAddr dlss_sl_load(const wchar_t *dir) {
    if (g_up) return (PFN_vkGetInstanceProcAddr)(void *)GetProcAddress(g_dll, "vkGetInstanceProcAddr");
    wcsncpy(g_dir, dir, MAX_PATH - 1);
    wchar_t path[MAX_PATH + 32];
    swprintf(path, sizeof path / sizeof *path, L"%ls\\sl.interposer.dll", g_dir);
    // the DLL's own folder first, for sl.common.dll and the plugins next to it
    g_dll = LoadLibraryExW(path, NULL, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (!g_dll) {
        LOG("sl.interposer.dll not found next to the exe (error %lu): DLSS off, FSR as before. Copy the four DLLs of "
            "Streamline 2.14.16's bin\\x64 (sl.interposer, sl.common, sl.dlss, nvngx_dlss) next to UltraInstinctTears.exe, or put "
            "the SDK in deps\\streamline and run setup.py again (setup.py --get-dlss fetches it)", GetLastError());
        return NULL;
    }
    PFN_vkGetInstanceProcAddr gipa = NULL;
    if (!(p_init = (slc_PFN_slInit *)sym("slInit")) ||
        !(p_supported = (slc_PFN_slIsFeatureSupported *)sym("slIsFeatureSupported")) ||
        !(p_evaluate = (slc_PFN_slEvaluateFeature *)sym("slEvaluateFeature")) ||
        !(p_tag = (slc_PFN_slSetTagForFrame *)sym("slSetTagForFrame")) ||
        !(p_constants = (slc_PFN_slSetConstants *)sym("slSetConstants")) ||
        !(p_feature_fn = (slc_PFN_slGetFeatureFunction *)sym("slGetFeatureFunction")) ||
        !(p_token = (slc_PFN_slGetNewFrameToken *)sym("slGetNewFrameToken")) ||
        !(gipa = (PFN_vkGetInstanceProcAddr)sym("vkGetInstanceProcAddr"))) {
        FreeLibrary(g_dll); g_dll = NULL; return NULL;
    }
    slc_Preferences pref;
    slc_preferences_init(&pref);
    static const wchar_t *paths[1];
    paths[0] = g_dir;
    pref.pathsToPlugins = paths;
    pref.numPathsToPlugins = 1;
    pref.showConsole = 0;
    const char *lv = getenv("TOTK_DLSS_LOG");
    pref.logLevel = lv && *lv == '2' ? SLC_LOG_VERBOSE : SLC_LOG_DEFAULT;
    pref.logMessageCallback = sl_log;
    // the replay restores its own command buffer state after the evaluate; tags per frame token; no downloads
    pref.flags = SLC_PREF_DISABLE_CL_STATE_TRACKING | SLC_PREF_USE_FRAME_BASED_RESOURCE_TAGGING;
    static const slc_Feature features[] = {SLC_FEATURE_DLSS};
    pref.featuresToLoad = features;
    pref.numFeaturesToLoad = 1;
    pref.engine = SLC_ENGINE_CUSTOM;
    pref.engineVersion = "Ultra Instinct Tears";
    pref.projectId = "5f0e3c2a-7d6b-4e8f-9a1c-2b3d4e5f6a70";
    pref.renderAPI = SLC_API_VULKAN;
    slc_Result r = p_init(&pref, SLC_SDK_VERSION);
    if (r != SLC_OK) {
        LOG("slInit failed (%d %s): DLSS off, FSR as before (the DLLs should be Streamline %d.%d.%d's; TOTK_DLSS_LOG=2 "
            "shows Streamline's own messages)", (int)r, slc_result_name(r), SLC_VERSION_MAJOR, SLC_VERSION_MINOR, SLC_VERSION_PATCH);
        FreeLibrary(g_dll); g_dll = NULL; return NULL;
    }
    g_up = 1;
    LOG("Streamline loaded (SDK %d.%d.%d)", SLC_VERSION_MAJOR, SLC_VERSION_MINOR, SLC_VERSION_PATCH);
    return gipa;
}

int dlss_sl_supported(VkPhysicalDevice gpu) {
    if (!g_up) return 0;
    slc_AdapterInfo ai;
    slc_adapter_info_init(&ai);
    ai.vkPhysicalDevice = (void *)gpu;
    slc_Result r = p_supported(SLC_FEATURE_DLSS, &ai);
    if (r != SLC_OK) {
        LOG("slIsFeatureSupported(DLSS): %d %s: DLSS not supported on this GPU or driver (an NVIDIA RTX GPU and a current "
            "driver are needed; TOTK_DLSS_LOG=2 shows Streamline's reason)", (int)r, slc_result_name(r));
        return 0;
    }
    void *f = NULL;
    slc_Result r1 = p_feature_fn(SLC_FEATURE_DLSS, "slDLSSGetOptimalSettings", &f);
    if (r1 == SLC_OK) p_optimal = (slc_PFN_slDLSSGetOptimalSettings *)f;
    f = NULL;
    slc_Result r2 = p_feature_fn(SLC_FEATURE_DLSS, "slDLSSSetOptions", &f);
    if (r2 == SLC_OK) p_options = (slc_PFN_slDLSSSetOptions *)f;
    if (!p_optimal || !p_options) {
        LOG("slGetFeatureFunction(DLSS): slDLSSGetOptimalSettings %d %s, slDLSSSetOptions %d %s: DLSS off", (int)r1,
            slc_result_name(r1), (int)r2, slc_result_name(r2));
        return 0;
    }
    return 1;
}

static uint32_t sl_mode(int mode, uint32_t in_w, uint32_t out_w) {
    if (in_w && in_w >= out_w) return SLC_DLSS_DLAA;
    switch (mode) {
    case DLSS_DLAA: return SLC_DLSS_MAX_QUALITY;   // the game's own size (1600x896) to the 1080p canvas: the nearest ratio (1.2)
    case DLSS_QUALITY: return SLC_DLSS_MAX_QUALITY;
    case DLSS_BALANCED: return SLC_DLSS_BALANCED;
    case DLSS_PERFORMANCE: return SLC_DLSS_MAX_PERFORMANCE;
    default: return SLC_DLSS_ULTRA_PERFORMANCE;
    }
}

int dlss_sl_optimal(int mode, uint32_t out_w, uint32_t out_h, uint32_t *w, uint32_t *h) {
    if (!g_up || !p_optimal || mode == DLSS_DLAA) return 0;
    slc_DLSSOptions o;
    slc_dlss_options_init(&o);
    o.mode = sl_mode(mode, 0, out_w);
    o.outputWidth = out_w; o.outputHeight = out_h;
    slc_DLSSOptimalSettings s;
    slc_dlss_optimal_init(&s);
    slc_Result r = p_optimal(&o, &s);
    if (r != SLC_OK || !s.optimalRenderWidth || !s.optimalRenderHeight) {
        LOG("slDLSSGetOptimalSettings(%ux%u): %d %s, %ux%u: the fixed ratio instead", out_w, out_h, (int)r,
            slc_result_name(r), s.optimalRenderWidth, s.optimalRenderHeight);
        return 0;
    }
    *w = s.optimalRenderWidth; *h = s.optimalRenderHeight;
    return 1;
}

static void res(slc_Resource *r, const dlss_image *im) {
    slc_resource_init(r, SLC_RESOURCE_TEX2D, (void *)im->image, (void *)im->memory, (void *)im->view, (uint32_t)im->layout);
    r->width = im->width; r->height = im->height; r->nativeFormat = (uint32_t)im->format;
    r->mipLevels = 1; r->arrayLayers = 1; r->flags = 0; r->usage = (uint32_t)im->usage;
}

static void mat(slc_float4x4 *o, const float *m) {
    for (int r = 0; r < 4; r++) {
        o->row[r].x = m[r * 4]; o->row[r].y = m[r * 4 + 1]; o->row[r].z = m[r * 4 + 2]; o->row[r].w = m[r * 4 + 3];
    }
}

static slc_float2 f2(float x, float y) { slc_float2 v = {x, y}; return v; }
static slc_float3 f3(float x, float y, float z) { slc_float3 v = {x, y, z}; return v; }

int dlss_sl_evaluate(const dlss_frame *f, int mode, int preset) {
    if (!g_up || !p_options) return 0;
    slc_ViewportHandle vp;
    slc_viewport_init(&vp, 0u);
    // options: only when they change (Streamline recreates the DLSS feature when the mode or sizes change)
    static uint32_t last_mode = SLC_DLSS_OFF;
    static uint32_t last_w, last_h; static int last_hdr = -1, last_preset = -1;
    uint32_t m = sl_mode(mode, f->in_w, f->output.width);
    if (m != last_mode || last_w != f->output.width || last_h != f->output.height || last_hdr != f->hdr || last_preset != preset) {
        slc_DLSSOptions o;
        slc_dlss_options_init(&o);
        o.mode = m;
        o.outputWidth = f->output.width; o.outputHeight = f->output.height;
        o.colorBuffersHDR = f->hdr ? SLC_TRUE : SLC_FALSE;
        o.useAutoExposure = f->hdr ? SLC_TRUE : SLC_FALSE;
        uint32_t p = (uint32_t)preset;
        o.dlaaPreset = o.qualityPreset = o.balancedPreset = o.performancePreset = o.ultraPerformancePreset = o.ultraQualityPreset = p;
        slc_Result r = p_options(&vp, &o);
        if (r != SLC_OK) { LOG("slDLSSSetOptions failed (%d %s)", (int)r, slc_result_name(r)); return 0; }
        LOG("DLSS %ux%u -> %ux%u, Streamline mode %d, preset %d, %s colour", f->in_w, f->in_h, f->output.width, f->output.height,
            (int)m, preset, f->hdr ? "HDR" : "display");
        last_mode = m; last_w = f->output.width; last_h = f->output.height; last_hdr = f->hdr; last_preset = preset;
    }
    slc_FrameToken *tok = NULL;
    uint32_t idx = f->frame;
    slc_Result rt = p_token(&tok, &idx);
    if (rt != SLC_OK || !tok) { LOG("slGetNewFrameToken failed (%d %s)", (int)rt, slc_result_name(rt)); return 0; }

    slc_Constants c;
    slc_constants_init(&c);
    // DLSS SR does not use the camera matrices (frame generation would); identity, the reprojection is what counts
    static const float id[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    mat(&c.cameraViewToClip, id); mat(&c.clipToCameraView, id); mat(&c.clipToLensClip, id);
    mat(&c.clipToPrevClip, f->clip_to_prev);
    mat(&c.prevClipToClip, f->prev_to_clip);
    c.jitterOffset = f2(f->jitter_x, f->jitter_y);
    dlss_mv_conv_t mv = dlss_mv_conv(f);   // [perf-mt] the camera vectors' conventions unless a source says otherwise
    c.mvecScale = f2(mv.scale_x, mv.scale_y);
    c.cameraPinholeOffset = f2(0, 0);
    c.cameraPos = f3(0, 0, 0); c.cameraUp = f3(0, 1, 0); c.cameraRight = f3(1, 0, 0); c.cameraFwd = f3(0, 0, 1);
    c.cameraNear = f->near_z; c.cameraFar = f->far_z; c.cameraFOV = f->fov_y;
    c.cameraAspectRatio = (float)f->in_w / (float)f->in_h;
    c.motionVectorsInvalidValue = 0.0f;
    c.depthInverted = mv.depth_inverted ? SLC_TRUE : SLC_FALSE;
    c.cameraMotionIncluded = mv.camera_included ? SLC_TRUE : SLC_FALSE;
    c.motionVectors3D = SLC_FALSE;
    c.reset = f->reset ? SLC_TRUE : SLC_FALSE;
    c.orthographicProjection = SLC_FALSE;
    c.motionVectorsDilated = mv.dilated ? SLC_TRUE : SLC_FALSE;
    c.motionVectorsJittered = mv.jittered ? SLC_TRUE : SLC_FALSE;
    slc_Result rc0 = p_constants(&c, tok, &vp);
    if (rc0 != SLC_OK) { LOG("slSetConstants failed (%d %s)", (int)rc0, slc_result_name(rc0)); return 0; }

    slc_Resource rc, rd, rm, ro, rb;
    res(&rc, &f->color); res(&rd, &f->depth); res(&rm, &f->motion); res(&ro, &f->output); res(&rb, &f->bias);
    slc_Extent in = {0, 0, f->in_w, f->in_h}, out = {0, 0, f->output.width, f->output.height};
    slc_ResourceTag tags[5];
    slc_resource_tag_init(&tags[0], &rc, SLC_BUFFER_SCALING_INPUT_COLOR, SLC_LIFECYCLE_VALID_UNTIL_EVALUATE, &in);
    slc_resource_tag_init(&tags[1], &rd, SLC_BUFFER_DEPTH, SLC_LIFECYCLE_VALID_UNTIL_EVALUATE, &in);
    slc_resource_tag_init(&tags[2], &rm, SLC_BUFFER_MOTION_VECTORS, SLC_LIFECYCLE_VALID_UNTIL_EVALUATE, &in);
    slc_resource_tag_init(&tags[3], &ro, SLC_BUFFER_SCALING_OUTPUT_COLOR, SLC_LIFECYCLE_VALID_UNTIL_EVALUATE, &out);
    slc_resource_tag_init(&tags[4], &rb, SLC_BUFFER_BIAS_CURRENT_COLOR_HINT, SLC_LIFECYCLE_VALID_UNTIL_EVALUATE, &in);
    uint32_t ntags = f->bias.image ? 5 : 4;
    slc_Result r = p_tag(tok, &vp, tags, ntags, (void *)f->cb);
    if (r != SLC_OK) { LOG("slSetTagForFrame failed (%d %s)", (int)r, slc_result_name(r)); return 0; }
    const slc_Base *inputs[] = {&vp.base};
    r = p_evaluate(SLC_FEATURE_DLSS, tok, inputs, 1, (void *)f->cb);
    if (r != SLC_OK) { static int told; if (told++ < 5) LOG("slEvaluateFeature failed (%d %s)", (int)r, slc_result_name(r)); return 0; }
    return 1;
}

#else   // built with TOTK_NO_DLSS (tools/setup.py --no-dlss)

int dlss_sl_built(void) { return 0; }
PFN_vkGetInstanceProcAddr dlss_sl_load(const wchar_t *dir) { (void)dir; return NULL; }
int dlss_sl_supported(VkPhysicalDevice gpu) { (void)gpu; return 0; }
int dlss_sl_optimal(int mode, uint32_t out_w, uint32_t out_h, uint32_t *w, uint32_t *h) {
    (void)mode; (void)out_w; (void)out_h; (void)w; (void)h; return 0;
}
int dlss_sl_evaluate(const dlss_frame *f, int mode, int preset) { (void)f; (void)mode; (void)preset; return 0; }

#endif
