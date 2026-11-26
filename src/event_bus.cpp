#include "event_bus.h"

#include <algorithm>

namespace frame_warp_lab {

double EventPayload::get_number(std::string_view key, double fallback) const {
    const auto it = fields.find(std::string(key));
    if (it == fields.end()) return fallback;
    if (const auto* d = std::get_if<double>(&it->second)) return *d;
    if (const auto* i = std::get_if<std::int64_t>(&it->second)) return static_cast<double>(*i);
    if (const auto* u = std::get_if<std::uint64_t>(&it->second)) return static_cast<double>(*u);
    if (const auto* b = std::get_if<bool>(&it->second)) return *b ? 1.0 : 0.0;
    return fallback;
}

std::int64_t EventPayload::get_int(std::string_view key, std::int64_t fallback) const {
    const auto it = fields.find(std::string(key));
    if (it == fields.end()) return fallback;
    if (const auto* i = std::get_if<std::int64_t>(&it->second)) return *i;
    if (const auto* u = std::get_if<std::uint64_t>(&it->second)) return static_cast<std::int64_t>(*u);
    if (const auto* d = std::get_if<double>(&it->second)) return static_cast<std::int64_t>(*d);
    if (const auto* b = std::get_if<bool>(&it->second)) return *b ? 1 : 0;
    return fallback;
}

std::uint64_t EventPayload::get_u64(std::string_view key, std::uint64_t fallback) const {
    const auto it = fields.find(std::string(key));
    if (it == fields.end()) return fallback;
    if (const auto* u = std::get_if<std::uint64_t>(&it->second)) return *u;
    if (const auto* i = std::get_if<std::int64_t>(&it->second)) return static_cast<std::uint64_t>(*i);
    if (const auto* d = std::get_if<double>(&it->second)) return static_cast<std::uint64_t>(*d);
    return fallback;
}

bool EventPayload::get_bool(std::string_view key, bool fallback) const {
    const auto it = fields.find(std::string(key));
    if (it == fields.end()) return fallback;
    if (const auto* b = std::get_if<bool>(&it->second)) return *b;
    if (const auto* i = std::get_if<std::int64_t>(&it->second)) return *i != 0;
    if (const auto* d = std::get_if<double>(&it->second)) return *d != 0.0;
    return fallback;
}

std::string EventPayload::get_string(std::string_view key, std::string_view fallback) const {
    const auto it = fields.find(std::string(key));
    if (it == fields.end()) return std::string(fallback);
    if (const auto* s = std::get_if<std::string>(&it->second)) return *s;
    return std::string(fallback);
}

std::uint64_t EventBus::subscribe(std::string event_name, EventCallback callback, std::uint32_t owner_tag) {
    const std::uint64_t id = next_sub_id_++;
    listeners_[std::move(event_name)].push_back(Listener{id, owner_tag, std::move(callback)});
    return id;
}

bool EventBus::unsubscribe(std::uint64_t subscription_id) {
    for (auto& kv : listeners_) {
        auto& vec = kv.second;
        const auto old_size = vec.size();
        vec.erase(
            std::remove_if(vec.begin(), vec.end(), [subscription_id](const Listener& l) { return l.id == subscription_id; }),
            vec.end());
        if (vec.size() != old_size) {
            return true;
        }
    }
    return false;
}

void EventBus::remove_owner_subscriptions(std::uint32_t owner_tag) {
    for (auto& kv : listeners_) {
        auto& vec = kv.second;
        vec.erase(
            std::remove_if(vec.begin(), vec.end(), [owner_tag](const Listener& l) { return l.owner_tag == owner_tag; }),
            vec.end());
    }
}

void EventBus::clear() {
    listeners_.clear();
    queue_.clear();
}

void EventBus::emit(const std::string& event_name, const EventPayload& payload) {
    ++dispatched_count_;
    last_event_name_ = event_name;
    const auto it = listeners_.find(event_name);
    if (it == listeners_.end() || it->second.empty()) {
        return;
    }
    const std::vector<Listener> snapshot = it->second;
    for (const auto& listener : snapshot) {
        if (listener.callback) {
            listener.callback(event_name, payload);
        }
    }
}

void EventBus::enqueue(std::string event_name, EventPayload payload) {
    queue_.push_back(QueuedEvent{std::move(event_name), std::move(payload)});
}

void EventBus::flush() {
    std::uint32_t guard = 0U;
    while (!queue_.empty() && guard < 64U) {
        std::vector<QueuedEvent> batch;
        batch.swap(queue_);
        for (const auto& ev : batch) {
            emit(ev.name, ev.payload);
        }
        ++guard;
    }
}

std::uint32_t EventBus::subscription_count() const {
    std::uint32_t total = 0U;
    for (const auto& kv : listeners_) {
        total += static_cast<std::uint32_t>(kv.second.size());
    }
    return total;
}

}
