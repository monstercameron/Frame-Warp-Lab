#pragma once

#include "simulation.h"

#include <windows.h>
#include <d3d12.h>

#include <cstdint>
#include <string>

namespace frame_warp_lab {

enum class StreamlineCall : std::int32_t {
    none = 0,
    sl_init = 1,
    sl_set_d3d_device = 2,
    sl_pcl_set_options = 3,
    sl_reflex_set_options = 4,
    sl_get_new_frame_token = 5,
    sl_reflex_sleep = 6,
    sl_pcl_set_marker = 7,
    sl_set_constants = 8,
    sl_set_tag_for_frame = 9,
    sl_dlssg_set_options = 10,
    sl_dlssg_get_state = 11,
    sl_shutdown = 12,
};

inline const char* streamline_call_name(std::int32_t call) {
    switch (static_cast<StreamlineCall>(call)) {
        case StreamlineCall::none: return "none";
        case StreamlineCall::sl_init: return "slInit";
        case StreamlineCall::sl_set_d3d_device: return "slSetD3DDevice";
        case StreamlineCall::sl_pcl_set_options: return "slPCLSetOptions";
        case StreamlineCall::sl_reflex_set_options: return "slReflexSetOptions";
        case StreamlineCall::sl_get_new_frame_token: return "slGetNewFrameToken";
        case StreamlineCall::sl_reflex_sleep: return "slReflexSleep";
        case StreamlineCall::sl_pcl_set_marker: return "slPCLSetMarker";
        case StreamlineCall::sl_set_constants: return "slSetConstants";
        case StreamlineCall::sl_set_tag_for_frame: return "slSetTagForFrame";
        case StreamlineCall::sl_dlssg_set_options: return "slDLSSGSetOptions";
        case StreamlineCall::sl_dlssg_get_state: return "slDLSSGGetState";
        case StreamlineCall::sl_shutdown: return "slShutdown";
        default: return "unknown";
    }
}

struct StreamlineCapabilities {
    bool compiled_with_sdk = false;
    bool initialized = false;
    bool dlssg_supported = false;
    bool dlss_sr_supported = false;
    bool reflex_supported = false;
    bool pcl_supported = false;
    bool dynamic_mfg_supported = false;
    bool vsync_supported = false;
    std::int32_t dlssg_support_result = -1;
    std::int32_t dlss_sr_support_result = -1;
    std::int32_t reflex_support_result = -1;
    std::uint32_t max_frames_to_generate = 0;
    std::uint32_t min_width_or_height = 0;
    std::uint64_t estimated_vram_bytes = 0;
    std::uint32_t sl_version_major = 0;
    std::uint32_t sl_version_minor = 0;
    std::uint32_t sl_version_build = 0;
    std::uint32_t ngx_version_major = 0;
    std::uint32_t ngx_version_minor = 0;
    std::uint32_t ngx_version_build = 0;
    std::string last_error;
};

struct StreamlineFrameState {
    bool dlssg_active = false;
    std::uint32_t dlssg_status_flags = 0;
    std::uint32_t frames_actually_presented = 0;
    std::uint32_t max_frames_to_generate = 0;
    bool dynamic_mfg_supported = false;
    std::int32_t last_result_code = 0;
    std::int32_t last_failed_call = 0;
    std::int64_t api_error_count = 0;
    ID3D12Fence* inputs_completion_fence = nullptr;
    std::uint64_t inputs_completion_fence_value = 0;
};

enum class StreamlinePclMarker : std::uint32_t {
    simulation_start = 0,
    simulation_end = 1,
    render_submit_start = 2,
    render_submit_end = 3,
    present_start = 4,
    present_end = 5,
    trigger_flash = 7,
    pc_latency_ping = 8,
    late_warp_present_start = 15,
    late_warp_present_end = 16,
    camera_constructed = 17,
    late_warp_render_submit_start = 18,
    late_warp_render_submit_end = 19,
};

class StreamlineBackend {
public:
    StreamlineBackend() = default;
    ~StreamlineBackend() noexcept;

    StreamlineBackend(const StreamlineBackend&) = delete;
    StreamlineBackend& operator=(const StreamlineBackend&) = delete;

    bool initialize(bool enable_dlssg_feature, bool quiet_logging = false);
    void query_adapter(const LUID& adapter_luid);
    bool attach_device(ID3D12Device* device);
    void shutdown() noexcept;

    bool initialized() const { return initialized_; }
    const StreamlineCapabilities& capabilities() const { return capabilities_; }
    std::string last_error() const;
    std::int64_t api_error_count() const { return api_error_count_; }
    std::int32_t last_api_result() const { return last_api_result_; }
    StreamlineCall last_failed_call() const { return last_failed_call_; }

    bool begin_frame(std::uint32_t frame_index, bool reflex_enabled, std::uint32_t frame_limit_us);
    bool set_pcl_marker(StreamlinePclMarker marker);
    bool set_common_constants(
        const CameraBasis& current_camera,
        const CameraBasis& previous_camera,
        float aspect_ratio,
        float near_plane,
        float far_plane,
        bool reset_history);
    bool tag_dlssg_resources(
        ID3D12Resource* clip_depth,
        D3D12_RESOURCE_STATES depth_state,
        ID3D12Resource* motion_vectors,
        D3D12_RESOURCE_STATES motion_state,
        ID3D12Resource* hudless_color,
        D3D12_RESOURCE_STATES hudless_state,
        UINT width,
        UINT height,
        ID3D12GraphicsCommandList* command_list);
    bool clear_tags();
    bool configure_dlssg(
        bool enabled,
        std::uint32_t multiplier,
        bool dynamic_mode,
        float dynamic_target_fps,
        UINT width,
        UINT height,
        UINT back_buffer_count,
        bool has_hudless_buffer);
    void prepare_for_resize();
    StreamlineFrameState poll_dlssg_state();

private:
    bool record_api_result(StreamlineCall call, std::int32_t result_code);

    bool initialized_ = false;
    bool device_attached_ = false;
    bool dlssg_feature_requested_ = false;
    bool reflex_configured_ = false;
    bool last_reflex_enabled_ = false;
    std::uint32_t last_reflex_limit_us_ = 0;
    bool dlssg_enabled_ = false;
    std::uint32_t dlssg_applied_multiplier_ = 0;
    bool dlssg_applied_dynamic_ = false;
    float dlssg_applied_dynamic_target_fps_ = 0.0f;
    UINT dlssg_applied_width_ = 0;
    UINT dlssg_applied_height_ = 0;
    bool dlssg_applied_hudless_ = false;
    std::int64_t api_error_count_ = 0;
    std::int32_t last_api_result_ = 0;
    StreamlineCall last_failed_call_ = StreamlineCall::none;
    void* current_frame_token_ = nullptr;
    StreamlineCapabilities capabilities_{};
};

}
