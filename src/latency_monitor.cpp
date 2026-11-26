#include "latency_monitor.h"

#include <TraceLoggingProvider.h>
#include <evntrace.h>

#include <random>

namespace frame_warp_lab {

namespace {

TRACELOGGING_DEFINE_PROVIDER(
    pcl_stats_provider,
    "PCLStatsTraceLoggingProvider",
    (0x0d216f06, 0x82a6, 0x4d49, 0xbc, 0x4f, 0x8f, 0x38, 0xae, 0x56, 0xef, 0xab));

std::atomic<bool> pcl_provider_enabled{false};

void WINAPI pcl_provider_callback(
    LPCGUID,
    ULONG control_code,
    UCHAR,
    ULONGLONG,
    ULONGLONG,
    PEVENT_FILTER_DESCRIPTOR,
    PVOID) {
    if (control_code == EVENT_CONTROL_CODE_ENABLE_PROVIDER) {
        pcl_provider_enabled.store(true, std::memory_order_release);
    } else if (control_code == EVENT_CONTROL_CODE_DISABLE_PROVIDER) {
        pcl_provider_enabled.store(false, std::memory_order_release);
    } else if (control_code == EVENT_CONTROL_CODE_CAPTURE_STATE) {
        constexpr std::uint32_t flags = 0U;
        TraceLoggingWrite(pcl_stats_provider, "PCLStatsFlags", TraceLoggingUInt32(flags, "Flags"));
    }
}

}

LatencyMonitor::LatencyMonitor() {
    ping_window_message_ = RegisterWindowMessageW(L"PC_Latency_Stats_Ping");
    if (SUCCEEDED(TraceLoggingRegisterEx(pcl_stats_provider, pcl_provider_callback, nullptr))) {
        provider_registered_ = true;
        TraceLoggingWrite(pcl_stats_provider, "PCLStatsInit");
    }
}

LatencyMonitor::~LatencyMonitor() {
    stop();
    if (provider_registered_) {
        TraceLoggingWrite(pcl_stats_provider, "PCLStatsShutdown");
        TraceLoggingUnregister(pcl_stats_provider);
        provider_registered_ = false;
    }
}

void LatencyMonitor::start_ping_loop(HWND target_window) {
    target_window_ = target_window;
    if (!quit_event_) {
        quit_event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    }
    if (quit_event_ && !ping_thread_) {
        ping_thread_ = CreateThread(nullptr, 0, ping_thread_entry, this, 0, nullptr);
    }
}

void LatencyMonitor::stop() {
    if (ping_thread_) {
        if (quit_event_) SetEvent(quit_event_);
        WaitForSingleObject(ping_thread_, 1000);
        CloseHandle(ping_thread_);
        ping_thread_ = nullptr;
    }
    if (quit_event_) {
        CloseHandle(quit_event_);
        quit_event_ = nullptr;
    }
}

void LatencyMonitor::set_enabled(bool enabled) {
    enabled_.store(enabled, std::memory_order_release);
}

bool LatencyMonitor::is_ping_message(UINT message) const {
    return ping_window_message_ != 0 && message == ping_window_message_;
}

void LatencyMonitor::record_ping_received(std::uint64_t frame_id) {
    if (!enabled_.load(std::memory_order_relaxed)) return;
    pings_received_.fetch_add(1, std::memory_order_relaxed);
    emit_marker(LatencyMarker::pc_latency_ping, frame_id);
}

void LatencyMonitor::emit_marker(LatencyMarker marker, std::uint64_t frame_id) {
    if (!enabled_.load(std::memory_order_relaxed)) return;
    markers_emitted_.fetch_add(1, std::memory_order_relaxed);
    if (!provider_registered_) return;
    const std::uint32_t marker_value = static_cast<std::uint32_t>(marker);
    constexpr std::uint32_t flags = 0U;
    TraceLoggingWrite(
        pcl_stats_provider,
        "PCLStatsEventV2",
        TraceLoggingUInt32(marker_value, "Marker"),
        TraceLoggingUInt64(frame_id, "FrameID"),
        TraceLoggingUInt32(flags, "Flags"));
}

bool LatencyMonitor::etw_listener_active() const {
    return pcl_provider_enabled.load(std::memory_order_acquire);
}

std::int64_t LatencyMonitor::markers_emitted() const {
    return markers_emitted_.load(std::memory_order_relaxed);
}

std::int64_t LatencyMonitor::pings_received() const {
    return pings_received_.load(std::memory_order_relaxed);
}

DWORD WINAPI LatencyMonitor::ping_thread_entry(LPVOID context) {
    static_cast<LatencyMonitor*>(context)->run_ping_loop();
    return 0;
}

void LatencyMonitor::run_ping_loop() {
    std::mt19937 generator(static_cast<std::uint32_t>(GetTickCount64()));
    std::uniform_int_distribution<DWORD> interval_ms(100, 300);
    while (WaitForSingleObject(quit_event_, interval_ms(generator)) == WAIT_TIMEOUT) {
        if (!pcl_provider_enabled.load(std::memory_order_acquire) || !ping_window_message_) {
            continue;
        }
        HWND foreground = GetForegroundWindow();
        if (!foreground) continue;
        DWORD foreground_pid = 0;
        GetWindowThreadProcessId(foreground, &foreground_pid);
        if (foreground_pid != GetCurrentProcessId()) continue;
        TraceLoggingWrite(pcl_stats_provider, "PCLStatsInput", TraceLoggingUInt32(ping_window_message_, "MsgId"));
        PostMessageW(foreground, ping_window_message_, 0, 0);
    }
}

}
