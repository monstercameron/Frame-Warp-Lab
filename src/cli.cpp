#include "runtime_state.h"

#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {

struct WindowInstance {
    HWND window;
    DWORD process_id;
};

BOOL CALLBACK collect_window(HWND window, LPARAM context) {
    wchar_t class_name[128]{};
    if (GetClassNameW(window, class_name, 128) == 0 || std::wstring(class_name) != frame_warp_lab::window_class_name) return TRUE;
    DWORD process_id = 0;
    GetWindowThreadProcessId(window, &process_id);
    reinterpret_cast<std::vector<WindowInstance>*>(context)->push_back({window, process_id});
    return TRUE;
}

std::vector<WindowInstance> find_instances() {
    std::vector<WindowInstance> instances;
    EnumWindows(collect_window, reinterpret_cast<LPARAM>(&instances));
    return instances;
}

std::string json_escape_utf8(std::string_view utf8) {
    std::string escaped = "\"";
    for (unsigned char character : utf8) {
        if (character == '"' || character == '\\') { escaped += '\\'; escaped += static_cast<char>(character); }
        else if (character < 0x20) {
            const char hex[] = "0123456789abcdef";
            escaped += "\\u00";
            escaped += hex[character >> 4];
            escaped += hex[character & 15];
        } else escaped += static_cast<char>(character);
    }
    return escaped + '"';
}

std::string json_string(const std::wstring& text) {
    if (text.empty()) return "\"\"";
    const int byte_count = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    if (byte_count <= 0) return "\"\"";
    std::string utf8(static_cast<std::size_t>(byte_count), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), utf8.data(), byte_count, nullptr, nullptr);
    return json_escape_utf8(utf8);
}

int fail(std::string_view code, std::string_view message) {
    std::cerr << "{\"ok\":false,\"error\":{\"code\":" << json_escape_utf8(code)
              << ",\"message\":" << json_escape_utf8(message) << "}}\n";
    return 1;
}

bool parse_unsigned_long(const wchar_t* text, unsigned long& value) {
    wchar_t* end = nullptr;
    const unsigned long parsed = std::wcstoul(text, &end, 10);
    if (text[0] == L'\0' || *end != L'\0' || parsed == ULONG_MAX) return false;
    value = parsed;
    return true;
}

class RuntimeStateView {
public:
    RuntimeStateView() = default;
    ~RuntimeStateView() {
        if (state_) UnmapViewOfFile(state_);
        if (mapping_) CloseHandle(mapping_);
    }
    RuntimeStateView(const RuntimeStateView&) = delete;
    RuntimeStateView& operator=(const RuntimeStateView&) = delete;

    bool open(DWORD process_id, const char*& error_code, const char*& error_message) {
        mapping_ = OpenFileMappingW(FILE_MAP_READ, FALSE, frame_warp_lab::runtime_mapping_name(process_id).c_str());
        if (!mapping_) {
            error_code = "state_unavailable";
            error_message = "Runtime state is unavailable for this instance";
            return false;
        }
        state_ = static_cast<frame_warp_lab::RuntimeState*>(MapViewOfFile(mapping_, FILE_MAP_READ, 0, 0, sizeof(frame_warp_lab::RuntimeState)));
        if (!state_) {
            error_code = "state_unavailable";
            error_message = "Could not map runtime state";
            return false;
        }
        const std::uint32_t version = std::atomic_ref<std::uint32_t>(state_->version).load(std::memory_order_acquire);
        const std::uint32_t mapped_process_id = std::atomic_ref<std::uint32_t>(state_->process_id).load(std::memory_order_acquire);
        if (version != frame_warp_lab::runtime_state_version || mapped_process_id != process_id) {
            error_code = "state_version_mismatch";
            error_message = "Runtime state version or process identity does not match";
            return false;
        }
        return true;
    }

    frame_warp_lab::RuntimeState* get() const { return state_; }

private:
    HANDLE mapping_ = nullptr;
    frame_warp_lab::RuntimeState* state_ = nullptr;
};

