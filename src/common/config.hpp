#pragma once

#include <cstddef>
#include <cstdint>

namespace sasr {

// Every tunable setting of the engine, in one place (like a Python config.py).
// Change a default here, or create a Config and override fields:
//     sasr::Config cfg;
//     cfg.frame_ms = 10;
struct Config {
    // ---- Audio (internal format: mono float32) ----
    std::uint32_t sample_rate = 16000;  // Hz; input files must match
    std::uint32_t frame_ms = 20;        // length of each AudioFrame

    // ---- Streaming (M2) settings will be added here ----

    // Samples per frame: 16000 Hz * 20 ms / 1000 = 320.
    [[nodiscard]] std::size_t frame_samples() const {
        return std::size_t{sample_rate} * frame_ms / 1000;
    }
};

}  // namespace sasr
