#include "common/config.hpp"

namespace sasr {

namespace {
void require(bool condition, const char* message) {
    if (!condition) {
        throw ConfigError(message);
    }
}
}  // namespace

void Config::validate() const {
    require(sample_rate > 0, "Config::sample_rate must be > 0");
    require(frame_ms > 0, "Config::frame_ms must be > 0");
    require(frame_samples() > 0, "Config::frame_ms is too small for sample_rate (frame_samples() == 0)");

    require(vad_threshold >= 0.0f && vad_threshold < 1.0f,
            "Config::vad_threshold must be in [0, 1)");
    require(min_speech_ms > 0, "Config::min_speech_ms must be > 0");
    // preroll_ms == 0 is valid (no preroll kept).

    require(interval_ms > 0, "Config::interval_ms must be > 0");
    require(pause_threshold_ms > 0, "Config::pause_threshold_ms must be > 0");
    require(max_utterance_ms > 0, "Config::max_utterance_ms must be > 0");
    require(trailing_silence_ms <= pause_threshold_ms,
            "Config::trailing_silence_ms must be <= pause_threshold_ms "
            "(silence beyond pause_threshold_ms is never reached: the utterance finalizes first)");

    require(asr_threads > 0, "Config::asr_threads must be > 0");
    require(asr_workers > 0, "Config::asr_workers must be > 0");
    require(beam_size > 0, "Config::beam_size must be > 0");
    if (asr_backend == AsrBackendKind::kWhisperCpp) {
        require(!model_path.empty(), "Config::model_path is required when asr_backend == kWhisperCpp");
    }

    require(job_queue_capacity > 0, "Config::job_queue_capacity must be > 0");
    require(result_queue_capacity > 0, "Config::result_queue_capacity must be > 0");
}

}  // namespace sasr
