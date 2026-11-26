#pragma once

#include "runtime_state.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace frame_warp_lab {

struct AdaptivePolicyTarget {
    PresentationMode mode;
    std::int32_t divider;
    VrsMode vrs;
    bool async_gpu;
    std::int32_t dlssg_multiplier;
};

struct AdaptiveTelemetry {
    std::int32_t tier;
    SchedulerReason reason;
    std::int64_t reserved_headroom_us;
    double gpu_budget_utilization;
    std::int64_t decision_count;
};

class AdaptiveScheduler {
public:
    static constexpr std::int32_t max_tier = 4;

    void set_enabled(bool enabled) {
        if (enabled_ == enabled) return;
        enabled_ = enabled;
        reset_counters();
        settle_frames_remaining_ = 24;
        if (!enabled_) {
            reason_ = SchedulerReason::manual_override;
        } else {
            reason_ = SchedulerReason::headroom_ok;
        }
    }

    bool enabled() const { return enabled_; }
    std::int32_t tier() const { return tier_; }
    SchedulerReason reason() const { return reason_; }
    std::int64_t reserved_headroom_us() const { return reserved_headroom_us_; }
    double gpu_budget_utilization() const { return gpu_budget_utilization_; }
    std::int64_t decision_count() const { return decision_count_; }

    void reset() {
        tier_ = 0;
        reason_ = enabled_ ? SchedulerReason::headroom_ok : SchedulerReason::manual_override;
        smoothed_scene_gpu_us_ = 0.0;
        smoothed_present_gpu_us_ = 100.0;
        has_gpu_sample_ = false;
        gpu_budget_utilization_ = 0.0;
        reset_counters();
    }

    static AdaptivePolicyTarget policy_for_tier(std::int32_t tier, std::int32_t vrs_tier, bool use_dlssg_policy = false) {
        const VrsMode foveated_or_tier1 = vrs_tier >= 2
            ? VrsMode::foveated
            : (vrs_tier >= 1 ? VrsMode::uniform_2x2 : VrsMode::off);
        const VrsMode coarse_vrs = vrs_tier >= 1 ? VrsMode::uniform_2x2 : VrsMode::off;
        const std::int32_t clamped = std::clamp(tier, 0, max_tier);
        if (use_dlssg_policy) {
            switch (clamped) {
                case 0: return AdaptivePolicyTarget{PresentationMode::full_render, 1, VrsMode::off, true, 2};
                case 1: return AdaptivePolicyTarget{PresentationMode::full_render, 1, foveated_or_tier1, true, 2};
                case 2: return AdaptivePolicyTarget{PresentationMode::dlss_g, 1, foveated_or_tier1, true, 2};
                case 3: return AdaptivePolicyTarget{PresentationMode::dlss_g_warp, 2, foveated_or_tier1, true, 2};
                default: return AdaptivePolicyTarget{PresentationMode::dlss_g_warp, 2, coarse_vrs, true, 3};
            }
        }
        switch (clamped) {
            case 0: return AdaptivePolicyTarget{PresentationMode::full_render, 1, VrsMode::off, true, 2};
            case 1: return AdaptivePolicyTarget{PresentationMode::full_render, 1, foveated_or_tier1, true, 2};
            case 2: return AdaptivePolicyTarget{PresentationMode::combined, 2, foveated_or_tier1, true, 2};
            case 3: return AdaptivePolicyTarget{PresentationMode::combined, 3, coarse_vrs, true, 2};
            default: return AdaptivePolicyTarget{PresentationMode::combined, 4, coarse_vrs, true, 2};
        }
    }

    static double expected_vrs_cost_scale(VrsMode mode) {
        switch (mode) {
            case VrsMode::off: return 1.0;
            case VrsMode::foveated: return 0.38;
            case VrsMode::uniform_2x2: return 0.28;
            default: return 1.0;
        }
    }

    static double effective_cadence_multiplier(PresentationMode mode, std::int32_t divider, std::int32_t dlssg_mult) {
        const double safe_div = static_cast<double>(std::max<std::int32_t>(1, divider));
        if (mode == PresentationMode::dlss_g || mode == PresentationMode::dlss_g_warp) {
            const double fg_mult = static_cast<double>(std::max<std::int32_t>(2, dlssg_mult));
            return safe_div * fg_mult;
        }
        return safe_div;
    }

