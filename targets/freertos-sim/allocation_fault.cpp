// Inject first-worker and partial-worker creation failures at the kernel API
// boundary. No callback may launch, and post-cleanup resources must identify
// which tasks were actually created.
#include <cstdio>
#include <cstdlib>
#include <string_view>

#include "rtx/freertos.hpp"
using namespace std::chrono_literals;
namespace {
unsigned calls = 0, fail_on = 0;
}
extern "C" BaseType_t __real_xTaskCreate(TaskFunction_t, const char*,
                                         configSTACK_DEPTH_TYPE, void*,
                                         UBaseType_t, TaskHandle_t*);
extern "C" BaseType_t __wrap_xTaskCreate(TaskFunction_t task, const char* name,
                                         configSTACK_DEPTH_TYPE stack,
                                         void* context, UBaseType_t priority,
                                         TaskHandle_t* handle) {
  if (fail_on && ++calls == fail_on)
    return errCOULD_NOT_ALLOCATE_REQUIRED_MEMORY;
  return __real_xTaskCreate(task, name, stack, context, priority, handle);
}
static void test(void*) {
  try {
    // Also test first-worker failure, not just cleanup of a partially created
    // set.
    for (unsigned target = 1; target <= 2; ++target) {
      unsigned invoked = 0;
      rtx::Scheduler scheduler(
          {{"first", 1, 1ms, 1ms, 0ns, {}, [&](auto&) { ++invoked; }},
           {"second", 2, 1ms, 1ms, 0ns, {"first"}, [&](auto&) { ++invoked; }}});
      rtx::FreeRtosTaskRunner runner(scheduler);
      calls = 0;
      fail_on = target;
      bool rejected = false;
      try {
        runner.run();
      } catch (const std::runtime_error& ex) {
        rejected =
            std::string_view(ex.what()) == "FreeRTOS task creation failed";
      }
      fail_on = 0;
      if (!rejected || invoked || calls != target)
        throw std::runtime_error("task creation failure was not isolated");
      for (const auto& h : scheduler.health())
        if (h.state != rtx::State::stopped)
          throw std::runtime_error("partial startup left node running");
      const auto resources = runner.task_resources();
      if (resources.size() != 2 || resources[0].created != (target == 2) ||
          resources[1].created)
        throw std::runtime_error("partial task resource snapshot incorrect");
      vTaskDelay(2);  // Allow idle-task cleanup of any self-deleted worker.
    }
    std::puts("first and partial FreeRTOS task-creation failures cleaned up");
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
