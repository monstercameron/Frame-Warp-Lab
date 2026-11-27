#include "adaptive_scheduler.h"
#include "frame_pacer.h"
#include "latency_monitor.h"
#include "renderer.h"
#include "runtime_state.h"
#include "simulation.h"
#include "tactical_range.h"

#include <windows.h>
#include <windowsx.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

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
    RuntimeStatePublisher(
        bool gpu_timing_available,
        bool allow_tearing_enabled,
        bool waitable_swap_chain,
        bool raw_input_enabled,
        bool high_priority_present_queue,
        std::int32_t vrs_tier,
        std::int32_t vrs_tile_size_px,
        bool vrs_additional_rates_supported,
        std::int32_t dxr_tier,
        bool dust2_loaded,
        std::int64_t dust2_pbr_build_ms,
        const frame_warp_lab::StreamlineCapabilities& streamline_caps)
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
        std::atomic_ref<std::int32_t>(state_->allow_tearing_enabled).store(allow_tearing_enabled ? 1 : 0, std::memory_order_relaxed);
        std::atomic_ref<std::int32_t>(state_->waitable_swap_chain).store(waitable_swap_chain ? 1 : 0, std::memory_order_relaxed);
        std::atomic_ref<std::int32_t>(state_->high_priority_present_queue).store(high_priority_present_queue ? 1 : 0, std::memory_order_relaxed);
        std::atomic_ref<std::int32_t>(state_->vrs_tier).store(vrs_tier, std::memory_order_relaxed);
        std::atomic_ref<std::int32_t>(state_->vrs_tile_size_px).store(vrs_tile_size_px, std::memory_order_relaxed);
        std::atomic_ref<std::int32_t>(state_->vrs_additional_rates_supported).store(vrs_additional_rates_supported ? 1 : 0, std::memory_order_relaxed);
        std::atomic_ref<std::int32_t>(state_->dxr_tier).store(dxr_tier, std::memory_order_relaxed);
        std::atomic_ref<std::int32_t>(state_->dust2_loaded).store(dust2_loaded ? 1 : 0, std::memory_order_relaxed);
        std::atomic_ref<std::int64_t>(state_->dust2_pbr_build_ms).store(dust2_pbr_build_ms, std::memory_order_relaxed);
        std::atomic_ref<std::int32_t>(state_->raw_input_enabled).store(raw_input_enabled ? 1 : 0, std::memory_order_relaxed);
        std::atomic_ref<std::int32_t>(state_->gpu_timing_available).store(gpu_timing_available ? 1 : 0, std::memory_order_relaxed);
        std::atomic_ref<std::int32_t>(state_->streamline_initialized).store(streamline_caps.initialized ? 1 : 0, std::memory_order_relaxed);
        std::atomic_ref<std::int32_t>(state_->dlssg_supported).store(streamline_caps.dlssg_supported ? 1 : 0, std::memory_order_relaxed);
        std::atomic_ref<std::int32_t>(state_->dlss_sr_supported).store(streamline_caps.dlss_sr_supported ? 1 : 0, std::memory_order_relaxed);
        std::atomic_ref<std::int32_t>(state_->reflex_supported).store(streamline_caps.reflex_supported ? 1 : 0, std::memory_order_relaxed);
        std::atomic_ref<std::int32_t>(state_->dlssg_dynamic_mfg_supported).store(streamline_caps.dynamic_mfg_supported ? 1 : 0, std::memory_order_relaxed);
        std::atomic_ref<std::int32_t>(state_->dlssg_max_frames_to_generate).store(static_cast<std::int32_t>(streamline_caps.max_frames_to_generate), std::memory_order_relaxed);
        std::atomic_ref<std::int32_t>(state_->dlssg_requested_multiplier).store(2, std::memory_order_relaxed);
        std::atomic_ref<std::int32_t>(state_->tile_budget_pct).store(100, std::memory_order_relaxed);
        std::atomic_ref<std::int32_t>(state_->tile_policy).store(static_cast<std::int32_t>(frame_warp_lab::TilePolicy::priority_stbn), std::memory_order_relaxed);
        std::atomic_ref<std::int64_t>(state_->last_input_to_present_us).store(-1, std::memory_order_relaxed);
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
        frame_warp_lab::PacerMode pacer_mode,
        std::int32_t target_fps,
        std::int64_t target_period_us,
        std::int32_t source_divider,
        bool vsync_enabled,
        bool async_gpu_enabled,
        frame_warp_lab::VrsMode vrs_requested_mode,
        frame_warp_lab::VrsMode vrs_active_mode,
        frame_warp_lab::InstrumentationLevel instrumentation_level,
        frame_warp_lab::DiagnosticView view,
        frame_warp_lab::CameraPath path,
        std::int32_t stress_level,
        std::uint32_t shader_flags,
        bool pbr_enabled,
        bool aniso_mip_enabled,
        frame_warp_lab::SceneKind scene_kind,
        bool overlay_enabled,
        bool flash_indicator_enabled) {
        std::atomic_ref<std::int32_t>(state_->presentation_mode).store(static_cast<std::int32_t>(presentation_mode), std::memory_order_release);
        std::atomic_ref<std::int32_t>(state_->pacer_mode).store(static_cast<std::int32_t>(pacer_mode), std::memory_order_release);
        std::atomic_ref<std::int32_t>(state_->target_fps).store(target_fps, std::memory_order_release);
        std::atomic_ref<std::int64_t>(state_->target_period_us).store(target_period_us, std::memory_order_release);
        std::atomic_ref<std::int32_t>(state_->source_divider).store(source_divider, std::memory_order_release);
        std::atomic_ref<std::int32_t>(state_->vsync_enabled).store(vsync_enabled ? 1 : 0, std::memory_order_release);
        std::atomic_ref<std::int32_t>(state_->async_gpu_enabled).store(async_gpu_enabled ? 1 : 0, std::memory_order_release);
        std::atomic_ref<std::int32_t>(state_->vrs_requested_mode).store(static_cast<std::int32_t>(vrs_requested_mode), std::memory_order_release);
        std::atomic_ref<std::int32_t>(state_->vrs_active_mode).store(static_cast<std::int32_t>(vrs_active_mode), std::memory_order_release);
        std::atomic_ref<std::int32_t>(state_->instrumentation_level).store(static_cast<std::int32_t>(instrumentation_level), std::memory_order_release);
        std::atomic_ref<std::int32_t>(state_->diagnostic_view).store(static_cast<std::int32_t>(view), std::memory_order_release);
        std::atomic_ref<std::int32_t>(state_->camera_path).store(static_cast<std::int32_t>(path), std::memory_order_release);
        std::atomic_ref<std::int32_t>(state_->stress_level).store(stress_level, std::memory_order_release);
        std::atomic_ref<std::uint32_t>(state_->shader_flags).store(shader_flags, std::memory_order_release);
        std::atomic_ref<std::int32_t>(state_->pbr_enabled).store(pbr_enabled ? 1 : 0, std::memory_order_release);
        std::atomic_ref<std::int32_t>(state_->aniso_mip_enabled).store(aniso_mip_enabled ? 1 : 0, std::memory_order_release);
        std::atomic_ref<std::int32_t>(state_->scene_kind).store(static_cast<std::int32_t>(scene_kind), std::memory_order_release);
        std::atomic_ref<std::int32_t>(state_->overlay_enabled).store(overlay_enabled ? 1 : 0, std::memory_order_release);
        std::atomic_ref<std::int32_t>(state_->flash_indicator_enabled).store(flash_indicator_enabled ? 1 : 0, std::memory_order_release);
    }

    void update_tactical_range(
        const frame_warp_lab::TacticalHudSnapshot& h,
        const frame_warp_lab::EcsLuaTelemetry& t,
        bool hero_pbr_textures_loaded,
        std::int64_t pbr_build_ms) {
        std::atomic_ref<std::int32_t>(state_->tactical_hp).store(h.hp, std::memory_order_release);
        std::atomic_ref<std::int32_t>(state_->tactical_armor).store(h.armor, std::memory_order_release);
        std::atomic_ref<std::int32_t>(state_->tactical_ammo_in_mag).store(h.ammo_in_mag, std::memory_order_release);
        std::atomic_ref<std::int32_t>(state_->tactical_ammo_reserve).store(h.ammo_reserve, std::memory_order_release);
        std::atomic_ref<std::int32_t>(state_->tactical_shots_fired).store(h.shots_fired, std::memory_order_release);
        std::atomic_ref<std::int32_t>(state_->tactical_hits).store(h.hits, std::memory_order_release);
        std::atomic_ref<std::int32_t>(state_->tactical_headshots).store(h.headshots, std::memory_order_release);
        std::atomic_ref<std::int32_t>(state_->tactical_score).store(h.score, std::memory_order_release);
        std::atomic_ref<std::int32_t>(state_->tactical_targets_up).store(h.targets_up, std::memory_order_release);
        std::atomic_ref<std::int32_t>(state_->tactical_ads_active).store(h.ads_alpha > 0.5f ? 1 : 0, std::memory_order_release);
        std::atomic_ref<std::uint32_t>(state_->ecs_entity_count).store(t.entity_count, std::memory_order_release);
        std::atomic_ref<std::uint32_t>(state_->ecs_renderable_count).store(t.renderable_count, std::memory_order_release);
        std::atomic_ref<std::uint32_t>(state_->ecs_physics_count).store(t.physics_body_count, std::memory_order_release);
        std::atomic_ref<std::uint32_t>(state_->ecs_script_count).store(t.script_count, std::memory_order_release);
        std::atomic_ref<std::uint32_t>(state_->lua_loaded_scripts).store(t.lua_loaded_scripts, std::memory_order_release);
        std::atomic_ref<std::uint32_t>(state_->lua_reload_count).store(t.lua_reload_count, std::memory_order_release);
        std::atomic_ref<std::uint32_t>(state_->lua_error_count).store(t.lua_error_count, std::memory_order_release);
        std::atomic_ref<std::int64_t>(state_->lua_last_step_us).store(t.lua_last_step_us, std::memory_order_release);
        std::atomic_ref<std::uint64_t>(state_->events_dispatched).store(t.events_dispatched, std::memory_order_release);
        std::atomic_ref<std::uint32_t>(state_->event_subscriptions).store(t.event_subscriptions, std::memory_order_release);
        std::atomic_ref<std::int32_t>(state_->hero_pistol_gltf_loaded).store(t.hero_pistol_gltf_loaded ? 1 : 0, std::memory_order_release);
        std::atomic_ref<std::int32_t>(state_->hero_pbr_textures_loaded).store(hero_pbr_textures_loaded ? 1 : 0, std::memory_order_release);
        std::atomic_ref<std::uint32_t>(state_->mt_worker_threads).store(t.mt_worker_threads, std::memory_order_release);
        std::atomic_ref<std::uint32_t>(state_->mt_jolt_threads).store(t.mt_jolt_threads, std::memory_order_release);
        std::atomic_ref<std::uint64_t>(state_->mt_jobs_executed).store(t.mt_jobs_executed, std::memory_order_release);
        std::atomic_ref<std::int64_t>(state_->mt_physics_step_us).store(t.mt_physics_step_us, std::memory_order_release);
        std::atomic_ref<std::int64_t>(state_->mt_ecs_sync_us).store(t.mt_ecs_sync_us, std::memory_order_release);
        std::atomic_ref<std::int64_t>(state_->mt_pbr_build_ms).store(pbr_build_ms, std::memory_order_release);
        std::atomic_ref<std::int32_t>(state_->tactical_spread_mrad_x100).store(static_cast<std::int32_t>(t.current_spread_mrad * 100.0f), std::memory_order_release);
        std::atomic_ref<std::int32_t>(state_->tactical_shot_offset_mrad_x100).store(static_cast<std::int32_t>(t.last_shot_offset_mrad * 100.0f), std::memory_order_release);
    }

    void update_dust2_octree(const frame_warp_lab::Dust2OctreeTelemetry& d) {
        std::atomic_ref<std::uint32_t>(state_->dust2_octree_total_nodes).store(d.total_nodes, std::memory_order_release);
        std::atomic_ref<std::uint32_t>(state_->dust2_octree_visited_nodes).store(d.visited_nodes, std::memory_order_release);
        std::atomic_ref<std::uint32_t>(state_->dust2_octree_culled_nodes).store(d.culled_nodes, std::memory_order_release);
        std::atomic_ref<std::uint32_t>(state_->dust2_octree_probe_occluded).store(d.probe_occluded_nodes, std::memory_order_release);
        std::atomic_ref<std::uint32_t>(state_->dust2_lod0_chunks).store(d.lod0_chunks, std::memory_order_release);
        std::atomic_ref<std::uint32_t>(state_->dust2_lod1_chunks).store(d.lod1_chunks, std::memory_order_release);
        std::atomic_ref<std::uint32_t>(state_->dust2_lod2_chunks).store(d.lod2_chunks, std::memory_order_release);
        std::atomic_ref<std::uint32_t>(state_->dust2_submitted_tris).store(d.submitted_triangles, std::memory_order_release);
        std::atomic_ref<std::uint32_t>(state_->dust2_total_lod0_tris).store(d.total_lod0_triangles, std::memory_order_release);
        std::atomic_ref<std::uint32_t>(state_->dust2_active_probes).store(d.active_probes, std::memory_order_release);
        std::atomic_ref<std::uint32_t>(state_->dust2_active_lights).store(d.active_lights, std::memory_order_release);
    }

    void update_dlssg(
        std::int32_t requested_multiplier,
        bool window_focused,
        frame_warp_lab::PacingOwner pacing_owner,
        bool adaptive_dlssg_enabled,
        bool warp_guard_active,
        std::int64_t warp_guard_trips,
        const frame_warp_lab::StreamlineFrameState& dlssg) {
        std::atomic_ref<std::int32_t>(state_->window_focused).store(window_focused ? 1 : 0, std::memory_order_release);
        std::atomic_ref<std::int32_t>(state_->pacing_owner).store(static_cast<std::int32_t>(pacing_owner), std::memory_order_release);
        std::atomic_ref<std::int32_t>(state_->adaptive_dlssg_enabled).store(adaptive_dlssg_enabled ? 1 : 0, std::memory_order_release);
        std::atomic_ref<std::int32_t>(state_->dlssg_requested_multiplier).store(requested_multiplier, std::memory_order_release);
        std::atomic_ref<std::int32_t>(state_->dlssg_active).store(dlssg.dlssg_active ? 1 : 0, std::memory_order_release);
        std::atomic_ref<std::int32_t>(state_->dlssg_last_api_result).store(dlssg.last_result_code, std::memory_order_release);
        std::atomic_ref<std::int32_t>(state_->dlssg_last_failed_call).store(dlssg.last_failed_call, std::memory_order_release);
        std::atomic_ref<std::int64_t>(state_->dlssg_api_error_count).store(dlssg.api_error_count, std::memory_order_release);
        std::atomic_ref<std::int32_t>(state_->dlssg_warp_guard_active).store(warp_guard_active ? 1 : 0, std::memory_order_release);
        std::atomic_ref<std::int64_t>(state_->dlssg_warp_guard_trips).store(warp_guard_trips, std::memory_order_release);
        std::atomic_ref<std::uint32_t>(state_->dlssg_status_flags).store(dlssg.dlssg_status_flags, std::memory_order_release);
        std::atomic_ref<std::uint32_t>(state_->dlssg_last_frames_presented).store(dlssg.frames_actually_presented, std::memory_order_release);
        if (dlssg.max_frames_to_generate > 0U) {
            std::atomic_ref<std::int32_t>(state_->dlssg_max_frames_to_generate).store(static_cast<std::int32_t>(dlssg.max_frames_to_generate), std::memory_order_release);
        }
        std::atomic_ref<std::int32_t>(state_->dlssg_dynamic_mfg_supported).store(dlssg.dynamic_mfg_supported ? 1 : 0, std::memory_order_release);
        if (dlssg.dlssg_active && dlssg.frames_actually_presented > 0U) {
            std::atomic_ref<std::int64_t>(state_->dlssg_total_frames_presented).fetch_add(static_cast<std::int64_t>(dlssg.frames_actually_presented), std::memory_order_release);
            if (dlssg.frames_actually_presented > 1U) {
                std::atomic_ref<std::int64_t>(state_->dlssg_total_generated_frames).fetch_add(static_cast<std::int64_t>(dlssg.frames_actually_presented - 1U), std::memory_order_release);
            }
        }
    }

    void update_adaptive(bool enabled, const frame_warp_lab::AdaptiveTelemetry& telemetry) {
        std::atomic_ref<std::int32_t>(state_->adaptive_enabled).store(enabled ? 1 : 0, std::memory_order_release);
        std::atomic_ref<std::int32_t>(state_->adaptive_tier).store(telemetry.tier, std::memory_order_release);
        std::atomic_ref<std::int32_t>(state_->adaptive_reason).store(static_cast<std::int32_t>(telemetry.reason), std::memory_order_release);
        const auto permille = static_cast<std::int32_t>(std::clamp(telemetry.gpu_budget_utilization * 1000.0, 0.0, 999999.0));
        std::atomic_ref<std::int32_t>(state_->adaptive_utilization_permille).store(permille, std::memory_order_release);
        std::atomic_ref<std::int64_t>(state_->adaptive_reserved_headroom_us).store(telemetry.reserved_headroom_us, std::memory_order_release);
        std::atomic_ref<std::int64_t>(state_->adaptive_decision_count).store(telemetry.decision_count, std::memory_order_release);
    }

    void update_unified_controller(const frame_warp_lab::UnifiedControllerTelemetry& telemetry) {
        std::atomic_ref<std::int32_t>(state_->unified_controller_enabled).store(telemetry.enabled ? 1 : 0, std::memory_order_release);
        std::atomic_ref<std::int32_t>(state_->unified_controller_tier).store(telemetry.tier, std::memory_order_release);
        std::atomic_ref<std::int32_t>(state_->unified_controller_util_permille).store(telemetry.utilization_permille, std::memory_order_release);
        std::atomic_ref<std::int64_t>(state_->unified_controller_budget_us).store(telemetry.target_budget_us, std::memory_order_release);
        std::atomic_ref<std::int64_t>(state_->unified_controller_transitions).store(telemetry.transitions, std::memory_order_release);
        std::atomic_ref<std::int64_t>(state_->unified_controller_oscillations).store(telemetry.oscillations, std::memory_order_release);
    }

    void update_selective_tiles(
        bool enabled,
        std::int32_t budget_pct,
        frame_warp_lab::TilePolicy policy,
        bool auto_budget_enabled,
        bool cache_sharpen_enabled,
        bool coarse_fallback_enabled,
        bool subtile_interleave_enabled,
        bool reactive_invalidation_enabled,
        bool shadow_delta_invalidation_enabled,
        bool forward_transparency_enabled,
        bool split_diff_spec_enabled,
        bool surface_cache_enabled,
        bool clustered_lighting_enabled,
        bool thin_gbuf_enabled,
        std::int32_t surface_cache_mb,
        bool par_bypass_active,
        std::int32_t smoothed_damage_pct,
        std::int32_t render_scale_pct,
        std::int32_t render_width,
        std::int32_t render_height,
        std::int32_t alloc_render_width,
        std::int32_t alloc_render_height,
        frame_warp_lab::UpscaleFilter upscale_filter,
        frame_warp_lab::DrsMode drs_mode,
        std::int32_t drs_min_pct,
        std::int32_t drs_max_pct,
        std::int64_t drs_scale_changes,
        std::int64_t drs_reallocations,
        std::int64_t drs_target_scene_budget_us,
        std::int32_t fixed_dt_us,
        std::int64_t target_scene_budget_us,
        const frame_warp_lab::TileCacheStats& stats) {
        std::atomic_ref<std::int32_t>(state_->selective_tiles_enabled).store(enabled ? 1 : 0, std::memory_order_release);
        std::atomic_ref<std::int32_t>(state_->tile_budget_pct).store(budget_pct, std::memory_order_release);
        std::atomic_ref<std::int32_t>(state_->tile_policy).store(static_cast<std::int32_t>(policy), std::memory_order_release);
        std::atomic_ref<std::int32_t>(state_->auto_tile_budget_enabled).store(auto_budget_enabled ? 1 : 0, std::memory_order_release);
        std::atomic_ref<std::int32_t>(state_->cache_sharpen_enabled).store(cache_sharpen_enabled ? 1 : 0, std::memory_order_release);
        std::atomic_ref<std::int32_t>(state_->coarse_fallback_enabled).store(coarse_fallback_enabled ? 1 : 0, std::memory_order_release);
        std::atomic_ref<std::int32_t>(state_->subtile_interleave_enabled).store(subtile_interleave_enabled ? 1 : 0, std::memory_order_release);
        std::atomic_ref<std::int32_t>(state_->reactive_invalidation_enabled).store(reactive_invalidation_enabled ? 1 : 0, std::memory_order_release);
        std::atomic_ref<std::int32_t>(state_->shadow_delta_invalidation_enabled).store(shadow_delta_invalidation_enabled ? 1 : 0, std::memory_order_release);
        std::atomic_ref<std::int32_t>(state_->forward_transparency_enabled).store(forward_transparency_enabled ? 1 : 0, std::memory_order_release);
        std::atomic_ref<std::int32_t>(state_->split_diff_spec_enabled).store(split_diff_spec_enabled ? 1 : 0, std::memory_order_release);
        std::atomic_ref<std::int32_t>(state_->surface_cache_enabled).store(surface_cache_enabled ? 1 : 0, std::memory_order_release);
        std::atomic_ref<std::int32_t>(state_->clustered_lighting_enabled).store(clustered_lighting_enabled ? 1 : 0, std::memory_order_release);
        std::atomic_ref<std::int32_t>(state_->thin_gbuf_enabled).store(thin_gbuf_enabled ? 1 : 0, std::memory_order_release);
        std::atomic_ref<std::int32_t>(state_->surface_cache_mb).store(surface_cache_mb, std::memory_order_release);
        std::atomic_ref<std::uint32_t>(state_->last_sc_hits).store(stats.sc_hits, std::memory_order_release);
        std::atomic_ref<std::uint32_t>(state_->last_sc_misses).store(stats.sc_misses, std::memory_order_release);
        std::atomic_ref<std::uint32_t>(state_->last_sc_writes).store(stats.sc_writes, std::memory_order_release);
        std::atomic_ref<std::uint32_t>(state_->last_sc_collisions).store(stats.sc_collisions, std::memory_order_release);
        std::atomic_ref<std::uint32_t>(state_->last_clustered_light_sum).store(stats.clustered_light_sum, std::memory_order_release);
        std::atomic_ref<std::int32_t>(state_->par_bypass_active).store(par_bypass_active ? 1 : 0, std::memory_order_release);
        std::atomic_ref<std::int32_t>(state_->smoothed_damage_pct).store(smoothed_damage_pct, std::memory_order_release);
        std::atomic_ref<std::int32_t>(state_->render_scale_pct).store(render_scale_pct, std::memory_order_release);
        std::atomic_ref<std::int32_t>(state_->render_width).store(render_width, std::memory_order_release);
        std::atomic_ref<std::int32_t>(state_->render_height).store(render_height, std::memory_order_release);
        std::atomic_ref<std::int32_t>(state_->alloc_render_width).store(alloc_render_width, std::memory_order_release);
        std::atomic_ref<std::int32_t>(state_->alloc_render_height).store(alloc_render_height, std::memory_order_release);
        std::atomic_ref<std::int32_t>(state_->upscale_filter).store(static_cast<std::int32_t>(upscale_filter), std::memory_order_release);
        std::atomic_ref<std::int32_t>(state_->drs_mode).store(static_cast<std::int32_t>(drs_mode), std::memory_order_release);
        std::atomic_ref<std::int32_t>(state_->drs_min_pct).store(drs_min_pct, std::memory_order_release);
        std::atomic_ref<std::int32_t>(state_->drs_max_pct).store(drs_max_pct, std::memory_order_release);
        std::atomic_ref<std::int64_t>(state_->drs_scale_changes).store(drs_scale_changes, std::memory_order_release);
        std::atomic_ref<std::int64_t>(state_->drs_reallocations).store(drs_reallocations, std::memory_order_release);
        std::atomic_ref<std::int64_t>(state_->drs_target_scene_budget_us).store(drs_target_scene_budget_us, std::memory_order_release);
        std::atomic_ref<std::int32_t>(state_->fixed_dt_us).store(fixed_dt_us, std::memory_order_release);
        std::atomic_ref<std::int64_t>(state_->last_target_scene_budget_us).store(target_scene_budget_us, std::memory_order_release);
        std::atomic_ref<std::uint32_t>(state_->last_tile_total_count).store(stats.total_tiles, std::memory_order_release);
        std::atomic_ref<std::uint32_t>(state_->last_tile_cached_count).store(stats.cached_tiles, std::memory_order_release);
        std::atomic_ref<std::uint32_t>(state_->last_tile_shaded_count).store(stats.shaded_tiles, std::memory_order_release);
        std::atomic_ref<std::uint32_t>(state_->last_tile_deferred_count).store(stats.deferred_tiles, std::memory_order_release);
        std::atomic_ref<std::uint32_t>(state_->last_tile_critical_total).store(stats.critical_total_tiles, std::memory_order_release);
        std::atomic_ref<std::uint32_t>(state_->last_tile_critical_deferred).store(stats.critical_deferred_tiles, std::memory_order_release);
    }

    void record_capture_completed(std::uint32_t tag, std::int64_t frame_id, std::int64_t scene_time_us) {
        std::atomic_ref<std::int64_t>(state_->last_capture_frame_id).store(frame_id, std::memory_order_relaxed);
        std::atomic_ref<std::int64_t>(state_->last_capture_scene_time_us).store(scene_time_us, std::memory_order_relaxed);
        std::atomic_ref<std::uint32_t>(state_->last_capture_tag).store(tag, std::memory_order_release);
    }

    void update_pending_steps(std::int32_t pending) {
        std::atomic_ref<std::int32_t>(state_->pending_step_frames).store(pending, std::memory_order_release);
    }

    void update_scene(
        double scene_time_seconds,
        bool paused,
        bool mouse_look_active,
        float yaw_deg,
        float pitch_deg,
        const frame_warp_lab::LatencyMonitor& latency_monitor) {
        std::atomic_ref<std::int64_t>(state_->scene_time_ms).store(static_cast<std::int64_t>(scene_time_seconds * 1000.0), std::memory_order_release);
        std::atomic_ref<std::int32_t>(state_->scene_paused).store(paused ? 1 : 0, std::memory_order_release);
        std::atomic_ref<std::int32_t>(state_->mouse_look_active).store(mouse_look_active ? 1 : 0, std::memory_order_release);
        std::atomic_ref<std::int32_t>(state_->camera_yaw_millideg).store(static_cast<std::int32_t>(yaw_deg * 1000.0f), std::memory_order_release);
        std::atomic_ref<std::int32_t>(state_->camera_pitch_millideg).store(static_cast<std::int32_t>(pitch_deg * 1000.0f), std::memory_order_release);
        std::atomic_ref<std::int32_t>(state_->pcl_etw_listener_active).store(latency_monitor.etw_listener_active() ? 1 : 0, std::memory_order_release);
        std::atomic_ref<std::int64_t>(state_->pcl_markers_emitted).store(latency_monitor.markers_emitted(), std::memory_order_release);
        std::atomic_ref<std::int64_t>(state_->pcl_pings_received).store(latency_monitor.pings_received(), std::memory_order_release);
    }

    void record_frame(const frame_warp_lab::RuntimeFrameSample& sample, std::int64_t smoothed_present_interval_us) {
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
            case frame_warp_lab::FrameKind::generated:
                std::atomic_ref<std::int64_t>(state_->generated_frame_count).fetch_add(1, std::memory_order_release);
                break;
            case frame_warp_lab::FrameKind::combined:
                std::atomic_ref<std::int64_t>(state_->combined_frame_count).fetch_add(1, std::memory_order_release);
                break;
            case frame_warp_lab::FrameKind::dlss_generated:
                std::atomic_ref<std::int64_t>(state_->generated_frame_count).fetch_add(1, std::memory_order_release);
                break;
            case frame_warp_lab::FrameKind::dlss_warped:
                std::atomic_ref<std::int64_t>(state_->combined_frame_count).fetch_add(1, std::memory_order_release);
                break;
        }
        if (sample.async_overlapped) {
            std::atomic_ref<std::int64_t>(state_->async_overlap_count).fetch_add(1, std::memory_order_release);
        }
        if (sample.missed_deadline) {
            std::atomic_ref<std::int64_t>(state_->missed_deadline_count).fetch_add(1, std::memory_order_release);
        }

        std::atomic_ref<std::int64_t>(state_->last_present_interval_us).store(sample.present_interval_us, std::memory_order_release);
        std::atomic_ref<std::int64_t>(state_->smoothed_present_interval_us).store(smoothed_present_interval_us, std::memory_order_release);
        std::atomic_ref<std::int64_t>(state_->last_source_age_us).store(sample.source_age_us, std::memory_order_release);
        if (sample.input_to_present_us >= 0) {
            std::atomic_ref<std::int64_t>(state_->last_input_to_present_us).store(sample.input_to_present_us, std::memory_order_release);
        }
        std::atomic_ref<std::int64_t>(state_->last_sim_cpu_us).store(sample.sim_cpu_us, std::memory_order_release);
        std::atomic_ref<std::int64_t>(state_->last_render_submit_cpu_us).store(sample.render_submit_cpu_us, std::memory_order_release);
        std::atomic_ref<std::int64_t>(state_->last_present_call_cpu_us).store(sample.present_call_cpu_us, std::memory_order_release);
        if (sample.scene_gpu_us >= 0) {
            std::atomic_ref<std::int64_t>(state_->last_scene_gpu_us).store(sample.scene_gpu_us, std::memory_order_release);
        }
        if (sample.present_gpu_us >= 0) {
            std::atomic_ref<std::int64_t>(state_->last_present_gpu_us).store(sample.present_gpu_us, std::memory_order_release);
        }
        std::atomic_ref<std::uint32_t>(state_->last_invalid_pixel_count).store(sample.invalid_pixel_count, std::memory_order_release);
        std::atomic_ref<std::uint32_t>(state_->last_temporal_recovered_count).store(sample.temporal_recovered_count, std::memory_order_release);
        std::atomic_ref<std::uint32_t>(state_->last_total_pixel_count).store(sample.total_pixel_count, std::memory_order_release);

        const std::int64_t sequence = std::atomic_ref<std::int64_t>(state_->trace_count).load(std::memory_order_relaxed) + 1;
        auto& slot = state_->trace[(static_cast<std::size_t>(sequence) - 1) % frame_warp_lab::runtime_frame_trace_capacity];
        frame_warp_lab::seqlock_write(slot, [&](frame_warp_lab::RuntimeFrameSample& target) {
            target.sequence = sequence;
            target.display_frame_id = sample.display_frame_id;
            target.source_frame_id = sample.source_frame_id;
            target.gpu_measured_display_frame_id = sample.gpu_measured_display_frame_id;
            target.gpu_measured_source_frame_id = sample.gpu_measured_source_frame_id;
            target.present_interval_us = sample.present_interval_us;
            target.source_age_us = sample.source_age_us;
            target.input_to_present_us = sample.input_to_present_us;
            target.sim_cpu_us = sample.sim_cpu_us;
            target.render_submit_cpu_us = sample.render_submit_cpu_us;
            target.present_call_cpu_us = sample.present_call_cpu_us;
            target.scene_gpu_us = sample.scene_gpu_us;
            target.present_gpu_us = sample.present_gpu_us;
            target.invalid_pixel_count = sample.invalid_pixel_count;
            target.temporal_recovered_count = sample.temporal_recovered_count;
            target.total_pixel_count = sample.total_pixel_count;
            target.presentation_mode = sample.presentation_mode;
            target.frame_kind = sample.frame_kind;
            target.fallback_reason = sample.fallback_reason;
            target.diagnostic_view = sample.diagnostic_view;
            target.camera_path = sample.camera_path;
            target.stress_level = sample.stress_level;
            target.vrs_mode = sample.vrs_mode;
            target.adaptive_tier = sample.adaptive_tier;
            target.source_divider = sample.source_divider;
            target.source_rendered = sample.source_rendered;
            target.gpu_slot_warped = sample.gpu_slot_warped;
            target.async_overlapped = sample.async_overlapped;
            target.missed_deadline = sample.missed_deadline;
            target.discontinuity = sample.discontinuity;
            target.dlssg_frames_presented = sample.dlssg_frames_presented;
            target.tile_total_count = sample.tile_total_count;
            target.tile_shaded_count = sample.tile_shaded_count;
            target.tile_deferred_count = sample.tile_deferred_count;
            target.tile_critical_total = sample.tile_critical_total;
            target.tile_critical_deferred = sample.tile_critical_deferred;
        });
        std::atomic_ref<std::int64_t>(state_->trace_count).store(sequence, std::memory_order_release);
    }

    void update_present_variant(frame_warp_lab::PresentVariant variant) {
        std::atomic_ref<std::int32_t>(state_->last_present_variant).store(static_cast<std::int32_t>(variant), std::memory_order_release);
    }

    void record_resize() {
        const std::int32_t resize_count = std::atomic_ref<std::int32_t>(state_->resize_count).fetch_add(1, std::memory_order_acq_rel) + 1;
        record_event(frame_warp_lab::RuntimeEventType::swap_chain_resized, resize_count);
    }

    void record_event(
        frame_warp_lab::RuntimeEventType event_type,
        std::int32_t value,
        frame_warp_lab::ConfigKey config_key = frame_warp_lab::ConfigKey::none) {
        const std::int32_t event_sequence = std::atomic_ref<std::int32_t>(state_->event_count).load(std::memory_order_relaxed) + 1;
        auto& event = state_->events[(static_cast<std::size_t>(event_sequence) - 1) % frame_warp_lab::runtime_event_capacity];
        FILETIME timestamp{};
        GetSystemTimeAsFileTime(&timestamp);
        frame_warp_lab::seqlock_write(event, [&](frame_warp_lab::RuntimeEvent& target) {
            target.sequence = event_sequence;
            target.timestamp_low = timestamp.dwLowDateTime;
            target.timestamp_high = timestamp.dwHighDateTime;
            target.event_type = static_cast<std::int32_t>(event_type);
            target.config_key = static_cast<std::int32_t>(config_key);
            target.value = value;
        });
        std::atomic_ref<std::int32_t>(state_->event_count).store(event_sequence, std::memory_order_release);
    }

