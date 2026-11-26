#include "frame_pacer.h"
#include "latency_monitor.h"
#include "renderer.h"
#include "runtime_state.h"
#include "simulation.h"

#include <windows.h>
#include <windowsx.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <system_error>

namespace {

class Handle {
public:
    Handle(HANDLE value, const char* operation) : value_(value) {
        if (!value_) throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), operation);
    }
    ~Handle() { CloseHandle(value_); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    HANDLE get() const { return value_; }
private:
    HANDLE value_;
};

class RuntimeStatePublisher {
public:
    RuntimeStatePublisher(bool gpu_timing_available, bool allow_tearing_enabled, bool waitable_swap_chain, bool raw_input_enabled)
        : mapping_(CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
                                      sizeof(frame_warp_lab::RuntimeState),
                                      frame_warp_lab::runtime_mapping_name(GetCurrentProcessId()).c_str()),
                   "Create runtime state mapping") {
        state_ = static_cast<frame_warp_lab::RuntimeState*>(MapViewOfFile(mapping_.get(), FILE_MAP_ALL_ACCESS, 0, 0, sizeof(frame_warp_lab::RuntimeState)));
        if (!state_) throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), "Map runtime state");
        std::memset(state_, 0, sizeof(frame_warp_lab::RuntimeState));
        std::atomic_ref<std::uint32_t>(state_->process_id).store(GetCurrentProcessId(), std::memory_order_relaxed);
        std::atomic_ref<std::int32_t>(state_->running).store(1, std::memory_order_relaxed);
        std::atomic_ref<std::int32_t>(state_->overlay_enabled).store(1, std::memory_order_relaxed);
        std::atomic_ref<std::int32_t>(state_->source_divider).store(1, std::memory_order_relaxed);
        std::atomic_ref<std::int32_t>(state_->allow_tearing_enabled).store(allow_tearing_enabled ? 1 : 0, std::memory_order_relaxed);
        std::atomic_ref<std::int32_t>(state_->waitable_swap_chain).store(waitable_swap_chain ? 1 : 0, std::memory_order_relaxed);
        std::atomic_ref<std::int32_t>(state_->raw_input_enabled).store(raw_input_enabled ? 1 : 0, std::memory_order_relaxed);
        std::atomic_ref<std::int32_t>(state_->gpu_timing_available).store(gpu_timing_available ? 1 : 0, std::memory_order_relaxed);
        std::atomic_ref<std::int64_t>(state_->last_input_to_present_us).store(-1, std::memory_order_relaxed);
        std::atomic_ref<std::int64_t>(state_->last_scene_gpu_us).store(-1, std::memory_order_relaxed);
        std::atomic_ref<std::int64_t>(state_->last_present_gpu_us).store(-1, std::memory_order_relaxed);
        std::atomic_ref<std::uint32_t>(state_->version).store(frame_warp_lab::runtime_state_version, std::memory_order_release);
        record_event(frame_warp_lab::RuntimeEventType::runtime_started, 0);
    }

    ~RuntimeStatePublisher() {
        record_event(frame_warp_lab::RuntimeEventType::runtime_stopping, 0);
        std::atomic_ref<std::int32_t>(state_->running).store(0, std::memory_order_release);
        UnmapViewOfFile(state_);
    }

    void update_config(
        frame_warp_lab::PresentationMode presentation_mode,
        frame_warp_lab::PacerMode pacer_mode,
        std::int32_t target_fps,
        std::int64_t target_period_us,
        std::int32_t source_divider,
        bool vsync_enabled,
        frame_warp_lab::DiagnosticView view,
        frame_warp_lab::CameraPath path,
        std::int32_t stress_level,
        bool overlay_enabled,
        bool flash_indicator_enabled) {
        std::atomic_ref<std::int32_t>(state_->presentation_mode).store(static_cast<std::int32_t>(presentation_mode), std::memory_order_release);
        std::atomic_ref<std::int32_t>(state_->pacer_mode).store(static_cast<std::int32_t>(pacer_mode), std::memory_order_release);
        std::atomic_ref<std::int32_t>(state_->target_fps).store(target_fps, std::memory_order_release);
        std::atomic_ref<std::int64_t>(state_->target_period_us).store(target_period_us, std::memory_order_release);
        std::atomic_ref<std::int32_t>(state_->source_divider).store(source_divider, std::memory_order_release);
        std::atomic_ref<std::int32_t>(state_->vsync_enabled).store(vsync_enabled ? 1 : 0, std::memory_order_release);
        std::atomic_ref<std::int32_t>(state_->diagnostic_view).store(static_cast<std::int32_t>(view), std::memory_order_release);
        std::atomic_ref<std::int32_t>(state_->camera_path).store(static_cast<std::int32_t>(path), std::memory_order_release);
        std::atomic_ref<std::int32_t>(state_->stress_level).store(stress_level, std::memory_order_release);
        std::atomic_ref<std::int32_t>(state_->overlay_enabled).store(overlay_enabled ? 1 : 0, std::memory_order_release);
        std::atomic_ref<std::int32_t>(state_->flash_indicator_enabled).store(flash_indicator_enabled ? 1 : 0, std::memory_order_release);
    }

    void update_scene(
        double scene_time_seconds,
        bool paused,
        bool mouse_look_active,
        float yaw_deg,
        float pitch_deg,
        const frame_warp_lab::LatencyMonitor& latency_monitor) {
        std::atomic_ref<std::int64_t>(state_->scene_time_ms).store(static_cast<std::int64_t>(scene_time_seconds * 1000.0), std::memory_order_release);
        std::atomic_ref<std::int32_t>(state_->scene_paused).store(paused ? 1 : 0, std::memory_order_release);
        std::atomic_ref<std::int32_t>(state_->mouse_look_active).store(mouse_look_active ? 1 : 0, std::memory_order_release);
        std::atomic_ref<std::int32_t>(state_->camera_yaw_millideg).store(static_cast<std::int32_t>(yaw_deg * 1000.0f), std::memory_order_release);
        std::atomic_ref<std::int32_t>(state_->camera_pitch_millideg).store(static_cast<std::int32_t>(pitch_deg * 1000.0f), std::memory_order_release);
        std::atomic_ref<std::int32_t>(state_->pcl_etw_listener_active).store(latency_monitor.etw_listener_active() ? 1 : 0, std::memory_order_release);
        std::atomic_ref<std::int64_t>(state_->pcl_markers_emitted).store(latency_monitor.markers_emitted(), std::memory_order_release);
        std::atomic_ref<std::int64_t>(state_->pcl_pings_received).store(latency_monitor.pings_received(), std::memory_order_release);
    }

    void record_frame(const frame_warp_lab::RuntimeFrameSample& sample, std::int64_t smoothed_present_interval_us) {
        std::atomic_ref<std::int32_t>(state_->present_count).fetch_add(1, std::memory_order_release);
        std::atomic_ref<std::int64_t>(state_->display_frame_id).store(sample.display_frame_id, std::memory_order_release);
        std::atomic_ref<std::int64_t>(state_->source_frame_id).store(sample.source_frame_id, std::memory_order_release);

        switch (static_cast<frame_warp_lab::FrameKind>(sample.frame_kind)) {
            case frame_warp_lab::FrameKind::rendered:
                std::atomic_ref<std::int64_t>(state_->rendered_frame_count).fetch_add(1, std::memory_order_release);
                break;
            case frame_warp_lab::FrameKind::repeated:
                std::atomic_ref<std::int64_t>(state_->repeated_frame_count).fetch_add(1, std::memory_order_release);
                break;
            case frame_warp_lab::FrameKind::warped:
                std::atomic_ref<std::int64_t>(state_->warped_frame_count).fetch_add(1, std::memory_order_release);
                break;
            case frame_warp_lab::FrameKind::fallback:
                std::atomic_ref<std::int64_t>(state_->fallback_frame_count).fetch_add(1, std::memory_order_release);
                break;
        }
        if (sample.missed_deadline) {
            std::atomic_ref<std::int64_t>(state_->missed_deadline_count).fetch_add(1, std::memory_order_release);
        }

        std::atomic_ref<std::int64_t>(state_->last_present_interval_us).store(sample.present_interval_us, std::memory_order_release);
        std::atomic_ref<std::int64_t>(state_->smoothed_present_interval_us).store(smoothed_present_interval_us, std::memory_order_release);
        std::atomic_ref<std::int64_t>(state_->last_source_age_us).store(sample.source_age_us, std::memory_order_release);
        if (sample.input_to_present_us >= 0) {
            std::atomic_ref<std::int64_t>(state_->last_input_to_present_us).store(sample.input_to_present_us, std::memory_order_release);
        }
        std::atomic_ref<std::int64_t>(state_->last_sim_cpu_us).store(sample.sim_cpu_us, std::memory_order_release);
        std::atomic_ref<std::int64_t>(state_->last_render_submit_cpu_us).store(sample.render_submit_cpu_us, std::memory_order_release);
        std::atomic_ref<std::int64_t>(state_->last_present_call_cpu_us).store(sample.present_call_cpu_us, std::memory_order_release);
        if (sample.scene_gpu_us >= 0) {
            std::atomic_ref<std::int64_t>(state_->last_scene_gpu_us).store(sample.scene_gpu_us, std::memory_order_release);
        }
        if (sample.present_gpu_us >= 0) {
            std::atomic_ref<std::int64_t>(state_->last_present_gpu_us).store(sample.present_gpu_us, std::memory_order_release);
        }
        std::atomic_ref<std::uint32_t>(state_->last_invalid_pixel_count).store(sample.invalid_pixel_count, std::memory_order_release);
        std::atomic_ref<std::uint32_t>(state_->last_total_pixel_count).store(sample.total_pixel_count, std::memory_order_release);

        const std::int64_t sequence = std::atomic_ref<std::int64_t>(state_->trace_count).load(std::memory_order_relaxed) + 1;
        auto& slot = state_->trace[(static_cast<std::size_t>(sequence) - 1) % frame_warp_lab::runtime_frame_trace_capacity];
        std::atomic_ref<std::int64_t>(slot.sequence).store(0, std::memory_order_release);
        std::atomic_thread_fence(std::memory_order_release);
        slot.display_frame_id = sample.display_frame_id;
        slot.source_frame_id = sample.source_frame_id;
        slot.present_interval_us = sample.present_interval_us;
        slot.source_age_us = sample.source_age_us;
        slot.input_to_present_us = sample.input_to_present_us;
        slot.sim_cpu_us = sample.sim_cpu_us;
        slot.render_submit_cpu_us = sample.render_submit_cpu_us;
        slot.present_call_cpu_us = sample.present_call_cpu_us;
        slot.scene_gpu_us = sample.scene_gpu_us;
        slot.present_gpu_us = sample.present_gpu_us;
        slot.invalid_pixel_count = sample.invalid_pixel_count;
        slot.total_pixel_count = sample.total_pixel_count;
        slot.presentation_mode = sample.presentation_mode;
        slot.frame_kind = sample.frame_kind;
        slot.fallback_reason = sample.fallback_reason;
        slot.diagnostic_view = sample.diagnostic_view;
        slot.camera_path = sample.camera_path;
        slot.stress_level = sample.stress_level;
        slot.source_divider = sample.source_divider;
        slot.source_rendered = sample.source_rendered;
        slot.gpu_slot_warped = sample.gpu_slot_warped;
        slot.missed_deadline = sample.missed_deadline;
        slot.discontinuity = sample.discontinuity;
        std::atomic_ref<std::int64_t>(slot.sequence).store(sequence, std::memory_order_release);
        std::atomic_ref<std::int64_t>(state_->trace_count).store(sequence, std::memory_order_release);
    }

    void record_resize() {
        const std::int32_t resize_count = std::atomic_ref<std::int32_t>(state_->resize_count).fetch_add(1, std::memory_order_acq_rel) + 1;
        record_event(frame_warp_lab::RuntimeEventType::swap_chain_resized, resize_count);
    }

    void record_event(frame_warp_lab::RuntimeEventType event_type, std::int32_t value) {
        const std::int32_t event_sequence = std::atomic_ref<std::int32_t>(state_->event_count).load(std::memory_order_relaxed) + 1;
        auto& event = state_->events[(static_cast<std::size_t>(event_sequence) - 1) % frame_warp_lab::runtime_event_capacity];
        std::atomic_ref<std::int32_t>(event.sequence).store(0, std::memory_order_release);
        std::atomic_thread_fence(std::memory_order_release);
        FILETIME timestamp{};
        GetSystemTimeAsFileTime(&timestamp);
        event.timestamp_low = timestamp.dwLowDateTime;
        event.timestamp_high = timestamp.dwHighDateTime;
        event.event_type = static_cast<std::int32_t>(event_type);
        event.value = value;
        std::atomic_ref<std::int32_t>(event.sequence).store(event_sequence, std::memory_order_release);
        std::atomic_ref<std::int32_t>(state_->event_count).store(event_sequence, std::memory_order_release);
    }

