#include "lua_engine.h"
#include "gltf_loader.h"

extern "C" {
#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>
}

#include <chrono>
#include <unordered_map>
#include <vector>

namespace frame_warp_lab {

namespace {

constexpr std::uint32_t k_lua_event_owner_tag = 0x4C554131U;
constexpr const char* k_host_registry_key = "frame_warp_lab_lua_host";

}

struct LuaScriptEngine::Impl {
    EcsWorld* world = nullptr;
    EventBus* bus = nullptr;
    LuaHostCallbacks callbacks{};
    lua_State* L = nullptr;

    struct LoadedModule {
        int table_ref = LUA_NOREF;
        int init_ref = LUA_NOREF;
        int update_ref = LUA_NOREF;
    };

    std::unordered_map<std::string, LoadedModule> modules;
    std::vector<int> event_callback_refs;

    std::uint32_t reload_count = 0;
    std::uint32_t error_count = 0;
    std::int64_t last_step_us = 0;
    std::string last_error;

    ~Impl() {
        shutdown_vm();
    }

    void record_error(const char* msg) {
        ++error_count;
        last_error = msg != nullptr ? std::string(msg) : "unknown lua error";
    }

    static Impl* get_impl(lua_State* state) {
        lua_getfield(state, LUA_REGISTRYINDEX, k_host_registry_key);
        auto* self = static_cast<Impl*>(lua_touserdata(state, -1));
        lua_pop(state, 1);
        return self;
    }

    static void push_payload_table(lua_State* state, const EventPayload& payload) {
        lua_createtable(state, 0, static_cast<int>(payload.fields.size()));
        for (const auto& kv : payload.fields) {
            if (const auto* d = std::get_if<double>(&kv.second)) {
                lua_pushnumber(state, *d);
            } else if (const auto* i = std::get_if<std::int64_t>(&kv.second)) {
                lua_pushinteger(state, static_cast<lua_Integer>(*i));
            } else if (const auto* u = std::get_if<std::uint64_t>(&kv.second)) {
                lua_pushinteger(state, static_cast<lua_Integer>(*u));
            } else if (const auto* b = std::get_if<bool>(&kv.second)) {
                lua_pushboolean(state, *b ? 1 : 0);
            } else if (const auto* s = std::get_if<std::string>(&kv.second)) {
                lua_pushlstring(state, s->data(), s->size());
            } else {
                lua_pushnil(state);
            }
            lua_setfield(state, -2, kv.first.c_str());
        }
    }

    static EventPayload read_payload_table(lua_State* state, int table_index) {
        EventPayload payload{};
        if (!lua_istable(state, table_index)) {
            return payload;
        }
        const int abs_idx = lua_absindex(state, table_index);
        lua_pushnil(state);
        while (lua_next(state, abs_idx) != 0) {
            if (lua_type(state, -2) == LUA_TSTRING) {
                const char* key = lua_tostring(state, -2);
                const int vtype = lua_type(state, -1);
                if (vtype == LUA_TBOOLEAN) {
                    payload.set_bool(key, lua_toboolean(state, -1) != 0);
                } else if (vtype == LUA_TNUMBER) {
                    if (lua_isinteger(state, -1)) {
                        payload.set_int(key, static_cast<std::int64_t>(lua_tointeger(state, -1)));
                    } else {
                        payload.set_number(key, static_cast<double>(lua_tonumber(state, -1)));
                    }
                } else if (vtype == LUA_TSTRING) {
                    std::size_t len = 0;
                    const char* s = lua_tolstring(state, -1, &len);
                    payload.set_string(key, std::string(s, len));
                }
            }
            lua_pop(state, 1);
        }
        return payload;
    }

    static bool read_table_number(lua_State* state, int idx, const char* key, float& out_val) {
        lua_getfield(state, idx, key);
        const bool ok = lua_isnumber(state, -1) != 0;
        if (ok) {
            out_val = static_cast<float>(lua_tonumber(state, -1));
        }
        lua_pop(state, 1);
        return ok;
    }

    static bool read_table_int(lua_State* state, int idx, const char* key, std::int32_t& out_val) {
        lua_getfield(state, idx, key);
        const bool ok = lua_isnumber(state, -1) != 0;
        if (ok) {
            out_val = static_cast<std::int32_t>(lua_tointeger(state, -1));
        }
        lua_pop(state, 1);
        return ok;
    }

    static bool read_table_bool(lua_State* state, int idx, const char* key, bool& out_val) {
        lua_getfield(state, idx, key);
        const bool ok = lua_isboolean(state, -1) != 0;
        if (ok) {
            out_val = lua_toboolean(state, -1) != 0;
        }
        lua_pop(state, 1);
        return ok;
    }

    static bool read_table_string(lua_State* state, int idx, const char* key, std::string& out_val) {
        lua_getfield(state, idx, key);
        const bool ok = lua_isstring(state, -1) != 0;
        if (ok) {
            std::size_t len = 0;
            const char* s = lua_tolstring(state, -1, &len);
            if (s != nullptr) out_val.assign(s, len);
        }
        lua_pop(state, 1);
        return ok;
    }

    static int l_ecs_create(lua_State* state) {
        auto* self = get_impl(state);
        const char* name = luaL_checkstring(state, 1);
        const char* tag = luaL_optstring(state, 2, "");
        const char* uri = luaL_optstring(state, 3, "");
        const EntityHandle h = self->world->create_entity(name, tag, uri);
        lua_pushinteger(state, static_cast<lua_Integer>(h));
        return 1;
    }

    static int l_ecs_destroy(lua_State* state) {
        auto* self = get_impl(state);
        const auto h = static_cast<EntityHandle>(luaL_checkinteger(state, 1));
        lua_pushboolean(state, self->world->destroy_entity(h) ? 1 : 0);
        return 1;
    }