void print_display(HWND window) {
    MONITORINFOEXW monitor{};
    monitor.cbSize = sizeof(monitor);
    if (!GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST), &monitor)) {
        std::cout << "null";
        return;
    }
    DEVMODEW active_mode{};
    active_mode.dmSize = sizeof(active_mode);
    if (!EnumDisplaySettingsW(monitor.szDevice, ENUM_CURRENT_SETTINGS, &active_mode)) {
        std::cout << "{\"device\":" << json_string(monitor.szDevice) << ",\"active_mode\":null}";
        return;
    }
    DWORD highest_frequency = 0;
    DEVMODEW mode{};
    mode.dmSize = sizeof(mode);
    for (DWORD mode_index = 0; EnumDisplaySettingsW(monitor.szDevice, mode_index, &mode); ++mode_index) {
        if (mode.dmPelsWidth == active_mode.dmPelsWidth && mode.dmPelsHeight == active_mode.dmPelsHeight && mode.dmDisplayFrequency > highest_frequency)
            highest_frequency = mode.dmDisplayFrequency;
        mode = {};
        mode.dmSize = sizeof(mode);
    }
    std::cout << "{\"device\":" << json_string(monitor.szDevice)
              << ",\"active_mode\":{\"width_px\":" << active_mode.dmPelsWidth
              << ",\"height_px\":" << active_mode.dmPelsHeight << ",\"nominal_refresh_hz\":" << active_mode.dmDisplayFrequency
              << "},\"enumerated_max_refresh_hz_at_active_resolution\":";
    if (highest_frequency > 1) std::cout << highest_frequency; else std::cout << "null";
    std::cout << ",\"upper_bound_verified\":false,\"note\":\"Windows mode enumeration is nominal and does not verify monitor limits or displayed timing\"}";
}

void print_nullable_int64(std::int64_t value) {
    if (value < 0) std::cout << "null";
    else std::cout << value;
}

void print_ratio(std::uint32_t invalid_pixels, std::uint32_t total_pixels) {
    if (total_pixels == 0) {
        std::cout << "null";
        return;
    }
    const double ratio = static_cast<double>(invalid_pixels) / static_cast<double>(total_pixels);
    std::cout << std::fixed << std::setprecision(6) << ratio;
}

void print_metric_summary(std::vector<std::int64_t> values) {
    if (values.empty()) {
        std::cout << "null";
        return;
    }
    std::sort(values.begin(), values.end());
    const std::size_t count = values.size();
    const std::int64_t min_value = values.front();
    const std::int64_t median_value = values[count / 2];
    const std::size_t p95_index = std::min(count - 1, (count * 95) / 100);
    const std::int64_t p95_value = values[p95_index];
    const std::int64_t max_value = values.back();
    std::cout << "{\"count\":" << count
              << ",\"min\":" << min_value
              << ",\"median\":" << median_value
              << ",\"p95\":" << p95_value
              << ",\"max\":" << max_value << "}";
}

void print_double_summary(std::vector<double> values) {
    if (values.empty()) {
        std::cout << "null";
        return;
    }
    std::sort(values.begin(), values.end());
    const std::size_t count = values.size();
    const std::size_t p95_index = std::min(count - 1, (count * 95) / 100);
    std::cout << "{\"count\":" << count
              << ",\"min\":" << std::fixed << std::setprecision(6) << values.front()
              << ",\"median\":" << values[count / 2]
              << ",\"p95\":" << values[p95_index]
              << ",\"max\":" << values.back() << "}";
}