    bool update(
        std::int64_t target_period_us,
        std::int64_t scene_gpu_us,
        std::int64_t present_gpu_us,
        bool missed_deadline,
        bool scene_overran_cadence,
        std::int32_t vrs_tier,
        VrsMode active_vrs_mode,
        PresentationMode current_mode,
        std::int32_t current_divider,
        std::int32_t current_dlssg_mult,
        bool use_dlssg_policy,
        AdaptivePolicyTarget& out_target) {
        const double period_us = static_cast<double>(std::max<std::int64_t>(target_period_us, 1000LL));
        if (present_gpu_us >= 0) {
            smoothed_present_gpu_us_ = smoothed_present_gpu_us_ * 0.85 + static_cast<double>(present_gpu_us) * 0.15;
        }
        if (scene_gpu_us >= 0) {
            if (!has_gpu_sample_) {
                smoothed_scene_gpu_us_ = static_cast<double>(scene_gpu_us);
                has_gpu_sample_ = true;
            } else {
                smoothed_scene_gpu_us_ = smoothed_scene_gpu_us_ * 0.80 + static_cast<double>(scene_gpu_us) * 0.20;
            }
        }

        const double dynamic_headroom_us = std::max(period_us * 0.25, smoothed_present_gpu_us_ * 2.5 + 800.0);
        reserved_headroom_us_ = static_cast<std::int64_t>(dynamic_headroom_us);

        const double current_cadence = effective_cadence_multiplier(current_mode, current_divider, current_dlssg_mult);
        const double effective_budget_us = std::max(1000.0, period_us * current_cadence - dynamic_headroom_us);
        const double total_gpu_us = smoothed_scene_gpu_us_ + smoothed_present_gpu_us_;
        gpu_budget_utilization_ = has_gpu_sample_ ? (total_gpu_us / effective_budget_us) : 0.0;

        if (!enabled_ || !has_gpu_sample_) {
            return false;
        }

        if (settle_frames_remaining_ > 0) {
            --settle_frames_remaining_;
            return false;
        }

        ++window_frame_counter_;
        if (missed_deadline) ++recent_misses_;
        if (scene_overran_cadence) ++recent_overruns_;
        if (window_frame_counter_ >= 24) {
            window_frame_counter_ = 0;
            recent_misses_ = 0;
            recent_overruns_ = 0;
        }

        const bool over_budget = gpu_budget_utilization_ > 0.98;
        if (over_budget) {
            ++high_pressure_streak_;
        } else {
            high_pressure_streak_ = 0;
        }

        const bool deadline_pressure = (recent_misses_ >= 3 && gpu_budget_utilization_ >= 0.45) || recent_overruns_ >= 3;
        if (tier_ < max_tier && (deadline_pressure || high_pressure_streak_ >= 12)) {
            ++tier_;
            reason_ = deadline_pressure ? SchedulerReason::deadline_overrun : SchedulerReason::gpu_headroom_pressure;
            ++decision_count_;
            out_target = policy_for_tier(tier_, vrs_tier, use_dlssg_policy);
            reset_counters();
            settle_frames_remaining_ = 24;
            return true;
        }

        if (tier_ > 0) {
            const AdaptivePolicyTarget lower_policy = policy_for_tier(tier_ - 1, vrs_tier, use_dlssg_policy);
            const double current_scale = std::max(0.20, expected_vrs_cost_scale(active_vrs_mode));
            const double lower_scale = expected_vrs_cost_scale(lower_policy.vrs);
            const double estimated_full_rate_us = smoothed_scene_gpu_us_ / current_scale;
            const double estimated_lower_scene_us = estimated_full_rate_us * lower_scale;
            const double lower_cadence = effective_cadence_multiplier(lower_policy.mode, lower_policy.divider, lower_policy.dlssg_multiplier);
            const double lower_budget_us = std::max(1000.0, period_us * lower_cadence - dynamic_headroom_us);
            const double lower_utilization = (estimated_lower_scene_us + smoothed_present_gpu_us_) / lower_budget_us;
            if (lower_utilization < 0.68 && recent_misses_ == 0 && recent_overruns_ == 0) {
                ++low_pressure_streak_;
            } else {
                low_pressure_streak_ = 0;
            }
            const std::int32_t hold_frames = std::max<std::int32_t>(72, static_cast<std::int32_t>(750000.0 / period_us));
            if (low_pressure_streak_ >= hold_frames) {
                --tier_;
                reason_ = tier_ == 0 ? SchedulerReason::headroom_ok : SchedulerReason::headroom_recovered;
                ++decision_count_;
                out_target = lower_policy;
                reset_counters();
                settle_frames_remaining_ = 24;
                return true;
            }
        } else {
            reason_ = SchedulerReason::headroom_ok;
        }

        return false;
    }

