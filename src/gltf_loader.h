#pragma once

#include "range_mesh.h"

#include <filesystem>
#include <string>
#include <vector>

namespace frame_warp_lab {

struct LoadedGltfMesh {
    std::string node_name;
    std::vector<RangeVertex> vertices;
    std::vector<std::uint32_t> indices;
    Float3 node_translation{0.0f, 0.0f, 0.0f};
    Float3 min_bounds{0.0f, 0.0f, 0.0f};
    Float3 max_bounds{0.0f, 0.0f, 0.0f};
};

struct LoadedGltfAsset {
    bool loaded = false;
    std::string source_path;
    std::vector<LoadedGltfMesh> meshes;
};

std::filesystem::path resolve_asset_path(const std::filesystem::path& relative_path);

LoadedGltfAsset load_gltf_meshes(
    const std::filesystem::path& relative_gltf_path,
    std::uint32_t material_id,
    bool gltf_x_forward_to_engine_z_forward,
    bool bake_node_translation);

struct LoadedWeaponMesh {
    bool loaded = false;
    std::vector<RangeVertex> vertices;
    std::vector<std::uint32_t> indices;
    Float3 muzzle_offset{0.0f, 0.05f, 0.35f};
    Float3 eject_offset{0.025f, 0.05f, 0.06f};
    float ads_sight_y = 0.08f;
};

enum class WeaponAxisMode : std::uint8_t {
    z_pos = 0,
    z_neg = 1,
    x_pos = 2,
    x_neg = 3,
};

LoadedWeaponMesh load_weapon_glb_merged(
    const std::filesystem::path& relative_glb_path,
    float target_length_m,
    WeaponAxisMode axis_mode,
    bool use_world_transform,
    bool skip_arms,
    std::vector<ArsenalPbrMaterialDesc>& out_materials);

}