private:
    Handle mapping_;
    frame_warp_lab::RuntimeState* state_ = nullptr;
};

struct WindowInputState {
    bool left_drag_active = false;
    bool mouse_look_locked = false;
    bool cursor_hidden = false;
    bool raw_input_registered = false;
    bool raw_input_received_this_frame = false;
    bool flash_trigger_pending = false;
    bool has_input_timestamp = false;
    std::chrono::steady_clock::time_point earliest_input_time{};
    int last_cursor_x = 0;
    int last_cursor_y = 0;
    float pending_mouse_delta_x = 0.0f;
    float pending_mouse_delta_y = 0.0f;
};

void stamp_input_arrival(WindowInputState& input) {
    if (!input.has_input_timestamp) {
        input.earliest_input_time = std::chrono::steady_clock::now();
        input.has_input_timestamp = true;
    }
}

void set_mouse_look_locked(HWND window, WindowInputState& input, bool locked) {
    if (input.mouse_look_locked == locked) return;
    input.mouse_look_locked = locked;
    if (locked) {
        input.left_drag_active = false;
        SetCapture(window);
        RECT client{};
        if (GetClientRect(window, &client)) {
            POINT top_left{client.left, client.top};
            POINT bottom_right{client.right, client.bottom};
            ClientToScreen(window, &top_left);
            ClientToScreen(window, &bottom_right);
            RECT screen_rect{top_left.x, top_left.y, bottom_right.x, bottom_right.y};
            ClipCursor(&screen_rect);
            POINT center{(client.right - client.left) / 2, (client.bottom - client.top) / 2};
            input.last_cursor_x = center.x;
            input.last_cursor_y = center.y;
            ClientToScreen(window, &center);
            SetCursorPos(center.x, center.y);
        }
        if (!input.cursor_hidden) {
            ShowCursor(FALSE);
            input.cursor_hidden = true;
        }
    } else {
        ClipCursor(nullptr);
        if (GetCapture() == window) ReleaseCapture();
        if (input.cursor_hidden) {
            ShowCursor(TRUE);
            input.cursor_hidden = false;
        }
    }
}