    static int l_ecs_alive(lua_State* state) {
        auto* self = get_impl(state);
        const auto h = static_cast<EntityHandle>(luaL_checkinteger(state, 1));
        lua_pushboolean(state, self->world->is_alive(h) ? 1 : 0);
        return 1;
    }

    static int l_ecs_find(lua_State* state) {
        auto* self = get_impl(state);
        const char* name = luaL_checkstring(state, 1);
        lua_pushinteger(state, static_cast<lua_Integer>(self->world->find_by_name(name)));
        return 1;
    }

    static int l_ecs_find_by_tag(lua_State* state) {
        auto* self = get_impl(state);
        const char* tag = luaL_checkstring(state, 1);
        const auto list = self->world->find_by_tag(tag);
        lua_createtable(state, static_cast<int>(list.size()), 0);
        for (std::size_t i = 0; i < list.size(); ++i) {
            lua_pushinteger(state, static_cast<lua_Integer>(list[i]));
            lua_rawseti(state, -2, static_cast<lua_Integer>(i + 1U));
        }
        return 1;
    }

    static int l_ecs_count(lua_State* state) {
        auto* self = get_impl(state);
        lua_pushinteger(state, static_cast<lua_Integer>(self->world->stats().entity_count));
        return 1;
    }

    static int l_ecs_get_weapon(lua_State* state) {
        auto* self = get_impl(state);
        const auto h = static_cast<EntityHandle>(luaL_checkinteger(state, 1));
        const WeaponComponent* w = self->world->weapons().try_get(h);
        if (w == nullptr) {
            lua_pushnil(state);
            return 1;
        }
        lua_createtable(state, 0, 36);
        auto set_i = [&](const char* k, std::int32_t v) { lua_pushinteger(state, v); lua_setfield(state, -2, k); };
        auto set_f = [&](const char* k, float v) { lua_pushnumber(state, v); lua_setfield(state, -2, k); };
        auto set_b = [&](const char* k, bool v) { lua_pushboolean(state, v ? 1 : 0); lua_setfield(state, -2, k); };
        set_i("weapon_id", w->weapon_id);
        set_i("ammo_in_mag", w->ammo_in_mag);
        set_i("mag_size", w->mag_size);
        set_i("ammo_reserve", w->ammo_reserve);
        set_i("shots_fired", w->shots_fired);
        set_f("fire_cooldown", w->fire_cooldown);
        set_f("fire_interval", w->fire_interval);
        set_b("firing_held", w->firing_held);
        set_b("full_auto", w->full_auto);
        set_i("pellet_count", w->pellet_count);
        set_f("base_damage", w->base_damage);
        set_f("ads_fov_rad", w->ads_fov_rad);
        set_f("scope_alpha", w->scope_alpha);
        set_f("dual_left_kick", w->dual_left_kick);
        set_f("dual_right_kick", w->dual_right_kick);
        set_i("dual_hand_toggle", w->dual_hand_toggle);
        set_b("ads_requested", w->ads_requested);
        set_f("ads_alpha", w->ads_alpha);
        set_b("reloading", w->reloading);
        set_f("reload_timer", w->reload_timer);
        set_f("reload_duration", w->reload_duration);
        set_f("slide_blowback", w->slide_blowback);
        set_f("trigger_pull", w->trigger_pull);
        set_f("hammer_cock", w->hammer_cock);
        set_f("recoil_kick", w->recoil_kick);
        set_f("recoil_pitch", w->recoil_pitch);
        set_f("recoil_yaw", w->recoil_yaw);
        set_f("recoil_roll", w->recoil_roll);
        set_f("crosshair_bloom", w->crosshair_bloom);
        set_f("muzzle_flash_timer", w->muzzle_flash_timer);
        set_f("muzzle_flash_spin", w->muzzle_flash_spin);
        set_f("move_speed", w->move_speed);
        set_f("spread_heat", w->spread_heat);
        set_f("current_spread_rad", w->current_spread_rad);
        set_f("last_shot_offset_rad", w->last_shot_offset_rad);
        set_i("spread_rng_state", w->spread_rng_state);
        return 1;
    }

    static int l_ecs_set_weapon(lua_State* state) {
        auto* self = get_impl(state);
        const auto h = static_cast<EntityHandle>(luaL_checkinteger(state, 1));
        luaL_checktype(state, 2, LUA_TTABLE);
        WeaponComponent* w = self->world->weapons().try_get(h);
        if (w == nullptr) {
            lua_pushboolean(state, 0);
            return 1;
        }
        read_table_int(state, 2, "weapon_id", w->weapon_id);
        read_table_int(state, 2, "ammo_in_mag", w->ammo_in_mag);
        read_table_int(state, 2, "mag_size", w->mag_size);
        read_table_int(state, 2, "ammo_reserve", w->ammo_reserve);
        read_table_int(state, 2, "shots_fired", w->shots_fired);
        read_table_number(state, 2, "fire_cooldown", w->fire_cooldown);
        read_table_number(state, 2, "fire_interval", w->fire_interval);
        read_table_bool(state, 2, "firing_held", w->firing_held);
        read_table_bool(state, 2, "full_auto", w->full_auto);
        read_table_int(state, 2, "pellet_count", w->pellet_count);
        read_table_number(state, 2, "base_damage", w->base_damage);
        read_table_number(state, 2, "ads_fov_rad", w->ads_fov_rad);
        read_table_number(state, 2, "scope_alpha", w->scope_alpha);
        read_table_number(state, 2, "dual_left_kick", w->dual_left_kick);
        read_table_number(state, 2, "dual_right_kick", w->dual_right_kick);
        read_table_int(state, 2, "dual_hand_toggle", w->dual_hand_toggle);
        read_table_bool(state, 2, "ads_requested", w->ads_requested);
        read_table_number(state, 2, "ads_alpha", w->ads_alpha);
        read_table_bool(state, 2, "reloading", w->reloading);
        read_table_number(state, 2, "reload_timer", w->reload_timer);
        read_table_number(state, 2, "reload_duration", w->reload_duration);
        read_table_number(state, 2, "slide_blowback", w->slide_blowback);
        read_table_number(state, 2, "trigger_pull", w->trigger_pull);
        read_table_number(state, 2, "hammer_cock", w->hammer_cock);
        read_table_number(state, 2, "recoil_kick", w->recoil_kick);
        read_table_number(state, 2, "recoil_pitch", w->recoil_pitch);
        read_table_number(state, 2, "recoil_yaw", w->recoil_yaw);
        read_table_number(state, 2, "recoil_roll", w->recoil_roll);
        read_table_number(state, 2, "crosshair_bloom", w->crosshair_bloom);
        read_table_number(state, 2, "muzzle_flash_timer", w->muzzle_flash_timer);
        read_table_number(state, 2, "muzzle_flash_spin", w->muzzle_flash_spin);
        read_table_number(state, 2, "move_speed", w->move_speed);
        read_table_number(state, 2, "spread_heat", w->spread_heat);
        read_table_number(state, 2, "current_spread_rad", w->current_spread_rad);
        read_table_number(state, 2, "last_shot_offset_rad", w->last_shot_offset_rad);
        read_table_int(state, 2, "spread_rng_state", w->spread_rng_state);
        lua_pushboolean(state, 1);
        return 1;
    }

