#pragma once

#include <d3d12.h>
#include <wrl/client.h>

#include <cstdint>

namespace frame_warp_lab {

struct PbrAssetBundle {
    Microsoft::WRL::ComPtr<ID3D12Resource> albedo_array;
    Microsoft::WRL::ComPtr<ID3D12Resource> normal_array;
    Microsoft::WRL::ComPtr<ID3D12Resource> orm_array;
    Microsoft::WRL::ComPtr<ID3D12Resource> dfg_lut;
    Microsoft::WRL::ComPtr<ID3D12Resource> env_cubemap;
    Microsoft::WRL::ComPtr<ID3D12Resource> hero_albedo;
    Microsoft::WRL::ComPtr<ID3D12Resource> hero_normal;
    Microsoft::WRL::ComPtr<ID3D12Resource> hero_orm;
    bool hero_textures_loaded = false;
    std::int64_t build_time_ms = 0;
};

PbrAssetBundle create_and_upload_pbr_assets(
    ID3D12Device* device,
    ID3D12CommandQueue* upload_queue,
    ID3D12CommandAllocator* command_allocator,
    ID3D12GraphicsCommandList* command_list,
    ID3D12Fence* fence,
    HANDLE fence_event,
    std::uint64_t& next_fence_value,
    D3D12_CPU_DESCRIPTOR_HANDLE srv_heap_start,
    UINT srv_descriptor_stride);

}
