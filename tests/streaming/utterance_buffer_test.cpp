#include "streaming/utterance_buffer.hpp"

#include <gtest/gtest.h>

#include <array>
#include <stdexcept>
#include <vector>

namespace sasr {
namespace {

TEST(UtteranceBuffer, StartsEmpty) {
    const UtteranceBuffer buf(100);
    EXPECT_EQ(buf.size(), 0U);
    EXPECT_EQ(buf.capacity(), 100U);
    EXPECT_FALSE(buf.full());
    EXPECT_TRUE(buf.view().empty());
}

TEST(UtteranceBuffer, RejectsZeroCapacity) {
    EXPECT_THROW(UtteranceBuffer(0), std::invalid_argument);
}

TEST(UtteranceBuffer, AppendAccumulatesAndViewIsContiguous) {
    UtteranceBuffer buf(10);
    const std::array<float, 4> a{1, 2, 3, 4};
    const std::array<float, 3> b{5, 6, 7};

    EXPECT_EQ(buf.append(a), 4U);
    EXPECT_EQ(buf.append(b), 3U);
    EXPECT_EQ(buf.size(), 7U);

    const auto view = buf.view();
    EXPECT_EQ(std::vector<float>(view.begin(), view.end()),
              (std::vector<float>{1, 2, 3, 4, 5, 6, 7}));
}

TEST(UtteranceBuffer, AppendPastCapacityWritesOnlyWhatFits) {
    UtteranceBuffer buf(5);
    const std::array<float, 8> in{1, 2, 3, 4, 5, 6, 7, 8};

    EXPECT_EQ(buf.append(in), 5U);  // only the first 5 fit
    EXPECT_TRUE(buf.full());
    EXPECT_EQ(buf.append(in), 0U);  // already full: nothing more fits

    const auto view = buf.view();
    EXPECT_EQ(std::vector<float>(view.begin(), view.end()),
              (std::vector<float>{1, 2, 3, 4, 5}));
}

TEST(UtteranceBuffer, ClearResetsSizeButKeepsCapacity) {
    UtteranceBuffer buf(10);
    const std::array<float, 5> in{1, 2, 3, 4, 5};
    buf.append(in);

    buf.clear();

    EXPECT_EQ(buf.size(), 0U);
    EXPECT_EQ(buf.capacity(), 10U);
    EXPECT_TRUE(buf.view().empty());
    EXPECT_EQ(buf.append(in), 5U);  // storage is still usable after clear
}

}  // namespace
}  // namespace sasr
