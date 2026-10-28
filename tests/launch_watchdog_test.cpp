// Check that a dependent node watchdog arms at its first dispatch, not during
// predecessor startup. Coordinate launch delays explicitly so this invariant is
// independent of unrelated host load.
#include <condition_variable>
#include <iostream>
#include <thread>

#include "rtx/posix.hpp"
using namespace std::chrono_literals;
int main() {
  std::mutex mutex;
  std::condition_variable wake;
  bool entered = false, release = false, dependent_ran = false;
  rtx::Scheduler scheduler(
      {{"first",
        1,
        1s,
        1s,
        0ns,
        {},
        [&](auto& c) {
          c.heartbeat();
          std::unique_lock lock(mutex);
          entered = true;
          wake.notify_all();
          wake.wait(lock, [&] { return release; });
        }},
       {"dependent", 2, 1s, 1s, 5ms, {"first"}, [&](auto& c) {
          c.heartbeat();
          std::lock_guard lock(mutex);
          dependent_ran = true;
          wake.notify_all();
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
  bool started, waiting_healthy, launched;
  {
    std::unique_lock lock(mutex);
    started = wake.wait_for(lock, 2s, [&] { return entered; });
  }
  // The predecessor remains blocked for several dependent watchdog intervals.
  std::this_thread::sleep_for(30ms);
  const auto waiting = scheduler.health()[1];
  waiting_healthy = waiting.state == rtx::State::running &&
                    waiting.ticks == 0 && waiting.fault.empty();
  {
    std::lock_guard lock(mutex);
    release = true;
  }
  wake.notify_all();
  {
    std::unique_lock lock(mutex);
    launched = wake.wait_for(lock, 2s, [&] { return dependent_ran; });
  }
  runner.request_stop();
  execution.join();
  if (failure || !started || !waiting_healthy || !launched) {
    std::cerr << "waiting launch dependency was incorrectly timed out\n";
    return 1;
  }
  std::cout << "watchdog arms at first invocation after launch dependencies "
               "complete\n";
}
