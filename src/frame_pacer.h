#pragma once

#include "runtime_state.h"

#include <windows.h>
#include <dwmapi.h>

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace frame_warp_lab {

class FramePacer {
public:
    FramePacer() {
        LARGE_INTEGER frequency{};
        QueryPerformanceFrequency(&frequency);
        qpc_frequency_ = frequency.QuadPart > 0 ? frequency.QuadPart : 10000000LL;
        timer_ = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
        if (!timer_) {
            timer_ = CreateWaitableTimerW(nullptr, TRUE, nullptr);
        }
        period_qpc_ = qpc_frequency_ / 120LL;
    }

    ~FramePacer() {
        if (timer_) {
            CloseHandle(timer_);
            timer_ = nullptr;
        }
    }

    FramePacer(const FramePacer&) = delete;
    FramePacer& operator=(const FramePacer&) = delete;

    void set_mode(PacerMode mode) {
        if (mode_ != mode) {
            mode_ = mode;
            reset();
        }
    }

    void set_target_fps(std::int32_t target_fps, HWND window) {
        target_fps_override_ = target_fps;
        refresh_display_timing(window);
        reset();
    }

    void refresh_display_timing(HWND window) {
        if (target_fps_override_ > 0) {
            period_qpc_ = std::max<std::int64_t>(1, static_cast<std::int64_t>(std::llround(static_cast<double>(qpc_frequency_) / static_cast<double>(target_fps_override_))));
            return;
        }
        DWM_TIMING_INFO timing{};
        timing.cbSize = sizeof(timing);
        if (SUCCEEDED(DwmGetCompositionTimingInfo(nullptr, &timing)) && timing.qpcRefreshPeriod > 0) {
            period_qpc_ = static_cast<std::int64_t>(timing.qpcRefreshPeriod);
            return;
        }
        MONITORINFOEXW monitor{};
        monitor.cbSize = sizeof(monitor);
        if (GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST), &monitor)) {
            DEVMODEW mode{};
            mode.dmSize = sizeof(mode);
            if (EnumDisplaySettingsW(monitor.szDevice, ENUM_CURRENT_SETTINGS, &mode) && mode.dmDisplayFrequency > 1) {
                period_qpc_ = std::max<std::int64_t>(1, static_cast<std::int64_t>(std::llround(static_cast<double>(qpc_frequency_) / static_cast<double>(mode.dmDisplayFrequency))));
                return;
            }
        }
        period_qpc_ = qpc_frequency_ / 120LL;
    }

    void reset() {
        deadline_initialized_ = false;
        next_deadline_qpc_ = 0;
    }

    PacerMode mode() const { return mode_; }
    std::int32_t target_fps_override() const { return target_fps_override_; }

    std::int64_t target_period_us() const {
        return (period_qpc_ * 1000000LL) / qpc_frequency_;
    }

    float target_frame_ms() const {
        return static_cast<float>(static_cast<double>(period_qpc_) * 1000.0 / static_cast<double>(qpc_frequency_));
    }

    template <typename MessagePump>
    void wait_for_input_window(std::int64_t lead_time_us, MessagePump&& pump_messages, bool& running) {
        if (mode_ != PacerMode::deadline) return;
        const std::int64_t now_qpc = current_qpc();
        if (!deadline_initialized_) {
            initialize_deadline(now_qpc);
        }
        const std::int64_t lead_qpc = (std::max<std::int64_t>(0, lead_time_us) * qpc_frequency_) / 1000000LL;
        const std::int64_t wake_target_qpc = next_deadline_qpc_ - lead_qpc;
        const std::int64_t spin_margin_qpc = (2000LL * qpc_frequency_) / 1000000LL;

        while (running) {
            const std::int64_t current = current_qpc();
            const std::int64_t remaining_qpc = wake_target_qpc - current;
            if (remaining_qpc <= 0) break;
            if (remaining_qpc > spin_margin_qpc && timer_) {
                const std::int64_t sleep_qpc = remaining_qpc - spin_margin_qpc;
                const std::int64_t sleep_100ns = std::max<std::int64_t>(1000LL, (sleep_qpc * 10000000LL) / qpc_frequency_);
                LARGE_INTEGER due_time{};
                due_time.QuadPart = -sleep_100ns;
                SetWaitableTimer(timer_, &due_time, 0, nullptr, nullptr, FALSE);
                HANDLE handles[] = {timer_};
                const DWORD wait_result = MsgWaitForMultipleObjectsEx(1, handles, 20, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
                if (wait_result == WAIT_OBJECT_0 + 1) {
                    pump_messages();
                }
            } else {
                YieldProcessor();
            }
        }
    }

    bool wait_for_present_deadline() {
        if (mode_ != PacerMode::deadline) return false;
        const std::int64_t now_before = current_qpc();
        if (!deadline_initialized_) {
            initialize_deadline(now_before);
            return false;
        }
        const std::int64_t miss_tolerance_qpc = (500LL * qpc_frequency_) / 1000000LL;
        if (now_before > next_deadline_qpc_ + miss_tolerance_qpc) {
            advance_after_present(now_before, true);
            return true;
        }
        while (current_qpc() < next_deadline_qpc_) {
            YieldProcessor();
        }
        advance_after_present(current_qpc(), false);
        return false;
    }

private:
    static std::int64_t current_qpc() {
        LARGE_INTEGER counter{};
        QueryPerformanceCounter(&counter);
        return counter.QuadPart;
    }

    void initialize_deadline(std::int64_t now_qpc) {
        if (target_fps_override_ == 0) {
            DWM_TIMING_INFO timing{};
            timing.cbSize = sizeof(timing);
            if (SUCCEEDED(DwmGetCompositionTimingInfo(nullptr, &timing)) && timing.qpcRefreshPeriod > 0 && timing.qpcVBlank > 0) {
                period_qpc_ = static_cast<std::int64_t>(timing.qpcRefreshPeriod);
                std::int64_t phase = static_cast<std::int64_t>(timing.qpcVBlank);
                if (phase <= now_qpc) {
                    const std::int64_t steps = ((now_qpc - phase) / period_qpc_) + 1;
                    phase += steps * period_qpc_;
                }
                next_deadline_qpc_ = phase;
                deadline_initialized_ = true;
                return;
            }
        }
        next_deadline_qpc_ = now_qpc + period_qpc_;
        deadline_initialized_ = true;
    }

    void advance_after_present(std::int64_t now_qpc, bool missed) {
        if (missed && now_qpc > next_deadline_qpc_ + (period_qpc_ / 2)) {
            const std::int64_t missed_periods = ((now_qpc - next_deadline_qpc_) / period_qpc_) + 1;
            next_deadline_qpc_ += missed_periods * period_qpc_;
            return;
        }
        next_deadline_qpc_ += period_qpc_;
    }

    HANDLE timer_ = nullptr;
    std::int64_t qpc_frequency_ = 10000000LL;
    std::int64_t period_qpc_ = 83333LL;
    std::int64_t next_deadline_qpc_ = 0;
    PacerMode mode_ = PacerMode::deadline;
    std::int32_t target_fps_override_ = 0;
    bool deadline_initialized_ = false;
};

}
