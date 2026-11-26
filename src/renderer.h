#pragma once

#include "frame_pacer.h"
#include "latency_monitor.h"
#include "pbr_assets.h"
#include "range_mesh.h"
#include "runtime_state.h"
#include "simulation.h"
#include "streamline_backend.h"
#include "tactical_range.h"

#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <array>
#include <chrono>
#include <cstdint>

namespace frame_warp_lab {

struct alignas(16) HudGraphSample {
    float present_interval_ms;
    float gpu_total_ms;
    float frame_kind;
    float source_age_ms;
};

struct HudOverlayState {
    char lines[4][64];
    std::array<HudGraphSample, hud_graph_sample_count> samples;
    std::uint32_t write_head;
    float max_graph_ms;
    float target_frame_ms;
    std::uint32_t alignment_padding;
    bool tactical_hud_enabled;
    TacticalHudSnapshot tactical_hud;
};

struct TileCacheStats {
    std::uint32_t total_tiles = 0;
    std::uint32_t cached_tiles = 0;
    std::uint32_t shaded_tiles = 0;
    std::uint32_t deferred_tiles = 0;
    std::uint32_t critical_total_tiles = 0;
    std::uint32_t critical_deferred_tiles = 0;
    bool active = false;
};

struct CompletedSourceInfo {
    bool valid;
    std::int32_t slot_index;
    SceneSnapshot snapshot;
    std::chrono::steady_clock::time_point capture_time;
    std::int64_t scene_gpu_us;
    TileCacheStats tile_stats;
};

enum class SynthesisMode : std::int32_t {
    passthrough = 0,
    reproject = 1,
    frame_gen = 2,
    combined = 3,
};

struct FrameExecutionMetrics {
    std::chrono::steady_clock::time_point present_call_time;
    std::int64_t completed_scene_gpu_us;
    std::int64_t completed_present_gpu_us;
    std::int64_t completed_gpu_display_frame_id;
    std::int64_t completed_gpu_source_frame_id;
    std::int64_t render_submit_cpu_us;
    std::int64_t present_call_cpu_us;
    std::uint32_t completed_invalid_pixel_count;
    std::uint32_t completed_temporal_recovered_count;
    std::uint32_t completed_total_pixel_count;
    PresentVariant selected_present_variant;
    bool completed_slot_valid;
    bool completed_slot_warped;
    bool async_overlapped;
    bool missed_deadline;
    bool gpu_timing_available;
    StreamlineFrameState dlssg_state;
};

struct PresentRequest {
    std::uint64_t display_frame_id;
    std::int32_t source_slot_index;
    std::int32_t previous_source_slot_index;
    SceneSnapshot source_snapshot;
    CameraBasis previous_source_camera;
    CameraBasis target_camera;
    const HudOverlayState* hud;
    DiagnosticView diagnostic_view;
    SynthesisMode synthesis_mode;
    VrsMode active_vrs_mode;
    UpscaleFilter upscale_filter;
    InstrumentationLevel instrumentation_level;
    float extrapolation_alpha;
    bool has_previous_history;
    bool overlay_enabled;
    bool flash_indicator_enabled;
    bool flash_triggered;
    bool vsync_enabled;
    bool dlssg_requested;
    std::int32_t dlssg_multiplier;
    float dlssg_dynamic_target_fps;
    float dlssg_object_motion_scale;
    bool discontinuity;
    const wchar_t* capture_output_path = nullptr;
};

class Renderer {
public:
    explicit Renderer(HWND window);
    ~Renderer() noexcept;

    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;

