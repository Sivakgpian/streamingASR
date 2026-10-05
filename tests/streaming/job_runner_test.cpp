#include "streaming/job_runner.hpp"

#include "asr/mock_backend.hpp"

#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <thread>
#include <vector>

namespace sasr {
namespace {

using namespace std::chrono_literals;

class ThrowingStream final : public AsrStream {
public:
    AsrResult decode(std::span<const float>) override { throw std::runtime_error("boom"); }
    AsrResult finalize(std::span<const float>) override { throw std::runtime_error("boom"); }
    void reset() override {}
};

const std::array<float, 100> kAudio{};

// Pumps until `done` callbacks have run, or gives up after a timeout.
// Returns the number of callbacks that ran.
std::size_t pump_until(ThreadPoolJobRunner& runner, std::size_t done,
                       std::chrono::milliseconds timeout = 10s) {
    std::size_t ran = 0;
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (ran < done && std::chrono::steady_clock::now() < deadline) {
        ran += runner.pump();
        std::this_thread::sleep_for(1ms);
    }
    return ran;
}

TEST(ExecuteJob, ExceptionBecomesAFailedOutcome) {
    ThrowingStream stream;
    const JobOutcome outcome = execute_job(stream, kAudio, /*is_final=*/false);
    EXPECT_FALSE(outcome.ok);
    EXPECT_EQ(outcome.error, "boom");
}

TEST(InlineJobRunner, CallsBackBeforeRunReturns) {
    MockBackend backend;
    auto stream = backend.create_stream();
    InlineJobRunner runner;

    std::optional<JobOutcome> got;
    runner.run(*stream, kAudio, /*is_final=*/true, [&](JobOutcome o) { got = std::move(o); });
    ASSERT_TRUE(got.has_value());
    EXPECT_TRUE(got->ok);
    EXPECT_TRUE(got->result.is_final);
    EXPECT_EQ(got->result.text, "<mock:100>");
}

TEST(ThreadPoolJobRunner, RejectsZeroWorkers) {
    EXPECT_THROW(ThreadPoolJobRunner(0, 4), std::invalid_argument);
}

TEST(ThreadPoolJobRunner, CallbackRunsOnlyInPumpOnTheCallingThread) {
    MockBackend backend;
    auto stream = backend.create_stream();

    std::mutex mutex;
    std::condition_variable cv;
    bool completed = false;
    ThreadPoolJobRunner runner(1, 4, [&] {
        {
            const std::lock_guard lock(mutex);
            completed = true;
        }
        cv.notify_one();
    });

    std::optional<JobOutcome> got;
    std::thread::id callback_thread;
    runner.run(*stream, kAudio, false, [&](JobOutcome o) {
        got = std::move(o);
        callback_thread = std::this_thread::get_id();
    });

    {
        std::unique_lock lock(mutex);
        ASSERT_TRUE(cv.wait_for(lock, 10s, [&] { return completed; }));
    }
    EXPECT_FALSE(got.has_value());  // finished on a worker, but not delivered yet

    EXPECT_EQ(runner.pump(), 1U);
    ASSERT_TRUE(got.has_value());
    EXPECT_TRUE(got->ok);
    EXPECT_EQ(got->result.text, "<mock:100>");
    EXPECT_EQ(callback_thread, std::this_thread::get_id());
}

TEST(ThreadPoolJobRunner, ReportsQueueTimeOfAJobThatWaited) {
    MockBackend backend(20ms);  // each job takes >= 20 ms
    auto first = backend.create_stream();
    auto second = backend.create_stream();
    ThreadPoolJobRunner runner(1, 4);  // one worker: the second job must wait

    std::vector<JobOutcome> outcomes;
    runner.run(*first, kAudio, false, [&](JobOutcome o) { outcomes.push_back(std::move(o)); });
    runner.run(*second, kAudio, false, [&](JobOutcome o) { outcomes.push_back(std::move(o)); });
    ASSERT_EQ(pump_until(runner, 2), 2U);

    ASSERT_EQ(outcomes.size(), 2U);
    EXPECT_GE(outcomes[1].result.timings.queue_us, 15000);  // waited behind the first
    EXPECT_GE(outcomes[1].result.timings.infer_us, 20000);
}

TEST(ThreadPoolJobRunner, ManyJobsAcrossWorkersAllComplete) {
    constexpr std::size_t kJobs = 50;
    MockBackend backend(1ms);
    std::vector<std::unique_ptr<AsrStream>> streams;
    for (std::size_t i = 0; i < kJobs; ++i) {
        streams.push_back(backend.create_stream());  // one job per stream, as Session guarantees
    }
    ThreadPoolJobRunner runner(4, kJobs);

    std::size_t ok = 0;
    for (auto& stream : streams) {
        runner.run(*stream, kAudio, false, [&](JobOutcome o) { ok += o.ok ? 1 : 0; });
    }
    EXPECT_EQ(pump_until(runner, kJobs), kJobs);
    EXPECT_EQ(ok, kJobs);
}

TEST(ThreadPoolJobRunner, ExceptionOnWorkerBecomesAFailedOutcome) {
    ThrowingStream stream;
    ThreadPoolJobRunner runner(1, 4);
    std::optional<JobOutcome> got;
    runner.run(stream, kAudio, false, [&](JobOutcome o) { got = std::move(o); });
    ASSERT_EQ(pump_until(runner, 1), 1U);
    ASSERT_TRUE(got.has_value());
    EXPECT_FALSE(got->ok);
    EXPECT_EQ(got->error, "boom");
}

TEST(ThreadPoolJobRunner, ShutdownFinishesQueuedJobsWhichStayPumpable) {
    MockBackend backend(2ms);
    auto a = backend.create_stream();
    auto b = backend.create_stream();
    ThreadPoolJobRunner runner(1, 4);

    std::size_t delivered = 0;
    runner.run(*a, kAudio, false, [&](JobOutcome) { ++delivered; });
    runner.run(*b, kAudio, false, [&](JobOutcome) { ++delivered; });
    runner.shutdown();  // joins only after both ran

    EXPECT_EQ(runner.pump(), 2U);
    EXPECT_EQ(delivered, 2U);
}

TEST(ThreadPoolJobRunner, RunAfterShutdownFailsImmediately) {
    MockBackend backend;
    auto stream = backend.create_stream();
    ThreadPoolJobRunner runner(1, 4);
    runner.shutdown();

    std::optional<JobOutcome> got;
    runner.run(*stream, kAudio, false, [&](JobOutcome o) { got = std::move(o); });
    ASSERT_TRUE(got.has_value());  // synchronously, exactly once
    EXPECT_FALSE(got->ok);
}

}  // namespace
}  // namespace sasr
