#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>

class MonitorSignal {
public:
    void notify() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            requested_ = true;
        }
        condition_.notify_one();
    }

    bool waitForNext(int seconds, const std::atomic<bool>& running) {
        std::unique_lock<std::mutex> lock(mutex_);
        condition_.wait_for(lock, std::chrono::seconds(seconds > 0 ? seconds : 1), [this, &running] {
            return requested_ || !running.load();
        });
        requested_ = false;
        return running.load();
    }

private:
    std::mutex mutex_;
    std::condition_variable condition_;
    bool requested_ = false;
};