private:
    Handle mapping_;
    frame_warp_lab::RuntimeState* state_ = nullptr;
};

struct WindowInputState {
    bool left_drag_active = false;
    bool mouse_look_locked = false;
    bool cursor_hidden = false;
    bool raw_input_registered = false;
    bool raw_input_received_this_frame = false;
    bool flash_trigger_pending = false;
    bool has_input_timestamp = false;
    std::chrono::steady_clock::time_point earliest_input_time{};
    int last_cursor_x = 0;
    int last_cursor_y = 0;
    float pending_mouse_delta_x = 0.0f;
    float pending_mouse_delta_y = 0.0f;
    bool range_mode = true;
    bool right_mouse_down = false;
    bool fire_trigger_pending = false;
    std::vector<std::pair<WPARAM, LPARAM>> queued_control_messages;
};

void stamp_input_arrival(WindowInputState& input) {
    if (!input.has_input_timestamp) {
        input.earliest_input_time = std::chrono::steady_clock::now();
        input.has_input_timestamp = true;
    }
}

void set_mouse_look_locked(HWND window, WindowInputState& input, bool locked) {
    if (input.mouse_look_locked == locked) return;
    input.mouse_look_locked = locked;
    if (locked) {
        input.left_drag_active = false;
        SetCapture(window);
        RECT client{};
        if (GetClientRect(window, &client)) {
            POINT top_left{client.left, client.top};
            POINT bottom_right{client.right, client.bottom};
            ClientToScreen(window, &top_left);
            ClientToScreen(window, &bottom_right);
            RECT screen_rect{top_left.x, top_left.y, bottom_right.x, bottom_right.y};
            ClipCursor(&screen_rect);
            POINT center{(client.right - client.left) / 2, (client.bottom - client.top) / 2};
            input.last_cursor_x = center.x;
            input.last_cursor_y = center.y;
            ClientToScreen(window, &center);
            SetCursorPos(center.x, center.y);
        }
        if (!input.cursor_hidden) {
            ShowCursor(FALSE);
            input.cursor_hidden = true;
        }
    } else {
        ClipCursor(nullptr);
        if (GetCapture() == window) ReleaseCapture();
        if (input.cursor_hidden) {
            ShowCursor(TRUE);
            input.cursor_hidden = false;
        }
    }
}

