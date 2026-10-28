// Internal Linux wait primitive for priority-inheriting launch and shutdown
// gates. Use absolute monotonic deadlines so wall-clock adjustments cannot
// alter the wait interval.
#pragma once
#include <pthread.h>

#include "rtx/scheduler.hpp"
namespace rtx::detail {
// Native monotonic waits paired with RuntimeMutex's Linux PI protocol. Kept
// separate from std::condition_variable, which accepts only std::mutex.
class PosixConditionVariable {
 public:
  PosixConditionVariable();
  ~PosixConditionVariable();
  PosixConditionVariable(const PosixConditionVariable&) = delete;
  PosixConditionVariable& operator=(const PosixConditionVariable&) = delete;
  void notify_all() noexcept;
  template <class Predicate>
  void wait(std::unique_lock<RuntimeMutex>& lock, Predicate predicate) {
    while (!predicate()) wait_once(lock);
  }
  template <class Predicate>
  bool wait_for(std::unique_lock<RuntimeMutex>& lock, Duration interval,
                Predicate predicate) {
    if (predicate()) return true;
    if (interval <= Duration::zero()) return false;
    const auto deadline = deadline_after(interval);
    while (!predicate())
      if (!wait_until(lock, deadline)) return predicate();
    return true;
  }

 private:
  static Time deadline_after(Duration interval);
  void wait_once(std::unique_lock<RuntimeMutex>& lock);
  bool wait_until(std::unique_lock<RuntimeMutex>& lock, Time deadline);
  pthread_cond_t handle_;
};
}  // namespace rtx::detail
