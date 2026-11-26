#pragma once

#include "ecs.h"
#include "event_bus.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace frame_warp_lab {

struct LuaHostCallbacks {
    std::function<void(EntityHandle, Float3, Float3)> apply_target_impulse;
    std::function<void(EntityHandle, bool)> set_target_motor_upright;
    std::function<void(EntityHandle)> reset_target_pose;
    std::function<void(EntityHandle, Float3, Float3, Float3)> launch_casing;
    std::function<void(EntityHandle)> deactivate_casing;
    std::function<void(bool)> trigger_hit_marker;
    std::function<void(std::int32_t, bool)> add_score;
    std::function<void()> reset_score;
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
