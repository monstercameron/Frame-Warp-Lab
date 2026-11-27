#include "renderer.h"
#include "render_graph.h"
#include "simd_math.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace frame_warp_lab {

namespace {

struct SceneConstantsData {
    float camera_position_tan_y[4];
    float camera_right_aspect[4];
    float camera_up_width[4];
    float camera_forward_height[4];
    float previous_camera_position_tan_y[4];
    float previous_camera_right_aspect[4];
    float previous_camera_up_cut[4];
    float previous_camera_forward_unused[4];
    float ball_center_stress[4];
    float previous_ball_center_unused[4];
    std::uint32_t tile_params[4];
    std::uint32_t cache_params[4];
    std::uint32_t policy_params[4];
    std::uint32_t prev_and_alloc_dims[4];
    std::uint32_t world_lighting_params[4];
};

struct PresentConstantsData {
    float viewport_and_flags[4];
    float warp_flags_and_aspect[4];
    float synthesis_params[4];
    float source_camera_position_tan_y[4];
    float source_camera_right_unused[4];
    float source_camera_up_unused[4];
    float source_camera_forward_unused[4];
    float target_camera_position_tan_y[4];
    float target_camera_right_unused[4];
    float target_camera_up_unused[4];
    float target_camera_forward_unused[4];
    float previous_camera_position_tan_y[4];
    float previous_camera_right_rcp_tan_x[4];
    float previous_camera_up_rcp_tan_y[4];
    float previous_camera_forward_tan_x[4];
    float vrs_params[4];
    float reprojection_thresholds[4];
    float homography_col_x[4];
    float homography_col_y[4];
    float homography_col_z[4];
    float prev_presented_camera_position_tan_y[4];
    float prev_presented_camera_right_rcp_tan_x[4];
    float prev_presented_camera_up_rcp_tan_y[4];
    float prev_presented_camera_forward_reset[4];
    float dlss_motion_params[4];
    float source_subrect_params[4];
    float prev_and_alloc_params[4];
};

struct HudConstantsData {
    float hud_panel_rect[4];
    float hud_graph_rect[4];
    float hud_graph_scale[4];
    std::uint32_t hud_text_meta[4];
    std::uint32_t hud_text_packed[64];
    float hud_graph_samples[hud_graph_sample_count][4];
    float tactical_flags[4];
    float tactical_combat[4];
    std::uint32_t tactical_ammo_hp[4];
    std::uint32_t tactical_score_meta[4];
    float tactical_player_radar[4];
    float tactical_targets[8][4];
};

static_assert(sizeof(SceneConstantsData) <= 256);
static_assert(sizeof(PresentConstantsData) <= 512);
static_assert(sizeof(HudConstantsData) <= 3072);

void check(HRESULT result, const char* operation) {
    if (FAILED(result)) {
        throw std::system_error(static_cast<int>(result), std::system_category(), operation);
    }
}

std::filesystem::path shader_directory() {
    wchar_t executable_path[32768]{};
    const DWORD path_length = GetModuleFileNameW(nullptr, executable_path, static_cast<DWORD>(std::size(executable_path)));
    if (path_length == 0 || path_length >= std::size(executable_path)) {
        throw std::runtime_error("Could not locate executable directory for shaders");
    }
    return std::filesystem::path(executable_path).parent_path() / L"shaders";
}

std::vector<std::uint8_t> load_shader_bytecode(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) {
        throw std::runtime_error("Could not open compiled shader: " + path.string());
    }
    const std::streamsize size = file.tellg();
    if (size <= 0) {
        throw std::runtime_error("Compiled shader is empty: " + path.string());
    }
    std::vector<std::uint8_t> bytecode(static_cast<std::size_t>(size));
    file.seekg(0, std::ios::beg);
    if (!file.read(reinterpret_cast<char*>(bytecode.data()), size)) {
        throw std::runtime_error("Could not read compiled shader: " + path.string());
    }
    return bytecode;
}

float dot3(Float3 a, Float3 b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

Float3 scale3(Float3 v, float s) {
    return Float3{v.x * s, v.y * s, v.z * s};
}

D3D12_RESOURCE_BARRIER transition_barrier(ID3D12Resource* resource, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = resource;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = before;
    barrier.Transition.StateAfter = after;
    return barrier;
}

D3D12_RESOURCE_BARRIER uav_barrier(ID3D12Resource* resource) {
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    barrier.UAV.pResource = resource;
    return barrier;
}

constexpr D3D12_RESOURCE_STATES source_slot_readable_state =
    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

ComPtr<ID3D12Resource> create_buffer(
    ID3D12Device* device,
    D3D12_HEAP_TYPE heap_type,
    std::size_t size_bytes,
    D3D12_RESOURCE_FLAGS flags,
    D3D12_RESOURCE_STATES initial_state,
    const char* operation) {
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = heap_type;
    D3D12_RESOURCE_DESC description{};
    description.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    description.Width = size_bytes;
    description.Height = 1;
    description.DepthOrArraySize = 1;
    description.MipLevels = 1;
    description.SampleDesc.Count = 1;
    description.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    description.Flags = flags;
    ComPtr<ID3D12Resource> resource;
    check(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &description, initial_state, nullptr, IID_PPV_ARGS(&resource)), operation);
    return resource;
}

ComPtr<ID3D12Resource> create_offscreen_texture(
    ID3D12Device* device,
    UINT width,
    UINT height,
    DXGI_FORMAT format,
    const char* operation,
    D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET,
    D3D12_RESOURCE_STATES initial_state = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE) {
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC description{};
    description.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    description.Width = width;
    description.Height = height;
    description.DepthOrArraySize = 1;
    description.MipLevels = 1;
    description.Format = format;
    description.SampleDesc.Count = 1;
    description.Flags = flags;
    ComPtr<ID3D12Resource> resource;
    check(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &description, initial_state, nullptr, IID_PPV_ARGS(&resource)), operation);
    return resource;
}

void write_float4(float (&destination)[4], Float3 vector, float w) {
    simd::store_float4(destination, vector.x, vector.y, vector.z, w);
}

void write_hud_constants_simd(HudConstantsData* destination, const HudOverlayState& hud, UINT viewport_width, UINT viewport_height) {
    const std::uint32_t char_scale = (viewport_width >= 1000 && viewport_height >= 520) ? 2U : 1U;
    constexpr std::uint32_t line_count = 4U;
    constexpr std::uint32_t chars_per_line = 48U;

    const float text_width = static_cast<float>(chars_per_line * 6U * char_scale);
    const float text_height = static_cast<float>(line_count * 9U * char_scale);
    const float graph_height = char_scale == 2U ? 104.0f : 72.0f;
    const float panel_width = std::min(static_cast<float>(viewport_width) - 24.0f, text_width + 20.0f);
    const float panel_height = std::min(static_cast<float>(viewport_height) - 24.0f, text_height + graph_height + 26.0f);
    const float panel_x = hud.tactical_hud_enabled
        ? std::max(196.0f, static_cast<float>(viewport_width) - panel_width - 14.0f)
        : 12.0f;
    if (panel_width > 40.0f && panel_height > 40.0f) {
        simd::store_float4(destination->hud_panel_rect, panel_x, 12.0f, panel_width, panel_height);
        simd::store_float4(
            destination->hud_graph_rect,
            panel_x + 10.0f,
            12.0f + 8.0f + text_height + 8.0f,
            std::max(0.0f, panel_width - 20.0f),
            std::max(0.0f, panel_height - (text_height + 26.0f)));
    } else {
        simd::store_float4(destination->hud_panel_rect, 0.0f, 0.0f, 0.0f, 0.0f);
        simd::store_float4(destination->hud_graph_rect, 0.0f, 0.0f, 0.0f, 0.0f);
    }

    simd::store_float4(
        destination->hud_graph_scale,
        hud.max_graph_ms > 1.0f ? hud.max_graph_ms : 20.0f,
        hud.target_frame_ms > 0.5f ? hud.target_frame_ms : 8.333f,
        static_cast<float>(hud_graph_sample_count),
        static_cast<float>(hud.write_head & (hud_graph_sample_count - 1)));
    simd::store_uint4(destination->hud_text_meta, line_count, chars_per_line, char_scale, 0U);

    alignas(16) std::uint32_t packed_text[64]{};
    for (std::size_t line_index = 0; line_index < 4U; ++line_index) {
        const char* line = hud.lines[line_index];
        for (std::size_t col = 0; col < 64U && line[col] != '\0'; ++col) {
            const unsigned char ascii = static_cast<unsigned char>(std::toupper(static_cast<unsigned char>(line[col])));
            const std::size_t char_index = line_index * 64U + col;
            packed_text[char_index >> 2U] |= static_cast<std::uint32_t>(ascii) << ((char_index & 3U) * 8U);
        }
    }
    simd::copy_uint4_stream(destination->hud_text_packed, packed_text, 16U);
    simd::copy_float4_stream(&destination->hud_graph_samples[0][0], &hud.samples[0].present_interval_ms, hud_graph_sample_count);

    const auto& th = hud.tactical_hud;
    simd::store_float4(
        destination->tactical_flags,
        hud.tactical_hud_enabled ? (hud.tactical_radar_enabled ? 2.0f : 1.0f) : 0.0f,
        th.ads_alpha + th.scope_alpha,
        th.crosshair_spread,
        th.hit_marker_alpha);
    simd::store_float4(
        destination->tactical_combat,
        th.hit_marker_headshot,
        th.reload_progress,
        static_cast<float>(std::clamp(th.hp, 0, 100)) / 100.0f,
        static_cast<float>(std::clamp(th.armor, 0, 100)) / 100.0f);
    simd::store_uint4(
        destination->tactical_ammo_hp,
        static_cast<std::uint32_t>(std::max(0, th.hp)),
        static_cast<std::uint32_t>(std::max(0, th.armor)),
        static_cast<std::uint32_t>(std::max(0, th.ammo_in_mag)),
        static_cast<std::uint32_t>(std::max(0, th.ammo_reserve)));
    simd::store_uint4(
        destination->tactical_score_meta,
        static_cast<std::uint32_t>(std::max(0, th.hits)),
        static_cast<std::uint32_t>(std::max(0, th.headshots)),
        static_cast<std::uint32_t>(std::max(0, th.score)),
        static_cast<std::uint32_t>(std::max(0, th.targets_up) & 0xFFFF)
            | (static_cast<std::uint32_t>(std::clamp(th.active_weapon_id, 0, 7)) << 16U));
    simd::store_float4(
        destination->tactical_player_radar,
        th.player_pos.x,
        th.player_pos.z,
        std::cos(th.player_yaw_rad),
        std::sin(th.player_yaw_rad));
    for (std::uint32_t ti = 0; ti < k_range_target_count; ++ti) {
        simd::store_float4(
            destination->tactical_targets[ti],
            th.target_radar_xy_state[ti * 4U + 0U],
            th.target_radar_xy_state[ti * 4U + 1U],
            th.target_radar_xy_state[ti * 4U + 2U],
            th.target_radar_xy_state[ti * 4U + 3U]);
    }
}

}

Renderer::OsHandle::OsHandle(HANDLE value, const char* operation) : value_(value) {
    if (!value_) throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), operation);
}

Renderer::OsHandle::~OsHandle() {
    CloseHandle(value_);
}

Renderer::Renderer(HWND window)
    : present_fence_event_(CreateEventW(nullptr, FALSE, FALSE, nullptr), "Create present fence event"),
      scene_fence_event_(CreateEventW(nullptr, FALSE, FALSE, nullptr), "Create scene fence event") {
    streamline_.initialize(true);

    UINT flags = 0;
#if defined(_DEBUG)
    ComPtr<ID3D12Debug> debug;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) {
        debug->EnableDebugLayer();
        flags = DXGI_CREATE_FACTORY_DEBUG;
    }
#endif
    check(CreateDXGIFactory2(flags, IID_PPV_ARGS(&factory_)), "Create DXGI factory");
    ComPtr<IDXGIFactory5> factory5;
    if (SUCCEEDED(factory_.As(&factory5))) {
        BOOL tearing = FALSE;
        if (SUCCEEDED(factory5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &tearing, sizeof(tearing)))) {
            allow_tearing_supported_ = tearing != FALSE;
        }
    }

    check(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device_)), "Create D3D12 device");
    device_.As(&device5_);
    streamline_.attach_device(device_.Get());

    D3D12_FEATURE_DATA_D3D12_OPTIONS5 options5{};
    if (SUCCEEDED(device_->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &options5, sizeof(options5)))) {
        dxr_tier_ = static_cast<std::int32_t>(options5.RaytracingTier);
    }

    D3D12_FEATURE_DATA_D3D12_OPTIONS6 options6{};
    if (SUCCEEDED(device_->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS6, &options6, sizeof(options6)))) {
        vrs_tier_ = static_cast<std::int32_t>(options6.VariableShadingRateTier);
        vrs_tile_size_px_ = static_cast<std::int32_t>(options6.ShadingRateImageTileSize);
        vrs_additional_rates_supported_ = options6.AdditionalShadingRatesSupported != FALSE;
    }

    D3D12_COMMAND_QUEUE_DESC present_queue_description{};
    present_queue_description.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    present_queue_description.Priority = D3D12_COMMAND_QUEUE_PRIORITY_HIGH;
    if (SUCCEEDED(device_->CreateCommandQueue(&present_queue_description, IID_PPV_ARGS(&present_queue_)))) {
        high_priority_present_queue_ = true;
    } else {
        present_queue_description.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
        check(device_->CreateCommandQueue(&present_queue_description, IID_PPV_ARGS(&present_queue_)), "Create present command queue");
    }

    D3D12_COMMAND_QUEUE_DESC scene_queue_description{};
    scene_queue_description.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    scene_queue_description.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
    check(device_->CreateCommandQueue(&scene_queue_description, IID_PPV_ARGS(&scene_queue_)), "Create scene command queue");

    RECT client_rectangle{};
    if (!GetClientRect(window, &client_rectangle)) {
        throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), "GetClientRect");
    }
    const UINT initial_width = std::max<UINT>(1, static_cast<UINT>(client_rectangle.right - client_rectangle.left));
    const UINT initial_height = std::max<UINT>(1, static_cast<UINT>(client_rectangle.bottom - client_rectangle.top));

    const bool use_waitable_swap_chain = !streamline_.initialized();
    swap_chain_flags_ = use_waitable_swap_chain ? DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT : 0U;
    if (allow_tearing_supported_) {
        swap_chain_flags_ |= DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
    }

    DXGI_SWAP_CHAIN_DESC1 swap_chain_description{};
    swap_chain_description.Width = initial_width;
    swap_chain_description.Height = initial_height;
    swap_chain_description.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    swap_chain_description.SampleDesc.Count = 1;
    swap_chain_description.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swap_chain_description.BufferCount = swap_chain_buffer_count;
    swap_chain_description.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    swap_chain_description.Flags = swap_chain_flags_;

    ComPtr<IDXGISwapChain1> swap_chain;
    check(factory_->CreateSwapChainForHwnd(present_queue_.Get(), window, &swap_chain_description, nullptr, nullptr, &swap_chain), "Create swap chain");
    check(swap_chain.As(&swap_chain_), "Query swap chain");
    check(factory_->MakeWindowAssociation(window, DXGI_MWA_NO_ALT_ENTER), "Configure window association");
    if (use_waitable_swap_chain) {
        check(swap_chain_->SetMaximumFrameLatency(swap_chain_buffer_count), "Set maximum frame latency");
        frame_latency_waitable_object_ = swap_chain_->GetFrameLatencyWaitableObject();
    }

    D3D12_DESCRIPTOR_HEAP_DESC rtv_heap_description{};
    rtv_heap_description.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtv_heap_description.NumDescriptors = swap_chain_buffer_count
        + dlss_input_slot_count * rtv_targets_per_dlss_slot
        + source_slot_count * targets_per_source_slot * 2U;
    check(device_->CreateDescriptorHeap(&rtv_heap_description, IID_PPV_ARGS(&render_target_heap_)), "Create RTV heap");
    render_target_descriptor_stride_ = device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    D3D12_DESCRIPTOR_HEAP_DESC dsv_heap_description{};
    dsv_heap_description.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    dsv_heap_description.NumDescriptors = 1;
    check(device_->CreateDescriptorHeap(&dsv_heap_description, IID_PPV_ARGS(&depth_stencil_heap_)), "Create DSV heap");

    D3D12_DESCRIPTOR_HEAP_DESC srv_heap_description{};
    srv_heap_description.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    srv_heap_description.NumDescriptors = source_slot_count * (targets_per_source_slot + 1U);
    srv_heap_description.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    check(device_->CreateDescriptorHeap(&srv_heap_description, IID_PPV_ARGS(&shader_resource_heap_)), "Create SRV heap");
    shader_resource_descriptor_stride_ = device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    D3D12_DESCRIPTOR_HEAP_DESC compute_heap_description{};
    compute_heap_description.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    compute_heap_description.NumDescriptors = source_slot_count * compute_descriptors_per_source_slot + 2U + pbr_srv_descriptor_count + ext_srv_descriptor_count * 2U;
    compute_heap_description.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    check(device_->CreateDescriptorHeap(&compute_heap_description, IID_PPV_ARGS(&scene_compute_heap_)), "Create scene compute heap");

    for (auto& allocator : present_command_allocators_) {
        check(device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)), "Create present command allocator");
    }
    check(device_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, present_command_allocators_[0].Get(), nullptr, IID_PPV_ARGS(&present_command_list_)), "Create present command list");
    check(present_command_list_->Close(), "Close initial present command list");

    for (auto& slot : source_slots_) {
        check(device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&slot.command_allocator)), "Create scene command allocator");
    }
    check(device_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, source_slots_[0].command_allocator.Get(), nullptr, IID_PPV_ARGS(&scene_command_list_)), "Create scene command list");
    scene_command_list_.As(&scene_command_list4_);
    scene_command_list_.As(&scene_command_list5_);
    check(scene_command_list_->Close(), "Close initial scene command list");

    check(device_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&present_fence_)), "Create present fence");
    check(device_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&scene_fence_)), "Create scene fence");

    viewport_width_ = initial_width;
    viewport_height_ = initial_height;
    resolve_render_dimensions(render_width_, render_height_);
    alloc_render_width_ = std::max(viewport_width_, render_width_);
    alloc_render_height_ = std::max(viewport_height_, render_height_);
    create_swap_chain_targets();
    create_dlss_input_targets();
    create_source_targets();
    create_constant_buffers();
    create_timestamp_resources();
    create_reprojection_stats_resources();
    create_selective_tile_resources();
    create_pbr_assets();
    create_range_resources();
    create_vrs_resources();
    create_pipelines();
}

Renderer::~Renderer() noexcept {
    if ((present_queue_ && present_fence_) || (scene_queue_ && scene_fence_)) {
        try {
            flush();
        } catch (...) {
        }
    }
    streamline_.shutdown();
    if (frame_latency_waitable_object_) {
        CloseHandle(frame_latency_waitable_object_);
        frame_latency_waitable_object_ = nullptr;
    }
}

void Renderer::begin_streamline_frame(std::uint64_t frame_id, bool reflex_enabled, std::uint32_t frame_limit_us) {
    streamline_.begin_frame(static_cast<std::uint32_t>(frame_id), reflex_enabled, frame_limit_us);
}

void Renderer::emit_streamline_pcl_marker(StreamlinePclMarker marker) {
    streamline_.set_pcl_marker(marker);
}

bool Renderer::wait_for_frame_slot() {
    if (viewport_width_ == 0 || viewport_height_ == 0) return false;
    if (has_acquired_slot_) return true;
    if (frame_latency_waitable_object_) {
        for (;;) {
            const DWORD wait_result = WaitForSingleObjectEx(frame_latency_waitable_object_, 1000, TRUE);
            if (wait_result == WAIT_OBJECT_0) break;
            if (wait_result == WAIT_IO_COMPLETION) continue;
            if (wait_result == WAIT_TIMEOUT) {
                const HRESULT removed_reason = device_ ? device_->GetDeviceRemovedReason() : S_OK;
                if (FAILED(removed_reason)) {
                    throw std::system_error(static_cast<int>(removed_reason), std::system_category(), "D3D12 device removed during swap-chain wait");
                }
                return false;
            }
            if (wait_result == WAIT_ABANDONED) {
                throw std::runtime_error("Swap-chain frame latency wait handle was abandoned");
            }
            throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), "Wait for swap-chain frame latency object");
        }
    }
    pending_buffer_index_ = swap_chain_->GetCurrentBackBufferIndex();
    wait_for_fence(present_fence_.Get(), present_fence_event_.get(), buffer_fence_values_[pending_buffer_index_]);
    has_acquired_slot_ = true;
    return true;
}

bool Renderer::poll_completed_scenes() {
    const std::uint64_t completed_scene_fence = scene_fence_->GetCompletedValue();
    if (completed_scene_fence == UINT64_MAX) throw std::runtime_error("D3D12 device was removed");
    bool promoted = false;
    for (UINT slot_index = 0; slot_index < source_slot_count; ++slot_index) {
        auto& slot = source_slots_[slot_index];
        if (!slot.in_flight || completed_scene_fence < slot.scene_fence_value) continue;
        slot.in_flight = false;
        slot.valid = true;
        if (mapped_scene_timestamps_ && scene_timestamp_frequency_ > 0) {
            const std::size_t base_index = static_cast<std::size_t>(slot_index) * timestamps_per_pass;
            const std::uint64_t begin_ts = mapped_scene_timestamps_[base_index + 0];
            const std::uint64_t end_ts = mapped_scene_timestamps_[base_index + 1];
            if (end_ts >= begin_ts) {
                slot.scene_gpu_us = static_cast<std::int64_t>(((end_ts - begin_ts) * 1000000ULL) / scene_timestamp_frequency_);
                last_completed_scene_gpu_us_ = slot.scene_gpu_us;
            }
        }
        if (slot.used_selective_tiles && mapped_tile_stats_) {
            const std::size_t stats_base = static_cast<std::size_t>(slot_index) * tile_stats_readback_u32_count;
            slot.tile_stats.total_tiles = mapped_tile_stats_[stats_base + 3];
            slot.tile_stats.cached_tiles = mapped_tile_stats_[stats_base + 4];
            slot.tile_stats.shaded_tiles = mapped_tile_stats_[stats_base + 0];
            slot.tile_stats.deferred_tiles = mapped_tile_stats_[stats_base + 6];
            slot.tile_stats.critical_total_tiles = mapped_tile_stats_[stats_base + 8] + mapped_tile_stats_[stats_base + 9];
            slot.tile_stats.critical_deferred_tiles = mapped_tile_stats_[stats_base + 7];
            slot.tile_stats.sc_hits = mapped_tile_stats_[stats_base + 24];
            slot.tile_stats.sc_misses = mapped_tile_stats_[stats_base + 25];
            slot.tile_stats.sc_writes = mapped_tile_stats_[stats_base + 26];
            slot.tile_stats.sc_collisions = mapped_tile_stats_[stats_base + 27];
            slot.tile_stats.clustered_light_sum = mapped_tile_stats_[stats_base + 28];
            slot.tile_stats.active = true;
        } else {
            slot.tile_stats = TileCacheStats{};
        }
        if (latest_completed_slot_ != static_cast<std::int32_t>(slot_index)) {
            previous_completed_slot_ = latest_completed_slot_;
            latest_completed_slot_ = static_cast<std::int32_t>(slot_index);
        }
        promoted = true;
    }
    return promoted;
}

bool Renderer::has_scene_in_flight() const {
    for (const auto& slot : source_slots_) {
        if (slot.in_flight) return true;
    }
    return false;
}

CompletedSourceInfo Renderer::latest_completed_source() const {
    if (latest_completed_slot_ < 0) return CompletedSourceInfo{false, -1, {}, {}, -1, {}};
    const auto& slot = source_slots_[static_cast<std::size_t>(latest_completed_slot_)];
    return CompletedSourceInfo{slot.valid, latest_completed_slot_, slot.snapshot, slot.capture_time, slot.scene_gpu_us, slot.tile_stats};
}

CompletedSourceInfo Renderer::previous_completed_source() const {
    if (previous_completed_slot_ < 0) return CompletedSourceInfo{false, -1, {}, {}, -1, {}};
    const auto& slot = source_slots_[static_cast<std::size_t>(previous_completed_slot_)];
    return CompletedSourceInfo{slot.valid, previous_completed_slot_, slot.snapshot, slot.capture_time, slot.scene_gpu_us, slot.tile_stats};
}

void Renderer::invalidate_source_history() {
    flush();
    for (auto& slot : source_slots_) {
        slot.in_flight = false;
        slot.valid = false;
        slot.used_selective_tiles = false;
        slot.tile_stats = TileCacheStats{};
        slot.scene_fence_value = 0;
        slot.last_read_present_fence = 0;
        slot.scene_gpu_us = -1;
    }
    latest_completed_slot_ = -1;
    previous_completed_slot_ = -1;
    has_previous_presented_camera_ = false;
    ++surface_cache_epoch_;
}

std::int32_t Renderer::find_available_source_slot() const {
    const std::uint64_t completed_present = present_fence_->GetCompletedValue();
    const std::uint64_t completed_scene = scene_fence_->GetCompletedValue();
    auto slot_is_free = [&](const SourceSlot& slot) {
        return !slot.in_flight && completed_scene >= slot.scene_fence_value && completed_present >= slot.last_read_present_fence;
    };
    for (UINT slot_index = 0; slot_index < source_slot_count; ++slot_index) {
        const auto signed_index = static_cast<std::int32_t>(slot_index);
        if (signed_index == latest_completed_slot_ || signed_index == previous_completed_slot_) continue;
        if (slot_is_free(source_slots_[slot_index])) {
            return signed_index;
        }
    }
    for (UINT slot_index = 0; slot_index < source_slot_count; ++slot_index) {
        const auto signed_index = static_cast<std::int32_t>(slot_index);
        if (signed_index == latest_completed_slot_) continue;
        if (slot_is_free(source_slots_[slot_index])) {
            return signed_index;
        }
    }
    return -1;
}

UINT Renderer::acquire_dlss_input_slot() {
    const UINT slot_index = next_dlss_input_slot_ % dlss_input_slot_count;
    next_dlss_input_slot_ = (slot_index + 1U) % dlss_input_slot_count;
    auto& slot = dlss_input_slots_[slot_index];
    wait_for_fence(present_fence_.Get(), present_fence_event_.get(), slot.last_present_fence);
    if (slot.dlssg_inputs_fence && slot.dlssg_inputs_fence_value > 0) {
        wait_for_fence(slot.dlssg_inputs_fence, present_fence_event_.get(), slot.dlssg_inputs_fence_value);
        slot.dlssg_inputs_fence = nullptr;
        slot.dlssg_inputs_fence_value = 0;
    }
    return slot_index;
}

