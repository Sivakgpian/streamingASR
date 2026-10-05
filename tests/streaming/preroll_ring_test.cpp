#include "streaming/preroll_ring.hpp"

#include <gtest/gtest.h>

#include <array>
#include <vector>

namespace sasr {
namespace {

std::vector<float> drained(const PrerollRing& ring) {
    UtteranceBuffer buf(ring.capacity() + 16);  // plenty of room; not what's under test
    ring.drain_into(buf);
    const auto view = buf.view();
    return {view.begin(), view.end()};
}

TEST(PrerollRing, EmptyRingDrainsNothing) {
    const PrerollRing ring(10);
    EXPECT_EQ(ring.size(), 0U);
    EXPECT_EQ(drained(ring), std::vector<float>{});
}

TEST(PrerollRing, ZeroCapacityIsANoOp) {
    PrerollRing ring(0);
    ring.push(std::array<float, 5>{1, 2, 3, 4, 5});
    EXPECT_EQ(ring.size(), 0U);
    EXPECT_EQ(drained(ring), std::vector<float>{});
}

TEST(PrerollRing, PartiallyFilledRingDrainsInOrderWithoutWrapping) {
    PrerollRing ring(10);
    ring.push(std::array<float, 4>{1, 2, 3, 4});
    EXPECT_EQ(ring.size(), 4U);
    EXPECT_EQ(drained(ring), (std::vector<float>{1, 2, 3, 4}));
}

TEST(PrerollRing, FullRingKeepsOnlyTheMostRecentSamples) {
    PrerollRing ring(5);
    ring.push(std::array<float, 3>{1, 2, 3});
    ring.push(std::array<float, 4>{4, 5, 6, 7});  // ring now wraps: oldest (1,2) overwritten
    EXPECT_EQ(ring.size(), 5U);
    EXPECT_EQ(drained(ring), (std::vector<float>{3, 4, 5, 6, 7}));
}

TEST(PrerollRing, SingleFrameLargerThanCapacityKeepsOnlyItsTail) {
    PrerollRing ring(3);
    ring.push(std::array<float, 6>{1, 2, 3, 4, 5, 6});
    EXPECT_EQ(ring.size(), 3U);
    EXPECT_EQ(drained(ring), (std::vector<float>{4, 5, 6}));
}

TEST(PrerollRing, ClearResetsWithoutReleasingStorage) {
    PrerollRing ring(5);
    ring.push(std::array<float, 5>{1, 2, 3, 4, 5});
    ring.clear();
    EXPECT_EQ(ring.size(), 0U);
    EXPECT_EQ(drained(ring), std::vector<float>{});

    ring.push(std::array<float, 2>{9, 9});
    EXPECT_EQ(drained(ring), (std::vector<float>{9, 9}));
}

TEST(PrerollRing, PushAfterWrapContinuesCorrectly) {
    PrerollRing ring(4);
    ring.push(std::array<float, 4>{1, 2, 3, 4});  // exactly full, write_pos_ wraps to 0
    ring.push(std::array<float, 2>{5, 6});        // overwrites the oldest two (1, 2)
    EXPECT_EQ(drained(ring), (std::vector<float>{3, 4, 5, 6}));
}

}  // namespace
}  // namespace sasr
