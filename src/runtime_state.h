#pragma once

#include <windows.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>

namespace frame_warp_lab {

inline constexpr wchar_t window_class_name[] = L"FrameWarpLabWindow";
inline constexpr std::uint32_t runtime_state_version = 4;
inline constexpr std::size_t runtime_event_capacity = 64;
inline constexpr UINT scene_control_message = WM_APP + 1;
inline constexpr WPARAM scene_pause = 1;
inline constexpr WPARAM scene_resume = 2;
inline constexpr WPARAM scene_reset = 3;

enum class RuntimeEventType : std::int32_t {
    runtime_started = 1,
    swap_chain_resized = 2,
    runtime_stopping = 3,
    scene_paused = 4,
    scene_resumed = 5,
    scene_reset = 6,
};

struct RuntimeEvent {
    std::int32_t sequence;
    std::uint32_t timestamp_low;
    std::uint32_t timestamp_high;
    std::int32_t event_type;
    std::int32_t value;
};

struct RuntimeState {
    std::uint32_t version;
    std::uint32_t process_id;
    std::int32_t present_count;
    std::int32_t resize_count;
    std::int32_t running;
    std::int32_t scene_paused;
    std::int64_t scene_time_ms;
    std::int32_t event_count;
    std::int32_t reserved;
    RuntimeEvent events[runtime_event_capacity];
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
        default: return "unknown";
    }
}

inline std::wstring runtime_mapping_name(DWORD process_id) {
    return L"Local\\FrameWarpLab.State." + std::to_wstring(process_id);
}

}
