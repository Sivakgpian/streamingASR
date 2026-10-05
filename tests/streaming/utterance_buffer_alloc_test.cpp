// Separate binary from utterance_buffer_test.cpp: see
// tests/audio/wav_reader_alloc_test.cpp for why.
#include "streaming/utterance_buffer.hpp"
#include "support/alloc_counter.hpp"

#include <gtest/gtest.h>

#include <array>

namespace sasr {
namespace {

TEST(UtteranceBufferAlloc, AppendAndClearDoNotAllocate) {
    UtteranceBuffer buf(16000);  // allocates once, here, before the bracket
    const std::array<float, 320> frame{};

    const auto before = test::allocation_count();
    for (int i = 0; i < 20; ++i) {
        buf.append(frame);
    }
    buf.clear();
    for (int i = 0; i < 20; ++i) {
        buf.append(frame);
    }
    EXPECT_EQ(test::allocation_count(), before);
}

}  // namespace
}  // namespace sasr
