#pragma once

#include "tactical_range.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace frame_warp_lab {

struct RangeVertex;
struct ArsenalPbrMaterialDesc;

inline constexpr std::uint32_t k_inline_pbr_material_flag = 0x80000000U;
inline constexpr std::uint32_t k_arsenal_pbr_material_flag = 0x40000000U;
inline constexpr std::uint32_t k_max_cs_bots = 8U;
inline constexpr std::uint32_t k_ragdoll_limb_count = 11U;

struct RagdollLimbPose {
    Float3 pos{0.0f, 0.0f, 0.0f};
    float qx = 0.0f;
    float qy = 0.0f;
    float qz = 0.0f;
    float qw = 1.0f;
};

inline std::uint32_t pack_inline_pbr_material(float lin_r, float lin_g, float lin_b, float roughness, float metallic) {
    auto to_srgb_u8 = [](float c) -> std::uint32_t {
        const float cl = c < 0.0f ? 0.0f : (c > 1.0f ? 1.0f : c);
        const float sr = std::pow(cl, 1.0f / 2.2f);
        const int v = static_cast<int>(sr * 255.0f + 0.5f);
        return static_cast<std::uint32_t>(v < 0 ? 0 : (v > 255 ? 255 : v));
    };
    const std::uint32_t r8 = to_srgb_u8(lin_r);
    const std::uint32_t g8 = to_srgb_u8(lin_g);
    const std::uint32_t b8 = to_srgb_u8(lin_b);
    const float rn = (roughness - 0.10f) / 0.85f;
    const int r4i = static_cast<int>((rn < 0.0f ? 0.0f : (rn > 1.0f ? 1.0f : rn)) * 15.0f + 0.5f);
    const int m3i = static_cast<int>((metallic < 0.0f ? 0.0f : (metallic > 1.0f ? 1.0f : metallic)) * 7.0f + 0.5f);
    const std::uint32_t r4 = static_cast<std::uint32_t>(r4i < 0 ? 0 : (r4i > 15 ? 15 : r4i));
    const std::uint32_t m3 = static_cast<std::uint32_t>(m3i < 0 ? 0 : (m3i > 7 ? 7 : m3i));
    return k_inline_pbr_material_flag | r8 | (g8 << 8U) | (b8 << 16U) | (r4 << 24U) | (m3 << 28U);
}

struct Quat4 {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    float w = 1.0f;
};

struct RigNode {
    std::string name;
    std::int32_t parent = -1;
    Float3 rest_t{0.0f, 0.0f, 0.0f};
    Quat4 rest_r{0.0f, 0.0f, 0.0f, 1.0f};
    Float3 rest_s{1.0f, 1.0f, 1.0f};
};

enum class AnimPathKind : std::uint8_t {
    translation = 0,
    rotation = 1,
    scale = 2,
};

struct AnimChannel {
    std::int32_t node_index = -1;
    AnimPathKind path = AnimPathKind::translation;
    std::vector<float> times;
    std::vector<float> values;
};

struct AnimClip {
    std::string name;
    float duration = 1.0f;
    std::vector<AnimChannel> channels;
};

struct SkinnedSourceVertex {
    Float3 pos;
    Float3 normal;
    Float3 tangent;
    float u;
    float v;
    std::uint32_t material_id;
    float emissive;
    std::uint16_t joints[4];
    float weights[4];
    std::int32_t rigid_node_index;
};

struct ValveBipedBones {
    std::int32_t pelvis = -1;
    std::int32_t spine0 = -1;
    std::int32_t spine1 = -1;
    std::int32_t spine2 = -1;
    std::int32_t spine3 = -1;
    std::int32_t neck = -1;
    std::int32_t head = -1;
    std::int32_t clavicle_l = -1;
    std::int32_t arm_upper_l = -1;
    std::int32_t arm_lower_l = -1;
    std::int32_t hand_l = -1;
    std::int32_t clavicle_r = -1;
    std::int32_t arm_upper_r = -1;
    std::int32_t arm_lower_r = -1;
    std::int32_t hand_r = -1;
    std::int32_t leg_upper_l = -1;
    std::int32_t leg_lower_l = -1;
    std::int32_t ankle_l = -1;
    std::int32_t leg_upper_r = -1;
    std::int32_t leg_lower_r = -1;
    std::int32_t ankle_r = -1;
};

struct SkinnedCharacterTemplate {
    bool loaded = false;
    bool is_ct_fbi = false;
    bool procedural_valve_biped = false;
    std::string source_path;
    float root_scale = 1.0f;
    float y_offset = 0.0f;
    float pitch_correction = 0.0f;
    float yaw_correction = 0.0f;
    std::int32_t head_node_index = -1;
    std::int32_t torso_node_index = -1;
    std::int32_t weapon_node_index = -1;
    ValveBipedBones biped{};
    std::vector<RigNode> nodes;
    std::vector<Quat4> rest_world_rot;
    std::vector<Mat4x4> rest_node_world;
    std::vector<std::uint8_t> node_ragdoll_limb;
    std::array<std::int32_t, k_ragdoll_limb_count> ragdoll_primary_nodes{};
    std::array<Float3, k_ragdoll_limb_count> ragdoll_rest_center{};
    std::array<Float3, k_ragdoll_limb_count> ragdoll_rest_joint_pos{};
    std::vector<std::int32_t> joint_nodes;
    std::vector<Mat4x4> inverse_bind_matrices;
    std::vector<AnimClip> clips;
    std::vector<SkinnedSourceVertex> source_vertices;
    std::vector<std::uint32_t> indices;

    std::int32_t find_clip(const std::string& clip_name) const;
};

struct SkinnedPoseParams {
    std::int32_t clip_index = 0;
    float clip_time = 0.0f;
    bool loop_clip = true;
    std::int32_t prev_clip_index = -1;
    float prev_clip_time = 0.0f;
    float blend_alpha = 1.0f;
    float aim_pitch_rad = 0.0f;
};

SkinnedCharacterTemplate load_skinned_character_glb(
    const std::filesystem::path& relative_glb_path,
    bool is_ct_fbi,
    std::vector<ArsenalPbrMaterialDesc>& out_materials);

void evaluate_skinned_character(
    const SkinnedCharacterTemplate& tpl,
    const SkinnedPoseParams& pose,
    RangeVertex* out_vertices,
    Float3& out_head_local,
    Float3& out_chest_local,
    Float3& out_muzzle_local,
    std::array<RagdollLimbPose, k_ragdoll_limb_count>* out_limb_poses = nullptr,
    Float3* out_hand_r_local = nullptr);

void evaluate_ragdoll_skinned_character(
    const SkinnedCharacterTemplate& tpl,
    const std::array<RagdollLimbPose, k_ragdoll_limb_count>& world_limb_poses,
    RangeVertex* out_vertices,
    Float3& out_head_world,
    Float3& out_chest_world,
    Float3& out_hand_r_world);

}
