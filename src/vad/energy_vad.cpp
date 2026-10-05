#include "vad/energy_vad.hpp"

#include <cmath>

namespace sasr {

bool EnergyVad::is_speech_frame(std::span<const float> frame) const {
    if (frame.empty()) {
        return false;
    }
    // Accumulate in double: a frame can be hundreds of samples, and RMS of
    // float32 audio in [-1, 1) is well within double's range/precision, so
    // this costs nothing and avoids any (however unlikely) accumulation
    // drift from staying in float.
    double sum_sq = 0.0;
    for (const float sample : frame) {
        const double s = static_cast<double>(sample);  // explicit: -Wdouble-promotion
        sum_sq += s * s;
    }
    const double rms = std::sqrt(sum_sq / static_cast<double>(frame.size()));
    return rms >= static_cast<double>(threshold_);
}

bool EnergyVad::process(std::span<const float> frame) {
    const bool above = is_speech_frame(frame);

    if (in_speech_) {
        if (above) {
            return true;
        }
        in_speech_ = false;
        candidate_samples_ = 0;
        return false;  // immediate offset; no debounce on this side
    }

    if (!above) {
        candidate_samples_ = 0;  // any below-threshold frame breaks the candidate run
        return false;
    }

    candidate_samples_ += frame.size();
    if (candidate_samples_ >= min_speech_samples_) {
        in_speech_ = true;
        return true;  // this frame confirms onset
    }
    return false;  // still just a candidate
}

}  // namespace sasr