int print_status(const WindowInstance& instance) {
    RuntimeStateView state_view;
    const char* error_code = nullptr;
    const char* error_message = nullptr;
    if (!state_view.open(instance.process_id, error_code, error_message)) return fail(error_code, error_message);
    auto* state = state_view.get();
    const std::int32_t running = std::atomic_ref<std::int32_t>(state->running).load(std::memory_order_acquire);
    const std::int32_t present_count = std::atomic_ref<std::int32_t>(state->present_count).load(std::memory_order_acquire);
    const std::int32_t resize_count = std::atomic_ref<std::int32_t>(state->resize_count).load(std::memory_order_acquire);
    const std::int64_t scene_time_ms = std::atomic_ref<std::int64_t>(state->scene_time_ms).load(std::memory_order_acquire);
    const std::int32_t scene_paused = std::atomic_ref<std::int32_t>(state->scene_paused).load(std::memory_order_acquire);
    const std::int64_t source_frame_id = std::atomic_ref<std::int64_t>(state->source_frame_id).load(std::memory_order_acquire);
    const std::int64_t display_frame_id = std::atomic_ref<std::int64_t>(state->display_frame_id).load(std::memory_order_acquire);
    const std::int64_t rendered_frames = std::atomic_ref<std::int64_t>(state->rendered_frame_count).load(std::memory_order_acquire);
    const std::int64_t repeated_frames = std::atomic_ref<std::int64_t>(state->repeated_frame_count).load(std::memory_order_acquire);
    const std::int64_t warped_frames = std::atomic_ref<std::int64_t>(state->warped_frame_count).load(std::memory_order_acquire);
    const std::int64_t fallback_frames = std::atomic_ref<std::int64_t>(state->fallback_frame_count).load(std::memory_order_acquire);
    const std::int64_t present_interval_us = std::atomic_ref<std::int64_t>(state->last_present_interval_us).load(std::memory_order_acquire);
    const std::int64_t smoothed_interval_us = std::atomic_ref<std::int64_t>(state->smoothed_present_interval_us).load(std::memory_order_acquire);
    const std::int64_t source_age_us = std::atomic_ref<std::int64_t>(state->last_source_age_us).load(std::memory_order_acquire);
    const std::int64_t scene_gpu_us = std::atomic_ref<std::int64_t>(state->last_scene_gpu_us).load(std::memory_order_acquire);
    const std::int64_t present_gpu_us = std::atomic_ref<std::int64_t>(state->last_present_gpu_us).load(std::memory_order_acquire);
    const std::uint32_t invalid_pixels = std::atomic_ref<std::uint32_t>(state->last_invalid_pixel_count).load(std::memory_order_acquire);
    const std::uint32_t total_pixels = std::atomic_ref<std::uint32_t>(state->last_total_pixel_count).load(std::memory_order_acquire);
    const std::int32_t presentation_mode = std::atomic_ref<std::int32_t>(state->presentation_mode).load(std::memory_order_acquire);
    const std::int32_t source_divider = std::atomic_ref<std::int32_t>(state->source_divider).load(std::memory_order_acquire);
    const std::int32_t vsync_enabled = std::atomic_ref<std::int32_t>(state->vsync_enabled).load(std::memory_order_acquire);
    const std::int32_t allow_tearing_enabled = std::atomic_ref<std::int32_t>(state->allow_tearing_enabled).load(std::memory_order_acquire);
    const std::int32_t waitable_swap_chain = std::atomic_ref<std::int32_t>(state->waitable_swap_chain).load(std::memory_order_acquire);
    const std::int32_t diagnostic_view = std::atomic_ref<std::int32_t>(state->diagnostic_view).load(std::memory_order_acquire);
    const std::int32_t camera_path = std::atomic_ref<std::int32_t>(state->camera_path).load(std::memory_order_acquire);
    const std::int32_t stress_level = std::atomic_ref<std::int32_t>(state->stress_level).load(std::memory_order_acquire);
    const std::int32_t overlay_enabled = std::atomic_ref<std::int32_t>(state->overlay_enabled).load(std::memory_order_acquire);
    const std::int32_t mouse_look_active = std::atomic_ref<std::int32_t>(state->mouse_look_active).load(std::memory_order_acquire);
    const std::int32_t camera_yaw_millideg = std::atomic_ref<std::int32_t>(state->camera_yaw_millideg).load(std::memory_order_acquire);
    const std::int32_t camera_pitch_millideg = std::atomic_ref<std::int32_t>(state->camera_pitch_millideg).load(std::memory_order_acquire);
    const std::int32_t gpu_timing_available = std::atomic_ref<std::int32_t>(state->gpu_timing_available).load(std::memory_order_acquire);
    const double smoothed_fps = smoothed_interval_us > 0 ? 1000000.0 / static_cast<double>(smoothed_interval_us) : 0.0;

    RECT client_rectangle{};
    const bool has_client_size = GetClientRect(instance.window, &client_rectangle) != 0;
    std::cout << "{\"ok\":true,\"schema_version\":4,\"process_id\":" << instance.process_id
              << ",\"running\":" << (running ? "true" : "false")
              << ",\"present_calls_completed\":" << present_count
              << ",\"display_frame_id\":" << display_frame_id
              << ",\"source_frame_id\":" << source_frame_id
              << ",\"frame_counts\":{\"rendered\":" << rendered_frames
              << ",\"repeated\":" << repeated_frames
              << ",\"warped\":" << warped_frames
              << ",\"fallback\":" << fallback_frames << "}"
              << ",\"resize_count\":" << resize_count << ",\"client_size_px\":";
    if (has_client_size) std::cout << "{\"width\":" << client_rectangle.right - client_rectangle.left << ",\"height\":" << client_rectangle.bottom - client_rectangle.top << "}";
    else std::cout << "null";
    std::cout << ",\"display\":";
    print_display(instance.window);
    std::cout << ",\"presentation\":{\"mode\":\"" << frame_warp_lab::presentation_mode_name(presentation_mode)
              << "\",\"source_divider\":" << source_divider
              << ",\"vsync_enabled\":" << (vsync_enabled ? "true" : "false")
              << ",\"allow_tearing_enabled\":" << (allow_tearing_enabled ? "true" : "false")
              << ",\"waitable_swap_chain\":" << (waitable_swap_chain ? "true" : "false")
              << ",\"max_frame_latency\":1,\"displayed_frame_timing\":null}"
              << ",\"timing\":{\"cpu_present_interval_us\":" << present_interval_us
              << ",\"smoothed_present_interval_us\":" << smoothed_interval_us
              << ",\"smoothed_present_fps\":" << std::fixed << std::setprecision(2) << smoothed_fps
              << ",\"cpu_source_age_us\":" << source_age_us
              << ",\"gpu_timing_available\":" << (gpu_timing_available ? "true" : "false")
              << ",\"scene_gpu_us\":";
    print_nullable_int64(scene_gpu_us);
    std::cout << ",\"present_gpu_us\":";
    print_nullable_int64(present_gpu_us);
    std::cout << ",\"invalid_pixel_count\":" << invalid_pixels
              << ",\"invalid_pixel_ratio\":";
    print_ratio(invalid_pixels, total_pixels);
    std::cout << ",\"note\":\"cpu_present_interval_us and smoothed_present_fps reflect Present return cadence, not physical display scanout\"}"
              << ",\"scene\":{\"name\":\"motion_lab\",\"time_ms\":" << scene_time_ms
              << ",\"paused\":" << (scene_paused ? "true" : "false")
              << ",\"camera_path\":\"" << frame_warp_lab::camera_path_name(camera_path)
              << "\",\"camera\":{\"yaw_deg\":" << std::fixed << std::setprecision(2) << (static_cast<double>(camera_yaw_millideg) / 1000.0)
              << ",\"pitch_deg\":" << (static_cast<double>(camera_pitch_millideg) / 1000.0)
              << ",\"mouse_look_active\":" << (mouse_look_active ? "true" : "false")
              << "},\"diagnostic_view\":\"" << frame_warp_lab::diagnostic_view_name(diagnostic_view)
              << "\",\"stress_level\":" << stress_level
              << ",\"overlay_enabled\":" << (overlay_enabled ? "true" : "false") << "}"
              << ",\"features\":{\"scene\":\"cpu_camera_gbuffer\",\"hud\":\"2d_frametime_graph_and_readout\",\"reprojection\":\"depth_camera_warp\",\"frame_generation\":\"not_implemented\",\"vrs\":\"not_implemented\"}}\n";
    return 0;
}

