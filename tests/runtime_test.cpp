// Exercise scheduler behavior and cross-process mailbox integration using real
// runtime primitives. Use explicit checks so Release builds retain the same
// validation as Debug builds.
#include <sys/wait.h>
#include <unistd.h>

#include <array>
#include <iostream>
#include <stdexcept>

#include "rtx/posix.hpp"
#include "rtx/scheduler.hpp"
#include "rtx/shared_memory.hpp"
using namespace std::chrono_literals;
void require(bool value) {
  if (!value) throw std::runtime_error("requirement failed");
}
int main() {
  try {
    std::vector<std::string> order;
    rtx::Time now = 0ns;
    auto callback = [&](std::string name) {
      return [&, name](const rtx::Context& c) {
        order.push_back(name);
        c.heartbeat();
      };
    };
    rtx::Scheduler s({{"high", 10, 10ms, 5ms, 20ms, {"low"}, callback("high")},
                      {"low", 1, 10ms, 5ms, 20ms, {}, callback("low")}});
    s.start(now);
    s.step(now, [&] { return now; });
    require(order == std::vector<std::string>({"low", "high"}));
    order.clear();
    now = 10ms;
    s.step(now, [&] { return now; });
    require(order == std::vector<std::string>({"high", "low"}));
    now = 45ms;
    s.step(now, [&] { return now; });
    require(s.health()[0].skipped_releases == 2);
    require(s.health()[0].deadline_misses == 1);
    s.inspect_watchdogs(66ms);
    require(s.health()[0].state == rtx::State::faulted);
    s.stop();
    s.start(100ms);
    require(s.health()[0].ticks == 0);
    bool rejected = false;
    try {
      rtx::Scheduler bad({{"a", 0, 1ms, 1ms, 0ns, {"a"}, callback("a")}});
    } catch (const std::invalid_argument&) {
      rejected = true;
    }
    require(rejected);
    rtx::Scheduler faults({{"throws", 0, 1ms, 1ms, 0ns, {}, [](auto&) {
                              throw std::runtime_error("failure");
                            }}});
    faults.start(0ns);
    faults.step(0ns, [] { return 0ns; });
    require(faults.health()[0].fault == "failure");
    const auto name = "/rtx-test-" + std::to_string(getpid());
    rtx::SharedMemory::unlink(name);
    {
      rtx::SharedMemory mailbox(name, 16, true);
      pid_t child = fork();
      require(child >= 0);
      if (child == 0) {
        try {
          rtx::SharedMemory peer(name, 16, false);
          std::array<std::byte, 2> data{std::byte{42}, std::byte{7}};
          peer.write(data);
          _exit(0);
        } catch (...) {
          _exit(1);
        }
      }
      int status;
      require(waitpid(child, &status, 0) == child && WIFEXITED(status) &&
              WEXITSTATUS(status) == 0);
      std::array<std::byte, 16> data{};
      require(mailbox.read(data) == 2 && data[0] == std::byte{42});
    }
    rtx::SharedMemory::unlink(name);
    require(rtx::monotonic_now() > 0ns);
    std::cout << "runtime behavior and cross-process mailbox tests passed\n";
  } catch (const std::exception& e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
