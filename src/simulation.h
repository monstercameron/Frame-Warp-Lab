#pragma once

#include "runtime_state.h"
#include "simd_math.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace frame_warp_lab {

struct Float3 {
    float x;
    float y;
    float z;
};

struct CameraBasis {
    Float3 position;
    Float3 right;
    Float3 up;
    Float3 forward;
    float tan_half_fov_y;
};

struct ManualCameraInput {
    float forward_axis;
    float right_axis;
    float vertical_axis;
    float yaw_axis;
    float pitch_axis;
    float mouse_delta_x_px;
    float mouse_delta_y_px;
};

struct SceneSnapshot {
    std::int64_t source_frame_id;
    double scene_time_seconds;
    CameraBasis camera;
    CameraBasis previous_camera;
    Float3 ball_center;
    Float3 previous_ball_center;
    std::int64_t cut_segment;
    bool discontinuity;
};

inline CameraBasis make_camera_basis(Float3 position, float yaw_radians, float pitch_radians, float vertical_fov_radians) {
    const float cos_yaw = std::cos(yaw_radians);
    const float sin_yaw = std::sin(yaw_radians);
    const float cos_pitch = std::cos(pitch_radians);
    const float sin_pitch = std::sin(pitch_radians);
    const Float3 forward{sin_yaw * cos_pitch, sin_pitch, cos_yaw * cos_pitch};
    const Float3 right{cos_yaw, 0.0f, -sin_yaw};
    const Float3 up{-sin_yaw * sin_pitch, cos_pitch, -cos_yaw * sin_pitch};
    return CameraBasis{position, right, up, forward, std::tan(vertical_fov_radians * 0.5f)};
}

inline Float3 evaluate_ball_center(double scene_time_seconds, CameraPath path) {
    if (path == CameraPath::fast_pan) {
        const double period = 2.4;
        const double phase = std::fmod(scene_time_seconds, period) / period;
        const double triangle = phase < 0.5 ? (phase * 4.0 - 1.0) : (3.0 - phase * 4.0);
        return Float3{static_cast<float>(2.1 * triangle), 1.05f, 12.2f};
    }
    return Float3{static_cast<float>(1.8 * std::sin(scene_time_seconds * 1.3)), 1.05f, 12.2f};
}

inline CameraBasis evaluate_scripted_camera(double scene_time_seconds, CameraPath path, std::int64_t& out_cut_segment) {
    constexpr float vertical_fov_radians = 1.04719755f;
    out_cut_segment = 0;
    switch (path) {
        case CameraPath::fast_pan: {
            const float yaw = static_cast<float>(0.55 * std::sin(scene_time_seconds * 2.4));
            const float pitch = static_cast<float>(0.08 * std::sin(scene_time_seconds * 1.1));
            const Float3 position{static_cast<float>(0.6 * std::sin(scene_time_seconds * 0.9)), 1.45f, 1.5f};
            return make_camera_basis(position, yaw, pitch, vertical_fov_radians);
        }
        case CameraPath::occlusion_sweep: {
            const float x = static_cast<float>(2.1 * std::sin(scene_time_seconds * 0.85));
            const float yaw = static_cast<float>(-0.18 * std::sin(scene_time_seconds * 0.85));
            const Float3 position{x, 1.45f, 2.2f};
            return make_camera_basis(position, yaw, 0.0f, vertical_fov_radians);
        }
        case CameraPath::camera_cut: {
            out_cut_segment = static_cast<std::int64_t>(std::floor(scene_time_seconds / 1.63));
            const bool alternate_view = (out_cut_segment % 2) != 0;
            if (alternate_view) {
                const float yaw = static_cast<float>(-0.24 + 0.08 * std::sin(scene_time_seconds * 0.6));
                return make_camera_basis(Float3{1.9f, 1.85f, 4.0f}, yaw, -0.06f, vertical_fov_radians);
            }
            const float yaw = static_cast<float>(0.20 + 0.08 * std::sin(scene_time_seconds * 0.6));
            return make_camera_basis(Float3{-1.8f, 1.35f, 1.0f}, yaw, 0.02f, vertical_fov_radians);
        }
        case CameraPath::pure_rotation: {
            const float yaw = static_cast<float>(0.65 * std::sin(scene_time_seconds * 2.2));
            const float pitch = static_cast<float>(0.14 * std::sin(scene_time_seconds * 1.4));
            return make_camera_basis(Float3{0.0f, 1.45f, 1.5f}, yaw, pitch, vertical_fov_radians);
        }
        case CameraPath::corridor_sway:
        default: {
            const float x = static_cast<float>(0.8 * std::sin(scene_time_seconds * 0.45));
            const float z = static_cast<float>(1.2 + 0.55 * std::sin(scene_time_seconds * 0.22));
            const float yaw = static_cast<float>(0.16 * std::sin(scene_time_seconds * 0.35));
            return make_camera_basis(Float3{x, 1.45f, z}, yaw, 0.0f, vertical_fov_radians);
        }
    }
}

