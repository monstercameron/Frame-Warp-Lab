#include "runtime_state.h"

#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <cstdlib>
#include <cstdio>
#include <iostream>
#include <limits>
#include <string>
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

std::string json_string(const std::wstring& text) {
    if (text.empty()) return "\"\"";
    const int byte_count = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    if (byte_count <= 0) return "\"\"";
    std::string utf8(static_cast<std::size_t>(byte_count), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), utf8.data(), byte_count, nullptr, nullptr);
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

int fail(const char* code, const char* message) {
    std::cerr << "{\"ok\":false,\"error\":{\"code\":\"" << code << "\",\"message\":\"" << message << "\"}}\n";
    return 1;
}

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

int print_status(const WindowInstance& instance) {
    HANDLE mapping = OpenFileMappingW(FILE_MAP_READ, FALSE, frame_warp_lab::runtime_mapping_name(instance.process_id).c_str());
    if (!mapping) return fail("state_unavailable", "Runtime state is unavailable for this instance");
    const auto* state = static_cast<const frame_warp_lab::RuntimeState*>(MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, sizeof(frame_warp_lab::RuntimeState)));
    if (!state) { CloseHandle(mapping); return fail("state_unavailable", "Could not map runtime state"); }
    if (state->version != frame_warp_lab::runtime_state_version || state->process_id != instance.process_id) {
        UnmapViewOfFile(state); CloseHandle(mapping);
        return fail("state_version_mismatch", "Runtime state version or process identity does not match");
    }
    RECT client_rectangle{};
    const bool has_client_size = GetClientRect(instance.window, &client_rectangle) != 0;
    std::cout << "{\"ok\":true,\"schema_version\":1,\"process_id\":" << instance.process_id
              << ",\"running\":" << (state->running ? "true" : "false")
              << ",\"present_calls_completed\":" << state->present_count
              << ",\"resize_count\":" << state->resize_count << ",\"client_size_px\":";
    if (has_client_size) std::cout << "{\"width\":" << client_rectangle.right - client_rectangle.left << ",\"height\":" << client_rectangle.bottom - client_rectangle.top << "}";
    else std::cout << "null";
    std::cout << ",\"display\":";
    print_display(instance.window);
    std::cout << ",\"presentation\":{\"requested_sync_interval\":1,\"displayed_frame_timing\":null},"
              << "\"scene\":{\"name\":\"motion_lab\",\"time_ms\":" << state->scene_time_ms
              << ",\"paused\":" << (state->scene_paused ? "true" : "false") << "},"
                 "\"features\":{\"scene\":\"procedural_initial\",\"reprojection\":\"not_implemented\",\"frame_generation\":\"not_implemented\",\"vrs\":\"not_implemented\"}}\n";
    UnmapViewOfFile(state);
    CloseHandle(mapping);
    return 0;
}

int print_events(const WindowInstance& instance) {
    HANDLE mapping = OpenFileMappingW(FILE_MAP_READ, FALSE, frame_warp_lab::runtime_mapping_name(instance.process_id).c_str());
    if (!mapping) return fail("state_unavailable", "Runtime state is unavailable for this instance");
    const auto* state = static_cast<const frame_warp_lab::RuntimeState*>(MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, sizeof(frame_warp_lab::RuntimeState)));
    if (!state) { CloseHandle(mapping); return fail("state_unavailable", "Could not map runtime state"); }
    if (state->version != frame_warp_lab::runtime_state_version || state->process_id != instance.process_id) {
        UnmapViewOfFile(state); CloseHandle(mapping);
        return fail("state_version_mismatch", "Runtime state version or process identity does not match");
    }
    const LONG event_count = state->event_count;
    const LONG first_sequence = event_count > static_cast<LONG>(frame_warp_lab::runtime_event_capacity)
        ? event_count - static_cast<LONG>(frame_warp_lab::runtime_event_capacity) + 1 : 1;
    std::cout << "{\"ok\":true,\"schema_version\":1,\"process_id\":" << instance.process_id
              << ",\"capacity\":" << frame_warp_lab::runtime_event_capacity << ",\"total_events\":" << event_count << ",\"events\":[";
    bool first_event = true;
    for (LONG sequence = first_sequence; sequence <= event_count; ++sequence) {
        const auto& event = state->events[(static_cast<std::size_t>(sequence) - 1) % frame_warp_lab::runtime_event_capacity];
        if (event.sequence != sequence) continue;
        const DWORD timestamp_low = event.timestamp_low;
        const DWORD timestamp_high = event.timestamp_high;
        const LONG event_type = event.event_type;
        const LONG value = event.value;
        if (event.sequence != sequence) continue;
        FILETIME file_time{timestamp_low, timestamp_high};
        SYSTEMTIME utc{};
        if (!FileTimeToSystemTime(&file_time, &utc)) continue;
        char timestamp[32]{};
        std::snprintf(timestamp, sizeof(timestamp), "%04u-%02u-%02uT%02u:%02u:%02u.%03uZ", utc.wYear, utc.wMonth, utc.wDay, utc.wHour, utc.wMinute, utc.wSecond, utc.wMilliseconds);
        const char* event_name = event_type == 1 ? "runtime_started" : event_type == 2 ? "swap_chain_resized" : "runtime_stopping";
        if (!first_event) std::cout << ',';
        first_event = false;
        std::cout << "{\"sequence\":" << sequence << ",\"timestamp_utc\":\"" << timestamp
                  << "\",\"severity\":\"info\",\"subsystem\":\"runtime\",\"event\":\"" << event_name << "\",\"value\":" << value << "}";
    }
    std::cout << "]}\n";
    UnmapViewOfFile(state);
    CloseHandle(mapping);
    return 0;
}

