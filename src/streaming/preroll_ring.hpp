#pragma once

#include "streaming/utterance_buffer.hpp"

#include <algorithm>
#include <cstddef>
#include <span>
#include <vector>

namespace sasr {

// A fixed-capacity circular buffer of raw audio, continuously fed while a
// session has no confirmed speech yet. Once onset is confirmed,
// drain_into() hands its entire contents to the utterance buffer in
// chronological order, so the utterance never loses the "preroll" audio
// immediately before speech began. Implementation detail of
// sasr::Session; not meant to be used directly outside it.
//
// Sizing note (see Session): to guarantee drain_into() never overflows
// the destination, the ring's capacity must not exceed
// Config::preroll_samples() + Config::min_speech_samples() (the most the
// VAD's onset debounce can have accumulated before confirming), and the
// destination UtteranceBuffer must have at least that much headroom.
//
// Never allocates after construction.
class PrerollRing {
public:
    // capacity_samples may be 0 (every push() and drain_into() is then a
    // no-op), which is how Session disables preroll when
    // Config::preroll_ms == 0.
    explicit PrerollRing(std::size_t capacity_samples) : storage_(capacity_samples) {}

    // Overwrites the oldest samples once full. A frame larger than the
    // whole ring only keeps its tail. Never allocates.
    void push(std::span<const float> frame) {
        if (storage_.empty()) {
            return;
        }
        std::size_t offset = 0;
        std::size_t remaining = frame.size();
        if (remaining > storage_.size()) {
            offset = remaining - storage_.size();
            remaining = storage_.size();
        }
        while (remaining > 0) {
            const std::size_t space_to_wrap = storage_.size() - write_pos_;
            const std::size_t n = std::min(remaining, space_to_wrap);
            std::copy_n(frame.begin() + static_cast<std::ptrdiff_t>(offset), n,
                       storage_.begin() + static_cast<std::ptrdiff_t>(write_pos_));
            write_pos_ = (write_pos_ + n) % storage_.size();
            offset += n;
            remaining -= n;
        }
        filled_ = std::min(filled_ + frame.size(), storage_.size());
    }

    // Appends this ring's contents, oldest-first, into dst. At most two
    // contiguous ranges (the ring wraps at most once). Never allocates.
    // See the sizing note above for why this is guaranteed to fit.
    void drain_into(UtteranceBuffer& dst) const {
        if (filled_ == 0) {
            return;
        }
        const std::size_t start = (write_pos_ + storage_.size() - filled_) % storage_.size();
        const std::size_t first_len = std::min(filled_, storage_.size() - start);
        dst.append(std::span<const float>(storage_).subspan(start, first_len));
        const std::size_t second_len = filled_ - first_len;
        if (second_len > 0) {
            dst.append(std::span<const float>(storage_).subspan(0, second_len));
        }
    }

    [[nodiscard]] std::size_t size() const noexcept { return filled_; }
    [[nodiscard]] std::size_t capacity() const noexcept { return storage_.size(); }

    void clear() noexcept {
        write_pos_ = 0;
        filled_ = 0;
    }

private:
    std::vector<float> storage_;
    std::size_t write_pos_ = 0;
    std::size_t filled_ = 0;
};

}  // namespace sasr
