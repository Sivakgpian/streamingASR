#pragma once

#include "common/config.hpp"

#include <cstddef>
#include <span>

namespace sasr {

// Energy (RMS amplitude) voice-activity detector with onset hysteresis:
// a frame whose RMS is >= Config::vad_threshold is a "candidate", but
// onset is only confirmed once candidate frames have accumulated at
// least Config::min_speech_ms of audio. This debounces brief loud
// transients (a cough, a door) from being treated as speech. Offset is
// reported immediately, frame by frame, with no debounce here: deciding
// whether a silence is a real pause (and for how long) is the streaming
// engine's job (Config::pause_threshold_ms / trailing_silence_ms),
// because that decision needs to accumulate consecutive-silence duration
// across frames, which belongs with the utterance buffer it also owns.
//
// One EnergyVad is owned by one session; not thread-safe; never allocates
// (process() takes a view and returns a bool, reset() only zeroes fields).
class EnergyVad {
public:
    explicit EnergyVad(const Config& config)
        : threshold_(config.vad_threshold), min_speech_samples_(config.min_speech_samples()) {}

    // Feeds one frame. Returns true iff this frame is part of confirmed
    // speech: either onset was already confirmed on an earlier frame and
    // this frame is still above threshold, or this frame is the one that
    // pushes the accumulated candidate run to min_speech_samples_. An
    // empty frame is treated as silence.
    bool process(std::span<const float> frame);

    // True from the frame that confirmed onset until the next frame
    // below threshold.
    [[nodiscard]] bool in_speech() const noexcept { return in_speech_; }

    // Returns to the initial (silent, no candidate run) state.
    void reset() noexcept {
        candidate_samples_ = 0;
        in_speech_ = false;
    }

private:
    [[nodiscard]] bool is_speech_frame(std::span<const float> frame) const;

    float threshold_;
    std::size_t min_speech_samples_;
    std::size_t candidate_samples_ = 0;  // consecutive above-threshold samples, not yet confirmed
    bool in_speech_ = false;
};

}  // namespace sasr
