#pragma once
// Bounded MPMC queue (mutex + condvar) with explicit drop accounting.
// The producer never blocks: try_push() either enqueues or counts a drop.
// Blocking the acquisition thread would back-pressure into the GVSP socket,
// where the loss is silent; dropping here is counted.
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>

namespace vs {

enum class DropPolicy { DropNewest, DropOldest };

template <typename T>
class BoundedQueue {
 public:
  BoundedQueue(size_t capacity, DropPolicy policy) : cap_(capacity), policy_(policy) {}

  // Returns false if the item was dropped (DropNewest) or another item was
  // evicted (DropOldest). Either way exactly one drop is counted. An evicted
  // item is handed back through `evicted` so its storage can be recycled.
  bool try_push(T item, std::optional<T>* evicted = nullptr) {
    {
      std::lock_guard lk(m_);
      ++pushed_;
      if (q_.size() >= cap_) {
        ++dropped_;
        if (policy_ == DropPolicy::DropNewest) {
          if (evicted) *evicted = std::move(item);
          return false;
        }
        if (evicted) *evicted = std::move(q_.front());
        q_.pop_front();
        q_.push_back(std::move(item));
        if (q_.size() > high_water_) high_water_ = q_.size();
        cv_.notify_one();
        return false;
      }
      q_.push_back(std::move(item));
      if (q_.size() > high_water_) high_water_ = q_.size();
    }
    cv_.notify_one();
    return true;
  }

  template <class Rep, class Period>
  std::optional<T> pop_for(std::chrono::duration<Rep, Period> timeout) {
    std::unique_lock lk(m_);
    if (!cv_.wait_for(lk, timeout, [&] { return !q_.empty() || closed_; })) return std::nullopt;
    if (q_.empty()) return std::nullopt;
    T v = std::move(q_.front());
    q_.pop_front();
    return v;
  }

  std::optional<T> try_pop() {
    std::lock_guard lk(m_);
    if (q_.empty()) return std::nullopt;
    T v = std::move(q_.front());
    q_.pop_front();
    return v;
  }

  void close() {
    {
      std::lock_guard lk(m_);
      closed_ = true;
    }
    cv_.notify_all();
  }

  void reopen() {
    std::lock_guard lk(m_);
    closed_ = false;
  }

  size_t size() const { std::lock_guard lk(m_); return q_.size(); }
  uint64_t pushed() const { std::lock_guard lk(m_); return pushed_; }
  uint64_t dropped() const { std::lock_guard lk(m_); return dropped_; }
  size_t high_water() const { std::lock_guard lk(m_); return high_water_; }
  size_t capacity() const { return cap_; }

 private:
  mutable std::mutex m_;
  std::condition_variable cv_;
  std::deque<T> q_;
  size_t cap_;
  DropPolicy policy_;
  bool closed_ = false;
  uint64_t pushed_ = 0, dropped_ = 0;
  size_t high_water_ = 0;
};

}  // namespace vs