int print_events(const WindowInstance& instance) {
    RuntimeStateView state_view;
    const char* error_code = nullptr;
    const char* error_message = nullptr;
    if (!state_view.open(instance.process_id, error_code, error_message)) return fail(error_code, error_message);
    auto* state = state_view.get();
    const std::int32_t event_count = std::atomic_ref<std::int32_t>(state->event_count).load(std::memory_order_acquire);
    const std::int32_t first_sequence = event_count > static_cast<std::int32_t>(frame_warp_lab::runtime_event_capacity)
        ? event_count - static_cast<std::int32_t>(frame_warp_lab::runtime_event_capacity) + 1 : 1;
    std::cout << "{\"ok\":true,\"schema_version\":4,\"process_id\":" << instance.process_id
              << ",\"capacity\":" << frame_warp_lab::runtime_event_capacity << ",\"total_events\":" << event_count << ",\"events\":[";
    bool first_event = true;
    for (std::int32_t sequence = first_sequence; sequence <= event_count; ++sequence) {
        auto& event = state->events[(static_cast<std::size_t>(sequence) - 1) % frame_warp_lab::runtime_event_capacity];
        if (std::atomic_ref<std::int32_t>(event.sequence).load(std::memory_order_acquire) != sequence) continue;
        const std::uint32_t timestamp_low = event.timestamp_low;
        const std::uint32_t timestamp_high = event.timestamp_high;
        const std::int32_t event_type = event.event_type;
        const std::int32_t value = event.value;
        std::atomic_thread_fence(std::memory_order_acquire);
        if (std::atomic_ref<std::int32_t>(event.sequence).load(std::memory_order_acquire) != sequence) continue;
        FILETIME file_time{timestamp_low, timestamp_high};
        SYSTEMTIME utc{};
        if (!FileTimeToSystemTime(&file_time, &utc)) continue;
        char timestamp[32]{};
        std::snprintf(timestamp, sizeof(timestamp), "%04u-%02u-%02uT%02u:%02u:%02u.%03uZ", utc.wYear, utc.wMonth, utc.wDay, utc.wHour, utc.wMinute, utc.wSecond, utc.wMilliseconds);
        if (!first_event) std::cout << ',';
        first_event = false;
        std::cout << "{\"sequence\":" << sequence << ",\"timestamp_utc\":\"" << timestamp
                  << "\",\"severity\":\"info\",\"subsystem\":\"runtime\",\"event\":\"" << frame_warp_lab::runtime_event_name(event_type)
                  << "\",\"value\":" << value << "}";
    }
    std::cout << "]}\n";
    return 0;
}

