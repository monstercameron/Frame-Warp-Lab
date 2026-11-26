#include "streamline_backend.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <mutex>

#if defined(FRAME_WARP_HAS_STREAMLINE) && FRAME_WARP_HAS_STREAMLINE
#include <sl.h>
#include <sl_consts.h>
#include <sl_core_api.h>
#include <sl_core_types.h>
#include <sl_dlss.h>
#include <sl_dlss_g.h>
#include <sl_matrix_helpers.h>
#include <sl_pcl.h>
#include <sl_reflex.h>
#endif

namespace frame_warp_lab {

namespace {

#if defined(FRAME_WARP_HAS_STREAMLINE) && FRAME_WARP_HAS_STREAMLINE

std::mutex g_streamline_log_mutex;
std::string g_streamline_last_error;

void streamline_log_callback(sl::LogType type, const char* message) {
    if (!message) return;
    if (type == sl::LogType::eError || type == sl::LogType::eWarn) {
        std::lock_guard<std::mutex> lock(g_streamline_log_mutex);
        if (type == sl::LogType::eError || g_streamline_last_error.empty()) {
            g_streamline_last_error = message;
        }
    }
}

std::string read_last_streamline_error() {
    std::lock_guard<std::mutex> lock(g_streamline_log_mutex);
    return g_streamline_last_error;
}

sl::float4x4 make_camera_view_to_world(const CameraBasis& camera) {
    sl::float4x4 matrix{};
    matrix[0] = sl::float4(camera.right.x, camera.right.y, camera.right.z, 0.0f);
    matrix[1] = sl::float4(camera.up.x, camera.up.y, camera.up.z, 0.0f);
    matrix[2] = sl::float4(camera.forward.x, camera.forward.y, camera.forward.z, 0.0f);
    matrix[3] = sl::float4(camera.position.x, camera.position.y, camera.position.z, 1.0f);
    return matrix;
}

sl::float4x4 make_camera_view_to_clip(const CameraBasis& camera, float aspect_ratio, float near_plane, float far_plane) {
    const float tan_y = std::max(camera.tan_half_fov_y, 0.001f);
    const float tan_x = std::max(tan_y * aspect_ratio, 0.001f);
    const float z_scale = far_plane / std::max(far_plane - near_plane, 0.001f);
    const float z_bias = -(near_plane * far_plane) / std::max(far_plane - near_plane, 0.001f);
    sl::float4x4 matrix{};
    matrix[0] = sl::float4(1.0f / tan_x, 0.0f, 0.0f, 0.0f);
    matrix[1] = sl::float4(0.0f, 1.0f / tan_y, 0.0f, 0.0f);
    matrix[2] = sl::float4(0.0f, 0.0f, z_scale, 1.0f);
    matrix[3] = sl::float4(0.0f, 0.0f, z_bias, 0.0f);
    return matrix;
}

sl::PCLMarker translate_pcl_marker(StreamlinePclMarker marker) {
    switch (marker) {
        case StreamlinePclMarker::simulation_start: return sl::PCLMarker::eSimulationStart;
        case StreamlinePclMarker::simulation_end: return sl::PCLMarker::eSimulationEnd;
        case StreamlinePclMarker::render_submit_start: return sl::PCLMarker::eRenderSubmitStart;
        case StreamlinePclMarker::render_submit_end: return sl::PCLMarker::eRenderSubmitEnd;
        case StreamlinePclMarker::present_start: return sl::PCLMarker::ePresentStart;
        case StreamlinePclMarker::present_end: return sl::PCLMarker::ePresentEnd;
        case StreamlinePclMarker::trigger_flash: return sl::PCLMarker::eTriggerFlash;
        case StreamlinePclMarker::pc_latency_ping: return sl::PCLMarker::ePCLatencyPing;
        case StreamlinePclMarker::late_warp_present_start: return sl::PCLMarker::eLateWarpPresentStart;
        case StreamlinePclMarker::late_warp_present_end: return sl::PCLMarker::eLateWarpPresentEnd;
        case StreamlinePclMarker::camera_constructed: return sl::PCLMarker::eCameraConstructed;
        case StreamlinePclMarker::late_warp_render_submit_start: return sl::PCLMarker::eLateWarpRenderSubmitStart;
        case StreamlinePclMarker::late_warp_render_submit_end: return sl::PCLMarker::eLateWarpRenderSubmitEnd;
        default: return sl::PCLMarker::eSimulationStart;
    }
}

#endif

}

StreamlineBackend::~StreamlineBackend() noexcept {
    shutdown();
}

bool StreamlineBackend::record_api_result(StreamlineCall call, std::int32_t result_code) {
    if (result_code == 0) {
        return true;
    }
    ++api_error_count_;
    last_api_result_ = result_code;
    last_failed_call_ = call;
#if defined(FRAME_WARP_HAS_STREAMLINE) && FRAME_WARP_HAS_STREAMLINE
    const std::string logged = read_last_streamline_error();
    if (!logged.empty()) {
        capabilities_.last_error = logged;
    } else {
        capabilities_.last_error = std::string(streamline_call_name(static_cast<std::int32_t>(call)))
            + " failed with code " + std::to_string(result_code);
    }
#endif
    return false;
}

bool StreamlineBackend::initialize(bool enable_dlssg_feature, bool quiet_logging) {
#if defined(FRAME_WARP_HAS_STREAMLINE) && FRAME_WARP_HAS_STREAMLINE
    capabilities_ = StreamlineCapabilities{};
    capabilities_.compiled_with_sdk = true;
    dlssg_feature_requested_ = enable_dlssg_feature;
    api_error_count_ = 0;
    last_api_result_ = 0;
    last_failed_call_ = StreamlineCall::none;

    {
        std::lock_guard<std::mutex> lock(g_streamline_log_mutex);
        g_streamline_last_error.clear();
    }

    const std::array<sl::Feature, 4> features = {
        sl::kFeaturePCL,
        sl::kFeatureReflex,
        sl::kFeatureDLSS,
        sl::kFeatureDLSS_G,
    };
    const std::uint32_t feature_count = enable_dlssg_feature ? 4U : 3U;

    wchar_t log_dir_buffer[1024]{};
    const DWORD log_dir_len = GetEnvironmentVariableW(L"FRAME_WARP_SL_LOG_DIR", log_dir_buffer, static_cast<DWORD>(std::size(log_dir_buffer)));
    const bool enable_file_logs = (log_dir_len > 0 && log_dir_len < std::size(log_dir_buffer));

    sl::Preferences preferences{};
    preferences.showConsole = false;
    preferences.logLevel = enable_file_logs
        ? sl::LogLevel::eVerbose
        : (quiet_logging ? sl::LogLevel::eOff : sl::LogLevel::eDefault);
    preferences.pathToLogsAndData = enable_file_logs ? log_dir_buffer : nullptr;
    preferences.logMessageCallback = streamline_log_callback;
    preferences.flags = sl::PreferenceFlags::eDisableCLStateTracking
        | sl::PreferenceFlags::eDisableDebugText
        | sl::PreferenceFlags::eUseFrameBasedResourceTagging;
    preferences.featuresToLoad = features.data();
    preferences.numFeaturesToLoad = feature_count;
    preferences.applicationId = 231313132U;
    preferences.engine = sl::EngineType::eCustom;
    preferences.engineVersion = "0.18.0";
    preferences.projectId = "a0f57b54-1daf-4934-90ae-c4035c19df04";
    preferences.renderAPI = sl::RenderAPI::eD3D12;

    const sl::Result result = slInit(preferences, sl::kSDKVersion);
    if (!record_api_result(StreamlineCall::sl_init, static_cast<std::int32_t>(result))) {
        return false;
    }

    initialized_ = true;
    capabilities_.initialized = true;
    sl::FeatureVersion version{};
    if (slGetFeatureVersion(enable_dlssg_feature ? sl::kFeatureDLSS_G : sl::kFeatureReflex, version) == sl::Result::eOk) {
        capabilities_.sl_version_major = version.versionSL.major;
        capabilities_.sl_version_minor = version.versionSL.minor;
        capabilities_.sl_version_build = version.versionSL.build;
        capabilities_.ngx_version_major = version.versionNGX.major;
        capabilities_.ngx_version_minor = version.versionNGX.minor;
        capabilities_.ngx_version_build = version.versionNGX.build;
    }
    return true;
#else
    (void)enable_dlssg_feature;
    (void)quiet_logging;
    capabilities_ = StreamlineCapabilities{};
    capabilities_.compiled_with_sdk = false;
    return false;
#endif
}

std::string StreamlineBackend::last_error() const {
#if defined(FRAME_WARP_HAS_STREAMLINE) && FRAME_WARP_HAS_STREAMLINE
    const std::string logged = read_last_streamline_error();
    return logged.empty() ? capabilities_.last_error : logged;
#else
    return {};
#endif
}

void StreamlineBackend::query_adapter(const LUID& adapter_luid) {
#if defined(FRAME_WARP_HAS_STREAMLINE) && FRAME_WARP_HAS_STREAMLINE
    if (!initialized_) return;
    LUID luid_copy = adapter_luid;
    sl::AdapterInfo adapter_info{};
    adapter_info.deviceLUID = reinterpret_cast<std::uint8_t*>(&luid_copy);
    adapter_info.deviceLUIDSizeInBytes = sizeof(LUID);

    const sl::Result pcl_result = slIsFeatureSupported(sl::kFeaturePCL, adapter_info);
    const sl::Result reflex_result = slIsFeatureSupported(sl::kFeatureReflex, adapter_info);
    const sl::Result dlss_sr_result = slIsFeatureSupported(sl::kFeatureDLSS, adapter_info);
    const sl::Result dlssg_result = dlssg_feature_requested_
        ? slIsFeatureSupported(sl::kFeatureDLSS_G, adapter_info)
        : sl::Result::eErrorFeatureMissing;

    capabilities_.pcl_supported = (pcl_result == sl::Result::eOk);
    capabilities_.reflex_supported = (reflex_result == sl::Result::eOk);
    capabilities_.dlss_sr_supported = (dlss_sr_result == sl::Result::eOk);
    capabilities_.dlssg_supported = (dlssg_result == sl::Result::eOk);
    capabilities_.reflex_support_result = static_cast<std::int32_t>(reflex_result);
    capabilities_.dlss_sr_support_result = static_cast<std::int32_t>(dlss_sr_result);
    capabilities_.dlssg_support_result = static_cast<std::int32_t>(dlssg_result);

    sl::FeatureVersion version{};
    if (capabilities_.dlssg_supported && slGetFeatureVersion(sl::kFeatureDLSS_G, version) == sl::Result::eOk) {
        capabilities_.sl_version_major = version.versionSL.major;
        capabilities_.sl_version_minor = version.versionSL.minor;
        capabilities_.sl_version_build = version.versionSL.build;
        capabilities_.ngx_version_major = version.versionNGX.major;
        capabilities_.ngx_version_minor = version.versionNGX.minor;
        capabilities_.ngx_version_build = version.versionNGX.build;
    }
    const std::string logged = read_last_streamline_error();
    if (!logged.empty()) capabilities_.last_error = logged;
#else
    (void)adapter_luid;
#endif
}

bool StreamlineBackend::attach_device(ID3D12Device* device) {
#if defined(FRAME_WARP_HAS_STREAMLINE) && FRAME_WARP_HAS_STREAMLINE
    if (!initialized_ || !device) return false;
    const sl::Result result = slSetD3DDevice(device);
    if (!record_api_result(StreamlineCall::sl_set_d3d_device, static_cast<std::int32_t>(result))) {
        return false;
    }
    device_attached_ = true;

    const LUID luid = device->GetAdapterLuid();
    query_adapter(luid);

    if (capabilities_.pcl_supported) {
        sl::PCLOptions pcl_options{};
        pcl_options.virtualKey = sl::PCLHotKey::eUsePingMessage;
        pcl_options.idThread = GetCurrentThreadId();
        record_api_result(StreamlineCall::sl_pcl_set_options, static_cast<std::int32_t>(slPCLSetOptions(pcl_options)));
    }

    if (capabilities_.reflex_supported) {
        sl::ReflexOptions reflex_options{};
        reflex_options.mode = sl::ReflexMode::eLowLatencyWithBoost;
        reflex_options.frameLimitUs = 0;
        reflex_options.useMarkersToOptimize = false;
        reflex_options.idThread = GetCurrentThreadId();
        if (record_api_result(StreamlineCall::sl_reflex_set_options, static_cast<std::int32_t>(slReflexSetOptions(reflex_options)))) {
            reflex_configured_ = true;
            last_reflex_enabled_ = true;
            last_reflex_limit_us_ = 0;
        }
    }

    if (capabilities_.dlssg_supported) {
        const sl::ViewportHandle viewport(0U);
        sl::DLSSGState state{};
        sl::DLSSGOptions probe_options{};
        probe_options.mode = sl::DLSSGMode::eOff;
        if (record_api_result(StreamlineCall::sl_dlssg_get_state, static_cast<std::int32_t>(slDLSSGGetState(viewport, state, &probe_options)))) {
            capabilities_.max_frames_to_generate = state.numFramesToGenerateMax;
            capabilities_.min_width_or_height = state.minWidthOrHeight;
            capabilities_.estimated_vram_bytes = state.estimatedVRAMUsageInBytes;
            capabilities_.dynamic_mfg_supported = (state.bIsDynamicMFGSupported == sl::Boolean::eTrue);
            capabilities_.vsync_supported = (state.bIsVsyncSupportAvailable == sl::Boolean::eTrue);
        }
    }
    return true;
#else
    (void)device;
    return false;
#endif
}

void StreamlineBackend::shutdown() noexcept {
#if defined(FRAME_WARP_HAS_STREAMLINE) && FRAME_WARP_HAS_STREAMLINE
    if (!initialized_) return;
    if (device_attached_ && capabilities_.dlssg_supported) {
        const sl::ViewportHandle viewport(0U);
        sl::DLSSGOptions options{};
        options.mode = sl::DLSSGMode::eOff;
        record_api_result(StreamlineCall::sl_dlssg_set_options, static_cast<std::int32_t>(slDLSSGSetOptions(viewport, options)));
        clear_tags();
    }
    record_api_result(StreamlineCall::sl_shutdown, static_cast<std::int32_t>(slShutdown()));
    initialized_ = false;
    device_attached_ = false;
    dlssg_enabled_ = false;
    current_frame_token_ = nullptr;
#endif
}

bool StreamlineBackend::begin_frame(std::uint32_t frame_index, bool reflex_enabled, std::uint32_t frame_limit_us) {
#if defined(FRAME_WARP_HAS_STREAMLINE) && FRAME_WARP_HAS_STREAMLINE
    (void)frame_index;
    if (!initialized_ || !device_attached_) return false;
    sl::FrameToken* token = nullptr;
    const sl::Result token_result = slGetNewFrameToken(token, nullptr);
    if (!record_api_result(StreamlineCall::sl_get_new_frame_token, static_cast<std::int32_t>(token_result)) || !token) {
        return false;
    }
    current_frame_token_ = token;

    if (capabilities_.reflex_supported) {
        if (!reflex_configured_ || last_reflex_enabled_ != reflex_enabled || last_reflex_limit_us_ != frame_limit_us) {
            sl::ReflexOptions reflex_options{};
            reflex_options.mode = reflex_enabled ? sl::ReflexMode::eLowLatencyWithBoost : sl::ReflexMode::eOff;
            reflex_options.frameLimitUs = frame_limit_us;
            reflex_options.useMarkersToOptimize = false;
            reflex_options.idThread = GetCurrentThreadId();
            if (record_api_result(StreamlineCall::sl_reflex_set_options, static_cast<std::int32_t>(slReflexSetOptions(reflex_options)))) {
                reflex_configured_ = true;
                last_reflex_enabled_ = reflex_enabled;
                last_reflex_limit_us_ = frame_limit_us;
            }
        }
        record_api_result(StreamlineCall::sl_reflex_sleep, static_cast<std::int32_t>(slReflexSleep(*token)));
    }
    return true;
#else
    (void)frame_index;
    (void)reflex_enabled;
    (void)frame_limit_us;
    return false;
#endif
}

bool StreamlineBackend::set_pcl_marker(StreamlinePclMarker marker) {
#if defined(FRAME_WARP_HAS_STREAMLINE) && FRAME_WARP_HAS_STREAMLINE
    if (!initialized_ || !device_attached_ || !capabilities_.pcl_supported || !current_frame_token_) return false;
    auto* token = static_cast<sl::FrameToken*>(current_frame_token_);
    const sl::Result result = slPCLSetMarker(translate_pcl_marker(marker), *token);
    return record_api_result(StreamlineCall::sl_pcl_set_marker, static_cast<std::int32_t>(result));
#else
    (void)marker;
    return false;
#endif
}

bool StreamlineBackend::set_common_constants(
    const CameraBasis& current_camera,
    const CameraBasis& previous_camera,
    float aspect_ratio,
    float near_plane,
    float far_plane,
    bool reset_history) {
#if defined(FRAME_WARP_HAS_STREAMLINE) && FRAME_WARP_HAS_STREAMLINE
    if (!initialized_ || !device_attached_ || !current_frame_token_) return false;
    auto* token = static_cast<sl::FrameToken*>(current_frame_token_);
    const sl::ViewportHandle viewport(0U);

    sl::Constants constants{};
    const sl::float4x4 view_to_world = make_camera_view_to_world(current_camera);
    const sl::float4x4 prev_view_to_world = make_camera_view_to_world(previous_camera);
    const sl::float4x4 view_to_clip = make_camera_view_to_clip(current_camera, aspect_ratio, near_plane, far_plane);
    const sl::float4x4 prev_view_to_clip = make_camera_view_to_clip(previous_camera, aspect_ratio, near_plane, far_plane);

    constants.cameraViewToClip = view_to_clip;
    sl::matrixFullInvert(constants.clipToCameraView, view_to_clip);

    sl::float4x4 view_to_prev_view{};
    sl::calcCameraToPrevCamera(view_to_prev_view, view_to_world, prev_view_to_world);
    sl::float4x4 clip_to_prev_view{};
    sl::matrixMul(clip_to_prev_view, constants.clipToCameraView, view_to_prev_view);
    sl::matrixMul(constants.clipToPrevClip, clip_to_prev_view, prev_view_to_clip);
    sl::matrixFullInvert(constants.prevClipToClip, constants.clipToPrevClip);

    constants.jitterOffset = sl::float2(0.0f, 0.0f);
    constants.mvecScale = sl::float2(1.0f, 1.0f);
    constants.cameraPinholeOffset = sl::float2(0.0f, 0.0f);
    constants.cameraPos = sl::float3(current_camera.position.x, current_camera.position.y, current_camera.position.z);
    constants.cameraUp = sl::float3(current_camera.up.x, current_camera.up.y, current_camera.up.z);
    constants.cameraRight = sl::float3(current_camera.right.x, current_camera.right.y, current_camera.right.z);
    constants.cameraFwd = sl::float3(current_camera.forward.x, current_camera.forward.y, current_camera.forward.z);
    constants.cameraNear = near_plane;
    constants.cameraFar = far_plane;
    constants.cameraFOV = 2.0f * std::atan(std::max(current_camera.tan_half_fov_y, 0.001f));
    constants.cameraAspectRatio = aspect_ratio;
    constants.motionVectorsInvalidValue = 0.0f;
    constants.depthInverted = sl::Boolean::eFalse;
    constants.cameraMotionIncluded = sl::Boolean::eTrue;
    constants.motionVectors3D = sl::Boolean::eFalse;
    constants.reset = reset_history ? sl::Boolean::eTrue : sl::Boolean::eFalse;
    constants.orthographicProjection = sl::Boolean::eFalse;
    constants.motionVectorsDilated = sl::Boolean::eFalse;
    constants.motionVectorsJittered = sl::Boolean::eFalse;

    const sl::Result result = slSetConstants(constants, *token, viewport);
    return record_api_result(StreamlineCall::sl_set_constants, static_cast<std::int32_t>(result));
#else
    (void)current_camera;
    (void)previous_camera;
    (void)aspect_ratio;
    (void)near_plane;
    (void)far_plane;
    (void)reset_history;
    return false;
#endif
}

bool StreamlineBackend::tag_dlssg_resources(
    ID3D12Resource* clip_depth,
    D3D12_RESOURCE_STATES depth_state,
    ID3D12Resource* motion_vectors,
    D3D12_RESOURCE_STATES motion_state,
    ID3D12Resource* hudless_color,
    D3D12_RESOURCE_STATES hudless_state,
    UINT width,
    UINT height,
    ID3D12GraphicsCommandList* command_list) {
#if defined(FRAME_WARP_HAS_STREAMLINE) && FRAME_WARP_HAS_STREAMLINE
    if (!initialized_ || !device_attached_ || !capabilities_.dlssg_supported || !current_frame_token_) return false;
    auto* token = static_cast<sl::FrameToken*>(current_frame_token_);
    const sl::ViewportHandle viewport(0U);
    const sl::Extent full_extent{0U, 0U, width, height};

    sl::Resource depth_resource{sl::ResourceType::eTex2d, clip_depth, nullptr, nullptr, static_cast<std::uint32_t>(depth_state)};
    sl::Resource motion_resource{sl::ResourceType::eTex2d, motion_vectors, nullptr, nullptr, static_cast<std::uint32_t>(motion_state)};
    sl::Resource hudless_resource{sl::ResourceType::eTex2d, hudless_color, nullptr, nullptr, static_cast<std::uint32_t>(hudless_state)};

    std::array<sl::ResourceTag, 3> tags = {
        sl::ResourceTag{clip_depth ? &depth_resource : nullptr, sl::kBufferTypeDepth, sl::ResourceLifecycle::eValidUntilPresent, clip_depth ? &full_extent : nullptr},
        sl::ResourceTag{motion_vectors ? &motion_resource : nullptr, sl::kBufferTypeMotionVectors, sl::ResourceLifecycle::eValidUntilPresent, motion_vectors ? &full_extent : nullptr},
        sl::ResourceTag{hudless_color ? &hudless_resource : nullptr, sl::kBufferTypeHUDLessColor, sl::ResourceLifecycle::eValidUntilPresent, hudless_color ? &full_extent : nullptr},
    };

    const sl::Result result = slSetTagForFrame(*token, viewport, tags.data(), static_cast<std::uint32_t>(tags.size()), command_list);
    return record_api_result(StreamlineCall::sl_set_tag_for_frame, static_cast<std::int32_t>(result));
#else
    (void)clip_depth;
    (void)depth_state;
    (void)motion_vectors;
    (void)motion_state;
    (void)hudless_color;
    (void)hudless_state;
    (void)width;
    (void)height;
    (void)command_list;
    return false;
#endif
}

bool StreamlineBackend::clear_tags() {
#if defined(FRAME_WARP_HAS_STREAMLINE) && FRAME_WARP_HAS_STREAMLINE
    if (!initialized_ || !device_attached_ || !current_frame_token_) return false;
    auto* token = static_cast<sl::FrameToken*>(current_frame_token_);
    const sl::ViewportHandle viewport(0U);
    const std::array<sl::ResourceTag, 3> tags = {
        sl::ResourceTag{nullptr, sl::kBufferTypeDepth, sl::ResourceLifecycle::eValidUntilPresent, nullptr},
        sl::ResourceTag{nullptr, sl::kBufferTypeMotionVectors, sl::ResourceLifecycle::eValidUntilPresent, nullptr},
        sl::ResourceTag{nullptr, sl::kBufferTypeHUDLessColor, sl::ResourceLifecycle::eValidUntilPresent, nullptr},
    };
    const sl::Result result = slSetTagForFrame(*token, viewport, tags.data(), static_cast<std::uint32_t>(tags.size()), nullptr);
    return record_api_result(StreamlineCall::sl_set_tag_for_frame, static_cast<std::int32_t>(result));
#else
    return false;
#endif
}

bool StreamlineBackend::configure_dlssg(
    bool enabled,
    std::uint32_t multiplier,
    bool dynamic_mode,
    float dynamic_target_fps,
    UINT width,
    UINT height,
    UINT back_buffer_count,
    bool has_hudless_buffer) {
#if defined(FRAME_WARP_HAS_STREAMLINE) && FRAME_WARP_HAS_STREAMLINE
    if (!initialized_ || !device_attached_ || !capabilities_.dlssg_supported) {
        dlssg_enabled_ = false;
        return false;
    }

    const std::uint32_t max_generated = capabilities_.max_frames_to_generate > 0U ? capabilities_.max_frames_to_generate : 1U;
    const bool use_dynamic = enabled && dynamic_mode && capabilities_.dynamic_mfg_supported;
    const std::uint32_t clamped_multiplier = std::clamp(multiplier, 2U, max_generated + 1U);
    const std::uint32_t num_frames_to_generate = clamped_multiplier - 1U;

    if (dlssg_enabled_ == enabled
        && (!enabled || (dlssg_applied_multiplier_ == clamped_multiplier
            && dlssg_applied_dynamic_ == use_dynamic
            && std::fabs(dlssg_applied_dynamic_target_fps_ - dynamic_target_fps) < 0.25f
            && dlssg_applied_width_ == width
            && dlssg_applied_height_ == height
            && dlssg_applied_hudless_ == has_hudless_buffer))) {
        return dlssg_enabled_;
    }

    sl::DLSSGOptions options{};
    if (!enabled) {
        options.mode = sl::DLSSGMode::eOff;
    } else if (use_dynamic) {
        options.mode = sl::DLSSGMode::eDynamic;
        options.numFramesToGenerate = num_frames_to_generate;
        options.dynamicTargetFrameRate = dynamic_target_fps;
    } else {
        options.mode = sl::DLSSGMode::eOn;
        options.numFramesToGenerate = num_frames_to_generate;
    }
    options.numBackBuffers = back_buffer_count;
    options.colorWidth = width;
    options.colorHeight = height;
    options.mvecDepthWidth = width;
    options.mvecDepthHeight = height;
    options.colorBufferFormat = static_cast<std::uint32_t>(DXGI_FORMAT_R8G8B8A8_UNORM);
    options.mvecBufferFormat = static_cast<std::uint32_t>(DXGI_FORMAT_R16G16_FLOAT);
    options.depthBufferFormat = static_cast<std::uint32_t>(DXGI_FORMAT_R32_FLOAT);
    options.hudLessBufferFormat = has_hudless_buffer ? static_cast<std::uint32_t>(DXGI_FORMAT_R8G8B8A8_UNORM) : 0U;
    options.queueParallelismMode = sl::DLSSGQueueParallelismMode::eBlockPresentingClientQueue;

    const sl::ViewportHandle viewport(0U);
    const sl::Result result = slDLSSGSetOptions(viewport, options);
    if (record_api_result(StreamlineCall::sl_dlssg_set_options, static_cast<std::int32_t>(result))) {
        dlssg_enabled_ = enabled;
        dlssg_applied_multiplier_ = clamped_multiplier;
        dlssg_applied_dynamic_ = use_dynamic;
        dlssg_applied_dynamic_target_fps_ = dynamic_target_fps;
        dlssg_applied_width_ = width;
        dlssg_applied_height_ = height;
        dlssg_applied_hudless_ = has_hudless_buffer;
        if (!enabled) {
            clear_tags();
        }
        return dlssg_enabled_;
    }
    dlssg_enabled_ = false;
    return false;
#else
    (void)enabled;
    (void)multiplier;
    (void)dynamic_mode;
    (void)dynamic_target_fps;
    (void)width;
    (void)height;
    (void)back_buffer_count;
    (void)has_hudless_buffer;
    return false;
#endif
}

void StreamlineBackend::prepare_for_resize() {
#if defined(FRAME_WARP_HAS_STREAMLINE) && FRAME_WARP_HAS_STREAMLINE
    if (!initialized_ || !device_attached_ || !capabilities_.dlssg_supported) return;
    const sl::ViewportHandle viewport(0U);
    sl::DLSSGOptions options{};
    options.mode = sl::DLSSGMode::eOff;
    record_api_result(StreamlineCall::sl_dlssg_set_options, static_cast<std::int32_t>(slDLSSGSetOptions(viewport, options)));
    clear_tags();
    dlssg_enabled_ = false;
    dlssg_applied_width_ = 0;
    dlssg_applied_height_ = 0;
#endif
}

StreamlineFrameState StreamlineBackend::poll_dlssg_state() {
    StreamlineFrameState frame_state{};
    frame_state.dlssg_active = dlssg_enabled_;
    frame_state.max_frames_to_generate = capabilities_.max_frames_to_generate;
    frame_state.dynamic_mfg_supported = capabilities_.dynamic_mfg_supported;
    frame_state.last_result_code = last_api_result_;
    frame_state.last_failed_call = static_cast<std::int32_t>(last_failed_call_);
    frame_state.api_error_count = api_error_count_;
#if defined(FRAME_WARP_HAS_STREAMLINE) && FRAME_WARP_HAS_STREAMLINE
    if (!initialized_ || !device_attached_ || !capabilities_.dlssg_supported) {
        return frame_state;
    }
    const sl::ViewportHandle viewport(0U);
    sl::DLSSGState state{};
    const sl::Result result = slDLSSGGetState(viewport, state, nullptr);
    if (record_api_result(StreamlineCall::sl_dlssg_get_state, static_cast<std::int32_t>(result))) {
        frame_state.dlssg_status_flags = static_cast<std::uint32_t>(state.status);
        frame_state.frames_actually_presented = state.numFramesActuallyPresented;
        frame_state.max_frames_to_generate = state.numFramesToGenerateMax;
        frame_state.dynamic_mfg_supported = (state.bIsDynamicMFGSupported == sl::Boolean::eTrue);
        frame_state.inputs_completion_fence = reinterpret_cast<ID3D12Fence*>(state.inputsProcessingCompletionFence);
        frame_state.inputs_completion_fence_value = state.lastPresentInputsProcessingCompletionFenceValue;
        capabilities_.max_frames_to_generate = state.numFramesToGenerateMax;
        capabilities_.dynamic_mfg_supported = frame_state.dynamic_mfg_supported;
        capabilities_.vsync_supported = (state.bIsVsyncSupportAvailable == sl::Boolean::eTrue);
        capabilities_.estimated_vram_bytes = state.estimatedVRAMUsageInBytes;
    }
    frame_state.last_result_code = last_api_result_;
    frame_state.last_failed_call = static_cast<std::int32_t>(last_failed_call_);
    frame_state.api_error_count = api_error_count_;
#endif
    return frame_state;
}

}
