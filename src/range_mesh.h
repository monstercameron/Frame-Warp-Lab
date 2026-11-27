#pragma once

#include "tactical_range.h"
#include "skinning.h"

#include <array>
#include <cstdint>
#include <vector>

namespace frame_warp_lab {

struct RangeVertex {
    float px, py, pz;
    float nx, ny, nz;
    float tx, ty, tz;
    float u, v;
    std::uint32_t material_id;
    float emissive_weight;
};

struct RangeSubmesh {
    std::uint32_t index_offset;
    std::uint32_t index_count;
};

struct RangeGeometryPackage {
    std::vector<RangeVertex> vertices;
    std::vector<std::uint32_t> indices;
    std::array<RangeSubmesh, static_cast<std::size_t>(RangeMeshKind::count)> submeshes{};
    bool hero_pistol_gltf_loaded = false;
    Float3 pistol_slide_offset{0.0f, 0.0f, 0.0f};
    Float3 pistol_hammer_offset{0.0f, 0.0f, 0.0f};
    Float3 pistol_trigger_offset{0.0f, 0.0f, 0.0f};
    Float3 pistol_magazine_offset{0.0f, 0.0f, 0.0f};
    Float3 pistol_muzzle_offset{0.0f, 0.042f, 0.145f};
    Float3 pistol_eject_offset{0.028f, 0.050f, 0.025f};
    float pistol_ads_sight_y = 0.068f;
    std::array<Float3, k_cs_weapon_count> weapon_muzzle_offsets{};
    std::array<Float3, k_cs_weapon_count> weapon_eject_offsets{};
    std::array<float, k_cs_weapon_count> weapon_ads_sight_y{};
    std::array<bool, k_cs_weapon_count> weapon_loaded{};
    SkinnedCharacterTemplate ct_bot_template{};
    SkinnedCharacterTemplate t_bot_template{};
    std::uint32_t bots_vb_base_vertex = 0U;
    std::uint32_t bots_total_vertices = 0U;
    std::array<std::uint32_t, 8> bot_vb_offsets{};
    std::array<std::uint32_t, 8> bot_vb_counts{};
};

RangeGeometryPackage build_tactical_range_geometry();

}
