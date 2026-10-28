// Verify real single-CPU FIFO preemption, supervisor progress and mutex
// priority inheritance. Return the designated skip code only when the initial
// privilege probe is denied.
#include <pthread.h>
#include <sched.h>

#include <atomic>
#include <cerrno>
#include <iostream>
#include <semaphore>
#include <system_error>
#include <thread>

#include "rtx/posix.hpp"
using namespace std::chrono_literals;
int main() {
  try {
    std::exception_ptr probe_failure;
    std::jthread probe([&] {
      try {
        rtx::set_fifo_priority(1);
      } catch (...) {
        probe_failure = std::current_exception();
      }
    });
    probe.join();
    if (probe_failure) {
      try {
        std::rethrow_exception(probe_failure);
      } catch (const std::system_error& ex) {
        if (ex.code().value() == EPERM || ex.code().value() == EACCES) {
          std::cout << "SKIP: SCHED_FIFO permission unavailable\n";
          return 77;
        }
        throw;
      }
    }
    cpu_set_t available;
    CPU_ZERO(&available);
    if (sched_getaffinity(0, sizeof(available), &available) != 0)
      throw std::system_error(errno, std::generic_category(), "get affinity");
    int cpu = -1;
    for (int i = 0; i < CPU_SETSIZE; ++i)
      if (CPU_ISSET(i, &available)) {
        cpu = i;
        break;
      }
    if (cpu < 0) throw std::runtime_error("no allowed CPU");
    cpu_set_t selected;
    CPU_ZERO(&selected);
    CPU_SET(cpu, &selected);
    int result =
        pthread_setaffinity_np(pthread_self(), sizeof(selected), &selected);
    if (result)
      throw std::system_error(result, std::generic_category(), "set affinity");
    std::atomic<bool> busy = false, observed = false, correct = true;
    rtx::PosixTaskRunner* runner = nullptr;
    auto verify = [&](int expected) {
      int policy = 0;
      sched_param parameter{};
      if (pthread_getschedparam(pthread_self(), &policy, &parameter) != 0 ||
          policy != SCHED_FIFO || parameter.sched_priority != expected ||
          sched_getcpu() != cpu)
        correct = false;
    };
    rtx::Scheduler scheduler(
        {{"low",
          10,
          1ms,
          100ms,
          0ns,
          {},
          [&](const rtx::Context& c) {
            verify(10);
            c.heartbeat();
            if (c.tick == 2) {
              busy = true;
              const auto limit = rtx::monotonic_now() + 30ms;
              // No mutex, sleep or yield in this interval. Both workers are on
              // one CPU.
              while (!observed.load() && rtx::monotonic_now() < limit) {
              }
              busy = false;
              runner->request_stop();
            }
          }},
         {"high", 20, 5ms, 100ms, 0ns, {"low"}, [&](const rtx::Context& c) {
            verify(20);
            c.heartbeat();
            if (busy.load()) observed = true;
            if (c.tick > 20) runner->request_stop();
          }}});
    rtx::PosixTaskRunner execution(scheduler, true, 1ms);
    runner = &execution;
    execution.run();
    if (!observed.load() || !correct.load())
      throw std::runtime_error(
          "single-CPU FIFO preemption failed: observed=" +
          std::to_string(observed.load()) +
          ", correct=" + std::to_string(correct.load()) +
          ", low ticks=" + std::to_string(scheduler.health()[0].ticks) +
          ", high ticks=" + std::to_string(scheduler.health()[1].ticks));
    // Supervision must run during even the first CPU-bound launch callback.
    // The health snapshot is taken before the FIFO worker yields or returns.
    bool timeout_during_callback = false;
    rtx::Scheduler* watched = nullptr;
    rtx::PosixTaskRunner* monitored_runner = nullptr;
    rtx::Scheduler watchdog(
        {{"busy", 10, 1s, 1s, 3ms, {}, [&](const rtx::Context& c) {
            verify(10);
            c.heartbeat();
            const auto limit = rtx::monotonic_now() + 40ms;
            while (rtx::monotonic_now() < limit) {
            }
            timeout_during_callback =
                watched->health()[0].state == rtx::State::faulted;
            monitored_runner->request_stop();
          }}});
    watched = &watchdog;
    rtx::PosixTaskRunner monitored(watchdog, true, 1ms);
    monitored_runner = &monitored;
    bool launch_failed = false;
    try {
      monitored.run();
    } catch (const std::runtime_error&) {
      launch_failed = true;
    }
    const auto health = watchdog.health()[0];
    if (!launch_failed || !timeout_during_callback || !correct.load() ||
        health.ticks != 1 || health.fault != "heartbeat timeout")
      throw std::runtime_error(
          "FIFO supervisor failed to inspect CPU-bound launch callback");
    // A medium FIFO spinner must not keep the low state-lock owner from
    // releasing a lock needed by a high-priority waiter on the same CPU.
    auto lock_owner_progress = [](auto& inherited) {
      std::binary_semaphore low_gate(0), medium_gate(0), high_gate(0);
      std::counting_semaphore<3> policies_ready(0);
      std::atomic<bool> released = false, medium_timed_out = false,
                        high_acquired = false;
      std::jthread low([&] {
        rtx::set_fifo_priority(10);
        policies_ready.release();
        low_gate.acquire();
        inherited.lock();
        high_gate
            .release();  // high wakes, then sleeps while low releases medium
        medium_gate.release();
        released = true;
        inherited.unlock();
      });
      std::jthread medium([&] {
        rtx::set_fifo_priority(20);
        policies_ready.release();
        medium_gate.acquire();
        const auto limit = rtx::monotonic_now() + 100ms;
        while (!released.load() && rtx::monotonic_now() < limit) {
        }
        if (!released.load()) medium_timed_out = true;
      });
      std::jthread high([&] {
        rtx::set_fifo_priority(30);
        policies_ready.release();
        high_gate.acquire();
        std::this_thread::sleep_for(5ms);
        std::lock_guard lock(inherited);
        high_acquired = true;
      });
      for (int i = 0; i < 3; ++i) policies_ready.acquire();
      low_gate.release();
      low.join();
      medium.join();
      high.join();
      return !medium_timed_out.load() && high_acquired.load();
    };
    std::mutex ordinary;
    if (lock_owner_progress(ordinary))
      throw std::runtime_error(
          "ordinary-mutex control did not expose priority inversion");
    rtx::RuntimeMutex inherited;
    if (!lock_owner_progress(inherited))
      throw std::runtime_error("state mutex priority inheritance failed");
    std::cout << "priority inheritance let low lock owner outrun medium FIFO "
                 "spinner\n";
    std::cout << "FIFO supervisor detected heartbeat timeout during "
                 "non-yielding launch callback\n";
    std::cout << "higher-priority FIFO callback preempted busy lower-priority "
                 "callback on CPU "
              << cpu << '\n';
  } catch (const std::exception& ex) {
    std::cerr << ex.what() << '\n';
    return 1;
  }
}
