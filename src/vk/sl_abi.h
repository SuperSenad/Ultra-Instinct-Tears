// The part of NVIDIA Streamline 2.14.16's ABI that src/vk/dlss_sl.c uses, declared in plain C.
//
// Streamline's own headers (sl.h and the ones it includes) need the C++ standard library (<string>, <vector>), which
// the builder package's trimmed CLANG64 does not carry, and they are not part of this repository. Streamline's ABI is
// C-compatible by design (versioned structs that start with a BaseStructure, functions found with GetProcAddress or
// slGetFeatureFunction, C++ references passed as pointers), so these declarations are all the game needs: the SDK's
// headers are not needed to build, only its DLLs to run (tools/setup.py copies them from deps\streamline).
//
// The struct layouts, GUIDs, versions and default values below are taken from Streamline 2.14.16's sl_struct.h,
// sl_core_types.h, sl_consts.h, sl_dlss.h, sl_version.h, sl_appidentity.h and sl_device_wrappers.h; tools/
// check_sl_abi.cpp checks every size, offset, GUID and default against the real headers (a developer check: it needs
// the SDK and a full C++ standard library). Those declarations are:
//
// Copyright (c) 2022-2023 NVIDIA CORPORATION. All rights reserved
// MIT License (the full text: src/vk/LICENSE-Streamline).
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <wchar.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SLC_VERSION_MAJOR 2
#define SLC_VERSION_MINOR 14
#define SLC_VERSION_PATCH 16
// sl::kSDKVersion: major << 48 | minor << 32 | patch << 16 | kSDKVersionMagic
#define SLC_SDK_VERSION (((uint64_t)SLC_VERSION_MAJOR << 48) | ((uint64_t)SLC_VERSION_MINOR << 32) | \
                         ((uint64_t)SLC_VERSION_PATCH << 16) | 0xfedcu)

#define SLC_INVALID_FLOAT 3.40282346638528859811704183484516925440e38f
#define SLC_INVALID_UINT 0xffffffffu

typedef int32_t slc_Result;          // sl::Result (enum class, int): eOk = 0
#define SLC_OK 0
// sl::Result's names in order (sl_result.h, 2.14: eOk = 0 .. eWarnOutOfVRAM = 39; tools/check_sl_abi.cpp checks them)
static inline const char *slc_result_name(slc_Result r) {
    static const char *const N[] = {
        "eOk", "eErrorIO", "eErrorDriverOutOfDate", "eErrorOSOutOfDate",
        "eErrorOSDisabledHWS", "eErrorDeviceNotCreated", "eErrorNoSupportedAdapterFound", "eErrorAdapterNotSupported",
        "eErrorNoPlugins", "eErrorVulkanAPI", "eErrorDXGIAPI", "eErrorD3DAPI",
        "eErrorNRDAPI", "eErrorNVAPI", "eErrorReflexAPI", "eErrorNGXFailed",
        "eErrorJSONParsing", "eErrorMissingProxy", "eErrorMissingResourceState", "eErrorInvalidIntegration",
        "eErrorMissingInputParameter", "eErrorNotInitialized", "eErrorComputeFailed", "eErrorInitNotCalled",
        "eErrorExceptionHandler", "eErrorInvalidParameter", "eErrorMissingConstants", "eErrorDuplicatedConstants",
        "eErrorMissingOrInvalidAPI", "eErrorCommonConstantsMissing", "eErrorUnsupportedInterface", "eErrorFeatureMissing",
        "eErrorFeatureNotSupported", "eErrorFeatureMissingHooks", "eErrorFeatureFailedToLoad", "eErrorFeatureWrongPriority",
        "eErrorFeatureMissingDependency", "eErrorFeatureManagerInvalidState", "eErrorInvalidState", "eWarnOutOfVRAM",
    };
    return r >= 0 && r < (slc_Result)(sizeof N / sizeof *N) ? N[r] : "unknown";
}
#define SLC_RESULT_COUNT 40
typedef int8_t slc_Boolean;          // sl::Boolean : char
enum { SLC_FALSE = 0, SLC_TRUE = 1, SLC_INVALID = 2 };
typedef uint32_t slc_Feature;
#define SLC_FEATURE_DLSS 0u
typedef uint32_t slc_BufferType;
enum { SLC_BUFFER_DEPTH = 0, SLC_BUFFER_MOTION_VECTORS = 1, SLC_BUFFER_SCALING_INPUT_COLOR = 3,
       SLC_BUFFER_SCALING_OUTPUT_COLOR = 4, SLC_BUFFER_BIAS_CURRENT_COLOR_HINT = 29 };
