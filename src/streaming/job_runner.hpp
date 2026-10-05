#pragma once

#include "asr/asr_backend.hpp"
#include "streaming/bounded_queue.hpp"

#include <chrono>
#include <cstddef>
#include <functional>
#include <span>
#include <string>
#include <thread>
#include <vector>

namespace sasr {

struct JobOutcome {
    bool ok = false;
    AsrResult result;   // meaningful only if ok
    std::string error;  // meaningful only if !ok
};

using JobCallback = std::function<void(JobOutcome)>;

// Runs stream.decode(pcm) or stream.finalize(pcm), turning any exception
// into a failed JobOutcome. Every runner goes through this, so they all
// classify failures the same way.
[[nodiscard]] JobOutcome execute_job(AsrStream& stream, std::span<const float> pcm, bool is_final);

// Decides where a Session's ASR work executes. Session never calls the
// backend directly; it hands each decode/finalize to its runner and
// continues when the runner calls back. This seam is what lets one
// state machine (Session) run either synchronously (InlineJobRunner,
// used by Engine and the Phase 4 tests) or on a worker pool
// (ThreadPoolJobRunner, used by ThreadedEngine) without duplicating it.
class JobRunner {
public:
    virtual ~JobRunner() = default;

    JobRunner() = default;
    JobRunner(const JobRunner&) = delete;
    JobRunner& operator=(const JobRunner&) = delete;
    JobRunner(JobRunner&&) = delete;
    JobRunner& operator=(JobRunner&&) = delete;

    // Executes the job and eventually calls on_done exactly once, on the
    // thread that owns the session (each implementation documents when).
    // The caller guarantees that until on_done runs: `stream` stays alive
    // and no other job uses it, and the memory `pcm` views stays alive
    // and is not modified.
    virtual void run(AsrStream& stream, std::span<const float> pcm, bool is_final,
                     JobCallback on_done) = 0;
};

// Runs the job immediately, inside run(), and calls on_done before run()
// returns. No threads; everything happens on the caller's thread.
class InlineJobRunner final : public JobRunner {
public:
    void run(AsrStream& stream, std::span<const float> pcm, bool is_final,
             JobCallback on_done) override {
        on_done(execute_job(stream, pcm, is_final));
    }
};

// Runs jobs on a fixed pool of worker threads. Results are NOT delivered
// on the workers: each completion is queued, and the owning thread runs
// the callbacks itself by calling pump(). That keeps every Session
// mutation on one thread, so Session needs no locks of its own.
//
// Threads and their synchronization:
//   - owning thread: calls run() and pump(); runs every callback.
//   - `workers` worker threads: pop jobs, run the backend call, push
//     completions. Talk to the owning thread only through jobs_ and
//     completions_ (both mutex-protected), whose push/pop pairs also
//     make the job's audio (written by the owning thread before run())
//     visible to the worker.
//
// Backpressure: both queues hold `queue_capacity` items. A session has
// at most one job outstanding (submitted, callback not yet pumped), so
// with at most `queue_capacity` sessions neither queue can fill: run()
// never blocks the owning thread, and workers never block on a full
// completion queue (which would otherwise deadlock against an owning
// thread blocked in run()). ThreadedEngine enforces that session limit.
class ThreadPoolJobRunner final : public JobRunner {
public:
    // workers and queue_capacity must be > 0. on_completion, if set, is
    // called from a worker thread right after it queues a completion: a
    // wake-up hint for the owning thread. It must be cheap, thread-safe,
    // and must not throw.
    ThreadPoolJobRunner(std::size_t workers, std::size_t queue_capacity,
                        std::function<void()> on_completion = {});
    ~ThreadPoolJobRunner() override;  // shutdown()

    // Queues the job for a worker. After shutdown(), calls on_done
    // immediately (on this thread) with a failed outcome instead.
    void run(AsrStream& stream, std::span<const float> pcm, bool is_final,
             JobCallback on_done) override;

    // Owning thread only: runs the callbacks of all completed jobs, in
    // completion order, on the calling thread. Returns how many ran.
    std::size_t pump();

    // Stops accepting jobs, lets the workers finish the ones already
    // queued, and joins them. Completions still queued stay pumpable.
    // Owning thread only; idempotent.
    void shutdown();

private:
    struct Job {
        AsrStream* stream = nullptr;
        std::span<const float> pcm;
        bool is_final = false;
        JobCallback on_done;
        std::chrono::steady_clock::time_point enqueued;
    };
    struct Completion {
        JobCallback on_done;
        JobOutcome outcome;
    };

    void worker_loop();

    BoundedQueue<Job> jobs_;
    BoundedQueue<Completion> completions_;
    std::function<void()> on_completion_;
    std::vector<std::thread> workers_;  // last: started after everything they use exists
    bool shut_down_ = false;            // owning thread only
};

}  // namespace sasr