int start_instance() {
    wchar_t executable_path[MAX_PATH]{};
    const DWORD path_length = GetModuleFileNameW(nullptr, executable_path, MAX_PATH);
    if (path_length == 0 || path_length == MAX_PATH) return fail("start_failed", "Could not locate the CLI executable");
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

int print_capabilities() {
    ComPtr<ID3D12Device> device;
    HRESULT result = D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device));
    if (FAILED(result)) return fail("device_unavailable", "Could not create the default D3D12 device");
    D3D12_FEATURE_DATA_D3D12_OPTIONS6 options{};
    result = device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS6, &options, sizeof(options));
    std::cout << "{\"ok\":true,\"schema_version\":1,\"scope\":\"new_default_d3d12_device_not_runtime_instance\",\"vrs\":";
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
        std::cout << "{\"ok\":true,\"schema_version\":1,\"commands\":[\"help\",\"start\",\"instances\",\"status <pid>\",\"events <pid>\",\"pause <pid>\",\"resume <pid>\",\"reset <pid>\",\"stop <pid>\",\"capabilities\"]}\n";
        return 0;
    }
    if (argument_count == 2 && std::wstring(arguments[1]) == L"capabilities") return print_capabilities();
    if (argument_count == 2 && std::wstring(arguments[1]) == L"start") return start_instance();
    const auto instances = find_instances();
    if (argument_count == 2 && std::wstring(arguments[1]) == L"instances") {
        std::cout << "{\"ok\":true,\"schema_version\":1,\"instances\":[";
        for (std::size_t index = 0; index < instances.size(); ++index) {
            if (index) std::cout << ',';
            std::cout << "{\"process_id\":" << instances[index].process_id << "}";
        }
        std::cout << "]}\n";
        return 0;
    }
    if (argument_count != 3 || (std::wstring(arguments[1]) != L"status" && std::wstring(arguments[1]) != L"events" && std::wstring(arguments[1]) != L"stop" && std::wstring(arguments[1]) != L"pause" && std::wstring(arguments[1]) != L"resume" && std::wstring(arguments[1]) != L"reset"))
        return fail("invalid_arguments", "Use help for the command list");
    wchar_t* end = nullptr;
    const unsigned long requested_process = std::wcstoul(arguments[2], &end, 10);
    if (arguments[2][0] == L'\0' || *end != L'\0' || requested_process == 0 || requested_process == ULONG_MAX)
        return fail("invalid_pid", "Expected a positive process ID");
    for (const auto& instance : instances) {
        if (instance.process_id != requested_process) continue;
        if (std::wstring(arguments[1]) == L"status") return print_status(instance);
        if (std::wstring(arguments[1]) == L"events") return print_events(instance);
        if (std::wstring(arguments[1]) != L"stop") {
            const std::wstring action = arguments[1];
            const WPARAM operation = action == L"pause" ? frame_warp_lab::scene_pause : action == L"resume" ? frame_warp_lab::scene_resume : frame_warp_lab::scene_reset;
            if (!PostMessageW(instance.window, frame_warp_lab::scene_control_message, operation, 0)) return fail("control_failed", "Could not send scene control request");
            std::cout << "{\"ok\":true,\"process_id\":" << instance.process_id << ",\"scene_control_requested\":" << json_string(action) << "}\n";
            return 0;
        }
        if (!PostMessageW(instance.window, WM_CLOSE, 0, 0)) return fail("stop_failed", "Could not send the close request");
        std::cout << "{\"ok\":true,\"process_id\":" << instance.process_id << ",\"stop_requested\":true}\n";
        return 0;
    }
    return fail("instance_not_found", "No matching Frame Warp Lab window was found");
}
