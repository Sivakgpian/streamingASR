#include "streaming/engine.hpp"

#include "asr/mock_backend.hpp"

#include <gtest/gtest.h>

#include <vector>

namespace sasr {
namespace {

Config fast_test_config() {
    Config config;
    config.preroll_ms = 0;
    config.min_speech_ms = 20;  // 1 frame: a single loud frame confirms onset
    return config;
}

constexpr std::size_t kFrameSamples = 320;
std::vector<float> loud_frame() { return std::vector<float>(kFrameSamples, 0.1f); }

struct Recorder {
    std::vector<SessionEvent> events;
    SessionEventSink sink() {
        return [this](const SessionEvent& e) { events.push_back(e); };
    }
};

TEST(Engine, PushAndEndOfStreamOnUnknownSessionThrows) {
    MockBackend backend;
    Engine engine(fast_test_config(), backend);

    EXPECT_THROW(engine.push(1, loud_frame()), EngineError);
    EXPECT_THROW(engine.end_of_stream(1), EngineError);
    EXPECT_THROW((void)engine.stats(1), EngineError);
    EXPECT_THROW(engine.close_session(1), EngineError);
}

TEST(Engine, OpeningTheSameSessionIdTwiceThrows) {
    MockBackend backend;
    Engine engine(fast_test_config(), backend);

    engine.open_session(1, {});
    EXPECT_THROW(engine.open_session(1, {}), EngineError);
}

TEST(Engine, HasSessionReflectsOpenAndClose) {
    MockBackend backend;
    Engine engine(fast_test_config(), backend);

    EXPECT_FALSE(engine.has_session(1));
    engine.open_session(1, {});
    EXPECT_TRUE(engine.has_session(1));
    engine.close_session(1);
    EXPECT_FALSE(engine.has_session(1));
}

TEST(Engine, TwoSessionsAreIndependent) {
    MockBackend backend;
    Engine engine(fast_test_config(), backend);

    Recorder rec1;
    Recorder rec2;
    engine.open_session(1, rec1.sink());
    engine.open_session(2, rec2.sink());

    engine.push(1, loud_frame());
    engine.end_of_stream(1);

    // Session 2 never received any audio: no events, and it's unaffected
    // by session 1's traffic (no shared state leaks between sessions).
    EXPECT_FALSE(rec1.events.empty());
    EXPECT_TRUE(rec2.events.empty());
    EXPECT_EQ(engine.stats(2).dropped_samples, 0U);
}

TEST(Engine, CloseSessionAbortsWithoutFinalizing) {
    MockBackend backend;
    Engine engine(fast_test_config(), backend);

    Recorder rec;
    engine.open_session(1, rec.sink());
    engine.push(1, loud_frame());  // mid-utterance, never finalized

    engine.close_session(1);

    EXPECT_TRUE(rec.events.empty());  // close_session aborts; no FINAL
    EXPECT_FALSE(engine.has_session(1));
}

}  // namespace
}  // namespace sasr
