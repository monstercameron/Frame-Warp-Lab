#include "runtime_state.h"

#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

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
    RECT client_rectangle{};
    const bool has_client_size = GetClientRect(instance.window, &client_rectangle) != 0;
    std::cout << "{\"ok\":true,\"schema_version\":1,\"process_id\":" << instance.process_id
              << ",\"running\":" << (running ? "true" : "false")
              << ",\"present_calls_completed\":" << present_count
              << ",\"resize_count\":" << resize_count << ",\"client_size_px\":";
    if (has_client_size) std::cout << "{\"width\":" << client_rectangle.right - client_rectangle.left << ",\"height\":" << client_rectangle.bottom - client_rectangle.top << "}";
    else std::cout << "null";
    std::cout << ",\"display\":";
    print_display(instance.window);
    std::cout << ",\"presentation\":{\"requested_sync_interval\":1,\"displayed_frame_timing\":null},"
              << "\"scene\":{\"name\":\"motion_lab\",\"time_ms\":" << scene_time_ms
              << ",\"paused\":" << (scene_paused ? "true" : "false") << "},"
              << "\"features\":{\"scene\":\"procedural_initial\",\"reprojection\":\"not_implemented\",\"frame_generation\":\"not_implemented\",\"vrs\":\"not_implemented\"}}\n";
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
    std::cout << "{\"ok\":true,\"schema_version\":1,\"process_id\":" << instance.process_id
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
    std::cout << "{\"ok\":true,\"schema_version\":1,\"scope\":\"new_default_d3d12_device_not_runtime_instance\",\"adapter\":";
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