    AdaptiveTelemetry telemetry() const {
        return AdaptiveTelemetry{
            tier_,
            reason_,
            reserved_headroom_us_,
            gpu_budget_utilization_,
            decision_count_,
        };
    }

private:
    void reset_counters() {
        high_pressure_streak_ = 0;
        low_pressure_streak_ = 0;
        recent_misses_ = 0;
        recent_overruns_ = 0;
        window_frame_counter_ = 0;
    }

    bool enabled_ = false;
    bool has_gpu_sample_ = false;
    std::int32_t tier_ = 0;
    SchedulerReason reason_ = SchedulerReason::manual_override;
    double smoothed_scene_gpu_us_ = 0.0;
    double smoothed_present_gpu_us_ = 100.0;
    std::int64_t reserved_headroom_us_ = 2083;
    double gpu_budget_utilization_ = 0.0;
    std::int64_t decision_count_ = 0;
    std::int32_t settle_frames_remaining_ = 0;
    std::int32_t high_pressure_streak_ = 0;
    std::int32_t low_pressure_streak_ = 0;
    std::int32_t recent_misses_ = 0;
    std::int32_t recent_overruns_ = 0;
    std::int32_t window_frame_counter_ = 0;
};

class AutoTileBudgetController {
public:
    void reset() {
        smoothed_scene_us_ = 0.0;
        smoothed_present_us_ = 80.0;
        smoothed_damage_pct_ = 0.0;
        has_sample_ = false;
        last_source_frame_id_ = -1;
        last_target_scene_budget_us_ = 0;
        stable_low_streak_ = 0;
        bypass_low_streak_ = 0;
        cut_burst_frames_ = 0;
        par_bypass_active_ = false;
    }

    std::int64_t last_target_scene_budget_us() const { return last_target_scene_budget_us_; }
    bool par_bypass_active() const { return par_bypass_active_; }
    std::int32_t smoothed_damage_pct() const { return static_cast<std::int32_t>(std::lround(smoothed_damage_pct_)); }

