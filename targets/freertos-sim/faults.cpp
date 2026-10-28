// Run isolated lifecycle, watchdog and runtime-fault scenarios under the real
// kernel. Separate processes keep each scenario independent of sticky runner
// stop state.
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <string_view>

#include "rtx/freertos.hpp"
using namespace std::chrono_literals;
namespace {
std::string_view scenario;
void require(bool value, const char* message) {
  if (!value) throw std::runtime_error(message);
}
void lifecycle() {
  unsigned calls = 0;
  auto callback = [&](auto&) { ++calls; };
  rtx::Scheduler invalid({{"invalid", 7, 1ms, 1ms, 0ns, {}, callback}});
  rtx::FreeRtosTaskRunner rejected(invalid);
  bool caught = false;
  try {
    rejected.run();
  } catch (const std::invalid_argument&) {
    caught = true;
  }
  require(
      caught && !calls && invalid.health()[0].state == rtx::State::configured,
      "priority validation failed");
  require(rejected.task_resources().empty(),
          "validation failure reported created task resources");
  rtx::Scheduler early({{"early", 1, 1ms, 1ms, 0ns, {}, callback}});
  rtx::FreeRtosTaskRunner stopped(early);
  stopped.request_stop();
  stopped.run();
  require(!calls && early.health()[0].state == rtx::State::stopped,
          "pre-start stop failed");
  const auto unused = stopped.task_resources();
  require(unused.size() == 1 && !unused[0].created &&
              !unused[0].minimum_free_stack_words,
          "pre-start stop reported a used task stack");
  bool repeated = false;
  try {
    stopped.run();
  } catch (const std::logic_error&) {
    repeated = true;
  }
  require(repeated, "FreeRTOS runner restart was accepted");
  rtx::Scheduler launch({{"fails",
                          1,
                          1ms,
                          1ms,
                          0ns,
                          {},
                          [](auto&) { throw std::runtime_error("startup"); }},
                         {"dependent", 2, 1ms, 1ms, 0ns, {"fails"}, callback}});
  rtx::FreeRtosTaskRunner broken(launch);
  caught = false;
  try {
    broken.run();
  } catch (const std::runtime_error&) {
    caught = true;
  }
  auto health = launch.health();
  require(caught && !calls && health[0].fault == "startup" &&
              health[1].state == rtx::State::stopped,
          "launch fault suppression failed");
}
void isolation(bool watchdog) {
  std::atomic<bool> healthy_started = false, returned = false;
  unsigned after_fault = 0;
  bool observed = false;
  rtx::Scheduler* state = nullptr;
  rtx::FreeRtosTaskRunner* runner = nullptr;
  rtx::Scheduler scheduler(
      {{"faulty",
        1,
        2ms,
        3ms,
        watchdog ? 5ms : 0ns,
        {},
        [&](const rtx::Context& c) {
          c.heartbeat();
          if (c.tick > 1 && healthy_started.load()) {
            if (!watchdog) throw std::runtime_error("runtime failure");
            vTaskDelay(
                30);  // Supervisor must fault this node before it resumes.
            require(state->health()[0].state == rtx::State::faulted,
                    "watchdog did not detect blocked callback");
            c.heartbeat();
            returned = true;
          }
        }},
       {"healthy", 4, 1ms, 10ms, 100ms, {"faulty"}, [&](const rtx::Context& c) {
          healthy_started = true;
          c.heartbeat();
          const auto h = state->health();
          if (h[0].state == rtx::State::faulted) {
            observed = true;
            if ((!watchdog || returned.load()) && ++after_fault >= 3)
              runner->request_stop();
          }
          if (c.tick > 500) {
            runner->request_stop();
            throw std::runtime_error("fault observation timeout");
          }
        }}});
  state = &scheduler;
  rtx::FreeRtosTaskRunner execution(scheduler);
  runner = &execution;
  execution.run();
  auto health = scheduler.health();
  require(observed && after_fault >= 3 &&
              health[0].state == rtx::State::faulted &&
              health[1].state == rtx::State::stopped,
          "runtime fault isolation failed");
  require(
      health[0].fault == (watchdog ? "heartbeat timeout" : "runtime failure"),
      "fault provenance changed");
  if (watchdog)
    require(returned.load() && health[0].deadline_misses >= 1 &&
                health[0].ticks == 2,
            "blocked callback completion accounting failed");
}
void test(void*) {
  try {
    if (scenario == "lifecycle")
      lifecycle();
    else
      isolation(scenario == "watchdog");
    std::printf("FreeRTOS %.*s passed\n", static_cast<int>(scenario.size()),
                scenario.data());
    std::exit(0);
  } catch (const std::exception& ex) {
    std::fprintf(stderr, "%s\n", ex.what());
    std::exit(1);
  }
}
}  // namespace
int main(int argc, char** argv) {
  if (argc != 2) return 2;
  scenario = argv[1];
  if (scenario != "lifecycle" && scenario != "watchdog" &&
      scenario != "runtime")
    return 2;
  if (xTaskCreate(test, "supervisor", 8192, nullptr, 7, nullptr) != pdPASS)
    return 1;
  vTaskStartScheduler();
  return 1;
}
