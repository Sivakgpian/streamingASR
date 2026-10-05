#pragma once

#include "asr/asr_backend.hpp"
#include "common/config.hpp"
#include "streaming/session.hpp"

#include <cstdint>
#include <memory>
#include <span>
#include <stdexcept>
#include <unordered_map>

namespace sasr {

using SessionId = std::uint64_t;

class EngineError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// Owns a set of concurrent Sessions sharing one AsrBackend. Single-
// threaded and synchronous (see Session); a threaded version with its
// own capture/worker threads is added later without changing this API.
class Engine {
public:
    Engine(Config config, AsrBackend& backend) : config_(std::move(config)), backend_(&backend) {}

    // Opens a new session. Throws EngineError if id is already open.
    void open_session(SessionId id, SessionEventSink on_event);

    // Closes a session (aborts any in-progress utterance, no FINAL).
    // Throws EngineError if id is not open.
    void close_session(SessionId id);

    [[nodiscard]] bool has_session(SessionId id) const noexcept;

    // Throws EngineError if id is not open.
    void push(SessionId id, std::span<const float> pcm);
    void end_of_stream(SessionId id);

    [[nodiscard]] const Session::Stats& stats(SessionId id) const;

private:
    [[nodiscard]] Session& session_at(SessionId id);
    [[nodiscard]] const Session& session_at(SessionId id) const;

    Config config_;
    AsrBackend* backend_;  // non-owning; shared across every session
    std::unordered_map<SessionId, std::unique_ptr<Session>> sessions_;
};

}  // namespace sasr
