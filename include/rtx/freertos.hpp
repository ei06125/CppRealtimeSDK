// Native FreeRTOS task execution and post-run resource inspection.
// Compile applications and the core with the same kernel configuration and RTX_FREERTOS definition.
#pragma once
#ifndef RTX_FREERTOS
#error "Compile the core and application with RTX_FREERTOS and a configured FreeRTOS kernel"
#endif
#include "rtx/scheduler.hpp"
#include <task.h>
#include <atomic>
#include <exception>
#include <optional>
namespace rtx {
Time freertos_now();
struct FreeRtosTaskResources {
 std::string name;
 std::size_t requested_stack_words,stack_word_bytes;
 bool created;
 std::optional<std::size_t> minimum_free_stack_words;
};
// Call run from a supervisor task with priority higher than every node.
// Requires 64-bit ticks, dynamic allocation, task notifications, and C++ exceptions.
class FreeRtosTaskRunner {
 public:
 explicit FreeRtosTaskRunner(Scheduler& scheduler,configSTACK_DEPTH_TYPE stack_depth=4096);
 void run();
 void request_stop() noexcept {stopping_.store(true);}
 // Management snapshot after run returns/throws and cleanup completes; allocates.
 // Stack watermark is absent if the kernel measurement API is disabled.
 std::vector<FreeRtosTaskResources> task_resources() const;
 private:
 struct Worker {
  FreeRtosTaskRunner* owner;std::size_t index;TaskHandle_t handle=nullptr;
  bool created=false;
  std::optional<std::size_t> minimum_free_stack_words=std::nullopt;
 };
 static void entry(void* argument);
 void fail(std::exception_ptr failure);
 Scheduler& scheduler_;
 configSTACK_DEPTH_TYPE stack_depth_;
 std::atomic<bool> stopping_{false};
 std::atomic<bool> started_{false};
 std::atomic<bool> finished_{false};
 std::atomic<unsigned> live_{0};
 std::vector<Worker> workers_;
 RuntimeMutex failure_mutex_;
 std::exception_ptr failure_;
};
}
