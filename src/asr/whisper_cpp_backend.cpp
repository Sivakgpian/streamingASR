#include "asr/whisper_cpp_backend.hpp"

#include <whisper.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <string>

namespace sasr {

namespace {

// whisper's encoder has 1500 positions for a 30 s window (25 ms mel
// window, 10 ms hop, 2x conv downsample -> 20 ms of audio per position).
// Sizing audio_ctx to the actual audio, rounded up to a multiple of 64
// (reported in community benchmarking to avoid repetition artifacts from
// odd context lengths -- see docs/ sources), gives most of the speedup
// short windows get from not paying for 30 s of silence padding. This is
// a heuristic, not a guarantee: revisit with real WER/latency numbers in
// a later optimization pass (CLAUDE.md: benchmark before optimizing).
int compute_audio_ctx(std::size_t n_samples, std::uint32_t sample_rate) {
    const double seconds = static_cast<double>(n_samples) / static_cast<double>(sample_rate);
    int ctx = static_cast<int>(std::ceil(seconds / 0.02));
    ctx = std::clamp(ctx, 64, 1500);
    ctx = ((ctx + 63) / 64) * 64;
    return std::min(ctx, 1500);
}

class WhisperStream final : public AsrStream {
public:
    WhisperStream(whisper_context* ctx, const Config& config) : ctx_(ctx), config_(config) {
        state_ = whisper_init_state(ctx_);
        if (state_ == nullptr) {
            throw std::runtime_error("WhisperCppBackend: whisper_init_state failed");
        }
    }

    ~WhisperStream() override { whisper_free_state(state_); }

    WhisperStream(const WhisperStream&) = delete;
    WhisperStream& operator=(const WhisperStream&) = delete;

    AsrResult decode(std::span<const float> pcm) override { return run(pcm, /*is_final=*/false); }
    AsrResult finalize(std::span<const float> pcm) override { return run(pcm, /*is_final=*/true); }

    // Deliberately does not clear prompt_: the previous utterance's
    // finalized text is kept as context for the next one (bounded; see
    // kMaxPromptChars). There is no other persistent decode state to
    // clear -- every call re-runs the full window from scratch.
    void reset() override {}

private:
    static constexpr std::size_t kMaxPromptChars = 200;  // a cheap bound; not exact token counting

    AsrResult run(std::span<const float> pcm, bool is_final) {
        whisper_full_params params = whisper_full_default_params(WHISPER_SAMPLING_GREEDY);
        params.language = config_.language.c_str();
        params.n_threads = static_cast<int>(config_.asr_threads);
        params.single_segment = true;
        params.no_timestamps = true;
        params.print_progress = false;
        params.print_realtime = false;
        params.print_special = false;
        params.print_timestamps = false;
        params.suppress_blank = true;
        params.no_context = true;  // we manage cross-utterance context ourselves, via initial_prompt
        params.vad = false;        // our own EnergyVad already runs upstream of the backend
        if (!is_final) {
            params.temperature_inc = 0.0f;  // no temperature fallback on partials: avoids p99 spikes
        }
        if (!prompt_.empty()) {
            params.initial_prompt = prompt_.c_str();
        }
        if (config_.dynamic_audio_ctx) {
            params.audio_ctx = compute_audio_ctx(pcm.size(), config_.sample_rate);
        }

        const auto start = std::chrono::steady_clock::now();
        const int rc = whisper_full_with_state(ctx_, state_, params, pcm.data(),
                                               static_cast<int>(pcm.size()));
        const auto infer_us = std::chrono::duration_cast<std::chrono::microseconds>(
                                   std::chrono::steady_clock::now() - start)
                                   .count();
        if (rc != 0) {
            throw std::runtime_error("WhisperCppBackend: whisper_full_with_state failed (rc=" +
                                     std::to_string(rc) + ")");
        }

        std::string text;
        const int n = whisper_full_n_segments_from_state(state_);
        for (int i = 0; i < n; ++i) {
            if (const char* segment = whisper_full_get_segment_text_from_state(state_, i)) {
                text += segment;
            }
        }

        if (is_final) {
            prompt_ = text;
            if (prompt_.size() > kMaxPromptChars) {
                prompt_ = prompt_.substr(prompt_.size() - kMaxPromptChars);
            }
        }

        AsrResult result;
        result.text = std::move(text);
        result.is_final = is_final;
        result.timings.infer_us = infer_us;
        return result;
    }

    whisper_context* ctx_;  // non-owning: shared, owned by WhisperCppBackend
    const Config& config_;  // non-owning: owned by WhisperCppBackend, outlives every stream it creates
    whisper_state* state_ = nullptr;
    std::string prompt_;  // previous utterance's finalized text, bounded; survives reset() on purpose
};

}  // namespace

void WhisperCppBackend::ContextDeleter::operator()(whisper_context* ctx) const noexcept {
    whisper_free(ctx);
}

WhisperCppBackend::WhisperCppBackend(const Config& config) : config_(config) {
    whisper_context_params cparams = whisper_context_default_params();
    cparams.use_gpu = false;  // CPU-first (CLAUDE.md); GPU is a later, explicit per-build choice
    whisper_context* raw = whisper_init_from_file_with_params(config_.model_path.c_str(), cparams);
    if (raw == nullptr) {
        throw std::runtime_error("WhisperCppBackend: failed to load model: " + config_.model_path);
    }
    ctx_.reset(raw);
}

WhisperCppBackend::~WhisperCppBackend() = default;

std::unique_ptr<AsrStream> WhisperCppBackend::create_stream() {
    return std::make_unique<WhisperStream>(ctx_.get(), config_);
}

}  // namespace sasr
