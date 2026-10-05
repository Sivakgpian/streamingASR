#include "audio/wav_reader.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <system_error>
#include <vector>

namespace sasr {
namespace {

// ---- helpers ---------------------------------------------------------------

void put_le(std::string& out, std::uint32_t value, int bytes) {
    for (int i = 0; i < bytes; ++i) {
        out.push_back(static_cast<char>((value >> (8 * i)) & 0xFF));
    }
}

// Builds the bytes of a WAV file in memory.
std::string make_wav(const std::vector<std::int16_t>& samples, std::uint32_t channels = 1,
                     std::uint32_t rate = 16000) {
    std::string wav = "RIFF";
    put_le(wav, static_cast<std::uint32_t>(36 + samples.size() * 2), 4);
    wav += "WAVEfmt ";
    put_le(wav, 16, 4);                     // fmt size
    put_le(wav, 1, 2);                      // PCM
    put_le(wav, channels, 2);
    put_le(wav, rate, 4);
    put_le(wav, rate * channels * 2, 4);    // byte rate
    put_le(wav, channels * 2, 2);           // block align
    put_le(wav, 16, 2);                     // bits
    wav += "data";
    put_le(wav, static_cast<std::uint32_t>(samples.size() * 2), 4);
    for (const std::int16_t s : samples) {
        put_le(wav, static_cast<std::uint16_t>(s), 2);
    }
    return wav;
}

// Writes bytes to a temp file and deletes it when the test ends.
struct TempFile {
    std::string path = (std::filesystem::temp_directory_path() /
                        ("sasr_" + std::to_string(std::random_device{}()) + ".wav"))
                           .string();
    explicit TempFile(const std::string& bytes) {
        std::ofstream(path, std::ios::binary) << bytes;
    }
    ~TempFile() {
        std::error_code ignored;  // destructors must not throw
        std::filesystem::remove(path, ignored);
    }
};

const Config kConfig;  // defaults: 16 kHz, 20 ms frames = 320 samples

// ---- tests -----------------------------------------------------------------

TEST(WavReader, ReadsRealFile) {
    WavReader reader(SASR_TEST_DATA_DIR "/en1.wav", kConfig);  // has a LIST chunk to skip
    EXPECT_EQ(reader.total_samples(), 255680);

    int frames = 0;
    std::int64_t expected_start = 0;
    while (const auto frame = reader.read_frame()) {
        EXPECT_EQ(frame->start_sample, expected_start);
        EXPECT_EQ(frame->samples.size(), 320U);
        for (const float s : frame->samples) {
            ASSERT_TRUE(s >= -1.0f && s < 1.0f);
        }
        expected_start += static_cast<std::int64_t>(frame->samples.size());
        ++frames;
    }
    EXPECT_EQ(frames, 799);
    EXPECT_FALSE(reader.read_frame().has_value());  // stays at end
}

TEST(WavReader, LastFrameIsShorter) {
    const TempFile file(make_wav(std::vector<std::int16_t>(1000)));
    WavReader reader(file.path, kConfig);

    std::vector<std::size_t> sizes;
    while (const auto frame = reader.read_frame()) {
        sizes.push_back(frame->samples.size());
    }
    EXPECT_EQ(sizes, (std::vector<std::size_t>{320, 320, 320, 40}));
}

TEST(WavReader, ConvertsInt16ToFloat) {
    const TempFile file(make_wav({0, 16384, -16384, -32768, 32767}));
    WavReader reader(file.path, kConfig);

    const auto frame = reader.read_frame();
    ASSERT_TRUE(frame.has_value());
    EXPECT_EQ(frame->samples, (std::vector<float>{0.0f, 0.5f, -0.5f, -1.0f, 32767.0f / 32768.0f}));
}

TEST(WavReader, EmptyFileHasNoFrames) {
    const TempFile file(make_wav({}));
    WavReader reader(file.path, kConfig);
    EXPECT_FALSE(reader.read_frame().has_value());
}

TEST(WavReader, ThrowsOnMissingFile) {
    EXPECT_THROW(WavReader("/nonexistent/file.wav", kConfig), WavError);
}

TEST(WavReader, ThrowsOnNonWavFile) {
    const TempFile file("this is not a wav file");
    EXPECT_THROW(WavReader(file.path, kConfig), WavError);
}

TEST(WavReader, ThrowsOnStereo) {
    const TempFile file(make_wav({0, 0}, /*channels=*/2));
    EXPECT_THROW(WavReader(file.path, kConfig), WavError);
}

TEST(WavReader, ThrowsOnWrongSampleRate) {
    const TempFile file(make_wav({0, 0}, 1, /*rate=*/44100));
    EXPECT_THROW(WavReader(file.path, kConfig), WavError);
}

TEST(WavReader, ThrowsOnTruncatedData) {
    std::string bytes = make_wav(std::vector<std::int16_t>(100));
    bytes.resize(bytes.size() - 10);  // header says 100 samples, file has 95
    const TempFile file(bytes);
    WavReader reader(file.path, kConfig);
    EXPECT_THROW(reader.read_frame(), WavError);
}

}  // namespace
}  // namespace sasr