int print_trace(const WindowInstance& instance, std::size_t max_samples, bool include_samples) {
    RuntimeStateView state_view;
    const char* error_code = nullptr;
    const char* error_message = nullptr;
    if (!state_view.open(instance.process_id, error_code, error_message)) return fail(error_code, error_message);
    auto* state = state_view.get();
    const std::int64_t total_samples = std::atomic_ref<std::int64_t>(state->trace_count).load(std::memory_order_acquire);
    const std::size_t bounded_limit = std::min(max_samples, frame_warp_lab::runtime_frame_trace_capacity);
    const std::int64_t available = std::min<std::int64_t>(total_samples, static_cast<std::int64_t>(bounded_limit));
    const std::int64_t first_sequence = total_samples - available + 1;

    std::vector<frame_warp_lab::RuntimeFrameSample> captured;
    captured.reserve(static_cast<std::size_t>(std::max<std::int64_t>(0, available)));
    std::vector<std::int64_t> present_intervals;
    std::vector<std::int64_t> source_ages;
    std::vector<std::int64_t> scene_gpu_times;
    std::vector<std::int64_t> present_gpu_times;
    std::vector<double> invalid_ratios;
    std::int64_t rendered_count = 0;
    std::int64_t repeated_count = 0;
    std::int64_t warped_count = 0;
    std::int64_t fallback_count = 0;

    for (std::int64_t sequence = first_sequence; sequence <= total_samples; ++sequence) {
        if (sequence <= 0) continue;
        auto& slot = state->trace[(static_cast<std::size_t>(sequence) - 1) % frame_warp_lab::runtime_frame_trace_capacity];
        if (std::atomic_ref<std::int64_t>(slot.sequence).load(std::memory_order_acquire) != sequence) continue;
        frame_warp_lab::RuntimeFrameSample copy = slot;
        copy.sequence = sequence;
        std::atomic_thread_fence(std::memory_order_acquire);
        if (std::atomic_ref<std::int64_t>(slot.sequence).load(std::memory_order_acquire) != sequence) continue;
        captured.push_back(copy);
        if (copy.present_interval_us > 0) present_intervals.push_back(copy.present_interval_us);
        if (copy.source_age_us >= 0) source_ages.push_back(copy.source_age_us);
        if (copy.scene_gpu_us >= 0) scene_gpu_times.push_back(copy.scene_gpu_us);
        if (copy.present_gpu_us >= 0) present_gpu_times.push_back(copy.present_gpu_us);
        if (copy.gpu_slot_warped && copy.total_pixel_count > 0) {
            invalid_ratios.push_back(static_cast<double>(copy.invalid_pixel_count) / static_cast<double>(copy.total_pixel_count));
        }
        switch (static_cast<frame_warp_lab::FrameKind>(copy.frame_kind)) {
            case frame_warp_lab::FrameKind::rendered: ++rendered_count; break;
            case frame_warp_lab::FrameKind::repeated: ++repeated_count; break;
            case frame_warp_lab::FrameKind::warped: ++warped_count; break;
            case frame_warp_lab::FrameKind::fallback: ++fallback_count; break;
        }
    }

    std::cout << "{\"ok\":true,\"schema_version\":4,\"process_id\":" << instance.process_id
              << ",\"capacity\":" << frame_warp_lab::runtime_frame_trace_capacity
              << ",\"total_frames_recorded\":" << total_samples
              << ",\"sample_count\":" << captured.size()
              << ",\"units\":\"microseconds\""
              << ",\"note\":\"cpu_present_interval_us is Present return cadence, not physical display scanout; GPU timings and invalid-pixel counts come from the completed swap-chain slot\""
              << ",\"summary\":{\"frame_kinds\":{\"rendered\":" << rendered_count
              << ",\"repeated\":" << repeated_count
              << ",\"warped\":" << warped_count
              << ",\"fallback\":" << fallback_count << "}"
              << ",\"cpu_present_interval_us\":";
    print_metric_summary(present_intervals);
    std::cout << ",\"cpu_source_age_us\":";
    print_metric_summary(source_ages);
    std::cout << ",\"scene_gpu_us\":";
    print_metric_summary(scene_gpu_times);
    std::cout << ",\"present_gpu_us\":";
    print_metric_summary(present_gpu_times);
    std::cout << ",\"invalid_pixel_ratio\":";
    print_double_summary(invalid_ratios);
    std::cout << "}";

    if (include_samples) {
        std::cout << ",\"samples\":[";
        for (std::size_t index = 0; index < captured.size(); ++index) {
            const auto& sample = captured[index];
            if (index) std::cout << ',';
            std::cout << "{\"sequence\":" << sample.sequence
                      << ",\"display_frame_id\":" << sample.display_frame_id
                      << ",\"source_frame_id\":" << sample.source_frame_id
                      << ",\"mode\":\"" << frame_warp_lab::presentation_mode_name(sample.presentation_mode)
                      << "\",\"frame_kind\":\"" << frame_warp_lab::frame_kind_name(sample.frame_kind)
                      << "\",\"fallback_reason\":\"" << frame_warp_lab::fallback_reason_name(sample.fallback_reason)
                      << "\",\"source_rendered\":" << (sample.source_rendered ? "true" : "false")
                      << ",\"source_divider\":" << sample.source_divider
                      << ",\"cpu_present_interval_us\":" << sample.present_interval_us
                      << ",\"cpu_source_age_us\":" << sample.source_age_us
                      << ",\"scene_gpu_us\":";
            print_nullable_int64(sample.scene_gpu_us);
            std::cout << ",\"present_gpu_us\":";
            print_nullable_int64(sample.present_gpu_us);
            std::cout << ",\"invalid_pixel_count\":" << sample.invalid_pixel_count
                      << ",\"invalid_pixel_ratio\":";
            print_ratio(sample.invalid_pixel_count, sample.total_pixel_count);
            std::cout << ",\"diagnostic_view\":\"" << frame_warp_lab::diagnostic_view_name(sample.diagnostic_view)
                      << "\",\"camera_path\":\"" << frame_warp_lab::camera_path_name(sample.camera_path)
                      << "\",\"stress_level\":" << sample.stress_level
                      << ",\"discontinuity\":" << (sample.discontinuity ? "true" : "false") << "}";
        }
        std::cout << "]";
    }
    std::cout << "}\n";
    return 0;
}

