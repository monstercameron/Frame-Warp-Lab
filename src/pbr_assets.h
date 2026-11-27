#pragma once

#include <d3d12.h>
#include <wrl/client.h>

#include <cstdint>
#include <string>
#include <vector>

namespace frame_warp_lab {

struct ArsenalPbrMaterialDesc {
    std::vector<std::uint8_t> albedo_encoded;
    std::vector<std::uint8_t> normal_encoded;
    std::vector<std::uint8_t> orm_encoded;
    float fallback_r = 0.28f;
    float fallback_g = 0.28f;
    float fallback_b = 0.28f;
    float roughness_factor = 0.55f;
    float metallic_factor = 0.12f;
    bool normal_alpha_is_specular = false;
    bool character_fabric_orm = false;
};

struct Dust2PbrBundle {
    Microsoft::WRL::ComPtr<ID3D12Resource> albedo_array;
    Microsoft::WRL::ComPtr<ID3D12Resource> normal_array;
    Microsoft::WRL::ComPtr<ID3D12Resource> orm_array;
    std::uint32_t material_count = 0;
    std::int64_t build_time_ms = 0;
    bool loaded = false;
};

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

Dust2PbrBundle create_and_upload_dust2_pbr_assets(
    ID3D12Device* device,
    ID3D12CommandQueue* upload_queue,
    ID3D12CommandAllocator* command_allocator,
    ID3D12GraphicsCommandList* command_list,
    ID3D12Fence* fence,
    HANDLE fence_event,
    std::uint64_t& next_fence_value,
    const std::vector<std::string>& texture_paths,
    D3D12_CPU_DESCRIPTOR_HANDLE srv_heap_start,
    UINT srv_descriptor_stride);

Dust2PbrBundle create_and_upload_arsenal_pbr_assets(
    ID3D12Device* device,
    ID3D12CommandQueue* upload_queue,
    ID3D12CommandAllocator* command_allocator,
    ID3D12GraphicsCommandList* command_list,
    ID3D12Fence* fence,
    HANDLE fence_event,
    std::uint64_t& next_fence_value,
    const std::vector<ArsenalPbrMaterialDesc>& materials,
    D3D12_CPU_DESCRIPTOR_HANDLE srv_heap_start,
    UINT srv_descriptor_stride);

}
