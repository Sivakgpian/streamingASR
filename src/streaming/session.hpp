#pragma once

#include "asr/asr_backend.hpp"
#include "common/config.hpp"
#include "streaming/preroll_ring.hpp"
#include "streaming/utterance_buffer.hpp"
#include "vad/energy_vad.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>

namespace sasr {

enum class SessionState { kIdle, kSpeech, kShortPause };

enum class SessionEventKind { kPartial, kFinal, kError };

struct SessionEvent {
    SessionEventKind kind;
    std::string text;   // the result text; empty for kError
    std::string error;  // populated only for kError (backend exception message)
};

// Called synchronously, from inside push()/end_of_stream()/reset(), for
// every PARTIAL/FINAL/ERROR this session produces. Called on whatever
// thread calls into Session (in Phase 4 there is only one).
using SessionEventSink = std::function<void(const SessionEvent&)>;

// One streaming session: VAD -> endpointer state machine -> ASR, per
// CLAUDE.md's cumulative-interval design. Single-threaded and synchronous
// for now (see Engine): push() may call into the ASR backend and emit
// events before returning. Not thread-safe; owned by exactly one Engine.
//
// State machine (see docs/ for the full design): IDLE -(onset)-> SPEECH
// <-> SHORT_PAUSE -(pause_threshold_ms of silence)-> finalize -> IDLE.
// A single utterance buffer is used (not yet the ping-pong pair the
// design calls for): with everything synchronous, a backend call always
// completes before more audio can arrive, so there is never a second
// utterance in flight that would need its own buffer. That changes in
// the threaded engine (added later).
//
// Deliberately not yet enforced: Config::trailing_silence_ms (trimming
// trailing silence out of the buffer before the final decode would need
// a mid-buffer excise, which is a real cost for a benefit nobody has
// measured yet -- see CLAUDE.md "benchmark before optimizing"). All
// silence during SHORT_PAUSE is kept in the buffer up to
// pause_threshold_ms, which already bounds it.
class Session {
public:
    Session(Config config, AsrBackend& backend, SessionEventSink on_event);

    // Feeds one chunk of audio (any non-empty size; VAD/endpointing track
    // real sample counts, so it need not be exactly one Config frame).
    // May call into the ASR backend and invoke on_event before returning.
    void push(std::span<const float> pcm);

    // Flushes: if mid-utterance (SPEECH/SHORT_PAUSE), finalizes and emits
    // FINAL; if IDLE, just clears any partial VAD/preroll state. Safe to
    // call once at the end of a stream's life.
    void end_of_stream();

    // Aborts any in-progress utterance without emitting FINAL, and
    // returns to IDLE. For an explicit caller-initiated reset (e.g. the
    // client disconnected).
    void reset();

    [[nodiscard]] SessionState state() const noexcept { return state_; }

    struct Stats {
        std::uint64_t dropped_samples = 0;       // utterance buffer was full; audio discarded
        std::uint64_t forced_finalizations = 0;  // max_utterance_ms reached without a pause
        std::uint64_t backend_errors = 0;        // decode()/finalize() threw
    };
    [[nodiscard]] const Stats& stats() const noexcept { return stats_; }

private:
    void begin_utterance();
    [[nodiscard]] bool check_max_utterance();     // true if it forced a finalize
    [[nodiscard]] bool check_pause_threshold();    // true if it finalized on a long pause
    void maybe_emit_partial();
    void finalize_utterance();
    void reset_to_idle() noexcept;
    void emit(SessionEventKind kind, std::string text);
    void emit_error(const std::string& message);

    Config config_;
    AsrBackend* backend_;  // non-owning; shared process-wide, outlives every session
    SessionEventSink on_event_;

    EnergyVad vad_;
    PrerollRing preroll_;
    UtteranceBuffer buffer_;
    std::unique_ptr<AsrStream> stream_;

    SessionState state_ = SessionState::kIdle;
    std::size_t silence_samples_in_pause_ = 0;
    std::size_t samples_since_last_partial_ = 0;
    // Total audio belonging to the current utterance since onset
    // (preroll-drain amount, then +pcm.size() per push, regardless of how
    // much of it the buffer actually had room for). Compared against
    // Config::preroll_samples() + Config::max_utterance_samples() in
    // check_max_utterance() -- a user-facing policy threshold, which is
    // intentionally smaller than buffer_'s actual capacity (see
    // utterance_buffer_capacity() in session.cpp): the gap is exactly
    // Config::min_speech_samples(), a safety margin so a forced finalize
    // always has room to complete before the physical buffer would
    // overflow.
    std::size_t utterance_samples_ = 0;
    Stats stats_;
};

}  // namespace sasr
