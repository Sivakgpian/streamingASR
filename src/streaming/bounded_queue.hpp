#pragma once

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <utility>

namespace sasr {

// Bounded, blocking multi-producer/multi-consumer queue (mutex + two
// condition variables). Meant for low-rate control-plane traffic -- ASR
// jobs and their completions, on the order of one per second per
// session -- where a mutex costs nothing measurable and is far simpler
// to get right than a lock-free structure. The per-frame audio path
// uses SpscAudioRing instead, which never blocks. push() may allocate
// (std::deque node); acceptable at this rate, never on the audio path.
//
// close() wakes every blocked push()/pop(). After close(), push()
// refuses (returns false) and pop() keeps returning what is left until
// the queue is drained, then returns std::nullopt -- so a consumer
// shutting down still sees everything enqueued before close().
template <typename T>
class BoundedQueue {
public:
    explicit BoundedQueue(std::size_t capacity) : capacity_(capacity) {
        if (capacity == 0) {
            throw std::invalid_argument("BoundedQueue: capacity must be > 0");
        }
    }

    BoundedQueue(const BoundedQueue&) = delete;
    BoundedQueue& operator=(const BoundedQueue&) = delete;

    // Blocks while full. Returns false (item not enqueued) if the queue
    // is or becomes closed.
    bool push(T item) {
        std::unique_lock lock(mutex_);
        not_full_.wait(lock, [&] { return closed_ || items_.size() < capacity_; });
        if (closed_) {
            return false;
        }
        items_.push_back(std::move(item));
        lock.unlock();
        not_empty_.notify_one();
        return true;
    }

    // Blocks while empty and open. Returns std::nullopt only once the
    // queue is closed and drained.
    std::optional<T> pop() {
        std::unique_lock lock(mutex_);
        not_empty_.wait(lock, [&] { return closed_ || !items_.empty(); });
        return take(lock);
    }

    // Never blocks: std::nullopt if nothing is queued right now.
    std::optional<T> try_pop() {
        std::unique_lock lock(mutex_);
        return take(lock);
    }

    void close() {
        {
            const std::lock_guard lock(mutex_);
            closed_ = true;
        }
        not_empty_.notify_all();
        not_full_.notify_all();
    }

    [[nodiscard]] std::size_t size() const {
        const std::lock_guard lock(mutex_);
        return items_.size();
    }

    [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }

private:
    std::optional<T> take(std::unique_lock<std::mutex>& lock) {
        if (items_.empty()) {
            return std::nullopt;
        }
        std::optional<T> item(std::move(items_.front()));
        items_.pop_front();
        lock.unlock();
        not_full_.notify_one();
        return item;
    }

    const std::size_t capacity_;
    mutable std::mutex mutex_;
    std::condition_variable not_full_;
    std::condition_variable not_empty_;
    std::deque<T> items_;
    bool closed_ = false;
};

}  // namespace sasr
