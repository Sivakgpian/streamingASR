#pragma once

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>

namespace sasr {

class ConfigError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// Every tunable setting of the engine, in one place (like a Python config.py).
// Change a default here, or create a Config and override fields:
//     sasr::Config cfg;
//     cfg.frame_ms = 10;
// Call validate() after overriding fields and before using the config;
// it throws ConfigError with a message naming the bad field.
struct Config {
    // ---- Audio (internal format: mono float32) ----
    std::uint32_t sample_rate = 16000;  // Hz; input files must match
    std::uint32_t frame_ms = 20;        // length of each AudioFrame

    // ---- VAD ----
    enum class VadKind { kEnergy };
    VadKind vad_kind = VadKind::kEnergy;
    float vad_threshold = 0.02f;        // RMS amplitude in [0, 1) above which a frame is "speech"
    std::uint32_t min_speech_ms = 100;  // speech must persist this long before onset is declared
    std::uint32_t preroll_ms = 300;     // audio kept before onset so it isn't clipped

    // ---- Streaming / endpointing (see CLAUDE.md: cumulative-interval) ----
    std::uint32_t interval_ms = 1000;          // cadence of INTERIM results
    std::uint32_t pause_threshold_ms = 600;    // silence this long finalizes the utterance
    std::uint32_t max_utterance_ms = 15000;    // forced finalize even without a pause
    std::uint32_t trailing_silence_ms = 300;   // silence kept inside an utterance before cutoff
    bool partials_enabled = true;              // disable to emit FINAL only (weak hardware)

    // ---- ASR backend ----
    enum class AsrBackendKind { kMock, kWhisperCpp };
    AsrBackendKind asr_backend = AsrBackendKind::kMock;
    std::string model_path;              // required when asr_backend == kWhisperCpp
    std::string language = "en";
    std::uint32_t asr_threads = 4;        // intra-op threads per inference call
    std::uint32_t asr_workers = 1;        // concurrent inference jobs
    bool dynamic_audio_ctx = true;        // size the encoder window to the audio, not 30s
    std::uint32_t beam_size = 1;          // 1 = greedy

    // ---- Queues (bounded; see CLAUDE.md: no unbounded queues) ----
    std::uint32_t job_queue_capacity = 8;
    std::uint32_t result_queue_capacity = 64;

    // Samples per frame: 16000 Hz * 20 ms / 1000 = 320.
    [[nodiscard]] std::size_t frame_samples() const { return ms_to_samples(frame_ms); }
    [[nodiscard]] std::size_t preroll_samples() const { return ms_to_samples(preroll_ms); }
    [[nodiscard]] std::size_t max_utterance_samples() const { return ms_to_samples(max_utterance_ms); }

    // Throws ConfigError naming the first invalid field found.
    void validate() const;

private:
    [[nodiscard]] std::size_t ms_to_samples(std::uint32_t ms) const {
        return std::size_t{sample_rate} * ms / 1000;
    }
};

}  // namespace sasr
