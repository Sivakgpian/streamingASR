#include "vad/energy_vad.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <cstddef>
#include <numbers>
#include <random>
#include <span>
#include <vector>

namespace sasr {
namespace {

// ---- synthetic signal generators ------------------------------------------
// All deterministic (fixed seed where randomness is involved) so these
// tests never flake.

std::vector<float> make_silence(std::size_t n) { return std::vector<float>(n, 0.0f); }

// A sine wave with the given peak amplitude; RMS = amplitude / sqrt(2).
std::vector<float> make_tone(std::size_t n, float amplitude, float freq_hz, float sample_rate) {
    std::vector<float> out(n);
    for (std::size_t i = 0; i < n; ++i) {
        const float t = static_cast<float>(i) / sample_rate;
        out[i] = amplitude * std::sin(2.0f * std::numbers::pi_v<float> * freq_hz * t);
    }
    return out;
}

// Uniform white noise in [-amplitude, amplitude]; RMS = amplitude / sqrt(3).
std::vector<float> make_noise(std::size_t n, float amplitude, unsigned seed) {
    std::mt19937 rng(seed);
    std::uniform_real_distribution<float> dist(-amplitude, amplitude);
    std::vector<float> out(n);
    for (float& s : out) {
        s = dist(rng);
    }
    return out;
}

// ---- test fixture ----------------------------------------------------------

// Defaults: sample_rate=16000, frame_ms=20 (320 samples/frame),
// vad_threshold=0.02, min_speech_ms=100 (1600 samples = 5 frames).
const Config kConfig;
constexpr std::size_t kFrameSamples = 320;
constexpr std::size_t kFramesForOnset = 5;  // 1600 / 320

std::vector<std::span<const float>> as_frames(const std::vector<float>& audio) {
    std::vector<std::span<const float>> frames;
    for (std::size_t off = 0; off + kFrameSamples <= audio.size(); off += kFrameSamples) {
        frames.push_back(std::span<const float>(audio).subspan(off, kFrameSamples));
    }
    return frames;
}

// ---- tests ------------------------------------------------------------------

TEST(EnergyVad, SilenceNeverTriggersSpeech) {
    EnergyVad vad(kConfig);
    const auto silence = make_silence(kFrameSamples * 20);

    for (const auto& frame : as_frames(silence)) {
        EXPECT_FALSE(vad.process(frame));
        EXPECT_FALSE(vad.in_speech());
    }
}

TEST(EnergyVad, EmptyFrameIsTreatedAsSilence) {
    EnergyVad vad(kConfig);
    EXPECT_FALSE(vad.process({}));
    EXPECT_FALSE(vad.in_speech());
}

TEST(EnergyVad, LoudToneConfirmsOnsetExactlyAfterMinSpeechMs) {
    EnergyVad vad(kConfig);
    // Peak 0.1 -> RMS ~0.0707, well above the 0.02 threshold.
    const auto tone = make_tone(kFrameSamples * kFramesForOnset, 0.1f, 440.0f, 16000.0f);
    const auto frames = as_frames(tone);
    ASSERT_EQ(frames.size(), kFramesForOnset);

    for (std::size_t i = 0; i < kFramesForOnset - 1; ++i) {
        EXPECT_FALSE(vad.process(frames[i])) << "frame " << i;
        EXPECT_FALSE(vad.in_speech()) << "frame " << i;
    }
    // The 5th frame pushes the candidate run to exactly min_speech_samples_.
    EXPECT_TRUE(vad.process(frames[kFramesForOnset - 1]));
    EXPECT_TRUE(vad.in_speech());
}

TEST(EnergyVad, StaysInSpeechWhileToneContinues) {
    EnergyVad vad(kConfig);
    const auto tone = make_tone(kFrameSamples * (kFramesForOnset + 3), 0.1f, 440.0f, 16000.0f);
    for (const auto& frame : as_frames(tone)) {
        vad.process(frame);
    }
    EXPECT_TRUE(vad.in_speech());
}

TEST(EnergyVad, BriefLoudSpikeShorterThanMinSpeechMsNeverConfirmsOnset) {
    EnergyVad vad(kConfig);
    // 3 loud frames (960 samples) is less than the 1600-sample requirement.
    const auto spike = make_tone(kFrameSamples * 3, 0.1f, 440.0f, 16000.0f);
    for (const auto& frame : as_frames(spike)) {
        EXPECT_FALSE(vad.process(frame));
    }
    EXPECT_FALSE(vad.in_speech());

    // Followed by silence: the candidate run is discarded, not carried over.
    const auto silence = make_silence(kFrameSamples * 2);
    for (const auto& frame : as_frames(silence)) {
        EXPECT_FALSE(vad.process(frame));
    }
    EXPECT_FALSE(vad.in_speech());
}

TEST(EnergyVad, OffsetIsImmediateOnSingleBelowThresholdFrame) {
    EnergyVad vad(kConfig);
    const auto tone = make_tone(kFrameSamples * kFramesForOnset, 0.1f, 440.0f, 16000.0f);
    for (const auto& frame : as_frames(tone)) {
        vad.process(frame);
    }
    ASSERT_TRUE(vad.in_speech());

    const auto silence = make_silence(kFrameSamples);
    EXPECT_FALSE(vad.process(silence));
    EXPECT_FALSE(vad.in_speech());
}

TEST(EnergyVad, ResetRequiresFullMinSpeechMsAgain) {
    EnergyVad vad(kConfig);
    const auto tone = make_tone(kFrameSamples * kFramesForOnset, 0.1f, 440.0f, 16000.0f);
    const auto frames = as_frames(tone);
    for (const auto& frame : frames) {
        vad.process(frame);
    }
    ASSERT_TRUE(vad.in_speech());

    vad.reset();
    EXPECT_FALSE(vad.in_speech());

    // Same tone again: onset must be re-earned frame by frame, not instant.
    for (std::size_t i = 0; i < frames.size() - 1; ++i) {
        EXPECT_FALSE(vad.process(frames[i]));
    }
    EXPECT_TRUE(vad.process(frames.back()));
}

TEST(EnergyVad, QuietNoiseBelowThresholdDoesNotTriggerSpeech) {
    EnergyVad vad(kConfig);
    // Amplitude 0.02 -> RMS ~0.0115, below the 0.02 threshold.
    const auto noise = make_noise(kFrameSamples * 20, 0.02f, /*seed=*/1);
    for (const auto& frame : as_frames(noise)) {
        EXPECT_FALSE(vad.process(frame));
    }
    EXPECT_FALSE(vad.in_speech());
}

TEST(EnergyVad, LoudNoiseAboveThresholdTriggersLikeTone) {
    EnergyVad vad(kConfig);
    // Amplitude 0.1 -> RMS ~0.0577, above the 0.02 threshold: energy VAD
    // reacts to amplitude, not to the signal being tonal.
    const auto noise = make_noise(kFrameSamples * kFramesForOnset, 0.1f, /*seed=*/2);
    const auto frames = as_frames(noise);
    for (std::size_t i = 0; i < frames.size() - 1; ++i) {
        EXPECT_FALSE(vad.process(frames[i]));
    }
    EXPECT_TRUE(vad.process(frames.back()));
    EXPECT_TRUE(vad.in_speech());
}

TEST(EnergyVad, HigherThresholdRequiresLouderSignal) {
    Config loud_only = kConfig;
    loud_only.vad_threshold = 0.08f;  // above the 0.1-peak tone's ~0.0707 RMS
    EnergyVad vad(loud_only);

    const auto tone = make_tone(kFrameSamples * kFramesForOnset, 0.1f, 440.0f, 16000.0f);
    for (const auto& frame : as_frames(tone)) {
        EXPECT_FALSE(vad.process(frame));
    }
    EXPECT_FALSE(vad.in_speech());
}

}  // namespace
}  // namespace sasr