frame_warp_lab::ManualCameraInput consume_manual_camera_input(
    HWND window,
    WindowInputState& input_state,
    bool& out_has_input_timestamp,
    std::chrono::steady_clock::time_point& out_earliest_input_time,
    bool& out_flash_triggered) {
    frame_warp_lab::ManualCameraInput input{};
    input.mouse_delta_x_px = input_state.pending_mouse_delta_x;
    input.mouse_delta_y_px = input_state.pending_mouse_delta_y;
    input_state.pending_mouse_delta_x = 0.0f;
    input_state.pending_mouse_delta_y = 0.0f;
    input_state.raw_input_received_this_frame = false;
    out_has_input_timestamp = input_state.has_input_timestamp;
    out_earliest_input_time = input_state.earliest_input_time;
    input_state.has_input_timestamp = false;
    out_flash_triggered = input_state.flash_trigger_pending;
    input_state.flash_trigger_pending = false;

    if (GetForegroundWindow() != window) return input;
    auto key_down = [](int virtual_key) -> bool { return (GetAsyncKeyState(virtual_key) & 0x8000) != 0; };
    if (key_down('W')) input.forward_axis += 1.0f;
    if (key_down('S')) input.forward_axis -= 1.0f;
    if (key_down('D')) input.right_axis += 1.0f;
    if (key_down('A')) input.right_axis -= 1.0f;
    if (key_down('E')) input.vertical_axis += 1.0f;
    if (key_down('Q')) input.vertical_axis -= 1.0f;
    if (key_down(VK_RIGHT)) input.yaw_axis += 1.0f;
    if (key_down(VK_LEFT)) input.yaw_axis -= 1.0f;
    if (key_down(VK_UP)) input.pitch_axis += 1.0f;
    if (key_down(VK_DOWN)) input.pitch_axis -= 1.0f;
    return input;
}

const char* short_mode_label(frame_warp_lab::PresentationMode mode) {
    switch (mode) {
        case frame_warp_lab::PresentationMode::full_render: return "FULL";
        case frame_warp_lab::PresentationMode::repeat: return "REPEAT";
        case frame_warp_lab::PresentationMode::reproject: return "REPROJ";
        default: return "UNK";
    }
}

const char* short_pacer_label(frame_warp_lab::PacerMode mode) {
    switch (mode) {
        case frame_warp_lab::PacerMode::deadline: return "LOCK";
        case frame_warp_lab::PacerMode::vsync_only: return "VSYNC";
        case frame_warp_lab::PacerMode::unlocked: return "FREE";
        default: return "UNK";
    }
}

const char* short_path_label(frame_warp_lab::CameraPath path) {
    switch (path) {
        case frame_warp_lab::CameraPath::corridor_sway: return "SWAY";
        case frame_warp_lab::CameraPath::fast_pan: return "FASTPAN";
        case frame_warp_lab::CameraPath::occlusion_sweep: return "OCCL";
        case frame_warp_lab::CameraPath::camera_cut: return "CUT";
        case frame_warp_lab::CameraPath::manual: return "MANUAL";
        default: return "UNK";
    }
}

