#include "audio/wav_reader.hpp"

#include <algorithm>
#include <bit>
#include <cstddef>

namespace sasr {

// We read int16 samples straight from the file; WAV is little-endian.
static_assert(std::endian::native == std::endian::little);

namespace {

// Reads a 4-character chunk id such as "RIFF", "fmt " or "data".
std::string read_id(std::istream& in) {
    std::string id(4, '\0');
    if (!in.read(id.data(), 4)) {
        throw WavError("truncated WAV header");
    }
    return id;
}

// Reads a little-endian unsigned integer of 2 or 4 bytes.
std::uint32_t read_le(std::istream& in, int bytes) {
    unsigned char b[4] = {};
    if (!in.read(reinterpret_cast<char*>(b), bytes)) {
        throw WavError("truncated WAV header");
    }
    return std::uint32_t{b[0]} | (std::uint32_t{b[1]} << 8) | (std::uint32_t{b[2]} << 16) |
           (std::uint32_t{b[3]} << 24);
}

}  // namespace

WavReader::WavReader(const std::string& path, const Config& config)
    : file_(path, std::ios::binary) {
    if (!file_) {
        throw WavError("cannot open WAV file: " + path);
    }
    if (config.frame_samples() == 0) {
        throw WavError("config.frame_ms is too small");
    }
    if (read_id(file_) != "RIFF") {
        throw WavError("not a WAV file: " + path);
    }
    read_le(file_, 4);  // RIFF size: unreliable in practice, not needed
    if (read_id(file_) != "WAVE") {
        throw WavError("not a WAV file: " + path);
    }

    // A WAV file is a list of chunks: [id (4 bytes)][size (4 bytes)][payload].
    // We need "fmt " (the format) and "data" (the samples); skip everything else.
    bool have_fmt = false;
    while (true) {
        const std::string id = read_id(file_);
        const std::uint32_t size = read_le(file_, 4);
        const std::streamsize padded_size = std::streamsize{size} + (size & 1);  // 2-byte aligned

        if (id == "fmt ") {
            const std::uint32_t format = read_le(file_, 2);    // 1 = PCM
            const std::uint32_t channels = read_le(file_, 2);
            const std::uint32_t rate = read_le(file_, 4);
            read_le(file_, 4);                                  // byte rate (derivable)
            read_le(file_, 2);                                  // block align (derivable)
            const std::uint32_t bits = read_le(file_, 2);

            if (size < 16 || format != 1 || channels != 1 || bits != 16 ||
                rate != config.sample_rate) {
                throw WavError("unsupported WAV format: format=" + std::to_string(format) +
                               " channels=" + std::to_string(channels) +
                               " rate=" + std::to_string(rate) + " bits=" + std::to_string(bits) +
                               " (need format=1, mono, " + std::to_string(config.sample_rate) +
                               " Hz, 16-bit)");
            }
            file_.ignore(padded_size - 16);  // optional fmt extension
            have_fmt = true;
        } else if (id == "data") {
            if (!have_fmt) {
                throw WavError("invalid WAV file: data chunk before fmt chunk");
            }
            total_samples_ = size / 2;  // 2 bytes per sample
            break;                      // file is now positioned at the first sample
        } else {
            file_.ignore(padded_size);  // LIST, fact, JUNK, ...
        }
    }

    scratch_.resize(config.frame_samples());
}

std::optional<AudioFrame> WavReader::read_frame() {
    const std::int64_t remaining = total_samples_ - position_;
    if (remaining <= 0) {
        return std::nullopt;
    }
    const auto n = static_cast<std::size_t>(std::min<std::int64_t>(remaining, std::ssize(scratch_)));
    const auto bytes = static_cast<std::streamsize>(n * sizeof(std::int16_t));

    if (!file_.read(reinterpret_cast<char*>(scratch_.data()), bytes)) {
        throw WavError("truncated WAV file: fewer samples than the header says");
    }

    AudioFrame frame;
    frame.start_sample = position_;
    frame.samples.resize(n);  // the only allocation per frame
    for (std::size_t i = 0; i < n; ++i) {
        frame.samples[i] = static_cast<float>(scratch_[i]) / 32768.0f;  // int16 -> [-1, 1)
    }
    position_ += static_cast<std::int64_t>(n);
    return frame;
}

}  // namespace sasr
