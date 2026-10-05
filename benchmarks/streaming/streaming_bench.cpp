// Benchmarks the Engine/Session/VAD pipeline end to end, using
// MockBackend (no real inference cost -- see Phase 6 for a real ASR
// backend). This measures *pipeline overhead*: the cost of VAD, buffer
// copies, and bookkeeping, which any real backend's cost is added on top
// of. For each scenario:
//   - per-push() latency, reported as p50/p90/p95/p99 (CLAUDE.md: never
//     only averages) via custom counters;
//   - the standard `real_time` column is the whole scenario's wall time
//     for one iteration; divide by the `audio_s` counter to get this
//     pipeline's RTF contribution;
//   - event counts and dropped_samples, to catch accidental behavior
//     changes (e.g. a scenario that should finalize once suddenly
//     finalizing twice) directly in the benchmark output.
//
// Self-contained: audio is synthesized in-process (deterministic, fixed
// seed where random), not loaded from scripts/make_test_audio.py's
// output -- that script is for generating files a human can feed to
// examples/transcribe.cpp (added in Phase 6) or listen to, not a build
// dependency of this benchmark.
#include "asr/mock_backend.hpp"
#include "common/config.hpp"
#include "profiling/metrics.hpp"
#include "streaming/engine.hpp"

#include <benchmark/benchmark.h>

#include <cstddef>
#include <cstdint>
#include <random>
#include <string>
#include <vector>

