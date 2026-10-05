#include "streaming/engine.hpp"

#include <utility>

namespace sasr {

void Engine::open_session(SessionId id, SessionEventSink on_event) {
    if (sessions_.contains(id)) {  // checked first: don't build a backend stream just to discard it
        throw EngineError("Engine::open_session: session already open");
    }
    Entry entry{std::make_unique<Session>(config_, *backend_, runner_, std::move(on_event)),
                FrameAssembler(config_.frame_samples())};
    sessions_.emplace(id, std::move(entry));
}

void Engine::close_session(SessionId id) {
    const auto it = sessions_.find(id);
    if (it == sessions_.end()) {
        throw EngineError("Engine::close_session: no such session");
    }
    it->second.session->reset();  // abort any in-progress utterance; no FINAL
    sessions_.erase(it);
}

bool Engine::has_session(SessionId id) const noexcept { return sessions_.contains(id); }

void Engine::push(SessionId id, std::span<const float> pcm) {
    Entry& entry = entry_at(id);
    entry.framer.feed(pcm, [&entry](std::span<const float> frame) { entry.session->push(frame); });
}

void Engine::end_of_stream(SessionId id) {
    Entry& entry = entry_at(id);
    entry.framer.flush([&entry](std::span<const float> frame) { entry.session->push(frame); });
    entry.session->end_of_stream();
}

const Session::Stats& Engine::stats(SessionId id) const { return entry_at(id).session->stats(); }

Engine::Entry& Engine::entry_at(SessionId id) {
    const auto it = sessions_.find(id);
    if (it == sessions_.end()) {
        throw EngineError("Engine: no such session");
    }
    return it->second;
}

const Engine::Entry& Engine::entry_at(SessionId id) const {
    const auto it = sessions_.find(id);
    if (it == sessions_.end()) {
        throw EngineError("Engine: no such session");
    }
    return it->second;
}

}  // namespace sasr