frame_warp_lab::ManualCameraInput consume_manual_camera_input(
    HWND window,
    WindowInputState& input_state,
    bool& out_has_input_timestamp,
    std::chrono::steady_clock::time_point& out_earliest_input_time,
    bool& out_flash_triggered) {
    frame_warp_lab::ManualCameraInput input{};
    input.mouse_delta_x_px = input_state.pending_mouse_delta_x;
    input.mouse_delta_y_px = input_state.pending_mouse_delta_y;
    input_state.pending_mouse_delta_x = 0.0f;
    input_state.pending_mouse_delta_y = 0.0f;
    input_state.raw_input_received_this_frame = false;
    out_has_input_timestamp = input_state.has_input_timestamp;
    out_earliest_input_time = input_state.earliest_input_time;
    input_state.has_input_timestamp = false;
    out_flash_triggered = input_state.flash_trigger_pending;
    input_state.flash_trigger_pending = false;

    if (GetForegroundWindow() != window) return input;
    auto key_down = [](int virtual_key) -> bool { return (GetAsyncKeyState(virtual_key) & 0x8000) != 0; };
    if (key_down('W')) input.forward_axis += 1.0f;
    if (key_down('S')) input.forward_axis -= 1.0f;
    if (key_down('D')) input.right_axis += 1.0f;
    if (key_down('A')) input.right_axis -= 1.0f;
    if (key_down('E')) input.vertical_axis += 1.0f;
    if (key_down('Q')) input.vertical_axis -= 1.0f;
    if (key_down(VK_RIGHT)) input.yaw_axis += 1.0f;
    if (key_down(VK_LEFT)) input.yaw_axis -= 1.0f;
    if (key_down(VK_UP)) input.pitch_axis += 1.0f;
    if (key_down(VK_DOWN)) input.pitch_axis -= 1.0f;
    return input;
}

const char* short_mode_label(frame_warp_lab::PresentationMode mode) {
    switch (mode) {
        case frame_warp_lab::PresentationMode::full_render: return "FULL";
        case frame_warp_lab::PresentationMode::repeat: return "REPEAT";
        case frame_warp_lab::PresentationMode::reproject: return "REPROJ";
        case frame_warp_lab::PresentationMode::frame_gen: return "GEN";
        case frame_warp_lab::PresentationMode::combined: return "COMB";
        case frame_warp_lab::PresentationMode::dlss_g: return "DLSSG";
        case frame_warp_lab::PresentationMode::dlss_g_warp: return "SW+DG";
        default: return "UNK";
    }
}

const char* short_pacer_label(frame_warp_lab::PacerMode mode) {
    switch (mode) {
        case frame_warp_lab::PacerMode::deadline: return "LOCK";
        case frame_warp_lab::PacerMode::vsync_only: return "VSYNC";
        case frame_warp_lab::PacerMode::unlocked: return "FREE";
        default: return "UNK";
    }
}

const char* short_path_label(frame_warp_lab::CameraPath path) {
    switch (path) {
        case frame_warp_lab::CameraPath::corridor_sway: return "SWAY";
        case frame_warp_lab::CameraPath::fast_pan: return "FASTPAN";
        case frame_warp_lab::CameraPath::occlusion_sweep: return "OCCL";
        case frame_warp_lab::CameraPath::camera_cut: return "CUT";
        case frame_warp_lab::CameraPath::manual: return "MANUAL";
        case frame_warp_lab::CameraPath::pure_rotation: return "ROTATE";
        default: return "UNK";
    }
}

const char* short_vrs_label(frame_warp_lab::VrsMode mode) {
    switch (mode) {
        case frame_warp_lab::VrsMode::off: return "OFF";
        case frame_warp_lab::VrsMode::uniform_2x2: return "2X2";
        case frame_warp_lab::VrsMode::foveated: return "FOV";
        default: return "UNK";
    }
}

void format_hud_lines(
    frame_warp_lab::HudOverlayState& hud,
    double smoothed_interval_us,
    std::int64_t last_scene_gpu_us,
    std::int64_t last_present_gpu_us,
    std::int64_t last_input_to_present_us,
    std::int64_t missed_deadlines,
    std::uint32_t last_invalid_pixels,
    std::uint32_t last_total_pixels,
    frame_warp_lab::PresentationMode mode,
    frame_warp_lab::PacerMode pacer_mode,
    std::int32_t source_divider,
    bool adaptive_enabled,
    std::int32_t adaptive_tier,
    frame_warp_lab::VrsMode active_vrs_mode,
    bool selective_tiles_enabled,
    const frame_warp_lab::TileCacheStats& tile_stats,
    frame_warp_lab::CameraPath path,
    std::int32_t stress_level,
    bool mouse_look_locked) {
    const double frame_ms = smoothed_interval_us > 1.0 ? smoothed_interval_us / 1000.0 : 0.0;
    const double fps = frame_ms > 0.01 ? 1000.0 / frame_ms : 0.0;
    const double scene_ms = last_scene_gpu_us >= 0 ? static_cast<double>(last_scene_gpu_us) / 1000.0 : 0.0;
    const double warp_ms = last_present_gpu_us >= 0 ? static_cast<double>(last_present_gpu_us) / 1000.0 : 0.0;
    const double input_ms = last_input_to_present_us >= 0 ? static_cast<double>(last_input_to_present_us) / 1000.0 : 0.0;
    const double holes_pct = last_total_pixels > 0
        ? (100.0 * static_cast<double>(last_invalid_pixels) / static_cast<double>(last_total_pixels))
        : 0.0;

    std::snprintf(hud.lines[0], sizeof(hud.lines[0]), "FPS %5.1f  FT %5.2f MS  %s /%d  PACE %s", fps, frame_ms, short_mode_label(mode), source_divider, short_pacer_label(pacer_mode));
    std::snprintf(hud.lines[1], sizeof(hud.lines[1]), "GPU SCN %4.2f  WRP %4.2f  IN-PR %4.2f  MS", scene_ms, warp_ms, input_ms);
    if (selective_tiles_enabled && tile_stats.total_tiles > 0U) {
        const double shaded_pct = 100.0 * static_cast<double>(tile_stats.shaded_tiles) / static_cast<double>(tile_stats.total_tiles);
        std::snprintf(hud.lines[2], sizeof(hud.lines[2]), "TILE %4.1f%%  DEF %u  HOLE %4.2f%%  STR %d", shaded_pct, tile_stats.deferred_tiles, holes_pct, stress_level);
    } else if (adaptive_enabled) {
        std::snprintf(hud.lines[2], sizeof(hud.lines[2]), "HOLE %4.2f%%  MISS %lld  VRS %s  ADP T%d", holes_pct, static_cast<long long>(missed_deadlines), short_vrs_label(active_vrs_mode), adaptive_tier);
    } else {
        std::snprintf(hud.lines[2], sizeof(hud.lines[2]), "HOLE %4.2f%%  MISS %lld  VRS %s  STR %d", holes_pct, static_cast<long long>(missed_deadlines), short_vrs_label(active_vrs_mode), stress_level);
    }
    std::snprintf(hud.lines[3], sizeof(hud.lines[3]), "%s  PATH %s  1-7 MODE  U TILE  T MAP", mouse_look_locked ? "RMB/ESC UNLK" : "LMB/RMB LOOK", short_path_label(path));
}

