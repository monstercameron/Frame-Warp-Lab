#pragma once

#include <windows.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>

namespace frame_warp_lab {

inline constexpr wchar_t window_class_name[] = L"FrameWarpLabWindow";
inline constexpr std::uint32_t runtime_state_version = 35;
inline constexpr std::size_t runtime_event_capacity = 64;
inline constexpr std::size_t runtime_frame_trace_capacity = 256;
inline constexpr std::size_t hud_graph_sample_count = 128;
inline constexpr std::int32_t max_stress_level = 64;
inline constexpr std::int32_t max_source_divider = 8;
inline constexpr std::int32_t max_target_fps = 500;
inline constexpr std::int64_t max_reprojection_source_age_us = 250000;
inline constexpr std::int64_t max_dlss_warp_source_age_us = 40000;
inline constexpr UINT control_message = WM_APP + 1;

enum class ControlCommand : WPARAM {
    scene_pause = 1,
    scene_resume = 2,
    scene_reset = 3,
    set_diagnostic_view = 4,
    set_camera_path = 5,
    set_stress_level = 6,
    set_overlay_enabled = 7,
    set_presentation_mode = 8,
    set_source_divider = 9,
    set_vsync_enabled = 10,
    set_flash_indicator = 11,
    set_pacer_mode = 12,
    set_target_fps = 13,
    set_async_gpu = 14,
    set_vrs_mode = 15,
    set_adaptive_scheduler = 16,
    set_instrumentation_level = 17,
    set_dlssg_multiplier = 18,
    set_adaptive_dlssg = 19,
    set_selective_tiles = 20,
    set_tile_budget_pct = 21,
    set_tile_policy = 22,
    set_auto_tile_budget = 23,
    set_cache_sharpen = 24,
    set_fixed_dt_us = 25,
    capture_frame = 26,
    step_frames = 27,
    set_coarse_fallback = 28,
    resize_window = 29,
    set_render_scale = 30,
    set_render_size = 31,
    set_upscale_filter = 32,
    set_drs_mode = 33,
    set_drs_min_pct = 34,
    set_drs_max_pct = 35,
    set_subtile_interleave = 36,
    set_shader_flags = 37,
    set_pbr_enabled = 38,
    set_aniso_mip_enabled = 39,
    set_scene_kind = 40,
    range_fire = 41,
    set_range_ads = 42,
    range_reload = 43,
    reload_lua_scripts = 44,
    emit_range_event = 45,
    set_reactive_invalidation = 46,
    set_shadow_delta_invalidation = 47,
    set_forward_transparency = 48,
    set_split_diff_spec = 49,
    set_surface_cache = 50,
    set_unified_controller = 51,
    set_clustered_lighting = 52,
    set_surface_cache_mb = 53,
    set_thin_gbuf = 54,
};

enum class SceneKind : std::int32_t {
    corridor = 0,
    range = 1,
};

enum class DrsMode : std::int32_t {
    off = 0,
    auto_deadline = 1,
    sweep = 2,
};

enum class UpscaleFilter : std::int32_t {
    bilinear = 0,
    catmull_rom = 1,
    cas = 2,
    nearest = 3,
};

enum class TilePolicy : std::int32_t {
    fifo = 0,
    priority_stbn = 1,
};

enum class PacingOwner : std::int32_t {
    app_deadline_pacer = 0,
    dxgi_vsync = 1,
    unlocked = 2,
    streamline_reflex_dlssg = 3,
};

enum class InstrumentationLevel : std::int32_t {
    minimal = 0,
    standard = 1,
    diagnostic = 2,
};

enum class SchedulerReason : std::int32_t {
    manual_override = 0,
    headroom_ok = 1,
    gpu_headroom_pressure = 2,
    deadline_overrun = 3,
    headroom_recovered = 4,
};

enum class VrsMode : std::int32_t {
    off = 0,
    uniform_2x2 = 1,
    foveated = 2,
};

enum class PacerMode : std::int32_t {
    deadline = 0,
    vsync_only = 1,
    unlocked = 2,
};

enum class PresentationMode : std::int32_t {
    full_render = 0,
    repeat = 1,
    reproject = 2,
    frame_gen = 3,
    combined = 4,
    dlss_g = 5,
    dlss_g_warp = 6,
};

enum class DiagnosticView : std::int32_t {
    final_color = 0,
    depth = 1,
    motion_vectors = 2,
    reprojection_holes = 3,
    vrs_mask = 4,
    tile_cache = 5,
    tile_priority = 6,
};

enum class CameraPath : std::int32_t {
    corridor_sway = 0,
    fast_pan = 1,
    occlusion_sweep = 2,
    camera_cut = 3,
    manual = 4,
    pure_rotation = 5,
};

enum class PresentVariant : std::int32_t {
    simple = 0,
    rotation_homography = 1,
    depth_fast = 2,
    depth_full = 3,
    synthesis = 4,
};

enum class FrameKind : std::int32_t {
    rendered = 0,
    repeated = 1,
    warped = 2,
    fallback = 3,
    generated = 4,
    combined = 5,
    dlss_generated = 6,
    dlss_warped = 7,
};

enum class FallbackReason : std::int32_t {
    none = 0,
    camera_cut = 1,
    stale_source = 2,
    no_source_frame = 3,
};

enum class ConfigKey : std::int32_t {
    none = 0,
    diagnostic_view = 1,
    camera_path = 2,
    stress_level = 3,
    overlay_enabled = 4,
    presentation_mode = 5,
    source_divider = 6,
    vsync_enabled = 7,
    flash_indicator = 8,
    pacer_mode = 9,
    target_fps = 10,
    async_gpu = 11,
    vrs_mode = 12,
    adaptive_scheduler = 13,
    instrumentation_level = 14,
    hotkey = 15,
    dlssg_multiplier = 16,
    adaptive_dlssg = 17,
    selective_tiles = 18,
    tile_budget_pct = 19,
    tile_policy = 20,
    auto_tile_budget = 21,
    cache_sharpen = 22,
    fixed_dt_us = 23,
    coarse_fallback = 24,
    render_scale = 25,
    render_size = 26,
    upscale_filter = 27,
    drs_mode = 28,
    drs_min_pct = 29,
    drs_max_pct = 30,
    subtile_interleave = 31,
    shader_flags = 32,
    pbr_enabled = 33,
    aniso_mip_enabled = 34,
    scene_kind = 35,
    reactive_invalidation = 36,
    shadow_delta_invalidation = 37,
    forward_transparency = 38,
    split_diff_spec = 39,
    surface_cache = 40,
    unified_controller = 41,
    clustered_lighting = 42,
    surface_cache_mb = 43,
    thin_gbuf = 44,
};

enum class RuntimeEventType : std::int32_t {
    runtime_started = 1,
    swap_chain_resized = 2,
    runtime_stopping = 3,
    scene_paused = 4,
    scene_resumed = 5,
    scene_reset = 6,
    config_updated = 7,
    reprojection_fallback = 8,
    scheduler_decision = 9,
};

struct RuntimeEvent {
    std::uint64_t seqlock_version;
    std::int32_t sequence;
    std::uint32_t timestamp_low;
    std::uint32_t timestamp_high;
    std::int32_t event_type;
    std::int32_t config_key;
    std::int32_t value;
};

struct RuntimeFrameSample {
    std::uint64_t seqlock_version;
    std::int64_t sequence;
    std::int64_t display_frame_id;
    std::int64_t source_frame_id;
    std::int64_t gpu_measured_display_frame_id;
    std::int64_t gpu_measured_source_frame_id;
    std::int64_t present_interval_us;
    std::int64_t source_age_us;
    std::int64_t input_to_present_us;
    std::int64_t sim_cpu_us;
    std::int64_t render_submit_cpu_us;
    std::int64_t present_call_cpu_us;
    std::int64_t scene_gpu_us;
    std::int64_t present_gpu_us;
    std::uint32_t invalid_pixel_count;
    std::uint32_t temporal_recovered_count;
    std::uint32_t total_pixel_count;
    std::int32_t presentation_mode;
    std::int32_t frame_kind;
    std::int32_t fallback_reason;
    std::int32_t diagnostic_view;
    std::int32_t camera_path;
    std::int32_t stress_level;
    std::int32_t vrs_mode;
    std::int32_t adaptive_tier;
    std::int32_t source_divider;
    std::int32_t source_rendered;
    std::int32_t gpu_slot_warped;
    std::int32_t async_overlapped;
    std::int32_t missed_deadline;
    std::int32_t discontinuity;
    std::uint32_t dlssg_frames_presented;
    std::uint32_t tile_total_count;
    std::uint32_t tile_shaded_count;
    std::uint32_t tile_deferred_count;
    std::uint32_t tile_critical_total;
    std::uint32_t tile_critical_deferred;
    std::int32_t render_scale_pct;
};

struct RuntimeState {
    std::uint32_t version;
    std::uint32_t process_id;
    std::int32_t present_count;
    std::int32_t resize_count;
    std::int32_t running;
    std::int32_t scene_paused;
    std::int64_t scene_time_ms;
    std::int64_t source_frame_id;
    std::int64_t display_frame_id;
    std::int64_t rendered_frame_count;
    std::int64_t repeated_frame_count;
    std::int64_t warped_frame_count;
    std::int64_t generated_frame_count;
    std::int64_t combined_frame_count;
    std::int64_t fallback_frame_count;
    std::int64_t async_overlap_count;
    std::int64_t missed_deadline_count;
    std::int64_t target_period_us;
    std::int64_t last_present_interval_us;
    std::int64_t smoothed_present_interval_us;
    std::int64_t last_source_age_us;
    std::int64_t last_input_to_present_us;
    std::int64_t last_sim_cpu_us;
    std::int64_t last_render_submit_cpu_us;
    std::int64_t last_present_call_cpu_us;
    std::int64_t last_scene_gpu_us;
    std::int64_t last_present_gpu_us;
    std::int64_t pcl_markers_emitted;
    std::int64_t pcl_pings_received;
    std::uint32_t last_invalid_pixel_count;
    std::uint32_t last_temporal_recovered_count;
    std::uint32_t last_total_pixel_count;
    std::int32_t presentation_mode;
    std::int32_t pacer_mode;
    std::int32_t target_fps;
    std::int32_t source_divider;
    std::int32_t vsync_enabled;
    std::int32_t allow_tearing_enabled;
    std::int32_t waitable_swap_chain;
    std::int32_t async_gpu_enabled;
    std::int32_t high_priority_present_queue;
    std::int32_t vrs_tier;
    std::int32_t vrs_tile_size_px;
    std::int32_t vrs_additional_rates_supported;
    std::int32_t vrs_requested_mode;
    std::int32_t vrs_active_mode;
    std::int32_t adaptive_enabled;
    std::int32_t adaptive_tier;
    std::int32_t adaptive_reason;
    std::int32_t adaptive_utilization_permille;
    std::int64_t adaptive_reserved_headroom_us;
    std::int64_t adaptive_decision_count;
    std::int32_t instrumentation_level;
    std::int32_t last_present_variant;
    std::int32_t diagnostic_view;
    std::int32_t camera_path;
    std::int32_t stress_level;
    std::uint32_t shader_flags;
    std::int32_t pbr_enabled;
    std::int32_t aniso_mip_enabled;
    std::int32_t scene_kind;
    std::int32_t tactical_hp;
    std::int32_t tactical_armor;
    std::int32_t tactical_ammo_in_mag;
    std::int32_t tactical_ammo_reserve;
    std::int32_t tactical_shots_fired;
    std::int32_t tactical_hits;
    std::int32_t tactical_headshots;
    std::int32_t tactical_score;
    std::int32_t tactical_targets_up;
    std::int32_t tactical_ads_active;
    std::uint32_t ecs_entity_count;
    std::uint32_t ecs_renderable_count;
    std::uint32_t ecs_physics_count;
    std::uint32_t ecs_script_count;
    std::uint32_t lua_loaded_scripts;
    std::uint32_t lua_reload_count;
    std::uint32_t lua_error_count;
    std::int64_t lua_last_step_us;
    std::uint64_t events_dispatched;
    std::uint32_t event_subscriptions;
    std::int32_t hero_pistol_gltf_loaded;
    std::int32_t hero_pbr_textures_loaded;
    std::uint32_t mt_worker_threads;
    std::uint32_t mt_jolt_threads;
    std::uint64_t mt_jobs_executed;
    std::int64_t mt_physics_step_us;
    std::int64_t mt_ecs_sync_us;
    std::int64_t mt_pbr_build_ms;
    std::int32_t tactical_spread_mrad_x100;
    std::int32_t tactical_shot_offset_mrad_x100;
    std::int32_t overlay_enabled;
    std::int32_t flash_indicator_enabled;
    std::int32_t raw_input_enabled;
    std::int32_t pcl_etw_listener_active;
    std::int32_t mouse_look_active;
    std::int32_t camera_yaw_millideg;
    std::int32_t camera_pitch_millideg;
    std::int32_t gpu_timing_available;
    std::int32_t streamline_initialized;
    std::int32_t dlssg_supported;
    std::int32_t dlss_sr_supported;
    std::int32_t reflex_supported;
    std::int32_t dlssg_dynamic_mfg_supported;
    std::int32_t dlssg_max_frames_to_generate;
    std::int32_t dlssg_requested_multiplier;
    std::int32_t dlssg_active;
    std::int32_t pacing_owner;
    std::int32_t adaptive_dlssg_enabled;
    std::int32_t dlssg_last_api_result;
    std::int32_t dlssg_last_failed_call;
    std::int32_t dlssg_warp_guard_active;
    std::int64_t dlssg_api_error_count;
    std::int64_t dlssg_warp_guard_trips;
    std::uint32_t dlssg_status_flags;
    std::uint32_t dlssg_last_frames_presented;
    std::int64_t dlssg_total_frames_presented;
    std::int64_t dlssg_total_generated_frames;
    std::int32_t event_count;
    std::int32_t window_focused;
    std::int32_t selective_tiles_enabled;
    std::int32_t tile_budget_pct;
    std::int32_t tile_policy;
    std::int32_t auto_tile_budget_enabled;
    std::int32_t cache_sharpen_enabled;
    std::int32_t coarse_fallback_enabled;
    std::int32_t subtile_interleave_enabled;
    std::int32_t reactive_invalidation_enabled;
    std::int32_t shadow_delta_invalidation_enabled;
    std::int32_t forward_transparency_enabled;
    std::int32_t split_diff_spec_enabled;
    std::int32_t surface_cache_enabled;
    std::int32_t clustered_lighting_enabled;
    std::int32_t thin_gbuf_enabled;
    std::int32_t surface_cache_mb;
    std::uint32_t last_sc_hits;
    std::uint32_t last_sc_misses;
    std::uint32_t last_sc_writes;
    std::uint32_t last_sc_collisions;
    std::uint32_t last_clustered_light_sum;
    std::int32_t unified_controller_enabled;
    std::int32_t unified_controller_tier;
    std::int32_t unified_controller_util_permille;
    std::int64_t unified_controller_budget_us;
    std::int64_t unified_controller_transitions;
    std::int64_t unified_controller_oscillations;
    std::int32_t par_bypass_active;
    std::int32_t smoothed_damage_pct;
    std::int32_t render_scale_pct;
    std::int32_t render_width;
    std::int32_t render_height;
    std::int32_t alloc_render_width;
    std::int32_t alloc_render_height;
    std::int32_t upscale_filter;
    std::int32_t drs_mode;
    std::int32_t drs_min_pct;
    std::int32_t drs_max_pct;
    std::int64_t drs_scale_changes;
    std::int64_t drs_reallocations;
    std::int64_t drs_target_scene_budget_us;
    std::int32_t fixed_dt_us;
    std::int64_t last_target_scene_budget_us;
    std::uint32_t last_tile_total_count;
    std::uint32_t last_tile_cached_count;
    std::uint32_t last_tile_shaded_count;
    std::uint32_t last_tile_deferred_count;
    std::uint32_t last_tile_critical_total;
    std::uint32_t last_tile_critical_deferred;
    std::uint32_t last_capture_tag;
    std::int64_t last_capture_frame_id;
    std::int64_t last_capture_scene_time_us;
    std::int32_t pending_step_frames;
    std::int64_t trace_count;
    RuntimeEvent events[runtime_event_capacity];
    RuntimeFrameSample trace[runtime_frame_trace_capacity];
};

static_assert(std::atomic_ref<std::int32_t>::is_always_lock_free);
static_assert(std::atomic_ref<std::uint32_t>::is_always_lock_free);
static_assert(std::atomic_ref<std::int64_t>::is_always_lock_free);
static_assert(std::atomic_ref<std::uint64_t>::is_always_lock_free);

template <typename Record, typename WriterFn>
inline void seqlock_write(Record& slot, WriterFn&& writer) {
    std::atomic_ref<std::uint64_t> version(slot.seqlock_version);
    const std::uint64_t prior = version.load(std::memory_order_relaxed);
    const std::uint64_t odd = (prior + 1ULL) | 1ULL;
    version.store(odd, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
    writer(slot);
    std::atomic_thread_fence(std::memory_order_release);
    version.store(odd + 1ULL, std::memory_order_release);
}

template <typename Record>
inline bool seqlock_try_read(const Record& slot, Record& out_copy) {
    std::atomic_ref<std::uint64_t> version(const_cast<std::uint64_t&>(slot.seqlock_version));
    for (int attempt = 0; attempt < 4; ++attempt) {
        const std::uint64_t begin_version = version.load(std::memory_order_acquire);
        if ((begin_version & 1ULL) != 0ULL || begin_version == 0ULL) continue;
        std::atomic_thread_fence(std::memory_order_acquire);
        out_copy = slot;
        std::atomic_thread_fence(std::memory_order_acquire);
        const std::uint64_t end_version = version.load(std::memory_order_acquire);
        if (begin_version == end_version) {
            out_copy.seqlock_version = end_version;
            return true;
        }
    }
    return false;
}

inline const char* instrumentation_level_name(std::int32_t level) {
    switch (static_cast<InstrumentationLevel>(level)) {
        case InstrumentationLevel::minimal: return "minimal";
        case InstrumentationLevel::standard: return "standard";
        case InstrumentationLevel::diagnostic: return "diagnostic";
        default: return "unknown";
    }
}

inline const char* config_key_name(std::int32_t key) {
    switch (static_cast<ConfigKey>(key)) {
        case ConfigKey::none: return "none";
        case ConfigKey::diagnostic_view: return "view";
        case ConfigKey::camera_path: return "path";
        case ConfigKey::stress_level: return "stress";
        case ConfigKey::overlay_enabled: return "overlay";
        case ConfigKey::presentation_mode: return "mode";
        case ConfigKey::source_divider: return "divider";
        case ConfigKey::vsync_enabled: return "vsync";
        case ConfigKey::flash_indicator: return "flash";
        case ConfigKey::pacer_mode: return "pacer";
        case ConfigKey::target_fps: return "target_fps";
        case ConfigKey::async_gpu: return "async_gpu";
        case ConfigKey::vrs_mode: return "vrs";
        case ConfigKey::adaptive_scheduler: return "adaptive";
        case ConfigKey::instrumentation_level: return "instrumentation";
        case ConfigKey::hotkey: return "hotkey";
        case ConfigKey::dlssg_multiplier: return "dlssg_mult";
        case ConfigKey::adaptive_dlssg: return "adaptive_dlssg";
        case ConfigKey::selective_tiles: return "selective_tiles";
        case ConfigKey::tile_budget_pct: return "tile_budget";
        case ConfigKey::tile_policy: return "tile_policy";
        case ConfigKey::auto_tile_budget: return "auto_tile_budget";
        case ConfigKey::cache_sharpen: return "cache_sharpen";
        case ConfigKey::fixed_dt_us: return "fixed_dt_us";
        case ConfigKey::coarse_fallback: return "coarse_fallback";
        case ConfigKey::render_scale: return "render_scale";
        case ConfigKey::render_size: return "render_size";
        case ConfigKey::upscale_filter: return "upscale_filter";
        case ConfigKey::drs_mode: return "drs";
        case ConfigKey::drs_min_pct: return "drs_min";
        case ConfigKey::drs_max_pct: return "drs_max";
        case ConfigKey::subtile_interleave: return "subtile_interleave";
        case ConfigKey::shader_flags: return "shader_feature";
        case ConfigKey::pbr_enabled: return "pbr";
        case ConfigKey::aniso_mip_enabled: return "aniso_mip";
        case ConfigKey::scene_kind: return "scene";
        case ConfigKey::reactive_invalidation: return "reactive_invalidation";
        case ConfigKey::shadow_delta_invalidation: return "shadow_delta_invalidation";
        case ConfigKey::forward_transparency: return "forward_transparency";
        case ConfigKey::split_diff_spec: return "split_diff_spec";
        case ConfigKey::surface_cache: return "surface_cache";
        case ConfigKey::unified_controller: return "unified_controller";
        case ConfigKey::clustered_lighting: return "clustered_lighting";
        case ConfigKey::surface_cache_mb: return "surface_cache_mb";
        case ConfigKey::thin_gbuf: return "thin_gbuf";
        default: return "unknown";
    }
}

inline const char* scene_kind_name(std::int32_t kind) {
    switch (static_cast<SceneKind>(kind)) {
        case SceneKind::corridor: return "corridor";
        case SceneKind::range: return "range";
        default: return "unknown";
    }
}

inline const char* drs_mode_name(std::int32_t mode) {
    switch (static_cast<DrsMode>(mode)) {
        case DrsMode::off: return "off";
        case DrsMode::auto_deadline: return "auto";
        case DrsMode::sweep: return "sweep";
        default: return "unknown";
    }
}

inline const char* upscale_filter_name(std::int32_t filter) {
    switch (static_cast<UpscaleFilter>(filter)) {
        case UpscaleFilter::bilinear: return "bilinear";
        case UpscaleFilter::catmull_rom: return "catmull_rom";
        case UpscaleFilter::cas: return "cas";
        case UpscaleFilter::nearest: return "nearest";
        default: return "unknown";
    }
}

inline const char* tile_policy_name(std::int32_t policy) {
    switch (static_cast<TilePolicy>(policy)) {
        case TilePolicy::fifo: return "fifo";
        case TilePolicy::priority_stbn: return "priority_stbn";
        default: return "unknown";
    }
}

inline const char* pacing_owner_name(std::int32_t owner) {
    switch (static_cast<PacingOwner>(owner)) {
        case PacingOwner::app_deadline_pacer: return "app_deadline_pacer";
        case PacingOwner::dxgi_vsync: return "dxgi_vsync";
        case PacingOwner::unlocked: return "unlocked";
        case PacingOwner::streamline_reflex_dlssg: return "streamline_reflex_dlssg";
        default: return "unknown";
    }
}

inline const char* runtime_event_name(std::int32_t event_type) {
    switch (static_cast<RuntimeEventType>(event_type)) {
        case RuntimeEventType::runtime_started: return "runtime_started";
        case RuntimeEventType::swap_chain_resized: return "swap_chain_resized";
        case RuntimeEventType::runtime_stopping: return "runtime_stopping";
        case RuntimeEventType::scene_paused: return "scene_paused";
        case RuntimeEventType::scene_resumed: return "scene_resumed";
        case RuntimeEventType::scene_reset: return "scene_reset";
        case RuntimeEventType::config_updated: return "config_updated";
        case RuntimeEventType::reprojection_fallback: return "reprojection_fallback";
        case RuntimeEventType::scheduler_decision: return "scheduler_decision";
        default: return "unknown";
    }
}

inline const char* scheduler_reason_name(std::int32_t reason) {
    switch (static_cast<SchedulerReason>(reason)) {
        case SchedulerReason::manual_override: return "manual_override";
        case SchedulerReason::headroom_ok: return "headroom_ok";
        case SchedulerReason::gpu_headroom_pressure: return "gpu_headroom_pressure";
        case SchedulerReason::deadline_overrun: return "deadline_overrun";
        case SchedulerReason::headroom_recovered: return "headroom_recovered";
        default: return "unknown";
    }
}

inline const char* pacer_mode_name(std::int32_t mode) {
    switch (static_cast<PacerMode>(mode)) {
        case PacerMode::deadline: return "deadline";
        case PacerMode::vsync_only: return "vsync_only";
        case PacerMode::unlocked: return "unlocked";
        default: return "unknown";
    }
}

inline const char* presentation_mode_name(std::int32_t mode) {
    switch (static_cast<PresentationMode>(mode)) {
        case PresentationMode::full_render: return "full_render";
        case PresentationMode::repeat: return "repeat";
        case PresentationMode::reproject: return "reproject";
        case PresentationMode::frame_gen: return "frame_gen";
        case PresentationMode::combined: return "combined";
        case PresentationMode::dlss_g: return "dlss_g";
        case PresentationMode::dlss_g_warp: return "dlss_g_warp";
        default: return "unknown";
    }
}

inline const char* frame_kind_name(std::int32_t kind) {
    switch (static_cast<FrameKind>(kind)) {
        case FrameKind::rendered: return "rendered";
        case FrameKind::repeated: return "repeated";
        case FrameKind::warped: return "warped";
        case FrameKind::fallback: return "fallback";
        case FrameKind::generated: return "generated";
        case FrameKind::combined: return "combined";
        case FrameKind::dlss_generated: return "dlss_generated";
        case FrameKind::dlss_warped: return "dlss_warped";
        default: return "unknown";
    }
}

inline const char* fallback_reason_name(std::int32_t reason) {
    switch (static_cast<FallbackReason>(reason)) {
        case FallbackReason::none: return "none";
        case FallbackReason::camera_cut: return "camera_cut";
        case FallbackReason::stale_source: return "stale_source";
        case FallbackReason::no_source_frame: return "no_source_frame";
        default: return "unknown";
    }
}

inline const char* vrs_mode_name(std::int32_t mode) {
    switch (static_cast<VrsMode>(mode)) {
        case VrsMode::off: return "off";
        case VrsMode::uniform_2x2: return "uniform_2x2";
        case VrsMode::foveated: return "foveated";
        default: return "unknown";
    }
}

inline const char* diagnostic_view_name(std::int32_t view) {
    switch (static_cast<DiagnosticView>(view)) {
        case DiagnosticView::final_color: return "final";
        case DiagnosticView::depth: return "depth";
        case DiagnosticView::motion_vectors: return "motion";
        case DiagnosticView::reprojection_holes: return "holes";
        case DiagnosticView::vrs_mask: return "vrs";
        case DiagnosticView::tile_cache: return "tiles";
        case DiagnosticView::tile_priority: return "priority";
        default: return "unknown";
    }
}

inline const char* present_variant_name(std::int32_t variant) {
    switch (static_cast<PresentVariant>(variant)) {
        case PresentVariant::simple: return "simple";
        case PresentVariant::rotation_homography: return "rotation_homography";
        case PresentVariant::depth_fast: return "depth_fast";
        case PresentVariant::depth_full: return "depth_full";
        case PresentVariant::synthesis: return "synthesis";
        default: return "unknown";
    }
}

inline const char* camera_path_name(std::int32_t path) {
    switch (static_cast<CameraPath>(path)) {
        case CameraPath::corridor_sway: return "corridor_sway";
        case CameraPath::fast_pan: return "fast_pan";
        case CameraPath::occlusion_sweep: return "occlusion_sweep";
        case CameraPath::camera_cut: return "camera_cut";
        case CameraPath::manual: return "manual";
        case CameraPath::pure_rotation: return "pure_rotation";
        default: return "unknown";
    }
}

inline std::wstring runtime_mapping_name(DWORD process_id) {
    return L"Local\\FrameWarpLab.State." + std::to_wstring(process_id);
}

}