int apply_setting(const WindowInstance& instance, const std::wstring& key, const std::wstring& value) {
    frame_warp_lab::ControlCommand command{};
    LPARAM parameter = 0;
    if (key == L"mode") {
        command = frame_warp_lab::ControlCommand::set_presentation_mode;
        if (value == L"full_render") parameter = static_cast<LPARAM>(frame_warp_lab::PresentationMode::full_render);
        else if (value == L"repeat") parameter = static_cast<LPARAM>(frame_warp_lab::PresentationMode::repeat);
        else if (value == L"reproject") parameter = static_cast<LPARAM>(frame_warp_lab::PresentationMode::reproject);
        else return fail("invalid_value", "Expected mode in {full_render, repeat, reproject}");
    } else if (key == L"divider") {
        unsigned long parsed = 0;
        if (!parse_unsigned_long(value.c_str(), parsed) || parsed < 1 || parsed > static_cast<unsigned long>(frame_warp_lab::max_source_divider)) {
            return fail("invalid_value", "Expected divider integer in [1, 8]");
        }
        command = frame_warp_lab::ControlCommand::set_source_divider;
        parameter = static_cast<LPARAM>(parsed);
    } else if (key == L"vsync") {
        command = frame_warp_lab::ControlCommand::set_vsync_enabled;
        if (value == L"on" || value == L"1" || value == L"true") parameter = 1;
        else if (value == L"off" || value == L"0" || value == L"false") parameter = 0;
        else return fail("invalid_value", "Expected vsync in {on, off}");
    } else if (key == L"view") {
        command = frame_warp_lab::ControlCommand::set_diagnostic_view;
        if (value == L"final") parameter = static_cast<LPARAM>(frame_warp_lab::DiagnosticView::final_color);
        else if (value == L"depth") parameter = static_cast<LPARAM>(frame_warp_lab::DiagnosticView::depth);
        else if (value == L"motion") parameter = static_cast<LPARAM>(frame_warp_lab::DiagnosticView::motion_vectors);
        else if (value == L"holes") parameter = static_cast<LPARAM>(frame_warp_lab::DiagnosticView::reprojection_holes);
        else return fail("invalid_value", "Expected view in {final, depth, motion, holes}");
    } else if (key == L"path") {
        command = frame_warp_lab::ControlCommand::set_camera_path;
        if (value == L"corridor_sway") parameter = static_cast<LPARAM>(frame_warp_lab::CameraPath::corridor_sway);
        else if (value == L"fast_pan") parameter = static_cast<LPARAM>(frame_warp_lab::CameraPath::fast_pan);
        else if (value == L"occlusion_sweep") parameter = static_cast<LPARAM>(frame_warp_lab::CameraPath::occlusion_sweep);
        else if (value == L"camera_cut") parameter = static_cast<LPARAM>(frame_warp_lab::CameraPath::camera_cut);
        else if (value == L"manual") parameter = static_cast<LPARAM>(frame_warp_lab::CameraPath::manual);
        else return fail("invalid_value", "Expected path in {corridor_sway, fast_pan, occlusion_sweep, camera_cut, manual}");
    } else if (key == L"stress") {
        unsigned long parsed = 0;
        if (!parse_unsigned_long(value.c_str(), parsed) || parsed > static_cast<unsigned long>(frame_warp_lab::max_stress_level)) {
            return fail("invalid_value", "Expected stress integer in [0, 64]");
        }
        command = frame_warp_lab::ControlCommand::set_stress_level;
        parameter = static_cast<LPARAM>(parsed);
    } else if (key == L"overlay") {
        command = frame_warp_lab::ControlCommand::set_overlay_enabled;
        if (value == L"on" || value == L"1" || value == L"true") parameter = 1;
        else if (value == L"off" || value == L"0" || value == L"false") parameter = 0;
        else return fail("invalid_value", "Expected overlay in {on, off}");
    } else {
        return fail("unknown_setting", "Supported settings are: mode, divider, vsync, view, path, stress, overlay");
    }

    if (!PostMessageW(instance.window, frame_warp_lab::control_message, static_cast<WPARAM>(command), parameter)) {
        return fail("control_failed", "Could not send configuration update request");
    }
    std::cout << "{\"ok\":true,\"process_id\":" << instance.process_id
              << ",\"setting\":" << json_string(key)
              << ",\"requested_value\":" << json_string(value)
              << ",\"applied_async\":true}\n";
    return 0;
}