LRESULT CALLBACK window_procedure(HWND window, UINT message, WPARAM message_parameter, LPARAM message_data) {
    auto* input = reinterpret_cast<WindowInputState*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_DESTROY) {
        if (input) set_mouse_look_locked(window, *input, false);
        PostQuitMessage(0);
        return 0;
    }
    if (!input) {
        return DefWindowProcW(window, message, message_parameter, message_data);
    }
    if (message == frame_warp_lab::control_message) {
        input->queued_control_messages.emplace_back(message_parameter, message_data);
        return 0;
    }
    if (message == WM_INPUT) {
        UINT data_size = 0;
        if (GetRawInputData(reinterpret_cast<HRAWINPUT>(message_data), RID_INPUT, nullptr, &data_size, sizeof(RAWINPUTHEADER)) == 0
            && data_size > 0 && data_size <= 256) {
            alignas(RAWINPUT) std::uint8_t buffer[256]{};
            if (GetRawInputData(reinterpret_cast<HRAWINPUT>(message_data), RID_INPUT, buffer, &data_size, sizeof(RAWINPUTHEADER)) == data_size) {
                const auto* raw = reinterpret_cast<const RAWINPUT*>(buffer);
                if (raw->header.dwType == RIM_TYPEMOUSE) {
                    if ((raw->data.mouse.usButtonFlags & RI_MOUSE_LEFT_BUTTON_DOWN) != 0) {
                        input->flash_trigger_pending = true;
                        stamp_input_arrival(*input);
                    }
                    if ((input->mouse_look_locked || input->left_drag_active)
                        && (raw->data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE) == 0
                        && (raw->data.mouse.lLastX != 0 || raw->data.mouse.lLastY != 0)) {
                        input->pending_mouse_delta_x += static_cast<float>(raw->data.mouse.lLastX);
                        input->pending_mouse_delta_y += static_cast<float>(raw->data.mouse.lLastY);
                        input->raw_input_received_this_frame = true;
                        stamp_input_arrival(*input);
                    }
                }
            }
        }
        return 0;
    }
    if (message == WM_LBUTTONDOWN) {
        input->flash_trigger_pending = true;
        stamp_input_arrival(*input);
        if (input->range_mode) {
            input->fire_trigger_pending = true;
            if (!input->mouse_look_locked) {
                set_mouse_look_locked(window, *input, true);
            }
        } else if (!input->mouse_look_locked) {
            input->left_drag_active = true;
            input->last_cursor_x = GET_X_LPARAM(message_data);
            input->last_cursor_y = GET_Y_LPARAM(message_data);
            SetCapture(window);
        }
        return 0;
    }
    if (message == WM_LBUTTONUP) {
        if (input->left_drag_active) {
            input->left_drag_active = false;
            if (GetCapture() == window) ReleaseCapture();
        }
        return 0;
    }
    if (message == WM_RBUTTONDOWN) {
        if (input->range_mode) {
            input->right_mouse_down = true;
        } else {
            set_mouse_look_locked(window, *input, !input->mouse_look_locked);
        }
        return 0;
    }
    if (message == WM_RBUTTONUP) {
        input->right_mouse_down = false;
        return 0;
    }
    if (message == WM_MOUSEMOVE) {
        const int x = GET_X_LPARAM(message_data);
        const int y = GET_Y_LPARAM(message_data);
        if (!input->raw_input_received_this_frame && input->left_drag_active) {
            const int dx = x - input->last_cursor_x;
            const int dy = y - input->last_cursor_y;
            if (dx != 0 || dy != 0) {
                input->pending_mouse_delta_x += static_cast<float>(dx);
                input->pending_mouse_delta_y += static_cast<float>(dy);
                stamp_input_arrival(*input);
            }
        }
        input->last_cursor_x = x;
        input->last_cursor_y = y;
        return 0;
    }
    if (message == WM_KILLFOCUS || (message == WM_CAPTURECHANGED && reinterpret_cast<HWND>(message_data) != window)) {
        input->left_drag_active = false;
        input->right_mouse_down = false;
        set_mouse_look_locked(window, *input, false);
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

        WindowInputState input_state{};
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&input_state));

        RAWINPUTDEVICE raw_mouse{};
        raw_mouse.usUsagePage = 0x01;
        raw_mouse.usUsage = 0x02;
        raw_mouse.dwFlags = 0;
        raw_mouse.hwndTarget = window;
        input_state.raw_input_registered = RegisterRawInputDevices(&raw_mouse, 1, sizeof(raw_mouse)) != FALSE;

        frame_warp_lab::LatencyMonitor latency_monitor;
        latency_monitor.start_ping_loop(window);

        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
        frame_warp_lab::Renderer renderer(window);
        frame_warp_lab::FramePacer pacer;
        pacer.refresh_display_timing(window);

        RuntimeStatePublisher runtime_state(
            renderer.gpu_timing_available(),
            renderer.allow_tearing_enabled(),
            renderer.waitable_swap_chain_enabled(),
            input_state.raw_input_registered,
            renderer.high_priority_present_queue(),
            renderer.vrs_tier(),
            renderer.vrs_tile_size_px(),
            renderer.vrs_additional_rates_supported(),
            renderer.dxr_tier(),
            renderer.dust2_map().loaded,
            renderer.dust2_pbr_build_time_ms(),
            renderer.streamline_capabilities());
        frame_warp_lab::SceneSimulation simulation;

        ShowWindow(window, show == SW_HIDE ? SW_SHOWNORMAL : show);
        SetForegroundWindow(window);
        SetActiveWindow(window);
        RECT previous_client_rectangle{};
        GetClientRect(window, &previous_client_rectangle);

        bool running = true;
        bool scene_paused = false;
        frame_warp_lab::PresentationMode presentation_mode = frame_warp_lab::PresentationMode::full_render;
        std::int32_t source_divider = 1;
        std::int32_t dlssg_multiplier = 2;
        bool adaptive_dlssg_enabled = renderer.streamline_capabilities().dlssg_supported;
        std::int32_t dlss_warp_guard_frames = 0;
        std::int64_t dlss_warp_guard_trips = 0;
        bool vsync_enabled = !renderer.allow_tearing_enabled();
        bool async_gpu_enabled = true;
        bool selective_tiles_enabled = false;
        std::int32_t tile_budget_pct = 100;
        frame_warp_lab::TilePolicy tile_policy = frame_warp_lab::TilePolicy::priority_stbn;
        bool auto_tile_budget_enabled = false;
        bool cache_sharpen_enabled = false;
        bool coarse_fallback_enabled = false;
        bool subtile_interleave_enabled = false;
        bool reactive_invalidation_enabled = true;
        bool shadow_delta_invalidation_enabled = true;
        bool forward_transparency_enabled = true;
        bool split_diff_spec_enabled = true;
        bool surface_cache_enabled = true;
        bool clustered_lighting_enabled = false;
        bool thin_gbuf_enabled = false;
        std::int32_t surface_cache_mb = 4;
        std::int32_t render_scale_pct = 100;
        UINT render_override_width = 0;
        UINT render_override_height = 0;
        frame_warp_lab::UpscaleFilter upscale_filter = frame_warp_lab::UpscaleFilter::bilinear;
        frame_warp_lab::DrsMode drs_mode = frame_warp_lab::DrsMode::off;
        std::int32_t drs_min_pct = 50;
        std::int32_t drs_max_pct = 100;
        std::int32_t fixed_dt_us = 0;
        std::int32_t pending_step_frames = 0;
        std::uint32_t pending_capture_tag = 0;
        frame_warp_lab::AutoTileBudgetController auto_tile_budget;
        frame_warp_lab::AutoDrsController auto_drs;
        frame_warp_lab::UnifiedQualityController unified_controller;
        wchar_t exe_path_buf[32768]{};
        GetModuleFileNameW(nullptr, exe_path_buf, static_cast<DWORD>(std::size(exe_path_buf)));
        const std::filesystem::path capture_dir = std::filesystem::path(exe_path_buf).parent_path() / L"captures";
        frame_warp_lab::VrsMode vrs_mode = frame_warp_lab::VrsMode::off;
        frame_warp_lab::InstrumentationLevel instrumentation_level = frame_warp_lab::InstrumentationLevel::diagnostic;
        frame_warp_lab::AdaptiveScheduler adaptive_scheduler;
        frame_warp_lab::DiagnosticView diagnostic_view = frame_warp_lab::DiagnosticView::final_color;
        std::int32_t stress_level = 0;
        std::uint32_t shader_flags = 0U;
        bool pbr_enabled = true;
        bool aniso_mip_enabled = true;
        frame_warp_lab::SceneKind scene_kind = frame_warp_lab::SceneKind::range;
        bool cli_ads_active = false;
        bool has_prev_dust2_cam = false;
        frame_warp_lab::CameraBasis prev_dust2_cam{};
        std::int64_t prev_dust2_cut = 0;
        frame_warp_lab::TacticalRangeSimulation tactical_range;
        {
            const auto& meta = renderer.range_geometry_meta();
            tactical_range.set_hero_pistol_geometry_meta(
                meta.hero_pistol_gltf_loaded,
                meta.pistol_slide_offset,
                meta.pistol_hammer_offset,
                meta.pistol_trigger_offset,
                meta.pistol_magazine_offset,
                meta.pistol_muzzle_offset,
                meta.pistol_eject_offset,
                meta.pistol_ads_sight_y);
        }
        frame_warp_lab::TacticalRangeFrame tactical_range_frame{};
        simulation.reset_for_tactical_range();
        input_state.range_mode = true;
        bool overlay_enabled = true;
        bool flash_indicator_enabled = false;
        bool pending_pcl_ping = false;
        bool hud_text_dirty = true;
        auto last_hud_text_update = std::chrono::steady_clock::now() - std::chrono::milliseconds(100);

        std::int64_t display_frame_id = 0;
        std::int64_t source_frame_id = 0;
        std::int64_t cadence_counter = 0;
        std::int64_t missed_deadlines = 0;
        auto previous_tick = std::chrono::steady_clock::now();
        auto previous_present_tick = previous_tick;
        bool has_previous_present = false;
        double smoothed_present_interval_us = 0.0;
        double smoothed_cpu_work_us = 700.0;
        std::int64_t last_scene_gpu_us = -1;
        std::int64_t last_present_gpu_us = -1;
        std::int64_t last_input_to_present_us = -1;
        std::uint32_t last_invalid_pixels = 0;
        std::uint32_t last_total_pixels = 0;

        frame_warp_lab::HudOverlayState hud_state{};
        hud_state.max_graph_ms = 20.0f;
        hud_state.target_frame_ms = pacer.target_frame_ms();

        auto publish_config = [&]() {
            hud_text_dirty = true;
            latency_monitor.set_enabled(instrumentation_level != frame_warp_lab::InstrumentationLevel::minimal);
            runtime_state.update_config(
                presentation_mode,
                pacer.mode(),
                pacer.target_fps_override(),
                pacer.target_period_us(),
                source_divider,
                vsync_enabled,
                async_gpu_enabled,
                vrs_mode,
                renderer.resolve_effective_vrs_mode(vrs_mode),
                instrumentation_level,
                diagnostic_view,
                simulation.path(),
                stress_level,
                shader_flags,
                pbr_enabled,
                aniso_mip_enabled,
                scene_kind,
                overlay_enabled,
                flash_indicator_enabled);
            runtime_state.update_adaptive(adaptive_scheduler.enabled(), adaptive_scheduler.telemetry());
            runtime_state.update_unified_controller(unified_controller.telemetry());
            runtime_state.update_selective_tiles(
                selective_tiles_enabled,
                tile_budget_pct,
                tile_policy,
                auto_tile_budget_enabled,
                cache_sharpen_enabled,
                coarse_fallback_enabled,
                subtile_interleave_enabled,
                reactive_invalidation_enabled,
                shadow_delta_invalidation_enabled,
                forward_transparency_enabled,
                split_diff_spec_enabled,
                surface_cache_enabled,
                clustered_lighting_enabled,
                thin_gbuf_enabled,
                surface_cache_mb,
                auto_tile_budget_enabled && auto_tile_budget.par_bypass_active(),
                auto_tile_budget.smoothed_damage_pct(),
                render_scale_pct,
                static_cast<std::int32_t>(renderer.render_width()),
                static_cast<std::int32_t>(renderer.render_height()),
                static_cast<std::int32_t>(renderer.alloc_render_width()),
                static_cast<std::int32_t>(renderer.alloc_render_height()),
                upscale_filter,
                drs_mode,
                drs_min_pct,
                drs_max_pct,
                renderer.drs_scale_changes(),
                renderer.drs_reallocations(),
                auto_drs.last_target_scene_budget_us(),
                fixed_dt_us,
                auto_tile_budget.last_target_scene_budget_us(),
                renderer.latest_completed_source().tile_stats);
        };
        publish_config();

        auto handle_control_message = [&](WPARAM message_wparam, LPARAM message_lparam) {
            const auto command = static_cast<frame_warp_lab::ControlCommand>(message_wparam);
            if (command == frame_warp_lab::ControlCommand::scene_pause) {
                scene_paused = true;
                runtime_state.record_event(frame_warp_lab::RuntimeEventType::scene_paused, 1);
            } else if (command == frame_warp_lab::ControlCommand::scene_resume) {
                scene_paused = false;
                runtime_state.record_event(frame_warp_lab::RuntimeEventType::scene_resumed, 0);
            } else if (command == frame_warp_lab::ControlCommand::scene_reset) {
                if (scene_kind == frame_warp_lab::SceneKind::range) {
                    simulation.reset_for_tactical_range();
                    tactical_range.reset();
                } else if (scene_kind == frame_warp_lab::SceneKind::dust2) {
                    const auto& d2 = renderer.dust2_map();
                    simulation.reset_for_dust2(d2.spawn_pos, d2.spawn_yaw, d2.world_min, d2.world_max);
                    tactical_range.reset();
                } else {
                    simulation.reset();
                }
                has_prev_dust2_cam = false;
                renderer.invalidate_source_history();
                cadence_counter = 0;
                pacer.reset();
                runtime_state.record_event(frame_warp_lab::RuntimeEventType::scene_reset, 0);
            } else if (command == frame_warp_lab::ControlCommand::set_diagnostic_view) {
                const auto raw = static_cast<std::int32_t>(message_lparam);
                if (raw >= 0 && raw <= 6) {
                    diagnostic_view = static_cast<frame_warp_lab::DiagnosticView>(raw);
                    runtime_state.record_event(frame_warp_lab::RuntimeEventType::config_updated, raw, frame_warp_lab::ConfigKey::diagnostic_view);
                }
            } else if (command == frame_warp_lab::ControlCommand::set_camera_path) {
                const auto raw = static_cast<std::int32_t>(message_lparam);
                if (raw >= 0 && raw <= 5) {
                    simulation.set_path(static_cast<frame_warp_lab::CameraPath>(raw));
                    has_prev_dust2_cam = false;
                    renderer.invalidate_source_history();
                    runtime_state.record_event(frame_warp_lab::RuntimeEventType::config_updated, raw, frame_warp_lab::ConfigKey::camera_path);
                }
            } else if (command == frame_warp_lab::ControlCommand::set_stress_level) {
                stress_level = std::clamp(static_cast<std::int32_t>(message_lparam), 0, frame_warp_lab::max_stress_level);
                runtime_state.record_event(frame_warp_lab::RuntimeEventType::config_updated, stress_level, frame_warp_lab::ConfigKey::stress_level);
            } else if (command == frame_warp_lab::ControlCommand::set_overlay_enabled) {
                overlay_enabled = message_lparam != 0;
                runtime_state.record_event(frame_warp_lab::RuntimeEventType::config_updated, overlay_enabled ? 1 : 0, frame_warp_lab::ConfigKey::overlay_enabled);
            } else if (command == frame_warp_lab::ControlCommand::set_presentation_mode) {
                const auto raw = static_cast<std::int32_t>(message_lparam);
                if (raw >= 0 && raw <= 6) {
                    adaptive_scheduler.set_enabled(false);
                    presentation_mode = static_cast<frame_warp_lab::PresentationMode>(raw);
                    cadence_counter = 0;
                    runtime_state.record_event(frame_warp_lab::RuntimeEventType::config_updated, raw, frame_warp_lab::ConfigKey::presentation_mode);
                }
            } else if (command == frame_warp_lab::ControlCommand::set_dlssg_multiplier) {
                const auto raw = static_cast<std::int32_t>(message_lparam);
                const auto max_mult = std::max<std::int32_t>(2, static_cast<std::int32_t>(renderer.streamline_capabilities().max_frames_to_generate) + 1);
                if (raw == 0) {
                    dlssg_multiplier = 0;
                } else {
                    dlssg_multiplier = std::clamp(raw, 2, max_mult);
                }
                runtime_state.record_event(frame_warp_lab::RuntimeEventType::config_updated, dlssg_multiplier, frame_warp_lab::ConfigKey::dlssg_multiplier);
            } else if (command == frame_warp_lab::ControlCommand::set_source_divider) {
                adaptive_scheduler.set_enabled(false);
                source_divider = std::clamp(static_cast<std::int32_t>(message_lparam), 1, frame_warp_lab::max_source_divider);
                cadence_counter = 0;
                runtime_state.record_event(frame_warp_lab::RuntimeEventType::config_updated, source_divider, frame_warp_lab::ConfigKey::source_divider);
            } else if (command == frame_warp_lab::ControlCommand::set_vsync_enabled) {
                vsync_enabled = message_lparam != 0;
                pacer.reset();
                runtime_state.record_event(frame_warp_lab::RuntimeEventType::config_updated, vsync_enabled ? 1 : 0, frame_warp_lab::ConfigKey::vsync_enabled);
            } else if (command == frame_warp_lab::ControlCommand::set_flash_indicator) {
                flash_indicator_enabled = message_lparam != 0;
                runtime_state.record_event(frame_warp_lab::RuntimeEventType::config_updated, flash_indicator_enabled ? 1 : 0, frame_warp_lab::ConfigKey::flash_indicator);
            } else if (command == frame_warp_lab::ControlCommand::set_pacer_mode) {
                const auto raw = static_cast<std::int32_t>(message_lparam);
                if (raw >= 0 && raw <= 2) {
                    const auto new_mode = static_cast<frame_warp_lab::PacerMode>(raw);
                    pacer.set_mode(new_mode);
                    if (new_mode == frame_warp_lab::PacerMode::vsync_only) vsync_enabled = true;
                    else if (new_mode == frame_warp_lab::PacerMode::deadline || new_mode == frame_warp_lab::PacerMode::unlocked) {
                        vsync_enabled = !renderer.allow_tearing_enabled();
                    }
                    runtime_state.record_event(frame_warp_lab::RuntimeEventType::config_updated, raw, frame_warp_lab::ConfigKey::pacer_mode);
                }
            } else if (command == frame_warp_lab::ControlCommand::set_target_fps) {
                const auto raw = std::clamp(static_cast<std::int32_t>(message_lparam), 0, frame_warp_lab::max_target_fps);
                pacer.set_target_fps(raw, window);
                hud_state.target_frame_ms = pacer.target_frame_ms();
                runtime_state.record_event(frame_warp_lab::RuntimeEventType::config_updated, raw, frame_warp_lab::ConfigKey::target_fps);
            } else if (command == frame_warp_lab::ControlCommand::set_async_gpu) {
                async_gpu_enabled = message_lparam != 0;
                runtime_state.record_event(frame_warp_lab::RuntimeEventType::config_updated, async_gpu_enabled ? 1 : 0, frame_warp_lab::ConfigKey::async_gpu);
            } else if (command == frame_warp_lab::ControlCommand::set_vrs_mode) {
                const auto raw = static_cast<std::int32_t>(message_lparam);
                if (raw >= 0 && raw <= 2) {
                    adaptive_scheduler.set_enabled(false);
                    vrs_mode = static_cast<frame_warp_lab::VrsMode>(raw);
                    runtime_state.record_event(frame_warp_lab::RuntimeEventType::config_updated, raw, frame_warp_lab::ConfigKey::vrs_mode);
                }
            } else if (command == frame_warp_lab::ControlCommand::set_adaptive_scheduler) {
                const bool enable_adaptive = message_lparam != 0;
                adaptive_scheduler.set_enabled(enable_adaptive);
                if (enable_adaptive) {
                    adaptive_scheduler.reset();
                    const bool use_dlssg_policy = adaptive_dlssg_enabled && renderer.streamline_capabilities().dlssg_supported;
                    const auto initial_policy = frame_warp_lab::AdaptiveScheduler::policy_for_tier(0, renderer.vrs_tier(), use_dlssg_policy);
                    presentation_mode = initial_policy.mode;
                    source_divider = initial_policy.divider;
                    vrs_mode = initial_policy.vrs;
                    async_gpu_enabled = initial_policy.async_gpu;
                    dlssg_multiplier = initial_policy.dlssg_multiplier;
                    cadence_counter = 0;
                }
                runtime_state.record_event(frame_warp_lab::RuntimeEventType::config_updated, enable_adaptive ? 1 : 0, frame_warp_lab::ConfigKey::adaptive_scheduler);
            } else if (command == frame_warp_lab::ControlCommand::set_adaptive_dlssg) {
                adaptive_dlssg_enabled = message_lparam != 0;
                runtime_state.record_event(frame_warp_lab::RuntimeEventType::config_updated, adaptive_dlssg_enabled ? 1 : 0, frame_warp_lab::ConfigKey::adaptive_dlssg);
            } else if (command == frame_warp_lab::ControlCommand::set_instrumentation_level) {
                const auto raw = static_cast<std::int32_t>(message_lparam);
                if (raw >= 0 && raw <= 2) {
                    instrumentation_level = static_cast<frame_warp_lab::InstrumentationLevel>(raw);
                    runtime_state.record_event(frame_warp_lab::RuntimeEventType::config_updated, raw, frame_warp_lab::ConfigKey::instrumentation_level);
                }
            } else if (command == frame_warp_lab::ControlCommand::set_selective_tiles) {
                selective_tiles_enabled = message_lparam != 0;
                runtime_state.record_event(frame_warp_lab::RuntimeEventType::config_updated, selective_tiles_enabled ? 1 : 0, frame_warp_lab::ConfigKey::selective_tiles);
            } else if (command == frame_warp_lab::ControlCommand::set_tile_budget_pct) {
                auto_tile_budget_enabled = false;
                tile_budget_pct = std::clamp(static_cast<std::int32_t>(message_lparam), 5, 100);
                runtime_state.record_event(frame_warp_lab::RuntimeEventType::config_updated, tile_budget_pct, frame_warp_lab::ConfigKey::tile_budget_pct);
            } else if (command == frame_warp_lab::ControlCommand::set_tile_policy) {
                const auto raw = static_cast<std::int32_t>(message_lparam);
                if (raw >= 0 && raw <= 1) {
                    tile_policy = static_cast<frame_warp_lab::TilePolicy>(raw);
                    runtime_state.record_event(frame_warp_lab::RuntimeEventType::config_updated, raw, frame_warp_lab::ConfigKey::tile_policy);
                }
            } else if (command == frame_warp_lab::ControlCommand::set_auto_tile_budget) {
                auto_tile_budget_enabled = message_lparam != 0;
                auto_tile_budget.reset();
                runtime_state.record_event(frame_warp_lab::RuntimeEventType::config_updated, auto_tile_budget_enabled ? 1 : 0, frame_warp_lab::ConfigKey::auto_tile_budget);
            } else if (command == frame_warp_lab::ControlCommand::set_cache_sharpen) {
                cache_sharpen_enabled = message_lparam != 0;
                runtime_state.record_event(frame_warp_lab::RuntimeEventType::config_updated, cache_sharpen_enabled ? 1 : 0, frame_warp_lab::ConfigKey::cache_sharpen);
            } else if (command == frame_warp_lab::ControlCommand::set_coarse_fallback) {
                coarse_fallback_enabled = message_lparam != 0;
                runtime_state.record_event(frame_warp_lab::RuntimeEventType::config_updated, coarse_fallback_enabled ? 1 : 0, frame_warp_lab::ConfigKey::coarse_fallback);
            } else if (command == frame_warp_lab::ControlCommand::set_subtile_interleave) {
                subtile_interleave_enabled = message_lparam != 0;
                runtime_state.record_event(frame_warp_lab::RuntimeEventType::config_updated, subtile_interleave_enabled ? 1 : 0, frame_warp_lab::ConfigKey::subtile_interleave);
            } else if (command == frame_warp_lab::ControlCommand::set_reactive_invalidation) {
                reactive_invalidation_enabled = message_lparam != 0;
                renderer.invalidate_source_history();
                runtime_state.record_event(frame_warp_lab::RuntimeEventType::config_updated, reactive_invalidation_enabled ? 1 : 0, frame_warp_lab::ConfigKey::reactive_invalidation);
            } else if (command == frame_warp_lab::ControlCommand::set_shadow_delta_invalidation) {
                shadow_delta_invalidation_enabled = message_lparam != 0;
                renderer.invalidate_source_history();
                runtime_state.record_event(frame_warp_lab::RuntimeEventType::config_updated, shadow_delta_invalidation_enabled ? 1 : 0, frame_warp_lab::ConfigKey::shadow_delta_invalidation);
            } else if (command == frame_warp_lab::ControlCommand::set_forward_transparency) {
                forward_transparency_enabled = message_lparam != 0;
                renderer.invalidate_source_history();
                runtime_state.record_event(frame_warp_lab::RuntimeEventType::config_updated, forward_transparency_enabled ? 1 : 0, frame_warp_lab::ConfigKey::forward_transparency);
            } else if (command == frame_warp_lab::ControlCommand::set_split_diff_spec) {
                split_diff_spec_enabled = message_lparam != 0;
                renderer.invalidate_source_history();
                runtime_state.record_event(frame_warp_lab::RuntimeEventType::config_updated, split_diff_spec_enabled ? 1 : 0, frame_warp_lab::ConfigKey::split_diff_spec);
            } else if (command == frame_warp_lab::ControlCommand::set_surface_cache) {
                surface_cache_enabled = message_lparam != 0;
                renderer.invalidate_source_history();
                runtime_state.record_event(frame_warp_lab::RuntimeEventType::config_updated, surface_cache_enabled ? 1 : 0, frame_warp_lab::ConfigKey::surface_cache);
            } else if (command == frame_warp_lab::ControlCommand::set_unified_controller) {
                const bool en = message_lparam != 0;
                unified_controller.set_enabled(en);
                if (en) {
                    adaptive_scheduler.set_enabled(false);
                    auto_tile_budget_enabled = false;
                    drs_mode = frame_warp_lab::DrsMode::off;
                    auto_tile_budget.reset();
                    auto_drs.reset();
                    const auto p0 = frame_warp_lab::UnifiedQualityController::profile_for_tier(unified_controller.tier());
                    selective_tiles_enabled = p0.selective_tiles;
                    split_diff_spec_enabled = p0.split_diff_spec;
                    surface_cache_enabled = p0.surface_cache;
                    subtile_interleave_enabled = p0.subtile_interleave;
                    cache_sharpen_enabled = p0.cache_sharpen;
                    tile_budget_pct = p0.tile_budget_pct;
                    render_scale_pct = p0.render_scale_pct;
                    vrs_mode = p0.vrs;
                    presentation_mode = p0.mode;
                    source_divider = p0.divider;
                }
                runtime_state.record_event(frame_warp_lab::RuntimeEventType::config_updated, en ? 1 : 0, frame_warp_lab::ConfigKey::unified_controller);
            } else if (command == frame_warp_lab::ControlCommand::set_clustered_lighting) {
                clustered_lighting_enabled = message_lparam != 0;
                runtime_state.record_event(frame_warp_lab::RuntimeEventType::config_updated, clustered_lighting_enabled ? 1 : 0, frame_warp_lab::ConfigKey::clustered_lighting);
            } else if (command == frame_warp_lab::ControlCommand::set_thin_gbuf) {
                thin_gbuf_enabled = message_lparam != 0;
                runtime_state.record_event(frame_warp_lab::RuntimeEventType::config_updated, thin_gbuf_enabled ? 1 : 0, frame_warp_lab::ConfigKey::thin_gbuf);
            } else if (command == frame_warp_lab::ControlCommand::set_surface_cache_mb) {
                const auto mb = static_cast<std::int32_t>(message_lparam);
                if (mb == 4 || mb == 16 || mb == 64) {
                    surface_cache_mb = mb;
                    runtime_state.record_event(frame_warp_lab::RuntimeEventType::config_updated, surface_cache_mb, frame_warp_lab::ConfigKey::surface_cache_mb);
                }
            } else if (command == frame_warp_lab::ControlCommand::set_shader_flags) {
                shader_flags = static_cast<std::uint32_t>(message_lparam) & 0xFFFFU;
                renderer.invalidate_source_history();
                runtime_state.record_event(frame_warp_lab::RuntimeEventType::config_updated, static_cast<std::int32_t>(shader_flags), frame_warp_lab::ConfigKey::shader_flags);
            } else if (command == frame_warp_lab::ControlCommand::set_pbr_enabled) {
                pbr_enabled = message_lparam != 0;
                renderer.invalidate_source_history();
                runtime_state.record_event(frame_warp_lab::RuntimeEventType::config_updated, pbr_enabled ? 1 : 0, frame_warp_lab::ConfigKey::pbr_enabled);
            } else if (command == frame_warp_lab::ControlCommand::set_aniso_mip_enabled) {
                aniso_mip_enabled = message_lparam != 0;
                renderer.invalidate_source_history();
                runtime_state.record_event(frame_warp_lab::RuntimeEventType::config_updated, aniso_mip_enabled ? 1 : 0, frame_warp_lab::ConfigKey::aniso_mip_enabled);
            } else if (command == frame_warp_lab::ControlCommand::set_scene_kind) {
                const auto raw = static_cast<std::int32_t>(message_lparam);
                if (raw >= 0 && raw <= 2) {
                    scene_kind = static_cast<frame_warp_lab::SceneKind>(raw);
                    input_state.range_mode = (scene_kind == frame_warp_lab::SceneKind::range || scene_kind == frame_warp_lab::SceneKind::dust2);
                    if (scene_kind == frame_warp_lab::SceneKind::range) {
                        simulation.reset_for_tactical_range();
                        tactical_range.reset();
                    } else if (scene_kind == frame_warp_lab::SceneKind::dust2) {
                        const auto& d2 = renderer.dust2_map();
                        simulation.reset_for_dust2(d2.spawn_pos, d2.spawn_yaw, d2.world_min, d2.world_max);
                        tactical_range.reset();
                    } else {
                        simulation.set_range_camera_modifiers(0.0f, 0.0f, 0.0f, false);
                        simulation.set_path(frame_warp_lab::CameraPath::corridor_sway);
                        simulation.reset();
                    }
                    has_prev_dust2_cam = false;
                    renderer.invalidate_source_history();
                    cadence_counter = 0;
                    runtime_state.record_event(frame_warp_lab::RuntimeEventType::config_updated, raw, frame_warp_lab::ConfigKey::scene_kind);
                }
            } else if (command == frame_warp_lab::ControlCommand::range_fire) {
                tactical_range.request_fire();
            } else if (command == frame_warp_lab::ControlCommand::set_range_ads) {
                cli_ads_active = message_lparam != 0;
                tactical_range.set_ads_requested(cli_ads_active || input_state.right_mouse_down);
            } else if (command == frame_warp_lab::ControlCommand::range_reload) {
                tactical_range.request_reload();
            } else if (command == frame_warp_lab::ControlCommand::reload_lua_scripts) {
                tactical_range.reload_lua_scripts();
            } else if (command == frame_warp_lab::ControlCommand::emit_range_event) {
                const auto event_code = static_cast<std::int32_t>(message_lparam);
                if (event_code == 1) tactical_range.emit_named_event("weapon.fire_requested");
                else if (event_code == 2) tactical_range.emit_named_event("weapon.reload_requested");
                else if (event_code == 3) tactical_range.emit_named_event("target.force_reset");
                else if (event_code == 4) tactical_range.emit_named_event("range.reset");
            } else if (command == frame_warp_lab::ControlCommand::set_render_scale) {
                drs_mode = frame_warp_lab::DrsMode::off;
                render_scale_pct = std::clamp(static_cast<std::int32_t>(message_lparam), 25, 200);
                render_override_width = 0;
                render_override_height = 0;
                renderer.set_render_resolution(0, 0, render_scale_pct);
                runtime_state.record_event(frame_warp_lab::RuntimeEventType::config_updated, render_scale_pct, frame_warp_lab::ConfigKey::render_scale);
            } else if (command == frame_warp_lab::ControlCommand::set_render_size) {
                const auto packed = static_cast<std::uint32_t>(message_lparam);
                const UINT req_w = static_cast<UINT>(packed & 0xFFFFU);
                const UINT req_h = static_cast<UINT>((packed >> 16U) & 0xFFFFU);
                if (req_w == 0U || req_h == 0U) {
                    render_override_width = 0;
                    render_override_height = 0;
                } else {
                    render_override_width = std::clamp<UINT>(req_w, 160U, 7680U);
                    render_override_height = std::clamp<UINT>(req_h, 120U, 4320U);
                }
                renderer.set_render_resolution(render_override_width, render_override_height, render_scale_pct);
                runtime_state.record_event(frame_warp_lab::RuntimeEventType::config_updated, static_cast<std::int32_t>(renderer.render_width()), frame_warp_lab::ConfigKey::render_size);
            } else if (command == frame_warp_lab::ControlCommand::set_upscale_filter) {
                const auto raw = static_cast<std::int32_t>(message_lparam);
                if (raw >= 0 && raw <= 3) {
                    upscale_filter = static_cast<frame_warp_lab::UpscaleFilter>(raw);
                    runtime_state.record_event(frame_warp_lab::RuntimeEventType::config_updated, raw, frame_warp_lab::ConfigKey::upscale_filter);
                }
            } else if (command == frame_warp_lab::ControlCommand::set_drs_mode) {
                const auto raw = static_cast<std::int32_t>(message_lparam);
                if (raw >= 0 && raw <= 2) {
                    drs_mode = static_cast<frame_warp_lab::DrsMode>(raw);
                    auto_drs.reset();
                    runtime_state.record_event(frame_warp_lab::RuntimeEventType::config_updated, raw, frame_warp_lab::ConfigKey::drs_mode);
                }
            } else if (command == frame_warp_lab::ControlCommand::set_drs_min_pct) {
                drs_min_pct = std::clamp(static_cast<std::int32_t>(message_lparam), 25, 200);
                if (drs_max_pct < drs_min_pct) drs_max_pct = drs_min_pct;
                runtime_state.record_event(frame_warp_lab::RuntimeEventType::config_updated, drs_min_pct, frame_warp_lab::ConfigKey::drs_min_pct);
            } else if (command == frame_warp_lab::ControlCommand::set_drs_max_pct) {
                drs_max_pct = std::clamp(static_cast<std::int32_t>(message_lparam), 25, 200);
                if (drs_min_pct > drs_max_pct) drs_min_pct = drs_max_pct;
                runtime_state.record_event(frame_warp_lab::RuntimeEventType::config_updated, drs_max_pct, frame_warp_lab::ConfigKey::drs_max_pct);
            } else if (command == frame_warp_lab::ControlCommand::resize_window) {
                const auto packed = static_cast<std::uint32_t>(message_lparam);
                const LONG requested_width = static_cast<LONG>(packed & 0xFFFFU);
                const LONG requested_height = static_cast<LONG>((packed >> 16U) & 0xFFFFU);
                if (requested_width >= 320 && requested_width <= 7680 && requested_height >= 240 && requested_height <= 4320) {
                    RECT desired_rect{0, 0, requested_width, requested_height};
                    const auto style = static_cast<DWORD>(GetWindowLongPtrW(window, GWL_STYLE));
                    const auto ex_style = static_cast<DWORD>(GetWindowLongPtrW(window, GWL_EXSTYLE));
                    AdjustWindowRectEx(&desired_rect, style, FALSE, ex_style);
                    SetWindowPos(
                        window,
                        nullptr,
                        0,
                        0,
                        desired_rect.right - desired_rect.left,
                        desired_rect.bottom - desired_rect.top,
                        SWP_NOZORDER | SWP_NOACTIVATE);
                }
            } else if (command == frame_warp_lab::ControlCommand::set_fixed_dt_us) {
                fixed_dt_us = std::clamp(static_cast<std::int32_t>(message_lparam), 0, 100000);
                runtime_state.record_event(frame_warp_lab::RuntimeEventType::config_updated, fixed_dt_us, frame_warp_lab::ConfigKey::fixed_dt_us);
            } else if (command == frame_warp_lab::ControlCommand::capture_frame) {
                pending_capture_tag = static_cast<std::uint32_t>(message_lparam);
            } else if (command == frame_warp_lab::ControlCommand::step_frames) {
                pending_step_frames = std::clamp(static_cast<std::int32_t>(message_lparam), 0, 2000);
                runtime_state.update_pending_steps(pending_step_frames);
            }
            previous_tick = std::chrono::steady_clock::now();
            publish_config();
            float yaw_deg = 0.0f;
            float pitch_deg = 0.0f;
            simulation.current_camera_angles_degrees(yaw_deg, pitch_deg);
            runtime_state.update_scene(
                simulation.time_seconds(),
                scene_paused,
                input_state.mouse_look_locked || input_state.left_drag_active,
                yaw_deg,
                pitch_deg,
                latency_monitor);
        };

        auto drain_queued_control_messages = [&]() {
            if (input_state.queued_control_messages.empty()) return;
            auto queued = std::move(input_state.queued_control_messages);
            input_state.queued_control_messages.clear();
            for (const auto& item : queued) {
                handle_control_message(item.first, item.second);
            }
        };

        auto pump_messages = [&]() {
            drain_queued_control_messages();
            MSG message{};
            while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                if (message.message == WM_QUIT) {
                    running = false;
                    break;
                }
                if (latency_monitor.is_ping_message(message.message)) {
                    pending_pcl_ping = true;
                    stamp_input_arrival(input_state);
                    continue;
                }
                if (message.message == WM_KEYDOWN && (message.lParam & (1LL << 30)) == 0) {
                    bool config_changed = false;
                    if (message.wParam == VK_ESCAPE && input_state.mouse_look_locked) {
                        set_mouse_look_locked(window, input_state, false);
                    } else if (message.wParam == 'M') {
                        set_mouse_look_locked(window, input_state, !input_state.mouse_look_locked);
                    } else if (message.wParam == '1') {
                        adaptive_scheduler.set_enabled(false);
                        presentation_mode = frame_warp_lab::PresentationMode::full_render;
                        cadence_counter = 0;
                        config_changed = true;
                    } else if (message.wParam == '2') {
                        adaptive_scheduler.set_enabled(false);
                        presentation_mode = frame_warp_lab::PresentationMode::repeat;
                        if (source_divider < 2) source_divider = 2;
                        cadence_counter = 0;
                        config_changed = true;
                    } else if (message.wParam == '3') {
                        adaptive_scheduler.set_enabled(false);
                        presentation_mode = frame_warp_lab::PresentationMode::reproject;
                        if (source_divider < 2) source_divider = 2;
                        cadence_counter = 0;
                        config_changed = true;
                    } else if (message.wParam == '4') {
                        adaptive_scheduler.set_enabled(false);
                        presentation_mode = frame_warp_lab::PresentationMode::frame_gen;
                        if (source_divider < 2) source_divider = 2;
                        cadence_counter = 0;
                        config_changed = true;
                    } else if (message.wParam == '5') {
                        adaptive_scheduler.set_enabled(false);
                        presentation_mode = frame_warp_lab::PresentationMode::combined;
                        if (source_divider < 2) source_divider = 2;
                        cadence_counter = 0;
                        config_changed = true;
                    } else if (message.wParam == '6') {
                        adaptive_scheduler.set_enabled(false);
                        presentation_mode = frame_warp_lab::PresentationMode::dlss_g;
                        cadence_counter = 0;
                        config_changed = true;
                    } else if (message.wParam == '7') {
                        adaptive_scheduler.set_enabled(false);
                        presentation_mode = frame_warp_lab::PresentationMode::dlss_g_warp;
                        cadence_counter = 0;
                        config_changed = true;
                    } else if (message.wParam == 'X') {
                        const auto max_mult = std::max<std::int32_t>(2, static_cast<std::int32_t>(renderer.streamline_capabilities().max_frames_to_generate) + 1);
                        if (dlssg_multiplier == 0) {
                            dlssg_multiplier = 2;
                        } else if (dlssg_multiplier < max_mult) {
                            ++dlssg_multiplier;
                        } else if (renderer.streamline_capabilities().dynamic_mfg_supported) {
                            dlssg_multiplier = 0;
                        } else {
                            dlssg_multiplier = 2;
                        }
                        config_changed = true;
                    } else if (message.wParam == 'L') {
                        const auto next_pacer = static_cast<frame_warp_lab::PacerMode>((static_cast<std::int32_t>(pacer.mode()) + 1) % 3);
                        pacer.set_mode(next_pacer);
                        vsync_enabled = next_pacer == frame_warp_lab::PacerMode::vsync_only;
                        config_changed = true;
                    } else if (message.wParam == 'G') {
                        async_gpu_enabled = !async_gpu_enabled;
                        config_changed = true;
                    } else if (message.wParam == 'B') {
                        adaptive_scheduler.set_enabled(false);
                        vrs_mode = static_cast<frame_warp_lab::VrsMode>((static_cast<std::int32_t>(vrs_mode) + 1) % 3);
                        config_changed = true;
                    } else if (message.wParam == 'K') {
                        const bool enable_adaptive = !adaptive_scheduler.enabled();
                        adaptive_scheduler.set_enabled(enable_adaptive);
                        if (enable_adaptive) {
                            adaptive_scheduler.reset();
                            const bool use_dlssg_policy = adaptive_dlssg_enabled && renderer.streamline_capabilities().dlssg_supported;
                            const auto initial_policy = frame_warp_lab::AdaptiveScheduler::policy_for_tier(0, renderer.vrs_tier(), use_dlssg_policy);
                            presentation_mode = initial_policy.mode;
                            source_divider = initial_policy.divider;
                            vrs_mode = initial_policy.vrs;
                            async_gpu_enabled = initial_policy.async_gpu;
                            dlssg_multiplier = initial_policy.dlssg_multiplier;
                            cadence_counter = 0;
                        }
                        config_changed = true;
                    } else if (message.wParam == 'H') {
                        diagnostic_view = diagnostic_view == frame_warp_lab::DiagnosticView::reprojection_holes
                            ? frame_warp_lab::DiagnosticView::final_color
                            : frame_warp_lab::DiagnosticView::reprojection_holes;
                        config_changed = true;
                    } else if (message.wParam == 'U') {
                        selective_tiles_enabled = !selective_tiles_enabled;
                        config_changed = true;
                    } else if (message.wParam == 'N') {
                        pbr_enabled = !pbr_enabled;
                        renderer.invalidate_source_history();
                        config_changed = true;
                    } else if (message.wParam == 'T') {
                        diagnostic_view = diagnostic_view == frame_warp_lab::DiagnosticView::tile_cache
                            ? frame_warp_lab::DiagnosticView::final_color
                            : frame_warp_lab::DiagnosticView::tile_cache;
                        config_changed = true;
                    } else if (message.wParam == 'Y') {
                        diagnostic_view = diagnostic_view == frame_warp_lab::DiagnosticView::tile_priority
                            ? frame_warp_lab::DiagnosticView::final_color
                            : frame_warp_lab::DiagnosticView::tile_priority;
                        config_changed = true;
                    } else if (message.wParam == 'O') {
                        overlay_enabled = !overlay_enabled;
                        config_changed = true;
                    } else if (message.wParam == 'F') {
                        flash_indicator_enabled = !flash_indicator_enabled;
                        config_changed = true;
                    } else if (message.wParam == 'V') {
                        vsync_enabled = !vsync_enabled;
                        pacer.reset();
                        config_changed = true;
                    } else if (message.wParam == 'P' || message.wParam == VK_SPACE) {
                        scene_paused = !scene_paused;
                        runtime_state.record_event(
                            scene_paused ? frame_warp_lab::RuntimeEventType::scene_paused : frame_warp_lab::RuntimeEventType::scene_resumed,
                            scene_paused ? 1 : 0);
                    } else if (message.wParam == 'R') {
                        if (scene_kind == frame_warp_lab::SceneKind::range || scene_kind == frame_warp_lab::SceneKind::dust2) {
                            tactical_range.request_reload();
                        } else {
                            simulation.reset();
                            renderer.invalidate_source_history();
                            cadence_counter = 0;
                            pacer.reset();
                            runtime_state.record_event(frame_warp_lab::RuntimeEventType::scene_reset, 0);
                        }
                    } else if (message.wParam == VK_F5) {
                        if (scene_kind == frame_warp_lab::SceneKind::range) {
                            simulation.reset_for_tactical_range();
                            tactical_range.reset();
                        } else if (scene_kind == frame_warp_lab::SceneKind::dust2) {
                            const auto& d2 = renderer.dust2_map();
                            simulation.reset_for_dust2(d2.spawn_pos, d2.spawn_yaw, d2.world_min, d2.world_max);
                            tactical_range.reset();
                        } else {
                            simulation.reset();
                        }
                        has_prev_dust2_cam = false;
                        renderer.invalidate_source_history();
                        cadence_counter = 0;
                        pacer.reset();
                        runtime_state.record_event(frame_warp_lab::RuntimeEventType::scene_reset, 0);
                    } else if (message.wParam == VK_F6) {
                        tactical_range.reload_lua_scripts();
                    } else if (message.wParam == VK_TAB) {
                        if (scene_kind == frame_warp_lab::SceneKind::range) {
                            scene_kind = renderer.dust2_map().loaded ? frame_warp_lab::SceneKind::dust2 : frame_warp_lab::SceneKind::corridor;
                        } else if (scene_kind == frame_warp_lab::SceneKind::dust2) {
                            scene_kind = frame_warp_lab::SceneKind::corridor;
                        } else {
                            scene_kind = frame_warp_lab::SceneKind::range;
                        }
                        input_state.range_mode = (scene_kind == frame_warp_lab::SceneKind::range || scene_kind == frame_warp_lab::SceneKind::dust2);
                        if (scene_kind == frame_warp_lab::SceneKind::range) {
                            simulation.reset_for_tactical_range();
                            tactical_range.reset();
                        } else if (scene_kind == frame_warp_lab::SceneKind::dust2) {
                            const auto& d2 = renderer.dust2_map();
                            simulation.reset_for_dust2(d2.spawn_pos, d2.spawn_yaw, d2.world_min, d2.world_max);
                            tactical_range.reset();
                        } else {
                            simulation.set_range_camera_modifiers(0.0f, 0.0f, 0.0f, false);
                            simulation.set_path(frame_warp_lab::CameraPath::corridor_sway);
                            simulation.reset();
                        }
                        has_prev_dust2_cam = false;
                        renderer.invalidate_source_history();
                        cadence_counter = 0;
                        config_changed = true;
                    } else if (message.wParam == 'C') {
                        const auto next_path = static_cast<frame_warp_lab::CameraPath>((static_cast<std::int32_t>(simulation.path()) + 1) % 6);
                        simulation.set_path(next_path);
                        has_prev_dust2_cam = false;
                        renderer.invalidate_source_history();
                        config_changed = true;
                    } else if (message.wParam == VK_OEM_4) {
                        source_divider = std::max(1, source_divider - 1);
                        cadence_counter = 0;
                        config_changed = true;
                    } else if (message.wParam == VK_OEM_6) {
                        source_divider = std::min(frame_warp_lab::max_source_divider, source_divider + 1);
                        cadence_counter = 0;
                        config_changed = true;
                    } else if (message.wParam == VK_OEM_MINUS) {
                        stress_level = std::max(0, stress_level - 4);
                        config_changed = true;
                    } else if (message.wParam == VK_OEM_PLUS) {
                        stress_level = std::min(frame_warp_lab::max_stress_level, stress_level + 4);
                        config_changed = true;
                    }
                    if (config_changed) {
                        runtime_state.record_event(
                            frame_warp_lab::RuntimeEventType::config_updated,
                            static_cast<std::int32_t>(message.wParam),
                            frame_warp_lab::ConfigKey::hotkey);
                        publish_config();
                    }
                }
                if (message.message == frame_warp_lab::control_message) {
                    handle_control_message(message.wParam, message.lParam);
                    continue;
                }
                TranslateMessage(&message);
                DispatchMessageW(&message);
                drain_queued_control_messages();
            }
            drain_queued_control_messages();
        };

        while (running) {
            pump_messages();
            if (!running) break;
            if (IsIconic(window)) {
                WaitMessage();
                previous_tick = std::chrono::steady_clock::now();
                has_previous_present = false;
                pacer.reset();
                continue;
            }

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
                pacer.reset();
                continue;
            }
            if (current_client_rectangle.right != previous_client_rectangle.right || current_client_rectangle.bottom != previous_client_rectangle.bottom) {
                renderer.resize(client_width, client_height);
                runtime_state.record_resize();
                previous_client_rectangle = current_client_rectangle;
                cadence_counter = 0;
                pacer.refresh_display_timing(window);
                pacer.reset();
                hud_state.target_frame_ms = pacer.target_frame_ms();
                publish_config();
            }

            if (!renderer.wait_for_frame_slot()) {
                continue;
            }
            const bool is_dlssg_mode = presentation_mode == frame_warp_lab::PresentationMode::dlss_g
                || presentation_mode == frame_warp_lab::PresentationMode::dlss_g_warp;
            const bool dlssg_can_run = is_dlssg_mode && renderer.streamline_capabilities().dlssg_supported;
            if (!dlssg_can_run) {
                const std::int64_t lead_us = std::clamp(static_cast<std::int64_t>(smoothed_cpu_work_us) + 450LL, 600LL, 2500LL);
                pacer.wait_for_input_window(lead_us, pump_messages, running);
            }
            pump_messages();
            if (!running) break;

            ++display_frame_id;
            const auto frame_id_u64 = static_cast<std::uint64_t>(display_frame_id);
            const std::uint32_t reflex_limit_us = [&]() -> std::uint32_t {
                if (!dlssg_can_run || pacer.target_fps_override() <= 0) return 0U;
                const std::int32_t fg_mult = dlssg_multiplier >= 2 ? dlssg_multiplier : 1;
                return static_cast<std::uint32_t>((1000000LL * static_cast<std::int64_t>(fg_mult)) / static_cast<std::int64_t>(pacer.target_fps_override()));
            }();
            renderer.begin_streamline_frame(frame_id_u64, dlssg_can_run, reflex_limit_us);
            const auto sim_begin_time = std::chrono::steady_clock::now();
            latency_monitor.emit_marker(frame_warp_lab::LatencyMarker::simulation_start, frame_id_u64);
            renderer.emit_streamline_pcl_marker(frame_warp_lab::StreamlinePclMarker::simulation_start);
            if (pending_pcl_ping) {
                latency_monitor.record_ping_received(frame_id_u64);
                renderer.emit_streamline_pcl_marker(frame_warp_lab::StreamlinePclMarker::pc_latency_ping);
                pending_pcl_ping = false;
            }

            bool has_input_timestamp = false;
            std::chrono::steady_clock::time_point earliest_input_time{};
            bool flash_triggered = false;
            const frame_warp_lab::ManualCameraInput manual_input = consume_manual_camera_input(
                window,
                input_state,
                has_input_timestamp,
                earliest_input_time,
                flash_triggered);
            if (flash_triggered && flash_indicator_enabled) {
                latency_monitor.emit_marker(frame_warp_lab::LatencyMarker::trigger_flash, frame_id_u64);
                renderer.emit_streamline_pcl_marker(frame_warp_lab::StreamlinePclMarker::trigger_flash);
            }

            if (simulation.take_manual_control_if_needed(manual_input)) {
                has_prev_dust2_cam = false;
                renderer.invalidate_source_history();
                runtime_state.record_event(
                    frame_warp_lab::RuntimeEventType::config_updated,
                    static_cast<std::int32_t>(frame_warp_lab::CameraPath::manual),
                    frame_warp_lab::ConfigKey::camera_path);
                publish_config();
            }

            const auto current_tick = std::chrono::steady_clock::now();
            const bool stepping_now = pending_step_frames > 0;
            const double delta_seconds = fixed_dt_us > 0
                ? (static_cast<double>(fixed_dt_us) / 1000000.0)
                : std::chrono::duration<double>(current_tick - previous_tick).count();
            const bool in_dust2_scene = (scene_kind == frame_warp_lab::SceneKind::dust2);
            const bool in_range_scene = (scene_kind == frame_warp_lab::SceneKind::range || in_dust2_scene);
            if (in_range_scene) {
                tactical_range.set_ads_requested(input_state.right_mouse_down || cli_ads_active);
                if (input_state.fire_trigger_pending) {
                    input_state.fire_trigger_pending = false;
                    tactical_range.request_fire();
                }
                simulation.set_range_camera_modifiers(
                    tactical_range.current_vertical_fov_radians(),
                    tactical_range.camera_recoil_pitch_radians(),
                    tactical_range.camera_recoil_yaw_radians(),
                    scene_kind == frame_warp_lab::SceneKind::range);
            } else {
                simulation.set_range_camera_modifiers(0.0f, 0.0f, 0.0f, false);
            }
            simulation.advance(delta_seconds, scene_paused && !stepping_now, manual_input);
            auto sample_active_camera = [&](std::int64_t& out_cut_seg) -> frame_warp_lab::CameraBasis {
                if (in_dust2_scene && simulation.path() != frame_warp_lab::CameraPath::manual) {
                    out_cut_seg = (simulation.path() == frame_warp_lab::CameraPath::camera_cut)
                        ? static_cast<std::int64_t>(std::floor(simulation.time_seconds() / 1.63))
                        : 0;
                    return frame_warp_lab::sample_dust2_camera_path(
                        renderer.dust2_map(),
                        simulation.path(),
                        simulation.time_seconds(),
                        tactical_range.current_vertical_fov_radians());
                }
                return simulation.sample_latest_camera(out_cut_seg);
            };
            if (in_range_scene) {
                std::int64_t range_cut_seg = 0;
                const frame_warp_lab::CameraBasis range_cam = sample_active_camera(range_cut_seg);
                tactical_range.step(delta_seconds, scene_paused && !stepping_now, range_cam);
                hud_state.tactical_hud_enabled = true;
                hud_state.tactical_hud = tactical_range.hud_snapshot(range_cam);
                runtime_state.update_tactical_range(
                    hud_state.tactical_hud,
                    tactical_range.ecs_lua_telemetry(),
                    renderer.hero_pbr_textures_loaded(),
                    renderer.pbr_build_time_ms());
            } else {
                hud_state.tactical_hud_enabled = false;
            }
            if (stepping_now) {
                --pending_step_frames;
                runtime_state.update_pending_steps(pending_step_frames);
            }
            previous_tick = current_tick;

            renderer.poll_completed_scenes();
            const std::int32_t effective_divider = (presentation_mode == frame_warp_lab::PresentationMode::full_render
                || presentation_mode == frame_warp_lab::PresentationMode::dlss_g) ? 1 : source_divider;
            const bool frozen_deterministic_pause = scene_paused && !stepping_now && fixed_dt_us > 0;
            const bool need_initial_source = !renderer.has_completed_source();
            const bool cadence_requests_scene = need_initial_source || (!frozen_deterministic_pause && (cadence_counter % effective_divider == 0));
            if (!frozen_deterministic_pause) {
                ++cadence_counter;
            }

            const frame_warp_lab::VrsMode active_vrs_mode = renderer.resolve_effective_vrs_mode(vrs_mode);
            const bool scene_overran_cadence = cadence_requests_scene && async_gpu_enabled && renderer.has_scene_in_flight() && !need_initial_source;
            const bool effective_selective_tiles = selective_tiles_enabled && !(auto_tile_budget_enabled && auto_tile_budget.par_bypass_active());
            bool scene_submitted_this_frame = false;
            if (cadence_requests_scene && (!renderer.has_scene_in_flight() || !async_gpu_enabled || need_initial_source)) {
                ++source_frame_id;
                const auto capture_time = std::chrono::steady_clock::now();
                frame_warp_lab::SceneSnapshot new_snapshot = simulation.capture_source_snapshot(source_frame_id);
                if (in_dust2_scene && simulation.path() != frame_warp_lab::CameraPath::manual) {
                    std::int64_t d2_cut = 0;
                    const frame_warp_lab::CameraBasis d2_cam = sample_active_camera(d2_cut);
                    const bool d2_disc = !has_prev_dust2_cam || (d2_cut != prev_dust2_cut);
                    new_snapshot.camera = d2_cam;
                    new_snapshot.previous_camera = d2_disc ? d2_cam : prev_dust2_cam;
                    new_snapshot.cut_segment = d2_cut;
                    new_snapshot.discontinuity = d2_disc;
                    prev_dust2_cam = d2_cam;
                    prev_dust2_cut = d2_cut;
                    has_prev_dust2_cam = true;
                }
                const bool must_wait = need_initial_source || !async_gpu_enabled;
                const frame_warp_lab::TacticalRangeFrame* range_payload = nullptr;
                if (in_range_scene) {
                    tactical_range.build_render_frame(new_snapshot.camera, new_snapshot.previous_camera, tactical_range_frame);
                    range_payload = &tactical_range_frame;
                }
                scene_submitted_this_frame = renderer.submit_scene_async(
                    new_snapshot,
                    capture_time,
                    stress_level,
                    shader_flags,
                    pbr_enabled,
                    aniso_mip_enabled,
                    vrs_mode,
                    effective_selective_tiles,
                    tile_budget_pct,
                    tile_policy,
                    cache_sharpen_enabled,
                    coarse_fallback_enabled,
                    subtile_interleave_enabled,
                    reactive_invalidation_enabled,
                    shadow_delta_invalidation_enabled,
                    forward_transparency_enabled,
                    split_diff_spec_enabled,
                    surface_cache_enabled,
                    clustered_lighting_enabled,
                    thin_gbuf_enabled,
                    surface_cache_mb,
                    frame_id_u64,
                    latency_monitor,
                    must_wait,
                    range_payload,
                    in_dust2_scene);
                if (in_dust2_scene) {
                    runtime_state.update_dust2_octree(renderer.dust2_octree_telemetry());
                }
            }

            renderer.poll_completed_scenes();
            const frame_warp_lab::CompletedSourceInfo active_source = renderer.latest_completed_source();
            const frame_warp_lab::CompletedSourceInfo previous_source = renderer.previous_completed_source();
            const bool source_is_fresh_this_frame = active_source.valid && (active_source.snapshot.source_frame_id == source_frame_id) && scene_submitted_this_frame;
            const bool has_previous_history = previous_source.valid
                && !active_source.snapshot.discontinuity
                && previous_source.slot_index != active_source.slot_index
                && previous_source.snapshot.cut_segment == active_source.snapshot.cut_segment;

            frame_warp_lab::FrameKind frame_kind = frame_warp_lab::FrameKind::rendered;
            frame_warp_lab::FallbackReason fallback_reason = frame_warp_lab::FallbackReason::none;
            frame_warp_lab::SynthesisMode synthesis_mode = frame_warp_lab::SynthesisMode::passthrough;
            float extrapolation_alpha = 0.0f;
            bool frame_discontinuity = active_source.snapshot.discontinuity;
            frame_warp_lab::CameraBasis target_camera = active_source.snapshot.camera;

            const bool mode_uses_synthesis = presentation_mode == frame_warp_lab::PresentationMode::reproject
                || presentation_mode == frame_warp_lab::PresentationMode::frame_gen
                || presentation_mode == frame_warp_lab::PresentationMode::combined
                || presentation_mode == frame_warp_lab::PresentationMode::dlss_g_warp;

            if (presentation_mode == frame_warp_lab::PresentationMode::dlss_g) {
                frame_kind = dlssg_can_run ? frame_warp_lab::FrameKind::dlss_generated : frame_warp_lab::FrameKind::rendered;
                target_camera = active_source.snapshot.camera;
            } else if (source_is_fresh_this_frame && presentation_mode != frame_warp_lab::PresentationMode::dlss_g_warp) {
                frame_kind = frame_warp_lab::FrameKind::rendered;
                target_camera = active_source.snapshot.camera;
            } else if (presentation_mode == frame_warp_lab::PresentationMode::repeat
                || presentation_mode == frame_warp_lab::PresentationMode::full_render) {
                frame_kind = frame_warp_lab::FrameKind::repeated;
                target_camera = active_source.snapshot.camera;
            } else if (mode_uses_synthesis) {
                std::int64_t current_cut_segment = 0;
                target_camera = sample_active_camera(current_cut_segment);
                const auto now_for_age = std::chrono::steady_clock::now();
                const std::int64_t pending_source_age_us = std::chrono::duration_cast<std::chrono::microseconds>(
                    now_for_age - active_source.capture_time).count();
                const std::int64_t max_safe_age_us = presentation_mode == frame_warp_lab::PresentationMode::dlss_g_warp
                    ? frame_warp_lab::max_dlss_warp_source_age_us
                    : frame_warp_lab::max_reprojection_source_age_us;
                if (current_cut_segment != active_source.snapshot.cut_segment) {
                    frame_kind = frame_warp_lab::FrameKind::fallback;
                    fallback_reason = frame_warp_lab::FallbackReason::camera_cut;
                    frame_discontinuity = true;
                    runtime_state.record_event(frame_warp_lab::RuntimeEventType::reprojection_fallback, static_cast<std::int32_t>(fallback_reason));
                } else if (pending_source_age_us > max_safe_age_us) {
                    frame_kind = frame_warp_lab::FrameKind::fallback;
                    fallback_reason = frame_warp_lab::FallbackReason::stale_source;
                    runtime_state.record_event(frame_warp_lab::RuntimeEventType::reprojection_fallback, static_cast<std::int32_t>(fallback_reason));
                } else {
                    const std::int64_t fallback_period_us = std::max<std::int64_t>(1000LL, static_cast<std::int64_t>(effective_divider) * pacer.target_period_us());
                    const std::int64_t measured_source_period_us = has_previous_history
                        ? std::max<std::int64_t>(1000LL, std::chrono::duration_cast<std::chrono::microseconds>(active_source.capture_time - previous_source.capture_time).count())
                        : fallback_period_us;
                    extrapolation_alpha = std::clamp(
                        static_cast<float>(std::max<std::int64_t>(0LL, pending_source_age_us)) / static_cast<float>(measured_source_period_us),
                        0.0f,
                        1.75f);
                    if (presentation_mode == frame_warp_lab::PresentationMode::reproject) {
                        frame_kind = frame_warp_lab::FrameKind::warped;
                        synthesis_mode = frame_warp_lab::SynthesisMode::reproject;
                    } else if (presentation_mode == frame_warp_lab::PresentationMode::dlss_g_warp) {
                        const bool guard_forcing_passthrough = dlss_warp_guard_frames > 0;
                        if (guard_forcing_passthrough) {
                            --dlss_warp_guard_frames;
                            frame_kind = dlssg_can_run ? frame_warp_lab::FrameKind::dlss_generated : frame_warp_lab::FrameKind::rendered;
                            synthesis_mode = frame_warp_lab::SynthesisMode::passthrough;
                            target_camera = active_source.snapshot.camera;
                        } else {
                            frame_kind = dlssg_can_run ? frame_warp_lab::FrameKind::dlss_warped : frame_warp_lab::FrameKind::warped;
                            synthesis_mode = source_is_fresh_this_frame
                                ? frame_warp_lab::SynthesisMode::passthrough
                                : frame_warp_lab::SynthesisMode::reproject;
                        }
                    } else if (presentation_mode == frame_warp_lab::PresentationMode::frame_gen) {
                        frame_kind = frame_warp_lab::FrameKind::generated;
                        synthesis_mode = frame_warp_lab::SynthesisMode::frame_gen;
                    } else {
                        frame_kind = frame_warp_lab::FrameKind::combined;
                        synthesis_mode = frame_warp_lab::SynthesisMode::combined;
                    }
                }
            }

            latency_monitor.emit_marker(frame_warp_lab::LatencyMarker::camera_constructed, frame_id_u64);
            renderer.emit_streamline_pcl_marker(frame_warp_lab::StreamlinePclMarker::camera_constructed);
            const auto sim_end_time = std::chrono::steady_clock::now();
            latency_monitor.emit_marker(frame_warp_lab::LatencyMarker::simulation_end, frame_id_u64);
            renderer.emit_streamline_pcl_marker(frame_warp_lab::StreamlinePclMarker::simulation_end);
            const std::int64_t sim_cpu_us = std::chrono::duration_cast<std::chrono::microseconds>(sim_end_time - sim_begin_time).count();

            if (overlay_enabled && instrumentation_level != frame_warp_lab::InstrumentationLevel::minimal) {
                const auto hud_elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(sim_end_time - last_hud_text_update).count();
                if (hud_text_dirty || hud_elapsed_ms >= 66) {
                    format_hud_lines(
                        hud_state,
                        smoothed_present_interval_us,
                        last_scene_gpu_us,
                        last_present_gpu_us,
                        last_input_to_present_us,
                        missed_deadlines,
                        last_invalid_pixels,
                        last_total_pixels,
                        presentation_mode,
                        pacer.mode(),
                        effective_divider,
                        adaptive_scheduler.enabled(),
                        adaptive_scheduler.tier(),
                        active_vrs_mode,
                        selective_tiles_enabled,
                        active_source.tile_stats,
                        simulation.path(),
                        stress_level,
                        input_state.mouse_look_locked);
                    last_hud_text_update = sim_end_time;
                    hud_text_dirty = false;
                }
            }

            const float dlss_object_motion_scale = presentation_mode == frame_warp_lab::PresentationMode::dlss_g_warp
                ? (1.0f / static_cast<float>(std::max<std::int32_t>(1, effective_divider)))
                : 1.0f;
            std::wstring capture_file_path;
            const std::uint32_t active_capture_tag = (pending_capture_tag != 0U && pending_step_frames == 0) ? pending_capture_tag : 0U;
            if (active_capture_tag != 0U) {
                pending_capture_tag = 0U;
                capture_file_path = (capture_dir / (L"capture_" + std::to_wstring(GetCurrentProcessId()) + L"_" + std::to_wstring(active_capture_tag) + L".ppm")).wstring();
            }
            const frame_warp_lab::FrameExecutionMetrics gpu_metrics = renderer.present_from_source(
                frame_warp_lab::PresentRequest{
                    frame_id_u64,
                    active_source.slot_index,
                    has_previous_history ? previous_source.slot_index : -1,
                    active_source.snapshot,
                    has_previous_history ? previous_source.snapshot.camera : active_source.snapshot.camera,
                    target_camera,
                    &hud_state,
                    diagnostic_view,
                    synthesis_mode,
                    active_vrs_mode,
                    upscale_filter,
                    instrumentation_level,
                    extrapolation_alpha,
                    has_previous_history,
                    overlay_enabled,
                    flash_indicator_enabled,
                    flash_triggered && flash_indicator_enabled,
                    vsync_enabled,
                    dlssg_can_run,
                    dlssg_multiplier,
                    static_cast<float>(pacer.target_fps_override()),
                    dlss_object_motion_scale,
                    frame_discontinuity,
                    capture_file_path.empty() ? nullptr : capture_file_path.c_str(),
                },
                latency_monitor,
                pacer);
            if (active_capture_tag != 0U) {
                runtime_state.record_capture_completed(
                    active_capture_tag,
                    display_frame_id,
                    static_cast<std::int64_t>(std::llround(simulation.time_seconds() * 1000000.0)));
            }

            const auto present_complete_time = std::chrono::steady_clock::now();
            const auto present_entry_time = gpu_metrics.present_call_time;
            const std::int64_t present_interval_us = has_previous_present
                ? std::chrono::duration_cast<std::chrono::microseconds>(present_entry_time - previous_present_tick).count()
                : 0;
            const std::int64_t source_age_us = std::chrono::duration_cast<std::chrono::microseconds>(present_entry_time - active_source.capture_time).count();
            const std::int64_t input_to_present_us = has_input_timestamp
                ? std::chrono::duration_cast<std::chrono::microseconds>(present_complete_time - earliest_input_time).count()
                : -1;
            previous_present_tick = present_entry_time;
            has_previous_present = true;

            if (gpu_metrics.missed_deadline) {
                ++missed_deadlines;
            }
            const double frame_cpu_work_us = static_cast<double>(sim_cpu_us + gpu_metrics.render_submit_cpu_us);
            smoothed_cpu_work_us = smoothed_cpu_work_us * 0.90 + frame_cpu_work_us * 0.10;

            if (present_interval_us > 0) {
                if (smoothed_present_interval_us <= 1.0) {
                    smoothed_present_interval_us = static_cast<double>(present_interval_us);
                } else {
                    smoothed_present_interval_us = smoothed_present_interval_us * 0.88 + static_cast<double>(present_interval_us) * 0.12;
                }
            }
            if (input_to_present_us >= 0) last_input_to_present_us = input_to_present_us;
            if (gpu_metrics.completed_scene_gpu_us >= 0) last_scene_gpu_us = gpu_metrics.completed_scene_gpu_us;
            if (gpu_metrics.completed_present_gpu_us >= 0) last_present_gpu_us = gpu_metrics.completed_present_gpu_us;

            const bool window_focused = GetForegroundWindow() == window;
            if (unified_controller.enabled() && active_source.valid && !frozen_deterministic_pause) {
                frame_warp_lab::UnifiedQualityProfile uprof{};
                const std::uint32_t sc_total = active_source.tile_stats.sc_hits + active_source.tile_stats.sc_misses;
                const std::int32_t sc_hit_permille = sc_total >= 64U
                    ? static_cast<std::int32_t>((static_cast<std::uint64_t>(active_source.tile_stats.sc_hits) * 1000ULL) / sc_total)
                    : -1;
                if (unified_controller.update(
                        pacer.target_period_us(),
                        active_source.snapshot.source_frame_id,
                        active_source.scene_gpu_us,
                        last_present_gpu_us,
                        gpu_metrics.missed_deadline,
                        active_source.tile_stats.total_tiles,
                        active_source.tile_stats.critical_deferred_tiles,
                        sc_hit_permille,
                        uprof)) {
                    const bool split_changed = (split_diff_spec_enabled != uprof.split_diff_spec);
                    selective_tiles_enabled = uprof.selective_tiles;
                    split_diff_spec_enabled = uprof.split_diff_spec;
                    surface_cache_enabled = uprof.surface_cache;
                    subtile_interleave_enabled = uprof.subtile_interleave;
                    cache_sharpen_enabled = uprof.cache_sharpen;
                    tile_budget_pct = uprof.tile_budget_pct;
                    vrs_mode = uprof.vrs;
                    presentation_mode = uprof.mode;
                    source_divider = uprof.divider;
                    if (render_scale_pct != uprof.render_scale_pct) {
                        render_scale_pct = uprof.render_scale_pct;
                        if (render_override_width > 0U && render_override_height > 0U) {
                            const UINT scaled_w = std::clamp<UINT>(static_cast<UINT>(std::lround(static_cast<double>(render_override_width) * static_cast<double>(render_scale_pct) / 100.0)) & ~1U, 160U, 7680U);
                            const UINT scaled_h = std::clamp<UINT>(static_cast<UINT>(std::lround(static_cast<double>(render_override_height) * static_cast<double>(render_scale_pct) / 100.0)) & ~1U, 120U, 4320U);
                            renderer.set_render_resolution(scaled_w, scaled_h, render_scale_pct);
                        } else {
                            renderer.set_render_resolution(0, 0, render_scale_pct);
                        }
                    }
                    (void)split_changed;
                    runtime_state.record_event(
                        frame_warp_lab::RuntimeEventType::scheduler_decision,
                        100 + unified_controller.tier());
                    publish_config();
                } else {
                    runtime_state.update_unified_controller(unified_controller.telemetry());
                }
            } else {
                const bool use_dlssg_adaptive_policy = adaptive_dlssg_enabled && renderer.streamline_capabilities().dlssg_supported && window_focused;
                frame_warp_lab::AdaptivePolicyTarget adapted_policy{};
                if (adaptive_scheduler.update(
                        pacer.target_period_us(),
                        last_scene_gpu_us,
                        last_present_gpu_us,
                        gpu_metrics.missed_deadline,
                        scene_overran_cadence,
                        renderer.vrs_tier(),
                        active_vrs_mode,
                        presentation_mode,
                        effective_divider,
                        dlssg_multiplier,
                        use_dlssg_adaptive_policy,
                        adapted_policy)) {
                    presentation_mode = adapted_policy.mode;
                    source_divider = adapted_policy.divider;
                    vrs_mode = adapted_policy.vrs;
                    async_gpu_enabled = adapted_policy.async_gpu;
                    if (adapted_policy.mode == frame_warp_lab::PresentationMode::dlss_g
                        || adapted_policy.mode == frame_warp_lab::PresentationMode::dlss_g_warp) {
                        dlssg_multiplier = adapted_policy.dlssg_multiplier;
                    }
                    cadence_counter = 0;
                    runtime_state.record_event(
                        frame_warp_lab::RuntimeEventType::scheduler_decision,
                        adaptive_scheduler.tier() * 10 + static_cast<std::int32_t>(adaptive_scheduler.reason()));
                    publish_config();
                } else {
                    runtime_state.update_adaptive(adaptive_scheduler.enabled(), adaptive_scheduler.telemetry());
                    runtime_state.update_unified_controller(unified_controller.telemetry());
                }
            }
            if (gpu_metrics.completed_slot_warped) {
                last_invalid_pixels = gpu_metrics.completed_invalid_pixel_count;
                last_total_pixels = gpu_metrics.completed_total_pixel_count;
                if (presentation_mode == frame_warp_lab::PresentationMode::dlss_g_warp
                    && gpu_metrics.completed_total_pixel_count > 0U) {
                    const double hole_ratio = static_cast<double>(gpu_metrics.completed_invalid_pixel_count)
                        / static_cast<double>(gpu_metrics.completed_total_pixel_count);
                    if (hole_ratio > 0.015 && dlss_warp_guard_frames == 0) {
                        dlss_warp_guard_frames = 8;
                        ++dlss_warp_guard_trips;
                    }
                }
            } else if (!mode_uses_synthesis) {
                last_invalid_pixels = 0;
                last_total_pixels = gpu_metrics.completed_total_pixel_count;
            }

            const float sample_gpu_ms = ((scene_submitted_this_frame && last_scene_gpu_us > 0 ? static_cast<float>(last_scene_gpu_us) : 0.0f)
                + (gpu_metrics.completed_present_gpu_us > 0 ? static_cast<float>(gpu_metrics.completed_present_gpu_us) : 0.0f)) / 1000.0f;
            hud_state.samples[hud_state.write_head & (frame_warp_lab::hud_graph_sample_count - 1)] = frame_warp_lab::HudGraphSample{
                static_cast<float>(present_interval_us) / 1000.0f,
                sample_gpu_ms,
                static_cast<float>(static_cast<std::int32_t>(frame_kind)),
                static_cast<float>(source_age_us) / 1000.0f,
            };
            hud_state.write_head = (hud_state.write_head + 1U) & (frame_warp_lab::hud_graph_sample_count - 1U);

            float yaw_deg = 0.0f;
            float pitch_deg = 0.0f;
            simulation.current_camera_angles_degrees(yaw_deg, pitch_deg);
            runtime_state.update_scene(
                simulation.time_seconds(),
                scene_paused,
                input_state.mouse_look_locked || input_state.left_drag_active,
                yaw_deg,
                pitch_deg,
                latency_monitor);
            const frame_warp_lab::PacingOwner active_pacing_owner = gpu_metrics.dlssg_state.dlssg_active
                ? frame_warp_lab::PacingOwner::streamline_reflex_dlssg
                : (pacer.mode() == frame_warp_lab::PacerMode::deadline
                    ? frame_warp_lab::PacingOwner::app_deadline_pacer
                    : (pacer.mode() == frame_warp_lab::PacerMode::vsync_only
                        ? frame_warp_lab::PacingOwner::dxgi_vsync
                        : frame_warp_lab::PacingOwner::unlocked));
            if (!unified_controller.enabled() && auto_tile_budget_enabled && selective_tiles_enabled && active_source.valid && !frozen_deterministic_pause) {
                if (auto_tile_budget.update(
                        pacer.target_period_us(),
                        effective_divider,
                        active_source.snapshot.source_frame_id,
                        active_source.scene_gpu_us,
                        last_present_gpu_us,
                        active_source.tile_stats.total_tiles,
                        active_source.tile_stats.shaded_tiles,
                        active_source.tile_stats.critical_total_tiles,
                        active_source.tile_stats.critical_deferred_tiles,
                        frame_discontinuity,
                        gpu_metrics.missed_deadline,
                        tile_budget_pct)) {
                    hud_text_dirty = true;
                }
            }
            if (!unified_controller.enabled() && drs_mode != frame_warp_lab::DrsMode::off && !frozen_deterministic_pause) {
                bool drs_changed = false;
                if (drs_mode == frame_warp_lab::DrsMode::sweep) {
                    drs_changed = auto_drs.update_sweep(drs_min_pct, drs_max_pct, render_scale_pct);
                } else if (active_source.valid) {
                    drs_changed = auto_drs.update_deadline(
                        pacer.target_period_us(),
                        effective_divider,
                        active_source.snapshot.source_frame_id,
                        active_source.scene_gpu_us,
                        last_present_gpu_us,
                        gpu_metrics.missed_deadline,
                        drs_min_pct,
                        drs_max_pct,
                        render_scale_pct);
                }
                if (drs_changed) {
                    if (render_override_width > 0U && render_override_height > 0U) {
                        const UINT scaled_w = std::clamp<UINT>(static_cast<UINT>(std::lround(static_cast<double>(render_override_width) * static_cast<double>(render_scale_pct) / 100.0)) & ~1U, 160U, 7680U);
                        const UINT scaled_h = std::clamp<UINT>(static_cast<UINT>(std::lround(static_cast<double>(render_override_height) * static_cast<double>(render_scale_pct) / 100.0)) & ~1U, 120U, 4320U);
                        renderer.set_render_resolution(scaled_w, scaled_h, render_scale_pct);
                    } else {
                        renderer.set_render_resolution(0, 0, render_scale_pct);
                    }
                    hud_text_dirty = true;
                }
            }
            runtime_state.update_present_variant(gpu_metrics.selected_present_variant);
            runtime_state.update_selective_tiles(
                selective_tiles_enabled,
                tile_budget_pct,
                tile_policy,
                auto_tile_budget_enabled,
                cache_sharpen_enabled,
                coarse_fallback_enabled,
                subtile_interleave_enabled,
                reactive_invalidation_enabled,
                shadow_delta_invalidation_enabled,
                forward_transparency_enabled,
                split_diff_spec_enabled,
                surface_cache_enabled,
                clustered_lighting_enabled,
                thin_gbuf_enabled,
                surface_cache_mb,
                auto_tile_budget_enabled && auto_tile_budget.par_bypass_active(),
                auto_tile_budget.smoothed_damage_pct(),
                render_scale_pct,
                static_cast<std::int32_t>(renderer.render_width()),
                static_cast<std::int32_t>(renderer.render_height()),
                static_cast<std::int32_t>(renderer.alloc_render_width()),
                static_cast<std::int32_t>(renderer.alloc_render_height()),
                upscale_filter,
                drs_mode,
                drs_min_pct,
                drs_max_pct,
                renderer.drs_scale_changes(),
                renderer.drs_reallocations(),
                auto_drs.last_target_scene_budget_us(),
                fixed_dt_us,
                auto_tile_budget.last_target_scene_budget_us(),
                active_source.tile_stats);
            runtime_state.update_dlssg(
                dlssg_multiplier,
                window_focused,
                active_pacing_owner,
                adaptive_dlssg_enabled,
                dlss_warp_guard_frames > 0,
                dlss_warp_guard_trips,
                gpu_metrics.dlssg_state);
            runtime_state.record_frame(
                frame_warp_lab::RuntimeFrameSample{
                    0,
                    0,
                    display_frame_id,
                    active_source.snapshot.source_frame_id,
                    gpu_metrics.completed_gpu_display_frame_id,
                    gpu_metrics.completed_gpu_source_frame_id,
                    present_interval_us,
                    source_age_us,
                    input_to_present_us,
                    sim_cpu_us,
                    gpu_metrics.render_submit_cpu_us,
                    gpu_metrics.present_call_cpu_us,
                    gpu_metrics.completed_scene_gpu_us,
                    gpu_metrics.completed_present_gpu_us,
                    gpu_metrics.completed_invalid_pixel_count,
                    gpu_metrics.completed_temporal_recovered_count,
                    gpu_metrics.completed_total_pixel_count,
                    static_cast<std::int32_t>(presentation_mode),
                    static_cast<std::int32_t>(frame_kind),
                    static_cast<std::int32_t>(fallback_reason),
                    static_cast<std::int32_t>(diagnostic_view),
                    static_cast<std::int32_t>(simulation.path()),
                    stress_level,
                    static_cast<std::int32_t>(active_vrs_mode),
                    adaptive_scheduler.tier(),
                    effective_divider,
                    scene_submitted_this_frame ? 1 : 0,
                    gpu_metrics.completed_slot_warped ? 1 : 0,
                    gpu_metrics.async_overlapped ? 1 : 0,
                    gpu_metrics.missed_deadline ? 1 : 0,
                    frame_discontinuity ? 1 : 0,
                    gpu_metrics.dlssg_state.frames_actually_presented,
                    active_source.tile_stats.total_tiles,
                    active_source.tile_stats.shaded_tiles,
                    active_source.tile_stats.deferred_tiles,
                    active_source.tile_stats.critical_total_tiles,
                    active_source.tile_stats.critical_deferred_tiles,
                    render_scale_pct,
                },
                static_cast<std::int64_t>(smoothed_present_interval_us));
        }

        set_mouse_look_locked(window, input_state, false);
        latency_monitor.stop();
        renderer.flush();
        return 0;
    } catch (const std::exception& error) {
        MessageBoxA(nullptr, error.what(), "Frame Warp Lab error", MB_OK | MB_ICONERROR);
        return 1;
    }
}