enum { SLC_LOG_OFF = 0, SLC_LOG_DEFAULT = 1, SLC_LOG_VERBOSE = 2 };                 // sl::LogLevel : uint32_t
enum { SLC_LOGTYPE_INFO = 0, SLC_LOGTYPE_WARN = 1, SLC_LOGTYPE_ERROR = 2 };         // sl::LogType : uint32_t
#define SLC_PREF_DISABLE_CL_STATE_TRACKING (1ull << 0)                             // sl::PreferenceFlags : uint64_t
#define SLC_PREF_ALLOW_OTA (1ull << 3)
#define SLC_PREF_LOAD_DOWNLOADED_PLUGINS (1ull << 6)
#define SLC_PREF_USE_FRAME_BASED_RESOURCE_TAGGING (1ull << 7)
enum { SLC_ENGINE_CUSTOM = 0 };                                                      // sl::EngineType : uint32_t
enum { SLC_API_D3D11 = 0, SLC_API_D3D12 = 1, SLC_API_VULKAN = 2 };                  // sl::RenderAPI : uint32_t
enum { SLC_RESOURCE_TEX2D = 0 };                                                     // sl::ResourceType : char
enum { SLC_LIFECYCLE_ONLY_VALID_NOW = 0, SLC_LIFECYCLE_VALID_UNTIL_PRESENT = 1,     // sl::ResourceLifecycle (int)
       SLC_LIFECYCLE_VALID_UNTIL_EVALUATE = 2 };
enum { SLC_DLSS_OFF = 0, SLC_DLSS_MAX_PERFORMANCE = 1, SLC_DLSS_BALANCED = 2, SLC_DLSS_MAX_QUALITY = 3,  // sl::DLSSMode
       SLC_DLSS_ULTRA_PERFORMANCE = 4, SLC_DLSS_ULTRA_QUALITY = 5, SLC_DLSS_DLAA = 6 };
// sl::DLSSPreset : uint32_t (0 = default, then 5..15 = presets E..O)

typedef struct { uint32_t data1; uint16_t data2, data3; uint8_t data4[8]; } slc_StructType;   // a GUID
typedef struct slc_Base {            // sl::BaseStructure
    struct slc_Base *next;
    slc_StructType structType;
    size_t structVersion;
} slc_Base;

typedef struct { float x, y; } slc_float2;
typedef struct { float x, y, z; } slc_float3;
typedef struct { float x, y, z, w; } slc_float4;
typedef struct { slc_float4 row[4]; } slc_float4x4;
typedef struct { uint32_t top, left, width, height; } slc_Extent;

typedef void slc_LogFn(uint32_t type, const char *msg);   // sl::PFun_LogMessageCallback

typedef struct {                     // sl::Preferences, version 1
    slc_Base base;
    uint8_t showConsole;             // bool
    uint32_t logLevel;
    const wchar_t **pathsToPlugins;
    uint32_t numPathsToPlugins;
    const wchar_t *pathToLogsAndData;
    void *allocateCallback;
    void *releaseCallback;
    slc_LogFn *logMessageCallback;
    uint64_t flags;
    const slc_Feature *featuresToLoad;
    uint32_t numFeaturesToLoad;
    uint32_t applicationId;
    uint32_t engine;
    const char *engineVersion;
    const char *projectId;
    uint32_t renderAPI;
} slc_Preferences;

typedef struct {                     // sl::AdapterInfo, version 1
    slc_Base base;
    uint8_t *deviceLUID;
    uint32_t deviceLUIDSizeInBytes;
    void *vkPhysicalDevice;
} slc_AdapterInfo;

typedef struct {                     // sl::ViewportHandle, version 1
    slc_Base base;
    uint32_t value;
} slc_ViewportHandle;

typedef struct slc_FrameToken slc_FrameToken;   // sl::FrameToken: made by Streamline (slGetNewFrameToken), opaque here

typedef struct {                     // sl::Resource, version 1
    slc_Base base;
    int8_t type;                     // sl::ResourceType : char
    void *native, *memory, *view;    // VkImage, VkDeviceMemory, VkImageView
    uint32_t state;                  // VkImageLayout
    uint32_t width, height, nativeFormat, mipLevels, arrayLayers;
    uint64_t gpuVirtualAddress;
    uint32_t flags, usage;           // VkImageCreateFlags, VkImageUsageFlags
    uint16_t internalFlags, reserved;
} slc_Resource;

typedef struct {                     // sl::ResourceTag, version 1
    slc_Base base;
    slc_Resource *resource;
    slc_BufferType type;
    int32_t lifecycle;               // sl::ResourceLifecycle (plain enum)
    slc_Extent extent;
} slc_ResourceTag;