    bool update(
        std::int64_t target_period_us,
        std::int32_t effective_divider,
        std::int64_t source_frame_id,
        std::int64_t scene_gpu_us,
        std::int64_t present_gpu_us,
        std::uint32_t total_tiles,
        std::uint32_t shaded_tiles,
        std::uint32_t critical_total_tiles,
        std::uint32_t critical_deferred_tiles,
        bool discontinuity,
        bool missed_deadline,
        std::int32_t& in_out_budget_pct) {
        const double period_us = static_cast<double>(std::max<std::int64_t>(target_period_us, 1000LL));
        const double cadence = static_cast<double>(std::max<std::int32_t>(1, effective_divider));
        if (present_gpu_us >= 0) {
            smoothed_present_us_ = smoothed_present_us_ * 0.85 + static_cast<double>(present_gpu_us) * 0.15;
        }
        const double headroom_us = std::max(period_us * 0.25, smoothed_present_us_ * 2.5 + 750.0);
        const double target_scene_us = std::max(450.0, period_us * cadence - smoothed_present_us_ - headroom_us);
        last_target_scene_budget_us_ = static_cast<std::int64_t>(target_scene_us);

        if (discontinuity) {
            cut_burst_frames_ = 4;
        }
        if (scene_gpu_us < 0 || source_frame_id == last_source_frame_id_) {
            return false;
        }
        last_source_frame_id_ = source_frame_id;
        if (!has_sample_) {
            smoothed_scene_us_ = static_cast<double>(scene_gpu_us);
            has_sample_ = true;
        } else {
            smoothed_scene_us_ = smoothed_scene_us_ * 0.65 + static_cast<double>(scene_gpu_us) * 0.35;
        }

        if (par_bypass_active_) {
            if (static_cast<double>(scene_gpu_us) > target_scene_us * 0.55 || smoothed_scene_us_ > target_scene_us * 0.45 || missed_deadline) {
                par_bypass_active_ = false;
                bypass_low_streak_ = 0;
                const double scale = (target_scene_us * 0.72) / std::max(static_cast<double>(scene_gpu_us), 1.0);
                in_out_budget_pct = std::clamp(static_cast<std::int32_t>(100.0 * scale), 10, 100);
                return true;
            }
            return false;
        }

        if (total_tiles == 0U) {
            return false;
        }

        const double instant_damage_pct = std::clamp(
            (100.0 * static_cast<double>(critical_total_tiles)) / static_cast<double>(total_tiles),
            0.0,
            100.0);
        smoothed_damage_pct_ = smoothed_damage_pct_ * 0.70 + instant_damage_pct * 0.30;

        const double actual_shaded_pct = std::clamp(
            (100.0 * static_cast<double>(shaded_tiles)) / static_cast<double>(total_tiles),
            5.0,
            100.0);
        const std::int32_t previous_budget = in_out_budget_pct;

        if (smoothed_scene_us_ > target_scene_us * 0.90 || (missed_deadline && smoothed_scene_us_ > target_scene_us * 0.70)) {
            stable_low_streak_ = 0;
            bypass_low_streak_ = 0;
            const double scale = (target_scene_us * 0.78) / std::max(smoothed_scene_us_, 1.0);
            const double anchor_pct = std::min(static_cast<double>(in_out_budget_pct), std::max(actual_shaded_pct, 8.0));
            const auto tightened = std::clamp(static_cast<std::int32_t>(anchor_pct * scale), 5, 100);
            in_out_budget_pct = (tightened < in_out_budget_pct) ? tightened : std::max(5, in_out_budget_pct - 2);
            return in_out_budget_pct != previous_budget;
        }

        const std::int32_t damage_floor = std::clamp(static_cast<std::int32_t>(std::ceil(smoothed_damage_pct_)) + 2, 5, 100);
        if (cut_burst_frames_ > 0) {
            --cut_burst_frames_;
            const std::int32_t burst_floor = std::max(damage_floor, 25);
            if (in_out_budget_pct < burst_floor && smoothed_scene_us_ < target_scene_us * 0.85) {
                in_out_budget_pct = burst_floor;
                return in_out_budget_pct != previous_budget;
            }
        }

        if (critical_deferred_tiles > 0U && in_out_budget_pct < damage_floor && smoothed_scene_us_ < target_scene_us * 0.82) {
            in_out_budget_pct = std::min(100, std::max(in_out_budget_pct + 4, damage_floor));
            stable_low_streak_ = 0;
            bypass_low_streak_ = 0;
            return in_out_budget_pct != previous_budget;
        }

        if (smoothed_scene_us_ < target_scene_us * 0.72 && in_out_budget_pct < 100) {
            ++stable_low_streak_;
            bypass_low_streak_ = 0;
            if (stable_low_streak_ >= 6) {
                const std::int32_t step = smoothed_scene_us_ < target_scene_us * 0.48 ? 5 : 2;
                in_out_budget_pct = std::min(100, in_out_budget_pct + step);
                stable_low_streak_ = 0;
                return in_out_budget_pct != previous_budget;
            }
        } else if (in_out_budget_pct >= 100 && smoothed_scene_us_ < target_scene_us * 0.25 && static_cast<double>(scene_gpu_us) < 1500.0) {
            ++bypass_low_streak_;
            if (bypass_low_streak_ >= 14) {
                par_bypass_active_ = true;
                bypass_low_streak_ = 0;
                return true;
            }
        } else {
            stable_low_streak_ = 0;
            bypass_low_streak_ = 0;
        }
        return false;
    }

private:
    double smoothed_scene_us_ = 0.0;
    double smoothed_present_us_ = 80.0;
    double smoothed_damage_pct_ = 0.0;
    bool has_sample_ = false;
    std::int64_t last_source_frame_id_ = -1;
    std::int64_t last_target_scene_budget_us_ = 0;
    std::int32_t stable_low_streak_ = 0;
    std::int32_t bypass_low_streak_ = 0;
    std::int32_t cut_burst_frames_ = 0;
    bool par_bypass_active_ = false;
};

