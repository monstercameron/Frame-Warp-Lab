#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <variant>
#include <vector>

namespace frame_warp_lab {

using EventValue = std::variant<double, std::int64_t, std::uint64_t, bool, std::string>;

struct EventPayload {
    std::unordered_map<std::string, EventValue> fields;

    void set_number(std::string key, double value) { fields[std::move(key)] = value; }
    void set_int(std::string key, std::int64_t value) { fields[std::move(key)] = value; }
    void set_u64(std::string key, std::uint64_t value) { fields[std::move(key)] = value; }
    void set_bool(std::string key, bool value) { fields[std::move(key)] = value; }
    void set_string(std::string key, std::string value) { fields[std::move(key)] = std::move(value); }

    double get_number(std::string_view key, double fallback = 0.0) const;
    std::int64_t get_int(std::string_view key, std::int64_t fallback = 0) const;
    std::uint64_t get_u64(std::string_view key, std::uint64_t fallback = 0ULL) const;
    bool get_bool(std::string_view key, bool fallback = false) const;
    std::string get_string(std::string_view key, std::string_view fallback = {}) const;
};

using EventCallback = std::function<void(const std::string&, const EventPayload&)>;

class EventBus {
public:
    std::uint64_t subscribe(std::string event_name, EventCallback callback, std::uint32_t owner_tag = 0U);
    bool unsubscribe(std::uint64_t subscription_id);
    void remove_owner_subscriptions(std::uint32_t owner_tag);
    void clear();

    void emit(const std::string& event_name, const EventPayload& payload = {});
    void enqueue(std::string event_name, EventPayload payload = {});
    void flush();

    std::uint64_t dispatched_count() const { return dispatched_count_; }
    std::uint32_t subscription_count() const;
    const std::string& last_event_name() const { return last_event_name_; }

private:
    struct Listener {
        std::uint64_t id = 0ULL;
        std::uint32_t owner_tag = 0U;
        EventCallback callback;
    };

    struct QueuedEvent {
        std::string name;
        EventPayload payload;
    };

    std::uint64_t next_sub_id_ = 1ULL;
    std::uint64_t dispatched_count_ = 0ULL;
    std::string last_event_name_;
    std::unordered_map<std::string, std::vector<Listener>> listeners_;
    std::vector<QueuedEvent> queue_;
};

}
