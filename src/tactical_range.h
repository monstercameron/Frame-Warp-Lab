#pragma once

#include "simulation.h"

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace frame_warp_lab {

struct RangeVertex;

inline constexpr std::uint32_t k_range_target_count = 8;
inline constexpr std::uint32_t k_range_casing_count = 12;
inline constexpr std::int32_t k_cs_weapon_count = 6;

enum class CsWeaponId : std::int32_t {
    usp = 0,
    smg = 1,
    rifle = 2,
    scout = 3,
    shotgun = 4,
    dual_pistols = 5,
    count = 6,
};

struct Mat4x4 {
    float m[16];
};

struct alignas(16) RangeInstanceGpu {
    float model[16];
    float prev_model[16];
    float tint_emissive[4];
    std::uint32_t entity_meta[4];
};

enum class RangeMeshKind : std::uint32_t {
    static_world = 0,
    target_plate = 1,
    pistol_frame = 2,
    pistol_slide = 3,
    pistol_barrel = 4,
    pistol_magazine = 5,
    pistol_sights = 6,
    pistol_hammer = 7,
    pistol_trigger = 8,
    muzzle_flash = 9,
    brass_casing = 10,
    bullet_tracer = 11,
    impact_spark = 12,
    transparent_glass = 13,
    weapon_smg = 14,
    weapon_rifle = 15,
    weapon_scout = 16,
    weapon_shotgun = 17,
    weapon_dual_pistol = 18,
    bot_slot_0 = 19,
    bot_slot_1 = 20,
    bot_slot_2 = 21,
    bot_slot_3 = 22,
    bot_slot_4 = 23,
    bot_slot_5 = 24,
    bot_slot_6 = 25,
    bot_slot_7 = 26,
    count = 27,
};

struct RangeDrawCall {
    RangeMeshKind mesh;
    std::uint32_t instance_index;
    bool viewmodel;
};

struct EcsLuaTelemetry {
    std::uint32_t entity_count = 0;
    std::uint32_t renderable_count = 0;
    std::uint32_t physics_body_count = 0;
    std::uint32_t script_count = 0;
    std::uint32_t lua_loaded_scripts = 0;
    std::uint32_t lua_reload_count = 0;
    std::uint32_t lua_error_count = 0;
    std::int64_t lua_last_step_us = 0;
    std::uint64_t events_dispatched = 0;
    std::uint32_t event_subscriptions = 0;
    bool hero_pistol_gltf_loaded = false;
    std::uint32_t mt_worker_threads = 0;
    std::uint32_t mt_jolt_threads = 0;
    std::uint64_t mt_jobs_executed = 0;
    std::int64_t mt_physics_step_us = 0;
    std::int64_t mt_ecs_sync_us = 0;
    float current_spread_mrad = 0.0f;
    float last_shot_offset_mrad = 0.0f;
    bool player_grounded = true;
    bool player_touching_wall = false;
    bool player_crouched = false;
    float player_horizontal_speed = 0.0f;
    std::int32_t player_collision_count = 0;
    Float3 player_feet_pos{0.0f, 0.0f, 1.2f};
    std::int32_t active_weapon_id = 0;
    std::int32_t bots_alive_ct = 0;
    std::int32_t bots_alive_t = 0;
    std::int32_t bot_kills = 0;
    std::int32_t ragdolls_active = 0;
};

struct TacticalHudSnapshot {
    std::int32_t hp;
    std::int32_t armor;
    std::int32_t ammo_in_mag;
    std::int32_t ammo_reserve;
    std::int32_t shots_fired;
    std::int32_t hits;
    std::int32_t headshots;
    std::int32_t score;
    std::int32_t targets_up;
    std::int32_t active_weapon_id;
    float ads_alpha;
    float scope_alpha;
    float crosshair_spread;
    float hit_marker_alpha;
    float hit_marker_headshot;
    float reload_progress;
    float muzzle_flash_alpha;
    Float3 muzzle_world_pos;
    Float3 player_pos;
    float player_yaw_rad;
    std::array<float, k_range_target_count * 4U> target_radar_xy_state;
};

struct TacticalRangeFrame {
    std::array<RangeInstanceGpu, 96> instances;
    std::array<RangeDrawCall, 96> draws;
    std::uint32_t draw_count = 0;
    TacticalHudSnapshot hud{};
    std::vector<RangeVertex> skinned_bot_vertices;
};

struct SkinnedCharacterTemplate;

class TacticalRangeSimulation {
public:
    TacticalRangeSimulation();
    ~TacticalRangeSimulation();

    TacticalRangeSimulation(const TacticalRangeSimulation&) = delete;
    TacticalRangeSimulation& operator=(const TacticalRangeSimulation&) = delete;

    void set_hero_pistol_geometry_meta(
        bool loaded_gltf,
        Float3 slide_offset,
        Float3 hammer_offset,
        Float3 trigger_offset,
        Float3 magazine_offset,
        Float3 muzzle_offset,
        Float3 eject_offset,
        float ads_sight_y);
    void set_arsenal_geometry_meta(
        const std::array<Float3, k_cs_weapon_count>& muzzle_offsets,
        const std::array<Float3, k_cs_weapon_count>& eject_offsets,
        const std::array<float, k_cs_weapon_count>& ads_sight_y,
        const std::array<bool, k_cs_weapon_count>& loaded_flags);
    void set_bot_character_templates(
        const SkinnedCharacterTemplate* ct_template,
        const SkinnedCharacterTemplate* t_template,
        const std::array<std::uint32_t, 8>& bot_vb_offsets,
        const std::array<std::uint32_t, 8>& bot_vb_counts,
        std::uint32_t total_bot_vertices);
    void set_dust2_waypoints(const std::vector<Float3>& waypoints);

    void reset();
    void set_dust2_collision_mesh(
        const std::vector<RangeVertex>& vertices,
        const std::vector<std::uint32_t>& indices,
        std::uint32_t lod0_index_count);
    void set_active_world(bool is_dust2);
    void sync_player_spawn(Float3 eye_pos, float yaw, float pitch, bool is_dust2);
    void set_player_input(const ManualCameraInput& input, float yaw, float pitch, bool manual_active, bool is_dust2);
    Float3 player_camera_position() const;
    void set_ads_requested(bool active);
    void set_firing_held(bool held);
    void request_fire();
    void request_reload();
    void cycle_weapon(std::int32_t delta);
    void select_weapon(std::int32_t weapon_id);
    std::int32_t active_weapon_id() const;
    bool reload_lua_scripts();
    bool emit_named_event(const std::string& event_name);

    void step(double delta_seconds, bool paused, const CameraBasis& camera);
    float current_vertical_fov_radians() const;
    float camera_recoil_pitch_radians() const;
    float camera_recoil_yaw_radians() const;

    void build_render_frame(const CameraBasis& camera, const CameraBasis& previous_camera, TacticalRangeFrame& out_frame);
    TacticalHudSnapshot hud_snapshot(const CameraBasis& camera) const;
    EcsLuaTelemetry ecs_lua_telemetry() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}
