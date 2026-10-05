#include "profiling/metrics.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <stdexcept>
#include <thread>
#include <vector>

namespace sasr {
namespace {

TEST(LatencyRecorder, EmptyRecorderPercentileIsZero) {
    const LatencyRecorder recorder(10);
    EXPECT_EQ(recorder.count(), 0U);
    EXPECT_EQ(recorder.percentile(50.0), 0);
    EXPECT_EQ(recorder.percentile(99.0), 0);
}

TEST(LatencyRecorder, PercentilesOfOneToOneHundred) {
    LatencyRecorder recorder(100);
    for (std::int64_t v = 1; v <= 100; ++v) {
        recorder.record(v);
    }
    EXPECT_EQ(recorder.count(), 100U);
    EXPECT_EQ(recorder.percentile(50.0), 50);
    EXPECT_EQ(recorder.percentile(90.0), 90);
    EXPECT_EQ(recorder.percentile(95.0), 95);
    EXPECT_EQ(recorder.percentile(99.0), 99);
    EXPECT_EQ(recorder.percentile(100.0), 100);
}

TEST(LatencyRecorder, OrderOfRecordingDoesNotMatter) {
    LatencyRecorder recorder(5);
    for (const std::int64_t v : {50, 10, 40, 20, 30}) {
        recorder.record(v);
    }
    EXPECT_EQ(recorder.percentile(50.0), 30);  // sorted: 10 20 30 40 50
}

TEST(LatencyRecorder, SamplesBeyondCapacityAreDroppedAndCounted) {
    LatencyRecorder recorder(3);
    recorder.record(1);
    recorder.record(2);
    recorder.record(3);
    recorder.record(4);  // dropped: already at capacity
    recorder.record(5);  // dropped

    EXPECT_EQ(recorder.count(), 3U);
    EXPECT_EQ(recorder.dropped(), 2U);
    EXPECT_EQ(recorder.samples(), (std::vector<std::int64_t>{1, 2, 3}));
}

TEST(Percentiles, MatchesIndividualPercentileCalls) {
    LatencyRecorder recorder(100);
    for (std::int64_t v = 1; v <= 100; ++v) {
        recorder.record(v);
    }
    const LatencyPercentiles p = percentiles(recorder);
    EXPECT_EQ(p.p50, recorder.percentile(50.0));
    EXPECT_EQ(p.p90, recorder.percentile(90.0));
    EXPECT_EQ(p.p95, recorder.percentile(95.0));
    EXPECT_EQ(p.p99, recorder.percentile(99.0));
}

TEST(ScopedTimer, RecordsAtLeastTheElapsedSleepDuration) {
    LatencyRecorder recorder(1);
    constexpr std::chrono::microseconds kSleep{5000};
    {
        ScopedTimer timer(recorder);
        std::this_thread::sleep_for(kSleep);
    }
    ASSERT_EQ(recorder.count(), 1U);
    EXPECT_GE(recorder.samples()[0], kSleep.count());
}

TEST(ScopedTimer, RecordsEvenWhenScopeExitsViaException) {
    LatencyRecorder recorder(1);
    try {
        ScopedTimer timer(recorder);
        throw std::runtime_error("boom");
    } catch (const std::runtime_error&) {
        // expected
    }
    EXPECT_EQ(recorder.count(), 1U);
}

TEST(ProcessMetrics, PeakRssAndCpuTimeAreNonNegative) {
    // Sanity checks only: exact values are OS/load-dependent. This mostly
    // guards against getrusage() failing silently and returning garbage.
    EXPECT_GE(peak_rss_kb(), 0);
    EXPECT_GE(process_cpu_time_us(), 0);
}

}  // namespace
}  // namespace sasr
