#include "renderer.h"
#include "runtime_state.h"
#include "simulation.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <stdexcept>
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
    RuntimeStatePublisher(bool gpu_timing_available, bool allow_tearing_enabled, bool waitable_swap_chain)
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
        std::atomic_ref<std::int32_t>(state_->vsync_enabled).store(1, std::memory_order_relaxed);
        std::atomic_ref<std::int32_t>(state_->allow_tearing_enabled).store(allow_tearing_enabled ? 1 : 0, std::memory_order_relaxed);
        std::atomic_ref<std::int32_t>(state_->waitable_swap_chain).store(waitable_swap_chain ? 1 : 0, std::memory_order_relaxed);
        std::atomic_ref<std::int32_t>(state_->gpu_timing_available).store(gpu_timing_available ? 1 : 0, std::memory_order_relaxed);
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
        std::int32_t source_divider,
        bool vsync_enabled,
        frame_warp_lab::DiagnosticView view,
        frame_warp_lab::CameraPath path,
        std::int32_t stress_level,
        bool overlay_enabled) {
        std::atomic_ref<std::int32_t>(state_->presentation_mode).store(static_cast<std::int32_t>(presentation_mode), std::memory_order_release);
        std::atomic_ref<std::int32_t>(state_->source_divider).store(source_divider, std::memory_order_release);
        std::atomic_ref<std::int32_t>(state_->vsync_enabled).store(vsync_enabled ? 1 : 0, std::memory_order_release);
        std::atomic_ref<std::int32_t>(state_->diagnostic_view).store(static_cast<std::int32_t>(view), std::memory_order_release);
        std::atomic_ref<std::int32_t>(state_->camera_path).store(static_cast<std::int32_t>(path), std::memory_order_release);
        std::atomic_ref<std::int32_t>(state_->stress_level).store(stress_level, std::memory_order_release);
        std::atomic_ref<std::int32_t>(state_->overlay_enabled).store(overlay_enabled ? 1 : 0, std::memory_order_release);
    }

    void update_scene(double scene_time_seconds, bool paused) {
        std::atomic_ref<std::int64_t>(state_->scene_time_ms).store(static_cast<std::int64_t>(scene_time_seconds * 1000.0), std::memory_order_release);
        std::atomic_ref<std::int32_t>(state_->scene_paused).store(paused ? 1 : 0, std::memory_order_release);
    }

    void record_frame(const frame_warp_lab::RuntimeFrameSample& sample) {
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

        std::atomic_ref<std::int64_t>(state_->last_present_interval_us).store(sample.present_interval_us, std::memory_order_release);
        std::atomic_ref<std::int64_t>(state_->last_source_age_us).store(sample.source_age_us, std::memory_order_release);
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

frame_warp_lab::ManualCameraInput poll_manual_camera_input(HWND window) {
    frame_warp_lab::ManualCameraInput input{};
    if (GetForegroundWindow() != window) return input;
    auto key_down = [](int virtual_key) -> bool { return (GetAsyncKeyState(virtual_key) & 0x8000) != 0; };
    if (key_down('W')) input.forward_axis += 1.0f;
    if (key_down('S')) input.forward_axis -= 1.0f;
    if (key_down('D')) input.right_axis += 1.0f;
    if (key_down('A')) input.right_axis -= 1.0f;
    if (key_down(VK_RIGHT)) input.yaw_axis += 1.0f;
    if (key_down(VK_LEFT)) input.yaw_axis -= 1.0f;
    if (key_down(VK_UP)) input.pitch_axis += 1.0f;
    if (key_down(VK_DOWN)) input.pitch_axis -= 1.0f;
    return input;
}

LRESULT CALLBACK window_procedure(HWND window, UINT message, WPARAM message_parameter, LPARAM message_data) {
    if (message == WM_DESTROY) {
        PostQuitMessage(0);
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

        frame_warp_lab::Renderer renderer(window);
        RuntimeStatePublisher runtime_state(
            renderer.gpu_timing_available(),
            renderer.allow_tearing_enabled(),
            renderer.waitable_swap_chain_enabled());
        frame_warp_lab::SceneSimulation simulation;

        ShowWindow(window, show);
        RECT previous_client_rectangle{};
        GetClientRect(window, &previous_client_rectangle);

        MSG message{};
        bool running = true;
        bool scene_paused = false;
        frame_warp_lab::PresentationMode presentation_mode = frame_warp_lab::PresentationMode::full_render;
        std::int32_t source_divider = 1;
        bool vsync_enabled = true;
        frame_warp_lab::DiagnosticView diagnostic_view = frame_warp_lab::DiagnosticView::final_color;
        std::int32_t stress_level = 0;
        bool overlay_enabled = true;

        std::int64_t display_frame_id = 0;
        std::int64_t source_frame_id = 0;
        std::int64_t cadence_counter = 0;
        bool has_completed_source = false;
        frame_warp_lab::SceneSnapshot latest_source_snapshot{};
        auto latest_source_capture_time = std::chrono::steady_clock::now();
        auto previous_tick = latest_source_capture_time;
        auto previous_present_tick = latest_source_capture_time;
        bool has_previous_present = false;

        runtime_state.update_config(presentation_mode, source_divider, vsync_enabled, diagnostic_view, simulation.path(), stress_level, overlay_enabled);

        while (running) {
            while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                if (message.message == WM_QUIT) {
                    running = false;
                    break;
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
                        runtime_state.record_event(frame_warp_lab::RuntimeEventType::config_updated, vsync_enabled ? 601 : 600);
                    }
                    previous_tick = std::chrono::steady_clock::now();
                    runtime_state.update_config(presentation_mode, source_divider, vsync_enabled, diagnostic_view, simulation.path(), stress_level, overlay_enabled);
                    runtime_state.update_scene(simulation.time_seconds(), scene_paused);
                    continue;
                }
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
            if (!running) break;
            if (IsIconic(window)) {
                WaitMessage();
                previous_tick = std::chrono::steady_clock::now();
                has_previous_present = false;
                continue;
            }

            const auto current_tick = std::chrono::steady_clock::now();
            const frame_warp_lab::ManualCameraInput manual_input = poll_manual_camera_input(window);
            simulation.advance(std::chrono::duration<double>(current_tick - previous_tick).count(), scene_paused, manual_input);
            previous_tick = current_tick;

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
                continue;
            }
            if (current_client_rectangle.right != previous_client_rectangle.right || current_client_rectangle.bottom != previous_client_rectangle.bottom) {
                renderer.resize(client_width, client_height);
                runtime_state.record_resize();
                previous_client_rectangle = current_client_rectangle;
                has_completed_source = false;
                cadence_counter = 0;
            }

            ++display_frame_id;
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

            const frame_warp_lab::FrameExecutionMetrics gpu_metrics = renderer.render_and_present(frame_warp_lab::FrameRenderRequest{
                latest_source_snapshot,
                target_camera,
                diagnostic_view,
                stress_level,
                overlay_enabled,
                render_scene_pass,
                warp_enabled,
                vsync_enabled,
            });
            const auto present_complete_time = std::chrono::steady_clock::now();
            const std::int64_t present_interval_us = has_previous_present
                ? std::chrono::duration_cast<std::chrono::microseconds>(present_complete_time - previous_present_tick).count()
                : 0;
            const std::int64_t source_age_us = std::chrono::duration_cast<std::chrono::microseconds>(present_complete_time - latest_source_capture_time).count();
            previous_present_tick = present_complete_time;
            has_previous_present = true;

            runtime_state.update_scene(simulation.time_seconds(), scene_paused);
            runtime_state.record_frame(frame_warp_lab::RuntimeFrameSample{
                0,
                display_frame_id,
                latest_source_snapshot.source_frame_id,
                present_interval_us,
                source_age_us,
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
                frame_discontinuity ? 1 : 0,
            });
        }

        renderer.flush();
        return 0;
    } catch (const std::exception& error) {
        MessageBoxA(nullptr, error.what(), "Frame Warp Lab error", MB_OK | MB_ICONERROR);
        return 1;
    }
}
