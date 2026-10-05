// Separate, dedicated binary: linking sasr::test_alloc_counter overrides
// global operator new/delete for this whole process (see
// tests/support/alloc_counter.hpp), so it stays isolated from the ordinary
// correctness tests in wav_reader_test.cpp.
#include "audio/wav_reader.hpp"
#include "support/alloc_counter.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <system_error>

namespace sasr {
namespace {

void put_le(std::string& out, std::uint32_t value, int bytes) {
    for (int i = 0; i < bytes; ++i) {
        out.push_back(static_cast<char>((value >> (8 * i)) & 0xFF));
    }
}

// All-zero-sample WAV of the given length; contents don't matter here.
std::string make_wav(std::size_t sample_count) {
    std::string wav = "RIFF";
    put_le(wav, static_cast<std::uint32_t>(36 + sample_count * 2), 4);
    wav += "WAVEfmt ";
    put_le(wav, 16, 4);
    put_le(wav, 1, 2);
    put_le(wav, 1, 2);
    put_le(wav, 16000, 4);
    put_le(wav, 32000, 4);
    put_le(wav, 2, 2);
    put_le(wav, 16, 2);
    wav += "data";
    put_le(wav, static_cast<std::uint32_t>(sample_count * 2), 4);
    wav.resize(wav.size() + sample_count * 2, '\0');
    return wav;
}

struct TempFile {
    std::string path = (std::filesystem::temp_directory_path() /
                        ("sasr_alloc_" + std::to_string(std::random_device{}()) + ".wav"))
                           .string();
    explicit TempFile(const std::string& bytes) {
        std::ofstream(path, std::ios::binary) << bytes;
    }
    ~TempFile() {
        std::error_code ignored;
        std::filesystem::remove(path, ignored);
    }
};

TEST(WavReaderAlloc, ReadIntoDoesNotAllocate) {
    constexpr std::size_t kFrames = 500;
    constexpr std::size_t kFrameSamples = 320;
    const TempFile file(make_wav(kFrames * kFrameSamples));

    const Config config;                     // frame_ms=20 -> scratch_ sized to 320
    WavReader reader(file.path, config);      // allocates scratch_ once, before the bracket

    std::array<float, kFrameSamples> out{};
    const auto before = test::allocation_count();
    for (std::size_t i = 0; i < kFrames; ++i) {
        ASSERT_EQ(reader.read_into(out), kFrameSamples);
    }
    EXPECT_EQ(test::allocation_count(), before);
}

TEST(WavReaderAlloc, ReadFrameAllocatesOncePerCall) {
    // Documents, by contrast, the cost read_into() avoids: this is the
    // convenience API, not the hot-path one.
    const TempFile file(make_wav(320 * 3));
    const Config config;
    WavReader reader(file.path, config);

    const auto before = test::allocation_count();
    const auto frame = reader.read_frame();
    ASSERT_TRUE(frame.has_value());
    EXPECT_GT(test::allocation_count(), before);
}

}  // namespace
}  // namespace sasr