    static int l_ecs_get_target(lua_State* state) {
        auto* self = get_impl(state);
        const auto h = static_cast<EntityHandle>(luaL_checkinteger(state, 1));
        const TargetComponent* t = self->world->targets().try_get(h);
        if (t == nullptr) {
            lua_pushnil(state);
            return 1;
        }
        lua_createtable(state, 0, 13);
        auto set_i = [&](const char* k, std::int32_t v) { lua_pushinteger(state, v); lua_setfield(state, -2, k); };
        auto set_f = [&](const char* k, float v) { lua_pushnumber(state, v); lua_setfield(state, -2, k); };
        auto set_b = [&](const char* k, bool v) { lua_pushboolean(state, v ? 1 : 0); lua_setfield(state, -2, k); };
        set_i("slot_index", t->slot_index);
        set_f("base_x", t->base_pos.x);
        set_f("base_y", t->base_pos.y);
        set_f("base_z", t->base_pos.z);
        set_b("knocked_down", t->knocked_down);
        set_f("reset_timer", t->reset_timer);
        set_f("reset_delay", t->reset_delay);
        set_f("hit_flash", t->hit_flash);
        set_f("head_height_threshold", t->head_height_threshold);
        set_i("headshot_points", t->headshot_points);
        set_i("body_points", t->body_points);
        set_f("headshot_impulse", t->headshot_impulse);
        set_f("body_impulse", t->body_impulse);
        return 1;
    }

    static int l_ecs_set_target(lua_State* state) {
        auto* self = get_impl(state);
        const auto h = static_cast<EntityHandle>(luaL_checkinteger(state, 1));
        luaL_checktype(state, 2, LUA_TTABLE);
        TargetComponent* t = self->world->targets().try_get(h);
        if (t == nullptr) {
            lua_pushboolean(state, 0);
            return 1;
        }
        read_table_bool(state, 2, "knocked_down", t->knocked_down);
        read_table_number(state, 2, "reset_timer", t->reset_timer);
        read_table_number(state, 2, "reset_delay", t->reset_delay);
        read_table_number(state, 2, "hit_flash", t->hit_flash);
        lua_pushboolean(state, 1);
        return 1;
    }

    static int l_ecs_get_casing(lua_State* state) {
        auto* self = get_impl(state);
        const auto h = static_cast<EntityHandle>(luaL_checkinteger(state, 1));
        const CasingComponent* c = self->world->casings().try_get(h);
        if (c == nullptr) {
            lua_pushnil(state);
            return 1;
        }
        lua_createtable(state, 0, 4);
        lua_pushinteger(state, c->slot_index);
        lua_setfield(state, -2, "slot_index");
        lua_pushboolean(state, c->active ? 1 : 0);
        lua_setfield(state, -2, "active");
        lua_pushnumber(state, c->age);
        lua_setfield(state, -2, "age");
        lua_pushnumber(state, c->lifetime);
        lua_setfield(state, -2, "lifetime");
        return 1;
    }

    static int l_ecs_set_casing(lua_State* state) {
        auto* self = get_impl(state);
        const auto h = static_cast<EntityHandle>(luaL_checkinteger(state, 1));
        luaL_checktype(state, 2, LUA_TTABLE);
        CasingComponent* c = self->world->casings().try_get(h);
        if (c == nullptr) {
            lua_pushboolean(state, 0);
            return 1;
        }
        read_table_bool(state, 2, "active", c->active);
        read_table_number(state, 2, "age", c->age);
        read_table_number(state, 2, "lifetime", c->lifetime);
        lua_pushboolean(state, 1);
        return 1;
    }

    static int l_ecs_get_health(lua_State* state) {
        auto* self = get_impl(state);
        const auto h = static_cast<EntityHandle>(luaL_checkinteger(state, 1));
        const HealthComponent* hc = self->world->healths().try_get(h);
        if (hc == nullptr) {
            lua_pushnil(state);
            return 1;
        }
        lua_createtable(state, 0, 4);
        lua_pushinteger(state, hc->hp);
        lua_setfield(state, -2, "hp");
        lua_pushinteger(state, hc->max_hp);
        lua_setfield(state, -2, "max_hp");
        lua_pushinteger(state, hc->armor);
        lua_setfield(state, -2, "armor");
        lua_pushinteger(state, hc->max_armor);
        lua_setfield(state, -2, "max_armor");
        return 1;
    }

