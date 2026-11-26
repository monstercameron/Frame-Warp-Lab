#pragma once

#include <windows.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>

namespace frame_warp_lab {

inline constexpr wchar_t window_class_name[] = L"FrameWarpLabWindow";
inline constexpr std::uint32_t runtime_state_version = 5;
inline constexpr std::size_t runtime_event_capacity = 64;
inline constexpr std::size_t runtime_frame_trace_capacity = 256;
inline constexpr std::int32_t max_stress_level = 64;
inline constexpr UINT control_message = WM_APP + 1;

enum class ControlCommand : WPARAM {
    scene_pause = 1,
    scene_resume = 2,
    scene_reset = 3,
    set_diagnostic_view = 4,
    set_camera_path = 5,
    set_stress_level = 6,
    set_overlay_enabled = 7,
};

enum class DiagnosticView : std::int32_t {
    final_color = 0,
    depth = 1,
    motion_vectors = 2,
};

enum class CameraPath : std::int32_t {
    corridor_sway = 0,
    fast_pan = 1,
    occlusion_sweep = 2,
    camera_cut = 3,
};

enum class RuntimeEventType : std::int32_t {
    runtime_started = 1,
    swap_chain_resized = 2,
    runtime_stopping = 3,
    scene_paused = 4,
    scene_resumed = 5,
    scene_reset = 6,
    config_updated = 7,
};

struct RuntimeEvent {
    std::int32_t sequence;
    std::uint32_t timestamp_low;
    std::uint32_t timestamp_high;
    std::int32_t event_type;
    std::int32_t value;
};

struct RuntimeFrameSample {
    std::int64_t sequence;
    std::int64_t display_frame_id;
    std::int64_t source_frame_id;
    std::int64_t present_interval_us;
    std::int64_t source_age_us;
    std::int64_t scene_gpu_us;
    std::int64_t present_gpu_us;
    std::int32_t diagnostic_view;
    std::int32_t camera_path;
    std::int32_t stress_level;
    std::int32_t discontinuity;
};

struct RuntimeState {
    std::uint32_t version;
    std::uint32_t process_id;
    std::int32_t present_count;
    std::int32_t resize_count;
    std::int32_t running;
    std::int32_t scene_paused;
    std::int64_t scene_time_ms;
    std::int64_t source_frame_id;
    std::int64_t display_frame_id;
    std::int64_t last_present_interval_us;
    std::int64_t last_source_age_us;
    std::int64_t last_scene_gpu_us;
    std::int64_t last_present_gpu_us;
    std::int32_t diagnostic_view;
    std::int32_t camera_path;
    std::int32_t stress_level;
    std::int32_t overlay_enabled;
    std::int32_t gpu_timing_available;
    std::int32_t event_count;
    std::int64_t trace_count;
    RuntimeEvent events[runtime_event_capacity];
    RuntimeFrameSample trace[runtime_frame_trace_capacity];
};

static_assert(std::atomic_ref<std::int32_t>::is_always_lock_free);
static_assert(std::atomic_ref<std::uint32_t>::is_always_lock_free);
static_assert(std::atomic_ref<std::int64_t>::is_always_lock_free);

inline const char* runtime_event_name(std::int32_t event_type) {
    switch (static_cast<RuntimeEventType>(event_type)) {
        case RuntimeEventType::runtime_started: return "runtime_started";
        case RuntimeEventType::swap_chain_resized: return "swap_chain_resized";
        case RuntimeEventType::runtime_stopping: return "runtime_stopping";
        case RuntimeEventType::scene_paused: return "scene_paused";
        case RuntimeEventType::scene_resumed: return "scene_resumed";
        case RuntimeEventType::scene_reset: return "scene_reset";
        case RuntimeEventType::config_updated: return "config_updated";
        default: return "unknown";
    }
}

inline const char* diagnostic_view_name(std::int32_t view) {
    switch (static_cast<DiagnosticView>(view)) {
        case DiagnosticView::final_color: return "final";
        case DiagnosticView::depth: return "depth";
        case DiagnosticView::motion_vectors: return "motion";
        default: return "unknown";
    }
}

inline const char* camera_path_name(std::int32_t path) {
    switch (static_cast<CameraPath>(path)) {
        case CameraPath::corridor_sway: return "corridor_sway";
        case CameraPath::fast_pan: return "fast_pan";
        case CameraPath::occlusion_sweep: return "occlusion_sweep";
        case CameraPath::camera_cut: return "camera_cut";
        default: return "unknown";
    }
}

inline std::wstring runtime_mapping_name(DWORD process_id) {
    return L"Local\\FrameWarpLab.State." + std::to_wstring(process_id);
}

}
