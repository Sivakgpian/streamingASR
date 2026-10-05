#include "streaming/bounded_queue.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <thread>
#include <vector>

namespace sasr {
namespace {

TEST(BoundedQueue, RejectsZeroCapacity) {
    EXPECT_THROW(BoundedQueue<int>(0), std::invalid_argument);
}

TEST(BoundedQueue, IsFifo) {
    BoundedQueue<int> queue(4);
    ASSERT_TRUE(queue.push(1));
    ASSERT_TRUE(queue.push(2));
    ASSERT_TRUE(queue.push(3));
    EXPECT_EQ(queue.pop(), 1);
    EXPECT_EQ(queue.pop(), 2);
    EXPECT_EQ(queue.pop(), 3);
}

TEST(BoundedQueue, TryPopOnEmptyReturnsNullopt) {
    BoundedQueue<int> queue(1);
    EXPECT_EQ(queue.try_pop(), std::nullopt);
}

TEST(BoundedQueue, CloseRefusesPushesButDrainsWhatIsLeft) {
    BoundedQueue<int> queue(4);
    ASSERT_TRUE(queue.push(1));
    ASSERT_TRUE(queue.push(2));
    queue.close();

    EXPECT_FALSE(queue.push(3));
    EXPECT_EQ(queue.pop(), 1);
    EXPECT_EQ(queue.pop(), 2);
    EXPECT_EQ(queue.pop(), std::nullopt);  // closed and drained: does not block
}

TEST(BoundedQueue, CloseWakesABlockedConsumer) {
    BoundedQueue<int> queue(1);
    std::optional<int> got = 42;
    std::thread consumer([&] { got = queue.pop(); });  // blocks: empty and open
    queue.close();
    consumer.join();
    EXPECT_EQ(got, std::nullopt);
}

TEST(BoundedQueue, PushBlocksWhileFullUntilAPop) {
    BoundedQueue<int> queue(1);
    ASSERT_TRUE(queue.push(1));
    std::atomic<bool> pushed{false};
    std::thread producer([&] {
        ASSERT_TRUE(queue.push(2));  // blocks until the pop below
        pushed = true;
    });
    EXPECT_EQ(queue.pop(), 1);
    producer.join();
    EXPECT_TRUE(pushed);
    EXPECT_EQ(queue.pop(), 2);
}

TEST(BoundedQueue, ManyProducersManyConsumersLoseNothing) {
    constexpr int kProducers = 4;
    constexpr int kItemsEach = 2500;
    constexpr int kConsumers = 3;
    BoundedQueue<int> queue(8);

    std::atomic<std::int64_t> sum{0};
    std::atomic<int> count{0};
    std::vector<std::thread> consumers;
    for (int c = 0; c < kConsumers; ++c) {
        consumers.emplace_back([&] {
            while (const std::optional<int> item = queue.pop()) {
                sum += *item;
                ++count;
            }
        });
    }
    std::vector<std::thread> producers;
    for (int p = 0; p < kProducers; ++p) {
        producers.emplace_back([&queue] {
            for (int i = 1; i <= kItemsEach; ++i) {
                ASSERT_TRUE(queue.push(i));
            }
        });
    }
    for (std::thread& t : producers) {
        t.join();
    }
    queue.close();
    for (std::thread& t : consumers) {
        t.join();
    }

    EXPECT_EQ(count, kProducers * kItemsEach);
    EXPECT_EQ(sum, static_cast<std::int64_t>(kProducers) * kItemsEach * (kItemsEach + 1) / 2);
}

}  // namespace
}  // namespace sasr
