#include "streaming/threaded_engine.hpp"

#include <algorithm>
#include <chrono>
#include <string>
#include <utility>

namespace sasr {

ThreadedEngine::ThreadedEngine(Config config, AsrBackend& backend)
    : config_(std::move(config)),
      backend_(&backend),
      runner_(config_.asr_workers, config_.job_queue_capacity, [this] { wake(); }),
      engine_thread_([this] { engine_loop(); }) {}

ThreadedEngine::~ThreadedEngine() { stop(); }

// ---- API threads ------------------------------------------------------

void ThreadedEngine::open_session(SessionId id, SessionEventSink on_event) {
    // Built here, not on the engine thread: creating a backend stream can
    // be slow (whisper allocates its per-state buffers), and the engine
    // thread must keep serving the other sessions meanwhile.
    auto inbox = std::make_shared<Inbox>(config_.ring_capacity_frames, config_.frame_samples());
    auto session = std::make_unique<Session>(config_, *backend_, runner_, std::move(on_event));
    {
        const std::unique_lock lock(registry_mutex_);
        if (!accepting_) {
            throw EngineError("ThreadedEngine::open_session: engine is stopping");
        }
        if (registry_.contains(id)) {
            throw EngineError("ThreadedEngine::open_session: session id in use (open or closing)");
        }
        if (registry_.size() >= config_.job_queue_capacity) {
            throw EngineError(
                "ThreadedEngine::open_session: session limit (Config::job_queue_capacity) reached");
        }
        registry_.emplace(id, inbox);
    }
    Command command;
    command.kind = CommandKind::kOpen;
    command.id = id;
    command.session = std::move(session);
    command.inbox = std::move(inbox);
    enqueue(std::move(command));
}

std::size_t ThreadedEngine::push(SessionId id, std::span<const float> pcm) {
    const std::shared_lock lock(registry_mutex_);
    SpscAudioRing& ring = open_inbox(id, "push").ring;
    const std::size_t frame = config_.frame_samples();
    std::size_t accepted = 0;
    while (accepted < pcm.size()) {
        const std::size_t n = std::min(frame, pcm.size() - accepted);
        if (!ring.try_push(pcm.subspan(accepted, n))) {
            break;  // ring full: the caller decides whether to drop or retry
        }
        accepted += n;
    }
    return accepted;
}

void ThreadedEngine::push_blocking(SessionId id, std::span<const float> pcm) {
    while (!pcm.empty()) {
        pcm = pcm.subspan(push(id, pcm));
        if (!pcm.empty()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(config_.engine_idle_poll_ms));
        }
    }
}

void ThreadedEngine::end_of_stream(SessionId id) {
    {
        const std::shared_lock lock(registry_mutex_);
        (void)open_inbox(id, "end_of_stream");
    }
    Command command;
    command.kind = CommandKind::kEndOfStream;
    command.id = id;
    enqueue(std::move(command));
}

void ThreadedEngine::close_session(SessionId id) {
    {
        const std::unique_lock lock(registry_mutex_);
        open_inbox(id, "close_session").closed = true;  // push() refuses it from now on
    }
    Command command;
    command.kind = CommandKind::kClose;
    command.id = id;
    enqueue(std::move(command));
}

void ThreadedEngine::stop() {
    if (stopped_) {
        return;
    }
    {
        const std::unique_lock lock(registry_mutex_);
        accepting_ = false;  // every API call but stats() now throws
    }
    stop_requested_.store(true, std::memory_order_release);
    wake();
    engine_thread_.join();  // returns once every FINAL is delivered (see engine_loop)
    runner_.shutdown();     // nothing outstanding by now; just joins the workers
    stopped_ = true;
}

Session::Stats ThreadedEngine::stats(SessionId id) const {
    if (!stopped_) {
        throw EngineError("ThreadedEngine::stats: only valid after stop()");
    }
    const auto it = slots_.find(id);
    if (it == slots_.end()) {
        throw EngineError("ThreadedEngine::stats: no such session");
    }
    return it->second.session->stats();
}

ThreadedEngine::Inbox& ThreadedEngine::open_inbox(SessionId id, const char* caller) const {
    if (!accepting_) {
        throw EngineError(std::string("ThreadedEngine::") + caller + ": engine is stopping");
    }
    const auto it = registry_.find(id);
    if (it == registry_.end() || it->second->closed) {
        throw EngineError(std::string("ThreadedEngine::") + caller + ": no such open session");
    }
    return *it->second;
}

