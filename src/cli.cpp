#include "runtime_state.h"

#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <cstdlib>
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
                 "\"features\":{\"scene\":\"not_implemented\",\"reprojection\":\"not_implemented\",\"frame_generation\":\"not_implemented\",\"vrs\":\"not_implemented\"}}\n";
    UnmapViewOfFile(state);
    CloseHandle(mapping);
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
        std::cout << "{\"ok\":true,\"schema_version\":1,\"commands\":[\"help\",\"instances\",\"status <pid>\",\"stop <pid>\",\"capabilities\"]}\n";
        return 0;
    }
    if (argument_count == 2 && std::wstring(arguments[1]) == L"capabilities") return print_capabilities();
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
    if (argument_count != 3 || (std::wstring(arguments[1]) != L"status" && std::wstring(arguments[1]) != L"stop"))
        return fail("invalid_arguments", "Use help for the command list");
    wchar_t* end = nullptr;
    const unsigned long requested_process = std::wcstoul(arguments[2], &end, 10);
    if (arguments[2][0] == L'\0' || *end != L'\0' || requested_process == 0 || requested_process == ULONG_MAX)
        return fail("invalid_pid", "Expected a positive process ID");
    for (const auto& instance : instances) {
        if (instance.process_id != requested_process) continue;
        if (std::wstring(arguments[1]) == L"status") return print_status(instance);
        if (!PostMessageW(instance.window, WM_CLOSE, 0, 0)) return fail("stop_failed", "Could not send the close request");
        std::cout << "{\"ok\":true,\"process_id\":" << instance.process_id << ",\"stop_requested\":true}\n";
        return 0;
    }
    return fail("instance_not_found", "No matching Frame Warp Lab window was found");
}
