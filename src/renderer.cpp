#include "renderer.h"

#include <d3dcompiler.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <system_error>

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
};

struct PresentConstantsData {
    float viewport_and_flags[4];
    float warp_flags_and_aspect[4];
    float source_camera_position_tan_y[4];
    float source_camera_right_unused[4];
    float source_camera_up_unused[4];
    float source_camera_forward_unused[4];
    float target_camera_position_tan_y[4];
    float target_camera_right_unused[4];
    float target_camera_up_unused[4];
    float target_camera_forward_unused[4];
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
static_assert(sizeof(PresentConstantsData) <= 256);
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

ComPtr<ID3DBlob> compile_hlsl(const std::filesystem::path& path, const char* entry, const char* target) {
    ComPtr<ID3DBlob> shader;
    ComPtr<ID3DBlob> errors;
    const HRESULT result = D3DCompileFromFile(path.c_str(), nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE,
                                              entry, target, D3DCOMPILE_ENABLE_STRICTNESS, 0, &shader, &errors);
    if (FAILED(result)) {
        const std::string detail = errors
            ? std::string(static_cast<const char*>(errors->GetBufferPointer()), errors->GetBufferSize())
            : "Shader file unavailable";
        throw std::runtime_error("Shader compilation failed (" + std::string(entry) + "): " + detail);
    }
    return shader;
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

ComPtr<ID3D12Resource> create_offscreen_texture(ID3D12Device* device, UINT width, UINT height, DXGI_FORMAT format, const char* operation) {
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
    description.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    ComPtr<ID3D12Resource> resource;
    check(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &description, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr, IID_PPV_ARGS(&resource)), operation);
    return resource;
}

void write_float4(float (&destination)[4], Float3 vector, float w) {
    destination[0] = vector.x;
    destination[1] = vector.y;
    destination[2] = vector.z;
    destination[3] = w;
}

HudConstantsData build_hud_constants(const HudOverlayState& hud, UINT viewport_width, UINT viewport_height) {
    HudConstantsData data{};
    const std::uint32_t char_scale = (viewport_width >= 1000 && viewport_height >= 520) ? 2U : 1U;
    constexpr std::uint32_t line_count = 4U;
    constexpr std::uint32_t chars_per_line = 48U;

    const float text_width = static_cast<float>(chars_per_line * 6U * char_scale);
    const float text_height = static_cast<float>(line_count * 9U * char_scale);
    const float graph_height = char_scale == 2U ? 104.0f : 72.0f;
    const float panel_width = std::min(static_cast<float>(viewport_width) - 24.0f, text_width + 20.0f);
    const float panel_height = std::min(static_cast<float>(viewport_height) - 24.0f, text_height + graph_height + 26.0f);
    if (panel_width > 40.0f && panel_height > 40.0f) {
        data.hud_panel_rect[0] = 12.0f;
        data.hud_panel_rect[1] = 12.0f;
        data.hud_panel_rect[2] = panel_width;
        data.hud_panel_rect[3] = panel_height;

        data.hud_graph_rect[0] = 22.0f;
        data.hud_graph_rect[1] = 12.0f + 8.0f + text_height + 8.0f;
        data.hud_graph_rect[2] = std::max(0.0f, panel_width - 20.0f);
        data.hud_graph_rect[3] = std::max(0.0f, panel_height - (text_height + 26.0f));
    }

    data.hud_graph_scale[0] = hud.max_graph_ms > 1.0f ? hud.max_graph_ms : 25.0f;
    data.hud_graph_scale[1] = hud.target_frame_ms > 0.5f ? hud.target_frame_ms : 8.333f;
    data.hud_graph_scale[2] = static_cast<float>(hud_graph_sample_count);
    data.hud_graph_scale[3] = static_cast<float>(hud.write_head & (hud_graph_sample_count - 1));

    data.hud_text_meta[0] = line_count;
    data.hud_text_meta[1] = chars_per_line;
    data.hud_text_meta[2] = char_scale;
    data.hud_text_meta[3] = 0U;

    for (std::size_t line_index = 0; line_index < hud.lines.size(); ++line_index) {
        const std::string& line = hud.lines[line_index];
        const std::size_t length = std::min<std::size_t>(line.size(), 64U);
        for (std::size_t col = 0; col < length; ++col) {
            const unsigned char ascii = static_cast<unsigned char>(std::toupper(static_cast<unsigned char>(line[col])));
            const std::size_t char_index = line_index * 64U + col;
            const std::size_t word_index = char_index >> 2U;
            const std::size_t byte_shift = (char_index & 3U) * 8U;
            data.hud_text_packed[word_index] |= static_cast<std::uint32_t>(ascii) << byte_shift;
        }
    }

    for (std::size_t index = 0; index < hud_graph_sample_count; ++index) {
        data.hud_graph_samples[index][0] = hud.samples[index].present_interval_ms;
        data.hud_graph_samples[index][1] = hud.samples[index].gpu_total_ms;
        data.hud_graph_samples[index][2] = hud.samples[index].frame_kind;
        data.hud_graph_samples[index][3] = hud.samples[index].source_age_ms;
    }

    return data;
}

}