class AutoDrsController {
public:
    void reset() {
        smoothed_scene_us_ = 0.0;
        smoothed_present_us_ = 80.0;
        has_sample_ = false;
        last_source_frame_id_ = -1;
        last_target_scene_budget_us_ = 0;
        stable_low_streak_ = 0;
        sweep_direction_ = -1;
    }

    std::int64_t last_target_scene_budget_us() const { return last_target_scene_budget_us_; }

    bool update_sweep(std::int32_t min_pct, std::int32_t max_pct, std::int32_t& in_out_scale_pct) {
        const std::int32_t lo = std::clamp(min_pct, 25, 200);
        const std::int32_t hi = std::clamp(std::max(lo, max_pct), 25, 200);
        if (lo == hi) {
            const bool changed = in_out_scale_pct != lo;
            in_out_scale_pct = lo;
            return changed;
        }
        const std::int32_t previous = in_out_scale_pct;
        std::int32_t next = previous + sweep_direction_ * 5;
        if (next <= lo) {
            next = lo;
            sweep_direction_ = 1;
        } else if (next >= hi) {
            next = hi;
            sweep_direction_ = -1;
        }
        in_out_scale_pct = next;
        return in_out_scale_pct != previous;
    }

    bool update_deadline(
        std::int64_t target_period_us,
        std::int32_t effective_divider,
        std::int64_t source_frame_id,
        std::int64_t scene_gpu_us,
        std::int64_t present_gpu_us,
        bool missed_deadline,
        std::int32_t min_pct,
        std::int32_t max_pct,
        std::int32_t& in_out_scale_pct) {
        const double period_us = static_cast<double>(std::max<std::int64_t>(target_period_us, 1000LL));
        const double cadence = static_cast<double>(std::max<std::int32_t>(1, effective_divider));
        if (present_gpu_us >= 0) {
            smoothed_present_us_ = smoothed_present_us_ * 0.85 + static_cast<double>(present_gpu_us) * 0.15;
        }
        const double headroom_us = std::max(period_us * 0.22, smoothed_present_us_ * 2.5 + 650.0);
        const double target_scene_us = std::max(450.0, period_us * cadence - smoothed_present_us_ - headroom_us);
        last_target_scene_budget_us_ = static_cast<std::int64_t>(target_scene_us);

        const std::int32_t lo = std::clamp(min_pct, 25, 200);
        const std::int32_t hi = std::clamp(std::max(lo, max_pct), 25, 200);
        if (scene_gpu_us < 0 || source_frame_id == last_source_frame_id_) {
            return false;
        }
        last_source_frame_id_ = source_frame_id;
        if (!has_sample_) {
            smoothed_scene_us_ = static_cast<double>(scene_gpu_us);
            has_sample_ = true;
        } else {
            smoothed_scene_us_ = smoothed_scene_us_ * 0.60 + static_cast<double>(scene_gpu_us) * 0.40;
        }

        const std::int32_t previous = in_out_scale_pct;
        if (smoothed_scene_us_ > target_scene_us * 0.90 || (missed_deadline && smoothed_scene_us_ > target_scene_us * 0.72)) {
            stable_low_streak_ = 0;
            const double ratio = (target_scene_us * 0.82) / std::max(smoothed_scene_us_, 1.0);
            const double linear_factor = std::sqrt(std::clamp(ratio, 0.25, 1.0));
            const auto target_scale = std::clamp(
                static_cast<std::int32_t>(std::floor(static_cast<double>(previous) * linear_factor)),
                lo,
                hi);
            const std::int32_t max_drop = std::max(lo, previous - 10);
            const std::int32_t stepped = std::max(target_scale, max_drop);
            in_out_scale_pct = (stepped < previous) ? stepped : std::max(lo, previous - 2);
            return in_out_scale_pct != previous;
        }

        if (smoothed_scene_us_ < target_scene_us * 0.74 && previous < hi) {
            ++stable_low_streak_;
            if (stable_low_streak_ >= 6) {
                const std::int32_t step = smoothed_scene_us_ < target_scene_us * 0.48 ? 4 : 2;
                in_out_scale_pct = std::min(hi, previous + step);
                stable_low_streak_ = 0;
                return in_out_scale_pct != previous;
            }
        } else {
            stable_low_streak_ = 0;
        }
        return false;
    }

private:
    double smoothed_scene_us_ = 0.0;
    double smoothed_present_us_ = 80.0;
    bool has_sample_ = false;
    std::int64_t last_source_frame_id_ = -1;
    std::int64_t last_target_scene_budget_us_ = 0;
    std::int32_t stable_low_streak_ = 0;
    std::int32_t sweep_direction_ = -1;
};