    bool wait_for_frame_slot();
    bool poll_completed_scenes();
    bool has_scene_in_flight() const;
    bool has_completed_source() const { return latest_completed_slot_ >= 0; }
    bool has_previous_completed_source() const { return previous_completed_slot_ >= 0; }
    CompletedSourceInfo latest_completed_source() const;
    CompletedSourceInfo previous_completed_source() const;
    bool submit_scene_async(
        const SceneSnapshot& snapshot,
        std::chrono::steady_clock::time_point capture_time,
        std::int32_t stress_level,
        std::uint32_t shader_flags,
        bool pbr_enabled,
        bool aniso_mip_enabled,
        VrsMode vrs_mode,
        bool selective_tiles_enabled,
        std::int32_t tile_budget_pct,
        TilePolicy tile_policy,
        bool cache_sharpen_enabled,
        bool coarse_fallback_enabled,
        bool subtile_interleave_enabled,
        bool reactive_invalidation_enabled,
        bool shadow_delta_invalidation_enabled,
        std::uint64_t frame_id,
        LatencyMonitor& latency_monitor,
        bool wait_for_completion,
        const TacticalRangeFrame* range_frame = nullptr);
    void invalidate_source_history();
    FrameExecutionMetrics present_from_source(const PresentRequest& request, LatencyMonitor& latency_monitor, FramePacer& pacer);
    void resize(UINT width, UINT height);
    void set_render_resolution(UINT override_width, UINT override_height, std::int32_t scale_pct);
    UINT render_width() const { return render_width_; }
    UINT render_height() const { return render_height_; }
    UINT alloc_render_width() const { return alloc_render_width_; }
    UINT alloc_render_height() const { return alloc_render_height_; }
    std::int64_t drs_scale_changes() const { return drs_scale_changes_; }
    std::int64_t drs_reallocations() const { return drs_reallocations_; }
    void flush();
    bool gpu_timing_available() const { return present_timestamp_frequency_ > 0; }
    bool allow_tearing_enabled() const { return allow_tearing_supported_; }
    bool waitable_swap_chain_enabled() const { return frame_latency_waitable_object_ != nullptr; }
    bool high_priority_present_queue() const { return high_priority_present_queue_; }
    std::int32_t vrs_tier() const { return vrs_tier_; }
    std::int32_t vrs_tile_size_px() const { return vrs_tile_size_px_; }
    bool vrs_additional_rates_supported() const { return vrs_additional_rates_supported_; }
    VrsMode resolve_effective_vrs_mode(VrsMode requested) const;
    const StreamlineCapabilities& streamline_capabilities() const { return streamline_.capabilities(); }
    void begin_streamline_frame(std::uint64_t frame_id, bool reflex_enabled, std::uint32_t frame_limit_us);
    void emit_streamline_pcl_marker(StreamlinePclMarker marker);
    const RangeGeometryPackage& range_geometry_meta() const { return range_geometry_meta_; }
    bool hero_pbr_textures_loaded() const { return pbr_assets_.hero_textures_loaded; }
    std::int64_t pbr_build_time_ms() const { return pbr_assets_.build_time_ms; }

private:
    static constexpr UINT swap_chain_buffer_count = 2;
    static constexpr UINT dlss_input_slot_count = 3;
    static constexpr UINT rtv_targets_per_dlss_slot = 3;
    static constexpr UINT source_slot_count = 3;
    static constexpr UINT targets_per_source_slot = 3;
    static constexpr UINT compute_descriptors_per_source_slot = 8;
    static constexpr UINT pbr_srv_descriptor_count = 8;
    static constexpr UINT max_dirty_tile_capacity = 131072;
    static constexpr UINT tile_indirect_u32_count = 24;
    static constexpr UINT tile_stats_readback_u32_count = 16;
    static constexpr UINT timestamps_per_pass = 2;
    static constexpr std::size_t scene_constant_buffer_stride = 256;
    static constexpr std::size_t present_constant_buffer_stride = 512;
    static constexpr std::size_t hud_constant_buffer_stride = 3072;
    static constexpr std::size_t max_range_instances = 64;

    class OsHandle {
    public:
        OsHandle(HANDLE value, const char* operation);
        ~OsHandle();
        OsHandle(const OsHandle&) = delete;
        OsHandle& operator=(const OsHandle&) = delete;
        HANDLE get() const { return value_; }
    private:
        HANDLE value_;
    };

    struct SourceSlot {
        Microsoft::WRL::ComPtr<ID3D12Resource> color_target;
        Microsoft::WRL::ComPtr<ID3D12Resource> depth_target;
        Microsoft::WRL::ComPtr<ID3D12Resource> motion_target;
        Microsoft::WRL::ComPtr<ID3D12Resource> vis_target;
        Microsoft::WRL::ComPtr<ID3D12Resource> tile_map_target;
        Microsoft::WRL::ComPtr<ID3D12CommandAllocator> command_allocator;
        SceneSnapshot snapshot{};
        std::chrono::steady_clock::time_point capture_time{};
        std::uint64_t scene_fence_value = 0;
        std::uint64_t last_read_present_fence = 0;
        std::int64_t scene_gpu_us = -1;
        TileCacheStats tile_stats{};
        UINT active_width = 0;
        UINT active_height = 0;
        bool used_selective_tiles = false;
        bool in_flight = false;
        bool valid = false;
    };

