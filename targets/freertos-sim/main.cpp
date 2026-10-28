// Verify actual kernel task priorities, launch ordering and preemption of a
// busy low task. The calling test task provides supervision above all
// configured node priorities.
#include <atomic>
#include <cstdio>
#include <cstdlib>

#include "rtx/freertos.hpp"
using namespace std::chrono_literals;
static void test(void*) {
  try {
    std::atomic<unsigned> high = 0;
    bool preempted = false;
    rtx::FreeRtosTaskRunner* runner = nullptr;
    rtx::Scheduler scheduler(
        {{"low",
          1,
          2ms,
          20ms,
          100ms,
          {},
          [&](const rtx::Context& c) {
            c.heartbeat();
            if (c.tick == 2) {
              const auto baseline = high.load();
              auto limit = rtx::freertos_now() + 50ms;
              // No yield or blocking: only the FreeRTOS scheduler can run the
              // high task.
              while (high.load() <= baseline && rtx::freertos_now() < limit) {
              }
              preempted = high.load() > baseline;
            }
          }},
         {"high", 4, 2ms, 20ms, 100ms, {"low"}, [&](const rtx::Context& c) {
            ++high;
            c.heartbeat();
            if (high >= 4) runner->request_stop();
          }}});
    rtx::FreeRtosTaskRunner execution(scheduler);
    runner = &execution;
    bool rejected = false;
    try {
      execution.task_resources();
    } catch (const std::logic_error&) {
      rejected = true;
    }
    if (!rejected) throw std::runtime_error("early resource snapshot accepted");
    execution.run();
    const auto resources = execution.task_resources();
    if (resources.size() != 2)
      throw std::runtime_error("missing task resources");
    for (const auto& item : resources)
      if (!item.created || item.requested_stack_words != 4096 ||
          item.stack_word_bytes != sizeof(StackType_t) ||
          item.minimum_free_stack_words.has_value())
        throw std::runtime_error(
            "invalid task resource snapshot with watermark disabled");
    auto health = scheduler.health();
    if (!preempted || high < 4 || health[0].state != rtx::State::stopped ||
        health[1].state != rtx::State::stopped)
      throw std::runtime_error("FreeRTOS priority execution failed");
    std::puts(
        "FreeRTOS kernel tasks, launch sequencing and priority preemption "
        "passed");
    std::exit(0);
  } catch (const std::exception& ex) {
    std::fprintf(stderr, "%s\n", ex.what());
    std::exit(1);
  }
}
int main() {
  if (xTaskCreate(test, "supervisor", 8192, nullptr, 7, nullptr) != pdPASS)
    return 1;
  vTaskStartScheduler();
  return 1;
}
