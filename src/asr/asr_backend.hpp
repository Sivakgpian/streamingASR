#pragma once

#include <cstdint>
#include <memory>
#include <span>
#include <string>

namespace sasr {

// Wall-clock breakdown of one decode()/finalize() call, for the metrics
// module (added later). Zero-initialized so a backend that doesn't measure
// something just leaves it at 0.
struct AsrTimings {
    std::int64_t queue_us = 0;   // time the job waited before a worker picked it up
    std::int64_t infer_us = 0;   // time spent inside the backend call
};

struct AsrResult {
    std::string text;
    bool is_final = false;
    AsrTimings timings;
};

// One ASR session. Holds per-utterance decode state (e.g. a model's KV
// cache). Owned by exactly one sasr::Session (added later); never shared
// between sessions and never called from two threads at once.
class AsrStream {
public:
    virtual ~AsrStream() = default;

    AsrStream() = default;
    AsrStream(const AsrStream&) = delete;
    AsrStream& operator=(const AsrStream&) = delete;
    AsrStream(AsrStream&&) = delete;
    AsrStream& operator=(AsrStream&&) = delete;

    // Appends audio for the current utterance. `pcm` is a view into the
    // caller's utterance buffer and is only required to stay valid for the
    // duration of this call (implementations must copy anything they need
    // to keep, not retain the span).
    virtual void accept(std::span<const float> pcm) = 0;

    // Decodes everything accepted so far into an INTERIM result. May be
    // called repeatedly without resetting state; audio already accepted
    // is not re-supplied by the caller (see AsrBackend::is_incremental for
    // whether the backend itself recomputes it internally).
    virtual AsrResult decode() = 0;

    // Flushes and returns the FINAL result for the current utterance.
    // After this call, reset() must be called before the next utterance.
    virtual AsrResult finalize() = 0;

    // Clears all decode state. The stream must never re-see audio from a
    // prior utterance after this call.
    virtual void reset() = 0;
};

// Shared, process-wide backend: owns model weights once and creates one
// AsrStream per session. A single AsrBackend may back many concurrent
// AsrStreams (one per session).
class AsrBackend {
public:
    virtual ~AsrBackend() = default;

    AsrBackend() = default;
    AsrBackend(const AsrBackend&) = delete;
    AsrBackend& operator=(const AsrBackend&) = delete;
    AsrBackend(AsrBackend&&) = delete;
    AsrBackend& operator=(AsrBackend&&) = delete;

    [[nodiscard]] virtual std::unique_ptr<AsrStream> create_stream() = 0;

    // True only for backends whose decode() genuinely updates state
    // incrementally, without recomputing audio already accepted (e.g. a
    // streaming transducer with cached state). False for backends that
    // re-run over the whole utterance on every decode() (e.g. Whisper).
    // The session scheduler uses this to decide how aggressively it can
    // schedule INTERIMs without wasting CPU on quadratic re-encoding.
    [[nodiscard]] virtual bool is_incremental() const noexcept = 0;
};

}  // namespace sasr
