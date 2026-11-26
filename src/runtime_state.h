#pragma once

#include <windows.h>

#include <cstdint>
#include <cstddef>
#include <string>

namespace frame_warp_lab {

inline constexpr wchar_t window_class_name[] = L"FrameWarpLabWindow";
inline constexpr std::uint32_t runtime_state_version = 3;
inline constexpr std::size_t runtime_event_capacity = 64;
inline constexpr UINT scene_control_message = WM_APP + 1;
inline constexpr WPARAM scene_pause = 1;
inline constexpr WPARAM scene_resume = 2;
inline constexpr WPARAM scene_reset = 3;

struct RuntimeEvent {
    volatile LONG sequence;
    DWORD timestamp_low;
    DWORD timestamp_high;
    LONG event_type;
    LONG value;
};

struct RuntimeState {
    std::uint32_t version;
    std::uint32_t process_id;
    volatile LONG present_count;
    volatile LONG resize_count;
    volatile LONG running;
    volatile LONG scene_time_ms;
    volatile LONG scene_paused;
    volatile LONG event_count;
    RuntimeEvent events[runtime_event_capacity];
};

inline std::wstring runtime_mapping_name(DWORD process_id) {
    return L"Local\\FrameWarpLab.State." + std::to_wstring(process_id);
}

}
