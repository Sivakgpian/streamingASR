#pragma once

#include "asr/asr_backend.hpp"
#include "common/config.hpp"
#include "streaming/job_runner.hpp"
#include "streaming/preroll_ring.hpp"
#include "streaming/utterance_buffer.hpp"
#include "vad/energy_vad.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>

namespace sasr {

enum class SessionState { kIdle, kSpeech, kShortPause };

enum class SessionEventKind { kPartial, kFinal, kError };

struct SessionEvent {
    SessionEventKind kind;
    std::string text;    // the result text; empty for kError
    std::string error;   // populated only for kError (backend exception message)
    AsrTimings timings;  // forwarded from the AsrResult; zero for kError
};

// Called for every PARTIAL/FINAL/ERROR this session produces, on the
// session's owning thread (see Session).
using SessionEventSink = std::function<void(const SessionEvent&)>;

// One streaming session: VAD -> endpointer state machine -> ASR, per
// CLAUDE.md's cumulative-interval design.
//
// Threading: every method, and every JobRunner callback, runs on one
// thread -- the session's owner. Session has no locks; it relies on
// that. With InlineJobRunner (Engine) the owner is whoever calls push(),
// and events are emitted before push() returns. With
// ThreadPoolJobRunner (ThreadedEngine) the owner is the engine thread,
// and events arrive later, when it pumps completions.
//
// State machine: IDLE -(onset)-> SPEECH <-> SHORT_PAUSE -(pause_threshold_ms
// of silence, max_utterance_ms, or end_of_stream)-> finalize -> IDLE.
//
// Scheduling invariants:
//   - At most one ASR job in flight per session (there is one AsrStream,
//     and AsrStream is not thread-safe).
//   - FINALs are never dropped: a finalize requested while a job is in
//     flight waits in pending_finals_ and is dispatched before any
//     partial. Partials coalesce instead: interval ticks that arrive
//     while a job is in flight collapse into one partial, dispatched
//     over the latest audio once the session is free.
//   - Two utterance buffers (ping-pong): when an utterance finalizes, its
//     buffer stays untouched until its FINAL completes, while the next
//     utterance accumulates in the other one. If both are busy, the
//     session stops accepting audio (ready_for_audio() == false) until a
//     FINAL completes -- the backpressure point.
//   - A buffer referenced by an in-flight job is never cleared or
//     written below the length the job was given (the job reads [0, n);
//     appends only write at >= n, in a buffer whose storage never moves).
//
// Not yet enforced: Config::trailing_silence_ms (trimming would need a
// mid-buffer excise; no benchmark has shown it's worth the cost yet).
class Session {
public:
    // `backend` is only used here, to create this session's stream.
    // `runner` must outlive the session.
    Session(Config config, AsrBackend& backend, JobRunner& runner, SessionEventSink on_event);

    // Pinned in memory: in-flight job callbacks capture `this`.
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;
    Session(Session&&) = delete;
    Session& operator=(Session&&) = delete;

    // Feeds one chunk of audio (any non-empty size). If
    // !ready_for_audio(), the audio is dropped and counted in
    // Stats::dropped_samples instead.
    void push(std::span<const float> pcm);

    // Finalizes the in-progress utterance, if any (its FINAL may be
    // emitted later if a job is in flight). In IDLE it only clears
    // partial VAD/preroll state.
    void end_of_stream();

    // Abandons the in-progress utterance and any not-yet-dispatched
    // FINALs, without emitting them. A job already in flight still
    // completes, but its result is discarded. Returns to IDLE.
    void reset();

    [[nodiscard]] SessionState state() const noexcept { return state_; }

    // False while both utterance buffers are busy finalizing: the owner
    // must stop feeding audio until this turns true again.
    [[nodiscard]] bool ready_for_audio() const noexcept { return active_ != kNoBuffer; }

    // True while an ASR job is in flight or a FINAL is waiting to be
    // dispatched. The session must not be destroyed while this is true:
    // the in-flight callback refers to it.
    [[nodiscard]] bool has_pending_work() const noexcept {
        return in_flight_.has_value() || pending_finals_count_ > 0;
    }

    struct Stats {
        std::uint64_t dropped_samples = 0;       // no room (buffer full or session stalled)
        std::uint64_t forced_finalizations = 0;  // max_utterance_ms reached without a pause
        std::uint64_t backend_errors = 0;        // decode()/finalize() threw
        std::uint64_t coalesced_partials = 0;    // partials that waited behind an in-flight job
    };
    [[nodiscard]] const Stats& stats() const noexcept { return stats_; }

private:
    static constexpr std::size_t kNoBuffer = 2;  // active_ when both buffers are busy

    enum class BufferState {
        kFree,          // empty, available
        kActive,        // receiving the current utterance
        kFinalPending,  // utterance done; FINAL queued or in flight
        kDraining,      // abandoned, but an in-flight job still reads it
    };

    struct InFlight {
        std::size_t buffer;
        bool is_final;
        bool discard;  // reset() happened: drop the result when it arrives
    };

    UtteranceBuffer& active_buffer() { return buffers_[active_]; }

    void begin_utterance();
    [[nodiscard]] bool check_max_utterance();   // true if it requested a finalize
    [[nodiscard]] bool check_pause_threshold();  // true if it requested a finalize
    void maybe_request_partial();
    void request_finalize();
    void try_dispatch();
    void dispatch(std::size_t buffer, bool is_final);
    void on_job_done(std::size_t buffer, bool is_final, JobOutcome outcome);
    void abandon_utterance(std::size_t buffer);
    void activate_free_buffer();
    void free_buffer(std::size_t buffer);
    void release_buffer(std::size_t buffer);
    void reset_utterance_state();

    void push_pending_final(std::size_t buffer);
    std::size_t pop_pending_final();
    bool remove_pending_final(std::size_t buffer);

    void emit(SessionEventKind kind, std::string text, AsrTimings timings);
    void emit_error(std::string message);

    Config config_;
    JobRunner* runner_;  // non-owning; outlives the session
    SessionEventSink on_event_;

    EnergyVad vad_;
    PrerollRing preroll_;
    std::array<UtteranceBuffer, 2> buffers_;
    std::unique_ptr<AsrStream> stream_;

    std::array<BufferState, 2> buffer_state_{BufferState::kActive, BufferState::kFree};
    std::size_t active_ = 0;
    std::array<std::size_t, 2> pending_finals_{};  // FIFO; at most one per buffer
    std::size_t pending_finals_count_ = 0;
    std::optional<InFlight> in_flight_;
    bool partial_due_ = false;

    SessionState state_ = SessionState::kIdle;
    std::size_t silence_samples_in_pause_ = 0;
    std::size_t samples_since_last_partial_ = 0;
    // Audio belonging to the current utterance since onset (preroll-drain
    // amount, then +pcm.size() per push, whether or not it all fit).
    // Compared against preroll_samples() + max_utterance_samples() -- a
    // policy threshold deliberately below the buffer's capacity (see
    // session.cpp), so a forced finalize always fires before overflow.
    std::size_t utterance_samples_ = 0;
    Stats stats_;
};

}  // namespace sasr