typedef struct {                     // sl::Constants, version 2
    slc_Base base;
    slc_float4x4 cameraViewToClip, clipToCameraView, clipToLensClip, clipToPrevClip, prevClipToClip;
    slc_float2 jitterOffset, mvecScale, cameraPinholeOffset;
    slc_float3 cameraPos, cameraUp, cameraRight, cameraFwd;
    float cameraNear, cameraFar, cameraFOV, cameraAspectRatio, motionVectorsInvalidValue;
    slc_Boolean depthInverted, cameraMotionIncluded, motionVectors3D, reset, orthographicProjection,
                motionVectorsDilated, motionVectorsJittered;
    float minRelativeLinearDepthObjectSeparation;
} slc_Constants;

typedef struct {                     // sl::DLSSOptions, version 3
    slc_Base base;
    uint32_t mode, outputWidth, outputHeight;
    float sharpness, preExposure, exposureScale;
    slc_Boolean colorBuffersHDR, indicatorInvertAxisX, indicatorInvertAxisY;
    uint32_t dlaaPreset, qualityPreset, balancedPreset, performancePreset, ultraPerformancePreset, ultraQualityPreset;
    slc_Boolean useAutoExposure, alphaUpscalingEnabled;
} slc_DLSSOptions;

typedef struct {                     // sl::DLSSOptimalSettings, version 1
    slc_Base base;
    uint32_t optimalRenderWidth, optimalRenderHeight;
    float optimalSharpness;
    uint32_t renderWidthMin, renderHeightMin, renderWidthMax, renderHeightMax;
} slc_DLSSOptimalSettings;

// The entry points (C++ references are pointers in the ABI; sl::Result is returned as an int)
typedef slc_Result slc_PFN_slInit(const slc_Preferences *pref, uint64_t sdkVersion);
typedef slc_Result slc_PFN_slIsFeatureSupported(slc_Feature feature, const slc_AdapterInfo *adapterInfo);
typedef slc_Result slc_PFN_slEvaluateFeature(slc_Feature feature, const slc_FrameToken *frame, const slc_Base **inputs,
                                              uint32_t numInputs, void *cmdBuffer);
typedef slc_Result slc_PFN_slSetTagForFrame(const slc_FrameToken *frame, const slc_ViewportHandle *viewport,
                                             const slc_ResourceTag *tags, uint32_t numTags, void *cmdBuffer);
typedef slc_Result slc_PFN_slSetConstants(const slc_Constants *values, const slc_FrameToken *frame,
                                           const slc_ViewportHandle *viewport);
typedef slc_Result slc_PFN_slGetFeatureFunction(slc_Feature feature, const char *functionName, void **function);
typedef slc_Result slc_PFN_slGetNewFrameToken(slc_FrameToken **token, const uint32_t *frameIndex);
typedef slc_Result slc_PFN_slDLSSGetOptimalSettings(const slc_DLSSOptions *options, slc_DLSSOptimalSettings *settings);
typedef slc_Result slc_PFN_slDLSSSetOptions(const slc_ViewportHandle *viewport, const slc_DLSSOptions *options);

// ---------------------------------------------------------------- the GUIDs and versions, and the C++ defaults
#define SLC_GUID(a, b, c, d0, d1, d2, d3, d4, d5, d6, d7) {a, b, c, {d0, d1, d2, d3, d4, d5, d6, d7}}
static const slc_StructType slc_guid_Preferences = SLC_GUID(0x1ca10965, 0xbf8e, 0x432b, 0x8d, 0xa1, 0x67, 0x16, 0xd8, 0x79, 0xfb, 0x14);
static const slc_StructType slc_guid_AdapterInfo = SLC_GUID(0x0677315f, 0xa746, 0x4492, 0x9f, 0x42, 0xcb, 0x61, 0x42, 0xc9, 0xc3, 0xd4);
static const slc_StructType slc_guid_ViewportHandle = SLC_GUID(0x171b6435, 0x9b3c, 0x4fc8, 0x99, 0x94, 0xfb, 0xe5, 0x25, 0x69, 0xaa, 0xa4);
static const slc_StructType slc_guid_Resource = SLC_GUID(0x3a9d70cf, 0x2418, 0x4b72, 0x83, 0x91, 0x13, 0xf8, 0x72, 0x1c, 0x72, 0x61);
static const slc_StructType slc_guid_ResourceTag = SLC_GUID(0x4c6a5aad, 0xb445, 0x496c, 0x87, 0xff, 0x1a, 0xf3, 0x84, 0x5b, 0xe6, 0x53);
static const slc_StructType slc_guid_Constants = SLC_GUID(0xdcd35ad7, 0x4e4a, 0x4bad, 0xa9, 0x0c, 0xe0, 0xc4, 0x9e, 0xb2, 0x3a, 0xfe);
static const slc_StructType slc_guid_DLSSOptions = SLC_GUID(0x6ac826e4, 0x4c61, 0x4101, 0xa9, 0x2d, 0x63, 0x8d, 0x42, 0x10, 0x57, 0xb8);
static const slc_StructType slc_guid_DLSSOptimalSettings = SLC_GUID(0xef1d0957, 0xfd58, 0x4df7, 0xb5, 0x04, 0x8b, 0x69, 0xd8, 0xaa, 0x6b, 0x76);