class SceneSimulation {
public:
    SceneSimulation() {
        reset();
    }

    void reset() {
        scene_time_seconds_ = 0.0;
        has_previous_ = false;
        previous_cut_segment_ = 0;
        manual_position_ = Float3{0.0f, 1.45f, 1.4f};
        manual_yaw_ = 0.0f;
        manual_pitch_ = 0.0f;
        vertical_fov_override_ = 0.0f;
        recoil_yaw_offset_ = 0.0f;
        recoil_pitch_offset_ = 0.0f;
        manual_min_ = Float3{-3.5f, 0.45f, 0.4f};
        manual_max_ = Float3{3.5f, 3.15f, 24.5f};
        manual_move_speed_ = 4.2f;
        clamp_to_range_bay_ = false;
    }

    void reset_for_tactical_range() {
        scene_time_seconds_ = 0.0;
        has_previous_ = false;
        previous_cut_segment_ = 0;
        path_ = CameraPath::manual;
        manual_position_ = Float3{0.0f, 1.42f, 1.2f};
        manual_yaw_ = 0.0f;
        manual_pitch_ = 0.0f;
        vertical_fov_override_ = 1.15f;
        recoil_yaw_offset_ = 0.0f;
        recoil_pitch_offset_ = 0.0f;
        manual_min_ = Float3{-5.6f, 1.42f, -0.8f};
        manual_max_ = Float3{5.6f, 1.42f, 2.85f};
        manual_move_speed_ = 4.2f;
    }

    void reset_for_dust2(Float3 spawn_pos, float spawn_yaw, Float3 world_min, Float3 world_max) {
        scene_time_seconds_ = 0.0;
        has_previous_ = false;
        previous_cut_segment_ = 0;
        path_ = CameraPath::manual;
        manual_position_ = spawn_pos;
        manual_yaw_ = spawn_yaw;
        manual_pitch_ = 0.0f;
        vertical_fov_override_ = 1.15f;
        recoil_yaw_offset_ = 0.0f;
        recoil_pitch_offset_ = 0.0f;
        manual_min_ = Float3{world_min.x - 8.0f, 0.5f, world_min.z - 8.0f};
        manual_max_ = Float3{world_max.x + 8.0f, world_max.y + 16.0f, world_max.z + 8.0f};
        manual_move_speed_ = 8.5f;
        clamp_to_range_bay_ = false;
    }

    void set_range_camera_modifiers(float vertical_fov_rad, float recoil_pitch_rad, float recoil_yaw_rad, bool clamp_to_bay) {
        vertical_fov_override_ = vertical_fov_rad;
        recoil_pitch_offset_ = recoil_pitch_rad;
        recoil_yaw_offset_ = recoil_yaw_rad;
        clamp_to_range_bay_ = clamp_to_bay;
    }

    void set_path(CameraPath path) {
        if (path_ != path) {
            if (path == CameraPath::manual) {
                sync_manual_pose_from_current();
            }
            path_ = path;
            has_previous_ = false;
        }
    }

    bool take_manual_control_if_needed(const ManualCameraInput& input) {
        const bool has_input = input.forward_axis != 0.0f
            || input.right_axis != 0.0f
            || input.vertical_axis != 0.0f
            || input.yaw_axis != 0.0f
            || input.pitch_axis != 0.0f
            || input.mouse_delta_x_px != 0.0f
            || input.mouse_delta_y_px != 0.0f;
        if (!has_input || path_ == CameraPath::manual) {
            return false;
        }
        sync_manual_pose_from_current();
        path_ = CameraPath::manual;
        has_previous_ = false;
        return true;
    }

    CameraPath path() const { return path_; }
    double time_seconds() const { return scene_time_seconds_; }

