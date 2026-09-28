#include "common/version.hpp"

#include <benchmark/benchmark.h>

// Placeholder benchmark proving the harness works. Replace with real hot-path
// benchmarks (frame conversion, ring buffer, VAD) as modules land.
static void BM_VersionString(benchmark::State& state) {
    for (auto _ : state) {
        benchmark::DoNotOptimize(sasr::version_string());
    }
}
BENCHMARK(BM_VersionString);
