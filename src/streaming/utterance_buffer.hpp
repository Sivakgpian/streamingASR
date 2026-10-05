#pragma once

#include <algorithm>
#include <cstddef>
#include <span>
#include <stdexcept>
#include <vector>

namespace sasr {

// A contiguous, preallocated float32 buffer that accumulates one
// utterance's audio. Capacity is fixed at construction (sized by the
// caller from Config::preroll_samples() + Config::max_utterance_samples());
// append() never allocates. Gives the ASR backend a single zero-copy span
// instead of per-frame chunks (see src/asr/asr_backend.hpp).
class UtteranceBuffer {
public:
    // capacity_samples must be > 0.
    explicit UtteranceBuffer(std::size_t capacity_samples) : storage_(capacity_samples) {
        if (capacity_samples == 0) {
            throw std::invalid_argument("UtteranceBuffer: capacity_samples must be > 0");
        }
    }

    // Appends as many leading samples of `in` as fit; returns the number
    // actually written, which is less than in.size() only once the buffer
    // is full. Never allocates. The caller (the streaming engine, added
    // later) is responsible for treating a short write as an overflow and
    // counting it; this class only enforces the bound.
    std::size_t append(std::span<const float> in) {
        const std::size_t space = storage_.size() - size_;
        const std::size_t n = std::min(space, in.size());
        std::copy_n(in.begin(), n, storage_.begin() + static_cast<std::ptrdiff_t>(size_));
        size_ += n;
        return n;
    }

    // The audio accumulated so far: a zero-copy view into this buffer,
    // valid until the next append() or clear().
    [[nodiscard]] std::span<const float> view() const { return {storage_.data(), size_}; }

    [[nodiscard]] std::size_t size() const noexcept { return size_; }
    [[nodiscard]] std::size_t capacity() const noexcept { return storage_.size(); }
    [[nodiscard]] bool full() const noexcept { return size_ == storage_.size(); }

    // Resets to empty without releasing storage_, so the next utterance
    // reuses the same allocation. Never allocates.
    void clear() noexcept { size_ = 0; }

private:
    std::vector<float> storage_;
    std::size_t size_ = 0;
};

}  // namespace sasr