int start_instance() {
    wchar_t executable_path[32768]{};
    const DWORD path_length = GetModuleFileNameW(nullptr, executable_path, static_cast<DWORD>(std::size(executable_path)));
    if (path_length == 0 || path_length >= std::size(executable_path)) return fail("start_failed", "Could not locate the CLI executable");
    std::wstring application_path(executable_path, path_length);
    const auto separator = application_path.find_last_of(L"\\/");
    if (separator == std::wstring::npos) return fail("start_failed", "Could not determine the executable directory");
    application_path = application_path.substr(0, separator + 1) + L"frame_warp_lab.exe";
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESHOWWINDOW;
    startup.wShowWindow = SW_SHOWNORMAL;
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(application_path.c_str(), nullptr, nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup, &process))
        return fail("start_failed", "Could not launch frame_warp_lab.exe next to the CLI");
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    std::cout << "{\"ok\":true,\"process_id\":" << process.dwProcessId << ",\"started\":true,\"ready\":false}\n";
    return 0;
}

void print_adapter(ID3D12Device* device, IDXGIFactory4* factory) {
    if (!factory) {
        std::cout << "null";
        return;
    }
    const LUID adapter_luid = device->GetAdapterLuid();
    ComPtr<IDXGIAdapter1> adapter;
    if (FAILED(factory->EnumAdapterByLuid(adapter_luid, IID_PPV_ARGS(&adapter)))) {
        std::cout << "null";
        return;
    }
    DXGI_ADAPTER_DESC1 description{};
    if (FAILED(adapter->GetDesc1(&description))) {
        std::cout << "null";
        return;
    }
    std::cout << "{\"description\":" << json_string(description.Description)
              << ",\"vendor_id\":" << description.VendorId
              << ",\"device_id\":" << description.DeviceId
              << ",\"dedicated_video_memory_mb\":" << (description.DedicatedVideoMemory / (1024ULL * 1024ULL))
              << ",\"luid_high\":" << adapter_luid.HighPart
              << ",\"luid_low\":" << adapter_luid.LowPart << "}";
}

int print_capabilities() {
    ComPtr<IDXGIFactory4> factory;
    CreateDXGIFactory1(IID_PPV_ARGS(&factory));
    ComPtr<ID3D12Device> device;
    HRESULT result = D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device));
    if (FAILED(result)) return fail("device_unavailable", "Could not create the default D3D12 device");
    BOOL allow_tearing = FALSE;
    bool tearing_query_available = false;
    ComPtr<IDXGIFactory5> factory5;
    if (factory && SUCCEEDED(factory.As(&factory5))) {
        if (SUCCEEDED(factory5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &allow_tearing, sizeof(allow_tearing)))) {
            tearing_query_available = true;
        }
    }
    D3D12_FEATURE_DATA_D3D12_OPTIONS6 options{};
    result = device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS6, &options, sizeof(options));
    std::cout << "{\"ok\":true,\"schema_version\":4,\"scope\":\"new_default_d3d12_device_not_runtime_instance\",\"adapter\":";
    print_adapter(device.Get(), factory.Get());
    std::cout << ",\"presentation\":{\"allow_tearing_query_available\":" << (tearing_query_available ? "true" : "false")
              << ",\"allow_tearing_supported\":" << (allow_tearing ? "true" : "false") << "},\"vrs\":";
    if (FAILED(result)) std::cout << "{\"query_available\":false}";
    else std::cout << "{\"query_available\":true,\"tier\":" << static_cast<int>(options.VariableShadingRateTier)
                   << ",\"additional_shading_rates_supported\":" << (options.AdditionalShadingRatesSupported ? "true" : "false")
                   << ",\"tile_size_px\":" << options.ShadingRateImageTileSize << "}";
    std::cout << ",\"dlss_frame_generation\":\"not_queried\"}\n";
    return 0;
}

}

