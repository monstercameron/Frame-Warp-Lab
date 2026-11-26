#include "renderer.h"
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
};

static_assert(sizeof(SceneConstantsData) <= 256);
static_assert(sizeof(PresentConstantsData) <= 512);
static_assert(sizeof(HudConstantsData) <= 2560);

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
    if (panel_width > 40.0f && panel_height > 40.0f) {
        simd::store_float4(destination->hud_panel_rect, 12.0f, 12.0f, panel_width, panel_height);
        simd::store_float4(
            destination->hud_graph_rect,
            22.0f,
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
    streamline_.attach_device(device_.Get());

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
        + source_slot_count * targets_per_source_slot;
    check(device_->CreateDescriptorHeap(&rtv_heap_description, IID_PPV_ARGS(&render_target_heap_)), "Create RTV heap");
    render_target_descriptor_stride_ = device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    D3D12_DESCRIPTOR_HEAP_DESC srv_heap_description{};
    srv_heap_description.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    srv_heap_description.NumDescriptors = source_slot_count * (targets_per_source_slot + 1U);
    srv_heap_description.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    check(device_->CreateDescriptorHeap(&srv_heap_description, IID_PPV_ARGS(&shader_resource_heap_)), "Create SRV heap");
    shader_resource_descriptor_stride_ = device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    D3D12_DESCRIPTOR_HEAP_DESC compute_heap_description{};
    compute_heap_description.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    compute_heap_description.NumDescriptors = source_slot_count * compute_descriptors_per_source_slot + 2U;
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
    VrsMode vrs_mode,
    bool selective_tiles_enabled,
    std::int32_t tile_budget_pct,
    TilePolicy tile_policy,
    bool cache_sharpen_enabled,
    bool coarse_fallback_enabled,
    std::uint64_t frame_id,
    LatencyMonitor& latency_monitor,
    bool wait_for_completion) {
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
    const UINT tile_budget = (clamped_budget_pct >= 100)
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
    write_float4(scene_constants->previous_camera_forward_unused, prev_camera.forward, 0.0f);
    write_float4(scene_constants->ball_center_stress, snapshot.ball_center, static_cast<float>(stress_level));
    write_float4(scene_constants->previous_ball_center_unused, prev_ball_center, 0.0f);
    simd::store_uint4(
        scene_constants->tile_params,
        tiles_x,
        tiles_y,
        std::min<UINT>(tile_budget, max_dirty_tile_capacity),
        static_cast<std::uint32_t>(snapshot.source_frame_id));
    simd::store_uint4(
        scene_constants->cache_params,
        can_use_prev_cache ? 1U : 0U,
        16U,
        render_width_,
        render_height_);
    const UINT prev_active_w = can_use_prev_cache && source_slots_[static_cast<std::size_t>(prev_slot_index)].active_width > 0U
        ? source_slots_[static_cast<std::size_t>(prev_slot_index)].active_width
        : render_width_;
    const UINT prev_active_h = can_use_prev_cache && source_slots_[static_cast<std::size_t>(prev_slot_index)].active_height > 0U
        ? source_slots_[static_cast<std::size_t>(prev_slot_index)].active_height
        : render_height_;
    simd::store_uint4(
        scene_constants->policy_params,
        tile_policy == TilePolicy::priority_stbn ? 1U : 0U,
        total_tiles,
        cache_sharpen_enabled ? 1U : 0U,
        coarse_fallback_enabled ? 1U : 0U);
    simd::store_uint4(
        scene_constants->prev_and_alloc_dims,
        prev_active_w,
        prev_active_h,
        alloc_render_width_,
        alloc_render_height_);

    latency_monitor.emit_marker(LatencyMarker::render_submit_start, frame_id);
    check(slot.command_allocator->Reset(), "Reset scene command allocator");
    check(scene_command_list_->Reset(slot.command_allocator.Get(), nullptr), "Reset scene command list");

    const UINT query_base = static_cast<UINT>(slot_index) * timestamps_per_pass;
    if (scene_timestamp_heap_) {
        scene_command_list_->EndQuery(scene_timestamp_heap_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, query_base + 0);
    }

    if (selective_tiles_enabled && tile_vis_classify_pipeline_ && tile_admit_pipeline_ && tile_shade_indirect_pipeline_ && dispatch_command_signature_) {
        const std::array<D3D12_RESOURCE_BARRIER, 6> to_uav = {
            transition_barrier(slot.color_target.Get(), source_slot_readable_state, D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
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

        scene_command_list_->SetComputeRootDescriptorTable(1, prev_srv_gpu);
        scene_command_list_->SetComputeRootDescriptorTable(2, curr_uav_gpu);
        scene_command_list_->SetComputeRootDescriptorTable(3, shared_uav_gpu);
        scene_command_list_->SetComputeRootUnorderedAccessView(4, tile_indirect_args_uav_->GetGPUVirtualAddress());
        scene_command_list_->Dispatch(tiles_x, tiles_y, 1);

        const std::array<D3D12_RESOURCE_BARRIER, 3> admit_barriers = {
            uav_barrier(tile_candidate_meta_uav_.Get()),
            uav_barrier(tile_indirect_args_uav_.Get()),
            uav_barrier(slot.tile_map_target.Get()),
        };
        scene_command_list_->ResourceBarrier(static_cast<UINT>(admit_barriers.size()), admit_barriers.data());

        scene_command_list_->SetPipelineState(tile_admit_pipeline_.Get());
        scene_command_list_->Dispatch((total_tiles + 255U) / 256U, 1, 1);

        const std::array<D3D12_RESOURCE_BARRIER, 6> mid_barriers = {
            uav_barrier(dirty_tile_list_uav_.Get()),
            uav_barrier(slot.color_target.Get()),
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

        const std::array<D3D12_RESOURCE_BARRIER, 6> after_compute = {
            transition_barrier(slot.color_target.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, source_slot_readable_state),
            transition_barrier(slot.depth_target.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, source_slot_readable_state),
            transition_barrier(slot.motion_target.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, source_slot_readable_state),
            transition_barrier(slot.vis_target.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, source_slot_readable_state),
            transition_barrier(slot.tile_map_target.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, source_slot_readable_state),
            transition_barrier(tile_indirect_args_uav_.Get(), D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT, D3D12_RESOURCE_STATE_COPY_SOURCE),
        };
        scene_command_list_->ResourceBarrier(static_cast<UINT>(after_compute.size()), after_compute.data());

        scene_command_list_->CopyBufferRegion(
            tile_stats_readback_.Get(),
            static_cast<UINT64>(slot_index) * tile_stats_readback_u32_count * sizeof(std::uint32_t),
            tile_indirect_args_uav_.Get(),
            0,
            static_cast<UINT64>(tile_stats_readback_u32_count) * sizeof(std::uint32_t));
        const auto args_restore_uav = transition_barrier(tile_indirect_args_uav_.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        scene_command_list_->ResourceBarrier(1, &args_restore_uav);
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
        scene_command_list_->OMSetRenderTargets(targets_per_source_slot, &offscreen_rtv, TRUE, nullptr);
        scene_command_list_->SetGraphicsRootSignature(scene_root_signature_.Get());
        scene_command_list_->SetPipelineState(scene_pipeline_.Get());
        scene_command_list_->SetGraphicsRootConstantBufferView(0, scene_constant_buffer_->GetGPUVirtualAddress() + constant_offset);
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

        const std::array<D3D12_RESOURCE_BARRIER, targets_per_source_slot> to_srv = {
            transition_barrier(slot.color_target.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, source_slot_readable_state),
            transition_barrier(slot.depth_target.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, source_slot_readable_state),
            transition_barrier(slot.motion_target.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, source_slot_readable_state),
        };
        scene_command_list_->ResourceBarrier(static_cast<UINT>(to_srv.size()), to_srv.data());
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
        slot.depth_target.Reset();
        slot.motion_target.Reset();
        slot.vis_target.Reset();
        slot.tile_map_target.Reset();
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
        slot.depth_target.Reset();
        slot.motion_target.Reset();
        slot.vis_target.Reset();
        slot.tile_map_target.Reset();
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

    D3D12_ROOT_PARAMETER scene_parameter{};
    scene_parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    scene_parameter.Descriptor.ShaderRegister = 0;
    scene_parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC scene_root_description{};
    scene_root_description.NumParameters = 1;
    scene_root_description.pParameters = &scene_parameter;
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

    std::array<D3D12_ROOT_PARAMETER, 5> tile_compute_parameters{};
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

    std::array<D3D12_STATIC_SAMPLER_DESC, 2> compute_samplers = samplers;
    compute_samplers[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    compute_samplers[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

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
        slot.depth_target = create_offscreen_texture(device_.Get(), alloc_render_width_, alloc_render_height_, DXGI_FORMAT_R32_FLOAT, "Create source depth target", rt_uav_flags, source_slot_readable_state);
        slot.motion_target = create_offscreen_texture(device_.Get(), alloc_render_width_, alloc_render_height_, DXGI_FORMAT_R16G16_FLOAT, "Create source motion target", rt_uav_flags, source_slot_readable_state);
        slot.vis_target = create_offscreen_texture(device_.Get(), alloc_render_width_, alloc_render_height_, DXGI_FORMAT_R32_UINT, "Create source visibility target", D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, source_slot_readable_state);
        slot.tile_map_target = create_offscreen_texture(device_.Get(), tiles_x, tiles_y, DXGI_FORMAT_R8_UINT, "Create source tile map target", D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, source_slot_readable_state);

        device_->CreateRenderTargetView(slot.color_target.Get(), nullptr, rtv);
        rtv.ptr += render_target_descriptor_stride_;
        device_->CreateRenderTargetView(slot.depth_target.Get(), nullptr, rtv);
        rtv.ptr += render_target_descriptor_stride_;
        device_->CreateRenderTargetView(slot.motion_target.Get(), nullptr, rtv);
        rtv.ptr += render_target_descriptor_stride_;

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
        device_->CreateShaderResourceView(slot.color_target.Get(), &srv_desc, compute_cpu);
        compute_cpu.ptr += shader_resource_descriptor_stride_;
        srv_desc.Format = DXGI_FORMAT_R32_FLOAT;
        device_->CreateShaderResourceView(slot.depth_target.Get(), &srv_desc, compute_cpu);
        compute_cpu.ptr += shader_resource_descriptor_stride_;
        srv_desc.Format = DXGI_FORMAT_R32_UINT;
        device_->CreateShaderResourceView(slot.vis_target.Get(), &srv_desc, compute_cpu);
        compute_cpu.ptr += shader_resource_descriptor_stride_;

        uav_desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        device_->CreateUnorderedAccessView(slot.color_target.Get(), nullptr, &uav_desc, compute_cpu);
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
    }
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
