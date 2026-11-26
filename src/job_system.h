#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace frame_warp_lab {

class EngineJobSystem {
public:
    explicit EngineJobSystem(std::uint32_t requested_workers = 0U);
    ~EngineJobSystem();

    EngineJobSystem(const EngineJobSystem&) = delete;
    EngineJobSystem& operator=(const EngineJobSystem&) = delete;

    static EngineJobSystem& global();

    void dispatch(std::function<void()> task);
    void parallel_for(std::size_t item_count, std::size_t grain_size, const std::function<void(std::size_t, std::size_t)>& range_fn);
    void wait_idle();

    std::uint32_t worker_count() const { return static_cast<std::uint32_t>(workers_.size()); }
    std::uint64_t jobs_executed() const { return jobs_executed_.load(std::memory_order_relaxed); }

private:
    void worker_loop();

    std::vector<std::thread> workers_;
    std::deque<std::function<void()>> queue_;
    std::mutex mutex_;
    std::condition_variable wake_cv_;
    std::condition_variable idle_cv_;
    std::atomic<std::uint64_t> jobs_executed_{0ULL};
    std::size_t active_tasks_ = 0U;
    bool stopping_ = false;
};

}
