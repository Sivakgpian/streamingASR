// Benchmarks the real whisper.cpp backend through the full Engine/
// Session/VAD pipeline, against tests/data/en1.wav (real speech, not
// synthetic). Only built when SASR_WITH_WHISPER=ON; SkipWithError()s at
// runtime if the model file isn't present (scripts/download_model.sh).
//
// Deliberately NOT measured here: allocations/sec and per-stream memory
// (would need tooling this environment doesn't have -- see docs/, which
// found perf/heaptrack/valgrind all missing). peak_rss_kb() reports a
// process-wide, monotonic high-water mark (see profiling/metrics.hpp),
// so both benchmarks are pinned to exactly 1 iteration: on a 2nd run in
// the same process, memory already at the prior peak would show as a 0
// delta, which would be about benchmark repetition, not the model or
// the pipeline.
#include "asr/whisper_cpp_backend.hpp"
#include "audio/wav_reader.hpp"
#include "common/config.hpp"
#include "profiling/metrics.hpp"
#include "streaming/engine.hpp"

#include <benchmark/benchmark.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace sasr {
namespace {

std::string model_path() {
    const std::string path = SASR_BENCH_WHISPER_MODEL;
    return std::filesystem::exists(path) ? path : std::string{};
}

Config make_whisper_config() {
    Config config;
    config.model_path = model_path();
    config.asr_backend = Config::AsrBackendKind::kWhisperCpp;
    return config;
}

void BM_ModelLoad(benchmark::State& state) {
    if (model_path().empty()) {
        state.SkipWithError("model not found (run scripts/download_model.sh)");
        return;
    }
    for (auto _ : state) {
        const long rss_before = peak_rss_kb();
        const Config config = make_whisper_config();
        WhisperCppBackend backend(config);
        const long rss_after = peak_rss_kb();
        state.counters["rss_after_load_kb"] = static_cast<double>(rss_after);
        state.counters["model_rss_delta_kb"] = static_cast<double>(rss_after - rss_before);
    }
}
BENCHMARK(BM_ModelLoad)->Iterations(1);

void BM_WhisperEn1(benchmark::State& state) {
    if (model_path().empty()) {
        state.SkipWithError("model not found (run scripts/download_model.sh)");
        return;
    }
    const Config config = make_whisper_config();
    WhisperCppBackend backend(config);

    // Read the file once; replay the same in-memory audio every
    // iteration so iteration time measures the pipeline, not disk I/O.
    std::vector<float> audio;
    {
        WavReader reader(SASR_BENCH_DATA_DIR "/en1.wav", config);
        std::vector<float> chunk(config.frame_samples());
        while (const std::size_t n = reader.read_into(chunk)) {
            audio.insert(audio.end(), chunk.begin(), chunk.begin() + static_cast<std::ptrdiff_t>(n));
        }
    }
    const double audio_s = static_cast<double>(audio.size()) / static_cast<double>(config.sample_rate);

    for (auto _ : state) {
        Engine engine(config, backend);
        std::vector<SessionEvent> events;
        constexpr SessionId kSession = 1;
        engine.open_session(kSession, [&events](const SessionEvent& e) { events.push_back(e); });

        LatencyRecorder push_latency(audio.size() / config.frame_samples() + 1);
        std::size_t offset = 0;
        while (offset < audio.size()) {
            const std::size_t n = std::min(config.frame_samples(), audio.size() - offset);
            {
                ScopedTimer timer(push_latency);
                engine.push(kSession, std::span<const float>(audio).subspan(offset, n));
            }
            offset += n;
        }
        engine.end_of_stream(kSession);

        std::int64_t final_infer_us = 0;
        std::size_t finals = 0;
        std::size_t partials = 0;
        for (const auto& e : events) {
            if (e.kind == SessionEventKind::kFinal) {
                final_infer_us += e.timings.infer_us;
                ++finals;
            } else if (e.kind == SessionEventKind::kPartial) {
                ++partials;
            }
        }

        const LatencyPercentiles p = percentiles(push_latency);
        state.counters["push_p50_us"] = static_cast<double>(p.p50);
        state.counters["push_p90_us"] = static_cast<double>(p.p90);
        state.counters["push_p95_us"] = static_cast<double>(p.p95);
        state.counters["push_p99_us"] = static_cast<double>(p.p99);
        state.counters["finals"] = static_cast<double>(finals);
        state.counters["partials"] = static_cast<double>(partials);
        // RTF restricted to just the FINAL calls' own compute, i.e. "if we
        // only ever emitted finals, never partials, what would RTF be".
        state.counters["rtf_final_only"] = (static_cast<double>(final_infer_us) / 1e6) / audio_s;
        state.counters["peak_rss_kb"] = static_cast<double>(peak_rss_kb());
    }
    // RTF (total, partials included) = the standard `real_time` column / audio_s.
    state.counters["audio_s"] = audio_s;
}
BENCHMARK(BM_WhisperEn1)->Iterations(1)->Unit(benchmark::kMillisecond);

}  // namespace
}  // namespace sasr
