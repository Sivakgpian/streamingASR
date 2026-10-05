#pragma once

#include <algorithm>
#include <cstddef>
#include <span>
#include <stdexcept>
#include <vector>

namespace sasr {

// Re-cuts arbitrarily sized chunks of audio into fixed-size frames
// aligned to the start of the stream. The VAD and endpointer decide
// frame by frame, so without this their decisions -- and therefore the
// transcript -- would depend on how a caller (a mic driver, a network
// transport) happened to chunk its audio. Both engines feed sessions
// through one of these.
//
// Never allocates after construction. Whole frames that arrive aligned
// are passed through without copying; only the pieces of a frame split
// across calls are copied into the internal frame.
class FrameAssembler {
public:
    explicit FrameAssembler(std::size_t frame_samples) : frame_(frame_samples) {
        if (frame_samples == 0) {
            throw std::invalid_argument("FrameAssembler: frame_samples must be > 0");
        }
    }

    // Calls on_frame(std::span<const float>) once per complete frame, in
    // order; the span is valid only during that call. Leftover samples
    // wait for the next feed() or flush().
    template <typename OnFrame>
    void feed(std::span<const float> pcm, OnFrame&& on_frame) {
        const std::size_t size = frame_.size();
        while (!pcm.empty()) {
            if (filled_ == 0 && pcm.size() >= size) {
                on_frame(pcm.first(size));  // aligned: straight from the input
                pcm = pcm.subspan(size);
                continue;
            }
            const std::size_t n = std::min(size - filled_, pcm.size());
            std::copy_n(pcm.begin(), n, frame_.begin() + static_cast<std::ptrdiff_t>(filled_));
            filled_ += n;
            pcm = pcm.subspan(n);
            if (filled_ == size) {
                filled_ = 0;
                on_frame(std::span<const float>(frame_));
            }
        }
    }

    // End of stream: emits the leftover partial frame, if any.
    template <typename OnFrame>
    void flush(OnFrame&& on_frame) {
        if (filled_ > 0) {
            const std::size_t n = filled_;
            filled_ = 0;
            on_frame(std::span<const float>(frame_).first(n));
        }
    }

    // Drops the leftover partial frame (session aborted).
    void clear() noexcept { filled_ = 0; }

    [[nodiscard]] std::size_t pending() const noexcept { return filled_; }

private:
    std::vector<float> frame_;
    std::size_t filled_ = 0;
};

}  // namespace sasr
