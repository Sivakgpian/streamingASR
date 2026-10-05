#include "streaming/session.hpp"

#include "asr/mock_backend.hpp"
#include "streaming/job_runner.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <deque>
#include <memory>
#include <span>
#include <stdexcept>
#include <utility>
#include <vector>

namespace sasr {
namespace {

// ---- fixtures ---------------------------------------------------------------

// Small ms values (but still sample_rate=16000, frame_ms=20 -> 320
// samples/frame) so tests need only a handful of pushes:
//   - min_speech_ms == frame_ms: a single loud frame confirms onset, so
//     these tests exercise the *session* state machine without re-testing
//     EnergyVad's own onset debounce (covered by energy_vad_test.cpp).
//   - preroll_ms = 0: buffer sizes in assertions are exact multiples of
//     320 with no preroll offset to account for.
Config fast_test_config() {
    Config config;
    config.preroll_ms = 0;
    config.min_speech_ms = 20;       // 1 frame
    config.interval_ms = 40;         // 2 frames
    config.pause_threshold_ms = 40;  // 2 frames
    config.trailing_silence_ms = 20;
    config.max_utterance_ms = 200;  // 10 frames
    return config;
}

constexpr std::size_t kFrameSamples = 320;
std::vector<float> loud_frame() { return std::vector<float>(kFrameSamples, 0.1f); }   // RMS 0.1
std::vector<float> silent_frame() { return std::vector<float>(kFrameSamples, 0.0f); }

struct Recorder {
    std::vector<SessionEvent> events;
    SessionEventSink sink() {
        return [this](const SessionEvent& e) { events.push_back(e); };
    }
};

class ThrowingStream final : public AsrStream {
public:
    AsrResult decode(std::span<const float>) override { throw std::runtime_error("decode boom"); }
    AsrResult finalize(std::span<const float>) override { throw std::runtime_error("finalize boom"); }
    void reset() override {}
};

class ThrowingBackend final : public AsrBackend {
public:
    [[nodiscard]] std::unique_ptr<AsrStream> create_stream() override {
        return std::make_unique<ThrowingStream>();
    }
    [[nodiscard]] bool is_incremental() const noexcept override { return false; }
};

// Stateless, so one instance can serve every test: jobs run inside
// run(), exactly like Phase 4's synchronous Session.
InlineJobRunner inline_runner;

// Test double: queues jobs instead of running them, so a test decides
// exactly when each one completes. Covers the asynchronous paths
// (in-flight jobs, deferred FINALs, stalls, reset mid-job)
// deterministically, without real threads.
class ManualJobRunner final : public JobRunner {
public:
    void run(AsrStream& stream, std::span<const float> pcm, bool is_final,
             JobCallback on_done) override {
        jobs_.push_back(Job{&stream, pcm, is_final, std::move(on_done)});
    }

    [[nodiscard]] std::size_t pending() const { return jobs_.size(); }
    [[nodiscard]] bool front_is_final() const { return jobs_.front().is_final; }
    [[nodiscard]] std::size_t front_samples() const { return jobs_.front().pcm.size(); }

