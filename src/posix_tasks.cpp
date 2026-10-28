// Gate per-node workers until all scheduling policies and supervision are
// ready. Complete initial callbacks in dependency order, then allow independent
// periodic execution.
#include <sched.h>

#include <algorithm>
#include <cerrno>
#include <stdexcept>
#include <system_error>
#include <thread>

#include "rtx/posix.hpp"
namespace rtx {
PosixTaskRunner::PosixTaskRunner(Scheduler& scheduler, bool fifo,
                                 Duration resolution)
    : scheduler_(scheduler), fifo_(fifo), resolution_(resolution) {
  if (resolution <= Duration::zero())
    throw std::invalid_argument("resolution must be positive");
}
void PosixTaskRunner::request_stop() noexcept {
  // Pair the predicate update with the wait mutex to prevent a lost
  // notification between a waiter's predicate check and its transition into
  // sleep.
  {
    std::lock_guard lock(wake_mutex_);
    stopping_.store(true);
  }
  wake_.notify_all();
}
void PosixTaskRunner::run() {
  if (started_.exchange(true))
    throw std::logic_error("runner can only be started once");
  // The owner must serialize run and exclude other dispatchers on this
  // Scheduler.
  std::vector<std::jthread> workers;
  std::jthread supervisor;
  std::exception_ptr failure;
  std::size_t configured = 0, launch = 0;
  bool prepared = false;
  auto fail = [&] {
    {
      std::lock_guard lock(wake_mutex_);
      if (!failure) failure = std::current_exception();
    }
    request_stop();
  };
  auto join = [&] {
    request_stop();
    for (auto& worker : workers)
      if (worker.joinable()) worker.join();
    if (supervisor.joinable()) supervisor.join();
    scheduler_.stop();
  };
  scheduler_.start(monotonic_now());
  try {
    int supervisor_priority = 0;
    if (fifo_) {
      const int minimum = sched_get_priority_min(SCHED_FIFO);
      const int maximum = sched_get_priority_max(SCHED_FIFO);
      if (minimum < 0 || maximum < 0)
        throw std::system_error(errno, std::generic_category(),
                                "FIFO priority range");
      for (const auto& entry : scheduler_.entries_) {
        if (entry.node.priority < minimum || entry.node.priority >= maximum)
          throw std::invalid_argument(
              "FIFO node priority must leave room for a higher-priority "
              "supervisor");
        supervisor_priority =
            std::max(supervisor_priority, entry.node.priority);
      }
      supervisor_priority = std::max(minimum, supervisor_priority + 1);
    }
    // Configure supervision before releasing any callback, including initial
    // launch.
    supervisor = std::jthread([&, supervisor_priority] {
      try {
        if (fifo_) set_fifo_priority(supervisor_priority);
        {
          std::unique_lock lock(wake_mutex_);
          ++configured;
          wake_.notify_all();
          wake_.wait(lock, [&] { return stopping_.load() || prepared; });
        }
        while (!stopping_.load()) {
          scheduler_.inspect_watchdogs(monotonic_now());
          std::unique_lock lock(wake_mutex_);
          wake_.wait_for(lock, resolution_, [&] { return stopping_.load(); });
        }
      } catch (...) {
        fail();
      }
    });
    for (std::size_t i = 0; i < scheduler_.entries_.size(); ++i) {
      workers.emplace_back([&, i] {
        try {
          if (fifo_) set_fifo_priority(scheduler_.entries_[i].node.priority);
          {
            std::unique_lock lock(wake_mutex_);
            ++configured;
            wake_.notify_all();
            wake_.wait(lock, [&] {
              return stopping_.load() || (prepared && launch == i);
            });
            if (stopping_.load()) return;
          }
          // Complete first invocations in the validated topological order.
          scheduler_.step_node(i, monotonic_now(), monotonic_now);
          {
            std::lock_guard state_lock(scheduler_.mutex_);
            if (scheduler_.entries_[i].health.state == State::faulted)
              throw std::runtime_error("node failed during launch: " +
                                       scheduler_.entries_[i].node.name);
          }
          {
            std::lock_guard lock(wake_mutex_);
            ++launch;
            wake_.notify_all();
          }
          while (!stopping_.load()) {
            std::unique_lock lock(wake_mutex_);
            wake_.wait_for(lock, resolution_, [&] { return stopping_.load(); });
            lock.unlock();
            if (!stopping_.load())
              scheduler_.step_node(i, monotonic_now(), monotonic_now);
          }
        } catch (...) {
          fail();
        }
      });
    }
    {
      std::unique_lock lock(wake_mutex_);
      wake_.wait(lock, [&] {
        return stopping_.load() || configured == workers.size() + 1;
      });
      // No application callbacks execute until all workers have their requested
      // policy.
      prepared = true;
      wake_.notify_all();
    }
    {
      std::unique_lock lock(wake_mutex_);
      wake_.wait(lock, [&] { return stopping_.load(); });
    }
  } catch (...) {
    fail();
  }
  join();
  if (failure) std::rethrow_exception(failure);
}
}  // namespace rtx
