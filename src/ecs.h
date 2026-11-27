#pragma once

#include "tactical_range.h"

#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

namespace frame_warp_lab {

using EntityHandle = std::uint64_t;
inline constexpr EntityHandle k_invalid_entity = 0ULL;

inline std::uint32_t entity_index(EntityHandle handle) {
    return static_cast<std::uint32_t>(handle & 0xFFFFFFFFULL);
}

inline std::uint32_t entity_generation(EntityHandle handle) {
    return static_cast<std::uint32_t>(handle >> 32U);
}

inline EntityHandle make_entity_handle(std::uint32_t index, std::uint32_t generation) {
    return (static_cast<std::uint64_t>(generation) << 32U) | static_cast<std::uint64_t>(index);
}

enum class PhysicsBodyKind : std::uint32_t {
    static_geometry = 0,
    hinge_target_plate = 1,
    dynamic_casing = 2,
};

struct TagComponent {
    std::string name;
    std::string tag;
    std::string asset_uri;
};

struct TransformComponent {
    Float3 position{0.0f, 0.0f, 0.0f};
    Float3 rotation_euler{0.0f, 0.0f, 0.0f};
    Float3 scale{1.0f, 1.0f, 1.0f};
    Mat4x4 model{};
    Mat4x4 prev_model{};
    bool has_prev = false;
    bool matrix_driven = false;
};

struct RenderableComponent {
    RangeMeshKind mesh = RangeMeshKind::static_world;
    Float3 tint{1.0f, 1.0f, 1.0f};
    float emissive = 0.0f;
    bool visible = true;
    bool viewmodel = false;
};

struct PhysicsBodyComponent {
    std::uint32_t jolt_body_raw = 0xFFFFFFFFU;
    PhysicsBodyKind kind = PhysicsBodyKind::static_geometry;
    std::int32_t slot_index = -1;
    bool active = true;
};

struct WeaponComponent {
    std::int32_t ammo_in_mag = 12;
    std::int32_t mag_size = 12;
    std::int32_t ammo_reserve = 48;
    std::int32_t shots_fired = 0;
    float fire_cooldown = 0.0f;
    float fire_interval = 0.135f;
    bool ads_requested = false;
    float ads_alpha = 0.0f;
    bool reloading = false;
    float reload_timer = 0.0f;
    float reload_duration = 1.30f;
    float slide_blowback = 0.0f;
    float trigger_pull = 0.0f;
    float hammer_cock = 0.0f;
    float recoil_kick = 0.0f;
    float recoil_pitch = 0.0f;
    float recoil_yaw = 0.0f;
    float recoil_roll = 0.0f;
    float crosshair_bloom = 0.0f;
    float muzzle_flash_timer = 0.0f;
    float muzzle_flash_spin = 0.0f;
    float move_speed = 0.0f;
    float spread_heat = 0.0f;
    float current_spread_rad = 0.011f;
    float last_shot_offset_rad = 0.0f;
    std::int32_t spread_rng_state = 1234567;
};

struct TargetComponent {
    std::int32_t slot_index = 0;
    Float3 base_pos{0.0f, 0.14f, 10.0f};
    bool knocked_down = false;
    float reset_timer = 0.0f;
    float reset_delay = 2.4f;
    float hit_flash = 0.0f;
    float head_height_threshold = 1.06f;
    std::int32_t headshot_points = 250;
    std::int32_t body_points = 100;
    float headshot_impulse = 135.0f;
    float body_impulse = 105.0f;
};

struct CasingComponent {
    std::int32_t slot_index = 0;
    bool active = false;
    float age = 0.0f;
    float lifetime = 6.0f;
};

struct HealthComponent {
    std::int32_t hp = 100;
    std::int32_t max_hp = 100;
    std::int32_t armor = 100;
    std::int32_t max_armor = 100;
};

struct PlayerControllerComponent {
    Float3 feet_pos{0.0f, 0.0f, 1.2f};
    Float3 velocity{0.0f, 0.0f, 0.0f};
    float yaw = 0.0f;
    float pitch = 0.0f;
    float eye_height = 1.60f;
    float stand_eye_height = 1.60f;
    float crouch_eye_height = 1.04f;
    float capsule_radius = 0.28f;
    float capsule_height = 1.72f;
    float move_forward = 0.0f;
    float move_right = 0.0f;
    bool jump_pressed = false;
    bool crouch_held = false;
    bool sprint_held = false;
    bool manual_active = true;
    bool is_dust2 = false;
    bool grounded = true;
    bool touching_wall = false;
    bool crouched = false;
    float coyote_timer = 0.0f;
    float jump_buffer_timer = 0.0f;
    float step_phase = 0.0f;
    float bob_x = 0.0f;
    float bob_y = 0.0f;
    float landing_dip = 0.0f;
    float horizontal_speed = 0.0f;
    std::int32_t collision_count = 0;
};

struct ScriptComponent {
    std::string script_name;
    bool enabled = true;
    bool initialized = false;
};

template <typename T>
class SparseComponentPool {
public:
    static constexpr std::uint32_t k_empty = std::numeric_limits<std::uint32_t>::max();

    bool has(EntityHandle handle) const {
        if (handle == k_invalid_entity) return false;
        const std::uint32_t idx = entity_index(handle);
        if (idx >= sparse_.size()) return false;
        const std::uint32_t dense_idx = sparse_[idx];
        return dense_idx != k_empty && entities_[dense_idx] == handle;
    }

