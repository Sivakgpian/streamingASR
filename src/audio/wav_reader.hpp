#pragma once

#include "common/config.hpp"

#include <cstdint>
#include <fstream>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace sasr {

// A block of mono float32 samples in [-1, 1).
// Memory: 32 bytes inline + samples.capacity() * 4 bytes on the heap.
struct AudioFrame {
    std::vector<float> samples;
    std::int64_t start_sample = 0;  // position of samples[0] in the stream
};

class WavError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// Reads a PCM, mono, 16-bit WAV file one frame at a time.
// The file is streamed, never loaded whole, so memory stays constant:
// sizeof(WavReader) + one frame of int16 scratch + the ifstream's 8 KiB buffer.
class WavReader {
public:
    // Opens the file and parses the header. Throws WavError if the file is
    // missing, malformed, or not PCM mono 16-bit at config.sample_rate.
    WavReader(const std::string& path, const Config& config);

    // Next frame (the last one may be shorter), or std::nullopt at end of file.
    std::optional<AudioFrame> read_frame();

    [[nodiscard]] std::int64_t total_samples() const { return total_samples_; }

private:
    std::ifstream file_;
    std::vector<std::int16_t> scratch_;  // raw samples for one frame, reused
    std::int64_t total_samples_ = 0;
    std::int64_t position_ = 0;          // samples read so far
};

}  // namespace sasr
