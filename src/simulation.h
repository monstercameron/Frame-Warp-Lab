#pragma once

#include "runtime_state.h"

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
    float yaw_axis;
    float pitch_axis;
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
    }

    void set_path(CameraPath path) {
        if (path_ != path) {
            path_ = path;
            has_previous_ = false;
        }
    }

    CameraPath path() const { return path_; }
    double time_seconds() const { return scene_time_seconds_; }

    void advance(double delta_seconds, bool paused, const ManualCameraInput& input) {
        if (delta_seconds <= 0.0) return;
        if (!paused) {
            scene_time_seconds_ += delta_seconds;
        }
        if (path_ == CameraPath::manual) {
            const float step = static_cast<float>(delta_seconds);
            manual_yaw_ += input.yaw_axis * 1.4f * step;
            manual_pitch_ = std::clamp(manual_pitch_ + input.pitch_axis * 1.1f * step, -0.8f, 0.8f);
            const CameraBasis basis = make_camera_basis(manual_position_, manual_yaw_, manual_pitch_, 1.04719755f);
            const float move_speed = 4.2f * step;
            manual_position_.x = std::clamp(manual_position_.x + (basis.forward.x * input.forward_axis + basis.right.x * input.right_axis) * move_speed, -3.4f, 3.4f);
            manual_position_.z = std::clamp(manual_position_.z + (basis.forward.z * input.forward_axis + basis.right.z * input.right_axis) * move_speed, 0.4f, 24.0f);
        }
    }

    CameraBasis sample_latest_camera(std::int64_t& out_cut_segment) const {
        if (path_ == CameraPath::manual) {
            out_cut_segment = 0;
            return make_camera_basis(manual_position_, manual_yaw_, manual_pitch_, 1.04719755f);
        }
        return evaluate_scripted_camera(scene_time_seconds_, path_, out_cut_segment);
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
    CameraPath path_ = CameraPath::corridor_sway;
    double scene_time_seconds_ = 0.0;
    CameraBasis previous_camera_{};
    Float3 previous_ball_{};
    std::int64_t previous_cut_segment_ = 0;
    bool has_previous_ = false;
    Float3 manual_position_{0.0f, 1.45f, 1.4f};
    float manual_yaw_ = 0.0f;
    float manual_pitch_ = 0.0f;
};

}
