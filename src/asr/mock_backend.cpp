#include "asr/mock_backend.hpp"

#include <cstddef>
#include <string>
#include <thread>

namespace sasr {

namespace {

class MockStream final : public AsrStream {
public:
    explicit MockStream(std::chrono::microseconds delay) : delay_(delay) {}

    void accept(std::span<const float> pcm) override { total_samples_ += pcm.size(); }

    AsrResult decode() override { return make_result(/*is_final=*/false); }

    AsrResult finalize() override { return make_result(/*is_final=*/true); }

    void reset() override { total_samples_ = 0; }

private:
    AsrResult make_result(bool is_final) {
        if (delay_.count() > 0) {
            std::this_thread::sleep_for(delay_);  // simulate inference latency
        }
        AsrResult result;
        result.text = "<mock:" + std::to_string(total_samples_) + ">";
        result.is_final = is_final;
        result.timings.infer_us = delay_.count();
        return result;
    }

    std::size_t total_samples_ = 0;
    std::chrono::microseconds delay_;
};

}  // namespace

std::unique_ptr<AsrStream> MockBackend::create_stream() {
    return std::make_unique<MockStream>(decode_delay_);
}

}  // namespace sasr