void format_hud_lines(
    frame_warp_lab::HudOverlayState& hud,
    double smoothed_interval_us,
    std::int64_t last_scene_gpu_us,
    std::int64_t last_present_gpu_us,
    std::int64_t last_input_to_present_us,
    std::int64_t missed_deadlines,
    std::uint32_t last_invalid_pixels,
    std::uint32_t last_total_pixels,
    frame_warp_lab::PresentationMode mode,
    frame_warp_lab::PacerMode pacer_mode,
    std::int32_t source_divider,
    frame_warp_lab::CameraPath path,
    std::int32_t stress_level,
    bool pcl_listener_active,
    bool mouse_look_locked) {
    const double frame_ms = smoothed_interval_us > 1.0 ? smoothed_interval_us / 1000.0 : 0.0;
    const double fps = frame_ms > 0.01 ? 1000.0 / frame_ms : 0.0;
    const double scene_ms = last_scene_gpu_us >= 0 ? static_cast<double>(last_scene_gpu_us) / 1000.0 : 0.0;
    const double warp_ms = last_present_gpu_us >= 0 ? static_cast<double>(last_present_gpu_us) / 1000.0 : 0.0;
    const double input_ms = last_input_to_present_us >= 0 ? static_cast<double>(last_input_to_present_us) / 1000.0 : 0.0;
    const double holes_pct = last_total_pixels > 0
        ? (100.0 * static_cast<double>(last_invalid_pixels) / static_cast<double>(last_total_pixels))
        : 0.0;

    char line_buffer[80]{};
    std::snprintf(line_buffer, sizeof(line_buffer), "FPS %5.1f  FT %5.2f MS  %s /%d  PACE %s", fps, frame_ms, short_mode_label(mode), source_divider, short_pacer_label(pacer_mode));
    hud.lines[0] = line_buffer;

    std::snprintf(line_buffer, sizeof(line_buffer), "GPU SCN %4.2f  WRP %4.2f  IN-PR %4.2f  MS", scene_ms, warp_ms, input_ms);
    hud.lines[1] = line_buffer;

    std::snprintf(line_buffer, sizeof(line_buffer), "HOLE %4.2f%%  MISS %lld  PCL %s  STR %d", holes_pct, static_cast<long long>(missed_deadlines), pcl_listener_active ? "LIVE" : "RDY", stress_level);
    hud.lines[2] = line_buffer;

    std::snprintf(line_buffer, sizeof(line_buffer), "%s  PATH %s  1-3 MODE  L PACE  H HOLE", mouse_look_locked ? "RMB/ESC UNLK" : "LMB/RMB LOOK", short_path_label(path));
    hud.lines[3] = line_buffer;
}

