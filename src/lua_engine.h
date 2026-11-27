#pragma once

#include "ecs.h"
#include "event_bus.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace frame_warp_lab {

struct LuaRaycastResult {
    bool hit = false;
    float dist = 0.0f;
    Float3 point{0.0f, 0.0f, 0.0f};
    Float3 normal{0.0f, 1.0f, 0.0f};
};

struct LuaCapsuleSweepResult {
    bool hit = false;
    float fraction = 1.0f;
    Float3 point{0.0f, 0.0f, 0.0f};
    Float3 normal{0.0f, 1.0f, 0.0f};
};

struct LuaOverlapResult {
    bool penetrated = false;
    Float3 push{0.0f, 0.0f, 0.0f};
};

struct LuaHostCallbacks {
    std::function<void(EntityHandle, Float3, Float3)> apply_target_impulse;
    std::function<void(EntityHandle, bool)> set_target_motor_upright;
    std::function<void(EntityHandle)> reset_target_pose;
    std::function<void(EntityHandle, Float3, Float3, Float3)> launch_casing;
    std::function<void(EntityHandle)> deactivate_casing;
    std::function<void(bool)> trigger_hit_marker;
    std::function<void(std::int32_t, bool)> add_score;
    std::function<void()> reset_score;
    std::function<LuaRaycastResult(Float3, Float3, float)> raycast_world;
    std::function<LuaCapsuleSweepResult(Float3, Float3, float, float)> sweep_capsule;
    std::function<LuaOverlapResult(Float3, float, float)> resolve_capsule_overlap;
    std::function<std::vector<Float3>()> get_waypoints;
    std::function<bool()> is_dust2_active;
    std::function<void(std::int32_t)> record_bot_kill;
};

struct LuaEngineStats {
    std::uint32_t loaded_scripts = 0;
    std::uint32_t reload_count = 0;
    std::uint32_t error_count = 0;
    std::int64_t last_step_us = 0;
    std::string last_error;
};

class LuaScriptEngine {
public:
    LuaScriptEngine();
    ~LuaScriptEngine();

    LuaScriptEngine(const LuaScriptEngine&) = delete;
    LuaScriptEngine& operator=(const LuaScriptEngine&) = delete;

    bool initialize(EcsWorld* world, EventBus* bus, LuaHostCallbacks callbacks);
    bool reload_all();
    void step(double delta_seconds);

    LuaEngineStats stats() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}