    static int l_ecs_set_health(lua_State* state) {
        auto* self = get_impl(state);
        const auto h = static_cast<EntityHandle>(luaL_checkinteger(state, 1));
        luaL_checktype(state, 2, LUA_TTABLE);
        HealthComponent* hc = self->world->healths().try_get(h);
        if (hc == nullptr) {
            lua_pushboolean(state, 0);
            return 1;
        }
        read_table_int(state, 2, "hp", hc->hp);
        read_table_int(state, 2, "armor", hc->armor);
        lua_pushboolean(state, 1);
        return 1;
    }

    static int l_ecs_get_player(lua_State* state) {
        auto* self = get_impl(state);
        const auto h = static_cast<EntityHandle>(luaL_checkinteger(state, 1));
        const PlayerControllerComponent* p = self->world->players().try_get(h);
        if (p == nullptr) {
            lua_pushnil(state);
            return 1;
        }
        lua_createtable(state, 0, 29);
        auto set_f = [&](const char* k, float v) { lua_pushnumber(state, v); lua_setfield(state, -2, k); };
        auto set_b = [&](const char* k, bool v) { lua_pushboolean(state, v ? 1 : 0); lua_setfield(state, -2, k); };
        auto set_i = [&](const char* k, std::int32_t v) { lua_pushinteger(state, v); lua_setfield(state, -2, k); };
        set_f("feet_x", p->feet_pos.x);
        set_f("feet_y", p->feet_pos.y);
        set_f("feet_z", p->feet_pos.z);
        set_f("vx", p->velocity.x);
        set_f("vy", p->velocity.y);
        set_f("vz", p->velocity.z);
        set_f("yaw", p->yaw);
        set_f("pitch", p->pitch);
        set_f("eye_height", p->eye_height);
        set_f("stand_eye_height", p->stand_eye_height);
        set_f("crouch_eye_height", p->crouch_eye_height);
        set_f("capsule_radius", p->capsule_radius);
        set_f("capsule_height", p->capsule_height);
        set_f("move_forward", p->move_forward);
        set_f("move_right", p->move_right);
        set_b("jump_pressed", p->jump_pressed);
        set_b("crouch_held", p->crouch_held);
        set_b("sprint_held", p->sprint_held);
        set_b("manual_active", p->manual_active);
        set_b("is_dust2", p->is_dust2);
        set_b("grounded", p->grounded);
        set_b("touching_wall", p->touching_wall);
        set_b("crouched", p->crouched);
        set_f("coyote_timer", p->coyote_timer);
        set_f("jump_buffer_timer", p->jump_buffer_timer);
        set_f("step_phase", p->step_phase);
        set_f("bob_x", p->bob_x);
        set_f("bob_y", p->bob_y);
        set_f("landing_dip", p->landing_dip);
        set_f("horizontal_speed", p->horizontal_speed);
        set_i("collision_count", p->collision_count);
        return 1;
    }

    static int l_ecs_set_player(lua_State* state) {
        auto* self = get_impl(state);
        const auto h = static_cast<EntityHandle>(luaL_checkinteger(state, 1));
        luaL_checktype(state, 2, LUA_TTABLE);
        PlayerControllerComponent* p = self->world->players().try_get(h);
        if (p == nullptr) {
            lua_pushboolean(state, 0);
            return 1;
        }
        read_table_number(state, 2, "feet_x", p->feet_pos.x);
        read_table_number(state, 2, "feet_y", p->feet_pos.y);
        read_table_number(state, 2, "feet_z", p->feet_pos.z);
        read_table_number(state, 2, "vx", p->velocity.x);
        read_table_number(state, 2, "vy", p->velocity.y);
        read_table_number(state, 2, "vz", p->velocity.z);
        read_table_number(state, 2, "eye_height", p->eye_height);
        read_table_number(state, 2, "capsule_height", p->capsule_height);
        read_table_bool(state, 2, "jump_pressed", p->jump_pressed);
        read_table_bool(state, 2, "grounded", p->grounded);
        read_table_bool(state, 2, "touching_wall", p->touching_wall);
        read_table_bool(state, 2, "crouched", p->crouched);
        read_table_number(state, 2, "coyote_timer", p->coyote_timer);
        read_table_number(state, 2, "jump_buffer_timer", p->jump_buffer_timer);
        read_table_number(state, 2, "step_phase", p->step_phase);
        read_table_number(state, 2, "bob_x", p->bob_x);
        read_table_number(state, 2, "bob_y", p->bob_y);
        read_table_number(state, 2, "landing_dip", p->landing_dip);
        read_table_number(state, 2, "horizontal_speed", p->horizontal_speed);
        read_table_int(state, 2, "collision_count", p->collision_count);
        lua_pushboolean(state, 1);
        return 1;
    }

    static int l_ecs_set_visible(lua_State* state) {
        auto* self = get_impl(state);
        const auto h = static_cast<EntityHandle>(luaL_checkinteger(state, 1));
        const bool visible = lua_toboolean(state, 2) != 0;
        RenderableComponent* r = self->world->renderables().try_get(h);
        if (r != nullptr) {
            r->visible = visible;
        }
        return 0;
    }

    static int l_ecs_set_emissive(lua_State* state) {
        auto* self = get_impl(state);
        const auto h = static_cast<EntityHandle>(luaL_checkinteger(state, 1));
        const float em = static_cast<float>(luaL_checknumber(state, 2));
        RenderableComponent* r = self->world->renderables().try_get(h);
        if (r != nullptr) {
            r->emissive = em;
        }
        return 0;
    }