LRESULT CALLBACK window_procedure(HWND window, UINT message, WPARAM message_parameter, LPARAM message_data) {
    auto* input = reinterpret_cast<WindowInputState*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_DESTROY) {
        if (input) set_mouse_look_locked(window, *input, false);
        PostQuitMessage(0);
        return 0;
    }
    if (!input) {
        return DefWindowProcW(window, message, message_parameter, message_data);
    }
    if (message == WM_INPUT) {
        UINT data_size = 0;
        if (GetRawInputData(reinterpret_cast<HRAWINPUT>(message_data), RID_INPUT, nullptr, &data_size, sizeof(RAWINPUTHEADER)) == 0
            && data_size > 0 && data_size <= 256) {
            alignas(RAWINPUT) std::uint8_t buffer[256]{};
            if (GetRawInputData(reinterpret_cast<HRAWINPUT>(message_data), RID_INPUT, buffer, &data_size, sizeof(RAWINPUTHEADER)) == data_size) {
                const auto* raw = reinterpret_cast<const RAWINPUT*>(buffer);
                if (raw->header.dwType == RIM_TYPEMOUSE) {
                    if ((raw->data.mouse.usButtonFlags & RI_MOUSE_LEFT_BUTTON_DOWN) != 0) {
                        input->flash_trigger_pending = true;
                        stamp_input_arrival(*input);
                    }
                    if ((input->mouse_look_locked || input->left_drag_active)
                        && (raw->data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE) == 0
                        && (raw->data.mouse.lLastX != 0 || raw->data.mouse.lLastY != 0)) {
                        input->pending_mouse_delta_x += static_cast<float>(raw->data.mouse.lLastX);
                        input->pending_mouse_delta_y += static_cast<float>(raw->data.mouse.lLastY);
                        input->raw_input_received_this_frame = true;
                        stamp_input_arrival(*input);
                    }
                }
            }
        }
        return 0;
    }
    if (message == WM_LBUTTONDOWN) {
        input->flash_trigger_pending = true;
        stamp_input_arrival(*input);
        if (!input->mouse_look_locked) {
            input->left_drag_active = true;
            input->last_cursor_x = GET_X_LPARAM(message_data);
            input->last_cursor_y = GET_Y_LPARAM(message_data);
            SetCapture(window);
        }
        return 0;
    }
    if (message == WM_LBUTTONUP) {
        if (input->left_drag_active) {
            input->left_drag_active = false;
            if (GetCapture() == window) ReleaseCapture();
        }
        return 0;
    }
    if (message == WM_RBUTTONDOWN) {
        set_mouse_look_locked(window, *input, !input->mouse_look_locked);
        return 0;
    }
    if (message == WM_MOUSEMOVE) {
        const int x = GET_X_LPARAM(message_data);
        const int y = GET_Y_LPARAM(message_data);
        if (!input->raw_input_received_this_frame && input->left_drag_active) {
            const int dx = x - input->last_cursor_x;
            const int dy = y - input->last_cursor_y;
            if (dx != 0 || dy != 0) {
                input->pending_mouse_delta_x += static_cast<float>(dx);
                input->pending_mouse_delta_y += static_cast<float>(dy);
                stamp_input_arrival(*input);
            }
        }
        input->last_cursor_x = x;
        input->last_cursor_y = y;
        return 0;
    }
    if (message == WM_KILLFOCUS || (message == WM_CAPTURECHANGED && reinterpret_cast<HWND>(message_data) != window)) {
        input->left_drag_active = false;
        set_mouse_look_locked(window, *input, false);
        return 0;
    }
    return DefWindowProcW(window, message, message_parameter, message_data);
}

}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show) {
    try {
        const wchar_t* class_name = frame_warp_lab::window_class_name;
        WNDCLASSW window_class{};
        window_class.lpfnWndProc = window_procedure;
        window_class.hInstance = instance;
        window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        window_class.lpszClassName = class_name;
        if (!RegisterClassW(&window_class)) {
            throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), "Register window class");
        }
        HWND window = CreateWindowExW(0, class_name, L"Frame Warp Lab - Motion Lab", WS_OVERLAPPEDWINDOW,
                                      CW_USEDEFAULT, CW_USEDEFAULT, 1280, 720, nullptr, nullptr, instance, nullptr);
        if (!window) {
            throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), "Create window");
        }

        WindowInputState input_state{};
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&input_state));

        RAWINPUTDEVICE raw_mouse{};
        raw_mouse.usUsagePage = 0x01;
        raw_mouse.usUsage = 0x02;
        raw_mouse.dwFlags = 0;
        raw_mouse.hwndTarget = window;
        input_state.raw_input_registered = RegisterRawInputDevices(&raw_mouse, 1, sizeof(raw_mouse)) != FALSE;

        frame_warp_lab::LatencyMonitor latency_monitor;
        latency_monitor.start_ping_loop(window);

        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
        frame_warp_lab::Renderer renderer(window);
        frame_warp_lab::FramePacer pacer;
        pacer.refresh_display_timing(window);

        RuntimeStatePublisher runtime_state(
            renderer.gpu_timing_available(),
            renderer.allow_tearing_enabled(),
            renderer.waitable_swap_chain_enabled(),
            input_state.raw_input_registered);
        frame_warp_lab::SceneSimulation simulation;

        ShowWindow(window, show == SW_HIDE ? SW_SHOWNORMAL : show);
        RECT previous_client_rectangle{};
        GetClientRect(window, &previous_client_rectangle);

        bool running = true;
        bool scene_paused = false;
        frame_warp_lab::PresentationMode presentation_mode = frame_warp_lab::PresentationMode::full_render;
        std::int32_t source_divider = 1;
        bool vsync_enabled = !renderer.allow_tearing_enabled();
        frame_warp_lab::DiagnosticView diagnostic_view = frame_warp_lab::DiagnosticView::final_color;
        std::int32_t stress_level = 0;
        bool overlay_enabled = true;
        bool flash_indicator_enabled = false;
        bool pending_pcl_ping = false;

        std::int64_t display_frame_id = 0;
        std::int64_t source_frame_id = 0;
        std::int64_t cadence_counter = 0;
        std::int64_t missed_deadlines = 0;
        bool has_completed_source = false;
        frame_warp_lab::SceneSnapshot latest_source_snapshot{};
        auto latest_source_capture_time = std::chrono::steady_clock::now();
        auto previous_tick = latest_source_capture_time;
        auto previous_present_tick = latest_source_capture_time;
        bool has_previous_present = false;
        double smoothed_present_interval_us = 0.0;
        double smoothed_cpu_work_us = 900.0;
        std::int64_t last_scene_gpu_us = -1;
        std::int64_t last_present_gpu_us = -1;
        std::int64_t last_input_to_present_us = -1;
        std::uint32_t last_invalid_pixels = 0;
        std::uint32_t last_total_pixels = 0;

        frame_warp_lab::HudOverlayState hud_state{};
        hud_state.max_graph_ms = 20.0f;
        hud_state.target_frame_ms = pacer.target_frame_ms();

        auto publish_config = [&]() {
            runtime_state.update_config(
                presentation_mode,
                pacer.mode(),
                pacer.target_fps_override(),
                pacer.target_period_us(),
                source_divider,
                vsync_enabled,
                diagnostic_view,
                simulation.path(),
                stress_level,
                overlay_enabled,
                flash_indicator_enabled);
        };
        publish_config();

        auto pump_messages = [&]() {
            MSG message{};
            while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                if (message.message == WM_QUIT) {
                    running = false;
                    break;
                }
                if (latency_monitor.is_ping_message(message.message)) {
                    pending_pcl_ping = true;
                    stamp_input_arrival(input_state);
                    continue;
                }
                if (message.message == WM_KEYDOWN && (message.lParam & (1LL << 30)) == 0) {
                    bool config_changed = false;
                    if (message.wParam == VK_ESCAPE && input_state.mouse_look_locked) {
                        set_mouse_look_locked(window, input_state, false);
                    } else if (message.wParam == 'M') {
                        set_mouse_look_locked(window, input_state, !input_state.mouse_look_locked);
                    } else if (message.wParam == '1') {
                        presentation_mode = frame_warp_lab::PresentationMode::full_render;
                        cadence_counter = 0;
                        config_changed = true;
                    } else if (message.wParam == '2') {
                        presentation_mode = frame_warp_lab::PresentationMode::repeat;
                        if (source_divider < 2) source_divider = 2;
                        cadence_counter = 0;
                        config_changed = true;
                    } else if (message.wParam == '3') {
                        presentation_mode = frame_warp_lab::PresentationMode::reproject;
                        if (source_divider < 2) source_divider = 2;
                        cadence_counter = 0;
                        config_changed = true;
                    } else if (message.wParam == 'L') {
                        const auto next_pacer = static_cast<frame_warp_lab::PacerMode>((static_cast<std::int32_t>(pacer.mode()) + 1) % 3);
                        pacer.set_mode(next_pacer);
                        vsync_enabled = next_pacer == frame_warp_lab::PacerMode::vsync_only;
                        config_changed = true;
                    } else if (message.wParam == 'H') {
                        diagnostic_view = diagnostic_view == frame_warp_lab::DiagnosticView::reprojection_holes
                            ? frame_warp_lab::DiagnosticView::final_color
                            : frame_warp_lab::DiagnosticView::reprojection_holes;
                        config_changed = true;
                    } else if (message.wParam == 'O') {
                        overlay_enabled = !overlay_enabled;
                        config_changed = true;
                    } else if (message.wParam == 'F') {
                        flash_indicator_enabled = !flash_indicator_enabled;
                        config_changed = true;
                    } else if (message.wParam == 'V') {
                        vsync_enabled = !vsync_enabled;
                        pacer.reset();
                        config_changed = true;
                    } else if (message.wParam == 'P' || message.wParam == VK_SPACE) {
                        scene_paused = !scene_paused;
                        runtime_state.record_event(
                            scene_paused ? frame_warp_lab::RuntimeEventType::scene_paused : frame_warp_lab::RuntimeEventType::scene_resumed,
                            scene_paused ? 1 : 0);
                    } else if (message.wParam == 'R') {
                        simulation.reset();
                        has_completed_source = false;
                        cadence_counter = 0;
                        pacer.reset();
                        runtime_state.record_event(frame_warp_lab::RuntimeEventType::scene_reset, 0);
                    } else if (message.wParam == 'C') {
                        const auto next_path = static_cast<frame_warp_lab::CameraPath>((static_cast<std::int32_t>(simulation.path()) + 1) % 5);
                        simulation.set_path(next_path);
                        has_completed_source = false;
                        config_changed = true;
                    } else if (message.wParam == VK_OEM_4) {
                        source_divider = std::max(1, source_divider - 1);
                        cadence_counter = 0;
                        config_changed = true;
                    } else if (message.wParam == VK_OEM_6) {
                        source_divider = std::min(frame_warp_lab::max_source_divider, source_divider + 1);
                        cadence_counter = 0;
                        config_changed = true;
                    } else if (message.wParam == VK_OEM_MINUS) {
                        stress_level = std::max(0, stress_level - 4);
                        config_changed = true;
                    } else if (message.wParam == VK_OEM_PLUS) {
                        stress_level = std::min(frame_warp_lab::max_stress_level, stress_level + 4);
                        config_changed = true;
                    }
                    if (config_changed) {
                        runtime_state.record_event(frame_warp_lab::RuntimeEventType::config_updated, static_cast<std::int32_t>(message.wParam));
                        publish_config();
                    }
                }
                if (message.message == frame_warp_lab::control_message) {
                    const auto command = static_cast<frame_warp_lab::ControlCommand>(message.wParam);
                    if (command == frame_warp_lab::ControlCommand::scene_pause) {
                        scene_paused = true;
                        runtime_state.record_event(frame_warp_lab::RuntimeEventType::scene_paused, 1);
                    } else if (command == frame_warp_lab::ControlCommand::scene_resume) {
                        scene_paused = false;
                        runtime_state.record_event(frame_warp_lab::RuntimeEventType::scene_resumed, 0);
                    } else if (command == frame_warp_lab::ControlCommand::scene_reset) {
                        simulation.reset();
                        has_completed_source = false;
                        cadence_counter = 0;
                        pacer.reset();
                        runtime_state.record_event(frame_warp_lab::RuntimeEventType::scene_reset, 0);
                    } else if (command == frame_warp_lab::ControlCommand::set_diagnostic_view) {
                        const auto raw = static_cast<std::int32_t>(message.lParam);
                        if (raw >= 0 && raw <= 3) {
                            diagnostic_view = static_cast<frame_warp_lab::DiagnosticView>(raw);
                            runtime_state.record_event(frame_warp_lab::RuntimeEventType::config_updated, raw);
                        }
                    } else if (command == frame_warp_lab::ControlCommand::set_camera_path) {
                        const auto raw = static_cast<std::int32_t>(message.lParam);
                        if (raw >= 0 && raw <= 4) {
                            simulation.set_path(static_cast<frame_warp_lab::CameraPath>(raw));
                            has_completed_source = false;
                            runtime_state.record_event(frame_warp_lab::RuntimeEventType::config_updated, 100 + raw);
                        }
                    } else if (command == frame_warp_lab::ControlCommand::set_stress_level) {
                        stress_level = std::clamp(static_cast<std::int32_t>(message.lParam), 0, frame_warp_lab::max_stress_level);
                        runtime_state.record_event(frame_warp_lab::RuntimeEventType::config_updated, 200 + stress_level);
                    } else if (command == frame_warp_lab::ControlCommand::set_overlay_enabled) {
                        overlay_enabled = message.lParam != 0;
                        runtime_state.record_event(frame_warp_lab::RuntimeEventType::config_updated, overlay_enabled ? 301 : 300);
                    } else if (command == frame_warp_lab::ControlCommand::set_presentation_mode) {
                        const auto raw = static_cast<std::int32_t>(message.lParam);
                        if (raw >= 0 && raw <= 2) {
                            presentation_mode = static_cast<frame_warp_lab::PresentationMode>(raw);
                            cadence_counter = 0;
                            runtime_state.record_event(frame_warp_lab::RuntimeEventType::config_updated, 400 + raw);
                        }
                    } else if (command == frame_warp_lab::ControlCommand::set_source_divider) {
                        source_divider = std::clamp(static_cast<std::int32_t>(message.lParam), 1, frame_warp_lab::max_source_divider);
                        cadence_counter = 0;
                        runtime_state.record_event(frame_warp_lab::RuntimeEventType::config_updated, 500 + source_divider);
                    } else if (command == frame_warp_lab::ControlCommand::set_vsync_enabled) {
                        vsync_enabled = message.lParam != 0;
                        pacer.reset();
                        runtime_state.record_event(frame_warp_lab::RuntimeEventType::config_updated, vsync_enabled ? 601 : 600);
                    } else if (command == frame_warp_lab::ControlCommand::set_flash_indicator) {
                        flash_indicator_enabled = message.lParam != 0;
                        runtime_state.record_event(frame_warp_lab::RuntimeEventType::config_updated, flash_indicator_enabled ? 701 : 700);
                    } else if (command == frame_warp_lab::ControlCommand::set_pacer_mode) {
                        const auto raw = static_cast<std::int32_t>(message.lParam);
                        if (raw >= 0 && raw <= 2) {
                            const auto new_mode = static_cast<frame_warp_lab::PacerMode>(raw);
                            pacer.set_mode(new_mode);
                            if (new_mode == frame_warp_lab::PacerMode::vsync_only) vsync_enabled = true;
                            else if (new_mode == frame_warp_lab::PacerMode::deadline || new_mode == frame_warp_lab::PacerMode::unlocked) {
                                vsync_enabled = !renderer.allow_tearing_enabled();
                            }
                            runtime_state.record_event(frame_warp_lab::RuntimeEventType::config_updated, 800 + raw);
                        }
                    } else if (command == frame_warp_lab::ControlCommand::set_target_fps) {
                        const auto raw = std::clamp(static_cast<std::int32_t>(message.lParam), 0, frame_warp_lab::max_target_fps);
                        pacer.set_target_fps(raw, window);
                        hud_state.target_frame_ms = pacer.target_frame_ms();
                        runtime_state.record_event(frame_warp_lab::RuntimeEventType::config_updated, 900 + raw);
                    }
                    previous_tick = std::chrono::steady_clock::now();
                    publish_config();
                    float yaw_deg = 0.0f;
                    float pitch_deg = 0.0f;
                    simulation.current_camera_angles_degrees(yaw_deg, pitch_deg);
                    runtime_state.update_scene(
                        simulation.time_seconds(),
                        scene_paused,
                        input_state.mouse_look_locked || input_state.left_drag_active,
                        yaw_deg,
                        pitch_deg,
                        latency_monitor);
                    continue;
                }
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
        };

        while (running) {
            pump_messages();
            if (!running) break;
            if (IsIconic(window)) {
                WaitMessage();
                previous_tick = std::chrono::steady_clock::now();
                has_previous_present = false;
                pacer.reset();
                continue;
            }

            RECT current_client_rectangle{};
            if (!GetClientRect(window, &current_client_rectangle)) {
                throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), "Get client size");
            }
            const UINT client_width = static_cast<UINT>(std::max<LONG>(0, current_client_rectangle.right - current_client_rectangle.left));
            const UINT client_height = static_cast<UINT>(std::max<LONG>(0, current_client_rectangle.bottom - current_client_rectangle.top));
            if (client_width == 0 || client_height == 0) {
                WaitMessage();
                previous_tick = std::chrono::steady_clock::now();
                has_previous_present = false;
                pacer.reset();
                continue;
            }
            if (current_client_rectangle.right != previous_client_rectangle.right || current_client_rectangle.bottom != previous_client_rectangle.bottom) {
                renderer.resize(client_width, client_height);
                runtime_state.record_resize();
                previous_client_rectangle = current_client_rectangle;
                has_completed_source = false;
                cadence_counter = 0;
                pacer.refresh_display_timing(window);
                pacer.reset();
                hud_state.target_frame_ms = pacer.target_frame_ms();
                publish_config();
            }

            renderer.wait_for_frame_slot();
            const std::int64_t lead_us = std::clamp(static_cast<std::int64_t>(smoothed_cpu_work_us) + 450LL, 600LL, 2500LL);
            pacer.wait_for_input_window(lead_us, pump_messages, running);
            pump_messages();
            if (!running) break;

            ++display_frame_id;
            const auto frame_id_u64 = static_cast<std::uint64_t>(display_frame_id);
            const auto sim_begin_time = std::chrono::steady_clock::now();
            latency_monitor.emit_marker(frame_warp_lab::LatencyMarker::simulation_start, frame_id_u64);
            if (pending_pcl_ping) {
                latency_monitor.record_ping_received(frame_id_u64);
                pending_pcl_ping = false;
            }

            bool has_input_timestamp = false;
            std::chrono::steady_clock::time_point earliest_input_time{};
            bool flash_triggered = false;
            const frame_warp_lab::ManualCameraInput manual_input = consume_manual_camera_input(
                window,
                input_state,
                has_input_timestamp,
                earliest_input_time,
                flash_triggered);
            if (flash_triggered && flash_indicator_enabled) {
                latency_monitor.emit_marker(frame_warp_lab::LatencyMarker::trigger_flash, frame_id_u64);
            }

            if (simulation.take_manual_control_if_needed(manual_input)) {
                has_completed_source = false;
                runtime_state.record_event(frame_warp_lab::RuntimeEventType::config_updated, 104);
                publish_config();
            }

            const auto current_tick = std::chrono::steady_clock::now();
            simulation.advance(std::chrono::duration<double>(current_tick - previous_tick).count(), scene_paused, manual_input);
            previous_tick = current_tick;

            const std::int32_t effective_divider = presentation_mode == frame_warp_lab::PresentationMode::full_render ? 1 : source_divider;
            const bool render_scene_pass = !has_completed_source || (cadence_counter % effective_divider == 0);
            ++cadence_counter;

            frame_warp_lab::FrameKind frame_kind = frame_warp_lab::FrameKind::rendered;
            frame_warp_lab::FallbackReason fallback_reason = frame_warp_lab::FallbackReason::none;
            bool warp_enabled = false;
            bool frame_discontinuity = false;
            frame_warp_lab::CameraBasis target_camera{};

            if (render_scene_pass) {
                ++source_frame_id;
                latest_source_capture_time = std::chrono::steady_clock::now();
                latest_source_snapshot = simulation.capture_source_snapshot(source_frame_id);
                has_completed_source = true;
                target_camera = latest_source_snapshot.camera;
                frame_discontinuity = latest_source_snapshot.discontinuity;
                frame_kind = frame_warp_lab::FrameKind::rendered;
            } else if (presentation_mode == frame_warp_lab::PresentationMode::repeat) {
                target_camera = latest_source_snapshot.camera;
                frame_kind = frame_warp_lab::FrameKind::repeated;
            } else {
                std::int64_t current_cut_segment = 0;
                target_camera = simulation.sample_latest_camera(current_cut_segment);
                const std::int64_t pending_source_age_us = std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now() - latest_source_capture_time).count();
                if (current_cut_segment != latest_source_snapshot.cut_segment) {
                    frame_kind = frame_warp_lab::FrameKind::fallback;
                    fallback_reason = frame_warp_lab::FallbackReason::camera_cut;
                    frame_discontinuity = true;
                    runtime_state.record_event(frame_warp_lab::RuntimeEventType::reprojection_fallback, static_cast<std::int32_t>(fallback_reason));
                } else if (pending_source_age_us > frame_warp_lab::max_reprojection_source_age_us) {
                    frame_kind = frame_warp_lab::FrameKind::fallback;
                    fallback_reason = frame_warp_lab::FallbackReason::stale_source;
                    runtime_state.record_event(frame_warp_lab::RuntimeEventType::reprojection_fallback, static_cast<std::int32_t>(fallback_reason));
                } else {
                    frame_kind = frame_warp_lab::FrameKind::warped;
                    warp_enabled = true;
                }
            }

            latency_monitor.emit_marker(frame_warp_lab::LatencyMarker::camera_constructed, frame_id_u64);
            const auto sim_end_time = std::chrono::steady_clock::now();
            latency_monitor.emit_marker(frame_warp_lab::LatencyMarker::simulation_end, frame_id_u64);
            const std::int64_t sim_cpu_us = std::chrono::duration_cast<std::chrono::microseconds>(sim_end_time - sim_begin_time).count();

            format_hud_lines(
                hud_state,
                smoothed_present_interval_us,
                last_scene_gpu_us,
                last_present_gpu_us,
                last_input_to_present_us,
                missed_deadlines,
                last_invalid_pixels,
                last_total_pixels,
                presentation_mode,
                pacer.mode(),
                effective_divider,
                simulation.path(),
                stress_level,
                latency_monitor.etw_listener_active(),
                input_state.mouse_look_locked);

            const frame_warp_lab::FrameExecutionMetrics gpu_metrics = renderer.render_and_present(
                frame_warp_lab::FrameRenderRequest{
                    frame_id_u64,
                    latest_source_snapshot,
                    target_camera,
                    hud_state,
                    diagnostic_view,
                    stress_level,
                    overlay_enabled,
                    flash_indicator_enabled,
                    flash_triggered && flash_indicator_enabled,
                    render_scene_pass,
                    warp_enabled,
                    vsync_enabled,
                },
                latency_monitor,
                pacer);

            const auto present_complete_time = std::chrono::steady_clock::now();
            const auto present_entry_time = gpu_metrics.present_call_time;
            const std::int64_t present_interval_us = has_previous_present
                ? std::chrono::duration_cast<std::chrono::microseconds>(present_entry_time - previous_present_tick).count()
                : 0;
            const std::int64_t source_age_us = std::chrono::duration_cast<std::chrono::microseconds>(present_entry_time - latest_source_capture_time).count();
            const std::int64_t input_to_present_us = has_input_timestamp
                ? std::chrono::duration_cast<std::chrono::microseconds>(present_complete_time - earliest_input_time).count()
                : -1;
            previous_present_tick = present_entry_time;
            has_previous_present = true;

            if (gpu_metrics.missed_deadline) {
                ++missed_deadlines;
            }
            const double frame_cpu_work_us = static_cast<double>(sim_cpu_us + gpu_metrics.render_submit_cpu_us);
            smoothed_cpu_work_us = smoothed_cpu_work_us * 0.90 + frame_cpu_work_us * 0.10;

            if (present_interval_us > 0) {
                if (smoothed_present_interval_us <= 1.0) {
                    smoothed_present_interval_us = static_cast<double>(present_interval_us);
                } else {
                    smoothed_present_interval_us = smoothed_present_interval_us * 0.88 + static_cast<double>(present_interval_us) * 0.12;
                }
            }
            if (input_to_present_us >= 0) last_input_to_present_us = input_to_present_us;
            if (gpu_metrics.completed_scene_gpu_us >= 0) last_scene_gpu_us = gpu_metrics.completed_scene_gpu_us;
            if (gpu_metrics.completed_present_gpu_us >= 0) last_present_gpu_us = gpu_metrics.completed_present_gpu_us;
            if (gpu_metrics.completed_slot_warped) {
                last_invalid_pixels = gpu_metrics.completed_invalid_pixel_count;
                last_total_pixels = gpu_metrics.completed_total_pixel_count;
            } else if (presentation_mode != frame_warp_lab::PresentationMode::reproject) {
                last_invalid_pixels = 0;
                last_total_pixels = gpu_metrics.completed_total_pixel_count;
            }

            const float sample_gpu_ms = ((gpu_metrics.completed_scene_gpu_us > 0 ? static_cast<float>(gpu_metrics.completed_scene_gpu_us) : 0.0f)
                + (gpu_metrics.completed_present_gpu_us > 0 ? static_cast<float>(gpu_metrics.completed_present_gpu_us) : 0.0f)) / 1000.0f;
            hud_state.samples[hud_state.write_head & (frame_warp_lab::hud_graph_sample_count - 1)] = frame_warp_lab::HudGraphSample{
                static_cast<float>(present_interval_us) / 1000.0f,
                sample_gpu_ms,
                static_cast<float>(static_cast<std::int32_t>(frame_kind)),
                static_cast<float>(source_age_us) / 1000.0f,
            };
            hud_state.write_head = (hud_state.write_head + 1U) & (frame_warp_lab::hud_graph_sample_count - 1U);

            float yaw_deg = 0.0f;
            float pitch_deg = 0.0f;
            simulation.current_camera_angles_degrees(yaw_deg, pitch_deg);
            runtime_state.update_scene(
                simulation.time_seconds(),
                scene_paused,
                input_state.mouse_look_locked || input_state.left_drag_active,
                yaw_deg,
                pitch_deg,
                latency_monitor);
            runtime_state.record_frame(
                frame_warp_lab::RuntimeFrameSample{
                    0,
                    display_frame_id,
                    latest_source_snapshot.source_frame_id,
                    present_interval_us,
                    source_age_us,
                    input_to_present_us,
                    sim_cpu_us,
                    gpu_metrics.render_submit_cpu_us,
                    gpu_metrics.present_call_cpu_us,
                    gpu_metrics.completed_scene_gpu_us,
                    gpu_metrics.completed_present_gpu_us,
                    gpu_metrics.completed_invalid_pixel_count,
                    gpu_metrics.completed_total_pixel_count,
                    static_cast<std::int32_t>(presentation_mode),
                    static_cast<std::int32_t>(frame_kind),
                    static_cast<std::int32_t>(fallback_reason),
                    static_cast<std::int32_t>(diagnostic_view),
                    static_cast<std::int32_t>(simulation.path()),
                    stress_level,
                    effective_divider,
                    render_scene_pass ? 1 : 0,
                    gpu_metrics.completed_slot_warped ? 1 : 0,
                    gpu_metrics.missed_deadline ? 1 : 0,
                    frame_discontinuity ? 1 : 0,
                },
                static_cast<std::int64_t>(smoothed_present_interval_us));
        }

        set_mouse_look_locked(window, input_state, false);
        latency_monitor.stop();
        renderer.flush();
        return 0;
    } catch (const std::exception& error) {
        MessageBoxA(nullptr, error.what(), "Frame Warp Lab error", MB_OK | MB_ICONERROR);
        return 1;
    }
}
