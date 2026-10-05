#include "profiling/metrics.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>

#include <sys/resource.h>

namespace sasr {

std::int64_t LatencyRecorder::percentile(double p) const {
    if (samples_.empty()) {
        return 0;
    }
    std::vector<std::int64_t> sorted = samples_;  // copy: reporting-time only, not on the hot path
    std::ranges::sort(sorted);

    // Nearest-rank method: the smallest sample such that at least p% of
    // samples are <= it. rank is 1-based and clamped into [1, n].
    const auto n = static_cast<double>(sorted.size());
    auto rank = static_cast<std::size_t>(std::ceil(p / 100.0 * n));
    rank = std::clamp<std::size_t>(rank, 1, sorted.size());
    return sorted[rank - 1];
}

LatencyPercentiles percentiles(const LatencyRecorder& recorder) {
    LatencyPercentiles result;
    result.p50 = recorder.percentile(50.0);
    result.p90 = recorder.percentile(90.0);
    result.p95 = recorder.percentile(95.0);
    result.p99 = recorder.percentile(99.0);
    return result;
}

long peak_rss_kb() noexcept {
    struct rusage usage{};
    if (getrusage(RUSAGE_SELF, &usage) != 0) {
        return 0;
    }
    return usage.ru_maxrss;  // Linux: already KiB (unlike macOS, which uses bytes)
}

std::int64_t process_cpu_time_us() noexcept {
    struct rusage usage{};
    if (getrusage(RUSAGE_SELF, &usage) != 0) {
        return 0;
    }
    const auto to_us = [](const timeval& tv) {
        return static_cast<std::int64_t>(tv.tv_sec) * 1'000'000 + tv.tv_usec;
    };
    return to_us(usage.ru_utime) + to_us(usage.ru_stime);
}

}  // namespace sasr