struct UnifiedQualityProfile {
    bool selective_tiles;
    bool split_diff_spec;
    bool surface_cache;
    bool subtile_interleave;
    bool cache_sharpen;
    std::int32_t tile_budget_pct;
    std::int32_t render_scale_pct;
    VrsMode vrs;
    PresentationMode mode;
    std::int32_t divider;
};

struct UnifiedControllerTelemetry {
    bool enabled = false;
    std::int32_t tier = 0;
    std::int32_t utilization_permille = 0;
    std::int64_t target_budget_us = 0;
    std::int64_t transitions = 0;
    std::int64_t oscillations = 0;
};

class UnifiedQualityController {
public:
    static constexpr std::int32_t max_tier = 6;

    void set_enabled(bool enabled) {
        if (enabled_ == enabled) return;
        enabled_ = enabled;
        reset_dynamics();
        if (enabled_) {
            tier_ = 0;
            force_apply_ = true;
        }
    }

    bool enabled() const { return enabled_; }
    std::int32_t tier() const { return tier_; }

    void reset() {
        tier_ = 0;
        transitions_ = 0;
        oscillations_ = 0;
        reset_dynamics();
        if (enabled_) {
            force_apply_ = true;
        }
    }

    static UnifiedQualityProfile profile_for_tier(std::int32_t tier) {
        switch (std::clamp(tier, 0, max_tier)) {
            case 0:
                return UnifiedQualityProfile{true, true, true, true, true, 25, 100, VrsMode::off, PresentationMode::full_render, 1};
            case 1:
                return UnifiedQualityProfile{true, true, true, true, true, 15, 100, VrsMode::off, PresentationMode::full_render, 1};
            case 2:
                return UnifiedQualityProfile{true, false, true, true, true, 22, 100, VrsMode::off, PresentationMode::full_render, 1};
            case 3:
                return UnifiedQualityProfile{true, false, true, true, true, 15, 100, VrsMode::off, PresentationMode::full_render, 1};
            case 4:
                return UnifiedQualityProfile{true, false, true, true, true, 12, 85, VrsMode::off, PresentationMode::full_render, 1};
            case 5:
                return UnifiedQualityProfile{true, false, true, true, true, 10, 72, VrsMode::off, PresentationMode::full_render, 1};
            default:
                return UnifiedQualityProfile{true, false, true, true, true, 10, 62, VrsMode::off, PresentationMode::combined, 2};
        }
    }

    static const char* profile_name(std::int32_t tier) {
        switch (std::clamp(tier, 0, max_tier)) {
            case 0: return "T0_SplitSurf_25p_100s";
            case 1: return "T1_SplitSurf_15p_100s";
            case 2: return "T2_UnifSurf_22p_100s";
            case 3: return "T3_UnifSurf_15p_100s";
            case 4: return "T4_UnifSurf_12p_85s";
            case 5: return "T5_UnifSurf_10p_72s";
            default: return "T6_Emergency_10p_62s_Div2";
        }
    }

    UnifiedControllerTelemetry telemetry() const {
        return UnifiedControllerTelemetry{
            enabled_,
            tier_,
            utilization_permille_,
            target_budget_us_,
            transitions_,
            oscillations_,
        };
    }

