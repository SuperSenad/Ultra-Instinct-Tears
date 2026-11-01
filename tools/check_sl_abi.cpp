// Developer check for src/vk/sl_abi.h: every size, offset, GUID, version and default against Streamline's own headers.
// Not part of the build (it needs the Streamline SDK and a full C++ standard library). For example:
//   clang++ -std=c++17 -I<streamline-sdk>/include tools/check_sl_abi.cpp -o check_sl_abi && ./check_sl_abi
//   clang++ -std=c++17 -target x86_64-w64-windows-gnu -fsyntax-only -I<streamline-sdk>/include tools/check_sl_abi.cpp
#pragma clang diagnostic ignored "-Winvalid-offsetof"   // the sl:: structs derive from BaseStructure
#include <sl.h>
#include <sl_consts.h>
#include <sl_dlss.h>
#include <sl_helpers.h>   // getResultAsStr: the names slc_result_name has

#include <cstdio>
#include <cstring>
#include <new>

#include "../src/vk/sl_abi.h"

#define SAME_SIZE(C, S) static_assert(sizeof(C) == sizeof(S) && alignof(C) == alignof(S), #C)
#define SAME_OFF(C, S, cf, sf) static_assert(offsetof(C, cf) == offsetof(S, sf), #C "." #cf)

SAME_SIZE(slc_StructType, sl::StructType);
SAME_SIZE(slc_Base, sl::BaseStructure);
SAME_OFF(slc_Base, sl::BaseStructure, next, next);
SAME_OFF(slc_Base, sl::BaseStructure, structType, structType);
SAME_OFF(slc_Base, sl::BaseStructure, structVersion, structVersion);
SAME_SIZE(slc_float4x4, sl::float4x4);

SAME_SIZE(slc_Preferences, sl::Preferences);
#define P(f) SAME_OFF(slc_Preferences, sl::Preferences, f, f)
P(showConsole); P(logLevel); P(pathsToPlugins); P(numPathsToPlugins); P(pathToLogsAndData); P(allocateCallback);
P(releaseCallback); P(logMessageCallback); P(flags); P(featuresToLoad); P(numFeaturesToLoad); P(applicationId); P(engine);
P(engineVersion); P(projectId); P(renderAPI);
SAME_SIZE(slc_AdapterInfo, sl::AdapterInfo);
SAME_OFF(slc_AdapterInfo, sl::AdapterInfo, deviceLUID, deviceLUID);
SAME_OFF(slc_AdapterInfo, sl::AdapterInfo, deviceLUIDSizeInBytes, deviceLUIDSizeInBytes);
SAME_OFF(slc_AdapterInfo, sl::AdapterInfo, vkPhysicalDevice, vkPhysicalDevice);
SAME_SIZE(slc_ViewportHandle, sl::ViewportHandle);
SAME_SIZE(slc_Resource, sl::Resource);
#define R(f) SAME_OFF(slc_Resource, sl::Resource, f, f)
R(type); R(native); R(memory); R(view); R(state); R(width); R(height); R(nativeFormat); R(mipLevels); R(arrayLayers);
R(gpuVirtualAddress); R(flags); R(usage); R(internalFlags); R(reserved);
SAME_SIZE(slc_ResourceTag, sl::ResourceTag);
SAME_OFF(slc_ResourceTag, sl::ResourceTag, resource, resource);
SAME_OFF(slc_ResourceTag, sl::ResourceTag, type, type);
SAME_OFF(slc_ResourceTag, sl::ResourceTag, lifecycle, lifecycle);
SAME_OFF(slc_ResourceTag, sl::ResourceTag, extent, extent);
static_assert(sizeof(sl::ResourceLifecycle) == sizeof(int32_t), "lifecycle");
SAME_SIZE(slc_Constants, sl::Constants);
#define C(f) SAME_OFF(slc_Constants, sl::Constants, f, f)
C(cameraViewToClip); C(clipToCameraView); C(clipToLensClip); C(clipToPrevClip); C(prevClipToClip); C(jitterOffset);
C(mvecScale); C(cameraPinholeOffset); C(cameraPos); C(cameraUp); C(cameraRight); C(cameraFwd); C(cameraNear); C(cameraFar);
C(cameraFOV); C(cameraAspectRatio); C(motionVectorsInvalidValue); C(depthInverted); C(cameraMotionIncluded);
C(motionVectors3D); C(reset); C(orthographicProjection); C(motionVectorsDilated); C(motionVectorsJittered);
C(minRelativeLinearDepthObjectSeparation);
SAME_SIZE(slc_DLSSOptions, sl::DLSSOptions);
#define O(f) SAME_OFF(slc_DLSSOptions, sl::DLSSOptions, f, f)
O(mode); O(outputWidth); O(outputHeight); O(sharpness); O(preExposure); O(exposureScale); O(colorBuffersHDR);
O(indicatorInvertAxisX); O(indicatorInvertAxisY); O(dlaaPreset); O(qualityPreset); O(balancedPreset); O(performancePreset);
O(ultraPerformancePreset); O(ultraQualityPreset); O(useAutoExposure); O(alphaUpscalingEnabled);
SAME_SIZE(slc_DLSSOptimalSettings, sl::DLSSOptimalSettings);
#define S(f) SAME_OFF(slc_DLSSOptimalSettings, sl::DLSSOptimalSettings, f, f)
S(optimalRenderWidth); S(optimalRenderHeight); S(optimalSharpness); S(renderWidthMin); S(renderHeightMin); S(renderWidthMax);
S(renderHeightMax);
#undef P
#undef R
#undef C
#undef O
#undef S

