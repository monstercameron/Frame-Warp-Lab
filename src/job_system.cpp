#include "job_system.h"

#include <algorithm>

namespace frame_warp_lab {

EngineJobSystem::EngineJobSystem(std::uint32_t requested_workers) {
    const std::uint32_t hw = std::max(1U, std::thread::hardware_concurrency());
    const std::uint32_t default_workers = hw > 3U ? std::min<std::uint32_t>(hw - 2U, 8U) : std::max<std::uint32_t>(2U, hw);
    const std::uint32_t count = requested_workers > 0U ? std::min<std::uint32_t>(requested_workers, 16U) : default_workers;
    workers_.reserve(count);
    for (std::uint32_t i = 0U; i < count; ++i) {
        workers_.emplace_back([this]() { worker_loop(); });
    }
}

EngineJobSystem::~EngineJobSystem() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = true;
    }
    wake_cv_.notify_all();
    for (auto& t : workers_) {
        if (t.joinable()) {
            t.join();
        }
    }
}

EngineJobSystem& EngineJobSystem::global() {
    static EngineJobSystem instance;
    return instance;
}

void EngineJobSystem::dispatch(std::function<void()> task) {
    if (!task) return;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        queue_.push_back(std::move(task));
        ++active_tasks_;
    }
    wake_cv_.notify_one();
}

void EngineJobSystem::parallel_for(
    std::size_t item_count,
    std::size_t grain_size,
    const std::function<void(std::size_t, std::size_t)>& range_fn) {
    if (item_count == 0U) return;
    const std::size_t grain = std::max<std::size_t>(1U, grain_size);
    if (item_count <= grain || workers_.empty()) {
        range_fn(0U, item_count);
        jobs_executed_.fetch_add(1ULL, std::memory_order_relaxed);
        return;
    }

    const std::size_t batch_count = (item_count + grain - 1U) / grain;
    if (batch_count <= 1U) {
        range_fn(0U, item_count);
        jobs_executed_.fetch_add(1ULL, std::memory_order_relaxed);
        return;
    }

    std::atomic<std::size_t> remaining{batch_count - 1U};
    std::mutex done_mutex;
    std::condition_variable done_cv;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (std::size_t b = 1U; b < batch_count; ++b) {
            const std::size_t begin = b * grain;
            const std::size_t end = std::min(item_count, begin + grain);
            queue_.push_back([begin, end, &range_fn, &remaining, &done_mutex, &done_cv]() {
                range_fn(begin, end);
                if (remaining.fetch_sub(1U, std::memory_order_acq_rel) == 1U) {
                    std::lock_guard<std::mutex> dl(done_mutex);
                    done_cv.notify_one();
                }
            });
            ++active_tasks_;
        }
    }
    wake_cv_.notify_all();

    range_fn(0U, std::min(item_count, grain));
    jobs_executed_.fetch_add(1ULL, std::memory_order_relaxed);

    if (remaining.load(std::memory_order_acquire) > 0U) {
        std::unique_lock<std::mutex> dl(done_mutex);
        done_cv.wait(dl, [&remaining]() { return remaining.load(std::memory_order_acquire) == 0U; });
    }
}

void EngineJobSystem::wait_idle() {
    std::unique_lock<std::mutex> lock(mutex_);
    idle_cv_.wait(lock, [this]() { return active_tasks_ == 0U && queue_.empty(); });
}

void EngineJobSystem::worker_loop() {
    for (;;) {
        std::function<void()> task;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            wake_cv_.wait(lock, [this]() { return stopping_ || !queue_.empty(); });
            if (stopping_ && queue_.empty()) {
                return;
            }
            task = std::move(queue_.front());
            queue_.pop_front();
        }
        task();
        jobs_executed_.fetch_add(1ULL, std::memory_order_relaxed);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (active_tasks_ > 0U) {
                --active_tasks_;
            }
            if (active_tasks_ == 0U && queue_.empty()) {
                idle_cv_.notify_all();
            }
        }
    }
}

}