    void complete_next() {
        Job job = std::move(jobs_.front());
        jobs_.pop_front();
        job.on_done(execute_job(*job.stream, job.pcm, job.is_final));
    }

private:
    struct Job {
        AsrStream* stream;
        std::span<const float> pcm;
        bool is_final;
        JobCallback on_done;
    };
    std::deque<Job> jobs_;
};

// ---- tests -------------------------------------------------------------------

TEST(Session, SilenceOnlyInputNeverCallsBackend) {
    Recorder rec;
    MockBackend backend;
    Session session(fast_test_config(), backend, inline_runner, rec.sink());

    for (int i = 0; i < 20; ++i) {
        session.push(silent_frame());
    }

    EXPECT_TRUE(rec.events.empty());  // a backend call would have produced a PARTIAL/FINAL/ERROR
    EXPECT_EQ(session.state(), SessionState::kIdle);
}

TEST(Session, OnsetAloneEmitsNothingBeforeIntervalElapses) {
    Recorder rec;
    MockBackend backend;
    Session session(fast_test_config(), backend, inline_runner, rec.sink());

    session.push(loud_frame());  // confirms onset (min_speech_ms == 1 frame)
    EXPECT_EQ(session.state(), SessionState::kSpeech);
    EXPECT_TRUE(rec.events.empty());  // interval_ms == 2 frames; only 1 frame of speech so far
}

TEST(Session, ContinuousSpeechEmitsPartialEveryIntervalMs) {
    Recorder rec;
    MockBackend backend;
    Session session(fast_test_config(), backend, inline_runner, rec.sink());  // interval == 2 frames

    session.push(loud_frame());  // #1: onset. buffer=320 samples. interval counter starts at 0.
    session.push(loud_frame());  // #2: buffer=640. counter=320 (< 640): no partial yet.
    EXPECT_TRUE(rec.events.empty());

    session.push(loud_frame());  // #3: buffer=960. counter=640 (>= 640): PARTIAL.
    ASSERT_EQ(rec.events.size(), 1U);
    EXPECT_EQ(rec.events[0].kind, SessionEventKind::kPartial);
    EXPECT_EQ(rec.events[0].text, "<mock:960>");

    session.push(loud_frame());  // #4: buffer=1280. counter=320: no partial.
    EXPECT_EQ(rec.events.size(), 1U);

    session.push(loud_frame());  // #5: buffer=1600. counter=640: PARTIAL.
    ASSERT_EQ(rec.events.size(), 2U);
    EXPECT_EQ(rec.events[1].text, "<mock:1600>");
}

TEST(Session, ShortPauseBelowThresholdReturnsToSpeechWithoutFinalizing) {
    Recorder rec;
    MockBackend backend;
    Session session(fast_test_config(), backend, inline_runner, rec.sink());  // pause_threshold == 2 frames

    session.push(loud_frame());    // onset
    session.push(silent_frame());  // 1 silent frame: below the 2-frame pause threshold
    EXPECT_EQ(session.state(), SessionState::kShortPause);
    EXPECT_TRUE(rec.events.empty());  // no FINAL yet

    session.push(loud_frame());  // speech resumes before the pause completed
    EXPECT_EQ(session.state(), SessionState::kSpeech);
    EXPECT_TRUE(std::ranges::none_of(
        rec.events, [](const SessionEvent& e) { return e.kind == SessionEventKind::kFinal; }));
}

TEST(Session, LongPauseAtOrAbovePauseThresholdEmitsExactlyOneFinalAndResets) {
    Recorder rec;
    MockBackend backend;
    Session session(fast_test_config(), backend, inline_runner, rec.sink());  // pause_threshold == 2 frames

    session.push(loud_frame());    // onset. buffer=320.
    session.push(silent_frame());  // buffer=640. silence=320 (< 640): SHORT_PAUSE.
    session.push(silent_frame());  // buffer=960. silence=640 (>= 640): finalize.

    ASSERT_EQ(rec.events.size(), 1U);
    EXPECT_EQ(rec.events[0].kind, SessionEventKind::kFinal);
    EXPECT_EQ(rec.events[0].text, "<mock:960>");  // the whole utterance, including both silence frames
    EXPECT_EQ(session.state(), SessionState::kIdle);
    EXPECT_EQ(session.stats().forced_finalizations, 0U);  // this was a pause, not a forced cutoff
}

TEST(Session, MaxUtteranceForcesFinalizeWithoutAnyPause) {
    Recorder rec;
    MockBackend backend;
    Config config = fast_test_config();
    config.partials_enabled = false;  // isolate max_utterance from interval partials, which
                                       // would otherwise also fire during 10 continuous frames
    Session session(config, backend, inline_runner, rec.sink());  // max_utterance == 10 frames

    for (int i = 0; i < 10; ++i) {
        session.push(loud_frame());  // continuous speech, never silent
    }

    ASSERT_EQ(rec.events.size(), 1U);
    EXPECT_EQ(rec.events[0].kind, SessionEventKind::kFinal);
    EXPECT_EQ(rec.events[0].text, "<mock:3200>");  // onset drain(320) + 9 more frames = 3200
    EXPECT_EQ(session.state(), SessionState::kIdle);
    EXPECT_EQ(session.stats().forced_finalizations, 1U);
}

TEST(Session, OverflowingSinglePushDropsExcessAndCountsIt) {
    Recorder rec;
    MockBackend backend;
    Session session(fast_test_config(), backend, inline_runner, rec.sink());
    // buffer capacity = preroll(0) + min_speech(320) + max_utterance(3200) = 3520.

    session.push(loud_frame());  // onset; buffer=320; capacity remaining = 3200
    const std::vector<float> big_chunk(5000, 0.1f);  // far more than the remaining capacity
    session.push(big_chunk);

    EXPECT_GT(session.stats().dropped_samples, 0U);
    EXPECT_EQ(session.stats().dropped_samples, 5000U - 3200U);
    ASSERT_EQ(rec.events.size(), 1U);  // the overflow forces a finalize
    EXPECT_EQ(rec.events[0].kind, SessionEventKind::kFinal);
    EXPECT_EQ(session.stats().forced_finalizations, 1U);
}

TEST(Session, EndOfStreamDuringSpeechFinalizesAndFlushes) {
    Recorder rec;
    MockBackend backend;
    Session session(fast_test_config(), backend, inline_runner, rec.sink());

    session.push(loud_frame());
    session.push(loud_frame());
    session.end_of_stream();

    ASSERT_EQ(rec.events.size(), 1U);
    EXPECT_EQ(rec.events[0].kind, SessionEventKind::kFinal);
    EXPECT_EQ(rec.events[0].text, "<mock:640>");
    EXPECT_EQ(session.state(), SessionState::kIdle);
}

TEST(Session, EndOfStreamDuringSilenceOnlyResetsWithoutFinalOrBackendCall) {
    Recorder rec;
    MockBackend backend;
    Session session(fast_test_config(), backend, inline_runner, rec.sink());

    session.push(silent_frame());
    session.push(silent_frame());
    session.end_of_stream();

    EXPECT_TRUE(rec.events.empty());
    EXPECT_EQ(session.state(), SessionState::kIdle);
}

TEST(Session, ExplicitResetAbortsInProgressUtteranceWithoutEmittingFinal) {
    Recorder rec;
    MockBackend backend;
    Session session(fast_test_config(), backend, inline_runner, rec.sink());

    session.push(loud_frame());
    session.push(loud_frame());
    ASSERT_EQ(session.state(), SessionState::kSpeech);

    session.reset();

    EXPECT_TRUE(rec.events.empty());  // explicit reset never flushes a FINAL
    EXPECT_EQ(session.state(), SessionState::kIdle);
}

TEST(Session, SecondUtteranceAfterFinalizeNeverReprocessesOldAudio) {
    Recorder rec;
    MockBackend backend;
    Session session(fast_test_config(), backend, inline_runner, rec.sink());

    // First utterance: 3 loud frames (960 samples) -- which also crosses
    // the 2-frame interval, so expect one PARTIAL along the way -- then a
    // long pause (2 silent frames) finalizes it at 1600 samples total.
    session.push(loud_frame());
    session.push(loud_frame());
    session.push(loud_frame());
    session.push(silent_frame());
    session.push(silent_frame());
    ASSERT_EQ(rec.events.size(), 2U);
    EXPECT_EQ(rec.events[0].kind, SessionEventKind::kPartial);
    EXPECT_EQ(rec.events[0].text, "<mock:960>");
    EXPECT_EQ(rec.events[1].kind, SessionEventKind::kFinal);
    EXPECT_EQ(rec.events[1].text, "<mock:1600>");  // 3 loud + 2 silent frames

    // Second utterance: a single loud frame. If old audio leaked through,
    // the backend would see more than 320 samples.
    session.push(loud_frame());
    EXPECT_EQ(session.state(), SessionState::kSpeech);
    session.end_of_stream();

    ASSERT_EQ(rec.events.size(), 3U);
    EXPECT_EQ(rec.events[2].kind, SessionEventKind::kFinal);
    EXPECT_EQ(rec.events[2].text, "<mock:320>");
}

TEST(Session, BackendErrorDuringFinalizeEmitsErrorAndLeavesSessionUsable) {
    Recorder rec;
    ThrowingBackend backend;
    Session session(fast_test_config(), backend, inline_runner, rec.sink());

    session.push(loud_frame());
    session.push(silent_frame());
    session.push(silent_frame());  // triggers the long-pause finalize -> throws

    ASSERT_EQ(rec.events.size(), 1U);
    EXPECT_EQ(rec.events[0].kind, SessionEventKind::kError);
    EXPECT_FALSE(rec.events[0].error.empty());
    EXPECT_EQ(session.stats().backend_errors, 1U);
    EXPECT_EQ(session.state(), SessionState::kIdle);  // engine keeps running; session is reusable

    // The session must still work for a new utterance.
    session.push(loud_frame());
    EXPECT_EQ(session.state(), SessionState::kSpeech);
}

TEST(Session, BackendErrorDuringPartialDecodeAlsoResetsSession) {
    Recorder rec;
    ThrowingBackend backend;
    Session session(fast_test_config(), backend, inline_runner, rec.sink());  // interval == 2 frames

    session.push(loud_frame());  // onset
    session.push(loud_frame());  // buffer=640: still below interval counter threshold (counter=320)
    session.push(loud_frame());  // counter=640 -> decode() called -> throws

    ASSERT_EQ(rec.events.size(), 1U);
    EXPECT_EQ(rec.events[0].kind, SessionEventKind::kError);
    EXPECT_EQ(session.stats().backend_errors, 1U);
    EXPECT_EQ(session.state(), SessionState::kIdle);
}

// ---- asynchronous scheduling (ManualJobRunner) -------------------------------

TEST(SessionAsync, AtMostOneJobInFlightAndDuePartialsCoalesceOntoLatestAudio) {
    Recorder rec;
    MockBackend backend;
    ManualJobRunner runner;
    Session session(fast_test_config(), backend, runner, rec.sink());  // interval == 2 frames

    for (int i = 0; i < 3; ++i) {
        session.push(loud_frame());  // #3 dispatches a partial over 960 samples
    }
    ASSERT_EQ(runner.pending(), 1U);
    EXPECT_EQ(runner.front_samples(), 960U);

    session.push(loud_frame());
    session.push(loud_frame());  // #5: another partial is due, but one is in flight
    EXPECT_EQ(runner.pending(), 1U);  // still only the first: never two at once
    EXPECT_EQ(session.stats().coalesced_partials, 1U);

    runner.complete_next();  // delivers the first, then dispatches the coalesced one
    ASSERT_EQ(rec.events.size(), 1U);
    EXPECT_EQ(rec.events[0].text, "<mock:960>");
    ASSERT_EQ(runner.pending(), 1U);
    EXPECT_EQ(runner.front_samples(), 1600U);  // over the latest audio, not the stale 1280

    runner.complete_next();
    ASSERT_EQ(rec.events.size(), 2U);
    EXPECT_EQ(rec.events[1].kind, SessionEventKind::kPartial);
    EXPECT_EQ(rec.events[1].text, "<mock:1600>");
}

TEST(SessionAsync, FinalRequestedWhilePartialInFlightIsDeferredNotDropped) {
    Recorder rec;
    MockBackend backend;
    ManualJobRunner runner;
    Session session(fast_test_config(), backend, runner, rec.sink());

    for (int i = 0; i < 3; ++i) {
        session.push(loud_frame());  // partial in flight over 960 samples
    }
    session.push(silent_frame());
    session.push(silent_frame());  // long pause: finalize requested, but the session is busy
    EXPECT_EQ(runner.pending(), 1U);
    EXPECT_TRUE(session.has_pending_work());

    runner.complete_next();  // the partial; its completion dispatches the deferred FINAL
    ASSERT_EQ(runner.pending(), 1U);
    EXPECT_TRUE(runner.front_is_final());
    EXPECT_EQ(runner.front_samples(), 1600U);  // the whole utterance

    runner.complete_next();
    ASSERT_EQ(rec.events.size(), 2U);
    EXPECT_EQ(rec.events[0].kind, SessionEventKind::kPartial);
    EXPECT_EQ(rec.events[1].kind, SessionEventKind::kFinal);
    EXPECT_EQ(rec.events[1].text, "<mock:1600>");
    EXPECT_FALSE(session.has_pending_work());
}

TEST(SessionAsync, NextUtteranceAccumulatesInOtherBufferWhileFinalIsInFlight) {
    Recorder rec;
    MockBackend backend;
    ManualJobRunner runner;
    Session session(fast_test_config(), backend, runner, rec.sink());

    session.push(loud_frame());
    session.push(silent_frame());
    session.push(silent_frame());  // utterance 1 (960 samples): FINAL dispatched, in flight
    ASSERT_EQ(runner.pending(), 1U);

    session.push(loud_frame());  // utterance 2 starts while utterance 1's FINAL runs
    EXPECT_EQ(session.state(), SessionState::kSpeech);
    EXPECT_TRUE(session.ready_for_audio());
    EXPECT_EQ(runner.pending(), 1U);

    runner.complete_next();
    ASSERT_EQ(rec.events.size(), 1U);
    EXPECT_EQ(rec.events[0].text, "<mock:960>");  // utterance 1 only: no audio from utterance 2

    session.end_of_stream();
    runner.complete_next();
    ASSERT_EQ(rec.events.size(), 2U);
    EXPECT_EQ(rec.events[1].kind, SessionEventKind::kFinal);
    EXPECT_EQ(rec.events[1].text, "<mock:320>");
}

TEST(SessionAsync, BothBuffersBusyStallsTheSessionUntilAFinalCompletes) {
    Recorder rec;
    MockBackend backend;
    ManualJobRunner runner;
    Session session(fast_test_config(), backend, runner, rec.sink());

    for (int u = 0; u < 2; ++u) {  // two utterances, neither FINAL completed yet
        session.push(loud_frame());
        session.push(silent_frame());
        session.push(silent_frame());
    }
    EXPECT_FALSE(session.ready_for_audio());  // both buffers finalizing: backpressure
    EXPECT_EQ(runner.pending(), 1U);          // and still one job at a time

    session.push(loud_frame());  // nowhere to put it
    EXPECT_EQ(session.stats().dropped_samples, 320U);

    runner.complete_next();  // FINAL 1 frees its buffer and dispatches FINAL 2
    EXPECT_TRUE(session.ready_for_audio());
    ASSERT_EQ(runner.pending(), 1U);
    EXPECT_TRUE(runner.front_is_final());

    runner.complete_next();
    ASSERT_EQ(rec.events.size(), 2U);
    EXPECT_EQ(rec.events[0].kind, SessionEventKind::kFinal);
    EXPECT_EQ(rec.events[1].kind, SessionEventKind::kFinal);
    EXPECT_FALSE(session.has_pending_work());
}

TEST(SessionAsync, ResetWithJobInFlightDiscardsItsResultAndStaysUsable) {
    Recorder rec;
    MockBackend backend;
    ManualJobRunner runner;
    Session session(fast_test_config(), backend, runner, rec.sink());

    for (int i = 0; i < 3; ++i) {
        session.push(loud_frame());  // partial in flight
    }
    session.reset();
    EXPECT_EQ(session.state(), SessionState::kIdle);
    EXPECT_TRUE(session.has_pending_work());  // the job is still running somewhere

    runner.complete_next();
    EXPECT_TRUE(rec.events.empty());  // result discarded
    EXPECT_FALSE(session.has_pending_work());

    session.push(loud_frame());
    session.end_of_stream();
    runner.complete_next();
    ASSERT_EQ(rec.events.size(), 1U);
    EXPECT_EQ(rec.events[0].text, "<mock:320>");  // nothing from before the reset
}

TEST(SessionAsync, FailedPartialWhileFinalPendingReplacesThatFinalWithError) {
    Recorder rec;
    ThrowingBackend backend;
    ManualJobRunner runner;
    Session session(fast_test_config(), backend, runner, rec.sink());

    for (int i = 0; i < 3; ++i) {
        session.push(loud_frame());  // partial in flight
    }
    session.push(silent_frame());
    session.push(silent_frame());  // FINAL deferred behind it

    runner.complete_next();  // partial throws
    EXPECT_EQ(runner.pending(), 0U);  // the utterance is abandoned: no FINAL dispatched
    ASSERT_EQ(rec.events.size(), 1U);
    EXPECT_EQ(rec.events[0].kind, SessionEventKind::kError);
    EXPECT_FALSE(session.has_pending_work());
    EXPECT_TRUE(session.ready_for_audio());
}

}  // namespace
}  // namespace sasr
