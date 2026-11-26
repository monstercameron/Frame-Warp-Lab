#include "runtime_state.h"
#include "simd_math.h"
#include "streamline_backend.h"

#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
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

bool file_exists(const wchar_t* path) {
    std::error_code error;
    return std::filesystem::exists(path, error);
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
    double sum = 0.0;
    double minimum = 0.0;
    double maximum = 0.0;
    frame_warp_lab::simd::sum_and_minmax_double(values.data(), values.size(), sum, minimum, maximum);
    std::sort(values.begin(), values.end());
    const std::size_t count = values.size();
    const std::size_t p95_index = std::min(count - 1, (count * 95) / 100);
    std::cout << "{\"count\":" << count
              << ",\"min\":" << std::fixed << std::setprecision(6) << minimum
              << ",\"mean\":" << (sum / static_cast<double>(count))
              << ",\"median\":" << values[count / 2]
              << ",\"p95\":" << values[p95_index]
              << ",\"max\":" << maximum << "}";
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
    const std::int64_t generated_frames = std::atomic_ref<std::int64_t>(state->generated_frame_count).load(std::memory_order_acquire);
    const std::int64_t combined_frames = std::atomic_ref<std::int64_t>(state->combined_frame_count).load(std::memory_order_acquire);
    const std::int64_t fallback_frames = std::atomic_ref<std::int64_t>(state->fallback_frame_count).load(std::memory_order_acquire);
    const std::int64_t async_overlaps = std::atomic_ref<std::int64_t>(state->async_overlap_count).load(std::memory_order_acquire);
    const std::int64_t missed_deadlines = std::atomic_ref<std::int64_t>(state->missed_deadline_count).load(std::memory_order_acquire);
    const std::int64_t target_period_us = std::atomic_ref<std::int64_t>(state->target_period_us).load(std::memory_order_acquire);
    const std::int64_t present_interval_us = std::atomic_ref<std::int64_t>(state->last_present_interval_us).load(std::memory_order_acquire);
    const std::int64_t smoothed_interval_us = std::atomic_ref<std::int64_t>(state->smoothed_present_interval_us).load(std::memory_order_acquire);
    const std::int64_t source_age_us = std::atomic_ref<std::int64_t>(state->last_source_age_us).load(std::memory_order_acquire);
    const std::int64_t input_to_present_us = std::atomic_ref<std::int64_t>(state->last_input_to_present_us).load(std::memory_order_acquire);
    const std::int64_t sim_cpu_us = std::atomic_ref<std::int64_t>(state->last_sim_cpu_us).load(std::memory_order_acquire);
    const std::int64_t render_submit_cpu_us = std::atomic_ref<std::int64_t>(state->last_render_submit_cpu_us).load(std::memory_order_acquire);
    const std::int64_t present_call_cpu_us = std::atomic_ref<std::int64_t>(state->last_present_call_cpu_us).load(std::memory_order_acquire);
    const std::int64_t scene_gpu_us = std::atomic_ref<std::int64_t>(state->last_scene_gpu_us).load(std::memory_order_acquire);
    const std::int64_t present_gpu_us = std::atomic_ref<std::int64_t>(state->last_present_gpu_us).load(std::memory_order_acquire);
    const std::int64_t pcl_markers_emitted = std::atomic_ref<std::int64_t>(state->pcl_markers_emitted).load(std::memory_order_acquire);
    const std::int64_t pcl_pings_received = std::atomic_ref<std::int64_t>(state->pcl_pings_received).load(std::memory_order_acquire);
    const std::uint32_t invalid_pixels = std::atomic_ref<std::uint32_t>(state->last_invalid_pixel_count).load(std::memory_order_acquire);
    const std::uint32_t temporal_recovered_pixels = std::atomic_ref<std::uint32_t>(state->last_temporal_recovered_count).load(std::memory_order_acquire);
    const std::uint32_t total_pixels = std::atomic_ref<std::uint32_t>(state->last_total_pixel_count).load(std::memory_order_acquire);
    const std::int32_t presentation_mode = std::atomic_ref<std::int32_t>(state->presentation_mode).load(std::memory_order_acquire);
    const std::int32_t pacer_mode = std::atomic_ref<std::int32_t>(state->pacer_mode).load(std::memory_order_acquire);
    const std::int32_t target_fps = std::atomic_ref<std::int32_t>(state->target_fps).load(std::memory_order_acquire);
    const std::int32_t source_divider = std::atomic_ref<std::int32_t>(state->source_divider).load(std::memory_order_acquire);
    const std::int32_t vsync_enabled = std::atomic_ref<std::int32_t>(state->vsync_enabled).load(std::memory_order_acquire);
    const std::int32_t allow_tearing_enabled = std::atomic_ref<std::int32_t>(state->allow_tearing_enabled).load(std::memory_order_acquire);
    const std::int32_t waitable_swap_chain = std::atomic_ref<std::int32_t>(state->waitable_swap_chain).load(std::memory_order_acquire);
    const std::int32_t async_gpu_enabled = std::atomic_ref<std::int32_t>(state->async_gpu_enabled).load(std::memory_order_acquire);
    const std::int32_t high_priority_present_queue = std::atomic_ref<std::int32_t>(state->high_priority_present_queue).load(std::memory_order_acquire);
    const std::int32_t vrs_tier = std::atomic_ref<std::int32_t>(state->vrs_tier).load(std::memory_order_acquire);
    const std::int32_t vrs_tile_size_px = std::atomic_ref<std::int32_t>(state->vrs_tile_size_px).load(std::memory_order_acquire);
    const std::int32_t vrs_additional_rates = std::atomic_ref<std::int32_t>(state->vrs_additional_rates_supported).load(std::memory_order_acquire);
    const std::int32_t vrs_requested_mode = std::atomic_ref<std::int32_t>(state->vrs_requested_mode).load(std::memory_order_acquire);
    const std::int32_t vrs_active_mode = std::atomic_ref<std::int32_t>(state->vrs_active_mode).load(std::memory_order_acquire);
    const std::int32_t adaptive_enabled = std::atomic_ref<std::int32_t>(state->adaptive_enabled).load(std::memory_order_acquire);
    const std::int32_t adaptive_tier = std::atomic_ref<std::int32_t>(state->adaptive_tier).load(std::memory_order_acquire);
    const std::int32_t adaptive_reason = std::atomic_ref<std::int32_t>(state->adaptive_reason).load(std::memory_order_acquire);
    const std::int32_t adaptive_utilization_permille = std::atomic_ref<std::int32_t>(state->adaptive_utilization_permille).load(std::memory_order_acquire);
    const std::int64_t adaptive_reserved_headroom_us = std::atomic_ref<std::int64_t>(state->adaptive_reserved_headroom_us).load(std::memory_order_acquire);
    const std::int64_t adaptive_decision_count = std::atomic_ref<std::int64_t>(state->adaptive_decision_count).load(std::memory_order_acquire);
    const std::int32_t instrumentation_level = std::atomic_ref<std::int32_t>(state->instrumentation_level).load(std::memory_order_acquire);
    const std::int32_t last_present_variant = std::atomic_ref<std::int32_t>(state->last_present_variant).load(std::memory_order_acquire);
    const std::int32_t diagnostic_view = std::atomic_ref<std::int32_t>(state->diagnostic_view).load(std::memory_order_acquire);
    const std::int32_t camera_path = std::atomic_ref<std::int32_t>(state->camera_path).load(std::memory_order_acquire);
    const std::int32_t stress_level = std::atomic_ref<std::int32_t>(state->stress_level).load(std::memory_order_acquire);
    const std::uint32_t shader_flags = std::atomic_ref<std::uint32_t>(state->shader_flags).load(std::memory_order_acquire);
    const std::int32_t pbr_enabled = std::atomic_ref<std::int32_t>(state->pbr_enabled).load(std::memory_order_acquire);
    const std::int32_t aniso_mip_enabled = std::atomic_ref<std::int32_t>(state->aniso_mip_enabled).load(std::memory_order_acquire);
    const std::int32_t scene_kind = std::atomic_ref<std::int32_t>(state->scene_kind).load(std::memory_order_acquire);
    const std::int32_t tactical_hp = std::atomic_ref<std::int32_t>(state->tactical_hp).load(std::memory_order_acquire);
    const std::int32_t tactical_armor = std::atomic_ref<std::int32_t>(state->tactical_armor).load(std::memory_order_acquire);
    const std::int32_t tactical_ammo_in_mag = std::atomic_ref<std::int32_t>(state->tactical_ammo_in_mag).load(std::memory_order_acquire);
    const std::int32_t tactical_ammo_reserve = std::atomic_ref<std::int32_t>(state->tactical_ammo_reserve).load(std::memory_order_acquire);
    const std::int32_t tactical_shots_fired = std::atomic_ref<std::int32_t>(state->tactical_shots_fired).load(std::memory_order_acquire);
    const std::int32_t tactical_hits = std::atomic_ref<std::int32_t>(state->tactical_hits).load(std::memory_order_acquire);
    const std::int32_t tactical_headshots = std::atomic_ref<std::int32_t>(state->tactical_headshots).load(std::memory_order_acquire);
    const std::int32_t tactical_score = std::atomic_ref<std::int32_t>(state->tactical_score).load(std::memory_order_acquire);
    const std::int32_t tactical_targets_up = std::atomic_ref<std::int32_t>(state->tactical_targets_up).load(std::memory_order_acquire);
    const std::int32_t tactical_ads_active = std::atomic_ref<std::int32_t>(state->tactical_ads_active).load(std::memory_order_acquire);
    const std::uint32_t ecs_entity_count = std::atomic_ref<std::uint32_t>(state->ecs_entity_count).load(std::memory_order_acquire);
    const std::uint32_t ecs_renderable_count = std::atomic_ref<std::uint32_t>(state->ecs_renderable_count).load(std::memory_order_acquire);
    const std::uint32_t ecs_physics_count = std::atomic_ref<std::uint32_t>(state->ecs_physics_count).load(std::memory_order_acquire);
    const std::uint32_t ecs_script_count = std::atomic_ref<std::uint32_t>(state->ecs_script_count).load(std::memory_order_acquire);
    const std::uint32_t lua_loaded_scripts = std::atomic_ref<std::uint32_t>(state->lua_loaded_scripts).load(std::memory_order_acquire);
    const std::uint32_t lua_reload_count = std::atomic_ref<std::uint32_t>(state->lua_reload_count).load(std::memory_order_acquire);
    const std::uint32_t lua_error_count = std::atomic_ref<std::uint32_t>(state->lua_error_count).load(std::memory_order_acquire);
    const std::int64_t lua_last_step_us = std::atomic_ref<std::int64_t>(state->lua_last_step_us).load(std::memory_order_acquire);
    const std::uint64_t events_dispatched = std::atomic_ref<std::uint64_t>(state->events_dispatched).load(std::memory_order_acquire);
    const std::uint32_t event_subscriptions = std::atomic_ref<std::uint32_t>(state->event_subscriptions).load(std::memory_order_acquire);
    const std::int32_t hero_pistol_gltf_loaded = std::atomic_ref<std::int32_t>(state->hero_pistol_gltf_loaded).load(std::memory_order_acquire);
    const std::int32_t hero_pbr_textures_loaded = std::atomic_ref<std::int32_t>(state->hero_pbr_textures_loaded).load(std::memory_order_acquire);
    const std::uint32_t mt_worker_threads = std::atomic_ref<std::uint32_t>(state->mt_worker_threads).load(std::memory_order_acquire);
    const std::uint32_t mt_jolt_threads = std::atomic_ref<std::uint32_t>(state->mt_jolt_threads).load(std::memory_order_acquire);
    const std::uint64_t mt_jobs_executed = std::atomic_ref<std::uint64_t>(state->mt_jobs_executed).load(std::memory_order_acquire);
    const std::int64_t mt_physics_step_us = std::atomic_ref<std::int64_t>(state->mt_physics_step_us).load(std::memory_order_acquire);
    const std::int64_t mt_ecs_sync_us = std::atomic_ref<std::int64_t>(state->mt_ecs_sync_us).load(std::memory_order_acquire);
    const std::int64_t mt_pbr_build_ms = std::atomic_ref<std::int64_t>(state->mt_pbr_build_ms).load(std::memory_order_acquire);
    const std::int32_t tactical_spread_mrad_x100 = std::atomic_ref<std::int32_t>(state->tactical_spread_mrad_x100).load(std::memory_order_acquire);
    const std::int32_t tactical_shot_offset_mrad_x100 = std::atomic_ref<std::int32_t>(state->tactical_shot_offset_mrad_x100).load(std::memory_order_acquire);
    const std::int32_t overlay_enabled = std::atomic_ref<std::int32_t>(state->overlay_enabled).load(std::memory_order_acquire);
    const std::int32_t flash_indicator_enabled = std::atomic_ref<std::int32_t>(state->flash_indicator_enabled).load(std::memory_order_acquire);
    const std::int32_t raw_input_enabled = std::atomic_ref<std::int32_t>(state->raw_input_enabled).load(std::memory_order_acquire);
    const std::int32_t pcl_etw_listener_active = std::atomic_ref<std::int32_t>(state->pcl_etw_listener_active).load(std::memory_order_acquire);
    const std::int32_t mouse_look_active = std::atomic_ref<std::int32_t>(state->mouse_look_active).load(std::memory_order_acquire);
    const std::int32_t camera_yaw_millideg = std::atomic_ref<std::int32_t>(state->camera_yaw_millideg).load(std::memory_order_acquire);
    const std::int32_t camera_pitch_millideg = std::atomic_ref<std::int32_t>(state->camera_pitch_millideg).load(std::memory_order_acquire);
    const std::int32_t gpu_timing_available = std::atomic_ref<std::int32_t>(state->gpu_timing_available).load(std::memory_order_acquire);
    const std::int32_t streamline_initialized = std::atomic_ref<std::int32_t>(state->streamline_initialized).load(std::memory_order_acquire);
    const std::int32_t dlssg_supported = std::atomic_ref<std::int32_t>(state->dlssg_supported).load(std::memory_order_acquire);
    const std::int32_t dlss_sr_supported = std::atomic_ref<std::int32_t>(state->dlss_sr_supported).load(std::memory_order_acquire);
    const std::int32_t reflex_supported = std::atomic_ref<std::int32_t>(state->reflex_supported).load(std::memory_order_acquire);
    const std::int32_t dlssg_dynamic_mfg_supported = std::atomic_ref<std::int32_t>(state->dlssg_dynamic_mfg_supported).load(std::memory_order_acquire);
    const std::int32_t dlssg_max_frames_to_generate = std::atomic_ref<std::int32_t>(state->dlssg_max_frames_to_generate).load(std::memory_order_acquire);
    const std::int32_t dlssg_requested_multiplier = std::atomic_ref<std::int32_t>(state->dlssg_requested_multiplier).load(std::memory_order_acquire);
    const std::int32_t dlssg_active = std::atomic_ref<std::int32_t>(state->dlssg_active).load(std::memory_order_acquire);
    const std::int32_t pacing_owner = std::atomic_ref<std::int32_t>(state->pacing_owner).load(std::memory_order_acquire);
    const std::int32_t adaptive_dlssg_enabled = std::atomic_ref<std::int32_t>(state->adaptive_dlssg_enabled).load(std::memory_order_acquire);
    const std::int32_t dlssg_last_api_result = std::atomic_ref<std::int32_t>(state->dlssg_last_api_result).load(std::memory_order_acquire);
    const std::int32_t dlssg_last_failed_call = std::atomic_ref<std::int32_t>(state->dlssg_last_failed_call).load(std::memory_order_acquire);
    const std::int32_t dlssg_warp_guard_active = std::atomic_ref<std::int32_t>(state->dlssg_warp_guard_active).load(std::memory_order_acquire);
    const std::int64_t dlssg_api_error_count = std::atomic_ref<std::int64_t>(state->dlssg_api_error_count).load(std::memory_order_acquire);
    const std::int64_t dlssg_warp_guard_trips = std::atomic_ref<std::int64_t>(state->dlssg_warp_guard_trips).load(std::memory_order_acquire);
    const std::uint32_t dlssg_status_flags = std::atomic_ref<std::uint32_t>(state->dlssg_status_flags).load(std::memory_order_acquire);
    const std::uint32_t dlssg_last_frames_presented = std::atomic_ref<std::uint32_t>(state->dlssg_last_frames_presented).load(std::memory_order_acquire);
    const std::int64_t dlssg_total_frames_presented = std::atomic_ref<std::int64_t>(state->dlssg_total_frames_presented).load(std::memory_order_acquire);
    const std::int64_t dlssg_total_generated_frames = std::atomic_ref<std::int64_t>(state->dlssg_total_generated_frames).load(std::memory_order_acquire);
    const std::int32_t window_focused = std::atomic_ref<std::int32_t>(state->window_focused).load(std::memory_order_acquire);
    const std::int32_t selective_tiles_enabled = std::atomic_ref<std::int32_t>(state->selective_tiles_enabled).load(std::memory_order_acquire);
    const std::int32_t tile_budget_pct = std::atomic_ref<std::int32_t>(state->tile_budget_pct).load(std::memory_order_acquire);
    const std::int32_t tile_policy = std::atomic_ref<std::int32_t>(state->tile_policy).load(std::memory_order_acquire);
    const std::int32_t auto_tile_budget_enabled = std::atomic_ref<std::int32_t>(state->auto_tile_budget_enabled).load(std::memory_order_acquire);
    const std::int32_t cache_sharpen_enabled = std::atomic_ref<std::int32_t>(state->cache_sharpen_enabled).load(std::memory_order_acquire);
    const std::int32_t coarse_fallback_enabled = std::atomic_ref<std::int32_t>(state->coarse_fallback_enabled).load(std::memory_order_acquire);
    const std::int32_t subtile_interleave_enabled = std::atomic_ref<std::int32_t>(state->subtile_interleave_enabled).load(std::memory_order_acquire);
    const std::int32_t reactive_invalidation_enabled = std::atomic_ref<std::int32_t>(state->reactive_invalidation_enabled).load(std::memory_order_acquire);
    const std::int32_t shadow_delta_invalidation_enabled = std::atomic_ref<std::int32_t>(state->shadow_delta_invalidation_enabled).load(std::memory_order_acquire);
    const std::int32_t forward_transparency_enabled = std::atomic_ref<std::int32_t>(state->forward_transparency_enabled).load(std::memory_order_acquire);
    const std::int32_t split_diff_spec_enabled = std::atomic_ref<std::int32_t>(state->split_diff_spec_enabled).load(std::memory_order_acquire);
    const std::int32_t surface_cache_enabled = std::atomic_ref<std::int32_t>(state->surface_cache_enabled).load(std::memory_order_acquire);
    const std::int32_t unified_controller_enabled = std::atomic_ref<std::int32_t>(state->unified_controller_enabled).load(std::memory_order_acquire);
    const std::int32_t unified_controller_tier = std::atomic_ref<std::int32_t>(state->unified_controller_tier).load(std::memory_order_acquire);
    const std::int32_t unified_controller_util_permille = std::atomic_ref<std::int32_t>(state->unified_controller_util_permille).load(std::memory_order_acquire);
    const std::int64_t unified_controller_budget_us = std::atomic_ref<std::int64_t>(state->unified_controller_budget_us).load(std::memory_order_acquire);
    const std::int64_t unified_controller_transitions = std::atomic_ref<std::int64_t>(state->unified_controller_transitions).load(std::memory_order_acquire);
    const std::int64_t unified_controller_oscillations = std::atomic_ref<std::int64_t>(state->unified_controller_oscillations).load(std::memory_order_acquire);
    const std::int32_t par_bypass_active = std::atomic_ref<std::int32_t>(state->par_bypass_active).load(std::memory_order_acquire);
    const std::int32_t smoothed_damage_pct = std::atomic_ref<std::int32_t>(state->smoothed_damage_pct).load(std::memory_order_acquire);
    const std::int32_t render_scale_pct = std::atomic_ref<std::int32_t>(state->render_scale_pct).load(std::memory_order_acquire);
    const std::int32_t render_width = std::atomic_ref<std::int32_t>(state->render_width).load(std::memory_order_acquire);
    const std::int32_t render_height = std::atomic_ref<std::int32_t>(state->render_height).load(std::memory_order_acquire);
    const std::int32_t alloc_render_width = std::atomic_ref<std::int32_t>(state->alloc_render_width).load(std::memory_order_acquire);
    const std::int32_t alloc_render_height = std::atomic_ref<std::int32_t>(state->alloc_render_height).load(std::memory_order_acquire);
    const std::int32_t upscale_filter = std::atomic_ref<std::int32_t>(state->upscale_filter).load(std::memory_order_acquire);
    const std::int32_t drs_mode = std::atomic_ref<std::int32_t>(state->drs_mode).load(std::memory_order_acquire);
    const std::int32_t drs_min_pct = std::atomic_ref<std::int32_t>(state->drs_min_pct).load(std::memory_order_acquire);
    const std::int32_t drs_max_pct = std::atomic_ref<std::int32_t>(state->drs_max_pct).load(std::memory_order_acquire);
    const std::int64_t drs_scale_changes = std::atomic_ref<std::int64_t>(state->drs_scale_changes).load(std::memory_order_acquire);
    const std::int64_t drs_reallocations = std::atomic_ref<std::int64_t>(state->drs_reallocations).load(std::memory_order_acquire);
    const std::int64_t drs_target_scene_budget_us = std::atomic_ref<std::int64_t>(state->drs_target_scene_budget_us).load(std::memory_order_acquire);
    const std::int32_t fixed_dt_us = std::atomic_ref<std::int32_t>(state->fixed_dt_us).load(std::memory_order_acquire);
    const std::int64_t target_scene_budget_us = std::atomic_ref<std::int64_t>(state->last_target_scene_budget_us).load(std::memory_order_acquire);
    const std::int32_t pending_step_frames = std::atomic_ref<std::int32_t>(state->pending_step_frames).load(std::memory_order_acquire);
    const std::uint32_t tile_total = std::atomic_ref<std::uint32_t>(state->last_tile_total_count).load(std::memory_order_acquire);
    const std::uint32_t tile_cached = std::atomic_ref<std::uint32_t>(state->last_tile_cached_count).load(std::memory_order_acquire);
    const std::uint32_t tile_shaded = std::atomic_ref<std::uint32_t>(state->last_tile_shaded_count).load(std::memory_order_acquire);
    const std::uint32_t tile_deferred = std::atomic_ref<std::uint32_t>(state->last_tile_deferred_count).load(std::memory_order_acquire);
    const std::uint32_t tile_crit_total = std::atomic_ref<std::uint32_t>(state->last_tile_critical_total).load(std::memory_order_acquire);
    const std::uint32_t tile_crit_deferred = std::atomic_ref<std::uint32_t>(state->last_tile_critical_deferred).load(std::memory_order_acquire);
    const double smoothed_fps = smoothed_interval_us > 0 ? 1000000.0 / static_cast<double>(smoothed_interval_us) : 0.0;

    RECT client_rectangle{};
    const bool has_client_size = GetClientRect(instance.window, &client_rectangle) != 0;
    std::cout << "{\"ok\":true,\"schema_version\":22,\"process_id\":" << instance.process_id
              << ",\"running\":" << (running ? "true" : "false")
              << ",\"present_calls_completed\":" << present_count
              << ",\"display_frame_id\":" << display_frame_id
              << ",\"source_frame_id\":" << source_frame_id
              << ",\"frame_counts\":{\"rendered\":" << rendered_frames
              << ",\"repeated\":" << repeated_frames
              << ",\"warped\":" << warped_frames
              << ",\"generated\":" << generated_frames
              << ",\"combined\":" << combined_frames
              << ",\"fallback\":" << fallback_frames << "}"
              << ",\"resize_count\":" << resize_count << ",\"client_size_px\":";
    if (has_client_size) std::cout << "{\"width\":" << client_rectangle.right - client_rectangle.left << ",\"height\":" << client_rectangle.bottom - client_rectangle.top << "}";
    else std::cout << "null";
    std::cout << ",\"display\":";
    print_display(instance.window);
    std::cout << ",\"presentation\":{\"mode\":\"" << frame_warp_lab::presentation_mode_name(presentation_mode)
              << "\",\"pacer_mode\":\"" << frame_warp_lab::pacer_mode_name(pacer_mode)
              << "\",\"pacing_owner\":\"" << frame_warp_lab::pacing_owner_name(pacing_owner)
              << "\",\"target_fps\":" << target_fps
              << ",\"target_period_us\":" << target_period_us
              << ",\"missed_deadlines\":" << missed_deadlines
              << ",\"source_divider\":" << source_divider
              << ",\"vsync_enabled\":" << (vsync_enabled ? "true" : "false")
              << ",\"allow_tearing_enabled\":" << (allow_tearing_enabled ? "true" : "false")
              << ",\"waitable_swap_chain\":" << (waitable_swap_chain ? "true" : "false")
              << ",\"async_gpu_enabled\":" << (async_gpu_enabled ? "true" : "false")
              << ",\"high_priority_present_queue\":" << (high_priority_present_queue ? "true" : "false")
              << ",\"async_overlapped_presents\":" << async_overlaps
              << ",\"instrumentation\":\"" << frame_warp_lab::instrumentation_level_name(instrumentation_level) << "\""
              << ",\"present_variant\":\"" << frame_warp_lab::present_variant_name(last_present_variant) << "\""
              << ",\"wait_before_input_sample\":true"
              << ",\"max_frame_latency\":2,\"displayed_frame_timing\":null}"
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
    std::cout << ",\"temporal_recovered_count\":" << temporal_recovered_pixels
              << ",\"temporal_recovered_ratio\":";
    print_ratio(temporal_recovered_pixels, total_pixels);
    std::cout << ",\"note\":\"cpu_present_interval_us and smoothed_present_fps reflect Present return cadence, not physical display scanout\"}"
              << ",\"latency\":{\"raw_input_enabled\":" << (raw_input_enabled ? "true" : "false")
              << ",\"flash_indicator_enabled\":" << (flash_indicator_enabled ? "true" : "false")
              << ",\"input_to_present_us\":";
    print_nullable_int64(input_to_present_us);
    std::cout << ",\"sim_cpu_us\":" << sim_cpu_us
              << ",\"render_submit_cpu_us\":" << render_submit_cpu_us
              << ",\"present_call_cpu_us\":" << present_call_cpu_us
              << ",\"nvidia_pcl_etw\":{\"provider\":\"PCLStatsTraceLoggingProvider\",\"listener_active\":"
              << (pcl_etw_listener_active ? "true" : "false")
              << ",\"markers_emitted\":" << pcl_markers_emitted
              << ",\"pings_received\":" << pcl_pings_received
              << "},\"note\":\"input_to_present_us measures software input arrival to Present return; physical display scanout requires FrameView/PresentMon ETW or Reflex Latency Analyzer\"}"
              << ",\"scene\":{\"name\":\"" << (scene_kind == static_cast<std::int32_t>(frame_warp_lab::SceneKind::range) ? "tactical_range" : "motion_lab")
              << "\",\"kind\":\"" << frame_warp_lab::scene_kind_name(scene_kind)
              << "\",\"time_ms\":" << scene_time_ms
              << ",\"fixed_dt_us\":" << fixed_dt_us
              << ",\"pending_step_frames\":" << pending_step_frames
              << ",\"paused\":" << (scene_paused ? "true" : "false")
              << ",\"camera_path\":\"" << frame_warp_lab::camera_path_name(camera_path)
              << "\",\"camera\":{\"yaw_deg\":" << std::fixed << std::setprecision(2) << (static_cast<double>(camera_yaw_millideg) / 1000.0)
              << ",\"pitch_deg\":" << (static_cast<double>(camera_pitch_millideg) / 1000.0)
              << ",\"mouse_look_active\":" << (mouse_look_active ? "true" : "false")
              << "},\"diagnostic_view\":\"" << frame_warp_lab::diagnostic_view_name(diagnostic_view)
              << "\",\"stress_level\":" << stress_level
              << ",\"shader_flags\":" << shader_flags
              << ",\"pbr_enabled\":" << (pbr_enabled ? "true" : "false")
              << ",\"aniso_mip_enabled\":" << (aniso_mip_enabled ? "true" : "false")
              << ",\"overlay_enabled\":" << (overlay_enabled ? "true" : "false")
              << "},\"tactical_range\":{\"hp\":" << tactical_hp
              << ",\"armor\":" << tactical_armor
              << ",\"ammo_in_mag\":" << tactical_ammo_in_mag
              << ",\"ammo_reserve\":" << tactical_ammo_reserve
              << ",\"shots_fired\":" << tactical_shots_fired
              << ",\"hits\":" << tactical_hits
              << ",\"headshots\":" << tactical_headshots
              << ",\"score\":" << tactical_score
              << ",\"targets_up\":" << tactical_targets_up
              << ",\"ads_active\":" << (tactical_ads_active ? "true" : "false")
              << ",\"hero_pistol_gltf_loaded\":" << (hero_pistol_gltf_loaded ? "true" : "false")
              << ",\"hero_pbr_textures_loaded\":" << (hero_pbr_textures_loaded ? "true" : "false")
              << ",\"spread_mrad\":" << std::fixed << std::setprecision(2) << (static_cast<double>(tactical_spread_mrad_x100) / 100.0)
              << ",\"last_shot_offset_mrad\":" << (static_cast<double>(tactical_shot_offset_mrad_x100) / 100.0)
              << "},\"ecs\":{\"entities\":" << ecs_entity_count
              << ",\"renderables\":" << ecs_renderable_count
              << ",\"physics_bodies\":" << ecs_physics_count
              << ",\"scripts\":" << ecs_script_count
              << "},\"lua\":{\"loaded_scripts\":" << lua_loaded_scripts
              << ",\"reload_count\":" << lua_reload_count
              << ",\"error_count\":" << lua_error_count
              << ",\"last_step_us\":" << lua_last_step_us
              << "},\"events\":{\"dispatched\":" << events_dispatched
              << ",\"subscriptions\":" << event_subscriptions
              << "},\"multithreading\":{\"worker_threads\":" << mt_worker_threads
              << ",\"jolt_threads\":" << mt_jolt_threads
              << ",\"jobs_executed\":" << mt_jobs_executed
              << ",\"physics_step_us\":" << mt_physics_step_us
              << ",\"ecs_sync_us\":" << mt_ecs_sync_us
              << ",\"pbr_build_ms\":" << mt_pbr_build_ms
              << "},\"vrs\":{\"tier\":" << vrs_tier
              << ",\"tile_size_px\":" << vrs_tile_size_px
              << ",\"additional_rates_supported\":" << (vrs_additional_rates ? "true" : "false")
              << ",\"requested_mode\":\"" << frame_warp_lab::vrs_mode_name(vrs_requested_mode)
              << "\",\"active_mode\":\"" << frame_warp_lab::vrs_mode_name(vrs_active_mode) << "\"}"
              << ",\"adaptive_scheduler\":{\"enabled\":" << (adaptive_enabled ? "true" : "false")
              << ",\"dlssg_policy_enabled\":" << (adaptive_dlssg_enabled ? "true" : "false")
              << ",\"tier\":" << adaptive_tier
              << ",\"reason\":\"" << frame_warp_lab::scheduler_reason_name(adaptive_reason)
              << "\",\"reserved_headroom_us\":" << adaptive_reserved_headroom_us
              << ",\"gpu_budget_utilization\":" << std::fixed << std::setprecision(3) << (static_cast<double>(adaptive_utilization_permille) / 1000.0)
              << ",\"decision_count\":" << adaptive_decision_count << "}"
              << ",\"unified_controller\":{\"enabled\":" << (unified_controller_enabled ? "true" : "false")
              << ",\"tier\":" << unified_controller_tier
              << ",\"target_scene_budget_us\":" << unified_controller_budget_us
              << ",\"utilization\":" << std::fixed << std::setprecision(3) << (static_cast<double>(unified_controller_util_permille) / 1000.0)
              << ",\"transitions\":" << unified_controller_transitions
              << ",\"oscillations\":" << unified_controller_oscillations << "}"
              << ",\"dlss_g\":{\"streamline_initialized\":" << (streamline_initialized ? "true" : "false")
              << ",\"supported\":" << (dlssg_supported ? "true" : "false")
              << ",\"dlss_sr_supported\":" << (dlss_sr_supported ? "true" : "false")
              << ",\"reflex_supported\":" << (reflex_supported ? "true" : "false")
              << ",\"dynamic_mfg_supported\":" << (dlssg_dynamic_mfg_supported ? "true" : "false")
              << ",\"max_frames_to_generate\":" << dlssg_max_frames_to_generate
              << ",\"max_multiplier\":" << (dlssg_max_frames_to_generate > 0 ? dlssg_max_frames_to_generate + 1 : 0)
              << ",\"requested_multiplier\":";
    if (dlssg_requested_multiplier == 0) std::cout << "\"dynamic\"";
    else std::cout << dlssg_requested_multiplier;
    std::cout << ",\"active\":" << (dlssg_active ? "true" : "false")
              << ",\"window_focused\":" << (window_focused ? "true" : "false")
              << ",\"status_flags\":" << dlssg_status_flags
              << ",\"api_error_count\":" << dlssg_api_error_count
              << ",\"last_api_result\":" << dlssg_last_api_result
              << ",\"last_failed_call\":\"" << frame_warp_lab::streamline_call_name(dlssg_last_failed_call) << "\""
              << ",\"warp_guard_active\":" << (dlssg_warp_guard_active ? "true" : "false")
              << ",\"warp_guard_trips\":" << dlssg_warp_guard_trips
              << ",\"last_frames_presented\":" << dlssg_last_frames_presented
              << ",\"total_frames_presented\":" << dlssg_total_frames_presented
              << ",\"total_generated_frames\":" << dlssg_total_generated_frames << "}"
              << ",\"selective_tiles\":{\"enabled\":" << (selective_tiles_enabled ? "true" : "false")
              << ",\"policy\":\"" << frame_warp_lab::tile_policy_name(tile_policy) << "\""
              << ",\"auto_budget_enabled\":" << (auto_tile_budget_enabled ? "true" : "false")
              << ",\"cache_sharpen_enabled\":" << (cache_sharpen_enabled ? "true" : "false")
              << ",\"coarse_fallback_enabled\":" << (coarse_fallback_enabled ? "true" : "false")
              << ",\"subtile_interleave_enabled\":" << (subtile_interleave_enabled ? "true" : "false")
              << ",\"reactive_invalidation_enabled\":" << (reactive_invalidation_enabled ? "true" : "false")
              << ",\"shadow_delta_invalidation_enabled\":" << (shadow_delta_invalidation_enabled ? "true" : "false")
              << ",\"forward_transparency_enabled\":" << (forward_transparency_enabled ? "true" : "false")
              << ",\"split_diff_spec_enabled\":" << (split_diff_spec_enabled ? "true" : "false")
              << ",\"surface_cache_enabled\":" << (surface_cache_enabled ? "true" : "false")
              << ",\"par_bypass_active\":" << (par_bypass_active ? "true" : "false")
              << ",\"smoothed_damage_pct\":" << smoothed_damage_pct
              << ",\"target_scene_budget_us\":" << target_scene_budget_us
              << ",\"tile_size_px\":16"
              << ",\"budget_pct\":" << tile_budget_pct
              << ",\"total_tiles\":" << tile_total
              << ",\"cached_tiles\":" << tile_cached
              << ",\"shaded_tiles\":" << tile_shaded
              << ",\"deferred_tiles\":" << tile_deferred
              << ",\"shaded_tile_ratio\":";
    print_ratio(tile_shaded, tile_total);
    std::cout << ",\"critical_total_tiles\":" << tile_crit_total
              << ",\"critical_deferred_tiles\":" << tile_crit_deferred
              << ",\"critical_coverage_ratio\":";
    print_ratio(tile_crit_total >= tile_crit_deferred ? tile_crit_total - tile_crit_deferred : 0U, tile_crit_total);
    std::cout << "}"
              << ",\"offscreen_render\":{\"width\":" << render_width
              << ",\"height\":" << render_height
              << ",\"alloc_width\":" << alloc_render_width
              << ",\"alloc_height\":" << alloc_render_height
              << ",\"scale_pct\":" << render_scale_pct
              << ",\"upscale_filter\":\"" << frame_warp_lab::upscale_filter_name(upscale_filter) << "\""
              << ",\"drs_mode\":\"" << frame_warp_lab::drs_mode_name(drs_mode) << "\""
              << ",\"drs_min_pct\":" << drs_min_pct
              << ",\"drs_max_pct\":" << drs_max_pct
              << ",\"scale_changes\":" << drs_scale_changes
              << ",\"reallocations\":" << drs_reallocations
              << ",\"target_scene_budget_us\":" << drs_target_scene_budget_us << "}"
              << ",\"features\":{\"scene\":\"cpu_camera_gbuffer\",\"offscreen_upscaling\":\"zero_alloc_per_frame_drs_subrect_bilinear_catmull_cas_nearest\",\"selective_tiles\":\"par3_subtile_interleave_damage_aware_bypass_trm_coarse\",\"hud\":\"2d_frametime_graph_and_readout\",\"async_gpu\":\"dual_queue_3slot_ring\",\"reprojection\":\"depth_camera_warp\",\"nvidia_pcl_markers\":\"etw_and_streamline_pcl\",\"frame_generation\":\"mv_extrapolation_temporal_and_dlss_g\",\"dlss_g_inputs\":\"3slot_aligned_hudless_clipdepth_motion\",\"vrs\":\""
              << (vrs_tier >= 2 ? "tier1_and_tier2_foveated_image" : vrs_tier == 1 ? "tier1_per_draw" : "unsupported")
              << "\",\"adaptive_scheduler\":\"unified_vrs_dlss_warp_ladder\",\"ipc\":\"seqlock_v26\",\"shaders\":\"dxc_sm6_0_pbr_ggx_multiscatter_clearcoat_ibl_16x_aniso_samplegrad\"}}\n";
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
    std::cout << "{\"ok\":true,\"schema_version\":22,\"process_id\":" << instance.process_id
              << ",\"capacity\":" << frame_warp_lab::runtime_event_capacity << ",\"total_events\":" << event_count << ",\"events\":[";
    bool first_event = true;
    for (std::int32_t sequence = first_sequence; sequence <= event_count; ++sequence) {
        const auto& event_slot = state->events[(static_cast<std::size_t>(sequence) - 1) % frame_warp_lab::runtime_event_capacity];
        frame_warp_lab::RuntimeEvent event{};
        if (!frame_warp_lab::seqlock_try_read(event_slot, event) || event.sequence != sequence) continue;
        FILETIME file_time{event.timestamp_low, event.timestamp_high};
        SYSTEMTIME utc{};
        if (!FileTimeToSystemTime(&file_time, &utc)) continue;
        char timestamp[32]{};
        std::snprintf(timestamp, sizeof(timestamp), "%04u-%02u-%02uT%02u:%02u:%02u.%03uZ", utc.wYear, utc.wMonth, utc.wDay, utc.wHour, utc.wMinute, utc.wSecond, utc.wMilliseconds);
        if (!first_event) std::cout << ',';
        first_event = false;
        std::cout << "{\"sequence\":" << sequence << ",\"timestamp_utc\":\"" << timestamp
                  << "\",\"severity\":\"info\",\"subsystem\":\"runtime\",\"event\":\"" << frame_warp_lab::runtime_event_name(event.event_type)
                  << "\",\"key\":\"" << frame_warp_lab::config_key_name(event.config_key)
                  << "\",\"value\":" << event.value << "}";
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
    std::vector<std::int64_t> input_to_presents;
    std::vector<std::int64_t> sim_cpu_times;
    std::vector<std::int64_t> render_submit_cpu_times;
    std::vector<std::int64_t> present_call_cpu_times;
    std::vector<std::int64_t> scene_gpu_times;
    std::vector<std::int64_t> present_gpu_times;
    std::vector<double> invalid_ratios;
    std::vector<double> temporal_recovered_ratios;
    std::vector<double> shaded_tile_ratios;
    std::int64_t total_deferred_tiles = 0;
    std::int64_t total_critical_tiles = 0;
    std::vector<std::int64_t> render_scales;
    std::int64_t total_critical_deferred_tiles = 0;
    std::int64_t rendered_count = 0;
    std::int64_t repeated_count = 0;
    std::int64_t warped_count = 0;
    std::int64_t generated_count = 0;
    std::int64_t combined_count = 0;
    std::int64_t dlss_generated_count = 0;
    std::int64_t dlss_warped_count = 0;
    std::int64_t fallback_count = 0;
    std::int64_t async_overlap_count = 0;
    std::int64_t missed_deadline_count = 0;
    std::int64_t total_dlssg_presented = 0;

    for (std::int64_t sequence = first_sequence; sequence <= total_samples; ++sequence) {
        if (sequence <= 0) continue;
        const auto& slot = state->trace[(static_cast<std::size_t>(sequence) - 1) % frame_warp_lab::runtime_frame_trace_capacity];
        frame_warp_lab::RuntimeFrameSample copy{};
        if (!frame_warp_lab::seqlock_try_read(slot, copy) || copy.sequence != sequence) continue;
        captured.push_back(copy);
        if (copy.present_interval_us > 0) present_intervals.push_back(copy.present_interval_us);
        if (copy.source_age_us >= 0) source_ages.push_back(copy.source_age_us);
        if (copy.input_to_present_us >= 0) input_to_presents.push_back(copy.input_to_present_us);
        if (copy.sim_cpu_us >= 0) sim_cpu_times.push_back(copy.sim_cpu_us);
        if (copy.render_submit_cpu_us >= 0) render_submit_cpu_times.push_back(copy.render_submit_cpu_us);
        if (copy.present_call_cpu_us >= 0) present_call_cpu_times.push_back(copy.present_call_cpu_us);
        if (copy.scene_gpu_us >= 0) scene_gpu_times.push_back(copy.scene_gpu_us);
        if (copy.present_gpu_us >= 0) present_gpu_times.push_back(copy.present_gpu_us);
        if (copy.render_scale_pct > 0) render_scales.push_back(static_cast<std::int64_t>(copy.render_scale_pct));
        if (copy.gpu_slot_warped && copy.total_pixel_count > 0) {
            invalid_ratios.push_back(static_cast<double>(copy.invalid_pixel_count) / static_cast<double>(copy.total_pixel_count));
            temporal_recovered_ratios.push_back(static_cast<double>(copy.temporal_recovered_count) / static_cast<double>(copy.total_pixel_count));
        }
        if (copy.tile_total_count > 0) {
            shaded_tile_ratios.push_back(static_cast<double>(copy.tile_shaded_count) / static_cast<double>(copy.tile_total_count));
            total_deferred_tiles += static_cast<std::int64_t>(copy.tile_deferred_count);
            total_critical_tiles += static_cast<std::int64_t>(copy.tile_critical_total);
            total_critical_deferred_tiles += static_cast<std::int64_t>(copy.tile_critical_deferred);
        }
        switch (static_cast<frame_warp_lab::FrameKind>(copy.frame_kind)) {
            case frame_warp_lab::FrameKind::rendered: ++rendered_count; break;
            case frame_warp_lab::FrameKind::repeated: ++repeated_count; break;
            case frame_warp_lab::FrameKind::warped: ++warped_count; break;
            case frame_warp_lab::FrameKind::fallback: ++fallback_count; break;
            case frame_warp_lab::FrameKind::generated: ++generated_count; break;
            case frame_warp_lab::FrameKind::combined: ++combined_count; break;
            case frame_warp_lab::FrameKind::dlss_generated: ++dlss_generated_count; break;
            case frame_warp_lab::FrameKind::dlss_warped: ++dlss_warped_count; break;
        }
        total_dlssg_presented += static_cast<std::int64_t>(copy.dlssg_frames_presented);
        if (copy.async_overlapped) ++async_overlap_count;
        if (copy.missed_deadline) ++missed_deadline_count;
    }

    std::cout << "{\"ok\":true,\"schema_version\":22,\"process_id\":" << instance.process_id
              << ",\"capacity\":" << frame_warp_lab::runtime_frame_trace_capacity
              << ",\"total_frames_recorded\":" << total_samples
              << ",\"sample_count\":" << captured.size()
              << ",\"units\":\"microseconds\""
              << ",\"note\":\"cpu_present_interval_us is Present entry cadence; input_to_present_us is software input arrival to Present return; GPU timings come from completed queue slots\""
              << ",\"summary\":{\"frame_kinds\":{\"rendered\":" << rendered_count
              << ",\"repeated\":" << repeated_count
              << ",\"warped\":" << warped_count
              << ",\"generated\":" << generated_count
              << ",\"combined\":" << combined_count
              << ",\"dlss_generated\":" << dlss_generated_count
              << ",\"dlss_warped\":" << dlss_warped_count
              << ",\"fallback\":" << fallback_count
              << "},\"dlssg_frames_presented\":" << total_dlssg_presented
              << ",\"async_overlaps\":" << async_overlap_count
              << ",\"missed_deadlines\":" << missed_deadline_count
              << ",\"cpu_present_interval_us\":";
    print_metric_summary(present_intervals);
    std::cout << ",\"cpu_source_age_us\":";
    print_metric_summary(source_ages);
    std::cout << ",\"input_to_present_us\":";
    print_metric_summary(input_to_presents);
    std::cout << ",\"sim_cpu_us\":";
    print_metric_summary(sim_cpu_times);
    std::cout << ",\"render_submit_cpu_us\":";
    print_metric_summary(render_submit_cpu_times);
    std::cout << ",\"present_call_cpu_us\":";
    print_metric_summary(present_call_cpu_times);
    std::cout << ",\"scene_gpu_us\":";
    print_metric_summary(scene_gpu_times);
    std::cout << ",\"present_gpu_us\":";
    print_metric_summary(present_gpu_times);
    std::cout << ",\"render_scale_pct\":";
    print_metric_summary(render_scales);
    std::cout << ",\"invalid_pixel_ratio\":";
    print_double_summary(invalid_ratios);
    std::cout << ",\"temporal_recovered_ratio\":";
    print_double_summary(temporal_recovered_ratios);
    std::cout << ",\"shaded_tile_ratio\":";
    print_double_summary(shaded_tile_ratios);
    std::cout << ",\"total_deferred_tiles\":" << total_deferred_tiles
              << ",\"total_critical_tiles\":" << total_critical_tiles
              << ",\"total_critical_deferred_tiles\":" << total_critical_deferred_tiles
              << ",\"critical_coverage_ratio\":";
    if (total_critical_tiles <= 0) {
        std::cout << "null";
    } else {
        const double cov = static_cast<double>(std::max<std::int64_t>(0, total_critical_tiles - total_critical_deferred_tiles))
            / static_cast<double>(total_critical_tiles);
        std::cout << std::fixed << std::setprecision(6) << cov;
    }
    std::cout << "}";

    if (include_samples) {
        std::cout << ",\"samples\":[";
        for (std::size_t index = 0; index < captured.size(); ++index) {
            const auto& sample = captured[index];
            if (index) std::cout << ',';
            std::cout << "{\"sequence\":" << sample.sequence
                      << ",\"display_frame_id\":" << sample.display_frame_id
                      << ",\"source_frame_id\":" << sample.source_frame_id
                      << ",\"gpu_measured_display_frame_id\":";
            print_nullable_int64(sample.gpu_measured_display_frame_id);
            std::cout << ",\"gpu_measured_source_frame_id\":";
            print_nullable_int64(sample.gpu_measured_source_frame_id);
            std::cout << ",\"mode\":\"" << frame_warp_lab::presentation_mode_name(sample.presentation_mode)
                      << "\",\"frame_kind\":\"" << frame_warp_lab::frame_kind_name(sample.frame_kind)
                      << "\",\"fallback_reason\":\"" << frame_warp_lab::fallback_reason_name(sample.fallback_reason)
                      << "\",\"source_rendered\":" << (sample.source_rendered ? "true" : "false")
                      << ",\"source_divider\":" << sample.source_divider
                      << ",\"cpu_present_interval_us\":" << sample.present_interval_us
                      << ",\"cpu_source_age_us\":" << sample.source_age_us
                      << ",\"input_to_present_us\":";
            print_nullable_int64(sample.input_to_present_us);
            std::cout << ",\"sim_cpu_us\":" << sample.sim_cpu_us
                      << ",\"render_submit_cpu_us\":" << sample.render_submit_cpu_us
                      << ",\"present_call_cpu_us\":" << sample.present_call_cpu_us
                      << ",\"scene_gpu_us\":";
            print_nullable_int64(sample.scene_gpu_us);
            std::cout << ",\"present_gpu_us\":";
            print_nullable_int64(sample.present_gpu_us);
            std::cout << ",\"invalid_pixel_count\":" << sample.invalid_pixel_count
                      << ",\"invalid_pixel_ratio\":";
            print_ratio(sample.invalid_pixel_count, sample.total_pixel_count);
            std::cout << ",\"temporal_recovered_count\":" << sample.temporal_recovered_count
                      << ",\"temporal_recovered_ratio\":";
            print_ratio(sample.temporal_recovered_count, sample.total_pixel_count);
            std::cout << ",\"diagnostic_view\":\"" << frame_warp_lab::diagnostic_view_name(sample.diagnostic_view)
                      << "\",\"camera_path\":\"" << frame_warp_lab::camera_path_name(sample.camera_path)
                      << "\",\"stress_level\":" << sample.stress_level
                      << ",\"vrs_mode\":\"" << frame_warp_lab::vrs_mode_name(sample.vrs_mode)
                      << "\",\"adaptive_tier\":" << sample.adaptive_tier
                      << ",\"dlssg_frames_presented\":" << sample.dlssg_frames_presented
                      << ",\"tile_total_count\":" << sample.tile_total_count
                      << ",\"tile_shaded_count\":" << sample.tile_shaded_count
                      << ",\"tile_deferred_count\":" << sample.tile_deferred_count
                      << ",\"tile_critical_total\":" << sample.tile_critical_total
                      << ",\"tile_critical_deferred\":" << sample.tile_critical_deferred
                      << ",\"shaded_tile_ratio\":";
            print_ratio(sample.tile_shaded_count, sample.tile_total_count);
            std::cout << ",\"render_scale_pct\":" << sample.render_scale_pct
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
        else if (value == L"frame_gen") parameter = static_cast<LPARAM>(frame_warp_lab::PresentationMode::frame_gen);
        else if (value == L"combined") parameter = static_cast<LPARAM>(frame_warp_lab::PresentationMode::combined);
        else if (value == L"dlss_g") parameter = static_cast<LPARAM>(frame_warp_lab::PresentationMode::dlss_g);
        else if (value == L"dlss_g_warp") parameter = static_cast<LPARAM>(frame_warp_lab::PresentationMode::dlss_g_warp);
        else return fail("invalid_value", "Expected mode in {full_render, repeat, reproject, frame_gen, combined, dlss_g, dlss_g_warp}");
    } else if (key == L"dlssg_mult") {
        command = frame_warp_lab::ControlCommand::set_dlssg_multiplier;
        if (value == L"dynamic") {
            parameter = 0;
        } else {
            unsigned long parsed = 0;
            if (!parse_unsigned_long(value.c_str(), parsed) || parsed < 2 || parsed > 6) {
                return fail("invalid_value", "Expected dlssg_mult in {2, 3, 4, 5, 6, dynamic}");
            }
            parameter = static_cast<LPARAM>(parsed);
        }
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
        else if (value == L"vrs") parameter = static_cast<LPARAM>(frame_warp_lab::DiagnosticView::vrs_mask);
        else if (value == L"tiles") parameter = static_cast<LPARAM>(frame_warp_lab::DiagnosticView::tile_cache);
        else if (value == L"priority") parameter = static_cast<LPARAM>(frame_warp_lab::DiagnosticView::tile_priority);
        else return fail("invalid_value", "Expected view in {final, depth, motion, holes, vrs, tiles, priority}");
    } else if (key == L"path") {
        command = frame_warp_lab::ControlCommand::set_camera_path;
        if (value == L"corridor_sway") parameter = static_cast<LPARAM>(frame_warp_lab::CameraPath::corridor_sway);
        else if (value == L"fast_pan") parameter = static_cast<LPARAM>(frame_warp_lab::CameraPath::fast_pan);
        else if (value == L"occlusion_sweep") parameter = static_cast<LPARAM>(frame_warp_lab::CameraPath::occlusion_sweep);
        else if (value == L"camera_cut") parameter = static_cast<LPARAM>(frame_warp_lab::CameraPath::camera_cut);
        else if (value == L"manual") parameter = static_cast<LPARAM>(frame_warp_lab::CameraPath::manual);
        else if (value == L"pure_rotation") parameter = static_cast<LPARAM>(frame_warp_lab::CameraPath::pure_rotation);
        else return fail("invalid_value", "Expected path in {corridor_sway, fast_pan, occlusion_sweep, camera_cut, manual, pure_rotation}");
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
    } else if (key == L"flash") {
        command = frame_warp_lab::ControlCommand::set_flash_indicator;
        if (value == L"on" || value == L"1" || value == L"true") parameter = 1;
        else if (value == L"off" || value == L"0" || value == L"false") parameter = 0;
        else return fail("invalid_value", "Expected flash in {on, off}");
    } else if (key == L"pacer") {
        command = frame_warp_lab::ControlCommand::set_pacer_mode;
        if (value == L"deadline") parameter = static_cast<LPARAM>(frame_warp_lab::PacerMode::deadline);
        else if (value == L"vsync_only") parameter = static_cast<LPARAM>(frame_warp_lab::PacerMode::vsync_only);
        else if (value == L"unlocked") parameter = static_cast<LPARAM>(frame_warp_lab::PacerMode::unlocked);
        else return fail("invalid_value", "Expected pacer in {deadline, vsync_only, unlocked}");
    } else if (key == L"target_fps") {
        unsigned long parsed = 0;
        if (!parse_unsigned_long(value.c_str(), parsed) || parsed > static_cast<unsigned long>(frame_warp_lab::max_target_fps) || (parsed > 0 && parsed < 30)) {
            return fail("invalid_value", "Expected target_fps of 0 (display refresh) or integer in [30, 500]");
        }
        command = frame_warp_lab::ControlCommand::set_target_fps;
        parameter = static_cast<LPARAM>(parsed);
    } else if (key == L"async_gpu") {
        command = frame_warp_lab::ControlCommand::set_async_gpu;
        if (value == L"on" || value == L"1" || value == L"true") parameter = 1;
        else if (value == L"off" || value == L"0" || value == L"false") parameter = 0;
        else return fail("invalid_value", "Expected async_gpu in {on, off}");
    } else if (key == L"vrs") {
        command = frame_warp_lab::ControlCommand::set_vrs_mode;
        if (value == L"off") parameter = static_cast<LPARAM>(frame_warp_lab::VrsMode::off);
        else if (value == L"uniform_2x2") parameter = static_cast<LPARAM>(frame_warp_lab::VrsMode::uniform_2x2);
        else if (value == L"foveated") parameter = static_cast<LPARAM>(frame_warp_lab::VrsMode::foveated);
        else return fail("invalid_value", "Expected vrs in {off, uniform_2x2, foveated}");
    } else if (key == L"adaptive") {
        command = frame_warp_lab::ControlCommand::set_adaptive_scheduler;
        if (value == L"on" || value == L"1" || value == L"true") parameter = 1;
        else if (value == L"off" || value == L"0" || value == L"false") parameter = 0;
        else return fail("invalid_value", "Expected adaptive in {on, off}");
    } else if (key == L"adaptive_dlssg") {
        command = frame_warp_lab::ControlCommand::set_adaptive_dlssg;
        if (value == L"on" || value == L"1" || value == L"true") parameter = 1;
        else if (value == L"off" || value == L"0" || value == L"false") parameter = 0;
        else return fail("invalid_value", "Expected adaptive_dlssg in {on, off}");
    } else if (key == L"instrumentation") {
        command = frame_warp_lab::ControlCommand::set_instrumentation_level;
        if (value == L"minimal") parameter = static_cast<LPARAM>(frame_warp_lab::InstrumentationLevel::minimal);
        else if (value == L"standard") parameter = static_cast<LPARAM>(frame_warp_lab::InstrumentationLevel::standard);
        else if (value == L"diagnostic") parameter = static_cast<LPARAM>(frame_warp_lab::InstrumentationLevel::diagnostic);
        else return fail("invalid_value", "Expected instrumentation in {minimal, standard, diagnostic}");
    } else if (key == L"selective_tiles") {
        command = frame_warp_lab::ControlCommand::set_selective_tiles;
        if (value == L"on" || value == L"1" || value == L"true") parameter = 1;
        else if (value == L"off" || value == L"0" || value == L"false") parameter = 0;
        else return fail("invalid_value", "Expected selective_tiles in {on, off}");
    } else if (key == L"tile_budget") {
        unsigned long parsed = 0;
        if (!parse_unsigned_long(value.c_str(), parsed) || parsed < 5 || parsed > 100) {
            return fail("invalid_value", "Expected tile_budget integer in [5, 100]");
        }
        command = frame_warp_lab::ControlCommand::set_tile_budget_pct;
        parameter = static_cast<LPARAM>(parsed);
    } else if (key == L"tile_policy") {
        command = frame_warp_lab::ControlCommand::set_tile_policy;
        if (value == L"fifo") parameter = static_cast<LPARAM>(frame_warp_lab::TilePolicy::fifo);
        else if (value == L"priority_stbn") parameter = static_cast<LPARAM>(frame_warp_lab::TilePolicy::priority_stbn);
        else return fail("invalid_value", "Expected tile_policy in {fifo, priority_stbn}");
    } else if (key == L"auto_tile_budget") {
        command = frame_warp_lab::ControlCommand::set_auto_tile_budget;
        if (value == L"on" || value == L"1" || value == L"true") parameter = 1;
        else if (value == L"off" || value == L"0" || value == L"false") parameter = 0;
        else return fail("invalid_value", "Expected auto_tile_budget in {on, off}");
    } else if (key == L"cache_sharpen") {
        command = frame_warp_lab::ControlCommand::set_cache_sharpen;
        if (value == L"on" || value == L"1" || value == L"true") parameter = 1;
        else if (value == L"off" || value == L"0" || value == L"false") parameter = 0;
        else return fail("invalid_value", "Expected cache_sharpen in {on, off}");
    } else if (key == L"coarse_fallback") {
        command = frame_warp_lab::ControlCommand::set_coarse_fallback;
        if (value == L"on" || value == L"1" || value == L"true") parameter = 1;
        else if (value == L"off" || value == L"0" || value == L"false") parameter = 0;
        else return fail("invalid_value", "Expected coarse_fallback in {on, off}");
    } else if (key == L"subtile_interleave") {
        command = frame_warp_lab::ControlCommand::set_subtile_interleave;
        if (value == L"on" || value == L"1" || value == L"true") parameter = 1;
        else if (value == L"off" || value == L"0" || value == L"false") parameter = 0;
        else return fail("invalid_value", "Expected subtile_interleave in {on, off}");
    } else if (key == L"reactive_invalidation") {
        command = frame_warp_lab::ControlCommand::set_reactive_invalidation;
        if (value == L"on" || value == L"1" || value == L"true") parameter = 1;
        else if (value == L"off" || value == L"0" || value == L"false") parameter = 0;
        else return fail("invalid_value", "Expected reactive_invalidation in {on, off}");
    } else if (key == L"shadow_delta_invalidation") {
        command = frame_warp_lab::ControlCommand::set_shadow_delta_invalidation;
        if (value == L"on" || value == L"1" || value == L"true") parameter = 1;
        else if (value == L"off" || value == L"0" || value == L"false") parameter = 0;
        else return fail("invalid_value", "Expected shadow_delta_invalidation in {on, off}");
    } else if (key == L"forward_transparency") {
        command = frame_warp_lab::ControlCommand::set_forward_transparency;
        if (value == L"on" || value == L"1" || value == L"true") parameter = 1;
        else if (value == L"off" || value == L"0" || value == L"false") parameter = 0;
        else return fail("invalid_value", "Expected forward_transparency in {on, off}");
    } else if (key == L"split_diff_spec") {
        command = frame_warp_lab::ControlCommand::set_split_diff_spec;
        if (value == L"on" || value == L"1" || value == L"true") parameter = 1;
        else if (value == L"off" || value == L"0" || value == L"false") parameter = 0;
        else return fail("invalid_value", "Expected split_diff_spec in {on, off}");
    } else if (key == L"surface_cache") {
        command = frame_warp_lab::ControlCommand::set_surface_cache;
        if (value == L"on" || value == L"1" || value == L"true") parameter = 1;
        else if (value == L"off" || value == L"0" || value == L"false") parameter = 0;
        else return fail("invalid_value", "Expected surface_cache in {on, off}");
    } else if (key == L"unified_controller") {
        command = frame_warp_lab::ControlCommand::set_unified_controller;
        if (value == L"on" || value == L"1" || value == L"true") parameter = 1;
        else if (value == L"off" || value == L"0" || value == L"false") parameter = 0;
        else return fail("invalid_value", "Expected unified_controller in {on, off}");
    } else if (key == L"shader_feature") {
        command = frame_warp_lab::ControlCommand::set_shader_flags;
        unsigned long parsed = 0;
        if (value == L"none") parameter = 0;
        else if (value == L"hf_checker") parameter = 1;
        else if (value == L"anim_uv") parameter = 2;
        else if (value == L"specular") parameter = 4;
        else if (value == L"moving_light") parameter = 8;
        else if (value == L"emissive") parameter = 16;
        else if (value == L"hard_shadows") parameter = 32;
        else if (value == L"normal_map") parameter = 64;
        else if (value == L"transparent") parameter = 128;
        else if (value == L"all") parameter = 255;
        else if (parse_unsigned_long(value.c_str(), parsed) && parsed <= 255UL) parameter = static_cast<LPARAM>(parsed);
        else return fail("invalid_value", "Expected shader_feature in {none, hf_checker, anim_uv, specular, moving_light, emissive, hard_shadows, normal_map, transparent, all, 0..255}");
    } else if (key == L"pbr") {
        command = frame_warp_lab::ControlCommand::set_pbr_enabled;
        if (value == L"on" || value == L"1" || value == L"true") parameter = 1;
        else if (value == L"off" || value == L"0" || value == L"false") parameter = 0;
        else return fail("invalid_value", "Expected pbr in {on, off}");
    } else if (key == L"aniso_mip") {
        command = frame_warp_lab::ControlCommand::set_aniso_mip_enabled;
        if (value == L"on" || value == L"1" || value == L"true") parameter = 1;
        else if (value == L"off" || value == L"0" || value == L"false") parameter = 0;
        else return fail("invalid_value", "Expected aniso_mip in {on, off}");
    } else if (key == L"scene") {
        command = frame_warp_lab::ControlCommand::set_scene_kind;
        if (value == L"corridor") parameter = static_cast<LPARAM>(frame_warp_lab::SceneKind::corridor);
        else if (value == L"range") parameter = static_cast<LPARAM>(frame_warp_lab::SceneKind::range);
        else return fail("invalid_value", "Expected scene in {range, corridor}");
    } else if (key == L"ads") {
        command = frame_warp_lab::ControlCommand::set_range_ads;
        if (value == L"on" || value == L"1" || value == L"true") parameter = 1;
        else if (value == L"off" || value == L"0" || value == L"false") parameter = 0;
        else return fail("invalid_value", "Expected ads in {on, off}");
    } else if (key == L"render_scale") {
        unsigned long parsed = 0;
        if (!parse_unsigned_long(value.c_str(), parsed) || parsed < 25UL || parsed > 200UL) {
            return fail("invalid_value", "Expected render_scale integer in [25, 200]");
        }
        command = frame_warp_lab::ControlCommand::set_render_scale;
        parameter = static_cast<LPARAM>(parsed);
    } else if (key == L"upscale_filter") {
        command = frame_warp_lab::ControlCommand::set_upscale_filter;
        if (value == L"bilinear") parameter = static_cast<LPARAM>(frame_warp_lab::UpscaleFilter::bilinear);
        else if (value == L"catmull_rom") parameter = static_cast<LPARAM>(frame_warp_lab::UpscaleFilter::catmull_rom);
        else if (value == L"cas") parameter = static_cast<LPARAM>(frame_warp_lab::UpscaleFilter::cas);
        else if (value == L"nearest") parameter = static_cast<LPARAM>(frame_warp_lab::UpscaleFilter::nearest);
        else return fail("invalid_value", "Expected upscale_filter in {bilinear, catmull_rom, cas, nearest}");
    } else if (key == L"drs") {
        command = frame_warp_lab::ControlCommand::set_drs_mode;
        if (value == L"off" || value == L"0" || value == L"false") parameter = static_cast<LPARAM>(frame_warp_lab::DrsMode::off);
        else if (value == L"auto" || value == L"1" || value == L"true") parameter = static_cast<LPARAM>(frame_warp_lab::DrsMode::auto_deadline);
        else if (value == L"sweep") parameter = static_cast<LPARAM>(frame_warp_lab::DrsMode::sweep);
        else return fail("invalid_value", "Expected drs in {off, auto, sweep}");
    } else if (key == L"drs_min") {
        unsigned long parsed = 0;
        if (!parse_unsigned_long(value.c_str(), parsed) || parsed < 25UL || parsed > 200UL) {
            return fail("invalid_value", "Expected drs_min integer in [25, 200]");
        }
        command = frame_warp_lab::ControlCommand::set_drs_min_pct;
        parameter = static_cast<LPARAM>(parsed);
    } else if (key == L"drs_max") {
        unsigned long parsed = 0;
        if (!parse_unsigned_long(value.c_str(), parsed) || parsed < 25UL || parsed > 200UL) {
            return fail("invalid_value", "Expected drs_max integer in [25, 200]");
        }
        command = frame_warp_lab::ControlCommand::set_drs_max_pct;
        parameter = static_cast<LPARAM>(parsed);
    } else if (key == L"fixed_dt_us") {
        unsigned long parsed = 0;
        if (!parse_unsigned_long(value.c_str(), parsed) || parsed > 100000UL) {
            return fail("invalid_value", "Expected fixed_dt_us integer in [0, 100000]");
        }
        command = frame_warp_lab::ControlCommand::set_fixed_dt_us;
        parameter = static_cast<LPARAM>(parsed);
    } else {
        return fail("unknown_setting", "Supported settings are: mode, dlssg_mult, pacer, target_fps, async_gpu, vrs, selective_tiles, tile_budget, tile_policy, auto_tile_budget, cache_sharpen, coarse_fallback, subtile_interleave, shader_feature, render_scale, upscale_filter, drs, drs_min, drs_max, fixed_dt_us, adaptive, adaptive_dlssg, instrumentation, divider, vsync, view, path, stress, overlay, flash");
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

int capture_instance_frame(const WindowInstance& instance, std::uint32_t tag, std::int32_t step_count = 0) {
    RuntimeStateView state_view;
    const char* error_code = nullptr;
    const char* error_message = nullptr;
    if (!state_view.open(instance.process_id, error_code, error_message)) return fail(error_code, error_message);
    if (step_count > 0) {
        if (!PostMessageW(instance.window, frame_warp_lab::control_message, static_cast<WPARAM>(frame_warp_lab::ControlCommand::step_frames), static_cast<LPARAM>(step_count))) {
            return fail("control_failed", "Could not send step request");
        }
    }
    if (!PostMessageW(instance.window, frame_warp_lab::control_message, static_cast<WPARAM>(frame_warp_lab::ControlCommand::capture_frame), static_cast<LPARAM>(tag))) {
        return fail("control_failed", "Could not send capture request");
    }
    auto* state = state_view.get();
    for (int attempt = 0; attempt < 600; ++attempt) {
        const std::uint32_t completed_tag = std::atomic_ref<std::uint32_t>(state->last_capture_tag).load(std::memory_order_acquire);
        if (completed_tag == tag) {
            const std::int64_t frame_id = std::atomic_ref<std::int64_t>(state->last_capture_frame_id).load(std::memory_order_acquire);
            const std::int64_t scene_time_us = std::atomic_ref<std::int64_t>(state->last_capture_scene_time_us).load(std::memory_order_acquire);
            wchar_t exe_path[32768]{};
            GetModuleFileNameW(nullptr, exe_path, static_cast<DWORD>(std::size(exe_path)));
            const std::filesystem::path capture_path = std::filesystem::path(exe_path).parent_path()
                / L"captures"
                / (L"capture_" + std::to_wstring(instance.process_id) + L"_" + std::to_wstring(tag) + L".ppm");
            std::cout << "{\"ok\":true,\"process_id\":" << instance.process_id
                      << ",\"tag\":" << tag
                      << ",\"display_frame_id\":" << frame_id
                      << ",\"scene_time_us\":" << scene_time_us
                      << ",\"path\":" << json_string(capture_path.wstring()) << "}\n";
            return 0;
        }
        Sleep(15);
    }
    return fail("capture_timeout", "Timed out waiting for frame capture completion");
}

int step_instance_frames(const WindowInstance& instance, std::int32_t count) {
    RuntimeStateView state_view;
    const char* error_code = nullptr;
    const char* error_message = nullptr;
    if (!state_view.open(instance.process_id, error_code, error_message)) return fail(error_code, error_message);
    if (!PostMessageW(instance.window, frame_warp_lab::control_message, static_cast<WPARAM>(frame_warp_lab::ControlCommand::step_frames), static_cast<LPARAM>(count))) {
        return fail("control_failed", "Could not send step request");
    }
    auto* state = state_view.get();
    Sleep(30);
    for (int attempt = 0; attempt < 600; ++attempt) {
        const std::int32_t remaining = std::atomic_ref<std::int32_t>(state->pending_step_frames).load(std::memory_order_acquire);
        if (remaining == 0) {
            const std::int64_t scene_time_ms = std::atomic_ref<std::int64_t>(state->scene_time_ms).load(std::memory_order_acquire);
            std::cout << "{\"ok\":true,\"process_id\":" << instance.process_id
                      << ",\"stepped_frames\":" << count
                      << ",\"scene_time_ms\":" << scene_time_ms << "}\n";
            return 0;
        }
        Sleep(15);
    }
    return fail("step_timeout", "Timed out waiting for deterministic step completion");
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
    frame_warp_lab::StreamlineBackend streamline;
    streamline.initialize(true, true);

    ComPtr<IDXGIFactory4> factory;
    CreateDXGIFactory1(IID_PPV_ARGS(&factory));
    ComPtr<ID3D12Device> device;
    HRESULT result = D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device));
    if (FAILED(result)) {
        streamline.shutdown();
        return fail("device_unavailable", "Could not create the default D3D12 device");
    }
    streamline.attach_device(device.Get());
    const auto sl_caps = streamline.capabilities();

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

    const bool nvapi_arm64_present = file_exists(L"C:\\Windows\\System32\\nvapia64.dll");
    const bool frameview_sdk_arm64_present = file_exists(L"C:\\Program Files\\NVIDIA Corporation\\FrameViewSDK\\bin\\FvSDK_arm64.dll")
        || file_exists(L"C:\\Program Files\\NVIDIA Corporation\\FrameViewSDK\\SDK\\Public_Release\\ARM64\\FvSDK_ARM64.dll");
    const bool presentmon_arm64_present = file_exists(L"C:\\Program Files\\NVIDIA Corporation\\FrameViewSDK\\bin\\PresentMon_arm64.exe");
    const bool nvrla_present = file_exists(L"C:\\Program Files\\NVIDIA Corporation\\FrameViewSDK\\bin\\nvrla.exe");

    std::cout << "{\"ok\":true,\"schema_version\":7,\"scope\":\"new_default_d3d12_device_not_runtime_instance\",\"simd_backend\":\""
              << frame_warp_lab::simd::backend_name() << "\",\"adapter\":";
    print_adapter(device.Get(), factory.Get());
    std::cout << ",\"presentation\":{\"allow_tearing_query_available\":" << (tearing_query_available ? "true" : "false")
              << ",\"allow_tearing_supported\":" << (allow_tearing ? "true" : "false") << "},\"vrs\":";
    if (FAILED(result)) std::cout << "{\"query_available\":false}";
    else std::cout << "{\"query_available\":true,\"tier\":" << static_cast<int>(options.VariableShadingRateTier)
                   << ",\"additional_shading_rates_supported\":" << (options.AdditionalShadingRatesSupported ? "true" : "false")
                   << ",\"tile_size_px\":" << options.ShadingRateImageTileSize << "}";
    std::cout << ",\"nvidia_latency_tools\":{\"pcl_etw_provider_integrated\":true,\"nvapi_arm64_dll_present\":"
              << (nvapi_arm64_present ? "true" : "false")
              << ",\"frameview_sdk_arm64_present\":" << (frameview_sdk_arm64_present ? "true" : "false")
              << ",\"presentmon_arm64_present\":" << (presentmon_arm64_present ? "true" : "false")
              << ",\"reflex_latency_analyzer_cli_present\":" << (nvrla_present ? "true" : "false")
              << "},\"dlss_frame_generation\":{\"compiled_with_sdk\":" << (sl_caps.compiled_with_sdk ? "true" : "false")
              << ",\"streamline_initialized\":" << (sl_caps.initialized ? "true" : "false")
              << ",\"sl_version\":\"" << sl_caps.sl_version_major << "." << sl_caps.sl_version_minor << "." << sl_caps.sl_version_build << "\""
              << ",\"ngx_version\":\"" << sl_caps.ngx_version_major << "." << sl_caps.ngx_version_minor << "." << sl_caps.ngx_version_build << "\""
              << ",\"supported\":" << (sl_caps.dlssg_supported ? "true" : "false")
              << ",\"support_result_code\":" << sl_caps.dlssg_support_result
              << ",\"dlss_sr_supported\":" << (sl_caps.dlss_sr_supported ? "true" : "false")
              << ",\"dlss_sr_result_code\":" << sl_caps.dlss_sr_support_result
              << ",\"reflex_supported\":" << (sl_caps.reflex_supported ? "true" : "false")
              << ",\"max_frames_to_generate\":" << sl_caps.max_frames_to_generate
              << ",\"max_multiplier\":" << (sl_caps.max_frames_to_generate > 0U ? sl_caps.max_frames_to_generate + 1U : 0U)
              << ",\"dynamic_mfg_supported\":" << (sl_caps.dynamic_mfg_supported ? "true" : "false")
              << ",\"vsync_supported\":" << (sl_caps.vsync_supported ? "true" : "false")
              << ",\"min_width_or_height\":" << sl_caps.min_width_or_height
              << ",\"estimated_vram_mb\":" << (sl_caps.estimated_vram_bytes / (1024ULL * 1024ULL))
              << ",\"last_error\":" << json_escape_utf8(sl_caps.last_error) << "}}\n";
    streamline.shutdown();
    return 0;
}

int resize_instance_window(const WindowInstance& instance, std::uint32_t width, std::uint32_t height) {
    RuntimeStateView state_view;
    const char* error_code = nullptr;
    const char* error_message = nullptr;
    if (!state_view.open(instance.process_id, error_code, error_message)) return fail(error_code, error_message);
    const std::int32_t initial_resizes = std::atomic_ref<std::int32_t>(state_view.get()->resize_count).load(std::memory_order_acquire);
    const auto packed = static_cast<LPARAM>((width & 0xFFFFU) | ((height & 0xFFFFU) << 16U));
    if (!PostMessageW(instance.window, frame_warp_lab::control_message, static_cast<WPARAM>(frame_warp_lab::ControlCommand::resize_window), packed)) {
        return fail("control_failed", "Could not send resize request");
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (std::chrono::steady_clock::now() < deadline) {
        const std::int32_t current_resizes = std::atomic_ref<std::int32_t>(state_view.get()->resize_count).load(std::memory_order_acquire);
        RECT rect{};
        if (GetClientRect(instance.window, &rect) != 0) {
            const std::uint32_t actual_w = static_cast<std::uint32_t>(std::max<LONG>(0, rect.right - rect.left));
            const std::uint32_t actual_h = static_cast<std::uint32_t>(std::max<LONG>(0, rect.bottom - rect.top));
            if (current_resizes > initial_resizes || (actual_w == width && actual_h == height)) {
                std::cout << "{\"ok\":true,\"process_id\":" << instance.process_id
                          << ",\"requested\":{\"width\":" << width << ",\"height\":" << height << "}"
                          << ",\"client_size_px\":{\"width\":" << actual_w << ",\"height\":" << actual_h << "}"
                          << ",\"resize_count\":" << current_resizes << "}\n";
                return 0;
            }
        }
        Sleep(20);
    }
    RECT rect{};
    GetClientRect(instance.window, &rect);
    std::cout << "{\"ok\":true,\"process_id\":" << instance.process_id
              << ",\"requested\":{\"width\":" << width << ",\"height\":" << height << "}"
              << ",\"client_size_px\":{\"width\":" << std::max<LONG>(0, rect.right - rect.left)
              << ",\"height\":" << std::max<LONG>(0, rect.bottom - rect.top) << "}"
              << ",\"resize_count\":" << std::atomic_ref<std::int32_t>(state_view.get()->resize_count).load(std::memory_order_acquire) << "}\n";
    return 0;
}

int set_instance_render_size(const WindowInstance& instance, std::uint32_t width, std::uint32_t height) {
    RuntimeStateView state_view;
    const char* error_code = nullptr;
    const char* error_message = nullptr;
    if (!state_view.open(instance.process_id, error_code, error_message)) return fail(error_code, error_message);
    const auto packed = static_cast<LPARAM>((width & 0xFFFFU) | ((height & 0xFFFFU) << 16U));
    if (!PostMessageW(instance.window, frame_warp_lab::control_message, static_cast<WPARAM>(frame_warp_lab::ControlCommand::set_render_size), packed)) {
        return fail("control_failed", "Could not send render_size request");
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (std::chrono::steady_clock::now() < deadline) {
        const std::int32_t cur_w = std::atomic_ref<std::int32_t>(state_view.get()->render_width).load(std::memory_order_acquire);
        const std::int32_t cur_h = std::atomic_ref<std::int32_t>(state_view.get()->render_height).load(std::memory_order_acquire);
        if ((width > 0U && cur_w == static_cast<std::int32_t>(width) && cur_h == static_cast<std::int32_t>(height))
            || (width == 0U && cur_w > 0 && cur_h > 0)) {
            std::cout << "{\"ok\":true,\"process_id\":" << instance.process_id
                      << ",\"render_size_px\":{\"width\":" << cur_w << ",\"height\":" << cur_h << "}}\n";
            return 0;
        }
        Sleep(15);
    }
    const std::int32_t cur_w = std::atomic_ref<std::int32_t>(state_view.get()->render_width).load(std::memory_order_acquire);
    const std::int32_t cur_h = std::atomic_ref<std::int32_t>(state_view.get()->render_height).load(std::memory_order_acquire);
    std::cout << "{\"ok\":true,\"process_id\":" << instance.process_id
              << ",\"render_size_px\":{\"width\":" << cur_w << ",\"height\":" << cur_h << "}}\n";
    return 0;
}

}

int wmain(int argument_count, wchar_t** arguments) {
    if (argument_count == 2 && std::wstring(arguments[1]) == L"help") {
        std::cout << "{\"ok\":true,\"schema_version\":20,\"commands\":[\"help\",\"start\",\"instances\",\"status <pid>\",\"events <pid>\",\"trace <pid> [limit] [summary]\",\"set <pid> <key> <value>\",\"resize <pid> <width> <height>\",\"render_size <pid> <width> <height>\",\"step <pid> <frames>\",\"capture <pid> <tag> [step_frames]\",\"pause <pid>\",\"resume <pid>\",\"reset <pid>\",\"stop <pid>\",\"capabilities\"],\"settings\":{\"mode\":[\"full_render\",\"repeat\",\"reproject\",\"frame_gen\",\"combined\",\"dlss_g\",\"dlss_g_warp\"],\"dlssg_mult\":[\"2\",\"3\",\"4\",\"5\",\"6\",\"dynamic\"],\"pacer\":[\"deadline\",\"vsync_only\",\"unlocked\"],\"target_fps\":\"0 or 30..500\",\"async_gpu\":[\"on\",\"off\"],\"vrs\":[\"off\",\"uniform_2x2\",\"foveated\"],\"selective_tiles\":[\"on\",\"off\"],\"tile_budget\":\"5..100\",\"tile_policy\":[\"fifo\",\"priority_stbn\"],\"auto_tile_budget\":[\"on\",\"off\"],\"cache_sharpen\":[\"on\",\"off\"],\"coarse_fallback\":[\"on\",\"off\"],\"subtile_interleave\":[\"on\",\"off\"],\"shader_feature\":[\"none\",\"hf_checker\",\"anim_uv\",\"specular\",\"moving_light\",\"emissive\",\"hard_shadows\",\"normal_map\",\"transparent\",\"all\",\"0..255\"],\"render_scale\":\"25..200\",\"upscale_filter\":[\"bilinear\",\"catmull_rom\",\"cas\",\"nearest\"],\"drs\":[\"off\",\"auto\",\"sweep\"],\"drs_min\":\"25..200\",\"drs_max\":\"25..200\",\"fixed_dt_us\":\"0..100000\",\"adaptive\":[\"on\",\"off\"],\"adaptive_dlssg\":[\"on\",\"off\"],\"instrumentation\":[\"minimal\",\"standard\",\"diagnostic\"],\"divider\":\"1..8\",\"vsync\":[\"on\",\"off\"],\"view\":[\"final\",\"depth\",\"motion\",\"holes\",\"vrs\",\"tiles\",\"priority\"],\"path\":[\"corridor_sway\",\"fast_pan\",\"occlusion_sweep\",\"camera_cut\",\"manual\",\"pure_rotation\"],\"stress\":\"0..64\",\"overlay\":[\"on\",\"off\"],\"flash\":[\"on\",\"off\"]}}\n";
        return 0;
    }
    if (argument_count == 2 && std::wstring(arguments[1]) == L"capabilities") return print_capabilities();
    if (argument_count == 2 && std::wstring(arguments[1]) == L"start") return start_instance();
    const auto instances = find_instances();
    if (argument_count == 2 && std::wstring(arguments[1]) == L"instances") {
        std::cout << "{\"ok\":true,\"schema_version\":20,\"instances\":[";
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
    if (command == L"resize" && argument_count == 5) {
        unsigned long parsed_w = 0;
        unsigned long parsed_h = 0;
        if (!parse_unsigned_long(arguments[3], parsed_w) || !parse_unsigned_long(arguments[4], parsed_h)
            || parsed_w < 320UL || parsed_w > 7680UL || parsed_h < 240UL || parsed_h > 4320UL) {
            return fail("invalid_size", "Expected width in [320, 7680] and height in [240, 4320]");
        }
        return resize_instance_window(*matched_instance, static_cast<std::uint32_t>(parsed_w), static_cast<std::uint32_t>(parsed_h));
    }
    if (command == L"render_size" && argument_count == 5) {
        unsigned long parsed_w = 0;
        unsigned long parsed_h = 0;
        if (!parse_unsigned_long(arguments[3], parsed_w) || !parse_unsigned_long(arguments[4], parsed_h)) {
            return fail("invalid_size", "Expected integers: 0 0 (reset) or width in [160, 7680] and height in [120, 4320]");
        }
        const bool is_reset = (parsed_w == 0UL && parsed_h == 0UL);
        const bool is_valid = (parsed_w >= 160UL && parsed_w <= 7680UL && parsed_h >= 120UL && parsed_h <= 4320UL);
        if (!is_reset && !is_valid) {
            return fail("invalid_size", "Expected 0 0 (reset) or width in [160, 7680] and height in [120, 4320]");
        }
        return set_instance_render_size(*matched_instance, static_cast<std::uint32_t>(parsed_w), static_cast<std::uint32_t>(parsed_h));
    }
    if (command == L"capture" && (argument_count == 4 || argument_count == 5)) {
        unsigned long parsed_tag = 0;
        if (!parse_unsigned_long(arguments[3], parsed_tag) || parsed_tag == 0UL) {
            return fail("invalid_tag", "Expected positive capture tag integer");
        }
        unsigned long parsed_steps = 0;
        if (argument_count == 5) {
            if (!parse_unsigned_long(arguments[4], parsed_steps) || parsed_steps == 0UL || parsed_steps > 2000UL) {
                return fail("invalid_count", "Expected step frame count in [1, 2000]");
            }
        }
        return capture_instance_frame(*matched_instance, static_cast<std::uint32_t>(parsed_tag), static_cast<std::int32_t>(parsed_steps));
    }
    if (command == L"step" && argument_count == 4) {
        unsigned long parsed_count = 0;
        if (!parse_unsigned_long(arguments[3], parsed_count) || parsed_count == 0UL || parsed_count > 2000UL) {
            return fail("invalid_count", "Expected step frame count in [1, 2000]");
        }
        return step_instance_frames(*matched_instance, static_cast<std::int32_t>(parsed_count));
    }
    if (command == L"emit" && argument_count == 4) {
        const std::wstring ev_name = arguments[3];
        LPARAM code = 0;
        if (ev_name == L"fire" || ev_name == L"weapon.fire_requested") code = 1;
        else if (ev_name == L"reload" || ev_name == L"weapon.reload_requested") code = 2;
        else if (ev_name == L"reset_targets" || ev_name == L"target.force_reset") code = 3;
        else if (ev_name == L"reset_range" || ev_name == L"range.reset") code = 4;
        else return fail("invalid_event", "Expected event in {fire, reload, reset_targets, reset_range}");
        if (!PostMessageW(matched_instance->window, frame_warp_lab::control_message, static_cast<WPARAM>(frame_warp_lab::ControlCommand::emit_range_event), code)) {
            return fail("control_failed", "Could not dispatch range event");
        }
        std::cout << "{\"ok\":true,\"process_id\":" << matched_instance->process_id << ",\"event_dispatched\":" << json_string(ev_name) << "}\n";
        return 0;
    }
    if (argument_count == 3 && (command == L"pause" || command == L"resume" || command == L"reset" || command == L"fire" || command == L"reload" || command == L"reload_scripts")) {
        const frame_warp_lab::ControlCommand operation = command == L"pause"
            ? frame_warp_lab::ControlCommand::scene_pause
            : command == L"resume" ? frame_warp_lab::ControlCommand::scene_resume
            : command == L"reset" ? frame_warp_lab::ControlCommand::scene_reset
            : command == L"fire" ? frame_warp_lab::ControlCommand::range_fire
            : command == L"reload" ? frame_warp_lab::ControlCommand::range_reload
            : frame_warp_lab::ControlCommand::reload_lua_scripts;
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
