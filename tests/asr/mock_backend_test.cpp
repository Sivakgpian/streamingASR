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

TEST(MockBackend, DecodeTextDependsOnlyOnSamplesAccepted) {
    MockBackend backend;
    auto stream = backend.create_stream();

    const std::array<float, 100> chunk{};
    stream->accept(chunk);
    const AsrResult first = stream->decode();

    stream->accept(chunk);
    const AsrResult second = stream->decode();

    EXPECT_NE(first.text, second.text);  // sample count changed
    EXPECT_FALSE(first.is_final);
    EXPECT_FALSE(second.is_final);
}

TEST(MockBackend, TwoStreamsFedIdenticalAudioProduceIdenticalResults) {
    MockBackend backend;
    auto stream_a = backend.create_stream();
    auto stream_b = backend.create_stream();

    const std::vector<float> audio(437, 0.0f);
    stream_a->accept(audio);
    stream_b->accept(audio);

    EXPECT_EQ(stream_a->decode().text, stream_b->decode().text);
    EXPECT_EQ(stream_a->finalize().text, stream_b->finalize().text);
}

TEST(MockBackend, FinalizeMarksResultFinal) {
    MockBackend backend;
    auto stream = backend.create_stream();
    stream->accept(std::array<float, 10>{});

    const AsrResult result = stream->finalize();
    EXPECT_TRUE(result.is_final);
}

TEST(MockBackend, ResetClearsAcceptedSamples) {
    MockBackend backend;
    auto stream = backend.create_stream();
    stream->accept(std::array<float, 100>{});

    stream->reset();
    const AsrResult after_reset = stream->decode();

    auto fresh_stream = backend.create_stream();
    const AsrResult fresh = fresh_stream->decode();
    EXPECT_EQ(after_reset.text, fresh.text);  // both saw 0 samples
}

TEST(MockBackend, DecodeDelayIsHonoredAndReportedInTimings) {
    const std::chrono::microseconds delay{5000};
    MockBackend backend(delay);
    auto stream = backend.create_stream();

    const auto start = std::chrono::steady_clock::now();
    const AsrResult result = stream->decode();
    const auto elapsed = std::chrono::steady_clock::now() - start;

    EXPECT_GE(elapsed, delay);
    EXPECT_EQ(result.timings.infer_us, delay.count());
}

}  // namespace
}  // namespace sasr