VrsMode Renderer::resolve_effective_vrs_mode(VrsMode requested) const {
    if (!scene_command_list5_ || vrs_tier_ <= 0 || requested == VrsMode::off) {
        return VrsMode::off;
    }
    if (requested == VrsMode::foveated) {
        if (vrs_tier_ >= static_cast<std::int32_t>(D3D12_VARIABLE_SHADING_RATE_TIER_2) && vrs_image_) {
            return VrsMode::foveated;
        }
        return VrsMode::uniform_2x2;
    }
    return VrsMode::uniform_2x2;
}

bool Renderer::submit_scene_async(
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
    bool forward_transparency_enabled,
    bool split_diff_spec_enabled,
    bool surface_cache_enabled,
    bool clustered_lighting_enabled,
    bool thin_gbuf_enabled,
    std::int32_t surface_cache_mb,
    std::uint64_t frame_id,
    LatencyMonitor& latency_monitor,
    bool wait_for_completion,
    const TacticalRangeFrame* range_frame,
    bool is_dust2_scene) {
    if (viewport_width_ == 0 || viewport_height_ == 0 || render_width_ == 0 || render_height_ == 0) return false;
    poll_completed_scenes();
    if (has_scene_in_flight()) return false;

    const std::int32_t prev_slot_index = latest_completed_slot_;
    std::int32_t slot_index = find_available_source_slot();
    if (slot_index < 0 && wait_for_completion) {
        flush();
        poll_completed_scenes();
        slot_index = find_available_source_slot();
        if (slot_index < 0) {
            slot_index = (latest_completed_slot_ >= 0)
                ? ((latest_completed_slot_ + 1) % static_cast<std::int32_t>(source_slot_count))
                : 0;
        }
    }
    if (slot_index < 0) return false;

    auto& slot = source_slots_[static_cast<std::size_t>(slot_index)];
    const bool can_use_prev_cache = selective_tiles_enabled
        && prev_slot_index >= 0
        && prev_slot_index != slot_index
        && source_slots_[static_cast<std::size_t>(prev_slot_index)].valid
        && source_slots_[static_cast<std::size_t>(prev_slot_index)].used_selective_tiles
        && (source_slots_[static_cast<std::size_t>(prev_slot_index)].used_split_diff_spec == split_diff_spec_enabled)
        && !snapshot.discontinuity;
    const CameraBasis& prev_camera = can_use_prev_cache
        ? source_slots_[static_cast<std::size_t>(prev_slot_index)].snapshot.camera
        : snapshot.previous_camera;
    const Float3 prev_ball_center = can_use_prev_cache
        ? source_slots_[static_cast<std::size_t>(prev_slot_index)].snapshot.ball_center
        : snapshot.previous_ball_center;

    const UINT tiles_x = std::max<UINT>(1U, (render_width_ + 15U) / 16U);
    const UINT tiles_y = std::max<UINT>(1U, (render_height_ + 15U) / 16U);
    const UINT total_tiles = tiles_x * tiles_y;
    const std::int32_t clamped_budget_pct = std::clamp(tile_budget_pct, 5, 100);
    const UINT tile_budget = (!can_use_prev_cache || clamped_budget_pct >= 100)
        ? total_tiles
        : std::max<UINT>(1U, (total_tiles * static_cast<UINT>(clamped_budget_pct) + 99U) / 100U);

    const float aspect_ratio = static_cast<float>(viewport_width_) / static_cast<float>(viewport_height_);
    const float rcp_width = 1.0f / static_cast<float>(render_width_);
    const float rcp_height = 1.0f / static_cast<float>(render_height_);
    const std::size_t constant_offset = static_cast<std::size_t>(slot_index) * scene_constant_buffer_stride;
    auto* scene_constants = reinterpret_cast<SceneConstantsData*>(mapped_scene_constants_ + constant_offset);
    const float tan_y = snapshot.camera.tan_half_fov_y;
    const float tan_x = tan_y * aspect_ratio;
    const float prev_tan_y = prev_camera.tan_half_fov_y;
    const float prev_tan_x = prev_tan_y * aspect_ratio;
    write_float4(scene_constants->camera_position_tan_y, snapshot.camera.position, tan_y);
    write_float4(scene_constants->camera_right_aspect, snapshot.camera.right, tan_x);
    write_float4(scene_constants->camera_up_width, snapshot.camera.up, rcp_width);
    write_float4(scene_constants->camera_forward_height, snapshot.camera.forward, rcp_height);
    write_float4(scene_constants->previous_camera_position_tan_y, prev_camera.position, 1.0f / prev_tan_y);
    write_float4(scene_constants->previous_camera_right_aspect, prev_camera.right, 1.0f / prev_tan_x);
    write_float4(scene_constants->previous_camera_up_cut, prev_camera.up, snapshot.discontinuity ? 1.0f : 0.0f);
    write_float4(scene_constants->previous_camera_forward_unused, prev_camera.forward, static_cast<float>(snapshot.scene_time_seconds));
    write_float4(scene_constants->ball_center_stress, snapshot.ball_center, static_cast<float>(stress_level));
    const std::uint32_t packed_shader_flags = (shader_flags & 0xFFU)
        | (pbr_enabled ? 256U : 0U)
        | (aniso_mip_enabled ? 512U : 0U);
    write_float4(scene_constants->previous_ball_center_unused, prev_ball_center, static_cast<float>(packed_shader_flags));
    simd::store_uint4(
        scene_constants->tile_params,
        tiles_x,
        tiles_y,
        std::min<UINT>(tile_budget, max_dirty_tile_capacity),
        static_cast<std::uint32_t>(snapshot.source_frame_id));
    const std::uint32_t sc_mask = (surface_cache_mb >= 64)
        ? 4194303U
        : ((surface_cache_mb >= 16) ? 1048575U : 262143U);
    simd::store_uint4(
        scene_constants->cache_params,
        can_use_prev_cache ? 1U : 0U,
        range_frame != nullptr ? sc_mask : 16U,
        render_width_,
        render_height_);
    const UINT prev_active_w = can_use_prev_cache && source_slots_[static_cast<std::size_t>(prev_slot_index)].active_width > 0U
        ? source_slots_[static_cast<std::size_t>(prev_slot_index)].active_width
        : render_width_;
    const UINT prev_active_h = can_use_prev_cache && source_slots_[static_cast<std::size_t>(prev_slot_index)].active_height > 0U
        ? source_slots_[static_cast<std::size_t>(prev_slot_index)].active_height
        : render_height_;
    if (snapshot.discontinuity || surface_cache_mb != last_surface_cache_mb_) {
        ++surface_cache_epoch_;
        last_surface_cache_mb_ = surface_cache_mb;
    }
    const std::uint32_t policy_flags = (tile_policy == TilePolicy::priority_stbn ? 1U : 0U)
        | (subtile_interleave_enabled ? 2U : 0U)
        | (reactive_invalidation_enabled ? 4U : 0U)
        | (shadow_delta_invalidation_enabled ? 8U : 0U)
        | (forward_transparency_enabled ? 16U : 0U)
        | (split_diff_spec_enabled ? 32U : 0U)
        | (surface_cache_enabled ? 64U : 0U)
        | (clustered_lighting_enabled ? 128U : 0U)
        | (thin_gbuf_enabled ? 256U : 0U)
        | ((surface_cache_epoch_ & 0xFFFFU) << 16U);
    simd::store_uint4(
        scene_constants->policy_params,
        policy_flags,
        total_tiles,
        cache_sharpen_enabled ? 1U : 0U,
        coarse_fallback_enabled ? 1U : 0U);
    simd::store_uint4(
        scene_constants->prev_and_alloc_dims,
        prev_active_w,
        prev_active_h,
        alloc_render_width_,
        alloc_render_height_);
    const bool use_dust2 = is_dust2_scene && dust2_map_.loaded && dust2_vertex_buffer_ && dust2_index_buffer_;
    const bool dxr_ready = dxr_tier_ >= static_cast<std::int32_t>(D3D12_RAYTRACING_TIER_1_1)
        && ((use_dust2 && dust2_tlas_) || (!use_dust2 && range_tlas_));
    const std::uint32_t active_lights = use_dust2
        ? static_cast<std::uint32_t>(dust2_map_.lights.size())
        : range_light_count_;
    const std::uint32_t active_probes = use_dust2
        ? static_cast<std::uint32_t>(dust2_map_.probes.size())
        : range_probe_count_;
    const std::uint32_t world_flags = (use_dust2 ? 1U : 0U)
        | (dxr_ready ? 2U : 0U)
        | (thin_gbuf_enabled ? 4U : 0U);
    simd::store_uint4(
        scene_constants->world_lighting_params,
        active_lights,
        active_probes,
        world_flags,
        static_cast<std::uint32_t>(snapshot.source_frame_id));

    latency_monitor.emit_marker(LatencyMarker::render_submit_start, frame_id);
    check(slot.command_allocator->Reset(), "Reset scene command allocator");
    check(scene_command_list_->Reset(slot.command_allocator.Get(), nullptr), "Reset scene command list");

    const UINT query_base = static_cast<UINT>(slot_index) * timestamps_per_pass;
    if (scene_timestamp_heap_) {
        scene_command_list_->EndQuery(scene_timestamp_heap_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, query_base + 0);
    }

    if (range_frame != nullptr && range_pipeline_ && range_root_signature_ && range_depth_buffer_) {
        if (use_dust2) {
            select_dust2_octree_draws(
                dust2_map_,
                snapshot.camera,
                aspect_ratio,
                dust2_visible_chunks_,
                last_dust2_octree_telemetry_);
        }
        write_float4(
            scene_constants->ball_center_stress,
            range_frame->hud.muzzle_world_pos,
            range_frame->hud.muzzle_flash_alpha);
        const float prev_scene_time = can_use_prev_cache
            ? static_cast<float>(source_slots_[static_cast<std::size_t>(prev_slot_index)].snapshot.scene_time_seconds)
            : static_cast<float>(snapshot.scene_time_seconds);
        write_float4(
            scene_constants->previous_ball_center_unused,
            Float3{pbr_enabled ? 1.0f : 0.0f, aniso_mip_enabled ? 1.0f : 0.0f, prev_scene_time},
            static_cast<float>(shader_flags & 0xFFFFU));

        const std::size_t inst_slot_bytes = max_range_instances * sizeof(RangeInstanceGpu);
        const std::size_t inst_offset = static_cast<std::size_t>(slot_index) * inst_slot_bytes;
        const std::uint32_t count = std::min<std::uint32_t>(range_frame->draw_count, static_cast<std::uint32_t>(max_range_instances));
        if (count > 0U) {
            std::memcpy(mapped_range_instances_ + inst_offset, range_frame->instances.data(), static_cast<std::size_t>(count) * sizeof(RangeInstanceGpu));
        }

        const D3D12_VIEWPORT viewport{0.0f, 0.0f, static_cast<float>(render_width_), static_cast<float>(render_height_), 0.0f, 1.0f};
        const D3D12_RECT scissor{0, 0, static_cast<LONG>(render_width_), static_cast<LONG>(render_height_)};
        scene_command_list_->RSSetViewports(1, &viewport);
        scene_command_list_->RSSetScissorRects(1, &scissor);
        scene_command_list_->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

        const auto active_vb_view = use_dust2 ? dust2_vb_view_ : range_vb_view_;
        const auto active_ib_view = use_dust2 ? dust2_ib_view_ : range_ib_view_;
        ID3D12Resource* active_vb_res = use_dust2 ? dust2_vertex_buffer_.Get() : range_vertex_buffer_.Get();
        ID3D12Resource* active_ib_res = use_dust2 ? dust2_index_buffer_.Get() : range_index_buffer_.Get();
        const auto& active_vm_submeshes = use_dust2 ? dust2_viewmodel_submeshes_ : range_submeshes_;

        const auto heap_gpu_start = scene_compute_heap_->GetGPUDescriptorHandleForHeapStart();
        const UINT ext_base_slot = source_slot_count * compute_descriptors_per_source_slot
            + 2U
            + pbr_srv_descriptor_count
            + (use_dust2 ? ext_srv_descriptor_count : 0U);
        D3D12_GPU_DESCRIPTOR_HANDLE active_ext_srv_gpu{
            heap_gpu_start.ptr + static_cast<UINT64>(ext_base_slot) * shader_resource_descriptor_stride_};

        const auto draw_world_geometry = [&]() {
            if (use_dust2) {
                for (const auto& ch : dust2_visible_chunks_) {
                    const std::uint32_t meta[4] = {0U, 0U, ch.index_offset / 3U, 0U};
                    scene_command_list_->SetGraphicsRoot32BitConstants(1, 4, meta, 0);
                    scene_command_list_->DrawIndexedInstanced(ch.index_count, 1, ch.index_offset, 0, 0);
                }
                for (std::uint32_t i = 0; i < count; ++i) {
                    const auto& dc = range_frame->draws[i];
                    if (dc.viewmodel
                        || dc.mesh == RangeMeshKind::static_world
                        || dc.mesh == RangeMeshKind::target_plate
                        || dc.mesh == RangeMeshKind::transparent_glass) {
                        continue;
                    }
                    const auto& sm = dust2_viewmodel_submeshes_[static_cast<std::size_t>(dc.mesh)];
                    if (sm.index_count == 0U) continue;
                    const std::uint32_t meta[4] = {dc.instance_index, 0U, sm.index_offset / 3U, 0U};
                    scene_command_list_->SetGraphicsRoot32BitConstants(1, 4, meta, 0);
                    scene_command_list_->DrawIndexedInstanced(sm.index_count, 1, sm.index_offset, 0, 0);
                }
            } else {
                for (std::uint32_t i = 0; i < count; ++i) {
                    const auto& dc = range_frame->draws[i];
                    if (dc.viewmodel) continue;
                    const auto& sm = range_submeshes_[static_cast<std::size_t>(dc.mesh)];
                    if (sm.index_count == 0U) continue;
                    const std::uint32_t meta[4] = {dc.instance_index, 0U, sm.index_offset / 3U, 0U};
                    scene_command_list_->SetGraphicsRoot32BitConstants(1, 4, meta, 0);
                    scene_command_list_->DrawIndexedInstanced(sm.index_count, 1, sm.index_offset, 0, 0);
                }
            }
        };

        if (bots_total_vertices_ > 0U
            && mapped_skinned_bots_ != nullptr
            && skinned_bot_upload_buffer_
            && range_frame->skinned_bot_vertices.size() == bots_total_vertices_) {
            const UINT64 bots_bytes = static_cast<UINT64>(bots_total_vertices_) * sizeof(RangeVertex);
            const UINT64 src_off = static_cast<UINT64>(slot_index) * bots_bytes;
            const UINT64 dst_off = static_cast<UINT64>(use_dust2 ? dust2_bots_vb_base_vertex_ : range_bots_vb_base_vertex_) * sizeof(RangeVertex);
            std::memcpy(mapped_skinned_bots_ + src_off, range_frame->skinned_bot_vertices.data(), static_cast<std::size_t>(bots_bytes));
            const auto to_copy = transition_barrier(
                active_vb_res,
                D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                D3D12_RESOURCE_STATE_COPY_DEST);
            scene_command_list_->ResourceBarrier(1, &to_copy);
            scene_command_list_->CopyBufferRegion(active_vb_res, dst_off, skinned_bot_upload_buffer_.Get(), src_off, bots_bytes);
            const auto from_copy = transition_barrier(
                active_vb_res,
                D3D12_RESOURCE_STATE_COPY_DEST,
                D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            scene_command_list_->ResourceBarrier(1, &from_copy);
        }

        const auto draw_viewmodel_geometry = [&]() {
            for (std::uint32_t i = 0; i < count; ++i) {
                const auto& dc = range_frame->draws[i];
                if (!dc.viewmodel) continue;
                const auto& sm = active_vm_submeshes[static_cast<std::size_t>(dc.mesh)];
                if (sm.index_count == 0U) continue;
                const std::uint32_t meta[4] = {dc.instance_index, 1U, sm.index_offset / 3U, 0U};
                scene_command_list_->SetGraphicsRoot32BitConstants(1, 4, meta, 0);
                scene_command_list_->DrawIndexedInstanced(sm.index_count, 1, sm.index_offset, 0, 0);
            }
        };

        const auto dsv_handle = depth_stencil_heap_->GetCPUDescriptorHandleForHeapStart();
        const bool use_visbuffer_par = selective_tiles_enabled
            && range_vis_pipeline_
            && range_vis_compute_root_signature_
            && range_vis_classify_pipeline_
            && range_vis_admit_pipeline_
            && range_vis_shade_pipeline_
            && dispatch_command_signature_;

        if (use_visbuffer_par) {
            RenderGraphContext rg;
            rg.track(slot.color_target.Get(), source_slot_readable_state);
            rg.track(slot.opaque_cache_target.Get(), source_slot_readable_state);
            rg.track(slot.vis_target.Get(), source_slot_readable_state);
            rg.track(slot.depth_target.Get(), source_slot_readable_state);
            rg.track(slot.motion_target.Get(), source_slot_readable_state);
            rg.track(slot.tile_map_target.Get(), source_slot_readable_state);
            rg.track(slot.mat_gbuf0_target.Get(), source_slot_readable_state);
            rg.track(slot.mat_gbuf1_target.Get(), source_slot_readable_state);
            rg.track(slot.half_spec_target.Get(), source_slot_readable_state);
            rg.track(tile_indirect_args_uav_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            rg.track(tile_candidate_meta_uav_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            rg.track(dirty_tile_list_uav_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            rg.track(surface_cache_uav_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

            ID3D12DescriptorHeap* scene_heaps[] = {scene_compute_heap_.Get()};
            D3D12_GPU_DESCRIPTOR_HANDLE range_pbr_srv_gpu{
                heap_gpu_start.ptr
                + static_cast<UINT64>(source_slot_count * compute_descriptors_per_source_slot + 2U) * shader_resource_descriptor_stride_};

            rg.execute_pass(
                scene_command_list_.Get(),
                [&](RenderGraphContext& g) {
                    g.transition(slot.vis_target.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET);
                    g.transition(slot.depth_target.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET);
                    g.transition(slot.motion_target.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET);
                },
                [&]() {
                    auto vis_rtv = render_target_heap_->GetCPUDescriptorHandleForHeapStart();
                    vis_rtv.ptr += static_cast<SIZE_T>(
                        swap_chain_buffer_count
                        + dlss_input_slot_count * rtv_targets_per_dlss_slot
                        + source_slot_count * targets_per_source_slot
                        + static_cast<UINT>(slot_index) * targets_per_source_slot) * render_target_descriptor_stride_;

                    const float clear_vis[4] = {0.0f, 0.0f, 0.0f, 0.0f};
                    const float clear_depth_rtv[4] = {1000.0f, 0.0f, 0.0f, 0.0f};
                    const float clear_motion[4] = {0.0f, 0.0f, 0.0f, 0.0f};
                    D3D12_CPU_DESCRIPTOR_HANDLE rtv_v = vis_rtv;
                    D3D12_CPU_DESCRIPTOR_HANDLE rtv_d{vis_rtv.ptr + render_target_descriptor_stride_};
                    D3D12_CPU_DESCRIPTOR_HANDLE rtv_m{vis_rtv.ptr + static_cast<SIZE_T>(2U) * render_target_descriptor_stride_};
                    scene_command_list_->ClearRenderTargetView(rtv_v, clear_vis, 0, nullptr);
                    scene_command_list_->ClearRenderTargetView(rtv_d, clear_depth_rtv, 0, nullptr);
                    scene_command_list_->ClearRenderTargetView(rtv_m, clear_motion, 0, nullptr);
                    scene_command_list_->ClearDepthStencilView(dsv_handle, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);

                    scene_command_list_->SetDescriptorHeaps(1, scene_heaps);
                    scene_command_list_->OMSetRenderTargets(targets_per_source_slot, &vis_rtv, TRUE, &dsv_handle);
                    scene_command_list_->SetGraphicsRootSignature(range_root_signature_.Get());
                    scene_command_list_->SetPipelineState(range_vis_pipeline_.Get());
                    scene_command_list_->SetGraphicsRootConstantBufferView(0, scene_constant_buffer_->GetGPUVirtualAddress() + constant_offset);
                    scene_command_list_->SetGraphicsRootShaderResourceView(2, range_instance_buffer_->GetGPUVirtualAddress() + inst_offset);
                    scene_command_list_->SetGraphicsRootDescriptorTable(3, range_pbr_srv_gpu);
                    scene_command_list_->SetGraphicsRootDescriptorTable(4, active_ext_srv_gpu);
                    scene_command_list_->IASetVertexBuffers(0, 1, &active_vb_view);
                    scene_command_list_->IASetIndexBuffer(&active_ib_view);
                    if (scene_command_list5_ && vrs_tier_ > 0) {
                        scene_command_list5_->RSSetShadingRate(D3D12_SHADING_RATE_1X1, nullptr);
                        if (vrs_tier_ >= static_cast<std::int32_t>(D3D12_VARIABLE_SHADING_RATE_TIER_2)) {
                            scene_command_list5_->RSSetShadingRateImage(nullptr);
                        }
                    }

                    draw_world_geometry();
                    scene_command_list_->ClearDepthStencilView(dsv_handle, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
                    draw_viewmodel_geometry();
                });

            rg.execute_pass(
                scene_command_list_.Get(),
                [&](RenderGraphContext& g) {
                    g.transition(tile_indirect_args_uav_.Get(), D3D12_RESOURCE_STATE_COPY_DEST);
                    g.transition(tile_candidate_meta_uav_.Get(), D3D12_RESOURCE_STATE_COPY_DEST);
                },
                [&]() {
                    scene_command_list_->CopyBufferRegion(
                        tile_indirect_args_uav_.Get(),
                        0,
                        tile_indirect_args_init_upload_.Get(),
                        0,
                        static_cast<UINT64>(tile_indirect_u32_count) * sizeof(std::uint32_t));
                    scene_command_list_->CopyBufferRegion(
                        tile_candidate_meta_uav_.Get(),
                        static_cast<UINT64>(131060U) * sizeof(std::uint32_t),
                        tile_indirect_args_init_upload_.Get(),
                        12U,
                        5U * sizeof(std::uint32_t));
                });

            const std::int32_t safe_prev_slot = (prev_slot_index >= 0 && prev_slot_index != slot_index)
                ? prev_slot_index
                : ((slot_index + 1) % static_cast<std::int32_t>(source_slot_count));
            D3D12_GPU_DESCRIPTOR_HANDLE prev_srv_gpu{
                heap_gpu_start.ptr + static_cast<UINT64>(safe_prev_slot) * compute_descriptors_per_source_slot * shader_resource_descriptor_stride_};
            D3D12_GPU_DESCRIPTOR_HANDLE curr_uav_gpu{
                heap_gpu_start.ptr + (static_cast<UINT64>(slot_index) * compute_descriptors_per_source_slot + 3ULL) * shader_resource_descriptor_stride_};
            D3D12_GPU_DESCRIPTOR_HANDLE shared_uav_gpu{
                heap_gpu_start.ptr + static_cast<UINT64>(source_slot_count * compute_descriptors_per_source_slot) * shader_resource_descriptor_stride_};
            D3D12_GPU_DESCRIPTOR_HANDLE curr_final_color_uav{
                heap_gpu_start.ptr + (static_cast<UINT64>(slot_index) * compute_descriptors_per_source_slot + 8ULL) * shader_resource_descriptor_stride_};
            D3D12_GPU_DESCRIPTOR_HANDLE prev_mat_srv_gpu{
                heap_gpu_start.ptr + (static_cast<UINT64>(safe_prev_slot) * compute_descriptors_per_source_slot + 9ULL) * shader_resource_descriptor_stride_};
            D3D12_GPU_DESCRIPTOR_HANDLE curr_mat_uav_gpu{
                heap_gpu_start.ptr + (static_cast<UINT64>(slot_index) * compute_descriptors_per_source_slot + 11ULL) * shader_resource_descriptor_stride_};
            D3D12_GPU_DESCRIPTOR_HANDLE curr_half_spec_srv_gpu{
                heap_gpu_start.ptr + (static_cast<UINT64>(slot_index) * compute_descriptors_per_source_slot + 13ULL) * shader_resource_descriptor_stride_};
            D3D12_GPU_DESCRIPTOR_HANDLE curr_half_spec_uav_gpu{
                heap_gpu_start.ptr + (static_cast<UINT64>(slot_index) * compute_descriptors_per_source_slot + 14ULL) * shader_resource_descriptor_stride_};

            const bool run_split_composite = split_diff_spec_enabled && range_vis_composite_pipeline_;
            const bool run_half_res_spec = run_split_composite && thin_gbuf_enabled && range_half_spec_pipeline_;

            rg.execute_pass(
                scene_command_list_.Get(),
                [&](RenderGraphContext& g) {
                    g.transition(slot.opaque_cache_target.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                    g.transition(slot.vis_target.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                    g.transition(slot.depth_target.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                    g.transition(slot.motion_target.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                    g.transition(slot.tile_map_target.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                    g.transition(slot.mat_gbuf0_target.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                    g.transition(slot.mat_gbuf1_target.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                    g.transition(tile_indirect_args_uav_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                    g.transition(tile_candidate_meta_uav_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                },
                [&]() {
                    scene_command_list_->SetComputeRootSignature(range_vis_compute_root_signature_.Get());
                    scene_command_list_->SetComputeRootConstantBufferView(0, scene_constant_buffer_->GetGPUVirtualAddress() + constant_offset);
                    scene_command_list_->SetComputeRootDescriptorTable(1, prev_srv_gpu);
                    scene_command_list_->SetComputeRootDescriptorTable(2, curr_uav_gpu);
                    scene_command_list_->SetComputeRootDescriptorTable(3, shared_uav_gpu);
                    scene_command_list_->SetComputeRootUnorderedAccessView(4, tile_indirect_args_uav_->GetGPUVirtualAddress());
                    scene_command_list_->SetComputeRootDescriptorTable(5, range_pbr_srv_gpu);
                    scene_command_list_->SetComputeRootShaderResourceView(6, active_vb_res->GetGPUVirtualAddress());
                    scene_command_list_->SetComputeRootShaderResourceView(7, active_ib_res->GetGPUVirtualAddress());
                    scene_command_list_->SetComputeRootShaderResourceView(8, range_instance_buffer_->GetGPUVirtualAddress() + inst_offset);
                    scene_command_list_->SetComputeRootDescriptorTable(9, curr_final_color_uav);
                    scene_command_list_->SetComputeRootUnorderedAccessView(10, surface_cache_uav_->GetGPUVirtualAddress());
                    scene_command_list_->SetComputeRootDescriptorTable(11, prev_mat_srv_gpu);
                    scene_command_list_->SetComputeRootDescriptorTable(12, curr_mat_uav_gpu);
                    scene_command_list_->SetComputeRootDescriptorTable(13, active_ext_srv_gpu);
                    scene_command_list_->SetComputeRootDescriptorTable(14, curr_half_spec_srv_gpu);
                    scene_command_list_->SetComputeRootDescriptorTable(15, curr_half_spec_uav_gpu);

                    scene_command_list_->SetPipelineState(range_vis_classify_pipeline_.Get());
                    scene_command_list_->Dispatch(tiles_x, tiles_y, 1);
                });

            rg.execute_pass(
                scene_command_list_.Get(),
                [&](RenderGraphContext& g) {
                    g.uav(tile_candidate_meta_uav_.Get());
                    g.uav(tile_indirect_args_uav_.Get());
                    g.uav(slot.tile_map_target.Get());
                },
                [&]() {
                    scene_command_list_->SetPipelineState(range_vis_admit_pipeline_.Get());
                    scene_command_list_->Dispatch((total_tiles + 255U) / 256U, 1, 1);
                });

            rg.execute_pass(
                scene_command_list_.Get(),
                [&](RenderGraphContext& g) {
                    g.uav(dirty_tile_list_uav_.Get());
                    g.uav(tile_candidate_meta_uav_.Get());
                    g.uav(slot.opaque_cache_target.Get());
                    g.uav(slot.tile_map_target.Get());
                    g.uav(slot.mat_gbuf0_target.Get());
                    g.uav(slot.mat_gbuf1_target.Get());
                    g.transition(tile_indirect_args_uav_.Get(), D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
                },
                [&]() {
                    scene_command_list_->SetPipelineState(range_vis_shade_pipeline_.Get());
                    scene_command_list_->ExecuteIndirect(dispatch_command_signature_.Get(), 1, tile_indirect_args_uav_.Get(), 0, nullptr, 0);
                });

            const auto draw_range_glass = [&]() {
                const auto& glass_sm = range_submeshes_[static_cast<std::size_t>(RangeMeshKind::transparent_glass)];
                if ((shader_flags & 128U) == 0U || !range_transparent_pipeline_ || !range_transparent_root_signature_ || glass_sm.index_count == 0U) {
                    return false;
                }
                auto color_rtv = render_target_heap_->GetCPUDescriptorHandleForHeapStart();
                color_rtv.ptr += static_cast<SIZE_T>(
                    swap_chain_buffer_count
                    + dlss_input_slot_count * rtv_targets_per_dlss_slot
                    + static_cast<UINT>(slot_index) * targets_per_source_slot) * render_target_descriptor_stride_;
                D3D12_GPU_DESCRIPTOR_HANDLE curr_depth_srv{
                    heap_gpu_start.ptr + (static_cast<UINT64>(slot_index) * compute_descriptors_per_source_slot + 1ULL) * shader_resource_descriptor_stride_};
                scene_command_list_->OMSetRenderTargets(1, &color_rtv, TRUE, nullptr);
                scene_command_list_->SetGraphicsRootSignature(range_transparent_root_signature_.Get());
                scene_command_list_->SetPipelineState(range_transparent_pipeline_.Get());
                scene_command_list_->SetGraphicsRootConstantBufferView(0, scene_constant_buffer_->GetGPUVirtualAddress() + constant_offset);
                scene_command_list_->SetGraphicsRootShaderResourceView(2, range_instance_buffer_->GetGPUVirtualAddress() + inst_offset);
                scene_command_list_->SetGraphicsRootDescriptorTable(3, range_pbr_srv_gpu);
                scene_command_list_->SetGraphicsRootDescriptorTable(4, curr_depth_srv);
                scene_command_list_->IASetVertexBuffers(0, 1, &range_vb_view_);
                scene_command_list_->IASetIndexBuffer(&range_ib_view_);
                const std::uint32_t glass_meta[4] = {0U, 0U, glass_sm.index_offset / 3U, 0U};
                scene_command_list_->SetGraphicsRoot32BitConstants(1, 4, glass_meta, 0);
                scene_command_list_->DrawIndexedInstanced(glass_sm.index_count, 1, glass_sm.index_offset, 0, 0);
                return true;
            };

            if (run_split_composite) {
                if (run_half_res_spec) {
                    const UINT half_w = (render_width_ + 1U) / 2U;
                    const UINT half_h = (render_height_ + 1U) / 2U;
                    rg.execute_pass(
                        scene_command_list_.Get(),
                        [&](RenderGraphContext& g) {
                            g.transition(slot.half_spec_target.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                        },
                        [&]() {
                            scene_command_list_->SetPipelineState(range_half_spec_pipeline_.Get());
                            scene_command_list_->Dispatch((half_w + 7U) / 8U, (half_h + 7U) / 8U, 1);
                        });
                }
                rg.execute_pass(
                    scene_command_list_.Get(),
                    [&](RenderGraphContext& g) {
                        g.uav(slot.opaque_cache_target.Get());
                        g.uav(surface_cache_uav_.Get());
                        g.uav(tile_candidate_meta_uav_.Get());
                        g.uav(slot.mat_gbuf0_target.Get());
                        g.uav(slot.mat_gbuf1_target.Get());
                        g.transition(slot.half_spec_target.Get(), source_slot_readable_state);
                        g.transition(slot.color_target.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                    },
                    [&]() {
                        scene_command_list_->SetPipelineState(range_vis_composite_pipeline_.Get());
                        scene_command_list_->Dispatch(tiles_x, tiles_y, 1);
                    });

                rg.execute_pass(
                    scene_command_list_.Get(),
                    [&](RenderGraphContext& g) {
                        g.transition(slot.opaque_cache_target.Get(), source_slot_readable_state);
                        g.transition(slot.depth_target.Get(), source_slot_readable_state);
                        g.transition(slot.motion_target.Get(), source_slot_readable_state);
                        g.transition(slot.vis_target.Get(), source_slot_readable_state);
                        g.transition(slot.tile_map_target.Get(), source_slot_readable_state);
                        g.transition(slot.mat_gbuf0_target.Get(), source_slot_readable_state);
                        g.transition(slot.mat_gbuf1_target.Get(), source_slot_readable_state);
                        g.transition(tile_indirect_args_uav_.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE);
                        g.transition(tile_candidate_meta_uav_.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE);
                    },
                    [&]() {
                        scene_command_list_->CopyBufferRegion(
                            tile_stats_readback_.Get(),
                            static_cast<UINT64>(slot_index) * tile_stats_readback_u32_count * sizeof(std::uint32_t),
                            tile_indirect_args_uav_.Get(),
                            0,
                            24U * sizeof(std::uint32_t));
                        scene_command_list_->CopyBufferRegion(
                            tile_stats_readback_.Get(),
                            (static_cast<UINT64>(slot_index) * tile_stats_readback_u32_count + 24U) * sizeof(std::uint32_t),
                            tile_candidate_meta_uav_.Get(),
                            static_cast<UINT64>(131060U) * sizeof(std::uint32_t),
                            5U * sizeof(std::uint32_t));
                    });

                if (!use_dust2 && (shader_flags & 128U) != 0U) {
                    rg.execute_pass(
                        scene_command_list_.Get(),
                        [&](RenderGraphContext& g) {
                            g.transition(slot.color_target.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET);
                            g.transition(tile_indirect_args_uav_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                            g.transition(tile_candidate_meta_uav_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                        },
                        [&]() { (void)draw_range_glass(); });
                }
                rg.transition(slot.color_target.Get(), source_slot_readable_state);
                rg.transition(tile_indirect_args_uav_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                rg.transition(tile_candidate_meta_uav_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                rg.flush_barriers(scene_command_list_.Get());
            } else {
                rg.execute_pass(
                    scene_command_list_.Get(),
                    [&](RenderGraphContext& g) {
                        g.transition(slot.opaque_cache_target.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE);
                        g.transition(slot.color_target.Get(), D3D12_RESOURCE_STATE_COPY_DEST);
                        g.transition(slot.depth_target.Get(), source_slot_readable_state);
                        g.transition(slot.motion_target.Get(), source_slot_readable_state);
                        g.transition(slot.vis_target.Get(), source_slot_readable_state);
                        g.transition(slot.tile_map_target.Get(), source_slot_readable_state);
                        g.transition(slot.mat_gbuf0_target.Get(), source_slot_readable_state);
                        g.transition(slot.mat_gbuf1_target.Get(), source_slot_readable_state);
                        g.transition(tile_indirect_args_uav_.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE);
                        g.transition(tile_candidate_meta_uav_.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE);
                    },
                    [&]() {
                        scene_command_list_->CopyResource(slot.color_target.Get(), slot.opaque_cache_target.Get());
                        scene_command_list_->CopyBufferRegion(
                            tile_stats_readback_.Get(),
                            static_cast<UINT64>(slot_index) * tile_stats_readback_u32_count * sizeof(std::uint32_t),
                            tile_indirect_args_uav_.Get(),
                            0,
                            24U * sizeof(std::uint32_t));
                        scene_command_list_->CopyBufferRegion(
                            tile_stats_readback_.Get(),
                            (static_cast<UINT64>(slot_index) * tile_stats_readback_u32_count + 24U) * sizeof(std::uint32_t),
                            tile_candidate_meta_uav_.Get(),
                            static_cast<UINT64>(131060U) * sizeof(std::uint32_t),
                            5U * sizeof(std::uint32_t));
                    });

                if (!use_dust2 && (shader_flags & 128U) != 0U) {
                    bool drew_glass = false;
                    rg.execute_pass(
                        scene_command_list_.Get(),
                        [&](RenderGraphContext& g) {
                            g.transition(slot.color_target.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET);
                            g.transition(slot.opaque_cache_target.Get(), source_slot_readable_state);
                            g.transition(tile_indirect_args_uav_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                            g.transition(tile_candidate_meta_uav_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                        },
                        [&]() { drew_glass = draw_range_glass(); });
                    if (drew_glass && !forward_transparency_enabled) {
                        rg.execute_pass(
                            scene_command_list_.Get(),
                            [&](RenderGraphContext& g) {
                                g.transition(slot.color_target.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE);
                                g.transition(slot.opaque_cache_target.Get(), D3D12_RESOURCE_STATE_COPY_DEST);
                            },
                            [&]() { scene_command_list_->CopyResource(slot.opaque_cache_target.Get(), slot.color_target.Get()); });
                    }
                }
                rg.transition(slot.color_target.Get(), source_slot_readable_state);
                rg.transition(slot.opaque_cache_target.Get(), source_slot_readable_state);
                rg.transition(tile_indirect_args_uav_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                rg.transition(tile_candidate_meta_uav_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                rg.flush_barriers(scene_command_list_.Get());
            }
            slot.used_selective_tiles = true;
            slot.used_split_diff_spec = run_split_composite;
        } else {
            const std::array<D3D12_RESOURCE_BARRIER, targets_per_source_slot> to_rtv = {
                transition_barrier(slot.color_target.Get(), source_slot_readable_state, D3D12_RESOURCE_STATE_RENDER_TARGET),
                transition_barrier(slot.depth_target.Get(), source_slot_readable_state, D3D12_RESOURCE_STATE_RENDER_TARGET),
                transition_barrier(slot.motion_target.Get(), source_slot_readable_state, D3D12_RESOURCE_STATE_RENDER_TARGET),
            };
            scene_command_list_->ResourceBarrier(static_cast<UINT>(to_rtv.size()), to_rtv.data());

            auto offscreen_rtv = render_target_heap_->GetCPUDescriptorHandleForHeapStart();
            offscreen_rtv.ptr += static_cast<SIZE_T>(
                swap_chain_buffer_count
                + dlss_input_slot_count * rtv_targets_per_dlss_slot
                + static_cast<UINT>(slot_index) * targets_per_source_slot) * render_target_descriptor_stride_;

            const float clear_col[4] = {
                use_dust2 ? 0.58f : 0.035f,
                use_dust2 ? 0.68f : 0.050f,
                use_dust2 ? 0.80f : 0.068f,
                0.0f};
            const float clear_depth_rtv[4] = {1000.0f, 0.0f, 0.0f, 0.0f};
            const float clear_motion[4] = {0.0f, 0.0f, 0.0f, 0.0f};
            D3D12_CPU_DESCRIPTOR_HANDLE rtv_c = offscreen_rtv;
            D3D12_CPU_DESCRIPTOR_HANDLE rtv_d{offscreen_rtv.ptr + render_target_descriptor_stride_};
            D3D12_CPU_DESCRIPTOR_HANDLE rtv_m{offscreen_rtv.ptr + static_cast<SIZE_T>(2U) * render_target_descriptor_stride_};
            scene_command_list_->ClearRenderTargetView(rtv_c, clear_col, 0, nullptr);
            scene_command_list_->ClearRenderTargetView(rtv_d, clear_depth_rtv, 0, nullptr);
            scene_command_list_->ClearRenderTargetView(rtv_m, clear_motion, 0, nullptr);
            scene_command_list_->ClearDepthStencilView(dsv_handle, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);

            ID3D12DescriptorHeap* scene_heaps[] = {scene_compute_heap_.Get()};
            scene_command_list_->SetDescriptorHeaps(1, scene_heaps);
            scene_command_list_->OMSetRenderTargets(targets_per_source_slot, &offscreen_rtv, TRUE, &dsv_handle);
            scene_command_list_->SetGraphicsRootSignature(range_root_signature_.Get());
            scene_command_list_->SetPipelineState(range_pipeline_.Get());
            scene_command_list_->SetGraphicsRootConstantBufferView(0, scene_constant_buffer_->GetGPUVirtualAddress() + constant_offset);
            scene_command_list_->SetGraphicsRootShaderResourceView(2, range_instance_buffer_->GetGPUVirtualAddress() + inst_offset);
            D3D12_GPU_DESCRIPTOR_HANDLE range_pbr_srv_gpu{
                heap_gpu_start.ptr
                + static_cast<UINT64>(source_slot_count * compute_descriptors_per_source_slot + 2U) * shader_resource_descriptor_stride_};
            scene_command_list_->SetGraphicsRootDescriptorTable(3, range_pbr_srv_gpu);
            scene_command_list_->SetGraphicsRootDescriptorTable(4, active_ext_srv_gpu);
            scene_command_list_->IASetVertexBuffers(0, 1, &active_vb_view);
            scene_command_list_->IASetIndexBuffer(&active_ib_view);

            if (scene_command_list5_ && vrs_tier_ > 0) {
                const VrsMode effective_vrs = resolve_effective_vrs_mode(vrs_mode);
                const bool full_backing_active = (render_width_ == alloc_render_width_ && render_height_ == alloc_render_height_);
                if (effective_vrs == VrsMode::foveated && vrs_image_ && full_backing_active) {
                    const D3D12_SHADING_RATE_COMBINER combiners[2] = {
                        D3D12_SHADING_RATE_COMBINER_PASSTHROUGH,
                        D3D12_SHADING_RATE_COMBINER_OVERRIDE,
                    };
                    scene_command_list5_->RSSetShadingRate(D3D12_SHADING_RATE_1X1, combiners);
                    scene_command_list5_->RSSetShadingRateImage(vrs_image_.Get());
                } else if (effective_vrs == VrsMode::uniform_2x2 || (effective_vrs == VrsMode::foveated && !full_backing_active)) {
                    scene_command_list5_->RSSetShadingRate(D3D12_SHADING_RATE_2X2, nullptr);
                    if (vrs_tier_ >= static_cast<std::int32_t>(D3D12_VARIABLE_SHADING_RATE_TIER_2)) {
                        scene_command_list5_->RSSetShadingRateImage(nullptr);
                    }
                } else {
                    scene_command_list5_->RSSetShadingRate(D3D12_SHADING_RATE_1X1, nullptr);
                    if (vrs_tier_ >= static_cast<std::int32_t>(D3D12_VARIABLE_SHADING_RATE_TIER_2)) {
                        scene_command_list5_->RSSetShadingRateImage(nullptr);
                    }
                }
            }

            draw_world_geometry();

            scene_command_list_->ClearDepthStencilView(dsv_handle, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
            if (scene_command_list5_ && vrs_tier_ > 0) {
                scene_command_list5_->RSSetShadingRate(D3D12_SHADING_RATE_1X1, nullptr);
                if (vrs_tier_ >= static_cast<std::int32_t>(D3D12_VARIABLE_SHADING_RATE_TIER_2)) {
                    scene_command_list5_->RSSetShadingRateImage(nullptr);
                }
            }
            draw_viewmodel_geometry();

            const auto& glass_sm = range_submeshes_[static_cast<std::size_t>(RangeMeshKind::transparent_glass)];
            if (!use_dust2 && (shader_flags & 128U) != 0U && range_transparent_pipeline_ && range_transparent_root_signature_ && glass_sm.index_count > 0U) {
                const std::array<D3D12_RESOURCE_BARRIER, 2> dm_to_srv = {
                    transition_barrier(slot.depth_target.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, source_slot_readable_state),
                    transition_barrier(slot.motion_target.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, source_slot_readable_state),
                };
                scene_command_list_->ResourceBarrier(static_cast<UINT>(dm_to_srv.size()), dm_to_srv.data());
                D3D12_GPU_DESCRIPTOR_HANDLE curr_depth_srv{
                    scene_compute_heap_->GetGPUDescriptorHandleForHeapStart().ptr
                    + (static_cast<UINT64>(slot_index) * compute_descriptors_per_source_slot + 1ULL) * shader_resource_descriptor_stride_};
                scene_command_list_->OMSetRenderTargets(1, &rtv_c, TRUE, nullptr);
                scene_command_list_->SetGraphicsRootSignature(range_transparent_root_signature_.Get());
                scene_command_list_->SetPipelineState(range_transparent_pipeline_.Get());
                scene_command_list_->SetGraphicsRootConstantBufferView(0, scene_constant_buffer_->GetGPUVirtualAddress() + constant_offset);
                scene_command_list_->SetGraphicsRootShaderResourceView(2, range_instance_buffer_->GetGPUVirtualAddress() + inst_offset);
                scene_command_list_->SetGraphicsRootDescriptorTable(3, range_pbr_srv_gpu);
                scene_command_list_->SetGraphicsRootDescriptorTable(4, curr_depth_srv);
                const std::uint32_t glass_meta[4] = {0U, 0U, glass_sm.index_offset / 3U, 0U};
                scene_command_list_->SetGraphicsRoot32BitConstants(1, 4, glass_meta, 0);
                scene_command_list_->DrawIndexedInstanced(glass_sm.index_count, 1, glass_sm.index_offset, 0, 0);
                const auto color_to_srv = transition_barrier(slot.color_target.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, source_slot_readable_state);
                scene_command_list_->ResourceBarrier(1, &color_to_srv);
            } else {
                const std::array<D3D12_RESOURCE_BARRIER, targets_per_source_slot> to_srv = {
                    transition_barrier(slot.color_target.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, source_slot_readable_state),
                    transition_barrier(slot.depth_target.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, source_slot_readable_state),
                    transition_barrier(slot.motion_target.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, source_slot_readable_state),
                };
                scene_command_list_->ResourceBarrier(static_cast<UINT>(to_srv.size()), to_srv.data());
            }
            slot.used_selective_tiles = false;
        }
    } else if (selective_tiles_enabled && tile_vis_classify_pipeline_ && tile_admit_pipeline_ && tile_shade_indirect_pipeline_ && dispatch_command_signature_) {
        const std::array<D3D12_RESOURCE_BARRIER, 6> to_uav = {
            transition_barrier(slot.opaque_cache_target.Get(), source_slot_readable_state, D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
            transition_barrier(slot.depth_target.Get(), source_slot_readable_state, D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
            transition_barrier(slot.motion_target.Get(), source_slot_readable_state, D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
            transition_barrier(slot.vis_target.Get(), source_slot_readable_state, D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
            transition_barrier(slot.tile_map_target.Get(), source_slot_readable_state, D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
            transition_barrier(tile_indirect_args_uav_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST),
        };
        scene_command_list_->ResourceBarrier(static_cast<UINT>(to_uav.size()), to_uav.data());

        scene_command_list_->CopyBufferRegion(
            tile_indirect_args_uav_.Get(),
            0,
            tile_indirect_args_init_upload_.Get(),
            0,
            static_cast<UINT64>(tile_indirect_u32_count) * sizeof(std::uint32_t));
        const auto args_to_uav = transition_barrier(tile_indirect_args_uav_.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        scene_command_list_->ResourceBarrier(1, &args_to_uav);

        ID3D12DescriptorHeap* compute_heaps[] = {scene_compute_heap_.Get()};
        scene_command_list_->SetDescriptorHeaps(1, compute_heaps);
        scene_command_list_->SetComputeRootSignature(tile_compute_root_signature_.Get());
        scene_command_list_->SetPipelineState(tile_vis_classify_pipeline_.Get());
        scene_command_list_->SetComputeRootConstantBufferView(0, scene_constant_buffer_->GetGPUVirtualAddress() + constant_offset);

        const std::int32_t safe_prev_slot = (prev_slot_index >= 0 && prev_slot_index != slot_index)
            ? prev_slot_index
            : ((slot_index + 1) % static_cast<std::int32_t>(source_slot_count));
        const auto heap_gpu_start = scene_compute_heap_->GetGPUDescriptorHandleForHeapStart();
        D3D12_GPU_DESCRIPTOR_HANDLE prev_srv_gpu{
            heap_gpu_start.ptr + static_cast<UINT64>(safe_prev_slot) * compute_descriptors_per_source_slot * shader_resource_descriptor_stride_};
        D3D12_GPU_DESCRIPTOR_HANDLE curr_uav_gpu{
            heap_gpu_start.ptr + (static_cast<UINT64>(slot_index) * compute_descriptors_per_source_slot + 3ULL) * shader_resource_descriptor_stride_};
        D3D12_GPU_DESCRIPTOR_HANDLE shared_uav_gpu{
            heap_gpu_start.ptr + static_cast<UINT64>(source_slot_count * compute_descriptors_per_source_slot) * shader_resource_descriptor_stride_};
        D3D12_GPU_DESCRIPTOR_HANDLE pbr_srv_gpu{
            heap_gpu_start.ptr + static_cast<UINT64>(source_slot_count * compute_descriptors_per_source_slot + 2U) * shader_resource_descriptor_stride_};

        scene_command_list_->SetComputeRootDescriptorTable(1, prev_srv_gpu);
        scene_command_list_->SetComputeRootDescriptorTable(2, curr_uav_gpu);
        scene_command_list_->SetComputeRootDescriptorTable(3, shared_uav_gpu);
        scene_command_list_->SetComputeRootUnorderedAccessView(4, tile_indirect_args_uav_->GetGPUVirtualAddress());
        scene_command_list_->SetComputeRootDescriptorTable(5, pbr_srv_gpu);
        scene_command_list_->Dispatch(tiles_x, tiles_y, 1);

        const std::array<D3D12_RESOURCE_BARRIER, 3> admit_barriers = {
            uav_barrier(tile_candidate_meta_uav_.Get()),
            uav_barrier(tile_indirect_args_uav_.Get()),
            uav_barrier(slot.tile_map_target.Get()),
        };
        scene_command_list_->ResourceBarrier(static_cast<UINT>(admit_barriers.size()), admit_barriers.data());

        scene_command_list_->SetPipelineState(tile_admit_pipeline_.Get());
        scene_command_list_->Dispatch((total_tiles + 255U) / 256U, 1, 1);

        const std::array<D3D12_RESOURCE_BARRIER, 7> mid_barriers = {
            uav_barrier(dirty_tile_list_uav_.Get()),
            uav_barrier(tile_candidate_meta_uav_.Get()),
            uav_barrier(slot.opaque_cache_target.Get()),
            uav_barrier(slot.depth_target.Get()),
            uav_barrier(slot.vis_target.Get()),
            uav_barrier(slot.tile_map_target.Get()),
            transition_barrier(tile_indirect_args_uav_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT),
        };
        scene_command_list_->ResourceBarrier(static_cast<UINT>(mid_barriers.size()), mid_barriers.data());

        scene_command_list_->SetPipelineState(tile_shade_indirect_pipeline_.Get());
        scene_command_list_->ExecuteIndirect(
            dispatch_command_signature_.Get(),
            1,
            tile_indirect_args_uav_.Get(),
            0,
            nullptr,
            0);

        const std::array<D3D12_RESOURCE_BARRIER, 7> after_compute = {
            transition_barrier(slot.opaque_cache_target.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE),
            transition_barrier(slot.color_target.Get(), source_slot_readable_state, D3D12_RESOURCE_STATE_COPY_DEST),
            transition_barrier(slot.depth_target.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, source_slot_readable_state),
            transition_barrier(slot.motion_target.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, source_slot_readable_state),
            transition_barrier(slot.vis_target.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, source_slot_readable_state),
            transition_barrier(slot.tile_map_target.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, source_slot_readable_state),
            transition_barrier(tile_indirect_args_uav_.Get(), D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT, D3D12_RESOURCE_STATE_COPY_SOURCE),
        };
        scene_command_list_->ResourceBarrier(static_cast<UINT>(after_compute.size()), after_compute.data());

        scene_command_list_->CopyResource(slot.color_target.Get(), slot.opaque_cache_target.Get());
        scene_command_list_->CopyBufferRegion(
            tile_stats_readback_.Get(),
            static_cast<UINT64>(slot_index) * tile_stats_readback_u32_count * sizeof(std::uint32_t),
            tile_indirect_args_uav_.Get(),
            0,
            static_cast<UINT64>(tile_stats_readback_u32_count) * sizeof(std::uint32_t));

        if ((shader_flags & 128U) != 0U && forward_transparency_enabled && corridor_transparent_pipeline_ && corridor_transparent_root_signature_) {
            const std::array<D3D12_RESOURCE_BARRIER, 3> prep_ct = {
                transition_barrier(slot.color_target.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_RENDER_TARGET),
                transition_barrier(slot.opaque_cache_target.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, source_slot_readable_state),
                transition_barrier(tile_indirect_args_uav_.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
            };
            scene_command_list_->ResourceBarrier(static_cast<UINT>(prep_ct.size()), prep_ct.data());
            const D3D12_VIEWPORT ct_vp{0.0f, 0.0f, static_cast<float>(render_width_), static_cast<float>(render_height_), 0.0f, 1.0f};
            const D3D12_RECT ct_sc{0, 0, static_cast<LONG>(render_width_), static_cast<LONG>(render_height_)};
            scene_command_list_->RSSetViewports(1, &ct_vp);
            scene_command_list_->RSSetScissorRects(1, &ct_sc);
            scene_command_list_->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            auto color_rtv = render_target_heap_->GetCPUDescriptorHandleForHeapStart();
            color_rtv.ptr += static_cast<SIZE_T>(
                swap_chain_buffer_count
                + dlss_input_slot_count * rtv_targets_per_dlss_slot
                + static_cast<UINT>(slot_index) * targets_per_source_slot) * render_target_descriptor_stride_;
            D3D12_GPU_DESCRIPTOR_HANDLE curr_depth_srv{
                heap_gpu_start.ptr + (static_cast<UINT64>(slot_index) * compute_descriptors_per_source_slot + 1ULL) * shader_resource_descriptor_stride_};
            scene_command_list_->OMSetRenderTargets(1, &color_rtv, TRUE, nullptr);
            scene_command_list_->SetGraphicsRootSignature(corridor_transparent_root_signature_.Get());
            scene_command_list_->SetPipelineState(corridor_transparent_pipeline_.Get());
            scene_command_list_->SetGraphicsRootConstantBufferView(0, scene_constant_buffer_->GetGPUVirtualAddress() + constant_offset);
            scene_command_list_->SetGraphicsRootDescriptorTable(1, curr_depth_srv);
            scene_command_list_->DrawInstanced(3, 1, 0, 0);
            const auto color_to_srv = transition_barrier(slot.color_target.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, source_slot_readable_state);
            scene_command_list_->ResourceBarrier(1, &color_to_srv);
        } else {
            const std::array<D3D12_RESOURCE_BARRIER, 3> finish_comp = {
                transition_barrier(slot.color_target.Get(), D3D12_RESOURCE_STATE_COPY_DEST, source_slot_readable_state),
                transition_barrier(slot.opaque_cache_target.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, source_slot_readable_state),
                transition_barrier(tile_indirect_args_uav_.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
            };
            scene_command_list_->ResourceBarrier(static_cast<UINT>(finish_comp.size()), finish_comp.data());
        }
        slot.used_selective_tiles = true;
    } else {
        const D3D12_VIEWPORT viewport{0.0f, 0.0f, static_cast<float>(render_width_), static_cast<float>(render_height_), 0.0f, 1.0f};
        const D3D12_RECT scissor{0, 0, static_cast<LONG>(render_width_), static_cast<LONG>(render_height_)};
        scene_command_list_->RSSetViewports(1, &viewport);
        scene_command_list_->RSSetScissorRects(1, &scissor);
        scene_command_list_->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

        const std::array<D3D12_RESOURCE_BARRIER, targets_per_source_slot> to_rtv = {
            transition_barrier(slot.color_target.Get(), source_slot_readable_state, D3D12_RESOURCE_STATE_RENDER_TARGET),
            transition_barrier(slot.depth_target.Get(), source_slot_readable_state, D3D12_RESOURCE_STATE_RENDER_TARGET),
            transition_barrier(slot.motion_target.Get(), source_slot_readable_state, D3D12_RESOURCE_STATE_RENDER_TARGET),
        };
        scene_command_list_->ResourceBarrier(static_cast<UINT>(to_rtv.size()), to_rtv.data());

        auto offscreen_rtv = render_target_heap_->GetCPUDescriptorHandleForHeapStart();
        offscreen_rtv.ptr += static_cast<SIZE_T>(
            swap_chain_buffer_count
            + dlss_input_slot_count * rtv_targets_per_dlss_slot
            + static_cast<UINT>(slot_index) * targets_per_source_slot) * render_target_descriptor_stride_;
        ID3D12DescriptorHeap* scene_heaps[] = {scene_compute_heap_.Get()};
        scene_command_list_->SetDescriptorHeaps(1, scene_heaps);
        scene_command_list_->OMSetRenderTargets(targets_per_source_slot, &offscreen_rtv, TRUE, nullptr);
        scene_command_list_->SetGraphicsRootSignature(scene_root_signature_.Get());
        scene_command_list_->SetPipelineState(scene_pipeline_.Get());
        scene_command_list_->SetGraphicsRootConstantBufferView(0, scene_constant_buffer_->GetGPUVirtualAddress() + constant_offset);
        D3D12_GPU_DESCRIPTOR_HANDLE scene_pbr_srv_gpu{
            scene_compute_heap_->GetGPUDescriptorHandleForHeapStart().ptr
            + static_cast<UINT64>(source_slot_count * compute_descriptors_per_source_slot + 2U) * shader_resource_descriptor_stride_};
        scene_command_list_->SetGraphicsRootDescriptorTable(1, scene_pbr_srv_gpu);
        if (scene_command_list5_ && vrs_tier_ > 0) {
            const VrsMode effective_vrs = resolve_effective_vrs_mode(vrs_mode);
            const bool full_backing_active = (render_width_ == alloc_render_width_ && render_height_ == alloc_render_height_);
            if (effective_vrs == VrsMode::foveated && vrs_image_ && full_backing_active) {
                const D3D12_SHADING_RATE_COMBINER combiners[2] = {
                    D3D12_SHADING_RATE_COMBINER_PASSTHROUGH,
                    D3D12_SHADING_RATE_COMBINER_OVERRIDE,
                };
                scene_command_list5_->RSSetShadingRate(D3D12_SHADING_RATE_1X1, combiners);
                scene_command_list5_->RSSetShadingRateImage(vrs_image_.Get());
            } else if (effective_vrs == VrsMode::uniform_2x2 || (effective_vrs == VrsMode::foveated && !full_backing_active)) {
                scene_command_list5_->RSSetShadingRate(D3D12_SHADING_RATE_2X2, nullptr);
                if (vrs_tier_ >= static_cast<std::int32_t>(D3D12_VARIABLE_SHADING_RATE_TIER_2)) {
                    scene_command_list5_->RSSetShadingRateImage(nullptr);
                }
            } else {
                scene_command_list5_->RSSetShadingRate(D3D12_SHADING_RATE_1X1, nullptr);
                if (vrs_tier_ >= static_cast<std::int32_t>(D3D12_VARIABLE_SHADING_RATE_TIER_2)) {
                    scene_command_list5_->RSSetShadingRateImage(nullptr);
                }
            }
        }
        scene_command_list_->DrawInstanced(3, 1, 0, 0);

        if ((shader_flags & 128U) != 0U && corridor_transparent_pipeline_ && corridor_transparent_root_signature_) {
            const std::array<D3D12_RESOURCE_BARRIER, 2> dm_to_srv = {
                transition_barrier(slot.depth_target.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, source_slot_readable_state),
                transition_barrier(slot.motion_target.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, source_slot_readable_state),
            };
            scene_command_list_->ResourceBarrier(static_cast<UINT>(dm_to_srv.size()), dm_to_srv.data());
            if (scene_command_list5_ && vrs_tier_ > 0) {
                scene_command_list5_->RSSetShadingRate(D3D12_SHADING_RATE_1X1, nullptr);
                if (vrs_tier_ >= static_cast<std::int32_t>(D3D12_VARIABLE_SHADING_RATE_TIER_2)) {
                    scene_command_list5_->RSSetShadingRateImage(nullptr);
                }
            }
            D3D12_GPU_DESCRIPTOR_HANDLE curr_depth_srv{
                scene_compute_heap_->GetGPUDescriptorHandleForHeapStart().ptr
                + (static_cast<UINT64>(slot_index) * compute_descriptors_per_source_slot + 1ULL) * shader_resource_descriptor_stride_};
            scene_command_list_->OMSetRenderTargets(1, &offscreen_rtv, TRUE, nullptr);
            scene_command_list_->SetGraphicsRootSignature(corridor_transparent_root_signature_.Get());
            scene_command_list_->SetPipelineState(corridor_transparent_pipeline_.Get());
            scene_command_list_->SetGraphicsRootConstantBufferView(0, scene_constant_buffer_->GetGPUVirtualAddress() + constant_offset);
            scene_command_list_->SetGraphicsRootDescriptorTable(1, curr_depth_srv);
            scene_command_list_->DrawInstanced(3, 1, 0, 0);
            const auto color_to_srv = transition_barrier(slot.color_target.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, source_slot_readable_state);
            scene_command_list_->ResourceBarrier(1, &color_to_srv);
        } else {
            const std::array<D3D12_RESOURCE_BARRIER, targets_per_source_slot> to_srv = {
                transition_barrier(slot.color_target.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, source_slot_readable_state),
                transition_barrier(slot.depth_target.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, source_slot_readable_state),
                transition_barrier(slot.motion_target.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, source_slot_readable_state),
            };
            scene_command_list_->ResourceBarrier(static_cast<UINT>(to_srv.size()), to_srv.data());
        }
        slot.used_selective_tiles = false;
    }

    if (scene_timestamp_heap_) {
        scene_command_list_->EndQuery(scene_timestamp_heap_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, query_base + 1);
        scene_command_list_->ResolveQueryData(
            scene_timestamp_heap_.Get(),
            D3D12_QUERY_TYPE_TIMESTAMP,
            query_base,
            timestamps_per_pass,
            scene_timestamp_readback_.Get(),
            static_cast<UINT64>(query_base) * sizeof(std::uint64_t));
    }

    check(scene_command_list_->Close(), "Close scene command list");
    ID3D12CommandList* command_lists[] = {scene_command_list_.Get()};
    scene_queue_->ExecuteCommandLists(1, command_lists);
    latency_monitor.emit_marker(LatencyMarker::render_submit_end, frame_id);

    const auto fence_value = next_scene_fence_value_++;
    check(scene_queue_->Signal(scene_fence_.Get(), fence_value), "Signal scene fence");
    slot.snapshot = snapshot;
    slot.capture_time = capture_time;
    slot.active_width = render_width_;
    slot.active_height = render_height_;
    slot.scene_fence_value = fence_value;
    slot.in_flight = true;

    if (wait_for_completion) {
        wait_for_fence(scene_fence_.Get(), scene_fence_event_.get(), fence_value);
        poll_completed_scenes();
    }
    return true;
}

FrameExecutionMetrics Renderer::present_from_source(const PresentRequest& request, LatencyMonitor& latency_monitor, FramePacer& pacer) {
    FrameExecutionMetrics metrics{
        std::chrono::steady_clock::now(),
        last_completed_scene_gpu_us_,
        -1,
        -1,
        -1,
        0,
        0,
        0,
        0,
        0,
        PresentVariant::simple,
        false,
        false,
        has_scene_in_flight(),
        false,
        gpu_timing_available(),
        StreamlineFrameState{},
    };
    if (viewport_width_ == 0 || viewport_height_ == 0 || request.source_slot_index < 0) {
        return metrics;
    }

    if (!has_acquired_slot_ && !wait_for_frame_slot()) {
        return metrics;
    }
    const UINT buffer_index = pending_buffer_index_;
    has_acquired_slot_ = false;

    if (buffer_has_data_[buffer_index]) {
        metrics.completed_slot_valid = true;
        metrics.completed_slot_warped = buffer_warped_[buffer_index] && buffer_stats_enabled_[buffer_index];
        metrics.completed_gpu_display_frame_id = buffer_display_frame_ids_[buffer_index];
        metrics.completed_gpu_source_frame_id = buffer_source_frame_ids_[buffer_index];
        metrics.completed_total_pixel_count = buffer_total_pixels_[buffer_index];
        if (mapped_present_timestamps_ && present_timestamp_frequency_ > 0) {
            const std::size_t base_index = static_cast<std::size_t>(buffer_index) * timestamps_per_pass;
            const std::uint64_t present_begin = mapped_present_timestamps_[base_index + 0];
            const std::uint64_t present_end = mapped_present_timestamps_[base_index + 1];
            if (present_end >= present_begin) {
                metrics.completed_present_gpu_us = static_cast<std::int64_t>(((present_end - present_begin) * 1000000ULL) / present_timestamp_frequency_);
            }
        }
        if (buffer_warped_[buffer_index] && buffer_stats_enabled_[buffer_index] && mapped_reprojection_stats_) {
            metrics.completed_invalid_pixel_count = mapped_reprojection_stats_[buffer_index * 2U + 0U];
            metrics.completed_temporal_recovered_count = mapped_reprojection_stats_[buffer_index * 2U + 1U];
        }
    }

    const bool synthesis_active = request.synthesis_mode != SynthesisMode::passthrough;
    const bool emit_etw = request.instrumentation_level != InstrumentationLevel::minimal;
    const bool collect_stats = synthesis_active
        && (request.instrumentation_level == InstrumentationLevel::diagnostic
            || request.diagnostic_view == DiagnosticView::reprojection_holes
            || request.dlssg_requested);
    const bool render_overlay = request.overlay_enabled && request.instrumentation_level != InstrumentationLevel::minimal;
    const bool dlss_inputs_ready = dlss_input_slots_[0].hudless_target != nullptr && present_dlss_inputs_pipeline_ != nullptr;
    const bool want_hudless_output = request.dlssg_requested && render_overlay && dlss_inputs_ready;
    const bool dlssg_dynamic = request.dlssg_multiplier == 0;
    const std::uint32_t dlssg_mult = dlssg_dynamic ? 2U : static_cast<std::uint32_t>(std::max(2, request.dlssg_multiplier));
    const bool dlssg_active = dlss_inputs_ready && streamline_.configure_dlssg(
        request.dlssg_requested,
        dlssg_mult,
        dlssg_dynamic,
        request.dlssg_dynamic_target_fps,
        viewport_width_,
        viewport_height_,
        swap_chain_buffer_count,
        want_hudless_output);
    const auto submit_begin_time = std::chrono::steady_clock::now();
    if (emit_etw) {
        latency_monitor.emit_marker(
            synthesis_active ? LatencyMarker::late_warp_submit_start : LatencyMarker::render_submit_start,
            request.display_frame_id);
    }
    streamline_.set_pcl_marker(synthesis_active ? StreamlinePclMarker::late_warp_render_submit_start : StreamlinePclMarker::render_submit_start);

    const float aspect_ratio = static_cast<float>(viewport_width_) / static_cast<float>(viewport_height_);
    const CameraBasis presented_camera = synthesis_active ? request.target_camera : request.source_snapshot.camera;
    const CameraBasis prev_presented_camera = has_previous_presented_camera_
        ? previous_presented_camera_
        : request.source_snapshot.previous_camera;
    const bool dlss_reset_history = request.discontinuity || request.source_snapshot.discontinuity || !has_previous_presented_camera_;
    UINT dlss_slot_index = 0;
    if (dlssg_active) {
        dlss_slot_index = acquire_dlss_input_slot();
        streamline_.set_common_constants(presented_camera, prev_presented_camera, aspect_ratio, 0.05f, 100.0f, dlss_reset_history);
        previous_presented_camera_ = presented_camera;
        has_previous_presented_camera_ = true;
    } else {
        has_previous_presented_camera_ = false;
    }
    const float rcp_width = 1.0f / static_cast<float>(viewport_width_);
    const float rcp_height = 1.0f / static_cast<float>(viewport_height_);
    const std::size_t constant_offset = static_cast<std::size_t>(buffer_index) * present_constant_buffer_stride;
    const std::size_t hud_offset = static_cast<std::size_t>(buffer_index) * hud_constant_buffer_stride;

    auto* present_constants = reinterpret_cast<PresentConstantsData*>(mapped_present_constants_ + constant_offset);
    const float source_tan_y = request.source_snapshot.camera.tan_half_fov_y;
    const float source_tan_x = source_tan_y * aspect_ratio;
    const float target_tan_y = request.target_camera.tan_half_fov_y;
    const float target_tan_x = target_tan_y * aspect_ratio;
    const CameraBasis& history_camera = request.has_previous_history ? request.previous_source_camera : request.source_snapshot.camera;
    const float prev_tan_y = history_camera.tan_half_fov_y;
    const float prev_tan_x = prev_tan_y * aspect_ratio;
    simd::store_float4(
        present_constants->viewport_and_flags,
        static_cast<float>(viewport_width_),
        static_cast<float>(viewport_height_),
        static_cast<float>(static_cast<std::int32_t>(request.diagnostic_view)),
        render_overlay ? 1.0f : 0.0f);
    simd::store_float4(
        present_constants->warp_flags_and_aspect,
        synthesis_active ? 1.0f : 0.0f,
        aspect_ratio,
        request.flash_indicator_enabled ? 1.0f : 0.0f,
        request.flash_triggered ? 1.0f : 0.0f);
    simd::store_float4(
        present_constants->synthesis_params,
        static_cast<float>(static_cast<std::int32_t>(request.synthesis_mode)),
        request.extrapolation_alpha,
        request.has_previous_history ? 1.0f : 0.0f,
        collect_stats ? 1.0f : 0.0f);
    write_float4(present_constants->source_camera_position_tan_y, request.source_snapshot.camera.position, source_tan_y);
    write_float4(present_constants->source_camera_right_unused, request.source_snapshot.camera.right, rcp_width);
    write_float4(present_constants->source_camera_up_unused, request.source_snapshot.camera.up, rcp_height);
    write_float4(present_constants->source_camera_forward_unused, request.source_snapshot.camera.forward, source_tan_x);
    write_float4(present_constants->target_camera_position_tan_y, request.target_camera.position, target_tan_y);
    write_float4(present_constants->target_camera_right_unused, request.target_camera.right, 1.0f / target_tan_x);
    write_float4(present_constants->target_camera_up_unused, request.target_camera.up, 1.0f / target_tan_y);
    write_float4(present_constants->target_camera_forward_unused, request.target_camera.forward, target_tan_x);
    write_float4(present_constants->previous_camera_position_tan_y, history_camera.position, prev_tan_y);
    write_float4(present_constants->previous_camera_right_rcp_tan_x, history_camera.right, 1.0f / prev_tan_x);
    write_float4(present_constants->previous_camera_up_rcp_tan_y, history_camera.up, 1.0f / prev_tan_y);
    write_float4(present_constants->previous_camera_forward_tan_x, history_camera.forward, prev_tan_x);
    simd::store_float4(
        present_constants->vrs_params,
        static_cast<float>(static_cast<std::int32_t>(request.active_vrs_mode)),
        static_cast<float>(vrs_tile_size_px_ > 0 ? vrs_tile_size_px_ : 16),
        vrs_additional_rates_supported_ ? 1.0f : 0.0f,
        0.0f);
    simd::store_float4(
        present_constants->reprojection_thresholds,
        1.75f,
        0.045f,
        0.18f,
        0.65f);

    const Float3 src_proj_x = scale3(request.source_snapshot.camera.right, 1.0f / source_tan_x);
    const Float3 src_proj_y = scale3(request.source_snapshot.camera.up, 1.0f / source_tan_y);
    const Float3 src_proj_z = request.source_snapshot.camera.forward;
    const Float3 tgt_col_x = scale3(request.target_camera.right, target_tan_x);
    const Float3 tgt_col_y = scale3(request.target_camera.up, target_tan_y);
    const Float3 tgt_col_z = request.target_camera.forward;
    const auto& active_src_slot = source_slots_[static_cast<std::size_t>(request.source_slot_index)];
    const UINT src_active_w = active_src_slot.active_width > 0U ? active_src_slot.active_width : render_width_;
    const UINT src_active_h = active_src_slot.active_height > 0U ? active_src_slot.active_height : render_height_;
    const UINT prev_active_w = (request.has_previous_history && request.previous_source_slot_index >= 0 && source_slots_[static_cast<std::size_t>(request.previous_source_slot_index)].active_width > 0U)
        ? source_slots_[static_cast<std::size_t>(request.previous_source_slot_index)].active_width
        : src_active_w;
    const UINT prev_active_h = (request.has_previous_history && request.previous_source_slot_index >= 0 && source_slots_[static_cast<std::size_t>(request.previous_source_slot_index)].active_height > 0U)
        ? source_slots_[static_cast<std::size_t>(request.previous_source_slot_index)].active_height
        : src_active_h;
    const float safe_alloc_w = static_cast<float>(std::max<UINT>(alloc_render_width_, 1U));
    const float safe_alloc_h = static_cast<float>(std::max<UINT>(alloc_render_height_, 1U));
    simd::store_float4(present_constants->homography_col_x, dot3(tgt_col_x, src_proj_x), dot3(tgt_col_x, src_proj_y), dot3(tgt_col_x, src_proj_z), static_cast<float>(src_active_w));
    simd::store_float4(present_constants->homography_col_y, dot3(tgt_col_y, src_proj_x), dot3(tgt_col_y, src_proj_y), dot3(tgt_col_y, src_proj_z), static_cast<float>(src_active_h));
    simd::store_float4(present_constants->homography_col_z, dot3(tgt_col_z, src_proj_x), dot3(tgt_col_z, src_proj_y), dot3(tgt_col_z, src_proj_z), static_cast<float>(static_cast<std::int32_t>(request.upscale_filter)));
    const float prev_pres_tan_y = prev_presented_camera.tan_half_fov_y;
    const float prev_pres_tan_x = prev_pres_tan_y * aspect_ratio;
    write_float4(present_constants->prev_presented_camera_position_tan_y, prev_presented_camera.position, prev_pres_tan_y);
    write_float4(present_constants->prev_presented_camera_right_rcp_tan_x, prev_presented_camera.right, 1.0f / prev_pres_tan_x);
    write_float4(present_constants->prev_presented_camera_up_rcp_tan_y, prev_presented_camera.up, 1.0f / prev_pres_tan_y);
    write_float4(present_constants->prev_presented_camera_forward_reset, prev_presented_camera.forward, dlss_reset_history ? 1.0f : 0.0f);
    simd::store_float4(present_constants->dlss_motion_params, request.dlssg_object_motion_scale, 0.05f, 100.0f, 0.0f);
    simd::store_float4(
        present_constants->source_subrect_params,
        static_cast<float>(src_active_w),
        static_cast<float>(src_active_h),
        static_cast<float>(src_active_w) / safe_alloc_w,
        static_cast<float>(src_active_h) / safe_alloc_h);
    simd::store_float4(
        present_constants->prev_and_alloc_params,
        static_cast<float>(prev_active_w) / safe_alloc_w,
        static_cast<float>(prev_active_h) / safe_alloc_h,
        1.0f / safe_alloc_w,
        1.0f / safe_alloc_h);

    PresentVariant chosen_variant = PresentVariant::synthesis;
    if (request.diagnostic_view == DiagnosticView::final_color) {
        if (request.synthesis_mode == SynthesisMode::passthrough) {
            chosen_variant = PresentVariant::simple;
        } else if (request.synthesis_mode == SynthesisMode::reproject) {
            const float dx = request.target_camera.position.x - request.source_snapshot.camera.position.x;
            const float dy = request.target_camera.position.y - request.source_snapshot.camera.position.y;
            const float dz = request.target_camera.position.z - request.source_snapshot.camera.position.z;
            const float translation_sq = dx * dx + dy * dy + dz * dz;
            if (translation_sq < 0.0005f * 0.0005f) {
                chosen_variant = PresentVariant::rotation_homography;
            } else if (translation_sq < 0.04f * 0.04f) {
                chosen_variant = PresentVariant::depth_fast;
            } else {
                chosen_variant = PresentVariant::depth_full;
            }
        }
    }
    metrics.selected_present_variant = chosen_variant;

    if (render_overlay && request.hud) {
        auto* hud_destination = reinterpret_cast<HudConstantsData*>(mapped_hud_constants_ + hud_offset);
        write_hud_constants_simd(hud_destination, *request.hud, viewport_width_, viewport_height_);
    }

    check(present_command_allocators_[buffer_index]->Reset(), "Reset present command allocator");
    check(present_command_list_->Reset(present_command_allocators_[buffer_index].Get(), nullptr), "Reset present command list");

    const D3D12_VIEWPORT viewport{0.0f, 0.0f, static_cast<float>(viewport_width_), static_cast<float>(viewport_height_), 0.0f, 1.0f};
    const D3D12_RECT scissor{0, 0, static_cast<LONG>(viewport_width_), static_cast<LONG>(viewport_height_)};
    present_command_list_->RSSetViewports(1, &viewport);
    present_command_list_->RSSetScissorRects(1, &scissor);
    present_command_list_->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    if (collect_stats) {
        const auto stats_to_copy_dest = transition_barrier(reprojection_stats_uav_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
        present_command_list_->ResourceBarrier(1, &stats_to_copy_dest);
        present_command_list_->CopyBufferRegion(reprojection_stats_uav_.Get(), 0, reprojection_stats_zero_upload_.Get(), 0, 2U * sizeof(std::uint32_t));
        const auto stats_to_uav = transition_barrier(reprojection_stats_uav_.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        present_command_list_->ResourceBarrier(1, &stats_to_uav);
    }

    const UINT query_base = buffer_index * timestamps_per_pass;
    if (present_timestamp_heap_) {
        present_command_list_->EndQuery(present_timestamp_heap_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, query_base + 0);
    }

    if (dlssg_active) {
        auto& dlss_slot = dlss_input_slots_[dlss_slot_index];
        const std::array<D3D12_RESOURCE_BARRIER, 4> to_rtv = {
            transition_barrier(render_targets_[buffer_index].Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET),
            transition_barrier(dlss_slot.hudless_target.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET),
            transition_barrier(dlss_slot.clip_depth_target.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET),
            transition_barrier(dlss_slot.motion_target.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET),
        };
        present_command_list_->ResourceBarrier(static_cast<UINT>(to_rtv.size()), to_rtv.data());
        auto back_buffer_rtv = render_target_heap_->GetCPUDescriptorHandleForHeapStart();
        back_buffer_rtv.ptr += static_cast<SIZE_T>(buffer_index) * render_target_descriptor_stride_;
        auto dlss_rtv_base = render_target_heap_->GetCPUDescriptorHandleForHeapStart();
        dlss_rtv_base.ptr += static_cast<SIZE_T>(swap_chain_buffer_count + dlss_slot_index * rtv_targets_per_dlss_slot) * render_target_descriptor_stride_;
        D3D12_CPU_DESCRIPTOR_HANDLE hudless_rtv = dlss_rtv_base;
        D3D12_CPU_DESCRIPTOR_HANDLE clip_depth_rtv{dlss_rtv_base.ptr + render_target_descriptor_stride_};
        D3D12_CPU_DESCRIPTOR_HANDLE motion_rtv{dlss_rtv_base.ptr + 2U * render_target_descriptor_stride_};
        const D3D12_CPU_DESCRIPTOR_HANDLE rtvs[4] = {back_buffer_rtv, hudless_rtv, clip_depth_rtv, motion_rtv};
        present_command_list_->OMSetRenderTargets(4, rtvs, FALSE, nullptr);
    } else {
        const auto back_buffer_to_rtv = transition_barrier(render_targets_[buffer_index].Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
        present_command_list_->ResourceBarrier(1, &back_buffer_to_rtv);
        auto back_buffer_rtv = render_target_heap_->GetCPUDescriptorHandleForHeapStart();
        back_buffer_rtv.ptr += static_cast<SIZE_T>(buffer_index) * render_target_descriptor_stride_;
        present_command_list_->OMSetRenderTargets(1, &back_buffer_rtv, FALSE, nullptr);
    }

    ID3D12DescriptorHeap* heaps[] = {shader_resource_heap_.Get()};
    present_command_list_->SetDescriptorHeaps(1, heaps);
    present_command_list_->SetGraphicsRootSignature(present_root_signature_.Get());
    present_command_list_->SetPipelineState(
        dlssg_active
            ? present_dlss_inputs_pipeline_.Get()
            : present_pipelines_[static_cast<std::size_t>(chosen_variant)].Get());
    present_command_list_->SetGraphicsRootConstantBufferView(0, present_constant_buffer_->GetGPUVirtualAddress() + constant_offset);
    auto source_srv_gpu = shader_resource_heap_->GetGPUDescriptorHandleForHeapStart();
    source_srv_gpu.ptr += static_cast<UINT64>(request.source_slot_index) * targets_per_source_slot * shader_resource_descriptor_stride_;
    present_command_list_->SetGraphicsRootDescriptorTable(1, source_srv_gpu);
    present_command_list_->SetGraphicsRootUnorderedAccessView(2, reprojection_stats_uav_->GetGPUVirtualAddress());
    present_command_list_->SetGraphicsRootConstantBufferView(3, hud_constant_buffer_->GetGPUVirtualAddress() + hud_offset);
    const std::int32_t history_slot_index = (request.has_previous_history && request.previous_source_slot_index >= 0)
        ? request.previous_source_slot_index
        : request.source_slot_index;
    auto prev_srv_gpu = shader_resource_heap_->GetGPUDescriptorHandleForHeapStart();
    prev_srv_gpu.ptr += static_cast<UINT64>(history_slot_index) * targets_per_source_slot * shader_resource_descriptor_stride_;
    present_command_list_->SetGraphicsRootDescriptorTable(4, prev_srv_gpu);
    auto tile_map_srv_gpu = shader_resource_heap_->GetGPUDescriptorHandleForHeapStart();
    tile_map_srv_gpu.ptr += static_cast<UINT64>(source_slot_count * targets_per_source_slot + static_cast<UINT>(request.source_slot_index)) * shader_resource_descriptor_stride_;
    present_command_list_->SetGraphicsRootDescriptorTable(5, tile_map_srv_gpu);
    present_command_list_->DrawInstanced(3, 1, 0, 0);

    const bool do_capture = request.capture_output_path != nullptr && request.capture_output_path[0] != L'\0';
    if (do_capture) {
        const UINT row_pitch = (viewport_width_ * 4U + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1U) & ~(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1U);
        const std::size_t required_bytes = static_cast<std::size_t>(row_pitch) * static_cast<std::size_t>(viewport_height_);
        if (!capture_readback_buffer_ || capture_readback_row_pitch_ != row_pitch) {
            capture_readback_buffer_ = create_buffer(
                device_.Get(),
                D3D12_HEAP_TYPE_READBACK,
                required_bytes,
                D3D12_RESOURCE_FLAG_NONE,
                D3D12_RESOURCE_STATE_COPY_DEST,
                "Create backbuffer capture readback buffer");
            capture_readback_row_pitch_ = row_pitch;
        }
    }

    if (dlssg_active) {
        auto& dlss_slot = dlss_input_slots_[dlss_slot_index];
        const std::array<D3D12_RESOURCE_BARRIER, 4> after_draw = {
            transition_barrier(
                render_targets_[buffer_index].Get(),
                D3D12_RESOURCE_STATE_RENDER_TARGET,
                do_capture ? D3D12_RESOURCE_STATE_COPY_SOURCE : D3D12_RESOURCE_STATE_PRESENT),
            transition_barrier(dlss_slot.hudless_target.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE),
            transition_barrier(dlss_slot.clip_depth_target.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE),
            transition_barrier(dlss_slot.motion_target.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE),
        };
        present_command_list_->ResourceBarrier(static_cast<UINT>(after_draw.size()), after_draw.data());
        if (do_capture) {
            D3D12_TEXTURE_COPY_LOCATION src_loc{};
            src_loc.pResource = render_targets_[buffer_index].Get();
            src_loc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            src_loc.SubresourceIndex = 0;
            D3D12_TEXTURE_COPY_LOCATION dst_loc{};
            dst_loc.pResource = capture_readback_buffer_.Get();
            dst_loc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            dst_loc.PlacedFootprint.Offset = 0;
            dst_loc.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            dst_loc.PlacedFootprint.Footprint.Width = viewport_width_;
            dst_loc.PlacedFootprint.Footprint.Height = viewport_height_;
            dst_loc.PlacedFootprint.Footprint.Depth = 1;
            dst_loc.PlacedFootprint.Footprint.RowPitch = capture_readback_row_pitch_;
            present_command_list_->CopyTextureRegion(&dst_loc, 0, 0, 0, &src_loc, nullptr);
            const auto to_present = transition_barrier(render_targets_[buffer_index].Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PRESENT);
            present_command_list_->ResourceBarrier(1, &to_present);
        }
        streamline_.tag_dlssg_resources(
            dlss_slot.clip_depth_target.Get(),
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
            dlss_slot.motion_target.Get(),
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
            want_hudless_output ? dlss_slot.hudless_target.Get() : nullptr,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
            viewport_width_,
            viewport_height_,
            present_command_list_.Get());
    } else if (do_capture) {
        const auto to_copy = transition_barrier(render_targets_[buffer_index].Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
        present_command_list_->ResourceBarrier(1, &to_copy);
        D3D12_TEXTURE_COPY_LOCATION src_loc{};
        src_loc.pResource = render_targets_[buffer_index].Get();
        src_loc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        src_loc.SubresourceIndex = 0;
        D3D12_TEXTURE_COPY_LOCATION dst_loc{};
        dst_loc.pResource = capture_readback_buffer_.Get();
        dst_loc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst_loc.PlacedFootprint.Offset = 0;
        dst_loc.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        dst_loc.PlacedFootprint.Footprint.Width = viewport_width_;
        dst_loc.PlacedFootprint.Footprint.Height = viewport_height_;
        dst_loc.PlacedFootprint.Footprint.Depth = 1;
        dst_loc.PlacedFootprint.Footprint.RowPitch = capture_readback_row_pitch_;
        present_command_list_->CopyTextureRegion(&dst_loc, 0, 0, 0, &src_loc, nullptr);
        const auto to_present = transition_barrier(render_targets_[buffer_index].Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PRESENT);
        present_command_list_->ResourceBarrier(1, &to_present);
    } else {
        const auto back_buffer_to_present = transition_barrier(render_targets_[buffer_index].Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
        present_command_list_->ResourceBarrier(1, &back_buffer_to_present);
    }

    if (present_timestamp_heap_) {
        present_command_list_->EndQuery(present_timestamp_heap_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, query_base + 1);
        present_command_list_->ResolveQueryData(
            present_timestamp_heap_.Get(),
            D3D12_QUERY_TYPE_TIMESTAMP,
            query_base,
            timestamps_per_pass,
            present_timestamp_readback_.Get(),
            static_cast<UINT64>(query_base) * sizeof(std::uint64_t));
    }

    if (collect_stats) {
        const auto stats_to_copy_source = transition_barrier(reprojection_stats_uav_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
        present_command_list_->ResourceBarrier(1, &stats_to_copy_source);
        present_command_list_->CopyBufferRegion(
            reprojection_stats_readback_.Get(),
            static_cast<UINT64>(buffer_index) * 2U * sizeof(std::uint32_t),
            reprojection_stats_uav_.Get(),
            0,
            2U * sizeof(std::uint32_t));
        const auto stats_restore_uav = transition_barrier(reprojection_stats_uav_.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        present_command_list_->ResourceBarrier(1, &stats_restore_uav);
    }

    check(present_command_list_->Close(), "Close present command list");
    ID3D12CommandList* command_lists[] = {present_command_list_.Get()};
    present_queue_->ExecuteCommandLists(1, command_lists);

    const auto submit_end_time = std::chrono::steady_clock::now();
    if (emit_etw) {
        latency_monitor.emit_marker(
            synthesis_active ? LatencyMarker::late_warp_submit_end : LatencyMarker::render_submit_end,
            request.display_frame_id);
    }
    streamline_.set_pcl_marker(synthesis_active ? StreamlinePclMarker::late_warp_render_submit_end : StreamlinePclMarker::render_submit_end);
    metrics.render_submit_cpu_us = std::chrono::duration_cast<std::chrono::microseconds>(submit_end_time - submit_begin_time).count();

    metrics.missed_deadline = dlssg_active ? false : pacer.wait_for_present_deadline();

    const auto present_begin_time = std::chrono::steady_clock::now();
    metrics.present_call_time = present_begin_time;
    if (emit_etw) {
        latency_monitor.emit_marker(
            synthesis_active ? LatencyMarker::late_warp_present_start : LatencyMarker::present_start,
            request.display_frame_id);
    }
    streamline_.set_pcl_marker(synthesis_active ? StreamlinePclMarker::late_warp_present_start : StreamlinePclMarker::present_start);

    const bool effective_vsync = dlssg_active
        ? (request.vsync_enabled && streamline_.capabilities().vsync_supported)
        : request.vsync_enabled;
    const UINT sync_interval = effective_vsync ? 1U : 0U;
    const UINT present_flags = (!effective_vsync && allow_tearing_supported_) ? DXGI_PRESENT_ALLOW_TEARING : 0U;
    check(swap_chain_->Present(sync_interval, present_flags), "Present");

    const auto present_end_time = std::chrono::steady_clock::now();
    if (emit_etw) {
        latency_monitor.emit_marker(
            synthesis_active ? LatencyMarker::late_warp_present_end : LatencyMarker::present_end,
            request.display_frame_id);
    }
    streamline_.set_pcl_marker(synthesis_active ? StreamlinePclMarker::late_warp_present_end : StreamlinePclMarker::present_end);
    metrics.present_call_cpu_us = std::chrono::duration_cast<std::chrono::microseconds>(present_end_time - present_begin_time).count();

    if (dlssg_active) {
        metrics.dlssg_state = streamline_.poll_dlssg_state();
        if (metrics.dlssg_state.inputs_completion_fence && metrics.dlssg_state.inputs_completion_fence_value > 0) {
            dlss_input_slots_[dlss_slot_index].dlssg_inputs_fence = metrics.dlssg_state.inputs_completion_fence;
            dlss_input_slots_[dlss_slot_index].dlssg_inputs_fence_value = metrics.dlssg_state.inputs_completion_fence_value;
        }
    } else {
        metrics.dlssg_state.last_result_code = streamline_.last_api_result();
        metrics.dlssg_state.last_failed_call = static_cast<std::int32_t>(streamline_.last_failed_call());
        metrics.dlssg_state.api_error_count = streamline_.api_error_count();
    }

    const auto fence_value = next_present_fence_value_++;
    check(present_queue_->Signal(present_fence_.Get(), fence_value), "Signal present fence");
    if (do_capture && capture_readback_buffer_) {
        wait_for_fence(present_fence_.Get(), present_fence_event_.get(), fence_value);
        const std::size_t total_bytes = static_cast<std::size_t>(capture_readback_row_pitch_) * static_cast<std::size_t>(viewport_height_);
        const D3D12_RANGE read_range{0, total_bytes};
        const std::uint8_t* mapped_pixels = nullptr;
        if (SUCCEEDED(capture_readback_buffer_->Map(0, &read_range, reinterpret_cast<void**>(const_cast<std::uint8_t**>(&mapped_pixels))))) {
            std::filesystem::path output_path(request.capture_output_path);
            std::error_code ec;
            if (output_path.has_parent_path()) {
                std::filesystem::create_directories(output_path.parent_path(), ec);
            }
            std::ofstream out_file(output_path, std::ios::binary | std::ios::trunc);
            if (out_file) {
                const std::string header = "P6\n" + std::to_string(viewport_width_) + " " + std::to_string(viewport_height_) + "\n255\n";
                out_file.write(header.data(), static_cast<std::streamsize>(header.size()));
                std::vector<std::uint8_t> row_rgb(static_cast<std::size_t>(viewport_width_) * 3U);
                for (UINT y = 0; y < viewport_height_; ++y) {
                    const std::uint8_t* src_row = mapped_pixels + static_cast<std::size_t>(y) * capture_readback_row_pitch_;
                    for (UINT x = 0; x < viewport_width_; ++x) {
                        row_rgb[static_cast<std::size_t>(x) * 3U + 0U] = src_row[static_cast<std::size_t>(x) * 4U + 0U];
                        row_rgb[static_cast<std::size_t>(x) * 3U + 1U] = src_row[static_cast<std::size_t>(x) * 4U + 1U];
                        row_rgb[static_cast<std::size_t>(x) * 3U + 2U] = src_row[static_cast<std::size_t>(x) * 4U + 2U];
                    }
                    out_file.write(reinterpret_cast<const char*>(row_rgb.data()), static_cast<std::streamsize>(row_rgb.size()));
                }
            }
            const D3D12_RANGE write_range{0, 0};
            capture_readback_buffer_->Unmap(0, &write_range);
        }
    }
    buffer_fence_values_[buffer_index] = fence_value;
    if (dlssg_active) {
        dlss_input_slots_[dlss_slot_index].last_present_fence = fence_value;
    }
    buffer_display_frame_ids_[buffer_index] = static_cast<std::int64_t>(request.display_frame_id);
    buffer_source_frame_ids_[buffer_index] = request.source_snapshot.source_frame_id;
    buffer_has_data_[buffer_index] = true;
    buffer_warped_[buffer_index] = synthesis_active;
    buffer_stats_enabled_[buffer_index] = collect_stats;
    buffer_total_pixels_[buffer_index] = viewport_width_ * viewport_height_;
    source_slots_[static_cast<std::size_t>(request.source_slot_index)].last_read_present_fence = fence_value;
    if (history_slot_index >= 0 && history_slot_index != request.source_slot_index) {
        source_slots_[static_cast<std::size_t>(history_slot_index)].last_read_present_fence = fence_value;
    }

    return metrics;
}

void Renderer::resize(UINT width, UINT height) {
    if (width == 0 || height == 0) return;
    streamline_.prepare_for_resize();
    flush();
    has_acquired_slot_ = false;
    for (auto& target : render_targets_) target.Reset();
    for (auto& dlss_slot : dlss_input_slots_) {
        dlss_slot.hudless_target.Reset();
        dlss_slot.clip_depth_target.Reset();
        dlss_slot.motion_target.Reset();
        dlss_slot.last_present_fence = 0;
        dlss_slot.dlssg_inputs_fence = nullptr;
        dlss_slot.dlssg_inputs_fence_value = 0;
    }
    next_dlss_input_slot_ = 0;
    for (auto& slot : source_slots_) {
        slot.color_target.Reset();
        slot.opaque_cache_target.Reset();
        slot.depth_target.Reset();
        slot.motion_target.Reset();
        slot.vis_target.Reset();
        slot.tile_map_target.Reset();
        slot.mat_gbuf0_target.Reset();
        slot.mat_gbuf1_target.Reset();
        slot.half_spec_target.Reset();
        slot.tile_stats = TileCacheStats{};
        slot.used_selective_tiles = false;
        slot.in_flight = false;
        slot.valid = false;
        slot.scene_fence_value = 0;
        slot.last_read_present_fence = 0;
        slot.scene_gpu_us = -1;
    }
    latest_completed_slot_ = -1;
    previous_completed_slot_ = -1;
    has_previous_presented_camera_ = false;
    check(swap_chain_->ResizeBuffers(swap_chain_buffer_count, width, height, DXGI_FORMAT_R8G8B8A8_UNORM, swap_chain_flags_), "Resize swap chain");
    viewport_width_ = width;
    viewport_height_ = height;
    resolve_render_dimensions(render_width_, render_height_);
    alloc_render_width_ = std::max(viewport_width_, render_width_);
    alloc_render_height_ = std::max(viewport_height_, render_height_);
    buffer_fence_values_.fill(0);
    buffer_display_frame_ids_.fill(-1);
    buffer_source_frame_ids_.fill(-1);
    buffer_has_data_.fill(false);
    buffer_warped_.fill(false);
    buffer_stats_enabled_.fill(false);
    buffer_total_pixels_.fill(0);
    vrs_image_.Reset();
    range_depth_buffer_.Reset();
    capture_readback_buffer_.Reset();
    capture_readback_row_pitch_ = 0;
    create_swap_chain_targets();
    create_dlss_input_targets();
    create_source_targets();
    create_vrs_resources();
}

void Renderer::resolve_render_dimensions(UINT& out_width, UINT& out_height) const {
    if (viewport_width_ == 0 || viewport_height_ == 0) {
        out_width = 0;
        out_height = 0;
        return;
    }
    if (render_override_width_ > 0 && render_override_height_ > 0) {
        out_width = std::clamp<UINT>(render_override_width_, 160U, 7680U);
        out_height = std::clamp<UINT>(render_override_height_, 120U, 4320U);
        return;
    }
    if (render_scale_pct_ == 100) {
        out_width = viewport_width_;
        out_height = viewport_height_;
        return;
    }
    const UINT raw_w = static_cast<UINT>(std::lround(static_cast<double>(viewport_width_) * static_cast<double>(render_scale_pct_) / 100.0));
    const UINT raw_h = static_cast<UINT>(std::lround(static_cast<double>(viewport_height_) * static_cast<double>(render_scale_pct_) / 100.0));
    out_width = std::clamp<UINT>(raw_w & ~1U, 160U, 7680U);
    out_height = std::clamp<UINT>(raw_h & ~1U, 120U, 4320U);
}

void Renderer::set_render_resolution(UINT override_width, UINT override_height, std::int32_t scale_pct) {
    render_override_width_ = override_width;
    render_override_height_ = override_height;
    render_scale_pct_ = std::clamp(scale_pct, 25, 200);
    UINT new_width = 0;
    UINT new_height = 0;
    resolve_render_dimensions(new_width, new_height);
    if (new_width == 0 || new_height == 0 || (new_width == render_width_ && new_height == render_height_)) {
        return;
    }
    ++drs_scale_changes_;
    if (new_width <= alloc_render_width_ && new_height <= alloc_render_height_) {
        render_width_ = new_width;
        render_height_ = new_height;
        return;
    }
    ++drs_reallocations_;
    flush();
    for (auto& slot : source_slots_) {
        slot.color_target.Reset();
        slot.opaque_cache_target.Reset();
        slot.depth_target.Reset();
        slot.motion_target.Reset();
        slot.vis_target.Reset();
        slot.tile_map_target.Reset();
        slot.mat_gbuf0_target.Reset();
        slot.mat_gbuf1_target.Reset();
        slot.half_spec_target.Reset();
        slot.tile_stats = TileCacheStats{};
        slot.active_width = 0;
        slot.active_height = 0;
        slot.used_selective_tiles = false;
        slot.in_flight = false;
        slot.valid = false;
        slot.scene_fence_value = 0;
        slot.last_read_present_fence = 0;
        slot.scene_gpu_us = -1;
    }
    latest_completed_slot_ = -1;
    previous_completed_slot_ = -1;
    has_previous_presented_camera_ = false;
    vrs_image_.Reset();
    range_depth_buffer_.Reset();
    render_width_ = new_width;
    render_height_ = new_height;
    alloc_render_width_ = std::max(alloc_render_width_, new_width);
    alloc_render_height_ = std::max(alloc_render_height_, new_height);
    create_source_targets();
    create_vrs_resources();
}

void Renderer::flush() {
    if (scene_queue_ && scene_fence_) {
        const auto scene_value = next_scene_fence_value_++;
        check(scene_queue_->Signal(scene_fence_.Get(), scene_value), "Signal scene flush fence");
        wait_for_fence(scene_fence_.Get(), scene_fence_event_.get(), scene_value);
    }
    if (present_queue_ && present_fence_) {
        const auto present_value = next_present_fence_value_++;
        check(present_queue_->Signal(present_fence_.Get(), present_value), "Signal present flush fence");
        wait_for_fence(present_fence_.Get(), present_fence_event_.get(), present_value);
    }
    for (auto& dlss_slot : dlss_input_slots_) {
        if (dlss_slot.dlssg_inputs_fence && dlss_slot.dlssg_inputs_fence_value > 0) {
            wait_for_fence(dlss_slot.dlssg_inputs_fence, present_fence_event_.get(), dlss_slot.dlssg_inputs_fence_value);
            dlss_slot.dlssg_inputs_fence = nullptr;
            dlss_slot.dlssg_inputs_fence_value = 0;
        }
    }
}

void Renderer::create_pipelines() {
    const auto shaders = shader_directory();
    const auto scene_vs = load_shader_bytecode(shaders / L"scene_vs.cso");
    const auto scene_ps = load_shader_bytecode(shaders / L"scene_ps.cso");
    const auto present_vs = load_shader_bytecode(shaders / L"present_vs.cso");
    const std::array<std::vector<std::uint8_t>, 5> present_ps_variants = {
        load_shader_bytecode(shaders / L"present_simple_ps.cso"),
        load_shader_bytecode(shaders / L"present_rot_warp_ps.cso"),
        load_shader_bytecode(shaders / L"present_depth_fast_ps.cso"),
        load_shader_bytecode(shaders / L"present_depth_full_ps.cso"),
        load_shader_bytecode(shaders / L"present_synthesis_ps.cso"),
    };
    const auto present_dlss_inputs_ps = load_shader_bytecode(shaders / L"present_dlss_inputs_ps.cso");
    const auto tile_vis_classify_cs = load_shader_bytecode(shaders / L"tile_vis_classify_cs.cso");
    const auto tile_admit_cs = load_shader_bytecode(shaders / L"tile_admit_cs.cso");
    const auto tile_shade_indirect_cs = load_shader_bytecode(shaders / L"tile_shade_indirect_cs.cso");
    const auto range_vs = load_shader_bytecode(shaders / L"range_vs.cso");
    const auto range_ps = load_shader_bytecode(shaders / L"range_ps.cso");
    const auto range_vis_ps = load_shader_bytecode(shaders / L"range_vis_ps.cso");
    const auto range_transparent_ps = load_shader_bytecode(shaders / L"range_transparent_ps.cso");
    const auto corridor_transparent_ps = load_shader_bytecode(shaders / L"corridor_transparent_ps.cso");
    const auto range_vis_classify_cs = load_shader_bytecode(shaders / L"range_vis_classify_cs.cso");
    const auto range_vis_admit_cs = load_shader_bytecode(shaders / L"range_vis_admit_cs.cso");
    const auto range_vis_shade_cs = load_shader_bytecode(shaders / L"range_vis_shade_cs.cso");
    const auto range_half_spec_cs = load_shader_bytecode(shaders / L"range_half_spec_cs.cso");
    const auto range_vis_composite_cs = load_shader_bytecode(shaders / L"range_vis_composite_cs.cso");

    D3D12_DESCRIPTOR_RANGE scene_pbr_srv_range{};
    scene_pbr_srv_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    scene_pbr_srv_range.NumDescriptors = pbr_srv_descriptor_count;
    scene_pbr_srv_range.BaseShaderRegister = 3;
    scene_pbr_srv_range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    std::array<D3D12_ROOT_PARAMETER, 2> scene_parameters{};
    scene_parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    scene_parameters[0].Descriptor.ShaderRegister = 0;
    scene_parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    scene_parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    scene_parameters[1].DescriptorTable.NumDescriptorRanges = 1;
    scene_parameters[1].DescriptorTable.pDescriptorRanges = &scene_pbr_srv_range;
    scene_parameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    std::array<D3D12_STATIC_SAMPLER_DESC, 2> scene_samplers{};
    scene_samplers[0].Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    scene_samplers[0].AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    scene_samplers[0].AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    scene_samplers[0].AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    scene_samplers[0].ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
    scene_samplers[0].MaxLOD = D3D12_FLOAT32_MAX;
    scene_samplers[0].ShaderRegister = 0;
    scene_samplers[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    scene_samplers[1].Filter = D3D12_FILTER_ANISOTROPIC;
    scene_samplers[1].AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    scene_samplers[1].AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    scene_samplers[1].AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    scene_samplers[1].MaxAnisotropy = 16;
    scene_samplers[1].ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
    scene_samplers[1].MaxLOD = D3D12_FLOAT32_MAX;
    scene_samplers[1].ShaderRegister = 2;
    scene_samplers[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC scene_root_description{};
    scene_root_description.NumParameters = static_cast<UINT>(scene_parameters.size());
    scene_root_description.pParameters = scene_parameters.data();
    scene_root_description.NumStaticSamplers = static_cast<UINT>(scene_samplers.size());
    scene_root_description.pStaticSamplers = scene_samplers.data();
    scene_root_description.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    ComPtr<ID3DBlob> serialized_root;
    ComPtr<ID3DBlob> errors;
    check(D3D12SerializeRootSignature(&scene_root_description, D3D_ROOT_SIGNATURE_VERSION_1, &serialized_root, &errors), "Serialize scene root signature");
    check(device_->CreateRootSignature(0, serialized_root->GetBufferPointer(), serialized_root->GetBufferSize(), IID_PPV_ARGS(&scene_root_signature_)), "Create scene root signature");

    D3D12_GRAPHICS_PIPELINE_STATE_DESC scene_pipeline_desc{};
    scene_pipeline_desc.pRootSignature = scene_root_signature_.Get();
    scene_pipeline_desc.VS = {scene_vs.data(), scene_vs.size()};
    scene_pipeline_desc.PS = {scene_ps.data(), scene_ps.size()};
    for (UINT i = 0; i < targets_per_source_slot; ++i) {
        scene_pipeline_desc.BlendState.RenderTarget[i].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    }
    scene_pipeline_desc.SampleMask = UINT_MAX;
    scene_pipeline_desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    scene_pipeline_desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    scene_pipeline_desc.RasterizerState.DepthClipEnable = TRUE;
    scene_pipeline_desc.DepthStencilState.DepthEnable = FALSE;
    scene_pipeline_desc.DepthStencilState.StencilEnable = FALSE;
    scene_pipeline_desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    scene_pipeline_desc.NumRenderTargets = targets_per_source_slot;
    scene_pipeline_desc.RTVFormats[0] = DXGI_FORMAT_R16G16B16A16_FLOAT;
    scene_pipeline_desc.RTVFormats[1] = DXGI_FORMAT_R32_FLOAT;
    scene_pipeline_desc.RTVFormats[2] = DXGI_FORMAT_R16G16_FLOAT;
    scene_pipeline_desc.SampleDesc.Count = 1;
    check(device_->CreateGraphicsPipelineState(&scene_pipeline_desc, IID_PPV_ARGS(&scene_pipeline_)), "Create scene graphics pipeline");

    D3D12_DESCRIPTOR_RANGE srv_range{};
    srv_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    srv_range.NumDescriptors = targets_per_source_slot;
    srv_range.BaseShaderRegister = 0;
    srv_range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    D3D12_DESCRIPTOR_RANGE prev_srv_range{};
    prev_srv_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    prev_srv_range.NumDescriptors = targets_per_source_slot;
    prev_srv_range.BaseShaderRegister = 3;
    prev_srv_range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    D3D12_DESCRIPTOR_RANGE tile_map_srv_range{};
    tile_map_srv_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    tile_map_srv_range.NumDescriptors = 1;
    tile_map_srv_range.BaseShaderRegister = 6;
    tile_map_srv_range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    std::array<D3D12_ROOT_PARAMETER, 6> present_parameters{};
    present_parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    present_parameters[0].Descriptor.ShaderRegister = 0;
    present_parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    present_parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    present_parameters[1].DescriptorTable.NumDescriptorRanges = 1;
    present_parameters[1].DescriptorTable.pDescriptorRanges = &srv_range;
    present_parameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    present_parameters[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    present_parameters[2].Descriptor.ShaderRegister = 1;
    present_parameters[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    present_parameters[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    present_parameters[3].Descriptor.ShaderRegister = 1;
    present_parameters[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    present_parameters[4].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    present_parameters[4].DescriptorTable.NumDescriptorRanges = 1;
    present_parameters[4].DescriptorTable.pDescriptorRanges = &prev_srv_range;
    present_parameters[4].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    present_parameters[5].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    present_parameters[5].DescriptorTable.NumDescriptorRanges = 1;
    present_parameters[5].DescriptorTable.pDescriptorRanges = &tile_map_srv_range;
    present_parameters[5].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    std::array<D3D12_STATIC_SAMPLER_DESC, 2> samplers{};
    samplers[0].Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    samplers[0].AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samplers[0].AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samplers[0].AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samplers[0].ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
    samplers[0].MaxLOD = D3D12_FLOAT32_MAX;
    samplers[0].ShaderRegister = 0;
    samplers[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    samplers[1] = samplers[0];
    samplers[1].Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
    samplers[1].ShaderRegister = 1;

    D3D12_ROOT_SIGNATURE_DESC present_root_description{};
    present_root_description.NumParameters = static_cast<UINT>(present_parameters.size());
    present_root_description.pParameters = present_parameters.data();
    present_root_description.NumStaticSamplers = static_cast<UINT>(samplers.size());
    present_root_description.pStaticSamplers = samplers.data();
    present_root_description.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    serialized_root.Reset();
    errors.Reset();
    check(D3D12SerializeRootSignature(&present_root_description, D3D_ROOT_SIGNATURE_VERSION_1, &serialized_root, &errors), "Serialize present root signature");
    check(device_->CreateRootSignature(0, serialized_root->GetBufferPointer(), serialized_root->GetBufferSize(), IID_PPV_ARGS(&present_root_signature_)), "Create present root signature");

    D3D12_GRAPHICS_PIPELINE_STATE_DESC present_pipeline_desc{};
    present_pipeline_desc.pRootSignature = present_root_signature_.Get();
    present_pipeline_desc.VS = {present_vs.data(), present_vs.size()};
    present_pipeline_desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    present_pipeline_desc.SampleMask = UINT_MAX;
    present_pipeline_desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    present_pipeline_desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    present_pipeline_desc.RasterizerState.DepthClipEnable = TRUE;
    present_pipeline_desc.DepthStencilState.DepthEnable = FALSE;
    present_pipeline_desc.DepthStencilState.StencilEnable = FALSE;
    present_pipeline_desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    present_pipeline_desc.NumRenderTargets = 1;
    present_pipeline_desc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    present_pipeline_desc.SampleDesc.Count = 1;
    for (std::size_t variant = 0; variant < present_pipelines_.size(); ++variant) {
        present_pipeline_desc.PS = {present_ps_variants[variant].data(), present_ps_variants[variant].size()};
        check(device_->CreateGraphicsPipelineState(&present_pipeline_desc, IID_PPV_ARGS(&present_pipelines_[variant])), "Create present graphics pipeline variant");
    }
    for (UINT i = 1; i < 4U; ++i) {
        present_pipeline_desc.BlendState.RenderTarget[i].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    }
    present_pipeline_desc.NumRenderTargets = 4;
    present_pipeline_desc.RTVFormats[1] = DXGI_FORMAT_R8G8B8A8_UNORM;
    present_pipeline_desc.RTVFormats[2] = DXGI_FORMAT_R32_FLOAT;
    present_pipeline_desc.RTVFormats[3] = DXGI_FORMAT_R16G16_FLOAT;
    present_pipeline_desc.PS = {present_dlss_inputs_ps.data(), present_dlss_inputs_ps.size()};
    check(device_->CreateGraphicsPipelineState(&present_pipeline_desc, IID_PPV_ARGS(&present_dlss_inputs_pipeline_)), "Create present DLSS inputs pipeline");

    D3D12_DESCRIPTOR_RANGE compute_prev_srv_range{};
    compute_prev_srv_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    compute_prev_srv_range.NumDescriptors = 3;
    compute_prev_srv_range.BaseShaderRegister = 0;
    compute_prev_srv_range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    D3D12_DESCRIPTOR_RANGE compute_out_uav_range{};
    compute_out_uav_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    compute_out_uav_range.NumDescriptors = 5;
    compute_out_uav_range.BaseShaderRegister = 0;
    compute_out_uav_range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    D3D12_DESCRIPTOR_RANGE compute_dirty_list_uav_range{};
    compute_dirty_list_uav_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    compute_dirty_list_uav_range.NumDescriptors = 2;
    compute_dirty_list_uav_range.BaseShaderRegister = 5;
    compute_dirty_list_uav_range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    D3D12_DESCRIPTOR_RANGE compute_pbr_srv_range{};
    compute_pbr_srv_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    compute_pbr_srv_range.NumDescriptors = pbr_srv_descriptor_count;
    compute_pbr_srv_range.BaseShaderRegister = 3;
    compute_pbr_srv_range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    std::array<D3D12_ROOT_PARAMETER, 6> tile_compute_parameters{};
    tile_compute_parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    tile_compute_parameters[0].Descriptor.ShaderRegister = 0;
    tile_compute_parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    tile_compute_parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    tile_compute_parameters[1].DescriptorTable.NumDescriptorRanges = 1;
    tile_compute_parameters[1].DescriptorTable.pDescriptorRanges = &compute_prev_srv_range;
    tile_compute_parameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    tile_compute_parameters[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    tile_compute_parameters[2].DescriptorTable.NumDescriptorRanges = 1;
    tile_compute_parameters[2].DescriptorTable.pDescriptorRanges = &compute_out_uav_range;
    tile_compute_parameters[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    tile_compute_parameters[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    tile_compute_parameters[3].DescriptorTable.NumDescriptorRanges = 1;
    tile_compute_parameters[3].DescriptorTable.pDescriptorRanges = &compute_dirty_list_uav_range;
    tile_compute_parameters[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    tile_compute_parameters[4].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    tile_compute_parameters[4].Descriptor.ShaderRegister = 7;
    tile_compute_parameters[4].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    tile_compute_parameters[5].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    tile_compute_parameters[5].DescriptorTable.NumDescriptorRanges = 1;
    tile_compute_parameters[5].DescriptorTable.pDescriptorRanges = &compute_pbr_srv_range;
    tile_compute_parameters[5].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    std::array<D3D12_STATIC_SAMPLER_DESC, 3> compute_samplers{};
    compute_samplers[0] = samplers[0];
    compute_samplers[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    compute_samplers[1] = samplers[1];
    compute_samplers[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    compute_samplers[2] = scene_samplers[1];
    compute_samplers[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_ROOT_SIGNATURE_DESC tile_compute_root_desc{};
    tile_compute_root_desc.NumParameters = static_cast<UINT>(tile_compute_parameters.size());
    tile_compute_root_desc.pParameters = tile_compute_parameters.data();
    tile_compute_root_desc.NumStaticSamplers = static_cast<UINT>(compute_samplers.size());
    tile_compute_root_desc.pStaticSamplers = compute_samplers.data();
    tile_compute_root_desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;

    serialized_root.Reset();
    errors.Reset();
    check(D3D12SerializeRootSignature(&tile_compute_root_desc, D3D_ROOT_SIGNATURE_VERSION_1, &serialized_root, &errors), "Serialize tile compute root signature");
    check(device_->CreateRootSignature(0, serialized_root->GetBufferPointer(), serialized_root->GetBufferSize(), IID_PPV_ARGS(&tile_compute_root_signature_)), "Create tile compute root signature");

    D3D12_COMPUTE_PIPELINE_STATE_DESC compute_desc{};
    compute_desc.pRootSignature = tile_compute_root_signature_.Get();
    compute_desc.CS = {tile_vis_classify_cs.data(), tile_vis_classify_cs.size()};
    check(device_->CreateComputePipelineState(&compute_desc, IID_PPV_ARGS(&tile_vis_classify_pipeline_)), "Create tile vis classify compute pipeline");
    compute_desc.CS = {tile_admit_cs.data(), tile_admit_cs.size()};
    check(device_->CreateComputePipelineState(&compute_desc, IID_PPV_ARGS(&tile_admit_pipeline_)), "Create tile admit compute pipeline");
    compute_desc.CS = {tile_shade_indirect_cs.data(), tile_shade_indirect_cs.size()};
    check(device_->CreateComputePipelineState(&compute_desc, IID_PPV_ARGS(&tile_shade_indirect_pipeline_)), "Create tile shade indirect compute pipeline");

    std::array<D3D12_DESCRIPTOR_RANGE, 2> range_ext_srv_ranges{};
    range_ext_srv_ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    range_ext_srv_ranges[0].NumDescriptors = 6;
    range_ext_srv_ranges[0].BaseShaderRegister = 16;
    range_ext_srv_ranges[0].OffsetInDescriptorsFromTableStart = 0;
    range_ext_srv_ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    range_ext_srv_ranges[1].NumDescriptors = 3;
    range_ext_srv_ranges[1].BaseShaderRegister = 23;
    range_ext_srv_ranges[1].OffsetInDescriptorsFromTableStart = 6;

    std::array<D3D12_ROOT_PARAMETER, 5> range_params{};
    range_params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    range_params[0].Descriptor.ShaderRegister = 0;
    range_params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    range_params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    range_params[1].Constants.ShaderRegister = 1;
    range_params[1].Constants.Num32BitValues = 4;
    range_params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    range_params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    range_params[2].Descriptor.ShaderRegister = 0;
    range_params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
    range_params[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    range_params[3].DescriptorTable.NumDescriptorRanges = 1;
    range_params[3].DescriptorTable.pDescriptorRanges = &scene_pbr_srv_range;
    range_params[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    range_params[4].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    range_params[4].DescriptorTable.NumDescriptorRanges = static_cast<UINT>(range_ext_srv_ranges.size());
    range_params[4].DescriptorTable.pDescriptorRanges = range_ext_srv_ranges.data();
    range_params[4].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC range_root_desc{};
    range_root_desc.NumParameters = static_cast<UINT>(range_params.size());
    range_root_desc.pParameters = range_params.data();
    range_root_desc.NumStaticSamplers = static_cast<UINT>(scene_samplers.size());
    range_root_desc.pStaticSamplers = scene_samplers.data();
    range_root_desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    serialized_root.Reset();
    errors.Reset();
    check(D3D12SerializeRootSignature(&range_root_desc, D3D_ROOT_SIGNATURE_VERSION_1, &serialized_root, &errors), "Serialize range root signature");
    check(device_->CreateRootSignature(0, serialized_root->GetBufferPointer(), serialized_root->GetBufferSize(), IID_PPV_ARGS(&range_root_signature_)), "Create range root signature");

    const std::array<D3D12_INPUT_ELEMENT_DESC, 6> range_layout = {{
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"TANGENT", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 36, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 1, DXGI_FORMAT_R32_UINT, 0, 44, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 2, DXGI_FORMAT_R32_FLOAT, 0, 48, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
    }};

    D3D12_GRAPHICS_PIPELINE_STATE_DESC range_pso_desc{};
    range_pso_desc.pRootSignature = range_root_signature_.Get();
    range_pso_desc.InputLayout = {range_layout.data(), static_cast<UINT>(range_layout.size())};
    range_pso_desc.VS = {range_vs.data(), range_vs.size()};
    range_pso_desc.PS = {range_ps.data(), range_ps.size()};
    for (UINT i = 0; i < targets_per_source_slot; ++i) {
        range_pso_desc.BlendState.RenderTarget[i].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    }
    range_pso_desc.SampleMask = UINT_MAX;
    range_pso_desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    range_pso_desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    range_pso_desc.RasterizerState.DepthClipEnable = TRUE;
    range_pso_desc.DepthStencilState.DepthEnable = TRUE;
    range_pso_desc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    range_pso_desc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
    range_pso_desc.DepthStencilState.StencilEnable = FALSE;
    range_pso_desc.DSVFormat = DXGI_FORMAT_D32_FLOAT;
    range_pso_desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    range_pso_desc.NumRenderTargets = targets_per_source_slot;
    range_pso_desc.RTVFormats[0] = DXGI_FORMAT_R16G16B16A16_FLOAT;
    range_pso_desc.RTVFormats[1] = DXGI_FORMAT_R32_FLOAT;
    range_pso_desc.RTVFormats[2] = DXGI_FORMAT_R16G16_FLOAT;
    range_pso_desc.SampleDesc.Count = 1;
    check(device_->CreateGraphicsPipelineState(&range_pso_desc, IID_PPV_ARGS(&range_pipeline_)), "Create range PBR graphics pipeline");

    D3D12_GRAPHICS_PIPELINE_STATE_DESC vis_pso_desc = range_pso_desc;
    vis_pso_desc.PS = {range_vis_ps.data(), range_vis_ps.size()};
    vis_pso_desc.RTVFormats[0] = DXGI_FORMAT_R32_UINT;
    check(device_->CreateGraphicsPipelineState(&vis_pso_desc, IID_PPV_ARGS(&range_vis_pipeline_)), "Create range VisBuffer graphics pipeline");

    D3D12_DESCRIPTOR_RANGE trans_depth_srv_range{};
    trans_depth_srv_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    trans_depth_srv_range.NumDescriptors = 1;
    trans_depth_srv_range.BaseShaderRegister = 1;
    trans_depth_srv_range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    std::array<D3D12_ROOT_PARAMETER, 5> range_trans_params{};
    for (std::size_t i = 0; i < 4U; ++i) {
        range_trans_params[i] = range_params[i];
    }
    range_trans_params[4].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    range_trans_params[4].DescriptorTable.NumDescriptorRanges = 1;
    range_trans_params[4].DescriptorTable.pDescriptorRanges = &trans_depth_srv_range;
    range_trans_params[4].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC range_trans_root_desc{};
    range_trans_root_desc.NumParameters = static_cast<UINT>(range_trans_params.size());
    range_trans_root_desc.pParameters = range_trans_params.data();
    range_trans_root_desc.NumStaticSamplers = static_cast<UINT>(scene_samplers.size());
    range_trans_root_desc.pStaticSamplers = scene_samplers.data();
    range_trans_root_desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    serialized_root.Reset();
    errors.Reset();
    check(D3D12SerializeRootSignature(&range_trans_root_desc, D3D_ROOT_SIGNATURE_VERSION_1, &serialized_root, &errors), "Serialize range transparent root signature");
    check(device_->CreateRootSignature(0, serialized_root->GetBufferPointer(), serialized_root->GetBufferSize(), IID_PPV_ARGS(&range_transparent_root_signature_)), "Create range transparent root signature");

    D3D12_GRAPHICS_PIPELINE_STATE_DESC range_trans_pso = range_pso_desc;
    range_trans_pso.pRootSignature = range_transparent_root_signature_.Get();
    range_trans_pso.PS = {range_transparent_ps.data(), range_transparent_ps.size()};
    range_trans_pso.BlendState.RenderTarget[0].BlendEnable = TRUE;
    range_trans_pso.BlendState.RenderTarget[0].SrcBlend = D3D12_BLEND_SRC_ALPHA;
    range_trans_pso.BlendState.RenderTarget[0].DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
    range_trans_pso.BlendState.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
    range_trans_pso.BlendState.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ZERO;
    range_trans_pso.BlendState.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_ONE;
    range_trans_pso.BlendState.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;
    range_trans_pso.DepthStencilState.DepthEnable = FALSE;
    range_trans_pso.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    range_trans_pso.DSVFormat = DXGI_FORMAT_UNKNOWN;
    range_trans_pso.NumRenderTargets = 1;
    range_trans_pso.RTVFormats[0] = DXGI_FORMAT_R16G16B16A16_FLOAT;
    range_trans_pso.RTVFormats[1] = DXGI_FORMAT_UNKNOWN;
    range_trans_pso.RTVFormats[2] = DXGI_FORMAT_UNKNOWN;
    check(device_->CreateGraphicsPipelineState(&range_trans_pso, IID_PPV_ARGS(&range_transparent_pipeline_)), "Create range transparent pipeline");

    std::array<D3D12_ROOT_PARAMETER, 2> corr_trans_params{};
    corr_trans_params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    corr_trans_params[0].Descriptor.ShaderRegister = 0;
    corr_trans_params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    corr_trans_params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    corr_trans_params[1].DescriptorTable.NumDescriptorRanges = 1;
    corr_trans_params[1].DescriptorTable.pDescriptorRanges = &trans_depth_srv_range;
    corr_trans_params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC corr_trans_root_desc{};
    corr_trans_root_desc.NumParameters = static_cast<UINT>(corr_trans_params.size());
    corr_trans_root_desc.pParameters = corr_trans_params.data();
    corr_trans_root_desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    serialized_root.Reset();
    errors.Reset();
    check(D3D12SerializeRootSignature(&corr_trans_root_desc, D3D_ROOT_SIGNATURE_VERSION_1, &serialized_root, &errors), "Serialize corridor transparent root signature");
    check(device_->CreateRootSignature(0, serialized_root->GetBufferPointer(), serialized_root->GetBufferSize(), IID_PPV_ARGS(&corridor_transparent_root_signature_)), "Create corridor transparent root signature");

    D3D12_GRAPHICS_PIPELINE_STATE_DESC corr_trans_pso = scene_pipeline_desc;
    corr_trans_pso.pRootSignature = corridor_transparent_root_signature_.Get();
    corr_trans_pso.PS = {corridor_transparent_ps.data(), corridor_transparent_ps.size()};
    corr_trans_pso.BlendState.RenderTarget[0].BlendEnable = TRUE;
    corr_trans_pso.BlendState.RenderTarget[0].SrcBlend = D3D12_BLEND_SRC_ALPHA;
    corr_trans_pso.BlendState.RenderTarget[0].DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
    corr_trans_pso.BlendState.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
    corr_trans_pso.BlendState.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ONE;
    corr_trans_pso.BlendState.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_ZERO;
    corr_trans_pso.BlendState.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;
    corr_trans_pso.NumRenderTargets = 1;
    corr_trans_pso.RTVFormats[0] = DXGI_FORMAT_R16G16B16A16_FLOAT;
    corr_trans_pso.RTVFormats[1] = DXGI_FORMAT_UNKNOWN;
    corr_trans_pso.RTVFormats[2] = DXGI_FORMAT_UNKNOWN;
    check(device_->CreateGraphicsPipelineState(&corr_trans_pso, IID_PPV_ARGS(&corridor_transparent_pipeline_)), "Create corridor transparent pipeline");

    D3D12_DESCRIPTOR_RANGE final_color_uav_range{};
    final_color_uav_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    final_color_uav_range.NumDescriptors = 1;
    final_color_uav_range.BaseShaderRegister = 8;
    final_color_uav_range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    D3D12_DESCRIPTOR_RANGE prev_mat_srv_range{};
    prev_mat_srv_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    prev_mat_srv_range.NumDescriptors = 2;
    prev_mat_srv_range.BaseShaderRegister = 14;
    prev_mat_srv_range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    D3D12_DESCRIPTOR_RANGE curr_mat_uav_range{};
    curr_mat_uav_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    curr_mat_uav_range.NumDescriptors = 2;
    curr_mat_uav_range.BaseShaderRegister = 10;
    curr_mat_uav_range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    D3D12_DESCRIPTOR_RANGE half_spec_srv_range{};
    half_spec_srv_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    half_spec_srv_range.NumDescriptors = 1;
    half_spec_srv_range.BaseShaderRegister = 22;
    half_spec_srv_range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    D3D12_DESCRIPTOR_RANGE half_spec_uav_range{};
    half_spec_uav_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    half_spec_uav_range.NumDescriptors = 1;
    half_spec_uav_range.BaseShaderRegister = 12;
    half_spec_uav_range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    std::array<D3D12_ROOT_PARAMETER, 16> range_vis_params{};
    for (std::size_t i = 0; i < tile_compute_parameters.size(); ++i) {
        range_vis_params[i] = tile_compute_parameters[i];
    }
    range_vis_params[6].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    range_vis_params[6].Descriptor.ShaderRegister = 11;
    range_vis_params[6].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    range_vis_params[7].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    range_vis_params[7].Descriptor.ShaderRegister = 12;
    range_vis_params[7].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    range_vis_params[8].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    range_vis_params[8].Descriptor.ShaderRegister = 13;
    range_vis_params[8].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    range_vis_params[9].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    range_vis_params[9].DescriptorTable.NumDescriptorRanges = 1;
    range_vis_params[9].DescriptorTable.pDescriptorRanges = &final_color_uav_range;
    range_vis_params[9].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    range_vis_params[10].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    range_vis_params[10].Descriptor.ShaderRegister = 9;
    range_vis_params[10].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    range_vis_params[11].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    range_vis_params[11].DescriptorTable.NumDescriptorRanges = 1;
    range_vis_params[11].DescriptorTable.pDescriptorRanges = &prev_mat_srv_range;
    range_vis_params[11].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    range_vis_params[12].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    range_vis_params[12].DescriptorTable.NumDescriptorRanges = 1;
    range_vis_params[12].DescriptorTable.pDescriptorRanges = &curr_mat_uav_range;
    range_vis_params[12].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    range_vis_params[13].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    range_vis_params[13].DescriptorTable.NumDescriptorRanges = static_cast<UINT>(range_ext_srv_ranges.size());
    range_vis_params[13].DescriptorTable.pDescriptorRanges = range_ext_srv_ranges.data();
    range_vis_params[13].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    range_vis_params[14].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    range_vis_params[14].DescriptorTable.NumDescriptorRanges = 1;
    range_vis_params[14].DescriptorTable.pDescriptorRanges = &half_spec_srv_range;
    range_vis_params[14].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    range_vis_params[15].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    range_vis_params[15].DescriptorTable.NumDescriptorRanges = 1;
    range_vis_params[15].DescriptorTable.pDescriptorRanges = &half_spec_uav_range;
    range_vis_params[15].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_ROOT_SIGNATURE_DESC range_vis_root_desc{};
    range_vis_root_desc.NumParameters = static_cast<UINT>(range_vis_params.size());
    range_vis_root_desc.pParameters = range_vis_params.data();
    range_vis_root_desc.NumStaticSamplers = static_cast<UINT>(compute_samplers.size());
    range_vis_root_desc.pStaticSamplers = compute_samplers.data();
    range_vis_root_desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;

    serialized_root.Reset();
    errors.Reset();
    check(D3D12SerializeRootSignature(&range_vis_root_desc, D3D_ROOT_SIGNATURE_VERSION_1, &serialized_root, &errors), "Serialize range VisBuffer compute root signature");
    check(device_->CreateRootSignature(0, serialized_root->GetBufferPointer(), serialized_root->GetBufferSize(), IID_PPV_ARGS(&range_vis_compute_root_signature_)), "Create range VisBuffer compute root signature");

    D3D12_COMPUTE_PIPELINE_STATE_DESC rvis_cs_desc{};
    rvis_cs_desc.pRootSignature = range_vis_compute_root_signature_.Get();
    rvis_cs_desc.CS = {range_vis_classify_cs.data(), range_vis_classify_cs.size()};
    check(device_->CreateComputePipelineState(&rvis_cs_desc, IID_PPV_ARGS(&range_vis_classify_pipeline_)), "Create range VisBuffer classify pipeline");
    rvis_cs_desc.CS = {range_vis_admit_cs.data(), range_vis_admit_cs.size()};
    check(device_->CreateComputePipelineState(&rvis_cs_desc, IID_PPV_ARGS(&range_vis_admit_pipeline_)), "Create range VisBuffer admit pipeline");
    rvis_cs_desc.CS = {range_vis_shade_cs.data(), range_vis_shade_cs.size()};
    check(device_->CreateComputePipelineState(&rvis_cs_desc, IID_PPV_ARGS(&range_vis_shade_pipeline_)), "Create range VisBuffer shade pipeline");
    rvis_cs_desc.CS = {range_half_spec_cs.data(), range_half_spec_cs.size()};
    check(device_->CreateComputePipelineState(&rvis_cs_desc, IID_PPV_ARGS(&range_half_spec_pipeline_)), "Create range half-res rough specular pipeline");
    rvis_cs_desc.CS = {range_vis_composite_cs.data(), range_vis_composite_cs.size()};
    check(device_->CreateComputePipelineState(&rvis_cs_desc, IID_PPV_ARGS(&range_vis_composite_pipeline_)), "Create range VisBuffer composite pipeline");
}

void Renderer::create_constant_buffers() {
    const std::size_t scene_total_size = static_cast<std::size_t>(source_slot_count) * scene_constant_buffer_stride;
    const std::size_t present_total_size = static_cast<std::size_t>(swap_chain_buffer_count) * present_constant_buffer_stride;
    const std::size_t total_hud_size = static_cast<std::size_t>(swap_chain_buffer_count) * hud_constant_buffer_stride;
    scene_constant_buffer_ = create_buffer(device_.Get(), D3D12_HEAP_TYPE_UPLOAD, scene_total_size, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ, "Create scene constant buffer");
    present_constant_buffer_ = create_buffer(device_.Get(), D3D12_HEAP_TYPE_UPLOAD, present_total_size, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ, "Create present constant buffer");
    hud_constant_buffer_ = create_buffer(device_.Get(), D3D12_HEAP_TYPE_UPLOAD, total_hud_size, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ, "Create HUD constant buffer");
    D3D12_RANGE empty_range{0, 0};
    check(scene_constant_buffer_->Map(0, &empty_range, reinterpret_cast<void**>(&mapped_scene_constants_)), "Map scene constant buffer");
    check(present_constant_buffer_->Map(0, &empty_range, reinterpret_cast<void**>(&mapped_present_constants_)), "Map present constant buffer");
    check(hud_constant_buffer_->Map(0, &empty_range, reinterpret_cast<void**>(&mapped_hud_constants_)), "Map HUD constant buffer");
}

void Renderer::create_timestamp_resources() {
    if (SUCCEEDED(present_queue_->GetTimestampFrequency(&present_timestamp_frequency_)) && present_timestamp_frequency_ > 0) {
        D3D12_QUERY_HEAP_DESC heap_desc{};
        heap_desc.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
        heap_desc.Count = swap_chain_buffer_count * timestamps_per_pass;
        if (SUCCEEDED(device_->CreateQueryHeap(&heap_desc, IID_PPV_ARGS(&present_timestamp_heap_)))) {
            present_timestamp_readback_ = create_buffer(
                device_.Get(),
                D3D12_HEAP_TYPE_READBACK,
                static_cast<std::size_t>(heap_desc.Count) * sizeof(std::uint64_t),
                D3D12_RESOURCE_FLAG_NONE,
                D3D12_RESOURCE_STATE_COPY_DEST,
                "Create present timestamp readback");
            if (FAILED(present_timestamp_readback_->Map(0, nullptr, reinterpret_cast<void**>(const_cast<std::uint64_t**>(&mapped_present_timestamps_))))) {
                present_timestamp_readback_.Reset();
                present_timestamp_heap_.Reset();
                present_timestamp_frequency_ = 0;
            }
        } else {
            present_timestamp_frequency_ = 0;
        }
    }

    if (SUCCEEDED(scene_queue_->GetTimestampFrequency(&scene_timestamp_frequency_)) && scene_timestamp_frequency_ > 0) {
        D3D12_QUERY_HEAP_DESC heap_desc{};
        heap_desc.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
        heap_desc.Count = source_slot_count * timestamps_per_pass;
        if (SUCCEEDED(device_->CreateQueryHeap(&heap_desc, IID_PPV_ARGS(&scene_timestamp_heap_)))) {
            scene_timestamp_readback_ = create_buffer(
                device_.Get(),
                D3D12_HEAP_TYPE_READBACK,
                static_cast<std::size_t>(heap_desc.Count) * sizeof(std::uint64_t),
                D3D12_RESOURCE_FLAG_NONE,
                D3D12_RESOURCE_STATE_COPY_DEST,
                "Create scene timestamp readback");
            if (FAILED(scene_timestamp_readback_->Map(0, nullptr, reinterpret_cast<void**>(const_cast<std::uint64_t**>(&mapped_scene_timestamps_))))) {
                scene_timestamp_readback_.Reset();
                scene_timestamp_heap_.Reset();
                scene_timestamp_frequency_ = 0;
            }
        } else {
            scene_timestamp_frequency_ = 0;
        }
    }
}

void Renderer::create_reprojection_stats_resources() {
    reprojection_stats_uav_ = create_buffer(
        device_.Get(),
        D3D12_HEAP_TYPE_DEFAULT,
        16,
        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
        "Create reprojection stats UAV");
    reprojection_stats_zero_upload_ = create_buffer(
        device_.Get(),
        D3D12_HEAP_TYPE_UPLOAD,
        16,
        D3D12_RESOURCE_FLAG_NONE,
        D3D12_RESOURCE_STATE_GENERIC_READ,
        "Create reprojection stats zero buffer");
    void* zero_pointer = nullptr;
    D3D12_RANGE empty_range{0, 0};
    check(reprojection_stats_zero_upload_->Map(0, &empty_range, &zero_pointer), "Map reprojection stats zero buffer");
    std::memset(zero_pointer, 0, 16);
    reprojection_stats_zero_upload_->Unmap(0, nullptr);

    reprojection_stats_readback_ = create_buffer(
        device_.Get(),
        D3D12_HEAP_TYPE_READBACK,
        static_cast<std::size_t>(swap_chain_buffer_count) * 2U * sizeof(std::uint32_t),
        D3D12_RESOURCE_FLAG_NONE,
        D3D12_RESOURCE_STATE_COPY_DEST,
        "Create reprojection stats readback buffer");
    check(reprojection_stats_readback_->Map(0, nullptr, reinterpret_cast<void**>(const_cast<std::uint32_t**>(&mapped_reprojection_stats_))), "Map reprojection stats readback buffer");
}

void Renderer::create_selective_tile_resources() {
    dirty_tile_list_uav_ = create_buffer(
        device_.Get(),
        D3D12_HEAP_TYPE_DEFAULT,
        static_cast<std::size_t>(max_dirty_tile_capacity) * sizeof(std::uint32_t),
        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
        "Create dirty tile list UAV");
    tile_candidate_meta_uav_ = create_buffer(
        device_.Get(),
        D3D12_HEAP_TYPE_DEFAULT,
        static_cast<std::size_t>(max_dirty_tile_capacity) * sizeof(std::uint32_t),
        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
        "Create tile candidate metadata UAV");
    tile_indirect_args_uav_ = create_buffer(
        device_.Get(),
        D3D12_HEAP_TYPE_DEFAULT,
        static_cast<std::size_t>(tile_indirect_u32_count) * sizeof(std::uint32_t),
        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
        "Create tile indirect args UAV");
    surface_cache_uav_ = create_buffer(
        device_.Get(),
        D3D12_HEAP_TYPE_DEFAULT,
        static_cast<std::size_t>(surface_cache_max_cells) * 16U,
        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
        "Create persistent surface-space cache UAV");
    tile_indirect_args_init_upload_ = create_buffer(
        device_.Get(),
        D3D12_HEAP_TYPE_UPLOAD,
        static_cast<std::size_t>(tile_indirect_u32_count) * sizeof(std::uint32_t),
        D3D12_RESOURCE_FLAG_NONE,
        D3D12_RESOURCE_STATE_GENERIC_READ,
        "Create tile indirect args init buffer");
    std::uint32_t* init_ptr = nullptr;
    D3D12_RANGE empty_range{0, 0};
    check(tile_indirect_args_init_upload_->Map(0, &empty_range, reinterpret_cast<void**>(&init_ptr)), "Map tile indirect args init buffer");
    std::memset(init_ptr, 0, static_cast<std::size_t>(tile_indirect_u32_count) * sizeof(std::uint32_t));
    init_ptr[1] = 1U;
    init_ptr[2] = 1U;
    tile_indirect_args_init_upload_->Unmap(0, nullptr);

    tile_stats_readback_ = create_buffer(
        device_.Get(),
        D3D12_HEAP_TYPE_READBACK,
        static_cast<std::size_t>(source_slot_count) * tile_stats_readback_u32_count * sizeof(std::uint32_t),
        D3D12_RESOURCE_FLAG_NONE,
        D3D12_RESOURCE_STATE_COPY_DEST,
        "Create tile stats readback buffer");
    check(tile_stats_readback_->Map(0, nullptr, reinterpret_cast<void**>(const_cast<std::uint32_t**>(&mapped_tile_stats_))), "Map tile stats readback buffer");

    auto shared_uav_cpu = scene_compute_heap_->GetCPUDescriptorHandleForHeapStart();
    shared_uav_cpu.ptr += static_cast<SIZE_T>(source_slot_count * compute_descriptors_per_source_slot) * shader_resource_descriptor_stride_;
    D3D12_UNORDERED_ACCESS_VIEW_DESC buf_uav_desc{};
    buf_uav_desc.Format = DXGI_FORMAT_UNKNOWN;
    buf_uav_desc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    buf_uav_desc.Buffer.FirstElement = 0;
    buf_uav_desc.Buffer.NumElements = max_dirty_tile_capacity;
    buf_uav_desc.Buffer.StructureByteStride = sizeof(std::uint32_t);
    device_->CreateUnorderedAccessView(dirty_tile_list_uav_.Get(), nullptr, &buf_uav_desc, shared_uav_cpu);
    shared_uav_cpu.ptr += shader_resource_descriptor_stride_;
    device_->CreateUnorderedAccessView(tile_candidate_meta_uav_.Get(), nullptr, &buf_uav_desc, shared_uav_cpu);

    D3D12_INDIRECT_ARGUMENT_DESC indirect_arg{};
    indirect_arg.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH;
    D3D12_COMMAND_SIGNATURE_DESC cmd_sig_desc{};
    cmd_sig_desc.ByteStride = sizeof(D3D12_DISPATCH_ARGUMENTS);
    cmd_sig_desc.NumArgumentDescs = 1;
    cmd_sig_desc.pArgumentDescs = &indirect_arg;
    check(device_->CreateCommandSignature(&cmd_sig_desc, nullptr, IID_PPV_ARGS(&dispatch_command_signature_)), "Create dispatch command signature");
}

void Renderer::create_pbr_assets() {
    auto pbr_srv_cpu = scene_compute_heap_->GetCPUDescriptorHandleForHeapStart();
    pbr_srv_cpu.ptr += static_cast<SIZE_T>(source_slot_count * compute_descriptors_per_source_slot + 2U) * shader_resource_descriptor_stride_;
    pbr_assets_ = create_and_upload_pbr_assets(
        device_.Get(),
        scene_queue_.Get(),
        source_slots_[0].command_allocator.Get(),
        scene_command_list_.Get(),
        scene_fence_.Get(),
        scene_fence_event_.get(),
        next_scene_fence_value_,
        pbr_srv_cpu,
        shader_resource_descriptor_stride_);
}

void Renderer::create_range_resources() {
    RangeGeometryPackage pkg = build_tactical_range_geometry();
    range_submeshes_ = pkg.submeshes;
    range_geometry_meta_.hero_pistol_gltf_loaded = pkg.hero_pistol_gltf_loaded;
    range_geometry_meta_.pistol_slide_offset = pkg.pistol_slide_offset;
    range_geometry_meta_.pistol_hammer_offset = pkg.pistol_hammer_offset;
    range_geometry_meta_.pistol_trigger_offset = pkg.pistol_trigger_offset;
    range_geometry_meta_.pistol_magazine_offset = pkg.pistol_magazine_offset;
    range_geometry_meta_.pistol_muzzle_offset = pkg.pistol_muzzle_offset;
    range_geometry_meta_.pistol_eject_offset = pkg.pistol_eject_offset;
    range_geometry_meta_.pistol_ads_sight_y = pkg.pistol_ads_sight_y;
    range_geometry_meta_.weapon_muzzle_offsets = pkg.weapon_muzzle_offsets;
    range_geometry_meta_.weapon_eject_offsets = pkg.weapon_eject_offsets;
    range_geometry_meta_.weapon_ads_sight_y = pkg.weapon_ads_sight_y;
    range_geometry_meta_.weapon_loaded = pkg.weapon_loaded;
    range_geometry_meta_.ct_bot_template = std::move(pkg.ct_bot_template);
    range_geometry_meta_.t_bot_template = std::move(pkg.t_bot_template);
    range_geometry_meta_.bots_vb_base_vertex = pkg.bots_vb_base_vertex;
    range_geometry_meta_.bots_total_vertices = pkg.bots_total_vertices;
    range_geometry_meta_.bot_vb_offsets = pkg.bot_vb_offsets;
    range_geometry_meta_.bot_vb_counts = pkg.bot_vb_counts;
    range_bots_vb_base_vertex_ = pkg.bots_vb_base_vertex;
    bots_total_vertices_ = pkg.bots_total_vertices;

    const std::size_t vb_bytes = pkg.vertices.size() * sizeof(RangeVertex);
    const std::size_t ib_bytes = pkg.indices.size() * sizeof(std::uint32_t);
    range_vertex_buffer_ = create_buffer(device_.Get(), D3D12_HEAP_TYPE_DEFAULT, vb_bytes, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST, "Create range vertex buffer");
    range_index_buffer_ = create_buffer(device_.Get(), D3D12_HEAP_TYPE_DEFAULT, ib_bytes, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST, "Create range index buffer");

    ComPtr<ID3D12Resource> vb_upload = create_buffer(device_.Get(), D3D12_HEAP_TYPE_UPLOAD, vb_bytes, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ, "Create range VB upload");
    ComPtr<ID3D12Resource> ib_upload = create_buffer(device_.Get(), D3D12_HEAP_TYPE_UPLOAD, ib_bytes, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ, "Create range IB upload");
    void* mapped = nullptr;
    D3D12_RANGE empty_range{0, 0};
    check(vb_upload->Map(0, &empty_range, &mapped), "Map range VB upload");
    std::memcpy(mapped, pkg.vertices.data(), vb_bytes);
    vb_upload->Unmap(0, nullptr);
    check(ib_upload->Map(0, &empty_range, &mapped), "Map range IB upload");
    std::memcpy(mapped, pkg.indices.data(), ib_bytes);
    ib_upload->Unmap(0, nullptr);

    check(source_slots_[0].command_allocator->Reset(), "Reset range mesh upload allocator");
    check(scene_command_list_->Reset(source_slots_[0].command_allocator.Get(), nullptr), "Reset range mesh upload command list");
    scene_command_list_->CopyBufferRegion(range_vertex_buffer_.Get(), 0, vb_upload.Get(), 0, vb_bytes);
    scene_command_list_->CopyBufferRegion(range_index_buffer_.Get(), 0, ib_upload.Get(), 0, ib_bytes);
    const std::array<D3D12_RESOURCE_BARRIER, 2> barriers = {
        transition_barrier(range_vertex_buffer_.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
        transition_barrier(range_index_buffer_.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_INDEX_BUFFER | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
    };
    scene_command_list_->ResourceBarrier(static_cast<UINT>(barriers.size()), barriers.data());
    check(scene_command_list_->Close(), "Close range mesh upload command list");
    ID3D12CommandList* lists[] = {scene_command_list_.Get()};
    scene_queue_->ExecuteCommandLists(1, lists);
    const auto fence_value = next_scene_fence_value_++;
    check(scene_queue_->Signal(scene_fence_.Get(), fence_value), "Signal range mesh upload fence");
    wait_for_fence(scene_fence_.Get(), scene_fence_event_.get(), fence_value);

    range_vb_view_.BufferLocation = range_vertex_buffer_->GetGPUVirtualAddress();
    range_vb_view_.SizeInBytes = static_cast<UINT>(vb_bytes);
    range_vb_view_.StrideInBytes = static_cast<UINT>(sizeof(RangeVertex));
    range_ib_view_.BufferLocation = range_index_buffer_->GetGPUVirtualAddress();
    range_ib_view_.SizeInBytes = static_cast<UINT>(ib_bytes);
    range_ib_view_.Format = DXGI_FORMAT_R32_UINT;

    const std::size_t inst_bytes = static_cast<std::size_t>(source_slot_count) * max_range_instances * sizeof(RangeInstanceGpu);
    range_instance_buffer_ = create_buffer(device_.Get(), D3D12_HEAP_TYPE_UPLOAD, inst_bytes, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ, "Create range instance buffer");
    check(range_instance_buffer_->Map(0, &empty_range, reinterpret_cast<void**>(&mapped_range_instances_)), "Map range instance buffer");
    if (bots_total_vertices_ > 0U) {
        const std::size_t skin_bytes = static_cast<std::size_t>(source_slot_count) * static_cast<std::size_t>(bots_total_vertices_) * sizeof(RangeVertex);
        skinned_bot_upload_buffer_ = create_buffer(device_.Get(), D3D12_HEAP_TYPE_UPLOAD, skin_bytes, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ, "Create skinned bot upload buffer");
        check(skinned_bot_upload_buffer_->Map(0, &empty_range, reinterpret_cast<void**>(&mapped_skinned_bots_)), "Map skinned bot upload buffer");
    }

    auto upload_default_buffer = [&](const void* src_data, std::size_t bytes, D3D12_RESOURCE_STATES final_state, const char* label) -> ComPtr<ID3D12Resource> {
        const std::size_t safe_bytes = std::max<std::size_t>(bytes, 64U);
        ComPtr<ID3D12Resource> dst = create_buffer(device_.Get(), D3D12_HEAP_TYPE_DEFAULT, safe_bytes, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST, label);
        ComPtr<ID3D12Resource> up = create_buffer(device_.Get(), D3D12_HEAP_TYPE_UPLOAD, safe_bytes, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ, label);
        void* up_ptr = nullptr;
        D3D12_RANGE r{0, 0};
        check(up->Map(0, &r, &up_ptr), label);
        if (bytes > 0U && src_data != nullptr) {
            std::memcpy(up_ptr, src_data, bytes);
        } else {
            std::memset(up_ptr, 0, safe_bytes);
        }
        up->Unmap(0, nullptr);
        check(source_slots_[0].command_allocator->Reset(), "Reset upload allocator");
        check(scene_command_list_->Reset(source_slots_[0].command_allocator.Get(), nullptr), "Reset upload command list");
        scene_command_list_->CopyBufferRegion(dst.Get(), 0, up.Get(), 0, safe_bytes);
        const auto bar = transition_barrier(dst.Get(), D3D12_RESOURCE_STATE_COPY_DEST, final_state);
        scene_command_list_->ResourceBarrier(1, &bar);
        check(scene_command_list_->Close(), "Close upload command list");
        ID3D12CommandList* ulists[] = {scene_command_list_.Get()};
        scene_queue_->ExecuteCommandLists(1, ulists);
        const auto fv = next_scene_fence_value_++;
        check(scene_queue_->Signal(scene_fence_.Get(), fv), "Signal upload fence");
        wait_for_fence(scene_fence_.Get(), scene_fence_event_.get(), fv);
        return dst;
    };

    auto build_static_blas_tlas = [&](
        ID3D12Resource* vb,
        UINT vertex_count,
        ID3D12Resource* ib,
        UINT index_offset,
        UINT index_count,
        ComPtr<ID3D12Resource>& out_blas,
        ComPtr<ID3D12Resource>& out_tlas) {
        if (dxr_tier_ < static_cast<std::int32_t>(D3D12_RAYTRACING_TIER_1_1) || !device5_ || !scene_command_list4_ || vertex_count == 0U || index_count == 0U) {
            return;
        }
        D3D12_RAYTRACING_GEOMETRY_DESC geom{};
        geom.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
        geom.Flags = D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;
        geom.Triangles.Transform3x4 = 0;
        geom.Triangles.IndexFormat = DXGI_FORMAT_R32_UINT;
        geom.Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
        geom.Triangles.IndexCount = index_count;
        geom.Triangles.VertexCount = vertex_count;
        geom.Triangles.IndexBuffer = ib->GetGPUVirtualAddress() + static_cast<UINT64>(index_offset) * sizeof(std::uint32_t);
        geom.Triangles.VertexBuffer.StartAddress = vb->GetGPUVirtualAddress();
        geom.Triangles.VertexBuffer.StrideInBytes = sizeof(RangeVertex);

        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS blas_in{};
        blas_in.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
        blas_in.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
        blas_in.NumDescs = 1;
        blas_in.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
        blas_in.pGeometryDescs = &geom;

        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO blas_info{};
        device5_->GetRaytracingAccelerationStructurePrebuildInfo(&blas_in, &blas_info);
        out_blas = create_buffer(device_.Get(), D3D12_HEAP_TYPE_DEFAULT, std::max<UINT64>(blas_info.ResultDataMaxSizeInBytes, 256ULL), D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE, "Create DXR BLAS");

        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS tlas_in{};
        tlas_in.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
        tlas_in.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
        tlas_in.NumDescs = 1;
        tlas_in.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;

        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO tlas_info{};
        device5_->GetRaytracingAccelerationStructurePrebuildInfo(&tlas_in, &tlas_info);
        out_tlas = create_buffer(device_.Get(), D3D12_HEAP_TYPE_DEFAULT, std::max<UINT64>(tlas_info.ResultDataMaxSizeInBytes, 256ULL), D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE, "Create DXR TLAS");

        const UINT64 scratch_bytes = std::max<UINT64>(std::max(blas_info.ScratchDataSizeInBytes, tlas_info.ScratchDataSizeInBytes), 256ULL);
        ComPtr<ID3D12Resource> scratch = create_buffer(device_.Get(), D3D12_HEAP_TYPE_DEFAULT, scratch_bytes, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, "Create DXR scratch");

        D3D12_RAYTRACING_INSTANCE_DESC inst_desc{};
        inst_desc.Transform[0][0] = 1.0f;
        inst_desc.Transform[1][1] = 1.0f;
        inst_desc.Transform[2][2] = 1.0f;
        inst_desc.InstanceID = 0;
        inst_desc.InstanceMask = 0xFF;
        inst_desc.InstanceContributionToHitGroupIndex = 0;
        inst_desc.Flags = D3D12_RAYTRACING_INSTANCE_FLAG_NONE;
        inst_desc.AccelerationStructure = out_blas->GetGPUVirtualAddress();
        ComPtr<ID3D12Resource> inst_upload = create_buffer(device_.Get(), D3D12_HEAP_TYPE_UPLOAD, sizeof(D3D12_RAYTRACING_INSTANCE_DESC), D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ, "Create DXR instance upload");
        void* ip = nullptr;
        D3D12_RANGE ir{0, 0};
        check(inst_upload->Map(0, &ir, &ip), "Map DXR instance upload");
        std::memcpy(ip, &inst_desc, sizeof(inst_desc));
        inst_upload->Unmap(0, nullptr);
        tlas_in.InstanceDescs = inst_upload->GetGPUVirtualAddress();

        check(source_slots_[0].command_allocator->Reset(), "Reset DXR build allocator");
        check(scene_command_list_->Reset(source_slots_[0].command_allocator.Get(), nullptr), "Reset DXR build command list");

        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC blas_build{};
        blas_build.DestAccelerationStructureData = out_blas->GetGPUVirtualAddress();
        blas_build.Inputs = blas_in;
        blas_build.ScratchAccelerationStructureData = scratch->GetGPUVirtualAddress();
        scene_command_list4_->BuildRaytracingAccelerationStructure(&blas_build, 0, nullptr);

        D3D12_RESOURCE_BARRIER blas_uav = uav_barrier(out_blas.Get());
        scene_command_list_->ResourceBarrier(1, &blas_uav);

        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC tlas_build{};
        tlas_build.DestAccelerationStructureData = out_tlas->GetGPUVirtualAddress();
        tlas_build.Inputs = tlas_in;
        tlas_build.ScratchAccelerationStructureData = scratch->GetGPUVirtualAddress();
        scene_command_list4_->BuildRaytracingAccelerationStructure(&tlas_build, 0, nullptr);

        D3D12_RESOURCE_BARRIER tlas_uav = uav_barrier(out_tlas.Get());
        scene_command_list_->ResourceBarrier(1, &tlas_uav);

        check(scene_command_list_->Close(), "Close DXR build command list");
        ID3D12CommandList* dlists[] = {scene_command_list_.Get()};
        scene_queue_->ExecuteCommandLists(1, dlists);
        const auto fv = next_scene_fence_value_++;
        check(scene_queue_->Signal(scene_fence_.Get(), fv), "Signal DXR build fence");
        wait_for_fence(scene_fence_.Get(), scene_fence_event_.get(), fv);
    };

    const auto range_lights = build_range_cluster_lights();
    const auto range_probes = build_range_visibility_probes();
    range_light_count_ = static_cast<std::uint32_t>(range_lights.size());
    range_probe_count_ = static_cast<std::uint32_t>(range_probes.size());
    range_lights_buf_ = upload_default_buffer(range_lights.data(), range_lights.size() * sizeof(GpuClusterLight), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, "Upload range cluster lights");
    range_probes_buf_ = upload_default_buffer(range_probes.data(), range_probes.size() * sizeof(OctreeVisProbeGpu), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, "Upload range visibility probes");
    const auto& static_range_sm = range_submeshes_[static_cast<std::size_t>(RangeMeshKind::static_world)];
    build_static_blas_tlas(
        range_vertex_buffer_.Get(),
        static_cast<UINT>(pkg.vertices.size()),
        range_index_buffer_.Get(),
        static_range_sm.index_offset,
        static_range_sm.index_count,
        range_blas_,
        range_tlas_);

    dust2_map_ = load_and_build_dust2_map();
    const UINT ext_range_slot = source_slot_count * compute_descriptors_per_source_slot + 2U + pbr_srv_descriptor_count;
    const UINT ext_dust2_slot = ext_range_slot + ext_srv_descriptor_count;
    auto heap_cpu_start = scene_compute_heap_->GetCPUDescriptorHandleForHeapStart();
    D3D12_CPU_DESCRIPTOR_HANDLE range_ext_cpu{heap_cpu_start.ptr + static_cast<SIZE_T>(ext_range_slot) * shader_resource_descriptor_stride_};
    D3D12_CPU_DESCRIPTOR_HANDLE dust2_ext_cpu{heap_cpu_start.ptr + static_cast<SIZE_T>(ext_dust2_slot) * shader_resource_descriptor_stride_};

    dust2_pbr_ = create_and_upload_dust2_pbr_assets(
        device_.Get(),
        scene_queue_.Get(),
        source_slots_[0].command_allocator.Get(),
        scene_command_list_.Get(),
        scene_fence_.Get(),
        scene_fence_event_.get(),
        next_scene_fence_value_,
        dust2_map_.texture_paths,
        dust2_ext_cpu,
        shader_resource_descriptor_stride_);

    {
        auto write_d2_array = [&](ID3D12Resource* res, DXGI_FORMAT fmt, D3D12_CPU_DESCRIPTOR_HANDLE dst) {
            D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
            srv.Format = fmt;
            srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
            srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            srv.Texture2DArray.MostDetailedMip = 0;
            srv.Texture2DArray.MipLevels = 9;
            srv.Texture2DArray.FirstArraySlice = 0;
            srv.Texture2DArray.ArraySize = std::max<UINT>(1U, dust2_pbr_.material_count);
            device_->CreateShaderResourceView(res, &srv, dst);
        };
        D3D12_CPU_DESCRIPTOR_HANDLE rc = range_ext_cpu;
        write_d2_array(dust2_pbr_.albedo_array.Get(), DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, rc);
        rc.ptr += shader_resource_descriptor_stride_;
        write_d2_array(dust2_pbr_.normal_array.Get(), DXGI_FORMAT_R8G8B8A8_UNORM, rc);
        rc.ptr += shader_resource_descriptor_stride_;
        write_d2_array(dust2_pbr_.orm_array.Get(), DXGI_FORMAT_R8G8B8A8_UNORM, rc);
    }

    if (dust2_map_.loaded) {
        std::vector<RangeVertex> combined_d2_verts = dust2_map_.vertices;
        std::vector<std::uint32_t> combined_d2_indices = dust2_map_.indices;
        const auto pistol_v_base = static_cast<std::uint32_t>(combined_d2_verts.size());
        dust2_bots_vb_base_vertex_ = pistol_v_base + pkg.bots_vb_base_vertex;
        combined_d2_verts.insert(combined_d2_verts.end(), pkg.vertices.begin(), pkg.vertices.end());
        for (std::size_t mi = 0; mi < static_cast<std::size_t>(RangeMeshKind::count); ++mi) {
            const auto& rsm = range_submeshes_[mi];
            dust2_viewmodel_submeshes_[mi].index_offset = static_cast<std::uint32_t>(combined_d2_indices.size());
            dust2_viewmodel_submeshes_[mi].index_count = rsm.index_count;
            for (std::uint32_t ii = 0; ii < rsm.index_count; ++ii) {
                combined_d2_indices.push_back(pistol_v_base + pkg.indices[rsm.index_offset + ii]);
            }
        }
        const std::size_t d2_vb_bytes = combined_d2_verts.size() * sizeof(RangeVertex);
        const std::size_t d2_ib_bytes = combined_d2_indices.size() * sizeof(std::uint32_t);
        dust2_vertex_buffer_ = upload_default_buffer(combined_d2_verts.data(), d2_vb_bytes, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, "Upload Dust2 vertex buffer");
        dust2_index_buffer_ = upload_default_buffer(combined_d2_indices.data(), d2_ib_bytes, D3D12_RESOURCE_STATE_INDEX_BUFFER | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, "Upload Dust2 index buffer");
        dust2_vb_view_.BufferLocation = dust2_vertex_buffer_->GetGPUVirtualAddress();
        dust2_vb_view_.SizeInBytes = static_cast<UINT>(d2_vb_bytes);
        dust2_vb_view_.StrideInBytes = static_cast<UINT>(sizeof(RangeVertex));
        dust2_ib_view_.BufferLocation = dust2_index_buffer_->GetGPUVirtualAddress();
        dust2_ib_view_.SizeInBytes = static_cast<UINT>(d2_ib_bytes);
        dust2_ib_view_.Format = DXGI_FORMAT_R32_UINT;

        dust2_lights_buf_ = upload_default_buffer(dust2_map_.lights.data(), dust2_map_.lights.size() * sizeof(GpuClusterLight), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, "Upload Dust2 cluster lights");
        dust2_probes_buf_ = upload_default_buffer(dust2_map_.probes.data(), dust2_map_.probes.size() * sizeof(OctreeVisProbeGpu), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, "Upload Dust2 octree probes");
        build_static_blas_tlas(
            dust2_vertex_buffer_.Get(),
            static_cast<UINT>(dust2_map_.vertices.size()),
            dust2_index_buffer_.Get(),
            0U,
            dust2_map_.lod0_total_index_count,
            dust2_blas_,
            dust2_tlas_);
    }

    auto write_ext_tail = [&](D3D12_CPU_DESCRIPTOR_HANDLE base_cpu, ID3D12Resource* lights_res, UINT light_cnt, ID3D12Resource* probes_res, UINT probe_cnt, ID3D12Resource* tlas_res) {
        D3D12_CPU_DESCRIPTOR_HANDLE c{base_cpu.ptr + 3ULL * shader_resource_descriptor_stride_};
        D3D12_SHADER_RESOURCE_VIEW_DESC lsrv{};
        lsrv.Format = DXGI_FORMAT_UNKNOWN;
        lsrv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        lsrv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        lsrv.Buffer.FirstElement = 0;
        lsrv.Buffer.NumElements = std::max<UINT>(light_cnt, 1U);
        lsrv.Buffer.StructureByteStride = sizeof(GpuClusterLight);
        device_->CreateShaderResourceView(lights_res, &lsrv, c);
        c.ptr += shader_resource_descriptor_stride_;

        D3D12_SHADER_RESOURCE_VIEW_DESC psrv{};
        psrv.Format = DXGI_FORMAT_UNKNOWN;
        psrv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        psrv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        psrv.Buffer.FirstElement = 0;
        psrv.Buffer.NumElements = std::max<UINT>(probe_cnt, 1U);
        psrv.Buffer.StructureByteStride = sizeof(OctreeVisProbeGpu);
        device_->CreateShaderResourceView(probes_res, &psrv, c);
        c.ptr += shader_resource_descriptor_stride_;

        if (tlas_res != nullptr && dxr_tier_ >= static_cast<std::int32_t>(D3D12_RAYTRACING_TIER_1_1)) {
            D3D12_SHADER_RESOURCE_VIEW_DESC tsrv{};
            tsrv.Format = DXGI_FORMAT_UNKNOWN;
            tsrv.ViewDimension = D3D12_SRV_DIMENSION_RAYTRACING_ACCELERATION_STRUCTURE;
            tsrv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            tsrv.RaytracingAccelerationStructure.Location = tlas_res->GetGPUVirtualAddress();
            device_->CreateShaderResourceView(nullptr, &tsrv, c);
        } else {
            device_->CreateShaderResourceView(probes_res, &psrv, c);
        }
    };

    write_ext_tail(range_ext_cpu, range_lights_buf_.Get(), range_light_count_, range_probes_buf_.Get(), range_probe_count_, range_tlas_.Get());
    write_ext_tail(
        dust2_ext_cpu,
        dust2_lights_buf_ ? dust2_lights_buf_.Get() : range_lights_buf_.Get(),
        dust2_lights_buf_ ? static_cast<UINT>(dust2_map_.lights.size()) : range_light_count_,
        dust2_probes_buf_ ? dust2_probes_buf_.Get() : range_probes_buf_.Get(),
        dust2_probes_buf_ ? static_cast<UINT>(dust2_map_.probes.size()) : range_probe_count_,
        dust2_tlas_ ? dust2_tlas_.Get() : range_tlas_.Get());

    D3D12_CPU_DESCRIPTOR_HANDLE range_ars_cpu{range_ext_cpu.ptr + 6ULL * shader_resource_descriptor_stride_};
    D3D12_CPU_DESCRIPTOR_HANDLE dust2_ars_cpu{dust2_ext_cpu.ptr + 6ULL * shader_resource_descriptor_stride_};
    arsenal_pbr_ = create_and_upload_arsenal_pbr_assets(
        device_.Get(),
        scene_queue_.Get(),
        source_slots_[0].command_allocator.Get(),
        scene_command_list_.Get(),
        scene_fence_.Get(),
        scene_fence_event_.get(),
        next_scene_fence_value_,
        pkg.arsenal_pbr_materials,
        range_ars_cpu,
        shader_resource_descriptor_stride_);
    {
        auto write_ars_array = [&](ID3D12Resource* res, DXGI_FORMAT fmt, D3D12_CPU_DESCRIPTOR_HANDLE dst) {
            D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
            srv.Format = fmt;
            srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
            srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            srv.Texture2DArray.MostDetailedMip = 0;
            srv.Texture2DArray.MipLevels = 11;
            srv.Texture2DArray.FirstArraySlice = 0;
            srv.Texture2DArray.ArraySize = std::max<UINT>(1U, arsenal_pbr_.material_count);
            device_->CreateShaderResourceView(res, &srv, dst);
        };
        D3D12_CPU_DESCRIPTOR_HANDLE dc = dust2_ars_cpu;
        write_ars_array(arsenal_pbr_.albedo_array.Get(), DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, dc);
        dc.ptr += shader_resource_descriptor_stride_;
        write_ars_array(arsenal_pbr_.normal_array.Get(), DXGI_FORMAT_R8G8B8A8_UNORM, dc);
        dc.ptr += shader_resource_descriptor_stride_;
        write_ars_array(arsenal_pbr_.orm_array.Get(), DXGI_FORMAT_R8G8B8A8_UNORM, dc);
    }
    range_geometry_meta_.arsenal_pbr_materials.clear();
    range_geometry_meta_.arsenal_pbr_materials.shrink_to_fit();
}

void Renderer::create_vrs_resources() {
    vrs_image_.Reset();
    if (vrs_tier_ < static_cast<std::int32_t>(D3D12_VARIABLE_SHADING_RATE_TIER_2) || vrs_tile_size_px_ <= 0 || alloc_render_width_ == 0 || alloc_render_height_ == 0) {
        return;
    }
    const UINT tile_size = static_cast<UINT>(vrs_tile_size_px_);
    const UINT image_width = std::max<UINT>(1U, (alloc_render_width_ + tile_size - 1U) / tile_size);
    const UINT image_height = std::max<UINT>(1U, (alloc_render_height_ + tile_size - 1U) / tile_size);

    D3D12_HEAP_PROPERTIES default_heap{};
    default_heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC image_desc{};
    image_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    image_desc.Width = image_width;
    image_desc.Height = image_height;
    image_desc.DepthOrArraySize = 1;
    image_desc.MipLevels = 1;
    image_desc.Format = DXGI_FORMAT_R8_UINT;
    image_desc.SampleDesc.Count = 1;
    image_desc.Flags = D3D12_RESOURCE_FLAG_NONE;
    check(device_->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &image_desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&vrs_image_)), "Create VRS image");

    const UINT row_pitch = (image_width + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1U) & ~(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1U);
    const std::size_t upload_bytes = static_cast<std::size_t>(row_pitch) * static_cast<std::size_t>(image_height);
    ComPtr<ID3D12Resource> upload_buffer = create_buffer(
        device_.Get(),
        D3D12_HEAP_TYPE_UPLOAD,
        upload_bytes,
        D3D12_RESOURCE_FLAG_NONE,
        D3D12_RESOURCE_STATE_GENERIC_READ,
        "Create VRS upload buffer");

    std::uint8_t* mapped = nullptr;
    D3D12_RANGE empty_range{0, 0};
    check(upload_buffer->Map(0, &empty_range, reinterpret_cast<void**>(&mapped)), "Map VRS upload buffer");
    for (UINT y = 0; y < image_height; ++y) {
        std::uint8_t* row = mapped + static_cast<std::size_t>(y) * row_pitch;
        const float ny = ((static_cast<float>(y) + 0.5f) / static_cast<float>(image_height) - 0.5f) * 2.0f;
        for (UINT x = 0; x < image_width; ++x) {
            const float nx = ((static_cast<float>(x) + 0.5f) / static_cast<float>(image_width) - 0.5f) * 2.0f;
            const float r2 = nx * nx + ny * ny;
            std::uint8_t rate = static_cast<std::uint8_t>(D3D12_SHADING_RATE_1X1);
            if (r2 < 0.36f * 0.36f) {
                rate = static_cast<std::uint8_t>(D3D12_SHADING_RATE_1X1);
            } else if (r2 < 0.65f * 0.65f) {
                rate = (nx * nx >= ny * ny)
                    ? static_cast<std::uint8_t>(D3D12_SHADING_RATE_2X1)
                    : static_cast<std::uint8_t>(D3D12_SHADING_RATE_1X2);
            } else if (r2 < 0.90f * 0.90f || !vrs_additional_rates_supported_) {
                rate = static_cast<std::uint8_t>(D3D12_SHADING_RATE_2X2);
            } else {
                rate = static_cast<std::uint8_t>(D3D12_SHADING_RATE_4X4);
            }
            row[x] = rate;
        }
    }
    upload_buffer->Unmap(0, nullptr);

    check(source_slots_[0].command_allocator->Reset(), "Reset VRS upload allocator");
    check(scene_command_list_->Reset(source_slots_[0].command_allocator.Get(), nullptr), "Reset VRS upload command list");

    D3D12_TEXTURE_COPY_LOCATION destination{};
    destination.pResource = vrs_image_.Get();
    destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    destination.SubresourceIndex = 0;

    D3D12_TEXTURE_COPY_LOCATION source{};
    source.pResource = upload_buffer.Get();
    source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    source.PlacedFootprint.Offset = 0;
    source.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8_UINT;
    source.PlacedFootprint.Footprint.Width = image_width;
    source.PlacedFootprint.Footprint.Height = image_height;
    source.PlacedFootprint.Footprint.Depth = 1;
    source.PlacedFootprint.Footprint.RowPitch = row_pitch;

    scene_command_list_->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
    const auto to_vrs_state = transition_barrier(vrs_image_.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_SHADING_RATE_SOURCE);
    scene_command_list_->ResourceBarrier(1, &to_vrs_state);
    check(scene_command_list_->Close(), "Close VRS upload command list");

    ID3D12CommandList* lists[] = {scene_command_list_.Get()};
    scene_queue_->ExecuteCommandLists(1, lists);
    const auto fence_value = next_scene_fence_value_++;
    check(scene_queue_->Signal(scene_fence_.Get(), fence_value), "Signal VRS upload fence");
    wait_for_fence(scene_fence_.Get(), scene_fence_event_.get(), fence_value);
}

void Renderer::create_swap_chain_targets() {
    auto handle = render_target_heap_->GetCPUDescriptorHandleForHeapStart();
    for (UINT i = 0; i < swap_chain_buffer_count; ++i) {
        check(swap_chain_->GetBuffer(i, IID_PPV_ARGS(&render_targets_[i])), "Get swap-chain buffer");
        device_->CreateRenderTargetView(render_targets_[i].Get(), nullptr, handle);
        handle.ptr += render_target_descriptor_stride_;
    }
}

void Renderer::create_dlss_input_targets() {
    if (!streamline_.capabilities().dlssg_supported) return;
    auto handle = render_target_heap_->GetCPUDescriptorHandleForHeapStart();
    handle.ptr += static_cast<SIZE_T>(swap_chain_buffer_count) * render_target_descriptor_stride_;
    for (UINT slot_index = 0; slot_index < dlss_input_slot_count; ++slot_index) {
        auto& slot = dlss_input_slots_[slot_index];
        slot.hudless_target = create_offscreen_texture(device_.Get(), viewport_width_, viewport_height_, DXGI_FORMAT_R8G8B8A8_UNORM, "Create DLSS HUD-less target");
        slot.clip_depth_target = create_offscreen_texture(device_.Get(), viewport_width_, viewport_height_, DXGI_FORMAT_R32_FLOAT, "Create DLSS warped clip-depth target");
        slot.motion_target = create_offscreen_texture(device_.Get(), viewport_width_, viewport_height_, DXGI_FORMAT_R16G16_FLOAT, "Create DLSS warped motion target");
        device_->CreateRenderTargetView(slot.hudless_target.Get(), nullptr, handle);
        handle.ptr += render_target_descriptor_stride_;
        device_->CreateRenderTargetView(slot.clip_depth_target.Get(), nullptr, handle);
        handle.ptr += render_target_descriptor_stride_;
        device_->CreateRenderTargetView(slot.motion_target.Get(), nullptr, handle);
        handle.ptr += render_target_descriptor_stride_;
    }
}

void Renderer::create_source_targets() {
    auto rtv = render_target_heap_->GetCPUDescriptorHandleForHeapStart();
    rtv.ptr += static_cast<SIZE_T>(swap_chain_buffer_count + dlss_input_slot_count * rtv_targets_per_dlss_slot) * render_target_descriptor_stride_;
    auto srv = shader_resource_heap_->GetCPUDescriptorHandleForHeapStart();
    auto tile_map_srv = shader_resource_heap_->GetCPUDescriptorHandleForHeapStart();
    tile_map_srv.ptr += static_cast<SIZE_T>(source_slot_count * targets_per_source_slot) * shader_resource_descriptor_stride_;
    auto compute_cpu = scene_compute_heap_->GetCPUDescriptorHandleForHeapStart();

    const UINT tiles_x = std::max<UINT>(1U, (alloc_render_width_ + 15U) / 16U);
    const UINT tiles_y = std::max<UINT>(1U, (alloc_render_height_ + 15U) / 16U);
    const D3D12_RESOURCE_FLAGS rt_uav_flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

    D3D12_SHADER_RESOURCE_VIEW_DESC srv_desc{};
    srv_desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv_desc.Texture2D.MipLevels = 1;

    D3D12_UNORDERED_ACCESS_VIEW_DESC uav_desc{};
    uav_desc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    uav_desc.Texture2D.MipSlice = 0;

    for (UINT slot_index = 0; slot_index < source_slot_count; ++slot_index) {
        auto& slot = source_slots_[slot_index];
        slot.color_target = create_offscreen_texture(device_.Get(), alloc_render_width_, alloc_render_height_, DXGI_FORMAT_R16G16B16A16_FLOAT, "Create source color target", rt_uav_flags, source_slot_readable_state);
        slot.opaque_cache_target = create_offscreen_texture(device_.Get(), alloc_render_width_, alloc_render_height_, DXGI_FORMAT_R16G16B16A16_FLOAT, "Create source opaque cache target", rt_uav_flags, source_slot_readable_state);
        slot.depth_target = create_offscreen_texture(device_.Get(), alloc_render_width_, alloc_render_height_, DXGI_FORMAT_R32_FLOAT, "Create source depth target", rt_uav_flags, source_slot_readable_state);
        slot.motion_target = create_offscreen_texture(device_.Get(), alloc_render_width_, alloc_render_height_, DXGI_FORMAT_R16G16_FLOAT, "Create source motion target", rt_uav_flags, source_slot_readable_state);
        slot.vis_target = create_offscreen_texture(device_.Get(), alloc_render_width_, alloc_render_height_, DXGI_FORMAT_R32_UINT, "Create source visibility target", rt_uav_flags, source_slot_readable_state);
        slot.tile_map_target = create_offscreen_texture(device_.Get(), tiles_x, tiles_y, DXGI_FORMAT_R8_UINT, "Create source tile map target", D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, source_slot_readable_state);
        slot.mat_gbuf0_target = create_offscreen_texture(device_.Get(), alloc_render_width_, alloc_render_height_, DXGI_FORMAT_R8G8B8A8_UNORM, "Create source thin material G-buffer 0", D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, source_slot_readable_state);
        slot.mat_gbuf1_target = create_offscreen_texture(device_.Get(), alloc_render_width_, alloc_render_height_, DXGI_FORMAT_R8G8B8A8_UNORM, "Create source thin material G-buffer 1", D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, source_slot_readable_state);
        const UINT half_alloc_w = std::max<UINT>(1U, (alloc_render_width_ + 1U) / 2U);
        const UINT half_alloc_h = std::max<UINT>(1U, (alloc_render_height_ + 1U) / 2U);
        slot.half_spec_target = create_offscreen_texture(device_.Get(), half_alloc_w, half_alloc_h, DXGI_FORMAT_R16G16B16A16_FLOAT, "Create source half-res rough specular target", D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, source_slot_readable_state);

        device_->CreateRenderTargetView(slot.color_target.Get(), nullptr, rtv);
        rtv.ptr += render_target_descriptor_stride_;
        device_->CreateRenderTargetView(slot.depth_target.Get(), nullptr, rtv);
        rtv.ptr += render_target_descriptor_stride_;
        device_->CreateRenderTargetView(slot.motion_target.Get(), nullptr, rtv);
        rtv.ptr += render_target_descriptor_stride_;

        auto vis_rtv = render_target_heap_->GetCPUDescriptorHandleForHeapStart();
        vis_rtv.ptr += static_cast<SIZE_T>(
            swap_chain_buffer_count
            + dlss_input_slot_count * rtv_targets_per_dlss_slot
            + source_slot_count * targets_per_source_slot
            + slot_index * targets_per_source_slot) * render_target_descriptor_stride_;
        device_->CreateRenderTargetView(slot.vis_target.Get(), nullptr, vis_rtv);
        vis_rtv.ptr += render_target_descriptor_stride_;
        device_->CreateRenderTargetView(slot.depth_target.Get(), nullptr, vis_rtv);
        vis_rtv.ptr += render_target_descriptor_stride_;
        device_->CreateRenderTargetView(slot.motion_target.Get(), nullptr, vis_rtv);

        srv_desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        device_->CreateShaderResourceView(slot.color_target.Get(), &srv_desc, srv);
        srv.ptr += shader_resource_descriptor_stride_;
        srv_desc.Format = DXGI_FORMAT_R32_FLOAT;
        device_->CreateShaderResourceView(slot.depth_target.Get(), &srv_desc, srv);
        srv.ptr += shader_resource_descriptor_stride_;
        srv_desc.Format = DXGI_FORMAT_R16G16_FLOAT;
        device_->CreateShaderResourceView(slot.motion_target.Get(), &srv_desc, srv);
        srv.ptr += shader_resource_descriptor_stride_;

        srv_desc.Format = DXGI_FORMAT_R8_UINT;
        device_->CreateShaderResourceView(slot.tile_map_target.Get(), &srv_desc, tile_map_srv);
        tile_map_srv.ptr += shader_resource_descriptor_stride_;

        srv_desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        device_->CreateShaderResourceView(slot.opaque_cache_target.Get(), &srv_desc, compute_cpu);
        compute_cpu.ptr += shader_resource_descriptor_stride_;
        srv_desc.Format = DXGI_FORMAT_R32_FLOAT;
        device_->CreateShaderResourceView(slot.depth_target.Get(), &srv_desc, compute_cpu);
        compute_cpu.ptr += shader_resource_descriptor_stride_;
        srv_desc.Format = DXGI_FORMAT_R32_UINT;
        device_->CreateShaderResourceView(slot.vis_target.Get(), &srv_desc, compute_cpu);
        compute_cpu.ptr += shader_resource_descriptor_stride_;

        uav_desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        device_->CreateUnorderedAccessView(slot.opaque_cache_target.Get(), nullptr, &uav_desc, compute_cpu);
        compute_cpu.ptr += shader_resource_descriptor_stride_;
        uav_desc.Format = DXGI_FORMAT_R32_FLOAT;
        device_->CreateUnorderedAccessView(slot.depth_target.Get(), nullptr, &uav_desc, compute_cpu);
        compute_cpu.ptr += shader_resource_descriptor_stride_;
        uav_desc.Format = DXGI_FORMAT_R16G16_FLOAT;
        device_->CreateUnorderedAccessView(slot.motion_target.Get(), nullptr, &uav_desc, compute_cpu);
        compute_cpu.ptr += shader_resource_descriptor_stride_;
        uav_desc.Format = DXGI_FORMAT_R32_UINT;
        device_->CreateUnorderedAccessView(slot.vis_target.Get(), nullptr, &uav_desc, compute_cpu);
        compute_cpu.ptr += shader_resource_descriptor_stride_;
        uav_desc.Format = DXGI_FORMAT_R8_UINT;
        device_->CreateUnorderedAccessView(slot.tile_map_target.Get(), nullptr, &uav_desc, compute_cpu);
        compute_cpu.ptr += shader_resource_descriptor_stride_;
        uav_desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        device_->CreateUnorderedAccessView(slot.color_target.Get(), nullptr, &uav_desc, compute_cpu);
        compute_cpu.ptr += shader_resource_descriptor_stride_;
        srv_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        device_->CreateShaderResourceView(slot.mat_gbuf0_target.Get(), &srv_desc, compute_cpu);
        compute_cpu.ptr += shader_resource_descriptor_stride_;
        srv_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        device_->CreateShaderResourceView(slot.mat_gbuf1_target.Get(), &srv_desc, compute_cpu);
        compute_cpu.ptr += shader_resource_descriptor_stride_;
        uav_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        device_->CreateUnorderedAccessView(slot.mat_gbuf0_target.Get(), nullptr, &uav_desc, compute_cpu);
        compute_cpu.ptr += shader_resource_descriptor_stride_;
        uav_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        device_->CreateUnorderedAccessView(slot.mat_gbuf1_target.Get(), nullptr, &uav_desc, compute_cpu);
        compute_cpu.ptr += shader_resource_descriptor_stride_;
        srv_desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        device_->CreateShaderResourceView(slot.half_spec_target.Get(), &srv_desc, compute_cpu);
        compute_cpu.ptr += shader_resource_descriptor_stride_;
        uav_desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        device_->CreateUnorderedAccessView(slot.half_spec_target.Get(), nullptr, &uav_desc, compute_cpu);
        compute_cpu.ptr += shader_resource_descriptor_stride_;
    }

    D3D12_HEAP_PROPERTIES dsv_heap_props{};
    dsv_heap_props.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC depth_desc{};
    depth_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    depth_desc.Width = alloc_render_width_;
    depth_desc.Height = alloc_render_height_;
    depth_desc.DepthOrArraySize = 1;
    depth_desc.MipLevels = 1;
    depth_desc.Format = DXGI_FORMAT_D32_FLOAT;
    depth_desc.SampleDesc.Count = 1;
    depth_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
    D3D12_CLEAR_VALUE depth_clear{};
    depth_clear.Format = DXGI_FORMAT_D32_FLOAT;
    depth_clear.DepthStencil.Depth = 1.0f;
    check(device_->CreateCommittedResource(&dsv_heap_props, D3D12_HEAP_FLAG_NONE, &depth_desc, D3D12_RESOURCE_STATE_DEPTH_WRITE, &depth_clear, IID_PPV_ARGS(&range_depth_buffer_)), "Create range depth buffer");
    D3D12_DEPTH_STENCIL_VIEW_DESC dsv_desc{};
    dsv_desc.Format = DXGI_FORMAT_D32_FLOAT;
    dsv_desc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
    device_->CreateDepthStencilView(range_depth_buffer_.Get(), &dsv_desc, depth_stencil_heap_->GetCPUDescriptorHandleForHeapStart());
}

void Renderer::wait_for_fence(ID3D12Fence* fence, HANDLE event_handle, std::uint64_t value) {
    const auto completed = fence->GetCompletedValue();
    if (completed == UINT64_MAX) {
        const HRESULT removed_reason = device_ ? device_->GetDeviceRemovedReason() : DXGI_ERROR_DEVICE_REMOVED;
        throw std::system_error(static_cast<int>(removed_reason), std::system_category(), "D3D12 device was removed");
    }
    if (value == 0 || completed >= value) return;
    check(fence->SetEventOnCompletion(value, event_handle), "Set fence event");
    for (int attempt = 0; attempt < 2; ++attempt) {
        const DWORD wait_result = WaitForSingleObject(event_handle, gpu_wait_timeout_ms);
        if (wait_result == WAIT_OBJECT_0) {
            if (fence->GetCompletedValue() == UINT64_MAX) {
                const HRESULT removed_reason = device_ ? device_->GetDeviceRemovedReason() : DXGI_ERROR_DEVICE_REMOVED;
                throw std::system_error(static_cast<int>(removed_reason), std::system_category(), "D3D12 device was removed after fence wait");
            }
            return;
        }
        if (wait_result == WAIT_TIMEOUT) {
            const HRESULT removed_reason = device_ ? device_->GetDeviceRemovedReason() : S_OK;
            if (FAILED(removed_reason)) {
                throw std::system_error(static_cast<int>(removed_reason), std::system_category(), "D3D12 device removed during GPU fence wait");
            }
            if (fence->GetCompletedValue() >= value) return;
            continue;
        }
        throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), "Wait for GPU fence");
    }
    throw std::runtime_error("Timed out waiting for D3D12 GPU fence completion");
}

}
