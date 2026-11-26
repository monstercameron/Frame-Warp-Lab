#pragma once

#include <windows.h>

#include <atomic>
#include <cstdint>

namespace frame_warp_lab {

enum class LatencyMarker : std::uint32_t {
    simulation_start = 0,
    simulation_end = 1,
    render_submit_start = 2,
    render_submit_end = 3,
    present_start = 4,
    present_end = 5,
    trigger_flash = 7,
    pc_latency_ping = 8,
    late_warp_present_start = 15,
    late_warp_present_end = 16,
    camera_constructed = 17,
    late_warp_submit_start = 18,
    late_warp_submit_end = 19,
};

class LatencyMonitor {
public:
    LatencyMonitor();
    ~LatencyMonitor();

    LatencyMonitor(const LatencyMonitor&) = delete;
    LatencyMonitor& operator=(const LatencyMonitor&) = delete;

    void start_ping_loop(HWND target_window);
    void stop();
    bool is_ping_message(UINT message) const;
    void record_ping_received(std::uint64_t frame_id);
    void emit_marker(LatencyMarker marker, std::uint64_t frame_id);

    bool etw_listener_active() const;
    std::int64_t markers_emitted() const;
    std::int64_t pings_received() const;

private:
    static DWORD WINAPI ping_thread_entry(LPVOID context);
    void run_ping_loop();

    UINT ping_window_message_ = 0;
    HWND target_window_ = nullptr;
    HANDLE quit_event_ = nullptr;
    HANDLE ping_thread_ = nullptr;
    std::atomic<std::int64_t> markers_emitted_{0};
    std::atomic<std::int64_t> pings_received_{0};
    bool provider_registered_ = false;
};

}
