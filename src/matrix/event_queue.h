#pragma once
#include <queue>
#include <string>
#include <mutex>
#include <condition_variable>

namespace Matrix {

enum class SyncEventType {
    SYNC_READY,   // a /sync response has been staged and is ready to parse
    ERROR,        // the sync worker hit a network/auth error worth surfacing
    UNKNOWN,
};

struct SyncEvent {
    SyncEventType type = SyncEventType::UNKNOWN;
    std::string   message;   // human-readable detail for ERROR events
};

class EventQueue {
public:
    void push(SyncEvent ev) {
        std::lock_guard<std::mutex> lock(mutex_);
        queue_.push(std::move(ev));
        cv_.notify_one();
    }

    bool try_pop(SyncEvent &ev) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (queue_.empty()) return false;
        ev = std::move(queue_.front());
        queue_.pop();
        return true;
    }

    bool wait_pop(SyncEvent &ev, int timeout_ms = 100) {
        std::unique_lock<std::mutex> lock(mutex_);
        if (cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                         [this] { return !queue_.empty(); })) {
            ev = std::move(queue_.front());
            queue_.pop();
            return true;
        }
        return false;
    }

    void clear() {
        std::lock_guard<std::mutex> lock(mutex_);
        while (!queue_.empty()) queue_.pop();
    }

private:
    std::queue<SyncEvent> queue_;
    std::mutex mutex_;
    std::condition_variable cv_;
};

} // namespace Matrix
