#pragma once

#include "runtime_state.h"

#include <algorithm>
#include <cstdint>

namespace frame_warp_lab {

struct AdaptivePolicyTarget {
    PresentationMode mode;
    std::int32_t divider;
    VrsMode vrs;
    bool async_gpu;
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

    static AdaptivePolicyTarget policy_for_tier(std::int32_t tier, std::int32_t vrs_tier) {
        const VrsMode foveated_or_tier1 = vrs_tier >= 2
            ? VrsMode::foveated
            : (vrs_tier >= 1 ? VrsMode::uniform_2x2 : VrsMode::off);
        const VrsMode coarse_vrs = vrs_tier >= 1 ? VrsMode::uniform_2x2 : VrsMode::off;
        switch (std::clamp(tier, 0, max_tier)) {
            case 0: return AdaptivePolicyTarget{PresentationMode::full_render, 1, VrsMode::off, true};
            case 1: return AdaptivePolicyTarget{PresentationMode::full_render, 1, foveated_or_tier1, true};
            case 2: return AdaptivePolicyTarget{PresentationMode::combined, 2, foveated_or_tier1, true};
            case 3: return AdaptivePolicyTarget{PresentationMode::combined, 3, coarse_vrs, true};
            default: return AdaptivePolicyTarget{PresentationMode::combined, 4, coarse_vrs, true};
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

    bool update(
        std::int64_t target_period_us,
        std::int64_t scene_gpu_us,
        std::int64_t present_gpu_us,
        bool missed_deadline,
        bool scene_overran_cadence,
        std::int32_t vrs_tier,
        VrsMode active_vrs_mode,
        std::int32_t current_divider,
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

        const std::int32_t safe_divider = std::max<std::int32_t>(1, current_divider);
        const double effective_budget_us = std::max(1000.0, period_us * static_cast<double>(safe_divider) - dynamic_headroom_us);
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
            out_target = policy_for_tier(tier_, vrs_tier);
            reset_counters();
            settle_frames_remaining_ = 24;
            return true;
        }

        if (tier_ > 0) {
            const AdaptivePolicyTarget lower_policy = policy_for_tier(tier_ - 1, vrs_tier);
            const double current_scale = std::max(0.20, expected_vrs_cost_scale(active_vrs_mode));
            const double lower_scale = expected_vrs_cost_scale(lower_policy.vrs);
            const double estimated_full_rate_us = smoothed_scene_gpu_us_ / current_scale;
            const double estimated_lower_scene_us = estimated_full_rate_us * lower_scale;
            const double lower_budget_us = std::max(1000.0, period_us * static_cast<double>(lower_policy.divider) - dynamic_headroom_us);
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

}