    static int l_ecs_get_bot(lua_State* state) {
        auto* self = get_impl(state);
        const auto h = static_cast<EntityHandle>(luaL_checkinteger(state, 1));
        const BotComponent* b = self->world->bots().try_get(h);
        if (b == nullptr) {
            lua_pushnil(state);
            return 1;
        }
        lua_createtable(state, 0, 22);
        auto set_i = [&](const char* k, std::int32_t v) { lua_pushinteger(state, v); lua_setfield(state, -2, k); };
        auto set_f = [&](const char* k, float v) { lua_pushnumber(state, v); lua_setfield(state, -2, k); };
        auto set_b = [&](const char* k, bool v) { lua_pushboolean(state, v ? 1 : 0); lua_setfield(state, -2, k); };
        auto set_s = [&](const char* k, const std::string& v) { lua_pushlstring(state, v.data(), v.size()); lua_setfield(state, -2, k); };
        set_i("slot_index", b->slot_index);
        set_i("team", b->team);
        set_b("active", b->active);
        set_b("alive", b->alive);
        set_i("hp", b->hp);
        set_i("max_hp", b->max_hp);
        set_f("x", b->pos.x);
        set_f("y", b->pos.y);
        set_f("z", b->pos.z);
        set_f("vx", b->vel.x);
        set_f("vy", b->vel.y);
        set_f("vz", b->vel.z);
        set_f("yaw", b->yaw);
        set_f("aim_pitch", b->aim_pitch);
        set_s("anim_clip", b->anim_clip);
        set_f("anim_time", b->anim_time);
        set_f("anim_speed", b->anim_speed);
        set_b("anim_loop", b->anim_loop);
        set_f("hit_flash", b->hit_flash);
        set_f("respawn_timer", b->respawn_timer);
        set_f("shoot_cooldown", b->shoot_cooldown);
        set_i("waypoint_index", b->waypoint_index);
        return 1;
    }

    static int l_ecs_set_bot(lua_State* state) {
        auto* self = get_impl(state);
        const auto h = static_cast<EntityHandle>(luaL_checkinteger(state, 1));
        luaL_checktype(state, 2, LUA_TTABLE);
        BotComponent* b = self->world->bots().try_get(h);
        if (b == nullptr) {
            lua_pushboolean(state, 0);
            return 1;
        }
        std::string next_clip = b->anim_clip;
        read_table_bool(state, 2, "active", b->active);
        read_table_bool(state, 2, "alive", b->alive);
        read_table_int(state, 2, "hp", b->hp);
        read_table_int(state, 2, "max_hp", b->max_hp);
        read_table_number(state, 2, "x", b->pos.x);
        read_table_number(state, 2, "y", b->pos.y);
        read_table_number(state, 2, "z", b->pos.z);
        read_table_number(state, 2, "vx", b->vel.x);
        read_table_number(state, 2, "vy", b->vel.y);
        read_table_number(state, 2, "vz", b->vel.z);
        read_table_number(state, 2, "yaw", b->yaw);
        read_table_number(state, 2, "aim_pitch", b->aim_pitch);
        if (read_table_string(state, 2, "anim_clip", next_clip) && next_clip != b->anim_clip) {
            b->prev_anim_clip = b->anim_clip;
            b->prev_anim_time = b->anim_time;
            b->anim_clip = next_clip;
            b->anim_blend = 0.0f;
        }
        read_table_number(state, 2, "anim_time", b->anim_time);
        read_table_number(state, 2, "anim_speed", b->anim_speed);
        read_table_bool(state, 2, "anim_loop", b->anim_loop);
        read_table_number(state, 2, "hit_flash", b->hit_flash);
        read_table_number(state, 2, "respawn_timer", b->respawn_timer);
        read_table_number(state, 2, "shoot_cooldown", b->shoot_cooldown);
        read_table_int(state, 2, "waypoint_index", b->waypoint_index);
        lua_pushboolean(state, 1);
        return 1;
    }

    static int l_ecs_get_waypoints(lua_State* state) {
        auto* self = get_impl(state);
        const std::vector<Float3> wps = self->callbacks.get_waypoints ? self->callbacks.get_waypoints() : std::vector<Float3>{};
        lua_createtable(state, static_cast<int>(wps.size()), 0);
        for (std::size_t i = 0; i < wps.size(); ++i) {
            lua_createtable(state, 0, 3);
            lua_pushnumber(state, wps[i].x); lua_setfield(state, -2, "x");
            lua_pushnumber(state, wps[i].y); lua_setfield(state, -2, "y");
            lua_pushnumber(state, wps[i].z); lua_setfield(state, -2, "z");
            lua_rawseti(state, -2, static_cast<lua_Integer>(i + 1U));
        }
        return 1;
    }

    static int l_ecs_is_dust2(lua_State* state) {
        auto* self = get_impl(state);
        const bool d2 = self->callbacks.is_dust2_active ? self->callbacks.is_dust2_active() : false;
        lua_pushboolean(state, d2 ? 1 : 0);
        return 1;
    }

    static int l_ecs_record_bot_kill(lua_State* state) {
        auto* self = get_impl(state);
        const auto team = static_cast<std::int32_t>(luaL_optinteger(state, 1, 1));
        if (self->callbacks.record_bot_kill) {
            self->callbacks.record_bot_kill(team);
        }
        return 0;
    }

    static int l_events_on(lua_State* state) {
        auto* self = get_impl(state);
        const char* event_name = luaL_checkstring(state, 1);
        luaL_checktype(state, 2, LUA_TFUNCTION);
        lua_pushvalue(state, 2);
        const int fn_ref = luaL_ref(state, LUA_REGISTRYINDEX);
        self->event_callback_refs.push_back(fn_ref);
        const std::uint64_t sub_id = self->bus->subscribe(
            event_name,
            [self, fn_ref](const std::string& name, const EventPayload& payload) {
                if (self->L == nullptr) return;
                lua_rawgeti(self->L, LUA_REGISTRYINDEX, fn_ref);
                if (!lua_isfunction(self->L, -1)) {
                    lua_pop(self->L, 1);
                    return;
                }
                push_payload_table(self->L, payload);
                lua_pushlstring(self->L, name.data(), name.size());
                if (lua_pcall(self->L, 2, 0, 0) != LUA_OK) {
                    self->record_error(lua_tostring(self->L, -1));
                    lua_pop(self->L, 1);
                }
            },
            k_lua_event_owner_tag);
        lua_pushinteger(state, static_cast<lua_Integer>(sub_id));
        return 1;
    }

