#pragma once

#include "asr/asr_backend.hpp"
#include "common/config.hpp"
#include "streaming/frame_assembler.hpp"
#include "streaming/job_runner.hpp"
#include "streaming/session.hpp"

#include <cstdint>
#include <memory>
#include <span>
#include <stdexcept>
#include <unordered_map>
#include <utility>

namespace sasr {

using SessionId = std::uint64_t;

class EngineError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// Owns a set of Sessions sharing one AsrBackend. Single-threaded and
// synchronous: every ASR call runs inline (InlineJobRunner), so events
// are emitted before push()/end_of_stream() return. Deterministic, which
// is what tests and simple tools want. For worker threads and a
// non-blocking push(), see ThreadedEngine; both produce the same events
// for the same audio.
class Engine {
public:
    Engine(Config config, AsrBackend& backend) : config_(std::move(config)), backend_(&backend) {}

    // Opens a new session. Throws EngineError if id is already open.
    void open_session(SessionId id, SessionEventSink on_event);

    // Closes a session (aborts any in-progress utterance, no FINAL).
    // Throws EngineError if id is not open.
    void close_session(SessionId id);

    [[nodiscard]] bool has_session(SessionId id) const noexcept;

    // Any chunk size: audio is re-cut into Config::frame_ms frames
    // aligned to the stream, so results don't depend on chunking.
    // Throws EngineError if id is not open.
    void push(SessionId id, std::span<const float> pcm);

    // Processes any leftover partial frame, then finalizes.
    void end_of_stream(SessionId id);

    [[nodiscard]] const Session::Stats& stats(SessionId id) const;

private:
    struct Entry {
        std::unique_ptr<Session> session;
        FrameAssembler framer;
    };

    [[nodiscard]] Entry& entry_at(SessionId id);
    [[nodiscard]] const Entry& entry_at(SessionId id) const;

    Config config_;
    AsrBackend* backend_;    // non-owning; shared across every session
    InlineJobRunner runner_;  // declared before sessions_: must outlive them
    std::unordered_map<SessionId, Entry> sessions_;
};

}  // namespace sasr