Renderer::OsHandle::OsHandle(HANDLE value, const char* operation) : value_(value) {
    if (!value_) throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), operation);
}

Renderer::OsHandle::~OsHandle() {
    CloseHandle(value_);
}

Renderer::Renderer(HWND window)
    : fence_completion_event_(CreateEventW(nullptr, FALSE, FALSE, nullptr), "Create fence event") {
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
    D3D12_COMMAND_QUEUE_DESC queue_description{};
    queue_description.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    check(device_->CreateCommandQueue(&queue_description, IID_PPV_ARGS(&queue_)), "Create command queue");

    RECT client_rectangle{};
    if (!GetClientRect(window, &client_rectangle)) {
        throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), "GetClientRect");
    }
    const UINT initial_width = std::max<UINT>(1, static_cast<UINT>(client_rectangle.right - client_rectangle.left));
    const UINT initial_height = std::max<UINT>(1, static_cast<UINT>(client_rectangle.bottom - client_rectangle.top));

    swap_chain_flags_ = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
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
    check(factory_->CreateSwapChainForHwnd(queue_.Get(), window, &swap_chain_description, nullptr, nullptr, &swap_chain), "Create swap chain");
    check(swap_chain.As(&swap_chain_), "Query swap chain");
    check(factory_->MakeWindowAssociation(window, DXGI_MWA_NO_ALT_ENTER), "Configure window association");
    check(swap_chain_->SetMaximumFrameLatency(swap_chain_buffer_count), "Set maximum frame latency");
    frame_latency_waitable_object_ = swap_chain_->GetFrameLatencyWaitableObject();

    D3D12_DESCRIPTOR_HEAP_DESC rtv_heap_description{};
    rtv_heap_description.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtv_heap_description.NumDescriptors = swap_chain_buffer_count + offscreen_target_count;
    check(device_->CreateDescriptorHeap(&rtv_heap_description, IID_PPV_ARGS(&render_target_heap_)), "Create RTV heap");
    render_target_descriptor_stride_ = device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    D3D12_DESCRIPTOR_HEAP_DESC srv_heap_description{};
    srv_heap_description.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    srv_heap_description.NumDescriptors = offscreen_target_count;
    srv_heap_description.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    check(device_->CreateDescriptorHeap(&srv_heap_description, IID_PPV_ARGS(&shader_resource_heap_)), "Create SRV heap");
    shader_resource_descriptor_stride_ = device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    for (auto& allocator : command_allocators_) {
        check(device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)), "Create command allocator");
    }
    check(device_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, command_allocators_[0].Get(), nullptr, IID_PPV_ARGS(&command_list_)), "Create command list");
    check(command_list_->Close(), "Close initial command list");
    check(device_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence_)), "Create fence");

    viewport_width_ = initial_width;
    viewport_height_ = initial_height;
    create_swap_chain_targets();
    create_offscreen_targets();
    create_constant_buffers();
    create_timestamp_resources();
    create_reprojection_stats_resources();
    create_pipelines();
}

Renderer::~Renderer() noexcept {
    if (queue_ && fence_) {
        try {
            flush();
        } catch (...) {
        }
    }
    if (frame_latency_waitable_object_) {
        CloseHandle(frame_latency_waitable_object_);
        frame_latency_waitable_object_ = nullptr;
    }
}

