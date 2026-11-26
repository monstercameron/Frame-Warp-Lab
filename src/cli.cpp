#include "runtime_state.h"

#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
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
    const std::int64_t present_interval_us = std::atomic_ref<std::int64_t>(state->last_present_interval_us).load(std::memory_order_acquire);
    const std::int64_t source_age_us = std::atomic_ref<std::int64_t>(state->last_source_age_us).load(std::memory_order_acquire);
    const std::int64_t scene_gpu_us = std::atomic_ref<std::int64_t>(state->last_scene_gpu_us).load(std::memory_order_acquire);
    const std::int64_t present_gpu_us = std::atomic_ref<std::int64_t>(state->last_present_gpu_us).load(std::memory_order_acquire);
    const std::int32_t diagnostic_view = std::atomic_ref<std::int32_t>(state->diagnostic_view).load(std::memory_order_acquire);
    const std::int32_t camera_path = std::atomic_ref<std::int32_t>(state->camera_path).load(std::memory_order_acquire);
    const std::int32_t stress_level = std::atomic_ref<std::int32_t>(state->stress_level).load(std::memory_order_acquire);
    const std::int32_t overlay_enabled = std::atomic_ref<std::int32_t>(state->overlay_enabled).load(std::memory_order_acquire);
    const std::int32_t gpu_timing_available = std::atomic_ref<std::int32_t>(state->gpu_timing_available).load(std::memory_order_acquire);

    RECT client_rectangle{};
    const bool has_client_size = GetClientRect(instance.window, &client_rectangle) != 0;
    std::cout << "{\"ok\":true,\"schema_version\":2,\"process_id\":" << instance.process_id
              << ",\"running\":" << (running ? "true" : "false")
              << ",\"present_calls_completed\":" << present_count
              << ",\"display_frame_id\":" << display_frame_id
              << ",\"source_frame_id\":" << source_frame_id
              << ",\"resize_count\":" << resize_count << ",\"client_size_px\":";
    if (has_client_size) std::cout << "{\"width\":" << client_rectangle.right - client_rectangle.left << ",\"height\":" << client_rectangle.bottom - client_rectangle.top << "}";
    else std::cout << "null";
    std::cout << ",\"display\":";
    print_display(instance.window);
    std::cout << ",\"presentation\":{\"mode\":\"full_render\",\"requested_sync_interval\":1,\"displayed_frame_timing\":null}"
              << ",\"timing\":{\"cpu_present_interval_us\":" << present_interval_us
              << ",\"cpu_source_age_us\":" << source_age_us
              << ",\"gpu_timing_available\":" << (gpu_timing_available ? "true" : "false")
              << ",\"scene_gpu_us\":";
    print_nullable_int64(scene_gpu_us);
    std::cout << ",\"present_gpu_us\":";
    print_nullable_int64(present_gpu_us);
    std::cout << ",\"note\":\"cpu_present_interval_us measures Present return cadence, not physical display scanout\"}"
              << ",\"scene\":{\"name\":\"motion_lab\",\"time_ms\":" << scene_time_ms
              << ",\"paused\":" << (scene_paused ? "true" : "false")
              << ",\"camera_path\":\"" << frame_warp_lab::camera_path_name(camera_path)
              << "\",\"diagnostic_view\":\"" << frame_warp_lab::diagnostic_view_name(diagnostic_view)
              << "\",\"stress_level\":" << stress_level
              << ",\"overlay_enabled\":" << (overlay_enabled ? "true" : "false") << "}"
              << ",\"features\":{\"scene\":\"cpu_camera_gbuffer\",\"reprojection\":\"not_implemented\",\"frame_generation\":\"not_implemented\",\"vrs\":\"not_implemented\"}}\n";
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
    std::cout << "{\"ok\":true,\"schema_version\":2,\"process_id\":" << instance.process_id
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

