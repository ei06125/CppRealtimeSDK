// Exercise signed nanosecond boundaries, clock regressions and release
// overflow. Use synthetic times to reach long-uptime edge cases without waiting
// on wall-clock execution.
#include <iostream>
#include <stdexcept>

#include "rtx/scheduler.hpp"
using namespace std::chrono_literals;
void require(bool value) {
  if (!value) throw std::runtime_error("time requirement failed");
}
int main() {
  try {
    unsigned calls = 0;
    rtx::Scheduler scheduler(
        {{"n", 1, 3ns, 3ns, 3ns, {}, [&](auto&) { ++calls; }}});
    const auto maximum = rtx::Time::max();
    scheduler.start(maximum - 5ns);
    scheduler.step(maximum - 5ns, [&] { return maximum - 5ns; });
    require(calls == 1 && scheduler.health()[0].next_release == maximum - 2ns);
    scheduler.step(maximum - 2ns, [&] { return maximum - 2ns; });
    require(calls == 1 && scheduler.health()[0].state == rtx::State::faulted &&
            scheduler.health()[0].fault == "release time overflow");
    scheduler.stop();
    bool rejected = false;
    try {
      scheduler.start(-1ns);
    } catch (const std::invalid_argument&) {
      rejected = true;
    }
    require(rejected);
    scheduler.start(5ns);
    rejected = false;
    try {
      scheduler.step(4ns, [] { return 4ns; });
    } catch (const std::invalid_argument&) {
      rejected = true;
    }
    require(rejected);
    scheduler.step(5ns, [] { return 4ns; });
    require(scheduler.health()[0].fault == "completion clock regressed");
    rtx::Scheduler huge(
        {{"huge", 1, maximum / 2 + 1ns, maximum, maximum, {}, [](auto&) {}}});
    huge.start(0ns);
    huge.step(maximum - 1ns, [&] { return maximum; });
    require(huge.health()[0].fault == "release time overflow");
    rtx::Scheduler watchdog(
        {{"w", 1, 1ns, 1ns, maximum - 1ns, {}, [](auto&) {}}});
    watchdog.start(0ns);
    watchdog.step(0ns, [] { return 0ns; });
    watchdog.inspect_watchdogs(maximum);
    require(watchdog.health()[0].state == rtx::State::faulted);
    std::cout << "clock range, regression and release overflow tests passed\n";
  } catch (const std::exception& ex) {
    std::cerr << ex.what() << '\n';
    return 1;
  }
}
