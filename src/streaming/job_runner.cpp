#include "streaming/job_runner.hpp"

#include <exception>
#include <optional>
#include <stdexcept>
#include <utility>

namespace sasr {

JobOutcome execute_job(AsrStream& stream, std::span<const float> pcm, bool is_final) {
    JobOutcome outcome;
    try {
        outcome.result = is_final ? stream.finalize(pcm) : stream.decode(pcm);
        outcome.ok = true;
    } catch (const std::exception& e) {
        outcome.error = e.what();
    } catch (...) {
        outcome.error = "unknown exception from ASR backend";
    }
    return outcome;
}

ThreadPoolJobRunner::ThreadPoolJobRunner(std::size_t workers, std::size_t queue_capacity,
                                         std::function<void()> on_completion)
    : jobs_(queue_capacity), completions_(queue_capacity), on_completion_(std::move(on_completion)) {
    if (workers == 0) {
        throw std::invalid_argument("ThreadPoolJobRunner: workers must be > 0");
    }
    workers_.reserve(workers);
    for (std::size_t i = 0; i < workers; ++i) {
        workers_.emplace_back([this] { worker_loop(); });
    }
}

ThreadPoolJobRunner::~ThreadPoolJobRunner() { shutdown(); }

void ThreadPoolJobRunner::run(AsrStream& stream, std::span<const float> pcm, bool is_final,
                              JobCallback on_done) {
    // Only shutdown() closes jobs_, and it runs on this same (owning)
    // thread, so this flag is exact: if it's false, push() cannot be
    // refused.
    if (shut_down_) {
        JobOutcome refused;
        refused.error = "ThreadPoolJobRunner: shut down";
        on_done(std::move(refused));
        return;
    }
    Job job;
    job.stream = &stream;
    job.pcm = pcm;
    job.is_final = is_final;
    job.on_done = std::move(on_done);
    job.enqueued = std::chrono::steady_clock::now();
    jobs_.push(std::move(job));  // never blocks under the session-limit invariant (see header)
}

std::size_t ThreadPoolJobRunner::pump() {
    std::size_t ran = 0;
    while (auto completion = completions_.try_pop()) {
        completion->on_done(std::move(completion->outcome));
        ++ran;
    }
    return ran;
}

void ThreadPoolJobRunner::shutdown() {
    if (shut_down_) {
        return;
    }
    shut_down_ = true;
    jobs_.close();  // workers finish what's queued, then pop() returns nullopt
    for (std::thread& worker : workers_) {
        worker.join();
    }
}

void ThreadPoolJobRunner::worker_loop() {
    while (std::optional<Job> job = jobs_.pop()) {
        const auto started = std::chrono::steady_clock::now();
        JobOutcome outcome = execute_job(*job->stream, job->pcm, job->is_final);
        if (outcome.ok) {
            outcome.result.timings.queue_us =
                std::chrono::duration_cast<std::chrono::microseconds>(started - job->enqueued)
                    .count();
        }
        // completions_ is never closed and, by the session-limit
        // invariant (see header), never full: this does not block.
        completions_.push(Completion{std::move(job->on_done), std::move(outcome)});
        if (on_completion_) {
            on_completion_();
        }
    }
}

}  // namespace sasr