int print_trace(const WindowInstance& instance, std::size_t max_samples) {
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

    for (std::int64_t sequence = first_sequence; sequence <= total_samples; ++sequence) {
        if (sequence <= 0) continue;
        auto& slot = state->trace[(static_cast<std::size_t>(sequence) - 1) % frame_warp_lab::runtime_frame_trace_capacity];
        if (std::atomic_ref<std::int64_t>(slot.sequence).load(std::memory_order_acquire) != sequence) continue;
        frame_warp_lab::RuntimeFrameSample copy{};
        copy.sequence = sequence;
        copy.display_frame_id = slot.display_frame_id;
        copy.source_frame_id = slot.source_frame_id;
        copy.present_interval_us = slot.present_interval_us;
        copy.source_age_us = slot.source_age_us;
        copy.scene_gpu_us = slot.scene_gpu_us;
        copy.present_gpu_us = slot.present_gpu_us;
        copy.diagnostic_view = slot.diagnostic_view;
        copy.camera_path = slot.camera_path;
        copy.stress_level = slot.stress_level;
        copy.discontinuity = slot.discontinuity;
        std::atomic_thread_fence(std::memory_order_acquire);
        if (std::atomic_ref<std::int64_t>(slot.sequence).load(std::memory_order_acquire) != sequence) continue;
        captured.push_back(copy);
        if (copy.present_interval_us > 0) present_intervals.push_back(copy.present_interval_us);
        if (copy.source_age_us >= 0) source_ages.push_back(copy.source_age_us);
        if (copy.scene_gpu_us >= 0) scene_gpu_times.push_back(copy.scene_gpu_us);
        if (copy.present_gpu_us >= 0) present_gpu_times.push_back(copy.present_gpu_us);
    }

    std::cout << "{\"ok\":true,\"schema_version\":2,\"process_id\":" << instance.process_id
              << ",\"capacity\":" << frame_warp_lab::runtime_frame_trace_capacity
              << ",\"total_frames_recorded\":" << total_samples
              << ",\"sample_count\":" << captured.size()
              << ",\"units\":\"microseconds\""
              << ",\"note\":\"present_interval_us is CPU Present cadence; scene_gpu_us and present_gpu_us are D3D12 GPU timestamp durations from the completed swap-chain slot\""
              << ",\"summary\":{\"cpu_present_interval_us\":";
    print_metric_summary(present_intervals);
    std::cout << ",\"cpu_source_age_us\":";
    print_metric_summary(source_ages);
    std::cout << ",\"scene_gpu_us\":";
    print_metric_summary(scene_gpu_times);
    std::cout << ",\"present_gpu_us\":";
    print_metric_summary(present_gpu_times);
    std::cout << "},\"samples\":[";
    for (std::size_t index = 0; index < captured.size(); ++index) {
        const auto& sample = captured[index];
        if (index) std::cout << ',';
        std::cout << "{\"sequence\":" << sample.sequence
                  << ",\"display_frame_id\":" << sample.display_frame_id
                  << ",\"source_frame_id\":" << sample.source_frame_id
                  << ",\"cpu_present_interval_us\":" << sample.present_interval_us
                  << ",\"cpu_source_age_us\":" << sample.source_age_us
                  << ",\"scene_gpu_us\":";
        print_nullable_int64(sample.scene_gpu_us);
        std::cout << ",\"present_gpu_us\":";
        print_nullable_int64(sample.present_gpu_us);
        std::cout << ",\"diagnostic_view\":\"" << frame_warp_lab::diagnostic_view_name(sample.diagnostic_view)
                  << "\",\"camera_path\":\"" << frame_warp_lab::camera_path_name(sample.camera_path)
                  << "\",\"stress_level\":" << sample.stress_level
                  << ",\"discontinuity\":" << (sample.discontinuity ? "true" : "false") << "}";
    }
    std::cout << "]}\n";
    return 0;
}

int apply_setting(const WindowInstance& instance, const std::wstring& key, const std::wstring& value) {
    frame_warp_lab::ControlCommand command{};
    LPARAM parameter = 0;
    if (key == L"view") {
        command = frame_warp_lab::ControlCommand::set_diagnostic_view;
        if (value == L"final") parameter = static_cast<LPARAM>(frame_warp_lab::DiagnosticView::final_color);
        else if (value == L"depth") parameter = static_cast<LPARAM>(frame_warp_lab::DiagnosticView::depth);
        else if (value == L"motion") parameter = static_cast<LPARAM>(frame_warp_lab::DiagnosticView::motion_vectors);
        else return fail("invalid_value", "Expected view in {final, depth, motion}");
    } else if (key == L"path") {
        command = frame_warp_lab::ControlCommand::set_camera_path;
        if (value == L"corridor_sway") parameter = static_cast<LPARAM>(frame_warp_lab::CameraPath::corridor_sway);
        else if (value == L"fast_pan") parameter = static_cast<LPARAM>(frame_warp_lab::CameraPath::fast_pan);
        else if (value == L"occlusion_sweep") parameter = static_cast<LPARAM>(frame_warp_lab::CameraPath::occlusion_sweep);
        else if (value == L"camera_cut") parameter = static_cast<LPARAM>(frame_warp_lab::CameraPath::camera_cut);
        else return fail("invalid_value", "Expected path in {corridor_sway, fast_pan, occlusion_sweep, camera_cut}");
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
        return fail("unknown_setting", "Supported settings are: view, path, stress, overlay");
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
    std::cout << "{\"ok\":true,\"schema_version\":2,\"scope\":\"new_default_d3d12_device_not_runtime_instance\",\"adapter\":";
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
        std::cout << "{\"ok\":true,\"schema_version\":2,\"commands\":[\"help\",\"start\",\"instances\",\"status <pid>\",\"events <pid>\",\"trace <pid> [limit]\",\"set <pid> <key> <value>\",\"pause <pid>\",\"resume <pid>\",\"reset <pid>\",\"stop <pid>\",\"capabilities\"],\"settings\":{\"view\":[\"final\",\"depth\",\"motion\"],\"path\":[\"corridor_sway\",\"fast_pan\",\"occlusion_sweep\",\"camera_cut\"],\"stress\":\"0..64\",\"overlay\":[\"on\",\"off\"]}}\n";
        return 0;
    }
    if (argument_count == 2 && std::wstring(arguments[1]) == L"capabilities") return print_capabilities();
    if (argument_count == 2 && std::wstring(arguments[1]) == L"start") return start_instance();
    const auto instances = find_instances();
    if (argument_count == 2 && std::wstring(arguments[1]) == L"instances") {
        std::cout << "{\"ok\":true,\"schema_version\":2,\"instances\":[";
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
    if (command == L"trace" && (argument_count == 3 || argument_count == 4)) {
        std::size_t limit = 64;
        if (argument_count == 4) {
            unsigned long parsed_limit = 0;
            if (!parse_unsigned_long(arguments[3], parsed_limit) || parsed_limit == 0 || parsed_limit > frame_warp_lab::runtime_frame_trace_capacity) {
                return fail("invalid_limit", "Expected trace limit in [1, 256]");
            }
            limit = static_cast<std::size_t>(parsed_limit);
        }
        return print_trace(*matched_instance, limit);
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