static_assert(SLC_SDK_VERSION == sl::kSDKVersion, "SDK version");
static_assert(SLC_VERSION_MAJOR == SL_VERSION_MAJOR && SLC_VERSION_MINOR == SL_VERSION_MINOR && SLC_VERSION_PATCH == SL_VERSION_PATCH, "version");
static_assert(SLC_FEATURE_DLSS == sl::kFeatureDLSS, "feature");
static_assert(SLC_BUFFER_DEPTH == sl::kBufferTypeDepth && SLC_BUFFER_MOTION_VECTORS == sl::kBufferTypeMotionVectors &&
              SLC_BUFFER_SCALING_INPUT_COLOR == sl::kBufferTypeScalingInputColor &&
              SLC_BUFFER_SCALING_OUTPUT_COLOR == sl::kBufferTypeScalingOutputColor &&
              SLC_BUFFER_BIAS_CURRENT_COLOR_HINT == sl::kBufferTypeBiasCurrentColorHint, "buffer types");
static_assert(SLC_LIFECYCLE_VALID_UNTIL_EVALUATE == (int)sl::ResourceLifecycle::eValidUntilEvaluate, "lifecycle");
static_assert(SLC_DLSS_DLAA == (int)sl::DLSSMode::eDLAA && SLC_DLSS_MAX_QUALITY == (int)sl::DLSSMode::eMaxQuality &&
              SLC_DLSS_BALANCED == (int)sl::DLSSMode::eBalanced && SLC_DLSS_MAX_PERFORMANCE == (int)sl::DLSSMode::eMaxPerformance &&
              SLC_DLSS_ULTRA_PERFORMANCE == (int)sl::DLSSMode::eUltraPerformance, "modes");
static_assert(SLC_PREF_DISABLE_CL_STATE_TRACKING == (uint64_t)sl::PreferenceFlags::eDisableCLStateTracking &&
              SLC_PREF_USE_FRAME_BASED_RESOURCE_TAGGING == (uint64_t)sl::PreferenceFlags::eUseFrameBasedResourceTagging, "flags");
static_assert(SLC_API_VULKAN == (int)sl::RenderAPI::eVulkan && SLC_LOG_VERBOSE == (int)sl::LogLevel::eVerbose &&
              SLC_LOGTYPE_INFO == (int)sl::LogType::eInfo, "enums");
static_assert(sizeof(sl::Result) == sizeof(slc_Result) && (int)sl::Result::eOk == SLC_OK, "result");

// The function types (slc_PFN_*) are PFun_sl* with each reference as a pointer (sl_core_api.h, sl_dlss.h): checked by
// reading, as C has no references to compare with.

static int fails;
template <class S> static void same(const char *name, const S &cpp, const void *c) {
    if (memcmp(&cpp, c, sizeof(S))) { printf("DIFFERENT: %s\n", name); fails++; } else printf("same: %s\n", name);
}
// both start from zeroed bytes, so padding compares equal
template <class S> static S *zeroed(void *buf) { memset(buf, 0, sizeof(S)); return new (buf) S(); }

int main() {
    alignas(16) static unsigned char a[4096], b[4096];
    { auto *s = zeroed<sl::Preferences>(a); slc_preferences_init((slc_Preferences *)b); same("Preferences", *s, b); }
    { auto *s = zeroed<sl::AdapterInfo>(a); slc_adapter_info_init((slc_AdapterInfo *)b); same("AdapterInfo", *s, b); }
    { memset(a, 0, sizeof(sl::ViewportHandle)); auto *s = new (a) sl::ViewportHandle(7u); slc_viewport_init((slc_ViewportHandle *)b, 7u); same("ViewportHandle", *s, b); }
    { memset(a, 0, sizeof(sl::Resource)); auto *s = new (a) sl::Resource(sl::ResourceType::eTex2d, (void *)1, (void *)2, (void *)3, 4);
      s->flags = 0; slc_resource_init((slc_Resource *)b, 0, (void *)1, (void *)2, (void *)3, 4); same("Resource", *s, b); }
    { sl::Extent e{1, 2, 3, 4}; slc_Extent ce = {1, 2, 3, 4};
      memset(a, 0, sizeof(sl::ResourceTag)); auto *s = new (a) sl::ResourceTag((sl::Resource *)8, sl::kBufferTypeDepth, sl::ResourceLifecycle::eValidUntilEvaluate, &e);
      slc_resource_tag_init((slc_ResourceTag *)b, (slc_Resource *)8, 0, 2, &ce); same("ResourceTag", *s, b); }
    { auto *s = zeroed<sl::Constants>(a); slc_constants_init((slc_Constants *)b); same("Constants", *s, b); }
    { auto *s = zeroed<sl::DLSSOptions>(a); slc_dlss_options_init((slc_DLSSOptions *)b); same("DLSSOptions", *s, b); }
    { auto *s = zeroed<sl::DLSSOptimalSettings>(a); slc_dlss_optimal_init((slc_DLSSOptimalSettings *)b); same("DLSSOptimalSettings", *s, b); }
    for (int r = 0; r < SLC_RESULT_COUNT; r++) {   // sl::Result's names and values (eOk .. eWarnOutOfVRAM)
        const char *cpp = sl::getResultAsStr((sl::Result)r), *c = slc_result_name(r), *colon = strrchr(cpp, ':');
        if (strcmp(colon ? colon + 1 : cpp, c)) { printf("DIFFERENT: Result %d: %s / %s\n", r, cpp, c); fails++; }
    }
    if ((int)sl::Result::eWarnOutOfVRAM != SLC_RESULT_COUNT - 1) { printf("DIFFERENT: Result count\n"); fails++; }
    else printf("same: Result names (%d)\n", SLC_RESULT_COUNT);
    printf(fails ? "FAIL\n" : "OK\n");
    return fails != 0;
}