namespace sasr {
namespace {

constexpr std::size_t kFrameSamples = 320;  // 20 ms at 16 kHz

std::vector<float> silent_frame() { return std::vector<float>(kFrameSamples, 0.0f); }
std::vector<float> loud_frame() { return std::vector<float>(kFrameSamples, 0.1f); }  // RMS 0.1

// Uniform noise in [-amplitude, amplitude], fixed seed: deterministic
// across runs, which repeated benchmark iterations depend on for stable
// numbers.
std::vector<float> noisy_frame(float amplitude, std::mt19937& rng) {
    std::uniform_real_distribution<float> dist(-amplitude, amplitude);
    std::vector<float> frame(kFrameSamples);
    for (float& s : frame) {
        s = 0.1f + dist(rng);  // a loud tone-ish level plus noise
    }
    return frame;
}

struct Scenario {
    Config config;
    std::vector<std::vector<float>> frames;
};

std::size_t total_samples(const Scenario& scenario) {
    std::size_t total = 0;
    for (const auto& frame : scenario.frames) {
        total += frame.size();
    }
    return total;
}

Scenario make_silence(double seconds) {
    Scenario s;
    const auto n = static_cast<std::size_t>(seconds * s.config.sample_rate / kFrameSamples);
    s.frames.assign(n, silent_frame());
    return s;
}

Scenario make_continuous_speech(double seconds) {
    Scenario s;
    const auto n = static_cast<std::size_t>(seconds * s.config.sample_rate / kFrameSamples);
    s.frames.assign(n, loud_frame());
    return s;
}

// Alternates `speech_s` of speech with `pause_s` of silence, `cycles` times.
Scenario make_speech_with_pauses(double speech_s, double pause_s, int cycles) {
    Scenario s;
    const auto speech_frames = static_cast<std::size_t>(speech_s * s.config.sample_rate / kFrameSamples);
    const auto pause_frames = static_cast<std::size_t>(pause_s * s.config.sample_rate / kFrameSamples);
    for (int i = 0; i < cycles; ++i) {
        for (std::size_t f = 0; f < speech_frames; ++f) {
            s.frames.push_back(loud_frame());
        }
        for (std::size_t f = 0; f < pause_frames; ++f) {
            s.frames.push_back(silent_frame());
        }
    }
    return s;
}

Scenario make_long_utterance(double seconds) {
    // Default max_utterance_ms (15 s) is well under `seconds`, so this
    // scenario is expected to force multiple finalizations with no pause.
    return make_continuous_speech(seconds);
}

Scenario make_noisy_speech(double seconds, float noise_amplitude) {
    Scenario s;
    std::mt19937 rng(42);  // fixed seed: deterministic across benchmark iterations
    const auto n = static_cast<std::size_t>(seconds * s.config.sample_rate / kFrameSamples);
    s.frames.reserve(n);
    for (std::size_t i = 0; i < n; ++i) {
        s.frames.push_back(noisy_frame(noise_amplitude, rng));
    }
    return s;
}

std::size_t count_kind(const std::vector<SessionEvent>& events, SessionEventKind kind) {
    std::size_t n = 0;
    for (const auto& e : events) {
        n += (e.kind == kind) ? 1 : 0;
    }
    return n;
}

constexpr SessionId kSessionId = 1;

void run_scenario(benchmark::State& state, const Scenario& scenario) {
    for (auto _ : state) {
        MockBackend backend;
        Engine engine(scenario.config, backend);
        std::vector<SessionEvent> events;
        engine.open_session(kSessionId, [&events](const SessionEvent& e) { events.push_back(e); });

        LatencyRecorder push_latency(scenario.frames.size() + 1);
        for (const auto& frame : scenario.frames) {
            ScopedTimer timer(push_latency);
            engine.push(kSessionId, frame);
        }
        engine.end_of_stream(kSessionId);

        const Session::Stats& stats = engine.stats(kSessionId);
        const LatencyPercentiles p = percentiles(push_latency);
        state.counters["push_p50_us"] = static_cast<double>(p.p50);
        state.counters["push_p90_us"] = static_cast<double>(p.p90);
        state.counters["push_p95_us"] = static_cast<double>(p.p95);
        state.counters["push_p99_us"] = static_cast<double>(p.p99);
        state.counters["partials"] = static_cast<double>(count_kind(events, SessionEventKind::kPartial));
        state.counters["finals"] = static_cast<double>(count_kind(events, SessionEventKind::kFinal));
        state.counters["errors"] = static_cast<double>(count_kind(events, SessionEventKind::kError));
        state.counters["dropped_samples"] = static_cast<double>(stats.dropped_samples);
        state.counters["forced_finalizations"] = static_cast<double>(stats.forced_finalizations);
    }
    // RTF for this pipeline (no real ASR cost yet) = real_time / audio_s,
    // where real_time is benchmark's own standard column.
    const double audio_s =
        static_cast<double>(total_samples(scenario)) / static_cast<double>(scenario.config.sample_rate);
    state.counters["audio_s"] = audio_s;
}

// ---- scenarios (the Phase 5/11 matrix, minus concurrency and real
// sample-rate rejection, which need a real backend / a real source) ----

void BM_Silence(benchmark::State& state) {
    static const Scenario scenario = make_silence(5.0);
    run_scenario(state, scenario);
}
BENCHMARK(BM_Silence);

void BM_ShortSpeech(benchmark::State& state) {
    static const Scenario scenario = make_continuous_speech(1.5);
    run_scenario(state, scenario);
}
BENCHMARK(BM_ShortSpeech);

void BM_ContinuousSpeech(benchmark::State& state) {
    static const Scenario scenario = make_continuous_speech(5.0);
    run_scenario(state, scenario);
}
BENCHMARK(BM_ContinuousSpeech);

void BM_SpeechWithShortPauses(benchmark::State& state) {
    // 300 ms pauses, well under the default 600 ms pause_threshold_ms:
    // expect one utterance (and one FINAL) for the whole scenario.
    static const Scenario scenario = make_speech_with_pauses(1.0, 0.3, 4);
    run_scenario(state, scenario);
}
BENCHMARK(BM_SpeechWithShortPauses);

void BM_SpeechWithLongPauses(benchmark::State& state) {
    // 2 s pauses, well over pause_threshold_ms: expect one utterance (one
    // FINAL) per speech run.
    static const Scenario scenario = make_speech_with_pauses(1.0, 2.0, 4);
    run_scenario(state, scenario);
}
BENCHMARK(BM_SpeechWithLongPauses);

void BM_LongUtterance(benchmark::State& state) {
    // 40 s continuous, default max_utterance_ms=15s: expect >= 2 forced
    // finalizations with 0 pauses.
    static const Scenario scenario = make_long_utterance(40.0);
    run_scenario(state, scenario);
}
BENCHMARK(BM_LongUtterance);

void BM_NoisySpeech(benchmark::State& state) {
    // amplitude 0.05 noise on a 0.1 tone: SNR = 20*log10(0.1/0.05) ~ 6 dB.
    static const Scenario scenario = make_noisy_speech(5.0, 0.05f);
    run_scenario(state, scenario);
}
BENCHMARK(BM_NoisySpeech);

}  // namespace
}  // namespace sasr

// No BENCHMARK_MAIN() here: this links against benchmark::benchmark_main
// (see benchmarks/CMakeLists.txt), which already supplies one.
