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
// cache). Owned by exactly one sasr::Session; never shared between
// sessions and never called from two threads at once.
//
// decode()/finalize() take the WHOLE utterance accepted so far as `pcm`:
// a zero-copy view into the caller's own UtteranceBuffer (see
// src/streaming/utterance_buffer.hpp), valid only for the duration of the
// call. There is no separate accept() step and the stream keeps no copy
// of the audio itself: the caller is already the audio's one owner. A
// backend that must recompute from scratch (e.g. Whisper, over its fixed
// window) just runs against `pcm` directly; a backend with genuine
// incremental state (e.g. a streaming transducer's cached activations)
// compares pcm.size() against how much of it it has already consumed to
// process only the new suffix. Either way, Session never has to keep a
// second copy of the audio alongside the backend's own.
class AsrStream {
public:
    virtual ~AsrStream() = default;

    AsrStream() = default;
    AsrStream(const AsrStream&) = delete;
    AsrStream& operator=(const AsrStream&) = delete;
    AsrStream(AsrStream&&) = delete;
    AsrStream& operator=(AsrStream&&) = delete;

    // Decodes the whole utterance accepted so far into an INTERIM result.
    // May be called repeatedly as pcm grows, without resetting state.
    virtual AsrResult decode(std::span<const float> pcm) = 0;

    // Same, but flushes and marks the result FINAL for this utterance.
    // After this call, reset() must be called before the next utterance.
    virtual AsrResult finalize(std::span<const float> pcm) = 0;

    // Clears all decode state. The stream must never re-see audio from a
    // prior utterance after this call. Safe to call on a stream that was
    // never used (e.g. a session that closes without ever hearing speech).
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