    struct DlssInputSlot {
        Microsoft::WRL::ComPtr<ID3D12Resource> hudless_target;
        Microsoft::WRL::ComPtr<ID3D12Resource> clip_depth_target;
        Microsoft::WRL::ComPtr<ID3D12Resource> motion_target;
        std::uint64_t last_present_fence = 0;
        ID3D12Fence* dlssg_inputs_fence = nullptr;
        std::uint64_t dlssg_inputs_fence_value = 0;
    };

    void create_pipelines();
    void create_constant_buffers();
    void create_timestamp_resources();
    void create_reprojection_stats_resources();
    void create_selective_tile_resources();
    void create_pbr_assets();
    void create_range_resources();
    void create_vrs_resources();
    void create_swap_chain_targets();
    void create_dlss_input_targets();
    void create_source_targets();
    void resolve_render_dimensions(UINT& out_width, UINT& out_height) const;
    std::int32_t find_available_source_slot() const;
    UINT acquire_dlss_input_slot();
    static constexpr DWORD gpu_wait_timeout_ms = 5000;
    void wait_for_fence(ID3D12Fence* fence, HANDLE event_handle, std::uint64_t value);

    OsHandle present_fence_event_;
    OsHandle scene_fence_event_;
    HANDLE frame_latency_waitable_object_ = nullptr;
    StreamlineBackend streamline_;
    Microsoft::WRL::ComPtr<IDXGIFactory4> factory_;
    Microsoft::WRL::ComPtr<ID3D12Device> device_;
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> present_queue_;
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> scene_queue_;
    Microsoft::WRL::ComPtr<IDXGISwapChain3> swap_chain_;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> render_target_heap_;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> depth_stencil_heap_;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> shader_resource_heap_;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> scene_compute_heap_;
    std::array<Microsoft::WRL::ComPtr<ID3D12Resource>, swap_chain_buffer_count> render_targets_;
    std::array<DlssInputSlot, dlss_input_slot_count> dlss_input_slots_;
    UINT next_dlss_input_slot_ = 0;
    std::array<Microsoft::WRL::ComPtr<ID3D12CommandAllocator>, swap_chain_buffer_count> present_command_allocators_;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> present_command_list_;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> scene_command_list_;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList5> scene_command_list5_;
    Microsoft::WRL::ComPtr<ID3D12Resource> vrs_image_;
    std::array<SourceSlot, source_slot_count> source_slots_;
    Microsoft::WRL::ComPtr<ID3D12Fence> present_fence_;
    Microsoft::WRL::ComPtr<ID3D12Fence> scene_fence_;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> scene_root_signature_;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> scene_pipeline_;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> tile_compute_root_signature_;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> tile_vis_classify_pipeline_;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> tile_admit_pipeline_;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> tile_shade_indirect_pipeline_;
    Microsoft::WRL::ComPtr<ID3D12CommandSignature> dispatch_command_signature_;
    Microsoft::WRL::ComPtr<ID3D12Resource> dirty_tile_list_uav_;
    Microsoft::WRL::ComPtr<ID3D12Resource> tile_candidate_meta_uav_;
    Microsoft::WRL::ComPtr<ID3D12Resource> tile_indirect_args_uav_;
    Microsoft::WRL::ComPtr<ID3D12Resource> tile_indirect_args_init_upload_;
    Microsoft::WRL::ComPtr<ID3D12Resource> tile_stats_readback_;
    const std::uint32_t* mapped_tile_stats_ = nullptr;
    PbrAssetBundle pbr_assets_{};
    Microsoft::WRL::ComPtr<ID3D12RootSignature> range_root_signature_;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> range_pipeline_;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> range_vis_pipeline_;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> range_vis_compute_root_signature_;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> range_vis_classify_pipeline_;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> range_vis_admit_pipeline_;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> range_vis_shade_pipeline_;
    Microsoft::WRL::ComPtr<ID3D12Resource> range_depth_buffer_;
    Microsoft::WRL::ComPtr<ID3D12Resource> range_vertex_buffer_;
    Microsoft::WRL::ComPtr<ID3D12Resource> range_index_buffer_;
    Microsoft::WRL::ComPtr<ID3D12Resource> range_instance_buffer_;
    std::uint8_t* mapped_range_instances_ = nullptr;
    D3D12_VERTEX_BUFFER_VIEW range_vb_view_{};
    D3D12_INDEX_BUFFER_VIEW range_ib_view_{};
    std::array<RangeSubmesh, static_cast<std::size_t>(RangeMeshKind::count)> range_submeshes_{};
    RangeGeometryPackage range_geometry_meta_{};
    Microsoft::WRL::ComPtr<ID3D12RootSignature> present_root_signature_;
    std::array<Microsoft::WRL::ComPtr<ID3D12PipelineState>, 5> present_pipelines_;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> present_dlss_inputs_pipeline_;
    CameraBasis previous_presented_camera_{};
    bool has_previous_presented_camera_ = false;
    Microsoft::WRL::ComPtr<ID3D12Resource> scene_constant_buffer_;
    Microsoft::WRL::ComPtr<ID3D12Resource> present_constant_buffer_;
    Microsoft::WRL::ComPtr<ID3D12Resource> hud_constant_buffer_;
    std::uint8_t* mapped_scene_constants_ = nullptr;
    std::uint8_t* mapped_present_constants_ = nullptr;
    std::uint8_t* mapped_hud_constants_ = nullptr;
    Microsoft::WRL::ComPtr<ID3D12QueryHeap> present_timestamp_heap_;
    Microsoft::WRL::ComPtr<ID3D12Resource> present_timestamp_readback_;
    const std::uint64_t* mapped_present_timestamps_ = nullptr;
    std::uint64_t present_timestamp_frequency_ = 0;
    Microsoft::WRL::ComPtr<ID3D12QueryHeap> scene_timestamp_heap_;
    Microsoft::WRL::ComPtr<ID3D12Resource> scene_timestamp_readback_;
    const std::uint64_t* mapped_scene_timestamps_ = nullptr;
    std::uint64_t scene_timestamp_frequency_ = 0;
    Microsoft::WRL::ComPtr<ID3D12Resource> reprojection_stats_uav_;
    Microsoft::WRL::ComPtr<ID3D12Resource> reprojection_stats_zero_upload_;
    Microsoft::WRL::ComPtr<ID3D12Resource> reprojection_stats_readback_;
    const std::uint32_t* mapped_reprojection_stats_ = nullptr;
    Microsoft::WRL::ComPtr<ID3D12Resource> capture_readback_buffer_;
    UINT capture_readback_row_pitch_ = 0;
    std::array<std::uint64_t, swap_chain_buffer_count> buffer_fence_values_{};
    std::array<std::int64_t, swap_chain_buffer_count> buffer_display_frame_ids_{};
    std::array<std::int64_t, swap_chain_buffer_count> buffer_source_frame_ids_{};
    std::array<bool, swap_chain_buffer_count> buffer_has_data_{};
    std::array<bool, swap_chain_buffer_count> buffer_warped_{};
    std::array<bool, swap_chain_buffer_count> buffer_stats_enabled_{};
    std::array<std::uint32_t, swap_chain_buffer_count> buffer_total_pixels_{};
    std::uint64_t next_present_fence_value_ = 1;
    std::uint64_t next_scene_fence_value_ = 1;
    std::int32_t latest_completed_slot_ = -1;
    std::int32_t previous_completed_slot_ = -1;
    std::int64_t last_completed_scene_gpu_us_ = -1;
    UINT pending_buffer_index_ = 0;
    bool has_acquired_slot_ = false;
    UINT swap_chain_flags_ = 0;
    bool allow_tearing_supported_ = false;
    bool high_priority_present_queue_ = false;
    std::int32_t vrs_tier_ = 0;
    std::int32_t vrs_tile_size_px_ = 0;
    bool vrs_additional_rates_supported_ = false;
    UINT render_target_descriptor_stride_ = 0;
    UINT shader_resource_descriptor_stride_ = 0;
    UINT viewport_width_ = 0;
    UINT viewport_height_ = 0;
    UINT render_width_ = 0;
    UINT render_height_ = 0;
    UINT alloc_render_width_ = 0;
    UINT alloc_render_height_ = 0;
    UINT render_override_width_ = 0;
    UINT render_override_height_ = 0;
    std::int32_t render_scale_pct_ = 100;
    std::int64_t drs_scale_changes_ = 0;
    std::int64_t drs_reallocations_ = 0;
};

}
