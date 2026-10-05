#include "common/config.hpp"

#include <gtest/gtest.h>

namespace sasr {
namespace {

TEST(Config, DefaultsAreValid) {
    const Config config;
    EXPECT_NO_THROW(config.validate());
}

TEST(Config, FrameSamplesMatchesSampleRateAndFrameMs) {
    Config config;
    config.sample_rate = 16000;
    config.frame_ms = 20;
    EXPECT_EQ(config.frame_samples(), 320U);
}

TEST(Config, PrerollAndMaxUtteranceSamplesScaleWithSampleRate) {
    Config config;
    config.sample_rate = 16000;
    config.preroll_ms = 300;
    config.max_utterance_ms = 15000;
    EXPECT_EQ(config.preroll_samples(), 4800U);
    EXPECT_EQ(config.max_utterance_samples(), 240000U);
}

TEST(Config, RejectsZeroSampleRate) {
    Config config;
    config.sample_rate = 0;
    EXPECT_THROW(config.validate(), ConfigError);
}

TEST(Config, RejectsFrameMsTooSmallForSampleRate) {
    Config config;
    config.sample_rate = 10;  // 10 Hz * 1 ms / 1000 == 0 samples
    config.frame_ms = 1;
    EXPECT_THROW(config.validate(), ConfigError);
}

TEST(Config, RejectsOutOfRangeVadThreshold) {
    Config config;
    config.vad_threshold = 1.0f;  // must be < 1
    EXPECT_THROW(config.validate(), ConfigError);

    config.vad_threshold = -0.1f;
    EXPECT_THROW(config.validate(), ConfigError);
}

TEST(Config, RejectsZeroMinSpeechMs) {
    Config config;
    config.min_speech_ms = 0;
    EXPECT_THROW(config.validate(), ConfigError);
}

TEST(Config, RejectsZeroIntervalPauseOrMaxUtterance) {
    Config base;

    Config a = base;
    a.interval_ms = 0;
    EXPECT_THROW(a.validate(), ConfigError);

    Config b = base;
    b.pause_threshold_ms = 0;
    EXPECT_THROW(b.validate(), ConfigError);

    Config c = base;
    c.max_utterance_ms = 0;
    EXPECT_THROW(c.validate(), ConfigError);
}

TEST(Config, RejectsTrailingSilenceLongerThanPauseThreshold) {
    Config config;
    config.pause_threshold_ms = 600;
    config.trailing_silence_ms = 601;
    EXPECT_THROW(config.validate(), ConfigError);

    config.trailing_silence_ms = 600;  // equal is fine
    EXPECT_NO_THROW(config.validate());
}

TEST(Config, RejectsZeroAsrThreadsWorkersOrBeamSize) {
    Config base;

    Config a = base;
    a.asr_threads = 0;
    EXPECT_THROW(a.validate(), ConfigError);

    Config b = base;
    b.asr_workers = 0;
    EXPECT_THROW(b.validate(), ConfigError);

    Config c = base;
    c.beam_size = 0;
    EXPECT_THROW(c.validate(), ConfigError);
}

TEST(Config, WhisperBackendRequiresModelPath) {
    Config config;
    config.asr_backend = Config::AsrBackendKind::kWhisperCpp;
    config.model_path.clear();
    EXPECT_THROW(config.validate(), ConfigError);

    config.model_path = "/path/to/model.bin";
    EXPECT_NO_THROW(config.validate());
}

TEST(Config, MockBackendDoesNotRequireModelPath) {
    Config config;
    config.asr_backend = Config::AsrBackendKind::kMock;
    config.model_path.clear();
    EXPECT_NO_THROW(config.validate());
}

TEST(Config, RejectsZeroQueueCapacities) {
    Config base;

    Config a = base;
    a.job_queue_capacity = 0;
    EXPECT_THROW(a.validate(), ConfigError);

    Config b = base;
    b.result_queue_capacity = 0;
    EXPECT_THROW(b.validate(), ConfigError);
}

}  // namespace
}  // namespace sasr