void ThreadedEngine::enqueue(Command command) {
    {
        const std::lock_guard lock(command_mutex_);
        commands_.push_back(std::move(command));
    }
    wake();
}

void ThreadedEngine::wake() {
    {
        const std::lock_guard lock(wake_mutex_);
        wake_pending_ = true;
    }
    wake_cv_.notify_one();
}

// ---- Engine thread ------------------------------------------------------

void ThreadedEngine::engine_loop() {
    scratch_.reserve(config_.frame_samples());
    bool flushing = false;
    while (true) {
        bool did_work = drain_commands();
        did_work = (runner_.pump() > 0) || did_work;

        if (stop_requested_.load(std::memory_order_acquire)) {
            flushing = true;
        }
        if (flushing) {
            // Includes sessions whose open command arrived after stop()
            // began: they were opened before it, so they get flushed too.
            for (auto& [id, slot] : slots_) {
                if (!slot.closing && !slot.flushed) {
                    slot.flushed = true;
                    slot.eos_requested = true;
                    did_work = true;
                }
            }
        }

        for (auto& [id, slot] : slots_) {
            did_work = service(slot) || did_work;
        }
        reap_closed();

        if (flushing && quiescent()) {
            return;
        }
        if (!did_work) {
            wait_for_work();
        }
    }
}

bool ThreadedEngine::drain_commands() {
    std::vector<Command> commands;
    {
        const std::lock_guard lock(command_mutex_);
        commands.swap(commands_);
    }
    for (Command& command : commands) {
        switch (command.kind) {
            case CommandKind::kOpen:
                slots_.try_emplace(command.id, std::move(command.session),
                                   std::move(command.inbox), config_.frame_samples());
                break;
            case CommandKind::kEndOfStream:
                if (const auto it = slots_.find(command.id); it != slots_.end()) {
                    it->second.eos_requested = true;
                }
                break;
            case CommandKind::kClose:
                if (const auto it = slots_.find(command.id); it != slots_.end()) {
                    it->second.closing = true;
                    it->second.eos_requested = false;
                    it->second.framer.clear();
                    it->second.session->reset();  // abandon; an in-flight result is discarded
                }
                break;
        }
    }
    return !commands.empty();
}

bool ThreadedEngine::service(Slot& slot) {
    if (slot.closing) {
        return false;
    }
    bool did_work = false;
    SpscAudioRing& ring = slot.inbox->ring;
    Session& session = *slot.session;
    const auto to_session = [&session](std::span<const float> frame) { session.push(frame); };
    // At most one ring's worth per pass, so one fast producer can't
    // starve the other sessions. Stops early while the session is
    // stalled (both buffers finalizing): the audio waits in the ring,
    // and the producer feels it as backpressure. Checking before every
    // pop is enough: a ring chunk is at most one frame long, so it
    // completes at most one frame.
    for (std::size_t budget = ring.capacity(); budget > 0 && session.ready_for_audio(); --budget) {
        if (!ring.try_pop(scratch_)) {
            break;
        }
        slot.framer.feed(scratch_, to_session);
        did_work = true;
    }
    // end_of_stream() applies only after every sample pushed before it,
    // including a trailing partial frame.
    if (slot.eos_requested && session.ready_for_audio() && ring.consumer_empty()) {
        slot.eos_requested = false;
        slot.framer.flush(to_session);
        session.end_of_stream();
        did_work = true;
    }
    return did_work;
}

void ThreadedEngine::reap_closed() {
    for (auto it = slots_.begin(); it != slots_.end();) {
        if (it->second.closing && !it->second.session->has_pending_work()) {
            {
                const std::unique_lock lock(registry_mutex_);
                registry_.erase(it->first);  // the id becomes reusable only now
            }
            it = slots_.erase(it);
        } else {
            ++it;
        }
    }
}

bool ThreadedEngine::quiescent() {
    {
        const std::lock_guard lock(command_mutex_);
        if (!commands_.empty()) {
            return false;
        }
    }
    return std::ranges::none_of(slots_, [](const auto& entry) {
        const Slot& slot = entry.second;
        return slot.closing || slot.eos_requested || slot.session->has_pending_work();
    });
}

void ThreadedEngine::wait_for_work() {
    std::unique_lock lock(wake_mutex_);
    wake_cv_.wait_for(lock, std::chrono::milliseconds(config_.engine_idle_poll_ms),
                      [this] { return wake_pending_; });
    wake_pending_ = false;
}

}  // namespace sasr