    static int l_events_off(lua_State* state) {
        auto* self = get_impl(state);
        const auto sub_id = static_cast<std::uint64_t>(luaL_checkinteger(state, 1));
        lua_pushboolean(state, self->bus->unsubscribe(sub_id) ? 1 : 0);
        return 1;
    }

    static int l_events_emit(lua_State* state) {
        auto* self = get_impl(state);
        const char* event_name = luaL_checkstring(state, 1);
        const EventPayload payload = lua_gettop(state) >= 2 ? read_payload_table(state, 2) : EventPayload{};
        self->bus->emit(event_name, payload);
        return 0;
    }

    static int l_events_enqueue(lua_State* state) {
        auto* self = get_impl(state);
        const char* event_name = luaL_checkstring(state, 1);
        const EventPayload payload = lua_gettop(state) >= 2 ? read_payload_table(state, 2) : EventPayload{};
        self->bus->enqueue(event_name, payload);
        return 0;
    }

    static int l_physics_apply_impulse(lua_State* state) {
        auto* self = get_impl(state);
        const auto h = static_cast<EntityHandle>(luaL_checkinteger(state, 1));
        const Float3 impulse{
            static_cast<float>(luaL_checknumber(state, 2)),
            static_cast<float>(luaL_checknumber(state, 3)),
            static_cast<float>(luaL_checknumber(state, 4))};
        const Float3 point{
            static_cast<float>(luaL_checknumber(state, 5)),
            static_cast<float>(luaL_checknumber(state, 6)),
            static_cast<float>(luaL_checknumber(state, 7))};
        if (self->callbacks.apply_target_impulse) {
            self->callbacks.apply_target_impulse(h, impulse, point);
        }
        return 0;
    }

    static int l_physics_set_target_upright(lua_State* state) {
        auto* self = get_impl(state);
        const auto h = static_cast<EntityHandle>(luaL_checkinteger(state, 1));
        const bool upright = lua_toboolean(state, 2) != 0;
        if (self->callbacks.set_target_motor_upright) {
            self->callbacks.set_target_motor_upright(h, upright);
        }
        return 0;
    }

    static int l_physics_reset_target_pose(lua_State* state) {
        auto* self = get_impl(state);
        const auto h = static_cast<EntityHandle>(luaL_checkinteger(state, 1));
        if (self->callbacks.reset_target_pose) {
            self->callbacks.reset_target_pose(h);
        }
        return 0;
    }

    static int l_physics_launch_casing(lua_State* state) {
        auto* self = get_impl(state);
        const auto h = static_cast<EntityHandle>(luaL_checkinteger(state, 1));
        const Float3 pos{
            static_cast<float>(luaL_checknumber(state, 2)),
            static_cast<float>(luaL_checknumber(state, 3)),
            static_cast<float>(luaL_checknumber(state, 4))};
        const Float3 vel{
            static_cast<float>(luaL_checknumber(state, 5)),
            static_cast<float>(luaL_checknumber(state, 6)),
            static_cast<float>(luaL_checknumber(state, 7))};
        const Float3 ang{
            static_cast<float>(luaL_checknumber(state, 8)),
            static_cast<float>(luaL_checknumber(state, 9)),
            static_cast<float>(luaL_checknumber(state, 10))};
        if (self->callbacks.launch_casing) {
            self->callbacks.launch_casing(h, pos, vel, ang);
        }
        return 0;
    }

    static int l_physics_deactivate_casing(lua_State* state) {
        auto* self = get_impl(state);
        const auto h = static_cast<EntityHandle>(luaL_checkinteger(state, 1));
        if (self->callbacks.deactivate_casing) {
            self->callbacks.deactivate_casing(h);
        }
        return 0;
    }

    static int l_physics_raycast(lua_State* state) {
        auto* self = get_impl(state);
        const Float3 origin{
            static_cast<float>(luaL_checknumber(state, 1)),
            static_cast<float>(luaL_checknumber(state, 2)),
            static_cast<float>(luaL_checknumber(state, 3))};
        const Float3 dir{
            static_cast<float>(luaL_checknumber(state, 4)),
            static_cast<float>(luaL_checknumber(state, 5)),
            static_cast<float>(luaL_checknumber(state, 6))};
        const float max_dist = static_cast<float>(luaL_checknumber(state, 7));
        LuaRaycastResult r{};
        if (self->callbacks.raycast_world) {
            r = self->callbacks.raycast_world(origin, dir, max_dist);
        }
        lua_createtable(state, 0, 8);
        lua_pushboolean(state, r.hit ? 1 : 0); lua_setfield(state, -2, "hit");
        lua_pushnumber(state, r.dist); lua_setfield(state, -2, "dist");
        lua_pushnumber(state, r.point.x); lua_setfield(state, -2, "x");
        lua_pushnumber(state, r.point.y); lua_setfield(state, -2, "y");
        lua_pushnumber(state, r.point.z); lua_setfield(state, -2, "z");
        lua_pushnumber(state, r.normal.x); lua_setfield(state, -2, "nx");
        lua_pushnumber(state, r.normal.y); lua_setfield(state, -2, "ny");
        lua_pushnumber(state, r.normal.z); lua_setfield(state, -2, "nz");
        return 1;
    }

