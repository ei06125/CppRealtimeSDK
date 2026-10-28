// Verify deterministic overload accounting, launch suppression and first-fault
// provenance. Distinguish configuration-order release ties from the topological
// order required at startup.
#include <iostream>
#include <stdexcept>

#include "rtx/scheduler.hpp"
using namespace std::chrono_literals;
void require(bool value, const char* message) {
  if (!value) throw std::runtime_error(message);
}
int main() {
  try {
    rtx::Time now = 0ns;
    rtx::Scheduler overloaded(
        {{"slow", 1, 1ms, 1ms, 0ns, {}, [&](auto&) { now += 3ms; }}});
    overloaded.start(now);
    overloaded.step(now, [&] { return now; });
    overloaded.step(now, [&] { return now; });
    auto h = overloaded.health()[0];
    require(h.ticks == 2 && h.deadline_misses == 2 && h.skipped_releases == 2 &&
                h.next_release == 4ms,
            "overload release accounting failed");
    unsigned dependent = 0, healthy = 0;
    rtx::Scheduler faults(
        {{"bad",
          1,
          1ms,
          1ms,
          0ns,
          {},
          [](auto&) { throw std::runtime_error("bad launch"); }},
         {"dependent", 2, 1ms, 1ms, 0ns, {"bad"}, [&](auto&) { ++dependent; }},
         {"healthy", 3, 1ms, 1ms, 0ns, {}, [&](auto&) { ++healthy; }}});
    faults.start(0ns);
    faults.step(0ns, [] { return 0ns; });
    faults.step(1ms, [] { return 1ms; });
    auto all = faults.health();
    require(!dependent && healthy == 2,
            "launch fault failed to isolate dependent");
    require(all[0].fault == "bad launch" &&
                all[1].fault == "launch dependency fault: bad",
            "launch fault provenance missing");
    now = 0ns;
    bool fresh = false;
    rtx::Scheduler sequence(
        {{"setup", 1, 1s, 1s, 0ns, {}, [&](auto&) { now = 30ms; }},
         {"after", 2, 1s, 1s, 5ms, {"setup"}, [&](const rtx::Context& c) {
            fresh = c.now == 30ms;
          }}});
    sequence.start(0ns);
    sequence.step(0ns, [&] { return now; });
    sequence.inspect_watchdogs(31ms);
    require(fresh && sequence.health()[1].state == rtx::State::running,
            "cooperative launch used stale watchdog timestamp");
    std::vector<int> order;
    rtx::Scheduler ties(
        {{"a", 5, 1ms, 1ms, 0ns, {}, [&](auto&) { order.push_back(1); }},
         {"b", 5, 1ms, 1ms, 0ns, {}, [&](auto&) { order.push_back(2); }},
         {"c", 5, 1ms, 1ms, 0ns, {}, [&](auto&) { order.push_back(3); }}});
    ties.start(0ns);
    ties.step(0ns, [] { return 0ns; });
    order.clear();
    ties.step(1ms, [] { return 1ms; });
    require(order == std::vector<int>{1, 2, 3},
            "equal priority ordering changed");
    order.clear();
    rtx::Scheduler reordered(
        {{"dependent",
          5,
          1ms,
          1ms,
          0ns,
          {"root"},
          [&](auto&) { order.push_back(1); }},
         {"independent",
          5,
          1ms,
          1ms,
          0ns,
          {},
          [&](auto&) { order.push_back(2); }},
         {"root", 5, 1ms, 1ms, 0ns, {}, [&](auto&) { order.push_back(3); }}});
    reordered.start(0ns);
    reordered.step(0ns, [] { return 0ns; });
    require(order == std::vector<int>{2, 3, 1},
            "dependency launch order changed");
    order.clear();
    reordered.step(1ms, [] { return 1ms; });
    require(
        order == std::vector<int>{1, 2, 3},
        "equal priorities used launch order instead of configuration order");
    rtx::Scheduler empty_error({{"empty", 1, 1ms, 1ms, 0ns, {}, [](auto&) {
                                   throw std::runtime_error("");
                                 }}});
    empty_error.start(0ns);
    empty_error.step(0ns, [] { return 0ns; });
    require(empty_error.health()[0].state == rtx::State::faulted &&
                !empty_error.health()[0].fault.empty(),
            "empty exception message ignored");
    rtx::Scheduler* active = nullptr;
    now = 0ns;
    rtx::Scheduler latched({{"late", 1, 1ms, 1ms, 5ms, {}, [&](auto&) {
                               now = 10ms;
                               active->inspect_watchdogs(now);
                               throw std::runtime_error("secondary failure");
                             }}});
    active = &latched;
    latched.start(0ns);
    latched.step(0ns, [&] { return now; });
    require(latched.health()[0].fault == "heartbeat timeout" &&
                latched.health()[0].deadline_misses == 1,
            "first fault was overwritten");
    std::cout << "deterministic overload and launch fault propagation passed\n";
  } catch (const std::exception& ex) {
    std::cerr << ex.what() << '\n';
    return 1;
  }
}
