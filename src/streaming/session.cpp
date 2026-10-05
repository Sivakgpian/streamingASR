#include "streaming/session.hpp"

#include <utility>

namespace sasr {

namespace {
// A buffer must hold everything PrerollRing can drain into it (preroll +
// the onset-debounce candidate run) *plus* a full max_utterance_ms of
// audio after that -- see preroll_ring.hpp's sizing note.
std::size_t utterance_buffer_capacity(const Config& config) {
    return config.preroll_samples() + config.min_speech_samples() + config.max_utterance_samples();
}
}  // namespace

Session::Session(Config config, AsrBackend& backend, JobRunner& runner, SessionEventSink on_event)
    : config_(std::move(config)),
      runner_(&runner),
      on_event_(std::move(on_event)),
      vad_(config_),
      preroll_(config_.preroll_samples() + config_.min_speech_samples()),
      buffers_{{UtteranceBuffer(utterance_buffer_capacity(config_)),
                UtteranceBuffer(utterance_buffer_capacity(config_))}},
      stream_(backend.create_stream()) {}

void Session::push(std::span<const float> pcm) {
    if (pcm.empty()) {
        return;
    }
    if (active_ == kNoBuffer) {
        stats_.dropped_samples += pcm.size();  // stalled: both buffers finalizing
        return;
    }

    if (state_ == SessionState::kIdle) {
        preroll_.push(pcm);
        if (vad_.process(pcm)) {
            begin_utterance();
            if (check_max_utterance()) {
                return;  // pathological config: preroll+candidate alone filled the buffer
            }
            maybe_request_partial();
        }
        return;
    }

    // SPEECH or SHORT_PAUSE.
    const bool speech = vad_.process(pcm);
    const std::size_t written = active_buffer().append(pcm);
    stats_.dropped_samples += pcm.size() - written;
    utterance_samples_ += pcm.size();

    if (speech) {
        state_ = SessionState::kSpeech;
        silence_samples_in_pause_ = 0;
        samples_since_last_partial_ += pcm.size();
    } else {
        state_ = SessionState::kShortPause;
        silence_samples_in_pause_ += pcm.size();
    }

    if (check_max_utterance()) {
        return;
    }
    if (check_pause_threshold()) {
        return;
    }
    maybe_request_partial();
}

void Session::end_of_stream() {
    if (state_ == SessionState::kIdle) {
        preroll_.clear();  // nothing to flush; drop any half-confirmed onset
        vad_.reset();
        return;
    }
    request_finalize();
}

void Session::reset() {
    if (in_flight_) {
        in_flight_->discard = true;
    }
    while (pending_finals_count_ > 0) {
        release_buffer(pop_pending_final());
    }
    if (active_ != kNoBuffer) {
        if (in_flight_ && in_flight_->buffer == active_) {
            // A partial is still reading this buffer: retire it rather
            // than clear it under the worker, and move to the other one.
            buffer_state_[active_] = BufferState::kDraining;
            active_ = kNoBuffer;
            activate_free_buffer();
        } else {
            active_buffer().clear();
        }
    }
    reset_utterance_state();
    if (!in_flight_) {
        stream_->reset();  // otherwise on_job_done() does it, once the stream is idle
    }
}

void Session::begin_utterance() {
    state_ = SessionState::kSpeech;
    const std::size_t drained = preroll_.size();
    preroll_.drain_into(active_buffer());
    preroll_.clear();
    utterance_samples_ = drained;
    silence_samples_in_pause_ = 0;
    samples_since_last_partial_ = 0;
}

bool Session::check_max_utterance() {
    const std::size_t threshold = config_.preroll_samples() + config_.max_utterance_samples();
    // full() is a backstop: the threshold should always fire first.
    if (utterance_samples_ < threshold && !active_buffer().full()) {
        return false;
    }
    ++stats_.forced_finalizations;
    request_finalize();
    return true;
}

bool Session::check_pause_threshold() {
    if (state_ != SessionState::kShortPause ||
        silence_samples_in_pause_ < config_.pause_threshold_samples()) {
        return false;
    }
    request_finalize();
    return true;
}

void Session::maybe_request_partial() {
    if (!config_.partials_enabled || samples_since_last_partial_ < config_.interval_samples()) {
        return;
    }
    if (!partial_due_) {
        partial_due_ = true;
        if (in_flight_ || pending_finals_count_ > 0) {
            ++stats_.coalesced_partials;  // will run later, over whatever audio exists by then
        }
    }
    try_dispatch();
}

void Session::request_finalize() {
    const std::size_t finished = active_;
    buffer_state_[finished] = BufferState::kFinalPending;
    push_pending_final(finished);
    active_ = kNoBuffer;
    activate_free_buffer();  // may leave active_ == kNoBuffer: stalled until a FINAL completes
    reset_utterance_state();  // also drops a due partial: this utterance's FINAL supersedes it
    try_dispatch();
}

void Session::try_dispatch() {
    if (in_flight_) {
        return;
    }
    if (pending_finals_count_ > 0) {
        dispatch(pop_pending_final(), /*is_final=*/true);
        return;
    }
    if (partial_due_ && active_ != kNoBuffer && state_ != SessionState::kIdle) {
        partial_due_ = false;
        samples_since_last_partial_ = 0;
        dispatch(active_, /*is_final=*/false);
    }
}

void Session::dispatch(std::size_t buffer, bool is_final) {
    in_flight_ = InFlight{buffer, is_final, /*discard=*/false};
    // The span is captured now: the job reads exactly this prefix, even
    // if (for a partial) the buffer keeps growing past it meanwhile.
    const std::span<const float> pcm = buffers_[buffer].view();
    // With InlineJobRunner this calls on_job_done() before returning,
    // which may dispatch the next job: nothing may follow this call.
    runner_->run(*stream_, pcm, is_final, [this, buffer, is_final](JobOutcome outcome) {
        on_job_done(buffer, is_final, std::move(outcome));
    });
}

void Session::on_job_done(std::size_t buffer, bool is_final, JobOutcome outcome) {
    const bool discard = in_flight_->discard;
    in_flight_.reset();

    if (is_final) {
        if (!discard) {
            if (outcome.ok) {
                emit(SessionEventKind::kFinal, std::move(outcome.result.text),
                     outcome.result.timings);
            } else {
                emit_error(std::move(outcome.error));  // replaces this utterance's FINAL
            }
        }
        stream_->reset();  // utterance over: no decode state may leak into the next
        free_buffer(buffer);
    } else {
        if (discard) {
            stream_->reset();
        } else if (outcome.ok) {
            emit(SessionEventKind::kPartial, std::move(outcome.result.text), outcome.result.timings);
        } else {
            emit_error(std::move(outcome.error));
            abandon_utterance(buffer);
        }
        if (buffer_state_[buffer] == BufferState::kDraining) {
            free_buffer(buffer);
        }
    }
    try_dispatch();
}

void Session::abandon_utterance(std::size_t buffer) {
    // A failed partial ends its utterance with ERROR instead of FINAL,
    // whether that utterance was still being recorded or already waiting
    // for its FINAL.
    if (remove_pending_final(buffer)) {
        free_buffer(buffer);
    } else if (buffer == active_) {
        active_buffer().clear();
        reset_utterance_state();
    }
    stream_->reset();
}

void Session::activate_free_buffer() {
    for (std::size_t i = 0; i < buffers_.size(); ++i) {
        if (buffer_state_[i] == BufferState::kFree) {
            buffer_state_[i] = BufferState::kActive;
            active_ = i;
            return;
        }
    }
}

void Session::free_buffer(std::size_t buffer) {
    buffers_[buffer].clear();
    buffer_state_[buffer] = BufferState::kFree;
    if (active_ == kNoBuffer) {
        activate_free_buffer();  // un-stalls the session
    }
}

void Session::release_buffer(std::size_t buffer) {
    if (in_flight_ && in_flight_->buffer == buffer) {
        buffer_state_[buffer] = BufferState::kDraining;  // freed when that job completes
    } else {
        free_buffer(buffer);
    }
}

void Session::reset_utterance_state() {
    preroll_.clear();
    vad_.reset();
    state_ = SessionState::kIdle;
    silence_samples_in_pause_ = 0;
    samples_since_last_partial_ = 0;
    utterance_samples_ = 0;
    partial_due_ = false;
}

void Session::push_pending_final(std::size_t buffer) {
    pending_finals_[pending_finals_count_++] = buffer;  // <= 2: one per buffer
}

std::size_t Session::pop_pending_final() {
    const std::size_t buffer = pending_finals_[0];
    pending_finals_[0] = pending_finals_[1];
    --pending_finals_count_;
    return buffer;
}

bool Session::remove_pending_final(std::size_t buffer) {
    for (std::size_t i = 0; i < pending_finals_count_; ++i) {
        if (pending_finals_[i] == buffer) {
            if (i == 0) {
                pending_finals_[0] = pending_finals_[1];
            }
            --pending_finals_count_;
            return true;
        }
    }
    return false;
}

void Session::emit(SessionEventKind kind, std::string text, AsrTimings timings) {
    if (!on_event_) {
        return;
    }
    SessionEvent event;
    event.kind = kind;
    event.text = std::move(text);
    event.timings = timings;
    on_event_(event);
}

void Session::emit_error(std::string message) {
    ++stats_.backend_errors;
    if (!on_event_) {
        return;
    }
    SessionEvent event;
    event.kind = SessionEventKind::kError;
    event.error = std::move(message);
    on_event_(event);
}

}  // namespace sasr