    T& emplace(EntityHandle handle, const T& value = T{}) {
        const std::uint32_t idx = entity_index(handle);
        if (idx >= sparse_.size()) {
            sparse_.resize(static_cast<std::size_t>(idx) + 1U, k_empty);
        }
        const std::uint32_t existing = sparse_[idx];
        if (existing != k_empty && entities_[existing] == handle) {
            dense_[existing] = value;
            return dense_[existing];
        }
        const auto dense_idx = static_cast<std::uint32_t>(dense_.size());
        sparse_[idx] = dense_idx;
        entities_.push_back(handle);
        dense_.push_back(value);
        return dense_.back();
    }

    T* try_get(EntityHandle handle) {
        if (!has(handle)) return nullptr;
        return &dense_[sparse_[entity_index(handle)]];
    }

    const T* try_get(EntityHandle handle) const {
        if (!has(handle)) return nullptr;
        return &dense_[sparse_[entity_index(handle)]];
    }

    void remove(EntityHandle handle) {
        if (!has(handle)) return;
        const std::uint32_t idx = entity_index(handle);
        const std::uint32_t dense_idx = sparse_[idx];
        const std::uint32_t last_idx = static_cast<std::uint32_t>(dense_.size() - 1U);
        if (dense_idx != last_idx) {
            dense_[dense_idx] = std::move(dense_[last_idx]);
            const EntityHandle moved = entities_[last_idx];
            entities_[dense_idx] = moved;
            sparse_[entity_index(moved)] = dense_idx;
        }
        dense_.pop_back();
        entities_.pop_back();
        sparse_[idx] = k_empty;
    }

    void clear() {
        dense_.clear();
        entities_.clear();
        sparse_.clear();
    }

    std::uint32_t count() const { return static_cast<std::uint32_t>(dense_.size()); }
    const std::vector<EntityHandle>& entities() const { return entities_; }
    std::vector<T>& components() { return dense_; }
    const std::vector<T>& components() const { return dense_; }

private:
    std::vector<T> dense_;
    std::vector<EntityHandle> entities_;
    std::vector<std::uint32_t> sparse_;
};

struct EcsWorldStats {
    std::uint32_t entity_count = 0;
    std::uint32_t renderable_count = 0;
    std::uint32_t physics_body_count = 0;
    std::uint32_t script_count = 0;
};

class EcsWorld {
public:
    EcsWorld();

    EntityHandle create_entity(std::string_view name, std::string_view tag = {}, std::string_view asset_uri = {});
    bool destroy_entity(EntityHandle handle);
    bool is_alive(EntityHandle handle) const;
    void clear();

    EntityHandle find_by_name(std::string_view name) const;
    std::vector<EntityHandle> find_by_tag(std::string_view tag) const;
    EntityHandle find_by_jolt_body(std::uint32_t jolt_body_raw) const;

    SparseComponentPool<TagComponent>& tags() { return tags_; }
    const SparseComponentPool<TagComponent>& tags() const { return tags_; }
    SparseComponentPool<TransformComponent>& transforms() { return transforms_; }
    const SparseComponentPool<TransformComponent>& transforms() const { return transforms_; }
    SparseComponentPool<RenderableComponent>& renderables() { return renderables_; }
    const SparseComponentPool<RenderableComponent>& renderables() const { return renderables_; }
    SparseComponentPool<PhysicsBodyComponent>& physics_bodies() { return physics_bodies_; }
    const SparseComponentPool<PhysicsBodyComponent>& physics_bodies() const { return physics_bodies_; }
    SparseComponentPool<WeaponComponent>& weapons() { return weapons_; }
    const SparseComponentPool<WeaponComponent>& weapons() const { return weapons_; }
    SparseComponentPool<TargetComponent>& targets() { return targets_; }
    const SparseComponentPool<TargetComponent>& targets() const { return targets_; }
    SparseComponentPool<CasingComponent>& casings() { return casings_; }
    const SparseComponentPool<CasingComponent>& casings() const { return casings_; }
    SparseComponentPool<HealthComponent>& healths() { return healths_; }
    const SparseComponentPool<HealthComponent>& healths() const { return healths_; }
    SparseComponentPool<PlayerControllerComponent>& players() { return players_; }
    const SparseComponentPool<PlayerControllerComponent>& players() const { return players_; }
    SparseComponentPool<ScriptComponent>& scripts() { return scripts_; }
    const SparseComponentPool<ScriptComponent>& scripts() const { return scripts_; }

    EcsWorldStats stats() const;

private:
    std::vector<std::uint32_t> generations_;
    std::vector<bool> alive_;
    std::vector<std::uint32_t> free_indices_;
    std::uint32_t alive_count_ = 0;

    SparseComponentPool<TagComponent> tags_;
    SparseComponentPool<TransformComponent> transforms_;
    SparseComponentPool<RenderableComponent> renderables_;
    SparseComponentPool<PhysicsBodyComponent> physics_bodies_;
    SparseComponentPool<WeaponComponent> weapons_;
    SparseComponentPool<TargetComponent> targets_;
    SparseComponentPool<CasingComponent> casings_;
    SparseComponentPool<HealthComponent> healths_;
    SparseComponentPool<PlayerControllerComponent> players_;
    SparseComponentPool<ScriptComponent> scripts_;
};

}
