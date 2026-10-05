#pragma once

#include "asr/asr_backend.hpp"
#include "common/config.hpp"
#include "streaming/engine.hpp"  // SessionId, EngineError
#include "streaming/frame_assembler.hpp"
#include "streaming/job_runner.hpp"
#include "streaming/session.hpp"
#include "streaming/spsc_ring.hpp"

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <span>
#include <thread>
#include <unordered_map>
#include <vector>

namespace sasr {

// Multi-threaded counterpart of Engine: same Session state machine, same
// events, but ingestion never blocks and ASR runs on worker threads.
//
//   producers --push()--> per-session SpscAudioRing --> engine thread
//   engine thread: drains rings, drives every Session, dispatches jobs
//   worker threads (Config::asr_workers): run the backend calls
//   engine thread: pumps completions, emits events
//
// Threads (CLAUDE.md "when adding a thread"):
//   - Engine thread: started by the constructor, joined by stop(). Sole
//     owner of every Session -- all Session calls and all event
//     callbacks run on it -- so sessions need no locks.
//   - Worker threads: owned by runner_ (see ThreadPoolJobRunner).
//   - Producers: any threads, but for a given session one at a time
//     (each ring is single-producer).
// Synchronization: audio crosses through lock-free SPSC rings (push()
// never blocks); jobs and completions through ThreadPoolJobRunner's
// mutex queues; open/end_of_stream/close through a mutex-protected
// command list. push() looks up a session's ring under a shared (reader)
// lock that only open/close/stop take exclusively.
// Backpressure: when a session can't keep up -- including while both its
// utterance buffers are busy finalizing, when the engine stops draining
// its ring -- the ring fills and push() accepts fewer samples than given.
// A live source drops (and counts) the rest; a file source retries
// (push_blocking()).
// Shutdown: stop() finalizes every open session and waits until each
// FINAL has been delivered before joining any thread.
// Errors: backend exceptions become ERROR events (see Session).
// on_event runs on the engine thread: it must not block, must not
// throw, and must not call back into this ThreadedEngine.
class ThreadedEngine {
public:
    // Starts the engine thread and Config::asr_workers workers. At most
    // Config::job_queue_capacity sessions may be open (or still closing)
    // at once: that bound is what keeps the job queues from ever
    // blocking (see ThreadPoolJobRunner). `backend` must outlive this.
    ThreadedEngine(Config config, AsrBackend& backend);
    ~ThreadedEngine();  // stop()

    ThreadedEngine(const ThreadedEngine&) = delete;
    ThreadedEngine& operator=(const ThreadedEngine&) = delete;
    ThreadedEngine(ThreadedEngine&&) = delete;
    ThreadedEngine& operator=(ThreadedEngine&&) = delete;

    // Thread-safe. Creates the session (and its backend stream) on the
    // calling thread. Throws EngineError if the id is in use (open or
    // still closing), the session limit is reached, or stop() has begun.
    void open_session(SessionId id, SessionEventSink on_event);

    // Producer side; never blocks. Any chunk size: pcm is split into
    // ring-slot-sized pieces, as many as fit are enqueued, and the engine
    // thread re-cuts them into stream-aligned frames (so results don't
    // depend on chunking, and match Engine's). Returns the number of
    // samples accepted. Throws EngineError if the session isn't open or
    // stop() has begun.
    [[nodiscard]] std::size_t push(SessionId id, std::span<const float> pcm);

    // push() until everything is accepted, sleeping
    // Config::engine_idle_poll_ms whenever the ring is full. For file
    // sources and tests, which would rather wait than drop.
    void push_blocking(SessionId id, std::span<const float> pcm);

    // Once all audio already pushed for `id` is processed, finalizes any
    // in-progress utterance. Must not be followed by more push() calls
    // for that id.
    void end_of_stream(SessionId id);

    // Aborts the session (no FINAL; audio still in its ring is dropped)
    // and removes it once its in-flight job, if any, has completed. Its
    // id stays in use until then.
    void close_session(SessionId id);

    // Finalizes every open session, waits until all their FINALs are
    // delivered, then joins the engine and worker threads. Idempotent.
    // Call from one controlling thread (as with the destructor).
    void stop();

    // Only valid after stop() -- the engine thread has exited, so session
    // state can be read without racing it. Throws EngineError otherwise,
    // or if the session isn't known (never opened, or closed).
    [[nodiscard]] Session::Stats stats(SessionId id) const;

private:
    struct Inbox {
        Inbox(std::size_t frames, std::size_t frame_samples) : ring(frames, frame_samples) {}
        SpscAudioRing ring;
        bool closed = false;  // guarded by registry_mutex_
    };

    struct Slot {  // engine thread only
        Slot(std::unique_ptr<Session> s, std::shared_ptr<Inbox> i, std::size_t frame_samples)
            : session(std::move(s)), inbox(std::move(i)), framer(frame_samples) {}

        std::unique_ptr<Session> session;
        std::shared_ptr<Inbox> inbox;
        FrameAssembler framer;  // re-cuts ring chunks into stream-aligned frames
        bool eos_requested = false;
        bool flushed = false;  // stop(): already asked to finalize
        bool closing = false;
    };

    enum class CommandKind { kOpen, kEndOfStream, kClose };
    struct Command {
        CommandKind kind = CommandKind::kOpen;
        SessionId id = 0;
        std::unique_ptr<Session> session;  // kOpen only
        std::shared_ptr<Inbox> inbox;      // kOpen only
    };

    // Requires registry_mutex_ held (shared or exclusive).
    Inbox& open_inbox(SessionId id, const char* caller) const;
    void enqueue(Command command);
    void wake();

    // Engine thread.
    void engine_loop();
    bool drain_commands();
    bool service(Slot& slot);
    void reap_closed();
    [[nodiscard]] bool quiescent();
    void wait_for_work();

    Config config_;
    AsrBackend* backend_;  // non-owning

    std::mutex wake_mutex_;
    std::condition_variable wake_cv_;
    bool wake_pending_ = false;  // guarded by wake_mutex_

    std::mutex command_mutex_;
    std::vector<Command> commands_;  // guarded by command_mutex_

    mutable std::shared_mutex registry_mutex_;
    std::unordered_map<SessionId, std::shared_ptr<Inbox>> registry_;  // open + closing
    bool accepting_ = true;  // guarded by registry_mutex_; false once stop() begins

    // After the wake members: its workers call wake(). Before slots_:
    // destroyed after the sessions whose callbacks it might hold.
    ThreadPoolJobRunner runner_;
    std::unordered_map<SessionId, Slot> slots_;  // engine thread only, until stop() joins it
    std::vector<float> scratch_;                 // engine thread only
    std::atomic<bool> stop_requested_{false};
    bool stopped_ = false;        // controlling thread only
    std::thread engine_thread_;  // last: started once everything it uses exists
};

}  // namespace sasr