int wmain(int argument_count, wchar_t** arguments) {
    if (argument_count == 2 && std::wstring(arguments[1]) == L"help") {
        std::cout << "{\"ok\":true,\"schema_version\":4,\"commands\":[\"help\",\"start\",\"instances\",\"status <pid>\",\"events <pid>\",\"trace <pid> [limit] [summary]\",\"set <pid> <key> <value>\",\"pause <pid>\",\"resume <pid>\",\"reset <pid>\",\"stop <pid>\",\"capabilities\"],\"settings\":{\"mode\":[\"full_render\",\"repeat\",\"reproject\"],\"divider\":\"1..8\",\"vsync\":[\"on\",\"off\"],\"view\":[\"final\",\"depth\",\"motion\",\"holes\"],\"path\":[\"corridor_sway\",\"fast_pan\",\"occlusion_sweep\",\"camera_cut\",\"manual\"],\"stress\":\"0..64\",\"overlay\":[\"on\",\"off\"]}}\n";
        return 0;
    }
    if (argument_count == 2 && std::wstring(arguments[1]) == L"capabilities") return print_capabilities();
    if (argument_count == 2 && std::wstring(arguments[1]) == L"start") return start_instance();
    const auto instances = find_instances();
    if (argument_count == 2 && std::wstring(arguments[1]) == L"instances") {
        std::cout << "{\"ok\":true,\"schema_version\":4,\"instances\":[";
        for (std::size_t index = 0; index < instances.size(); ++index) {
            if (index) std::cout << ',';
            std::cout << "{\"process_id\":" << instances[index].process_id << "}";
        }
        std::cout << "]}\n";
        return 0;
    }

    if (argument_count < 3) return fail("invalid_arguments", "Use help for the command list");
    const std::wstring command = arguments[1];
    unsigned long requested_process = 0;
    if (!parse_unsigned_long(arguments[2], requested_process) || requested_process == 0) {
        return fail("invalid_pid", "Expected a positive process ID");
    }

    const WindowInstance* matched_instance = nullptr;
    for (const auto& instance : instances) {
        if (instance.process_id == requested_process) {
            matched_instance = &instance;
            break;
        }
    }
    if (!matched_instance) return fail("instance_not_found", "No matching Frame Warp Lab window was found");

    if (command == L"status" && argument_count == 3) return print_status(*matched_instance);
    if (command == L"events" && argument_count == 3) return print_events(*matched_instance);
    if (command == L"trace" && (argument_count >= 3 && argument_count <= 5)) {
        std::size_t limit = 64;
        bool include_samples = true;
        if (argument_count >= 4) {
            unsigned long parsed_limit = 0;
            if (!parse_unsigned_long(arguments[3], parsed_limit) || parsed_limit == 0 || parsed_limit > frame_warp_lab::runtime_frame_trace_capacity) {
                return fail("invalid_limit", "Expected trace limit in [1, 256]");
            }
            limit = static_cast<std::size_t>(parsed_limit);
        }
        if (argument_count == 5) {
            if (std::wstring(arguments[4]) == L"summary") include_samples = false;
            else return fail("invalid_arguments", "Expected optional fifth argument 'summary'");
        }
        return print_trace(*matched_instance, limit, include_samples);
    }
    if (command == L"set" && argument_count == 5) {
        return apply_setting(*matched_instance, arguments[3], arguments[4]);
    }
    if (argument_count == 3 && (command == L"pause" || command == L"resume" || command == L"reset")) {
        const frame_warp_lab::ControlCommand operation = command == L"pause"
            ? frame_warp_lab::ControlCommand::scene_pause
            : command == L"resume" ? frame_warp_lab::ControlCommand::scene_resume : frame_warp_lab::ControlCommand::scene_reset;
        if (!PostMessageW(matched_instance->window, frame_warp_lab::control_message, static_cast<WPARAM>(operation), 0)) {
            return fail("control_failed", "Could not send scene control request");
        }
        std::cout << "{\"ok\":true,\"process_id\":" << matched_instance->process_id << ",\"scene_control_requested\":" << json_string(command) << "}\n";
        return 0;
    }
    if (command == L"stop" && argument_count == 3) {
        if (!PostMessageW(matched_instance->window, WM_CLOSE, 0, 0)) return fail("stop_failed", "Could not send the close request");
        std::cout << "{\"ok\":true,\"process_id\":" << matched_instance->process_id << ",\"stop_requested\":true}\n";
        return 0;
    }
    return fail("invalid_arguments", "Use help for the command list");
}