static inline void slc_base(slc_Base *b, size_t size, slc_StructType type, size_t version) {
    memset(b, 0, size);
    b->structType = type;
    b->structVersion = version;
}

// Each *_init gives the same bytes as the C++ default constructor (checked by tools/check_sl_abi.cpp).
static inline void slc_preferences_init(slc_Preferences *p) {
    slc_base(&p->base, sizeof *p, slc_guid_Preferences, 1);
    p->logLevel = SLC_LOG_DEFAULT;
    p->flags = SLC_PREF_DISABLE_CL_STATE_TRACKING | SLC_PREF_ALLOW_OTA | SLC_PREF_LOAD_DOWNLOADED_PLUGINS;
    p->engine = SLC_ENGINE_CUSTOM;
    p->renderAPI = SLC_API_D3D12;
}
static inline void slc_adapter_info_init(slc_AdapterInfo *a) { slc_base(&a->base, sizeof *a, slc_guid_AdapterInfo, 1); }
static inline void slc_viewport_init(slc_ViewportHandle *v, uint32_t value) {
    slc_base(&v->base, sizeof *v, slc_guid_ViewportHandle, 1);
    v->value = value;
}
// sl::Resource(type, native, mem, view, state): `flags` has no default there; 0 here
static inline void slc_resource_init(slc_Resource *r, int8_t type, void *native, void *mem, void *view, uint32_t state) {
    slc_base(&r->base, sizeof *r, slc_guid_Resource, 1);
    r->type = type; r->native = native; r->memory = mem; r->view = view; r->state = state;
}
static inline void slc_resource_tag_init(slc_ResourceTag *t, slc_Resource *r, slc_BufferType type, int32_t lifecycle,
                                         const slc_Extent *e) {
    slc_base(&t->base, sizeof *t, slc_guid_ResourceTag, 1);
    t->resource = r; t->type = type; t->lifecycle = lifecycle;
    if (e) t->extent = *e;
}
static inline void slc_constants_init(slc_Constants *c) {
    slc_base(&c->base, sizeof *c, slc_guid_Constants, 2);
    float *f = &c->cameraViewToClip.row[0].x;   // every matrix and vector: INVALID_FLOAT (sl::float2/3/4())
    for (size_t i = 0; i < (offsetof(slc_Constants, cameraNear) - offsetof(slc_Constants, cameraViewToClip)) / sizeof(float); i++)
        f[i] = SLC_INVALID_FLOAT;
    c->cameraNear = c->cameraFar = c->cameraFOV = c->cameraAspectRatio = c->motionVectorsInvalidValue = SLC_INVALID_FLOAT;
    c->depthInverted = c->cameraMotionIncluded = c->motionVectors3D = c->reset = SLC_INVALID;
    c->orthographicProjection = c->motionVectorsDilated = c->motionVectorsJittered = SLC_FALSE;
    c->minRelativeLinearDepthObjectSeparation = 40.0f;
}
static inline void slc_dlss_options_init(slc_DLSSOptions *o) {
    slc_base(&o->base, sizeof *o, slc_guid_DLSSOptions, 3);
    o->mode = SLC_DLSS_OFF;
    o->outputWidth = o->outputHeight = SLC_INVALID_UINT;
    o->sharpness = 0.0f; o->preExposure = 1.0f; o->exposureScale = 1.0f;
    o->colorBuffersHDR = SLC_TRUE;
    o->indicatorInvertAxisX = o->indicatorInvertAxisY = SLC_FALSE;
    o->useAutoExposure = o->alphaUpscalingEnabled = SLC_FALSE;
}
static inline void slc_dlss_optimal_init(slc_DLSSOptimalSettings *s) {
    slc_base(&s->base, sizeof *s, slc_guid_DLSSOptimalSettings, 1);
}

#ifdef __cplusplus
}
#endif
