#include <gtest/gtest.h>

#include <memory>
#include <thread>

#include "core/bounded_queue.hpp"

using namespace vs;
using namespace std::chrono_literals;

TEST(Queue, DropNewestKeepsOldestAndCounts) {
  BoundedQueue<int> q(3, DropPolicy::DropNewest);
  for (int i = 0; i < 5; ++i) q.try_push(i);
  EXPECT_EQ(q.dropped(), 2u);
  EXPECT_EQ(q.pushed(), 5u);
  EXPECT_EQ(*q.try_pop(), 0);
  EXPECT_EQ(*q.try_pop(), 1);
  EXPECT_EQ(*q.try_pop(), 2);
  EXPECT_FALSE(q.try_pop());
}

TEST(Queue, DropOldestKeepsNewest) {
  BoundedQueue<int> q(2, DropPolicy::DropOldest);
  std::optional<int> ev;
  q.try_push(1);
  q.try_push(2);
  EXPECT_FALSE(q.try_push(3, &ev));
  EXPECT_EQ(ev.value(), 1);
  EXPECT_EQ(*q.try_pop(), 2);
  EXPECT_EQ(*q.try_pop(), 3);
}

TEST(Queue, RejectedItemIsHandedBackForRecycling) {
  BoundedQueue<std::unique_ptr<int>> q(1, DropPolicy::DropNewest);
  q.try_push(std::make_unique<int>(1));
  std::optional<std::unique_ptr<int>> back;
  EXPECT_FALSE(q.try_push(std::make_unique<int>(2), &back));
  ASSERT_TRUE(back && *back);
  EXPECT_EQ(**back, 2);
}

TEST(Queue, PopTimesOutAndCloseReleasesWaiter) {
  BoundedQueue<int> q(4, DropPolicy::DropNewest);
  EXPECT_FALSE(q.pop_for(5ms));
  std::thread t([&] {
    std::this_thread::sleep_for(20ms);
    q.close();
  });
  const auto t0 = std::chrono::steady_clock::now();
  EXPECT_FALSE(q.pop_for(5s));
  EXPECT_LT(std::chrono::steady_clock::now() - t0, 2s);
  t.join();
}

TEST(Queue, ProducerConsumerAccountingBalances) {
  BoundedQueue<int> q(8, DropPolicy::DropNewest);
  constexpr int N = 200000;
  uint64_t consumed = 0;
  std::atomic<bool> done{false};
  std::thread c([&] {
    while (!done || q.size() > 0)
      if (q.pop_for(1ms)) ++consumed;
  });
  for (int i = 0; i < N; ++i) q.try_push(i);
  done = true;
  c.join();
  EXPECT_EQ(consumed + q.dropped(), static_cast<uint64_t>(N));
}