void Renderer::wait_for_frame_slot() {
    if (viewport_width_ == 0 || viewport_height_ == 0 || has_acquired_slot_) return;
    if (frame_latency_waitable_object_) {
        WaitForSingleObjectEx(frame_latency_waitable_object_, 1000, TRUE);
    }
    pending_buffer_index_ = swap_chain_->GetCurrentBackBufferIndex();
    wait_for_fence(buffer_fence_values_[pending_buffer_index_]);
    has_acquired_slot_ = true;
}

FrameExecutionMetrics Renderer::render_and_present(const FrameRenderRequest& request, LatencyMonitor& latency_monitor, FramePacer& pacer) {
    FrameExecutionMetrics metrics{std::chrono::steady_clock::now(), -1, -1, 0, 0, 0, 0, false, false, false, false, gpu_timing_available()};
    if (viewport_width_ == 0 || viewport_height_ == 0) {
        return metrics;
    }

    if (!has_acquired_slot_) {
        wait_for_frame_slot();
    }
    const UINT buffer_index = pending_buffer_index_;
    has_acquired_slot_ = false;

    if (buffer_has_data_[buffer_index]) {
        metrics.completed_slot_valid = true;
        metrics.completed_slot_source_rendered = buffer_source_rendered_[buffer_index];
        metrics.completed_slot_warped = buffer_warped_[buffer_index];
        metrics.completed_total_pixel_count = buffer_total_pixels_[buffer_index];
        if (mapped_timestamps_ && gpu_timestamp_frequency_ > 0) {
            const std::size_t base_index = static_cast<std::size_t>(buffer_index) * timestamps_per_frame;
            const std::uint64_t scene_begin = mapped_timestamps_[base_index + 0];
            const std::uint64_t scene_end = mapped_timestamps_[base_index + 1];
            const std::uint64_t present_begin = mapped_timestamps_[base_index + 2];
            const std::uint64_t present_end = mapped_timestamps_[base_index + 3];
            if (buffer_source_rendered_[buffer_index] && scene_end >= scene_begin) {
                metrics.completed_scene_gpu_us = static_cast<std::int64_t>(((scene_end - scene_begin) * 1000000ULL) / gpu_timestamp_frequency_);
            }
            if (present_end >= present_begin) {
                metrics.completed_present_gpu_us = static_cast<std::int64_t>(((present_end - present_begin) * 1000000ULL) / gpu_timestamp_frequency_);
            }
        }
        if (buffer_warped_[buffer_index] && mapped_reprojection_stats_) {
            metrics.completed_invalid_pixel_count = mapped_reprojection_stats_[buffer_index];
        }
    }

    const auto submit_begin_time = std::chrono::steady_clock::now();
    latency_monitor.emit_marker(
        request.warp_enabled ? LatencyMarker::late_warp_submit_start : LatencyMarker::render_submit_start,
        request.display_frame_id);

    const float aspect_ratio = static_cast<float>(viewport_width_) / static_cast<float>(viewport_height_);
    const std::size_t constant_offset = static_cast<std::size_t>(buffer_index) * constant_buffer_stride;
    const std::size_t hud_offset = static_cast<std::size_t>(buffer_index) * hud_constant_buffer_stride;

    if (request.render_scene_pass) {
        SceneConstantsData scene_constants{};
        write_float4(scene_constants.camera_position_tan_y, request.source_snapshot.camera.position, request.source_snapshot.camera.tan_half_fov_y);
        write_float4(scene_constants.camera_right_aspect, request.source_snapshot.camera.right, aspect_ratio);
        write_float4(scene_constants.camera_up_width, request.source_snapshot.camera.up, static_cast<float>(viewport_width_));
        write_float4(scene_constants.camera_forward_height, request.source_snapshot.camera.forward, static_cast<float>(viewport_height_));
        write_float4(scene_constants.previous_camera_position_tan_y, request.source_snapshot.previous_camera.position, request.source_snapshot.previous_camera.tan_half_fov_y);
        write_float4(scene_constants.previous_camera_right_aspect, request.source_snapshot.previous_camera.right, aspect_ratio);
        write_float4(scene_constants.previous_camera_up_cut, request.source_snapshot.previous_camera.up, request.source_snapshot.discontinuity ? 1.0f : 0.0f);
        write_float4(scene_constants.previous_camera_forward_unused, request.source_snapshot.previous_camera.forward, 0.0f);
        write_float4(scene_constants.ball_center_stress, request.source_snapshot.ball_center, static_cast<float>(request.stress_level));
        write_float4(scene_constants.previous_ball_center_unused, request.source_snapshot.previous_ball_center, 0.0f);
        std::memcpy(mapped_scene_constants_ + constant_offset, &scene_constants, sizeof(scene_constants));
    }

    PresentConstantsData present_constants{};
    present_constants.viewport_and_flags[0] = static_cast<float>(viewport_width_);
    present_constants.viewport_and_flags[1] = static_cast<float>(viewport_height_);
    present_constants.viewport_and_flags[2] = static_cast<float>(static_cast<std::int32_t>(request.diagnostic_view));
    present_constants.viewport_and_flags[3] = request.overlay_enabled ? 1.0f : 0.0f;
    present_constants.warp_flags_and_aspect[0] = request.warp_enabled ? 1.0f : 0.0f;
    present_constants.warp_flags_and_aspect[1] = aspect_ratio;
    present_constants.warp_flags_and_aspect[2] = request.flash_indicator_enabled ? 1.0f : 0.0f;
    present_constants.warp_flags_and_aspect[3] = request.flash_triggered ? 1.0f : 0.0f;
    write_float4(present_constants.source_camera_position_tan_y, request.source_snapshot.camera.position, request.source_snapshot.camera.tan_half_fov_y);
    write_float4(present_constants.source_camera_right_unused, request.source_snapshot.camera.right, 0.0f);
    write_float4(present_constants.source_camera_up_unused, request.source_snapshot.camera.up, 0.0f);
    write_float4(present_constants.source_camera_forward_unused, request.source_snapshot.camera.forward, 0.0f);
    write_float4(present_constants.target_camera_position_tan_y, request.target_camera.position, request.target_camera.tan_half_fov_y);
    write_float4(present_constants.target_camera_right_unused, request.target_camera.right, 0.0f);
    write_float4(present_constants.target_camera_up_unused, request.target_camera.up, 0.0f);
    write_float4(present_constants.target_camera_forward_unused, request.target_camera.forward, 0.0f);
    std::memcpy(mapped_present_constants_ + constant_offset, &present_constants, sizeof(present_constants));

    if (request.overlay_enabled) {
        const HudConstantsData hud_constants = build_hud_constants(request.hud, viewport_width_, viewport_height_);
        std::memcpy(mapped_hud_constants_ + hud_offset, &hud_constants, sizeof(hud_constants));
    }

    check(command_allocators_[buffer_index]->Reset(), "Reset command allocator");
    check(command_list_->Reset(command_allocators_[buffer_index].Get(), nullptr), "Reset command list");

    const D3D12_VIEWPORT viewport{0.0f, 0.0f, static_cast<float>(viewport_width_), static_cast<float>(viewport_height_), 0.0f, 1.0f};
    const D3D12_RECT scissor{0, 0, static_cast<LONG>(viewport_width_), static_cast<LONG>(viewport_height_)};
    command_list_->RSSetViewports(1, &viewport);
    command_list_->RSSetScissorRects(1, &scissor);
    command_list_->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    const UINT query_base = buffer_index * timestamps_per_frame;
    if (timestamp_query_heap_) {
        command_list_->EndQuery(timestamp_query_heap_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, query_base + 0);
    }

    if (request.render_scene_pass) {
        const std::array<D3D12_RESOURCE_BARRIER, offscreen_target_count> to_render_target = {
            transition_barrier(scene_color_target_.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET),
            transition_barrier(scene_depth_target_.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET),
            transition_barrier(scene_motion_target_.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET),
        };
        command_list_->ResourceBarrier(static_cast<UINT>(to_render_target.size()), to_render_target.data());

        auto offscreen_rtv = render_target_heap_->GetCPUDescriptorHandleForHeapStart();
        offscreen_rtv.ptr += static_cast<SIZE_T>(swap_chain_buffer_count) * render_target_descriptor_stride_;
        command_list_->OMSetRenderTargets(offscreen_target_count, &offscreen_rtv, TRUE, nullptr);
        command_list_->SetGraphicsRootSignature(scene_root_signature_.Get());
        command_list_->SetPipelineState(scene_pipeline_.Get());
        command_list_->SetGraphicsRootConstantBufferView(0, scene_constant_buffer_->GetGPUVirtualAddress() + constant_offset);
        command_list_->DrawInstanced(3, 1, 0, 0);

        const std::array<D3D12_RESOURCE_BARRIER, offscreen_target_count> to_shader_resource = {
            transition_barrier(scene_color_target_.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE),
            transition_barrier(scene_depth_target_.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE),
            transition_barrier(scene_motion_target_.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE),
        };
        command_list_->ResourceBarrier(static_cast<UINT>(to_shader_resource.size()), to_shader_resource.data());
    }

    if (timestamp_query_heap_) {
        command_list_->EndQuery(timestamp_query_heap_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, query_base + 1);
    }

    const auto stats_to_copy_dest = transition_barrier(reprojection_stats_uav_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
    command_list_->ResourceBarrier(1, &stats_to_copy_dest);
    command_list_->CopyBufferRegion(reprojection_stats_uav_.Get(), 0, reprojection_stats_zero_upload_.Get(), 0, sizeof(std::uint32_t));
    const auto stats_to_uav = transition_barrier(reprojection_stats_uav_.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    command_list_->ResourceBarrier(1, &stats_to_uav);

    if (timestamp_query_heap_) {
        command_list_->EndQuery(timestamp_query_heap_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, query_base + 2);
    }

    const auto back_buffer_to_rtv = transition_barrier(render_targets_[buffer_index].Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
    command_list_->ResourceBarrier(1, &back_buffer_to_rtv);

    auto back_buffer_rtv = render_target_heap_->GetCPUDescriptorHandleForHeapStart();
    back_buffer_rtv.ptr += static_cast<SIZE_T>(buffer_index) * render_target_descriptor_stride_;
    command_list_->OMSetRenderTargets(1, &back_buffer_rtv, FALSE, nullptr);

    ID3D12DescriptorHeap* heaps[] = {shader_resource_heap_.Get()};
    command_list_->SetDescriptorHeaps(1, heaps);
    command_list_->SetGraphicsRootSignature(present_root_signature_.Get());
    command_list_->SetPipelineState(present_pipeline_.Get());
    command_list_->SetGraphicsRootConstantBufferView(0, present_constant_buffer_->GetGPUVirtualAddress() + constant_offset);
    command_list_->SetGraphicsRootDescriptorTable(1, shader_resource_heap_->GetGPUDescriptorHandleForHeapStart());
    command_list_->SetGraphicsRootUnorderedAccessView(2, reprojection_stats_uav_->GetGPUVirtualAddress());
    command_list_->SetGraphicsRootConstantBufferView(3, hud_constant_buffer_->GetGPUVirtualAddress() + hud_offset);
    command_list_->DrawInstanced(3, 1, 0, 0);

    const auto back_buffer_to_present = transition_barrier(render_targets_[buffer_index].Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
    command_list_->ResourceBarrier(1, &back_buffer_to_present);

    if (timestamp_query_heap_) {
        command_list_->EndQuery(timestamp_query_heap_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, query_base + 3);
        command_list_->ResolveQueryData(
            timestamp_query_heap_.Get(),
            D3D12_QUERY_TYPE_TIMESTAMP,
            query_base,
            timestamps_per_frame,
            timestamp_readback_buffer_.Get(),
            static_cast<UINT64>(query_base) * sizeof(std::uint64_t));
    }

    const auto stats_to_copy_source = transition_barrier(reprojection_stats_uav_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    command_list_->ResourceBarrier(1, &stats_to_copy_source);
    command_list_->CopyBufferRegion(
        reprojection_stats_readback_.Get(),
        static_cast<UINT64>(buffer_index) * sizeof(std::uint32_t),
        reprojection_stats_uav_.Get(),
        0,
        sizeof(std::uint32_t));
    const auto stats_restore_uav = transition_barrier(reprojection_stats_uav_.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    command_list_->ResourceBarrier(1, &stats_restore_uav);

    check(command_list_->Close(), "Close command list");
    ID3D12CommandList* command_lists[] = {command_list_.Get()};
    queue_->ExecuteCommandLists(1, command_lists);

    const auto submit_end_time = std::chrono::steady_clock::now();
    latency_monitor.emit_marker(
        request.warp_enabled ? LatencyMarker::late_warp_submit_end : LatencyMarker::render_submit_end,
        request.display_frame_id);
    metrics.render_submit_cpu_us = std::chrono::duration_cast<std::chrono::microseconds>(submit_end_time - submit_begin_time).count();

    metrics.missed_deadline = pacer.wait_for_present_deadline();

    const auto present_begin_time = std::chrono::steady_clock::now();
    metrics.present_call_time = present_begin_time;
    latency_monitor.emit_marker(
        request.warp_enabled ? LatencyMarker::late_warp_present_start : LatencyMarker::present_start,
        request.display_frame_id);

    const UINT sync_interval = request.vsync_enabled ? 1U : 0U;
    const UINT present_flags = (!request.vsync_enabled && allow_tearing_supported_) ? DXGI_PRESENT_ALLOW_TEARING : 0U;
    check(swap_chain_->Present(sync_interval, present_flags), "Present");

    const auto present_end_time = std::chrono::steady_clock::now();
    latency_monitor.emit_marker(
        request.warp_enabled ? LatencyMarker::late_warp_present_end : LatencyMarker::present_end,
        request.display_frame_id);
    metrics.present_call_cpu_us = std::chrono::duration_cast<std::chrono::microseconds>(present_end_time - present_begin_time).count();

    const auto value = next_fence_value_++;
    check(queue_->Signal(fence_.Get(), value), "Signal frame fence");
    buffer_fence_values_[buffer_index] = value;
    buffer_has_data_[buffer_index] = true;
    buffer_source_rendered_[buffer_index] = request.render_scene_pass;
    buffer_warped_[buffer_index] = request.warp_enabled;
    buffer_total_pixels_[buffer_index] = viewport_width_ * viewport_height_;

    return metrics;
}

void Renderer::resize(UINT width, UINT height) {
    if (width == 0 || height == 0) return;
    flush();
    has_acquired_slot_ = false;
    for (auto& target : render_targets_) target.Reset();
    scene_color_target_.Reset();
    scene_depth_target_.Reset();
    scene_motion_target_.Reset();
    check(swap_chain_->ResizeBuffers(swap_chain_buffer_count, width, height, DXGI_FORMAT_R8G8B8A8_UNORM, swap_chain_flags_), "Resize swap chain");
    viewport_width_ = width;
    viewport_height_ = height;
    buffer_fence_values_.fill(0);
    buffer_has_data_.fill(false);
    buffer_source_rendered_.fill(false);
    buffer_warped_.fill(false);
    buffer_total_pixels_.fill(0);
    create_swap_chain_targets();
    create_offscreen_targets();
}

void Renderer::flush() {
    const auto value = next_fence_value_++;
    check(queue_->Signal(fence_.Get(), value), "Signal flush fence");
    wait_for_fence(value);
}

void Renderer::create_pipelines() {
    const auto shaders = shader_directory();
    const auto scene_vs = compile_hlsl(shaders / L"motion_lab.hlsl", "vertex_main", "vs_5_0");
    const auto scene_ps = compile_hlsl(shaders / L"motion_lab.hlsl", "pixel_main", "ps_5_0");
    const auto present_vs = compile_hlsl(shaders / L"present.hlsl", "vertex_main", "vs_5_0");
    const auto present_ps = compile_hlsl(shaders / L"present.hlsl", "pixel_main", "ps_5_0");

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
    scene_pipeline_desc.VS = {scene_vs->GetBufferPointer(), scene_vs->GetBufferSize()};
    scene_pipeline_desc.PS = {scene_ps->GetBufferPointer(), scene_ps->GetBufferSize()};
    for (UINT i = 0; i < offscreen_target_count; ++i) {
        scene_pipeline_desc.BlendState.RenderTarget[i].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    }
    scene_pipeline_desc.SampleMask = UINT_MAX;
    scene_pipeline_desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    scene_pipeline_desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    scene_pipeline_desc.RasterizerState.DepthClipEnable = TRUE;
    scene_pipeline_desc.DepthStencilState.DepthEnable = FALSE;
    scene_pipeline_desc.DepthStencilState.StencilEnable = FALSE;
    scene_pipeline_desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    scene_pipeline_desc.NumRenderTargets = offscreen_target_count;
    scene_pipeline_desc.RTVFormats[0] = DXGI_FORMAT_R16G16B16A16_FLOAT;
    scene_pipeline_desc.RTVFormats[1] = DXGI_FORMAT_R32_FLOAT;
    scene_pipeline_desc.RTVFormats[2] = DXGI_FORMAT_R16G16_FLOAT;
    scene_pipeline_desc.SampleDesc.Count = 1;
    check(device_->CreateGraphicsPipelineState(&scene_pipeline_desc, IID_PPV_ARGS(&scene_pipeline_)), "Create scene graphics pipeline");

    D3D12_DESCRIPTOR_RANGE srv_range{};
    srv_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    srv_range.NumDescriptors = offscreen_target_count;
    srv_range.BaseShaderRegister = 0;
    srv_range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    std::array<D3D12_ROOT_PARAMETER, 4> present_parameters{};
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
    present_pipeline_desc.VS = {present_vs->GetBufferPointer(), present_vs->GetBufferSize()};
    present_pipeline_desc.PS = {present_ps->GetBufferPointer(), present_ps->GetBufferSize()};
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
    check(device_->CreateGraphicsPipelineState(&present_pipeline_desc, IID_PPV_ARGS(&present_pipeline_)), "Create present graphics pipeline");
}

void Renderer::create_constant_buffers() {
    const std::size_t total_size = static_cast<std::size_t>(swap_chain_buffer_count) * constant_buffer_stride;
    const std::size_t total_hud_size = static_cast<std::size_t>(swap_chain_buffer_count) * hud_constant_buffer_stride;
    scene_constant_buffer_ = create_buffer(device_.Get(), D3D12_HEAP_TYPE_UPLOAD, total_size, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ, "Create scene constant buffer");
    present_constant_buffer_ = create_buffer(device_.Get(), D3D12_HEAP_TYPE_UPLOAD, total_size, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ, "Create present constant buffer");
    hud_constant_buffer_ = create_buffer(device_.Get(), D3D12_HEAP_TYPE_UPLOAD, total_hud_size, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ, "Create HUD constant buffer");
    D3D12_RANGE empty_range{0, 0};
    check(scene_constant_buffer_->Map(0, &empty_range, reinterpret_cast<void**>(&mapped_scene_constants_)), "Map scene constant buffer");
    check(present_constant_buffer_->Map(0, &empty_range, reinterpret_cast<void**>(&mapped_present_constants_)), "Map present constant buffer");
    check(hud_constant_buffer_->Map(0, &empty_range, reinterpret_cast<void**>(&mapped_hud_constants_)), "Map HUD constant buffer");
}

void Renderer::create_timestamp_resources() {
    if (FAILED(queue_->GetTimestampFrequency(&gpu_timestamp_frequency_)) || gpu_timestamp_frequency_ == 0) {
        gpu_timestamp_frequency_ = 0;
        return;
    }
    D3D12_QUERY_HEAP_DESC query_heap_description{};
    query_heap_description.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
    query_heap_description.Count = swap_chain_buffer_count * timestamps_per_frame;
    if (FAILED(device_->CreateQueryHeap(&query_heap_description, IID_PPV_ARGS(&timestamp_query_heap_)))) {
        gpu_timestamp_frequency_ = 0;
        return;
    }
    timestamp_readback_buffer_ = create_buffer(
        device_.Get(),
        D3D12_HEAP_TYPE_READBACK,
        static_cast<std::size_t>(query_heap_description.Count) * sizeof(std::uint64_t),
        D3D12_RESOURCE_FLAG_NONE,
        D3D12_RESOURCE_STATE_COPY_DEST,
        "Create timestamp readback buffer");
    if (FAILED(timestamp_readback_buffer_->Map(0, nullptr, reinterpret_cast<void**>(const_cast<std::uint64_t**>(&mapped_timestamps_))))) {
        timestamp_readback_buffer_.Reset();
        timestamp_query_heap_.Reset();
        gpu_timestamp_frequency_ = 0;
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
        static_cast<std::size_t>(swap_chain_buffer_count) * sizeof(std::uint32_t),
        D3D12_RESOURCE_FLAG_NONE,
        D3D12_RESOURCE_STATE_COPY_DEST,
        "Create reprojection stats readback buffer");
    check(reprojection_stats_readback_->Map(0, nullptr, reinterpret_cast<void**>(const_cast<std::uint32_t**>(&mapped_reprojection_stats_))), "Map reprojection stats readback buffer");
}

void Renderer::create_swap_chain_targets() {
    auto handle = render_target_heap_->GetCPUDescriptorHandleForHeapStart();
    for (UINT i = 0; i < swap_chain_buffer_count; ++i) {
        check(swap_chain_->GetBuffer(i, IID_PPV_ARGS(&render_targets_[i])), "Get swap-chain buffer");
        device_->CreateRenderTargetView(render_targets_[i].Get(), nullptr, handle);
        handle.ptr += render_target_descriptor_stride_;
    }
}

void Renderer::create_offscreen_targets() {
    scene_color_target_ = create_offscreen_texture(device_.Get(), viewport_width_, viewport_height_, DXGI_FORMAT_R16G16B16A16_FLOAT, "Create scene color target");
    scene_depth_target_ = create_offscreen_texture(device_.Get(), viewport_width_, viewport_height_, DXGI_FORMAT_R32_FLOAT, "Create scene depth target");
    scene_motion_target_ = create_offscreen_texture(device_.Get(), viewport_width_, viewport_height_, DXGI_FORMAT_R16G16_FLOAT, "Create scene motion target");

    auto rtv = render_target_heap_->GetCPUDescriptorHandleForHeapStart();
    rtv.ptr += static_cast<SIZE_T>(swap_chain_buffer_count) * render_target_descriptor_stride_;
    device_->CreateRenderTargetView(scene_color_target_.Get(), nullptr, rtv);
    rtv.ptr += render_target_descriptor_stride_;
    device_->CreateRenderTargetView(scene_depth_target_.Get(), nullptr, rtv);
    rtv.ptr += render_target_descriptor_stride_;
    device_->CreateRenderTargetView(scene_motion_target_.Get(), nullptr, rtv);

    auto srv = shader_resource_heap_->GetCPUDescriptorHandleForHeapStart();
    D3D12_SHADER_RESOURCE_VIEW_DESC srv_desc{};
    srv_desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv_desc.Texture2D.MipLevels = 1;

    srv_desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    device_->CreateShaderResourceView(scene_color_target_.Get(), &srv_desc, srv);
    srv.ptr += shader_resource_descriptor_stride_;
    srv_desc.Format = DXGI_FORMAT_R32_FLOAT;
    device_->CreateShaderResourceView(scene_depth_target_.Get(), &srv_desc, srv);
    srv.ptr += shader_resource_descriptor_stride_;
    srv_desc.Format = DXGI_FORMAT_R16G16_FLOAT;
    device_->CreateShaderResourceView(scene_motion_target_.Get(), &srv_desc, srv);
}

void Renderer::wait_for_fence(std::uint64_t value) {
    const auto completed = fence_->GetCompletedValue();
    if (completed == UINT64_MAX) throw std::runtime_error("D3D12 device was removed");
    if (value == 0 || completed >= value) return;
    check(fence_->SetEventOnCompletion(value, fence_completion_event_.get()), "Set fence event");
    if (WaitForSingleObject(fence_completion_event_.get(), INFINITE) != WAIT_OBJECT_0) {
        throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), "Wait for GPU");
    }
    if (fence_->GetCompletedValue() == UINT64_MAX) throw std::runtime_error("D3D12 device was removed");
}

}
