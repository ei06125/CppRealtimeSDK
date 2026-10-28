// Reject simultaneous or repeated run attempts on the same runner instance.
// A rejected attempt must not disturb the execution session that already owns
// the scheduler.
#include <future>
#include <iostream>
#include <thread>

#include "rtx/posix.hpp"
using namespace std::chrono_literals;
template <class Runner>
void verify(Runner& runner, rtx::Scheduler& scheduler,
            std::future<void> entered) {
  std::exception_ptr failure;
  std::jthread execution([&] {
    try {
      runner.run();
    } catch (...) {
      failure = std::current_exception();
    }
  });
  struct Guard {
    Runner& runner;
    ~Guard() { runner.request_stop(); }
  } guard{runner};
  if (entered.wait_for(2s) != std::future_status::ready)
    throw std::runtime_error("runner did not start");
  bool concurrent = false;
  try {
    runner.run();
  } catch (const std::logic_error&) {
    concurrent = true;
  }
  runner.request_stop();
  execution.join();
  if (failure) std::rethrow_exception(failure);
  bool repeated = false;
  try {
    runner.run();
  } catch (const std::logic_error&) {
    repeated = true;
  }
  auto h = scheduler.health()[0];
  if (!concurrent || !repeated || h.state != rtx::State::stopped ||
      h.ticks != 1)
    throw std::runtime_error("runner ownership guard failed");
}
int main() {
  try {
    {
      std::promise<void> entered;
      rtx::Scheduler scheduler(
          {{"node", 1, 10s, 1s, 0ns, {}, [&](auto&) { entered.set_value(); }}});
      rtx::PosixRunner runner(scheduler);
      verify(runner, scheduler, entered.get_future());
    }
    {
      std::promise<void> entered;
      rtx::Scheduler scheduler(
          {{"node", 1, 10s, 1s, 0ns, {}, [&](auto&) { entered.set_value(); }}});
      rtx::PosixTaskRunner runner(scheduler, false);
      verify(runner, scheduler, entered.get_future());
    }
    std::cout << "concurrent and repeated run attempts rejected without "
                 "disturbing execution\n";
  } catch (const std::exception& ex) {
    std::cerr << ex.what() << '\n';
    return 1;
  }
}
