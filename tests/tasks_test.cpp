// Show that independent node workers can progress while a peer callback is
// blocked. Policy validation must also reject invalid priorities before any
// application callback launches.
#include <sched.h>

#include <condition_variable>
#include <iostream>
#include <thread>

#include "rtx/posix.hpp"
using namespace std::chrono_literals;
int main() {
  std::mutex mutex;
  std::condition_variable wake;
  bool blocked = false, release = false, high_ran = false;
  unsigned low_calls = 0, high_calls = 0;
  rtx::Scheduler scheduler(
      {{"low",
        1,
        1ms,
        1s,
        1s,
        {},
        [&](const rtx::Context& c) {
          c.heartbeat();
          std::unique_lock lock(mutex);
          if (++low_calls == 2) {
            blocked = true;
            wake.notify_all();
            wake.wait(lock, [&] { return release; });
          }
        }},
       {"high", 20, 1ms, 1s, 1s, {"low"}, [&](const rtx::Context& c) {
          c.heartbeat();
          std::lock_guard lock(mutex);
          ++high_calls;
          if (blocked) {
            high_ran = true;
            wake.notify_all();
          }
        }}});
  rtx::PosixTaskRunner runner(scheduler, false);
  std::exception_ptr failure;
  std::jthread execution([&] {
    try {
      runner.run();
    } catch (...) {
      failure = std::current_exception();
    }
  });
  bool observed;
  {
    std::unique_lock lock(mutex);
    observed = wake.wait_for(lock, 3s, [&] { return high_ran; });
    release = true;
  }
  wake.notify_all();
  runner.request_stop();
  execution.join();
  if (failure || !observed || low_calls < 2 || high_calls < 2) {
    std::cerr << "independent worker execution failed\n";
    return 1;
  }
  // Invalid FIFO priorities must abort before any callback can execute, even if
  // another worker was already configured successfully.
  unsigned invoked = 0;
  rtx::Scheduler invalid(
      {{"invalid", 0, 1ms, 1ms, 0ns, {}, [&](auto&) { ++invoked; }}});
  rtx::PosixTaskRunner strict(invalid, true);
  bool rejected = false;
  try {
    strict.run();
  } catch (const std::invalid_argument&) {
    rejected = true;
  }
  if (!rejected || invoked ||
      invalid.health()[0].state != rtx::State::stopped) {
    std::cerr << "FIFO startup rejection failed\n";
    return 1;
  }
  rtx::Scheduler no_room({{"maximum",
                           sched_get_priority_max(SCHED_FIFO),
                           1ms,
                           1ms,
                           0ns,
                           {},
                           [&](auto&) { ++invoked; }}});
  rtx::PosixTaskRunner reserved(no_room, true);
  rejected = false;
  try {
    reserved.run();
  } catch (const std::invalid_argument&) {
    rejected = true;
  }
  if (!rejected || invoked ||
      no_room.health()[0].state != rtx::State::stopped) {
    std::cerr << "FIFO supervisor priority reservation failed\n";
    return 1;
  }
  std::cout << "independent node workers and strict FIFO startup passed\n";
}
