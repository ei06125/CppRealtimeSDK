// Run native Cortex-M scenarios for launch, messages, preemption, faults and
// exception-state isolation. Emit the final success marker only after every
// scenario and resource check completes.
#include <atomic>
#include <exception>
#include <sstream>
#include <stdexcept>

#include "rtx/freertos.hpp"
#include "rtx/launch.hpp"
#include "rtx/mailbox.hpp"
using namespace std::chrono_literals;
extern "C" void board_print(const char*);
extern "C" void board_print_unsigned(unsigned);
extern "C" [[noreturn]] void board_exit(int);
namespace {
void require(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}
void report_resources(const char* scenario,
                      const rtx::FreeRtosTaskRunner& runner,
                      std::size_t count) {
  const auto resources = runner.task_resources();
  require(resources.size() == count, "Cortex-M4 task resource count failed");
  for (const auto& item : resources) {
    require(item.created && item.minimum_free_stack_words &&
                *item.minimum_free_stack_words > 0 &&
                *item.minimum_free_stack_words <= item.requested_stack_words,
            "Cortex-M4 task stack watermark unavailable or invalid");
    board_print("STACK ");
    board_print(scenario);
    board_print("/");
    board_print(item.name.c_str());
    board_print(" minimum_free_words=");
    board_print_unsigned(*item.minimum_free_stack_words);
    board_print(" reserved_words=");
    board_print_unsigned(item.requested_stack_words);
    board_print(" word_bytes=");
    board_print_unsigned(item.stack_word_bytes);
    board_print("\n");
  }
}
void preemption() {
  std::atomic<bool> busy = false, observed = false;
  bool low_launched = false, launch_order = true;
  rtx::Mailbox<16> channel;
  bool messages_valid = true;
  rtx::FreeRtosTaskRunner* active = nullptr;
  rtx::CallbackRegistry callbacks{
      {"low_tick",
       [&](const rtx::Context& c) {
         c.heartbeat();
         if (c.tick == 1) low_launched = true;
         const std::array<std::uint64_t, 2> message{c.tick, ~c.tick};
         channel.write(std::as_bytes(std::span(message)));
         if (c.tick == 2) {
           busy = true;
           const auto limit = rtx::freertos_now() + 50ms;
           while (!observed.load() && rtx::freertos_now() < limit) {
           }
           busy = false;
           active->request_stop();
         }
       }},
      {"high_tick", [&](const rtx::Context& c) {
         c.heartbeat();
         if (c.tick == 1) launch_order = low_launched;
         std::array<std::uint64_t, 2> message{};
         if (channel.read(std::as_writable_bytes(std::span(message))) !=
                 sizeof(message) ||
             message[0] == 0 || message[1] != ~message[0])
           messages_valid = false;
         if (busy.load()) observed = true;
         if (c.tick > 20) active->request_stop();
       }}};
  // Configure the dependent first to exercise parser and topological
  // reordering.
  std::istringstream launch(
      "rtx-launch 1\n"
      "node high high_tick 3 5000000 1000000000 0 low\n"
      "node low low_tick 1 1000000 1000000000 0\n");
  rtx::Scheduler scheduler(rtx::load_launch(launch, callbacks));
  rtx::FreeRtosTaskRunner runner(scheduler);
  active = &runner;
  runner.run();
  report_resources("preemption", runner, 2);
  require(observed.load() && launch_order && messages_valid,
          "Cortex-M4 preemption or launch order failed");
  for (const auto& health : scheduler.health())
    require(health.state == rtx::State::stopped && health.ticks >= 2,
            "Cortex-M4 stopped health failed");
  board_print(
      "PASS: Cortex-M4 launch parser, preemption, heartbeat and stop\n");
}
void watchdog() {
  bool detected = false, launch_failed = false;
  rtx::Scheduler* watched = nullptr;
  rtx::Scheduler scheduler(
      {{"busy", 1, 1s, 1s, 3ms, {}, [&](const rtx::Context& context) {
          const auto limit = rtx::freertos_now() + 20ms;
          while (rtx::freertos_now() < limit) {
          }
          detected = watched->health()[0].state == rtx::State::faulted;
          context.heartbeat();  // A late heartbeat and secondary error must not
                                // erase timeout.
          throw std::runtime_error("secondary callback failure");
        }}});
  watched = &scheduler;
  rtx::FreeRtosTaskRunner runner(scheduler);
  try {
    runner.run();
  } catch (const std::runtime_error&) {
    launch_failed = true;
  }
  report_resources("watchdog", runner, 1);
  const auto health = scheduler.health()[0];
  require(detected && launch_failed && health.ticks == 1 &&
              health.fault == "heartbeat timeout",
          "Cortex-M4 watchdog did not preempt callback or latch first fault");
  board_print(
      "PASS: Cortex-M4 watchdog during busy callback and first-fault latch\n");
}
void runtime_fault() {
  rtx::Scheduler* watched = nullptr;
  rtx::FreeRtosTaskRunner* active = nullptr;
  rtx::Scheduler scheduler(
      {{"bad",
        1,
        1ms,
        1s,
        0ns,
        {},
        [](const rtx::Context& context) {
          context.heartbeat();
          if (context.tick == 2)
            throw std::runtime_error("Cortex-M runtime failure");
        }},
       {"healthy", 3, 1ms, 1s, 0ns, {"bad"}, [&](const rtx::Context& context) {
          context.heartbeat();
          if (context.tick >= 2 &&
              watched->health()[0].state == rtx::State::faulted)
            active->request_stop();
          if (context.tick > 100) active->request_stop();
        }}});
  watched = &scheduler;
  rtx::FreeRtosTaskRunner runner(scheduler);
  active = &runner;
  runner.run();
  report_resources("runtime_fault", runner, 2);
  const auto health = scheduler.health();
  require(health[0].ticks == 2 &&
              health[0].fault == "Cortex-M runtime failure" &&
              health[1].ticks >= 2 && health[1].state == rtx::State::stopped,
          "Cortex-M4 callback failure was not isolated");
  board_print("PASS: Cortex-M4 runtime callback fault isolation\n");
}
void exception_state() {
  std::atomic<unsigned> phase = 0;
  rtx::FreeRtosTaskRunner* active = nullptr;
  rtx::Scheduler scheduler(
      {{"catch_low",
        1,
        1ms,
        1s,
        0ns,
        {},
        [&](const rtx::Context& context) {
          context.heartbeat();
          if (context.tick != 2) return;
          try {
            throw std::runtime_error("low task exception");
          } catch (...) {
            const auto own = std::current_exception();
            phase = 1;
            const auto limit = rtx::freertos_now() + 50ms;
            while (phase.load() < 2 && rtx::freertos_now() < limit)
              vTaskDelay(1);
            if (phase.load() != 2 || std::current_exception() != own) {
              board_print(
                  "FAIL: task-local C++ exception state leaked into low "
                  "task\n");
              board_exit(1);
            }
          }
          phase = 3;
        }},
       {"catch_high",
        3,
        1ms,
        1s,
        0ns,
        {"catch_low"},
        [&](const rtx::Context& context) {
          context.heartbeat();
          if (phase.load() == 1) {
            try {
              throw std::runtime_error("high task exception");
            } catch (...) {
              const auto own = std::current_exception();
              phase = 2;
              vTaskDelay(5);
              if (phase.load() != 3 || std::current_exception() != own) {
                board_print(
                    "FAIL: task-local C++ exception state leaked into high "
                    "task\n");
                board_exit(1);
              }
            }
            phase = 4;
            active->request_stop();
          }
          if (context.tick > 100) active->request_stop();
        }}});
  rtx::FreeRtosTaskRunner runner(scheduler);
  active = &runner;
  runner.run();
  report_resources("exception_state", runner, 2);
  require(phase.load() == 4,
          "Cortex-M4 concurrent catch test did not complete");
  board_print(
      "PASS: Cortex-M4 task-local caught exception state across task "
      "switches\n");
}
void supervise(void*) {
  try {
    preemption();
    watchdog();
    runtime_fault();
    exception_state();
    const auto free_words = uxTaskGetStackHighWaterMark(nullptr);
    board_print("STACK supervisor minimum_free_words=");
    board_print_unsigned(free_words);
    board_print(" reserved_words=8192 word_bytes=4\n");
    board_print("PASS: Cortex-M4 full execution suite\n");
    board_exit(0);
  } catch (const std::exception& error) {
    board_print("FAIL: ");
    board_print(error.what());
    board_print("\n");
    board_exit(1);
  }
}
}  // namespace
int main() {
  board_print("RTX Cortex-M4 starting\n");
  if (xTaskCreate(supervise, "supervisor", 8192, nullptr, 6, nullptr) != pdPASS)
    board_exit(1);
  vTaskStartScheduler();
  board_exit(1);
}
