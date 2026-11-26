#pragma once

#include <windows.h>

#include <cstdint>
#include <string>

namespace frame_warp_lab {

inline constexpr wchar_t window_class_name[] = L"FrameWarpLabWindow";
inline constexpr std::uint32_t runtime_state_version = 1;

struct RuntimeState {
    std::uint32_t version;
    std::uint32_t process_id;
    volatile LONG present_count;
    volatile LONG resize_count;
    volatile LONG running;
};

inline std::wstring runtime_mapping_name(DWORD process_id) {
    return L"Local\\FrameWarpLab.State." + std::to_wstring(process_id);
}

}
