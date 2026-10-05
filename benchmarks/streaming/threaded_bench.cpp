// Scaling of ThreadedEngine with 1/2/4/8 concurrent streams, each fed by
// its own producer thread, sharing a fixed pool of 4 workers.
//
// Inference is simulated by MockBackend as a fixed 20 ms *sleep* per
// call. So this measures the engine's own scheduling, queueing and
// scaling -- how latency degrades as streams contend for workers --
// not CPU contention from a real model (for that, see whisper_bench and
// a real backend). Producers push as fast as the engine accepts
// (push_blocking), not paced to real time: this is a capacity test.
//
// Per run:
//   x_realtime   total audio seconds processed per wall-clock second
//   queue_p*_us  how long ASR jobs waited for a free worker (the
//                degradation signal as streams grow)
//   infer_p50_us sanity check: ~20000 (the simulated inference time)
//   finals       must equal streams x utterances (nothing lost)
#include "asr/mock_backend.hpp"
#include "common/config.hpp"
#include "profiling/metrics.hpp"
#include "streaming/threaded_engine.hpp"

#include <benchmark/benchmark.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <thread>
#include <vector>

namespace sasr {
namespace {

constexpr int kUtterances = 10;
constexpr double kSpeechSeconds = 2.5;  // 2 partials + 1 final per utterance at default Config
constexpr double kPauseSeconds = 0.8;   // > default pause_threshold_ms (600 ms)

const std::vector<float>& stream_audio() {
    static const std::vector<float> audio = [] {
        const Config config;
        const auto rate = static_cast<double>(config.sample_rate);
        std::vector<float> out;
        for (int u = 0; u < kUtterances; ++u) {
            out.insert(out.end(), static_cast<std::size_t>(kSpeechSeconds * rate), 0.1f);
            out.insert(out.end(), static_cast<std::size_t>(kPauseSeconds * rate), 0.0f);
        }
        return out;
    }();
    return audio;
}

void BM_ThreadedEngineStreams(benchmark::State& state) {
    const auto streams = static_cast<std::size_t>(state.range(0));
    const std::vector<float>& audio = stream_audio();

    Config config;
    config.asr_workers = 4;  // = physical cores on the dev machine
    config.job_queue_capacity = 8;
    MockBackend backend(std::chrono::milliseconds(20));

    std::vector<std::vector<SessionEvent>> events(streams);
    std::uint64_t coalesced = 0;
    for (auto _ : state) {
        ThreadedEngine engine(config, backend);
        for (std::size_t s = 0; s < streams; ++s) {
            engine.open_session(s, [&events, s](const SessionEvent& e) { events[s].push_back(e); });
        }
        std::vector<std::thread> producers;
        for (std::size_t s = 0; s < streams; ++s) {
            producers.emplace_back([&engine, &audio, s] {
                engine.push_blocking(s, audio);
                engine.end_of_stream(s);
            });
        }
        for (std::thread& t : producers) {
            t.join();
        }
        engine.stop();  // returns once every FINAL has been delivered
        for (std::size_t s = 0; s < streams; ++s) {
            coalesced += engine.stats(s).coalesced_partials;
        }
    }

    LatencyRecorder queue_us(4096);
    LatencyRecorder infer_us(4096);
    std::size_t finals = 0;
    std::size_t partials = 0;
    for (const auto& stream_events : events) {
        for (const SessionEvent& e : stream_events) {
            queue_us.record(e.timings.queue_us);
            infer_us.record(e.timings.infer_us);
            finals += e.kind == SessionEventKind::kFinal ? 1 : 0;
            partials += e.kind == SessionEventKind::kPartial ? 1 : 0;
        }
    }
    const LatencyPercentiles q = percentiles(queue_us);
    const double audio_s = static_cast<double>(audio.size() * streams) / Config{}.sample_rate;
    state.counters["streams"] = static_cast<double>(streams);
    state.counters["audio_s"] = audio_s;
    state.counters["x_realtime"] = benchmark::Counter(audio_s, benchmark::Counter::kIsRate);
    state.counters["finals"] = static_cast<double>(finals);
    state.counters["partials"] = static_cast<double>(partials);
    state.counters["coalesced"] = static_cast<double>(coalesced);
    state.counters["queue_p50_us"] = static_cast<double>(q.p50);
    state.counters["queue_p95_us"] = static_cast<double>(q.p95);
    state.counters["queue_p99_us"] = static_cast<double>(q.p99);
    state.counters["infer_p50_us"] = static_cast<double>(infer_us.percentile(50.0));
}
BENCHMARK(BM_ThreadedEngineStreams)
    ->Arg(1)
    ->Arg(2)
    ->Arg(4)
    ->Arg(8)
    ->Iterations(1)
    ->UseRealTime()
    ->Unit(benchmark::kMillisecond);

}  // namespace
}  // namespace sasr
