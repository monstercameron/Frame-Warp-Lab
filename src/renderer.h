#pragma once

#include "runtime_state.h"
#include "simulation.h"

#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <array>
#include <cstdint>

namespace frame_warp_lab {

struct FrameExecutionMetrics {
    std::int64_t completed_scene_gpu_us;
    std::int64_t completed_present_gpu_us;
    bool gpu_timing_available;
};

struct FrameRenderRequest {
    SceneSnapshot snapshot;
    DiagnosticView diagnostic_view;
    std::int32_t stress_level;
    bool overlay_enabled;
    bool render_scene_pass;
};

class Renderer {
public:
    explicit Renderer(HWND window);
    ~Renderer() noexcept;

    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;

    FrameExecutionMetrics render_and_present(const FrameRenderRequest& request);
    void resize(UINT width, UINT height);
    void flush();
    bool gpu_timing_available() const { return gpu_timestamp_frequency_ > 0; }

private:
    static constexpr UINT swap_chain_buffer_count = 2;
    static constexpr UINT offscreen_target_count = 3;
    static constexpr UINT timestamps_per_frame = 4;
    static constexpr std::size_t constant_buffer_stride = 256;

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

    void create_pipelines();
    void create_constant_buffers();
    void create_timestamp_resources();
    void create_swap_chain_targets();
    void create_offscreen_targets();
    void wait_for_fence(std::uint64_t value);

    OsHandle fence_completion_event_;
    Microsoft::WRL::ComPtr<IDXGIFactory4> factory_;
    Microsoft::WRL::ComPtr<ID3D12Device> device_;
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> queue_;
    Microsoft::WRL::ComPtr<IDXGISwapChain3> swap_chain_;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> render_target_heap_;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> shader_resource_heap_;
    std::array<Microsoft::WRL::ComPtr<ID3D12Resource>, swap_chain_buffer_count> render_targets_;
    Microsoft::WRL::ComPtr<ID3D12Resource> scene_color_target_;
    Microsoft::WRL::ComPtr<ID3D12Resource> scene_depth_target_;
    Microsoft::WRL::ComPtr<ID3D12Resource> scene_motion_target_;
    std::array<Microsoft::WRL::ComPtr<ID3D12CommandAllocator>, swap_chain_buffer_count> command_allocators_;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> command_list_;
    Microsoft::WRL::ComPtr<ID3D12Fence> fence_;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> scene_root_signature_;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> scene_pipeline_;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> present_root_signature_;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> present_pipeline_;
    Microsoft::WRL::ComPtr<ID3D12Resource> scene_constant_buffer_;
    Microsoft::WRL::ComPtr<ID3D12Resource> present_constant_buffer_;
    std::uint8_t* mapped_scene_constants_ = nullptr;
    std::uint8_t* mapped_present_constants_ = nullptr;
    Microsoft::WRL::ComPtr<ID3D12QueryHeap> timestamp_query_heap_;
    Microsoft::WRL::ComPtr<ID3D12Resource> timestamp_readback_buffer_;
    const std::uint64_t* mapped_timestamps_ = nullptr;
    std::uint64_t gpu_timestamp_frequency_ = 0;
    std::array<std::uint64_t, swap_chain_buffer_count> buffer_fence_values_{};
    std::array<bool, swap_chain_buffer_count> buffer_has_timestamps_{};
    std::uint64_t next_fence_value_ = 1;
    std::int64_t last_scene_gpu_us_ = -1;
    std::int64_t last_present_gpu_us_ = -1;
    UINT render_target_descriptor_stride_ = 0;
    UINT shader_resource_descriptor_stride_ = 0;
    UINT viewport_width_ = 0;
    UINT viewport_height_ = 0;
};

}
