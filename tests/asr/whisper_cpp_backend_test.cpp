// Integration tests against the real whisper.cpp backend. Only built
// when SASR_WITH_WHISPER=ON (see tests/asr/CMakeLists.txt). Tests that
// need the model file GTEST_SKIP() if it isn't present at
// SASR_TEST_WHISPER_MODEL (scripts/download_model.sh's default output
// path) -- a missing model is an environment fact, not a test failure:
// it's a ~75 MiB download deliberately not committed to the repo.
#include "asr/whisper_cpp_backend.hpp"

#include "audio/wav_reader.hpp"
#include "common/config.hpp"
#include "streaming/engine.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace sasr {
namespace {

std::string model_path() {
    const std::string path = SASR_TEST_WHISPER_MODEL;
    return std::filesystem::exists(path) ? path : std::string{};
}

#define SASR_SKIP_IF_NO_MODEL()                                                       \
    do {                                                                              \
        if (model_path().empty()) {                                                  \
            GTEST_SKIP() << "model not found at " SASR_TEST_WHISPER_MODEL             \
                            " (run scripts/download_model.sh)";                       \
        }                                                                             \
    } while (false)

TEST(WhisperCppBackend, ThrowsOnMissingModelFile) {
    Config config;
    config.model_path = "/nonexistent/model.bin";
    EXPECT_THROW(WhisperCppBackend backend(config), std::runtime_error);
}

TEST(WhisperCppBackend, FinalizeOnShortAudioProducesAFinalResult) {
    SASR_SKIP_IF_NO_MODEL();
    Config config;
    config.model_path = model_path();
    config.asr_threads = 2;
    WhisperCppBackend backend(config);
    auto stream = backend.create_stream();

    // Not real speech, so no assertion on the text itself: this proves
    // the call completes without crashing/throwing and reports timings.
    const std::vector<float> tone(16000, 0.1f);  // 1 s @ 16 kHz
    const AsrResult result = stream->finalize(tone);
    EXPECT_TRUE(result.is_final);
    EXPECT_GT(result.timings.infer_us, 0);
}

TEST(WhisperCppBackend, SilenceDoesNotCrash) {
    SASR_SKIP_IF_NO_MODEL();
    Config config;
    config.model_path = model_path();
    WhisperCppBackend backend(config);
    auto stream = backend.create_stream();

    // Whisper may still emit a special token on pure silence depending on
    // the model; we only assert it completes and reports FINAL. Exact
    // text-content assertions need a labeled dataset (Phase 8+).
    const std::vector<float> silence(16000, 0.0f);
    const AsrResult result = stream->finalize(silence);
    EXPECT_TRUE(result.is_final);
}

TEST(WhisperCppBackend, SecondUtteranceAfterResetDoesNotCrash) {
    SASR_SKIP_IF_NO_MODEL();
    Config config;
    config.model_path = model_path();
    WhisperCppBackend backend(config);
    auto stream = backend.create_stream();

    const std::vector<float> tone(16000, 0.1f);
    stream->finalize(tone);
    stream->reset();
    const AsrResult second = stream->finalize(tone);
    EXPECT_TRUE(second.is_final);
}

TEST(WhisperCppBackend, TranscribesEn1ThroughTheFullEngine) {
    SASR_SKIP_IF_NO_MODEL();
    Config config;
    config.model_path = model_path();
    config.asr_backend = Config::AsrBackendKind::kWhisperCpp;
    WhisperCppBackend backend(config);
    Engine engine(config, backend);

    std::vector<SessionEvent> events;
    constexpr SessionId kSession = 1;
    engine.open_session(kSession, [&events](const SessionEvent& e) { events.push_back(e); });

    WavReader reader(SASR_TEST_DATA_DIR "/en1.wav", config);
    std::vector<float> chunk(config.frame_samples());
    while (true) {
        const std::size_t n = reader.read_into(chunk);
        if (n == 0) {
            break;
        }
        engine.push(kSession, std::span<const float>(chunk).first(n));
    }
    engine.end_of_stream(kSession);

    const bool got_final = std::ranges::any_of(
        events, [](const SessionEvent& e) { return e.kind == SessionEventKind::kFinal; });
    const bool got_error = std::ranges::any_of(
        events, [](const SessionEvent& e) { return e.kind == SessionEventKind::kError; });
    EXPECT_TRUE(got_final) << "expected at least one FINAL transcribing a real speech file";
    EXPECT_FALSE(got_error);

    for (const SessionEvent& e : events) {
        if (e.kind == SessionEventKind::kFinal) {
            EXPECT_FALSE(e.text.empty()) << "en1.wav is real speech; FINAL text should not be empty";
        }
    }
}

}  // namespace
}  // namespace sasr
