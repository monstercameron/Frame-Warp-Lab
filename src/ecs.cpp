#include "ecs.h"

namespace frame_warp_lab {

EcsWorld::EcsWorld() {
    generations_.push_back(0U);
    alive_.push_back(false);
}

EntityHandle EcsWorld::create_entity(std::string_view name, std::string_view tag, std::string_view asset_uri) {
    std::uint32_t idx = 0;
    if (!free_indices_.empty()) {
        idx = free_indices_.back();
        free_indices_.pop_back();
    } else {
        idx = static_cast<std::uint32_t>(generations_.size());
        generations_.push_back(1U);
        alive_.push_back(false);
    }
    alive_[idx] = true;
    ++alive_count_;
    const EntityHandle handle = make_entity_handle(idx, generations_[idx]);
    TagComponent tc{};
    tc.name = std::string(name);
    tc.tag = std::string(tag);
    tc.asset_uri = std::string(asset_uri);
    tags_.emplace(handle, tc);
    return handle;
}

bool EcsWorld::destroy_entity(EntityHandle handle) {
    if (!is_alive(handle)) {
        return false;
    }
    const std::uint32_t idx = entity_index(handle);
    tags_.remove(handle);
    transforms_.remove(handle);
    renderables_.remove(handle);
    physics_bodies_.remove(handle);
    weapons_.remove(handle);
    targets_.remove(handle);
    casings_.remove(handle);
    healths_.remove(handle);
    scripts_.remove(handle);

    alive_[idx] = false;
    generations_[idx] = generations_[idx] + 1U;
    if (generations_[idx] == 0U) {
        generations_[idx] = 1U;
    }
    free_indices_.push_back(idx);
    if (alive_count_ > 0U) {
        --alive_count_;
    }
    return true;
}

bool EcsWorld::is_alive(EntityHandle handle) const {
    if (handle == k_invalid_entity) return false;
    const std::uint32_t idx = entity_index(handle);
    if (idx == 0U || idx >= alive_.size()) return false;
    return alive_[idx] && generations_[idx] == entity_generation(handle);
}

void EcsWorld::clear() {
    tags_.clear();
    transforms_.clear();
    renderables_.clear();
    physics_bodies_.clear();
    weapons_.clear();
    targets_.clear();
    casings_.clear();
    healths_.clear();
    scripts_.clear();
    generations_.clear();
    alive_.clear();
    free_indices_.clear();
    generations_.push_back(0U);
    alive_.push_back(false);
    alive_count_ = 0U;
}

EntityHandle EcsWorld::find_by_name(std::string_view name) const {
    const auto& ents = tags_.entities();
    const auto& comps = tags_.components();
    for (std::size_t i = 0; i < comps.size(); ++i) {
        if (comps[i].name == name && is_alive(ents[i])) {
            return ents[i];
        }
    }
    return k_invalid_entity;
}

std::vector<EntityHandle> EcsWorld::find_by_tag(std::string_view tag) const {
    std::vector<EntityHandle> result;
    const auto& ents = tags_.entities();
    const auto& comps = tags_.components();
    for (std::size_t i = 0; i < comps.size(); ++i) {
        if (comps[i].tag == tag && is_alive(ents[i])) {
            result.push_back(ents[i]);
        }
    }
    return result;
}

EntityHandle EcsWorld::find_by_jolt_body(std::uint32_t jolt_body_raw) const {
    const auto& ents = physics_bodies_.entities();
    const auto& comps = physics_bodies_.components();
    for (std::size_t i = 0; i < comps.size(); ++i) {
        if (comps[i].jolt_body_raw == jolt_body_raw && is_alive(ents[i])) {
            return ents[i];
        }
    }
    return k_invalid_entity;
}

EcsWorldStats EcsWorld::stats() const {
    return EcsWorldStats{
        alive_count_,
        renderables_.count(),
        physics_bodies_.count(),
        scripts_.count(),
    };
}

}
