#include "asr/mock_backend.hpp"

#include <string>
#include <thread>

namespace sasr {

namespace {

class MockStream final : public AsrStream {
public:
    explicit MockStream(std::chrono::microseconds delay) : delay_(delay) {}

    AsrResult decode(std::span<const float> pcm) override { return make_result(pcm, /*is_final=*/false); }

    AsrResult finalize(std::span<const float> pcm) override {
        return make_result(pcm, /*is_final=*/true);
    }

    void reset() override {}  // stateless: nothing to clear

private:
    AsrResult make_result(std::span<const float> pcm, bool is_final) {
        if (delay_.count() > 0) {
            std::this_thread::sleep_for(delay_);  // simulate inference latency
        }
        AsrResult result;
        result.text = "<mock:" + std::to_string(pcm.size()) + ">";
        result.is_final = is_final;
        result.timings.infer_us = delay_.count();
        return result;
    }

    std::chrono::microseconds delay_;
};

}  // namespace

std::unique_ptr<AsrStream> MockBackend::create_stream() {
    return std::make_unique<MockStream>(decode_delay_);
}

}  // namespace sasr
