#pragma once

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <span>
#include <stdexcept>
#include <vector>

namespace sasr {

// A bounded, preallocated single-producer/single-consumer ring of
// fixed-size audio frames. Exactly one thread may call try_push()
// (the producer); exactly one thread -- not necessarily the same one --
// may call try_pop() (the consumer); any other combination is undefined
// behavior (this is not a general-purpose MPMC queue).
//
// Used as the handoff between whatever thread feeds a session's audio
// (the producer) and ThreadedEngine's background thread (the consumer):
// try_push() never blocks and never allocates, so ingestion is never
// stalled behind however fast the engine is draining (CLAUDE.md: "no
// blocking of ingestion").
//
// Correctness: head_/tail_ are monotonically increasing counters
// (wrapping only at std::size_t's range, i.e. never in practice); a
// slot index is `counter % capacity`. They sit on separate cache lines
// (alignas(64)) because the producer writes tail_ and the consumer
// writes head_ -- on the same line they'd cause false sharing between
// the two threads' hot variables. The release (on publish) / acquire
// (on observe) pair on each counter is what makes a slot's data,
// written by the producer before the release store, visible to the
// consumer after the matching acquire load; this is the standard
// SPSC ring handoff pattern.
class SpscAudioRing {
public:
    // capacity (number of frame slots) and max_frame_samples (the
    // largest single push()) must both be > 0.
    SpscAudioRing(std::size_t capacity, std::size_t max_frame_samples)
        : capacity_(capacity),
          max_frame_samples_(max_frame_samples),
          storage_(capacity * max_frame_samples),
          sizes_(capacity, 0) {
        if (capacity == 0 || max_frame_samples == 0) {
            throw std::invalid_argument(
                "SpscAudioRing: capacity and max_frame_samples must be > 0");
        }
    }

    // Copies pcm into the next free slot. Returns false -- without
    // blocking or allocating -- if the ring is full or pcm.size()
    // exceeds max_frame_samples (the latter is a caller sizing error,
    // not a transient condition, but is reported the same way rather
    // than throwing, since this method must never throw from the
    // producer's hot path).
    bool try_push(std::span<const float> pcm) noexcept {
        if (pcm.size() > max_frame_samples_) {
            return false;
        }
        const std::size_t tail = tail_.load(std::memory_order_relaxed);
        const std::size_t head = head_.load(std::memory_order_acquire);
        if (tail - head >= capacity_) {
            return false;  // full
        }
        const std::size_t slot = tail % capacity_;
        std::copy(pcm.begin(), pcm.end(),
                 storage_.begin() + static_cast<std::ptrdiff_t>(slot * max_frame_samples_));
        sizes_[slot] = pcm.size();
        tail_.store(tail + 1, std::memory_order_release);
        return true;
    }

    // Copies the oldest unread frame's samples into `out` (cleared and
    // resized as needed -- amortized allocation-free: `out` is the
    // consumer's own reused scratch buffer, and frame sizes are
    // typically constant, so after a warm-up call this stops
    // reallocating). Returns false if the ring is empty.
    bool try_pop(std::vector<float>& out) {
        const std::size_t head = head_.load(std::memory_order_relaxed);
        const std::size_t tail = tail_.load(std::memory_order_acquire);
        if (head == tail) {
            return false;  // empty
        }
        const std::size_t slot = head % capacity_;
        const std::size_t n = sizes_[slot];
        const auto begin = storage_.begin() + static_cast<std::ptrdiff_t>(slot * max_frame_samples_);
        out.assign(begin, begin + static_cast<std::ptrdiff_t>(n));
        head_.store(head + 1, std::memory_order_release);
        return true;
    }

    [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }

    // Consumer thread only: exact as of this call -- everything the
    // producer published before any synchronization point the consumer
    // has since observed (e.g. a mutex the producer released after its
    // last push) is accounted for.
    [[nodiscard]] bool consumer_empty() const noexcept {
        return head_.load(std::memory_order_relaxed) == tail_.load(std::memory_order_acquire);
    }

    // Approximate occupancy: a racy snapshot if called from a third
    // thread (neither the producer nor the consumer), fine for
    // diagnostics/metrics, not for correctness decisions.
    [[nodiscard]] std::size_t size_approx() const noexcept {
        return tail_.load(std::memory_order_relaxed) - head_.load(std::memory_order_relaxed);
    }

private:
    std::size_t capacity_;
    std::size_t max_frame_samples_;
    std::vector<float> storage_;       // capacity_ * max_frame_samples_, laid out slot by slot
    std::vector<std::size_t> sizes_;   // actual sample count used in each slot

    alignas(64) std::atomic<std::size_t> head_{0};  // written only by the consumer
    alignas(64) std::atomic<std::size_t> tail_{0};  // written only by the producer
};

}  // namespace sasr