    static int l_physics_sweep_capsule(lua_State* state) {
        auto* self = get_impl(state);
        const Float3 center{
            static_cast<float>(luaL_checknumber(state, 1)),
            static_cast<float>(luaL_checknumber(state, 2)),
            static_cast<float>(luaL_checknumber(state, 3))};
        const Float3 disp{
            static_cast<float>(luaL_checknumber(state, 4)),
            static_cast<float>(luaL_checknumber(state, 5)),
            static_cast<float>(luaL_checknumber(state, 6))};
        const float radius = static_cast<float>(luaL_checknumber(state, 7));
        const float half_height = static_cast<float>(luaL_checknumber(state, 8));
        LuaCapsuleSweepResult r{};
        if (self->callbacks.sweep_capsule) {
            r = self->callbacks.sweep_capsule(center, disp, radius, half_height);
        }
        lua_createtable(state, 0, 8);
        lua_pushboolean(state, r.hit ? 1 : 0); lua_setfield(state, -2, "hit");
        lua_pushnumber(state, r.fraction); lua_setfield(state, -2, "fraction");
        lua_pushnumber(state, r.point.x); lua_setfield(state, -2, "x");
        lua_pushnumber(state, r.point.y); lua_setfield(state, -2, "y");
        lua_pushnumber(state, r.point.z); lua_setfield(state, -2, "z");
        lua_pushnumber(state, r.normal.x); lua_setfield(state, -2, "nx");
        lua_pushnumber(state, r.normal.y); lua_setfield(state, -2, "ny");
        lua_pushnumber(state, r.normal.z); lua_setfield(state, -2, "nz");
        return 1;
    }

    static int l_physics_resolve_capsule_overlap(lua_State* state) {
        auto* self = get_impl(state);
        const Float3 center{
            static_cast<float>(luaL_checknumber(state, 1)),
            static_cast<float>(luaL_checknumber(state, 2)),
            static_cast<float>(luaL_checknumber(state, 3))};
        const float radius = static_cast<float>(luaL_checknumber(state, 4));
        const float half_height = static_cast<float>(luaL_checknumber(state, 5));
        LuaOverlapResult r{};
        if (self->callbacks.resolve_capsule_overlap) {
            r = self->callbacks.resolve_capsule_overlap(center, radius, half_height);
        }
        lua_createtable(state, 0, 4);
        lua_pushboolean(state, r.penetrated ? 1 : 0); lua_setfield(state, -2, "penetrated");
        lua_pushnumber(state, r.push.x); lua_setfield(state, -2, "push_x");
        lua_pushnumber(state, r.push.y); lua_setfield(state, -2, "push_y");
        lua_pushnumber(state, r.push.z); lua_setfield(state, -2, "push_z");
        return 1;
    }

    static int l_hud_hit_marker(lua_State* state) {
        auto* self = get_impl(state);
        const bool hs = lua_toboolean(state, 1) != 0;
        if (self->callbacks.trigger_hit_marker) {
            self->callbacks.trigger_hit_marker(hs);
        }
        return 0;
    }

    static int l_hud_add_score(lua_State* state) {
        auto* self = get_impl(state);
        const auto pts = static_cast<std::int32_t>(luaL_checkinteger(state, 1));
        const bool hs = lua_toboolean(state, 2) != 0;
        if (self->callbacks.add_score) {
            self->callbacks.add_score(pts, hs);
        }
        return 0;
    }

    static int l_hud_reset_score(lua_State* state) {
        auto* self = get_impl(state);
        if (self->callbacks.reset_score) {
            self->callbacks.reset_score();
        }
        return 0;
    }

    void register_bindings() {
        lua_pushlightuserdata(L, this);
        lua_setfield(L, LUA_REGISTRYINDEX, k_host_registry_key);

        static const luaL_Reg ecs_funcs[] = {
            {"create", l_ecs_create},
            {"destroy", l_ecs_destroy},
            {"alive", l_ecs_alive},
            {"find", l_ecs_find},
            {"find_by_tag", l_ecs_find_by_tag},
            {"count", l_ecs_count},
            {"get_weapon", l_ecs_get_weapon},
            {"set_weapon", l_ecs_set_weapon},
            {"get_target", l_ecs_get_target},
            {"set_target", l_ecs_set_target},
            {"get_casing", l_ecs_get_casing},
            {"set_casing", l_ecs_set_casing},
            {"get_health", l_ecs_get_health},
            {"set_health", l_ecs_set_health},
            {"get_player", l_ecs_get_player},
            {"set_player", l_ecs_set_player},
            {"get_bot", l_ecs_get_bot},
            {"set_bot", l_ecs_set_bot},
            {"get_waypoints", l_ecs_get_waypoints},
            {"is_dust2", l_ecs_is_dust2},
            {"record_bot_kill", l_ecs_record_bot_kill},
            {"set_visible", l_ecs_set_visible},
            {"set_emissive", l_ecs_set_emissive},
            {nullptr, nullptr},
        };
        luaL_newlib(L, ecs_funcs);
        lua_setglobal(L, "ecs");

        static const luaL_Reg events_funcs[] = {
            {"on", l_events_on},
            {"off", l_events_off},
            {"emit", l_events_emit},
            {"enqueue", l_events_enqueue},
            {nullptr, nullptr},
        };
        luaL_newlib(L, events_funcs);
        lua_setglobal(L, "events");

        static const luaL_Reg physics_funcs[] = {
            {"apply_impulse", l_physics_apply_impulse},
            {"set_target_upright", l_physics_set_target_upright},
            {"reset_target_pose", l_physics_reset_target_pose},
            {"launch_casing", l_physics_launch_casing},
            {"deactivate_casing", l_physics_deactivate_casing},
            {"raycast", l_physics_raycast},
            {"sweep_capsule", l_physics_sweep_capsule},
            {"resolve_capsule_overlap", l_physics_resolve_capsule_overlap},
            {nullptr, nullptr},
        };
        luaL_newlib(L, physics_funcs);
        lua_setglobal(L, "physics");

        static const luaL_Reg hud_funcs[] = {
            {"hit_marker", l_hud_hit_marker},
            {"add_score", l_hud_add_score},
            {"reset_score", l_hud_reset_score},
            {nullptr, nullptr},
        };
        luaL_newlib(L, hud_funcs);
        lua_setglobal(L, "hud");
    }

