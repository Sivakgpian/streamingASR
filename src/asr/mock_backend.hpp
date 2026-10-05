#pragma once

#include "asr/asr_backend.hpp"

#include <chrono>
#include <memory>

namespace sasr {

// Deterministic backend for tests and benchmarks: no real inference.
// decode()/finalize() return text derived only from the number of samples
// accepted so far ("<mock:N>"), so two streams fed the same audio always
// produce the same results. An optional busy-delay lets tests exercise
// queueing/latency paths without a real model.
class MockBackend final : public AsrBackend {
public:
    explicit MockBackend(std::chrono::microseconds decode_delay = std::chrono::microseconds{0})
        : decode_delay_(decode_delay) {}

    [[nodiscard]] std::unique_ptr<AsrStream> create_stream() override;
    [[nodiscard]] bool is_incremental() const noexcept override { return false; }

private:
    std::chrono::microseconds decode_delay_;
};

}  // namespace sasr
