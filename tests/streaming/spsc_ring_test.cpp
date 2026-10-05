#include "streaming/spsc_ring.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <stdexcept>
#include <thread>
#include <vector>

namespace sasr {
namespace {

TEST(SpscAudioRing, RejectsZeroSizes) {
    EXPECT_THROW(SpscAudioRing(0, 4), std::invalid_argument);
    EXPECT_THROW(SpscAudioRing(4, 0), std::invalid_argument);
}

TEST(SpscAudioRing, PopOnEmptyReturnsFalse) {
    SpscAudioRing ring(2, 4);
    std::vector<float> out;
    EXPECT_TRUE(ring.consumer_empty());
    EXPECT_FALSE(ring.try_pop(out));
}

TEST(SpscAudioRing, RoundTripPreservesFramesSizesAndOrder) {
    SpscAudioRing ring(4, 4);
    ASSERT_TRUE(ring.try_push(std::array<float, 3>{1, 2, 3}));
    ASSERT_TRUE(ring.try_push(std::array<float, 1>{4}));

    std::vector<float> out;
    ASSERT_TRUE(ring.try_pop(out));
    EXPECT_EQ(out, (std::vector<float>{1, 2, 3}));
    ASSERT_TRUE(ring.try_pop(out));
    EXPECT_EQ(out, (std::vector<float>{4}));
    EXPECT_FALSE(ring.try_pop(out));
}

TEST(SpscAudioRing, FullRingRefusesWithoutBlockingUntilAPopFreesASlot) {
    SpscAudioRing ring(2, 1);
    EXPECT_TRUE(ring.try_push(std::array<float, 1>{1}));
    EXPECT_TRUE(ring.try_push(std::array<float, 1>{2}));
    EXPECT_FALSE(ring.try_push(std::array<float, 1>{3}));  // full

    std::vector<float> out;
    ASSERT_TRUE(ring.try_pop(out));
    EXPECT_TRUE(ring.try_push(std::array<float, 1>{3}));
}

TEST(SpscAudioRing, OversizedFrameIsRefused) {
    SpscAudioRing ring(2, 2);
    EXPECT_FALSE(ring.try_push(std::array<float, 3>{1, 2, 3}));
    EXPECT_TRUE(ring.consumer_empty());
}

TEST(SpscAudioRing, WrapsAroundCorrectlyOverManyCycles) {
    SpscAudioRing ring(3, 1);
    std::vector<float> out;
    for (int i = 0; i < 1000; ++i) {
        ASSERT_TRUE(ring.try_push(std::array<float, 1>{static_cast<float>(i)}));
        ASSERT_TRUE(ring.try_pop(out));
        ASSERT_EQ(out.front(), static_cast<float>(i));
    }
}

// One producer thread, one consumer thread: every frame must arrive
// exactly once, in order, with its contents intact. Under the TSan
// presets this checks the release/acquire handoff claimed in the header.
TEST(SpscAudioRing, TwoThreadsDeliverEveryFrameInOrder) {
    constexpr std::size_t kFrames = 20000;
    constexpr std::size_t kFrameSamples = 4;
    SpscAudioRing ring(8, kFrameSamples);

    std::thread producer([&ring] {
        std::array<float, kFrameSamples> frame{};
        for (std::size_t seq = 0; seq < kFrames; ++seq) {
            frame.fill(static_cast<float>(seq));
            while (!ring.try_push(frame)) {
                std::this_thread::yield();
            }
        }
    });

    std::vector<float> out;
    for (std::size_t seq = 0; seq < kFrames; ++seq) {
        while (!ring.try_pop(out)) {
            std::this_thread::yield();
        }
        ASSERT_EQ(out.size(), kFrameSamples);
        for (const float s : out) {
            ASSERT_EQ(s, static_cast<float>(seq));
        }
    }
    producer.join();
    EXPECT_TRUE(ring.consumer_empty());
}

}  // namespace
}  // namespace sasr
