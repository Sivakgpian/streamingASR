#include "streaming/threaded_engine.hpp"

#include "asr/mock_backend.hpp"
#include "streaming/engine.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <span>
#include <string>
#include <thread>
#include <vector>

namespace sasr {
namespace {

using namespace std::chrono_literals;

// Same small timings as the Session tests (320 samples/frame):
// onset after 1 frame, partial every 2 frames, finalize after 2 silent
// frames, forced finalize at 10 frames.
Config fast_test_config() {
    Config config;
    config.preroll_ms = 0;
    config.min_speech_ms = 20;
    config.interval_ms = 40;
    config.pause_threshold_ms = 40;
    config.trailing_silence_ms = 20;
    config.max_utterance_ms = 200;
    config.asr_workers = 2;
    config.job_queue_capacity = 8;
    return config;
}

constexpr std::size_t kFrameSamples = 320;
const std::vector<float> kLoud(kFrameSamples, 0.1f);
const std::vector<float> kSilent(kFrameSamples, 0.0f);

// One utterance: 3 speech frames then a 2-frame pause -> one FINAL over
// exactly 1600 samples, whatever the thread timing (partials vary).
std::vector<float> utterances(int count) {
    std::vector<float> audio;
    for (int u = 0; u < count; ++u) {
        for (int i = 0; i < 3; ++i) {
            audio.insert(audio.end(), kLoud.begin(), kLoud.end());
        }
        for (int i = 0; i < 2; ++i) {
            audio.insert(audio.end(), kSilent.begin(), kSilent.end());
        }
    }
    return audio;
}

std::size_t mock_samples(const std::string& text) {  // "<mock:N>" -> N
    return std::stoul(text.substr(6, text.size() - 7));
}

// Events are delivered on the engine thread; tests only read them after
// stop(), whose join() orders those writes before the reads.
struct Recorder {
    std::vector<SessionEvent> events;
    SessionEventSink sink() {
        return [this](const SessionEvent& e) { events.push_back(e); };
    }
    [[nodiscard]] std::vector<std::string> finals() const {
        std::vector<std::string> out;
        for (const auto& e : events) {
            if (e.kind == SessionEventKind::kFinal) {
                out.push_back(e.text);
            }
        }
        return out;
    }
    [[nodiscard]] std::size_t errors() const {
        std::size_t n = 0;
        for (const auto& e : events) {
            n += e.kind == SessionEventKind::kError ? 1 : 0;
        }
        return n;
    }
};

// Within one utterance, partials only grow and the FINAL is at least as
// long as every partial before it: proves events from different
// utterances never interleave.
void expect_well_ordered(const std::vector<SessionEvent>& events) {
    std::size_t longest_partial = 0;
    for (const auto& e : events) {
        if (e.kind == SessionEventKind::kPartial) {
            const std::size_t n = mock_samples(e.text);
            EXPECT_GE(n, longest_partial) << "partial shrank within an utterance";
            longest_partial = n;
        } else if (e.kind == SessionEventKind::kFinal) {
            EXPECT_GE(mock_samples(e.text), longest_partial) << "FINAL shorter than its partials";
            longest_partial = 0;
        }
    }
}

TEST(ThreadedEngine, ProducesTheSameFinalsAsTheSynchronousEngine) {
    const std::vector<float> audio = utterances(3);
    MockBackend backend(1ms);

    Recorder sync_rec;
    {
        Engine engine(fast_test_config(), backend);
        engine.open_session(1, sync_rec.sink());
        engine.push(1, audio);  // one call: Engine re-frames it
        engine.end_of_stream(1);
    }

    Recorder threaded_rec;
    ThreadedEngine engine(fast_test_config(), backend);
    engine.open_session(1, threaded_rec.sink());
    engine.push_blocking(1, audio);
    engine.end_of_stream(1);
    engine.stop();

    EXPECT_EQ(threaded_rec.finals(), sync_rec.finals());
    EXPECT_EQ(threaded_rec.finals(),
              (std::vector<std::string>{"<mock:1600>", "<mock:1600>", "<mock:1600>"}));
    EXPECT_EQ(threaded_rec.errors(), 0U);
    expect_well_ordered(threaded_rec.events);
}

TEST(ThreadedEngine, EndOfStreamFlushesTheInProgressUtterance) {
    MockBackend backend;
    Recorder rec;
    ThreadedEngine engine(fast_test_config(), backend);
    engine.open_session(1, rec.sink());
    for (int i = 0; i < 3; ++i) {
        engine.push_blocking(1, kLoud);
    }
    engine.end_of_stream(1);
    engine.stop();
    EXPECT_EQ(rec.finals(), (std::vector<std::string>{"<mock:960>"}));
}

TEST(ThreadedEngine, StopFlushesSessionsThatNeverCalledEndOfStream) {
    MockBackend backend;
    Recorder rec;
    ThreadedEngine engine(fast_test_config(), backend);
    engine.open_session(1, rec.sink());
    for (int i = 0; i < 4; ++i) {
        engine.push_blocking(1, kLoud);
    }
    engine.stop();
    EXPECT_EQ(rec.finals(), (std::vector<std::string>{"<mock:1280>"}));
}

TEST(ThreadedEngine, TinyRingStillDeliversEverythingThroughBackpressure) {
    Config config = fast_test_config();
    config.ring_capacity_frames = 2;  // the producer constantly hits a full ring
    MockBackend backend(3ms);         // and the backend is slow enough to stall sessions
    Recorder rec;
    ThreadedEngine engine(config, backend);
    engine.open_session(1, rec.sink());
    engine.push_blocking(1, utterances(10));
    engine.end_of_stream(1);
    engine.stop();

    EXPECT_EQ(rec.finals(), std::vector<std::string>(10, "<mock:1600>"));
    EXPECT_EQ(engine.stats(1).dropped_samples, 0U);  // backpressure, not loss
    expect_well_ordered(rec.events);
}

// The concurrency stress test: many sessions, each fed by its own
// producer thread, sharing a small worker pool, with tiny rings forcing
// constant backpressure. Under the TSan presets this is the main check
// on the ring, the queues, and the engine thread's ownership of sessions.
TEST(ThreadedEngine, ManySessionsFromManyProducerThreads) {
    constexpr int kSessions = 8;
    constexpr int kUtterances = 20;
    Config config = fast_test_config();
    config.asr_workers = 3;
    config.job_queue_capacity = kSessions;
    config.ring_capacity_frames = 8;
    MockBackend backend(std::chrono::microseconds{200});

    std::vector<Recorder> recorders(kSessions);
    ThreadedEngine engine(config, backend);
    for (int s = 0; s < kSessions; ++s) {
        engine.open_session(static_cast<SessionId>(s), recorders[static_cast<std::size_t>(s)].sink());
    }

    const std::vector<float> audio = utterances(kUtterances);
    std::vector<std::thread> producers;
    for (int s = 0; s < kSessions; ++s) {
        producers.emplace_back([&engine, &audio, s] {
            const auto id = static_cast<SessionId>(s);
            // Odd-sized chunks, so pushes don't line up with frames.
            for (std::size_t off = 0; off < audio.size(); off += 500) {
                const std::size_t n = std::min<std::size_t>(500, audio.size() - off);
                engine.push_blocking(id, std::span<const float>(audio).subspan(off, n));
            }
            engine.end_of_stream(id);
        });
    }
    for (std::thread& t : producers) {
        t.join();
    }
    engine.stop();

    for (int s = 0; s < kSessions; ++s) {
        const Recorder& rec = recorders[static_cast<std::size_t>(s)];
        EXPECT_EQ(rec.finals(), std::vector<std::string>(kUtterances, "<mock:1600>"))
            << "session " << s;
        EXPECT_EQ(rec.errors(), 0U);
        expect_well_ordered(rec.events);
    }
}

TEST(ThreadedEngine, ClosingASessionWithAJobInFlightIsSafeAndEmitsNoFinal) {
    MockBackend backend(20ms);
    Recorder rec;
    ThreadedEngine engine(fast_test_config(), backend);
    engine.open_session(1, rec.sink());
    for (int i = 0; i < 3; ++i) {
        engine.push_blocking(1, kLoud);  // the 3rd frame dispatches a slow partial
    }
    std::this_thread::sleep_for(5ms);  // let the engine thread dispatch it
    engine.close_session(1);
    EXPECT_THROW((void)engine.push(1, kLoud), EngineError);
    engine.stop();  // must wait for the in-flight job before destroying the session

    EXPECT_TRUE(rec.finals().empty());
    EXPECT_THROW((void)engine.stats(1), EngineError);  // closed sessions are gone
}

TEST(ThreadedEngine, EnforcesTheSessionLimitAndRejectsMisuse) {
    Config config = fast_test_config();
    config.job_queue_capacity = 2;
    MockBackend backend;
    ThreadedEngine engine(config, backend);

    engine.open_session(1, {});
    engine.open_session(2, {});
    EXPECT_THROW(engine.open_session(3, {}), EngineError);  // limit
    EXPECT_THROW(engine.open_session(1, {}), EngineError);  // duplicate
    EXPECT_THROW((void)engine.push(9, kLoud), EngineError);
    EXPECT_THROW(engine.end_of_stream(9), EngineError);
    EXPECT_THROW(engine.close_session(9), EngineError);
    EXPECT_THROW((void)engine.stats(1), EngineError);  // only after stop()

    engine.stop();
    EXPECT_THROW((void)engine.push(1, kLoud), EngineError);
    EXPECT_EQ(engine.stats(1).dropped_samples, 0U);
    engine.stop();  // idempotent
}

}  // namespace
}  // namespace sasr