    bool update(
        std::int64_t target_period_us,
        std::int64_t source_frame_id,
        std::int64_t scene_gpu_us,
        std::int64_t present_gpu_us,
        bool missed_deadline,
        UnifiedQualityProfile& out_profile) {
        if (!enabled_) return false;

        const double period_us = static_cast<double>(std::max<std::int64_t>(target_period_us, 1000LL));
        if (present_gpu_us >= 0) {
            smoothed_present_us_ = smoothed_present_us_ * 0.85 + static_cast<double>(present_gpu_us) * 0.15;
        }
        const double cadence = static_cast<double>(profile_for_tier(tier_).divider);
        const double headroom_us = std::max(period_us * 0.20, smoothed_present_us_ * 2.2 + 450.0);
        const double budget_us = std::max(500.0, period_us * cadence - smoothed_present_us_ - headroom_us);
        target_budget_us_ = static_cast<std::int64_t>(budget_us);

        if (force_apply_) {
            force_apply_ = false;
            out_profile = profile_for_tier(tier_);
            cooldown_frames_ = 8;
            return true;
        }

        if (scene_gpu_us < 0 || source_frame_id == last_source_frame_id_) {
            return false;
        }
        last_source_frame_id_ = source_frame_id;
        ++frame_counter_;

        if (!has_sample_) {
            smoothed_scene_us_ = static_cast<double>(scene_gpu_us);
            has_sample_ = true;
        } else {
            smoothed_scene_us_ = smoothed_scene_us_ * 0.65 + static_cast<double>(scene_gpu_us) * 0.35;
        }

        const double util = smoothed_scene_us_ / std::max(budget_us, 1.0);
        utilization_permille_ = std::clamp(static_cast<std::int32_t>(std::lround(util * 1000.0)), 0, 9999);

        if (cooldown_frames_ > 0) {
            --cooldown_frames_;
            return false;
        }

        const bool over_budget = (util > 0.88) || (missed_deadline && util > 0.68);
        const bool severe_over = (util > 1.12) || (missed_deadline && util > 0.90);
        if (over_budget && tier_ < max_tier) {
            ++high_streak_;
            low_streak_ = 0;
            if (severe_over || high_streak_ >= 2) {
                const std::int32_t step = (util > 1.35 && tier_ + 2 <= max_tier) ? 2 : 1;
                record_transition(+1);
                tier_ = std::min(max_tier, tier_ + step);
                high_streak_ = 0;
                cooldown_frames_ = 3;
                recover_hold_frames_ = 48;
                out_profile = profile_for_tier(tier_);
                return true;
            }
            return false;
        }
        high_streak_ = 0;

        if (recover_hold_frames_ > 0) {
            --recover_hold_frames_;
            low_streak_ = 0;
            return false;
        }

        if (util < 0.56 && tier_ > 0) {
            ++low_streak_;
            if (low_streak_ >= 24) {
                record_transition(-1);
                tier_ = std::max(0, tier_ - 1);
                low_streak_ = 0;
                cooldown_frames_ = 12;
                out_profile = profile_for_tier(tier_);
                return true;
            }
        } else {
            low_streak_ = 0;
        }
        return false;
    }

private:
    void reset_dynamics() {
        smoothed_scene_us_ = 0.0;
        smoothed_present_us_ = 80.0;
        has_sample_ = false;
        last_source_frame_id_ = -1;
        high_streak_ = 0;
        low_streak_ = 0;
        cooldown_frames_ = 0;
        recover_hold_frames_ = 0;
        last_dir_ = 0;
        last_transition_frame_ = -1000;
        frame_counter_ = 0;
    }

    void record_transition(std::int32_t dir) {
        ++transitions_;
        if (last_dir_ != 0 && dir != last_dir_ && (frame_counter_ - last_transition_frame_) <= 45) {
            ++oscillations_;
        }
        last_dir_ = dir;
        last_transition_frame_ = frame_counter_;
    }

    bool enabled_ = false;
    bool force_apply_ = false;
    std::int32_t tier_ = 0;
    double smoothed_scene_us_ = 0.0;
    double smoothed_present_us_ = 80.0;
    bool has_sample_ = false;
    std::int64_t last_source_frame_id_ = -1;
    std::int64_t target_budget_us_ = 0;
    std::int32_t utilization_permille_ = 0;
    std::int64_t transitions_ = 0;
    std::int64_t oscillations_ = 0;
    std::int32_t high_streak_ = 0;
    std::int32_t low_streak_ = 0;
    std::int32_t cooldown_frames_ = 0;
    std::int32_t recover_hold_frames_ = 0;
    std::int32_t last_dir_ = 0;
    std::int64_t last_transition_frame_ = -1000;
    std::int64_t frame_counter_ = 0;
};

}