    void shutdown_vm() {
        if (bus != nullptr) {
            bus->remove_owner_subscriptions(k_lua_event_owner_tag);
        }
        modules.clear();
        event_callback_refs.clear();
        if (L != nullptr) {
            lua_close(L);
            L = nullptr;
        }
    }

    bool boot_vm() {
        shutdown_vm();
        L = luaL_newstate();
        if (L == nullptr) {
            record_error("luaL_newstate failed");
            return false;
        }
        luaL_requiref(L, "_G", luaopen_base, 1);
        lua_pop(L, 1);
        luaL_requiref(L, LUA_MATHLIBNAME, luaopen_math, 1);
        lua_pop(L, 1);
        luaL_requiref(L, LUA_TABLIBNAME, luaopen_table, 1);
        lua_pop(L, 1);
        luaL_requiref(L, LUA_STRLIBNAME, luaopen_string, 1);
        lua_pop(L, 1);

        register_bindings();
        return true;
    }

    LoadedModule* load_script_module(const std::string& script_name) {
        const auto existing = modules.find(script_name);
        if (existing != modules.end()) {
            return &existing->second;
        }
        const auto full_path = resolve_asset_path(std::filesystem::path("assets/scripts") / script_name);
        if (luaL_loadfile(L, full_path.string().c_str()) != LUA_OK) {
            record_error(lua_tostring(L, -1));
            lua_pop(L, 1);
            return nullptr;
        }
        if (lua_pcall(L, 0, 1, 0) != LUA_OK) {
            record_error(lua_tostring(L, -1));
            lua_pop(L, 1);
            return nullptr;
        }
        if (!lua_istable(L, -1)) {
            lua_pop(L, 1);
            record_error("lua script must return a module table");
            return nullptr;
        }

        LoadedModule mod{};
        lua_getfield(L, -1, "on_init");
        if (lua_isfunction(L, -1)) {
            mod.init_ref = luaL_ref(L, LUA_REGISTRYINDEX);
        } else {
            lua_pop(L, 1);
        }

        lua_getfield(L, -1, "on_update");
        if (lua_isfunction(L, -1)) {
            mod.update_ref = luaL_ref(L, LUA_REGISTRYINDEX);
        } else {
            lua_pop(L, 1);
        }

        mod.table_ref = luaL_ref(L, LUA_REGISTRYINDEX);
        auto inserted = modules.emplace(script_name, mod);
        return &inserted.first->second;
    }

    bool bind_all_script_components() {
        if (world == nullptr || L == nullptr) return false;
        bool all_ok = true;
        const auto& ents = world->scripts().entities();
        auto& comps = world->scripts().components();
        for (std::size_t i = 0; i < comps.size(); ++i) {
            comps[i].initialized = false;
        }
        for (std::size_t i = 0; i < comps.size(); ++i) {
            if (!comps[i].enabled || comps[i].script_name.empty()) {
                continue;
            }
            LoadedModule* mod = load_script_module(comps[i].script_name);
            if (mod == nullptr) {
                all_ok = false;
                continue;
            }
            comps[i].initialized = true;
            if (mod->init_ref != LUA_NOREF) {
                lua_rawgeti(L, LUA_REGISTRYINDEX, mod->init_ref);
                lua_pushinteger(L, static_cast<lua_Integer>(ents[i]));
                if (lua_pcall(L, 1, 0, 0) != LUA_OK) {
                    record_error(lua_tostring(L, -1));
                    lua_pop(L, 1);
                    all_ok = false;
                }
            }
        }
        return all_ok;
    }
};

LuaScriptEngine::LuaScriptEngine() : impl_(std::make_unique<Impl>()) {}
LuaScriptEngine::~LuaScriptEngine() = default;

bool LuaScriptEngine::initialize(EcsWorld* world, EventBus* bus, LuaHostCallbacks callbacks) {
    impl_->world = world;
    impl_->bus = bus;
    impl_->callbacks = std::move(callbacks);
    if (!impl_->boot_vm()) {
        return false;
    }
    return impl_->bind_all_script_components();
}

bool LuaScriptEngine::reload_all() {
    if (impl_->world == nullptr || impl_->bus == nullptr) {
        return false;
    }
    ++impl_->reload_count;
    if (!impl_->boot_vm()) {
        return false;
    }
    return impl_->bind_all_script_components();
}

void LuaScriptEngine::step(double delta_seconds) {
    if (impl_->L == nullptr || impl_->world == nullptr) {
        return;
    }
    const auto t0 = std::chrono::steady_clock::now();
    const auto& ents = impl_->world->scripts().entities();
    auto& comps = impl_->world->scripts().components();
    for (std::size_t i = 0; i < comps.size(); ++i) {
        if (!comps[i].enabled || comps[i].script_name.empty()) {
            continue;
        }
        auto it = impl_->modules.find(comps[i].script_name);
        if (it == impl_->modules.end()) {
            continue;
        }
        if (it->second.update_ref == LUA_NOREF) {
            continue;
        }
        lua_rawgeti(impl_->L, LUA_REGISTRYINDEX, it->second.update_ref);
        lua_pushinteger(impl_->L, static_cast<lua_Integer>(ents[i]));
        lua_pushnumber(impl_->L, delta_seconds);
        if (lua_pcall(impl_->L, 2, 0, 0) != LUA_OK) {
            impl_->record_error(lua_tostring(impl_->L, -1));
            lua_pop(impl_->L, 1);
        }
    }
    lua_gc(impl_->L, LUA_GCSTEP, 2);
    const auto t1 = std::chrono::steady_clock::now();
    impl_->last_step_us = std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();
}

LuaEngineStats LuaScriptEngine::stats() const {
    return LuaEngineStats{
        static_cast<std::uint32_t>(impl_->modules.size()),
        impl_->reload_count,
        impl_->error_count,
        impl_->last_step_us,
        impl_->last_error,
    };
}

}
