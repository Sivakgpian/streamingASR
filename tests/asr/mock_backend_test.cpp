#include "asr/mock_backend.hpp"

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <vector>

namespace sasr {
namespace {

TEST(MockBackend, IsNotIncremental) {
    MockBackend backend;
    EXPECT_FALSE(backend.is_incremental());
}

TEST(MockBackend, DecodeTextDependsOnlyOnPcmSize) {
    MockBackend backend;
    auto stream = backend.create_stream();

    const std::array<float, 100> short_audio{};
    const AsrResult first = stream->decode(short_audio);

    const std::array<float, 200> longer_audio{};
    const AsrResult second = stream->decode(longer_audio);

    EXPECT_NE(first.text, second.text);
    EXPECT_FALSE(first.is_final);
    EXPECT_FALSE(second.is_final);
}

TEST(MockBackend, TwoStreamsGivenIdenticalAudioProduceIdenticalResults) {
    MockBackend backend;
    auto stream_a = backend.create_stream();
    auto stream_b = backend.create_stream();

    const std::vector<float> audio(437, 0.0f);

    EXPECT_EQ(stream_a->decode(audio).text, stream_b->decode(audio).text);
    EXPECT_EQ(stream_a->finalize(audio).text, stream_b->finalize(audio).text);
}

TEST(MockBackend, FinalizeMarksResultFinal) {
    MockBackend backend;
    auto stream = backend.create_stream();

    const AsrResult result = stream->finalize(std::array<float, 10>{});
    EXPECT_TRUE(result.is_final);
}

TEST(MockBackend, ResetIsSafeToCallAnytime) {
    MockBackend backend;
    auto stream = backend.create_stream();
    stream->decode(std::array<float, 100>{});

    EXPECT_NO_THROW(stream->reset());
    EXPECT_NO_THROW(stream->reset());  // idempotent
}

TEST(MockBackend, DecodeDelayIsHonoredAndReportedInTimings) {
    const std::chrono::microseconds delay{5000};
    MockBackend backend(delay);
    auto stream = backend.create_stream();

    const auto start = std::chrono::steady_clock::now();
    const AsrResult result = stream->decode(std::array<float, 10>{});
    const auto elapsed = std::chrono::steady_clock::now() - start;

    EXPECT_GE(elapsed, delay);
    EXPECT_EQ(result.timings.infer_us, delay.count());
}

}  // namespace
}  // namespace sasr
