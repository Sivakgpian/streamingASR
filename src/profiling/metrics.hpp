#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace sasr {

// Collects latency samples (microseconds) into a fixed-capacity,
// preallocated buffer. The point of measuring latency is to not perturb
// it: record() never allocates once the reserved capacity is reached,
// it just stops recording and counts what it dropped instead of
// growing. Not thread-safe (each session/worker gets its own).
class LatencyRecorder {
public:
    explicit LatencyRecorder(std::size_t reserve_capacity) { samples_.reserve(reserve_capacity); }

    void record(std::int64_t microseconds) {
        if (samples_.size() < samples_.capacity()) {
            samples_.push_back(microseconds);
        } else {
            ++dropped_;
        }
    }

    [[nodiscard]] std::size_t count() const noexcept { return samples_.size(); }
    [[nodiscard]] std::size_t dropped() const noexcept { return dropped_; }
    [[nodiscard]] const std::vector<std::int64_t>& samples() const noexcept { return samples_; }

    // The p-th percentile (0 <= p <= 100) by the nearest-rank method.
    // Returns 0 if no samples were recorded. This sorts a *copy* of the
    // samples: a reporting-time cost, never on the path being measured.
    [[nodiscard]] std::int64_t percentile(double p) const;

private:
    std::vector<std::int64_t> samples_;
    std::size_t dropped_ = 0;
};

// The set this project reports everywhere (CLAUDE.md: "Report
// p50/p90/p95/p99, never only averages").
struct LatencyPercentiles {
    std::int64_t p50 = 0;
    std::int64_t p90 = 0;
    std::int64_t p95 = 0;
    std::int64_t p99 = 0;
};

[[nodiscard]] LatencyPercentiles percentiles(const LatencyRecorder& recorder);

// RAII stopwatch: records elapsed wall time into `recorder` when it goes
// out of scope (including via an exception). Never allocates itself.
class ScopedTimer {
public:
    explicit ScopedTimer(LatencyRecorder& recorder) noexcept
        : recorder_(&recorder), start_(std::chrono::steady_clock::now()) {}

    ~ScopedTimer() {
        const auto elapsed = std::chrono::steady_clock::now() - start_;
        recorder_->record(std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count());
    }

    ScopedTimer(const ScopedTimer&) = delete;
    ScopedTimer& operator=(const ScopedTimer&) = delete;
    ScopedTimer(ScopedTimer&&) = delete;
    ScopedTimer& operator=(ScopedTimer&&) = delete;

private:
    LatencyRecorder* recorder_;
    std::chrono::steady_clock::time_point start_;
};

// Peak resident set size in KiB since process start (Linux: ru_maxrss,
// already in KiB there). POSIX/Linux only -- this project targets Linux
// (WSL2 now, embedded Linux/ARM later); revisit if a non-POSIX target is
// ever added. Returns 0 if unavailable.
[[nodiscard]] long peak_rss_kb() noexcept;

// Total user+system CPU time this process has consumed so far, in
// microseconds (POSIX getrusage). Divide a delta of this by a delta of
// wall time to get CPU utilization over some interval.
[[nodiscard]] std::int64_t process_cpu_time_us() noexcept;

}  // namespace sasr
