#include "streaming/engine.hpp"

#include <utility>

namespace sasr {

void Engine::open_session(SessionId id, SessionEventSink on_event) {
    const auto [it, inserted] = sessions_.try_emplace(
        id, std::make_unique<Session>(config_, *backend_, std::move(on_event)));
    if (!inserted) {
        throw EngineError("Engine::open_session: session already open");
    }
}

void Engine::close_session(SessionId id) {
    const auto it = sessions_.find(id);
    if (it == sessions_.end()) {
        throw EngineError("Engine::close_session: no such session");
    }
    it->second->reset();  // abort any in-progress utterance; no FINAL
    sessions_.erase(it);
}

bool Engine::has_session(SessionId id) const noexcept { return sessions_.contains(id); }

void Engine::push(SessionId id, std::span<const float> pcm) { session_at(id).push(pcm); }

void Engine::end_of_stream(SessionId id) { session_at(id).end_of_stream(); }

const Session::Stats& Engine::stats(SessionId id) const { return session_at(id).stats(); }

Session& Engine::session_at(SessionId id) {
    const auto it = sessions_.find(id);
    if (it == sessions_.end()) {
        throw EngineError("Engine: no such session");
    }
    return *it->second;
}

const Session& Engine::session_at(SessionId id) const {
    const auto it = sessions_.find(id);
    if (it == sessions_.end()) {
        throw EngineError("Engine: no such session");
    }
    return *it->second;
}

}  // namespace sasr