    void advance(double delta_seconds, bool paused, const ManualCameraInput& input) {
        if (delta_seconds <= 0.0) return;
        if (!paused) {
            scene_time_seconds_ += delta_seconds;
        }
        if (path_ == CameraPath::manual) {
            constexpr float mouse_sensitivity = 0.0024f;
            const float step = static_cast<float>(delta_seconds);
            const float fov_scale = vertical_fov_override_ > 0.1f ? (vertical_fov_override_ / 1.15f) : 1.0f;
            manual_yaw_ += input.yaw_axis * 1.4f * step + input.mouse_delta_x_px * mouse_sensitivity * fov_scale;
            manual_pitch_ = std::clamp(
                manual_pitch_ + input.pitch_axis * 1.1f * step - input.mouse_delta_y_px * mouse_sensitivity * fov_scale,
                -1.15f,
                1.15f);
            const float active_fov = vertical_fov_override_ > 0.1f ? vertical_fov_override_ : 1.04719755f;
            const CameraBasis basis = make_camera_basis(manual_position_, manual_yaw_, manual_pitch_, active_fov);
            const float move_speed = manual_move_speed_ * step;
            if (clamp_to_range_bay_) {
                simd::advance_clamped_position(
                    manual_position_.x,
                    manual_position_.y,
                    manual_position_.z,
                    (basis.forward.x * input.forward_axis + basis.right.x * input.right_axis) * move_speed,
                    0.0f,
                    (basis.forward.z * input.forward_axis + basis.right.z * input.right_axis) * move_speed,
                    -5.6f,
                    1.42f,
                    -0.8f,
                    5.6f,
                    1.42f,
                    2.85f);
            } else {
                simd::advance_clamped_position(
                    manual_position_.x,
                    manual_position_.y,
                    manual_position_.z,
                    (basis.forward.x * input.forward_axis + basis.right.x * input.right_axis) * move_speed,
                    input.vertical_axis * move_speed,
                    (basis.forward.z * input.forward_axis + basis.right.z * input.right_axis) * move_speed,
                    manual_min_.x,
                    manual_min_.y,
                    manual_min_.z,
                    manual_max_.x,
                    manual_max_.y,
                    manual_max_.z);
            }
        }
    }

    CameraBasis sample_latest_camera(std::int64_t& out_cut_segment) const {
        if (path_ == CameraPath::manual) {
            out_cut_segment = 0;
            const float active_fov = vertical_fov_override_ > 0.1f ? vertical_fov_override_ : 1.04719755f;
            const float pitched = std::clamp(manual_pitch_ + recoil_pitch_offset_, -1.25f, 1.25f);
            return make_camera_basis(manual_position_, manual_yaw_ + recoil_yaw_offset_, pitched, active_fov);
        }
        return evaluate_scripted_camera(scene_time_seconds_, path_, out_cut_segment);
    }

    void current_camera_angles_degrees(float& out_yaw_deg, float& out_pitch_deg) const {
        constexpr float radians_to_degrees = 57.2957795f;
        std::int64_t cut_segment = 0;
        const CameraBasis basis = sample_latest_camera(cut_segment);
        out_yaw_deg = std::atan2(basis.forward.x, basis.forward.z) * radians_to_degrees;
        out_pitch_deg = std::asin(std::clamp(basis.forward.y, -1.0f, 1.0f)) * radians_to_degrees;
    }

    SceneSnapshot capture_source_snapshot(std::int64_t source_frame_id) {
        std::int64_t cut_segment = 0;
        const CameraBasis current_camera = sample_latest_camera(cut_segment);
        const Float3 current_ball = evaluate_ball_center(scene_time_seconds_, path_);
        const bool discontinuity = !has_previous_ || (cut_segment != previous_cut_segment_);
        const CameraBasis previous_camera = discontinuity ? current_camera : previous_camera_;
        const Float3 previous_ball = discontinuity ? current_ball : previous_ball_;
        previous_camera_ = current_camera;
        previous_ball_ = current_ball;
        previous_cut_segment_ = cut_segment;
        has_previous_ = true;
        return SceneSnapshot{
            source_frame_id,
            scene_time_seconds_,
            current_camera,
            previous_camera,
            current_ball,
            previous_ball,
            cut_segment,
            discontinuity,
        };
    }

private:
    void sync_manual_pose_from_current() {
        std::int64_t cut_segment = 0;
        const CameraBasis current = sample_latest_camera(cut_segment);
        manual_position_ = current.position;
        manual_yaw_ = std::atan2(current.forward.x, current.forward.z);
        manual_pitch_ = std::asin(std::clamp(current.forward.y, -1.0f, 1.0f));
    }

    CameraPath path_ = CameraPath::corridor_sway;
    double scene_time_seconds_ = 0.0;
    CameraBasis previous_camera_{};
    Float3 previous_ball_{};
    std::int64_t previous_cut_segment_ = 0;
    bool has_previous_ = false;
    Float3 manual_position_{0.0f, 1.45f, 1.4f};
    float manual_yaw_ = 0.0f;
    float manual_pitch_ = 0.0f;
    float vertical_fov_override_ = 0.0f;
    float recoil_yaw_offset_ = 0.0f;
    float recoil_pitch_offset_ = 0.0f;
    Float3 manual_min_{-3.5f, 0.45f, 0.4f};
    Float3 manual_max_{3.5f, 3.15f, 24.5f};
    float manual_move_speed_ = 4.2f;
    bool clamp_to_range_bay_ = false;
};

}
