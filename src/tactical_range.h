#pragma once

#include "simulation.h"

#include <array>
#include <cstdint>
#include <memory>

namespace frame_warp_lab {

inline constexpr std::uint32_t k_range_target_count = 8;
inline constexpr std::uint32_t k_range_casing_count = 12;

struct Mat4x4 {
    float m[16];
};

struct alignas(16) RangeInstanceGpu {
    float model[16];
    float prev_model[16];
    float tint_emissive[4];
};

enum class RangeMeshKind : std::uint32_t {
    static_world = 0,
    target_plate = 1,
    pistol_frame = 2,
    pistol_slide = 3,
    pistol_barrel = 4,
    pistol_magazine = 5,
    pistol_sights = 6,
    muzzle_flash = 7,
    brass_casing = 8,
    bullet_tracer = 9,
    impact_spark = 10,
    count = 11,
};

struct RangeDrawCall {
    RangeMeshKind mesh;
    std::uint32_t instance_index;
    bool viewmodel;
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
    float ads_alpha;
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
    std::array<RangeInstanceGpu, 48> instances;
    std::array<RangeDrawCall, 48> draws;
    std::uint32_t draw_count = 0;
    TacticalHudSnapshot hud{};
};

class TacticalRangeSimulation {
public:
    TacticalRangeSimulation();
    ~TacticalRangeSimulation();

    TacticalRangeSimulation(const TacticalRangeSimulation&) = delete;
    TacticalRangeSimulation& operator=(const TacticalRangeSimulation&) = delete;

    void reset();
    void set_ads_requested(bool active);
    void request_fire();
    void request_reload();

    void step(double delta_seconds, bool paused, const CameraBasis& camera);
    float current_vertical_fov_radians() const;
    float camera_recoil_pitch_radians() const;
    float camera_recoil_yaw_radians() const;

    void build_render_frame(const CameraBasis& camera, const CameraBasis& previous_camera, TacticalRangeFrame& out_frame);
    TacticalHudSnapshot hud_snapshot(const CameraBasis& camera) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}
