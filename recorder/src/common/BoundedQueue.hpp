#pragma once
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <mutex>
#include <optional>
#include <stop_token>

namespace evidence {
template<class T> class BoundedQueue {
public:
  explicit BoundedQueue(std::size_t capacity) : capacity_(capacity) {}
  bool push_drop_oldest(T value) {
    std::scoped_lock lock(mutex_);
    bool dropped = false;
    if (closed_) return false;
    if (queue_.size() >= capacity_) { queue_.pop_front(); ++dropped_; dropped = true; }
    queue_.push_back(std::move(value));
    cv_.notify_one();
    return !dropped;
  }
  std::optional<T> pop(std::stop_token token) {
    std::unique_lock lock(mutex_);
    cv_.wait(lock, token, [&]{ return closed_ || !queue_.empty(); });
    if (queue_.empty()) return std::nullopt;
    T value = std::move(queue_.front()); queue_.pop_front(); return value;
  }
  void close() { std::scoped_lock lock(mutex_); closed_ = true; cv_.notify_all(); }
  void clear() { std::scoped_lock lock(mutex_); queue_.clear(); }
  std::uint64_t dropped() const { std::scoped_lock lock(mutex_); return dropped_; }
  std::size_t size() const { std::scoped_lock lock(mutex_); return queue_.size(); }
private:
  const std::size_t capacity_;
  mutable std::mutex mutex_;
  std::condition_variable_any cv_;
  std::deque<T> queue_;
  bool closed_{};
  std::uint64_t dropped_{};
};
}
