#include "streaming/frame_assembler.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <numeric>
#include <span>
#include <stdexcept>
#include <vector>

namespace sasr {
namespace {

std::vector<float> iota(std::size_t n) {
    std::vector<float> v(n);
    std::iota(v.begin(), v.end(), 0.0f);
    return v;
}

// Feeds `audio` in chunks of `chunk` samples; returns the frames emitted
// (including the flushed remainder) as owned vectors.
std::vector<std::vector<float>> frames_from(const std::vector<float>& audio, std::size_t frame,
                                            std::size_t chunk) {
    FrameAssembler framer(frame);
    std::vector<std::vector<float>> out;
    const auto collect = [&out](std::span<const float> f) { out.emplace_back(f.begin(), f.end()); };
    for (std::size_t off = 0; off < audio.size(); off += chunk) {
        const std::size_t n = std::min(chunk, audio.size() - off);
        framer.feed(std::span<const float>(audio).subspan(off, n), collect);
    }
    framer.flush(collect);
    return out;
}

TEST(FrameAssembler, RejectsZeroFrameSize) {
    EXPECT_THROW(FrameAssembler(0), std::invalid_argument);
}

TEST(FrameAssembler, OutputIsIndependentOfInputChunking) {
    const std::vector<float> audio = iota(1000);
    const auto reference = frames_from(audio, 320, 1000);  // one big chunk
    for (const std::size_t chunk : {1UL, 7UL, 180UL, 320UL, 500UL, 641UL}) {
        EXPECT_EQ(frames_from(audio, 320, chunk), reference) << "chunk=" << chunk;
    }
    ASSERT_EQ(reference.size(), 4U);  // 320, 320, 320, then the 40-sample remainder
    EXPECT_EQ(reference[3].size(), 40U);
    EXPECT_EQ(reference[1].front(), 320.0f);  // frames are aligned to the stream
}

TEST(FrameAssembler, LeftoverWaitsForTheNextFeed) {
    FrameAssembler framer(4);
    std::vector<std::vector<float>> out;
    const auto collect = [&out](std::span<const float> f) { out.emplace_back(f.begin(), f.end()); };

    const std::vector<float> a{1, 2, 3};
    framer.feed(a, collect);
    EXPECT_TRUE(out.empty());
    EXPECT_EQ(framer.pending(), 3U);

    const std::vector<float> b{4, 5};
    framer.feed(b, collect);
    ASSERT_EQ(out.size(), 1U);
    EXPECT_EQ(out[0], (std::vector<float>{1, 2, 3, 4}));
    EXPECT_EQ(framer.pending(), 1U);
}

TEST(FrameAssembler, ClearDropsTheLeftover) {
    FrameAssembler framer(4);
    const std::vector<float> a{1, 2, 3};
    std::size_t emitted = 0;
    framer.feed(a, [&](std::span<const float>) { ++emitted; });
    framer.clear();
    framer.flush([&](std::span<const float>) { ++emitted; });
    EXPECT_EQ(emitted, 0U);
}

}  // namespace
}  // namespace sasr
