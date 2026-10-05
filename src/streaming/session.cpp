#include "streaming/session.hpp"

#include <exception>
#include <utility>

namespace sasr {

namespace {
// buffer_ must hold everything PrerollRing could ever drain into it
// (preroll + the onset-debounce candidate run) *plus* a full
// max_utterance_ms of subsequent audio -- see preroll_ring.hpp's sizing
// note. This is why it's bigger than the plan's first-pass
// "preroll + max_utterance" estimate.
std::size_t utterance_buffer_capacity(const Config& config) {
    return config.preroll_samples() + config.min_speech_samples() + config.max_utterance_samples();
}
}  // namespace

Session::Session(Config config, AsrBackend& backend, SessionEventSink on_event)
    : config_(std::move(config)),
      backend_(&backend),
      on_event_(std::move(on_event)),
      vad_(config_),
      preroll_(config_.preroll_samples() + config_.min_speech_samples()),
      buffer_(utterance_buffer_capacity(config_)),
      stream_(backend_->create_stream()) {}

void Session::push(std::span<const float> pcm) {
    if (pcm.empty()) {
        return;
    }

    if (state_ == SessionState::kIdle) {
        preroll_.push(pcm);
        if (vad_.process(pcm)) {
            begin_utterance();
            if (check_max_utterance()) {
                return;  // pathological config: preroll+candidate alone filled the buffer
            }
            maybe_emit_partial();
        }
        return;
    }

    // SPEECH or SHORT_PAUSE.
    const bool speech = vad_.process(pcm);
    const std::size_t written = buffer_.append(pcm);
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
    maybe_emit_partial();
}

void Session::end_of_stream() {
    if (state_ == SessionState::kIdle) {
        reset_to_idle();  // nothing to flush; tidy up any mid-candidate VAD/preroll state
        return;
    }
    finalize_utterance();
}

void Session::reset() { reset_to_idle(); }

void Session::begin_utterance() {
    state_ = SessionState::kSpeech;
    const std::size_t drained = preroll_.size();
    preroll_.drain_into(buffer_);
    preroll_.clear();
    utterance_samples_ = drained;
    silence_samples_in_pause_ = 0;
    samples_since_last_partial_ = 0;
}

bool Session::check_max_utterance() {
    const std::size_t threshold = config_.preroll_samples() + config_.max_utterance_samples();
    // buffer_.full() is a defensive backstop: it should never fire before
    // the threshold does (see the sizing note on utterance_samples_), but
    // this guarantees the physical buffer is never exceeded even if some
    // future config combination made the margin tighter than expected.
    if (utterance_samples_ < threshold && !buffer_.full()) {
        return false;
    }
    ++stats_.forced_finalizations;
    finalize_utterance();
    return true;
}

bool Session::check_pause_threshold() {
    if (state_ != SessionState::kShortPause) {
        return false;
    }
    if (silence_samples_in_pause_ < config_.pause_threshold_samples()) {
        return false;
    }
    finalize_utterance();
    return true;
}

void Session::maybe_emit_partial() {
    if (!config_.partials_enabled) {
        return;
    }
    if (samples_since_last_partial_ < config_.interval_samples()) {
        return;
    }
    samples_since_last_partial_ = 0;

    try {
        const AsrResult result = stream_->decode(buffer_.view());
        emit(SessionEventKind::kPartial, result.text, result.timings);
    } catch (const std::exception& e) {
        emit_error(e.what());
    }
}

void Session::finalize_utterance() {
    try {
        const AsrResult result = stream_->finalize(buffer_.view());
        emit(SessionEventKind::kFinal, result.text, result.timings);
    } catch (const std::exception& e) {
        emit_error(e.what());
        return;  // reset_to_idle() already run inside emit_error()
    }
    reset_to_idle();
}

void Session::reset_to_idle() noexcept {
    buffer_.clear();
    preroll_.clear();
    vad_.reset();
    stream_->reset();
    state_ = SessionState::kIdle;
    silence_samples_in_pause_ = 0;
    samples_since_last_partial_ = 0;
    utterance_samples_ = 0;
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

void Session::emit_error(const std::string& message) {
    ++stats_.backend_errors;
    SessionEvent event;
    event.kind = SessionEventKind::kError;
    event.error = message;
    if (on_event_) {
        on_event_(event);
    }
    reset_to_idle();  // D.3: a backend error resets the session; the engine stays up
}

}  // namespace sasr
