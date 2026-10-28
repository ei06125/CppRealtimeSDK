// Run each node in a native task beneath the calling supervisor priority.
// Publish worker measurements before completion so snapshots never query a
// deleted task handle.
#include "rtx/freertos.hpp"

#include <stdexcept>
namespace rtx {
Time freertos_now() {
  static_assert(sizeof(TickType_t) >= 8,
                "RTX FreeRTOS backend requires 64-bit ticks");
  static_assert(1000000000ULL % configTICK_RATE_HZ == 0,
                "tick frequency must divide one billion");
  constexpr auto scale = 1000000000ULL / configTICK_RATE_HZ;
  const auto ticks = xTaskGetTickCount();
  if (ticks > static_cast<std::uint64_t>(Time::max().count()) / scale)
    throw std::overflow_error("FreeRTOS clock range exceeded");
  return Time(ticks * scale);
}
FreeRtosTaskRunner::FreeRtosTaskRunner(Scheduler& scheduler,
                                       configSTACK_DEPTH_TYPE stack_depth)
    : scheduler_(scheduler), stack_depth_(stack_depth) {
  if (!stack_depth)
    throw std::invalid_argument("task stack depth must be positive");
}
std::vector<FreeRtosTaskResources> FreeRtosTaskRunner::task_resources() const {
  if (!finished_.load())
    throw std::logic_error("task resources require completed runner cleanup");
  std::vector<FreeRtosTaskResources> result;
  result.reserve(workers_.size());
  for (const auto& worker : workers_)
    result.push_back({scheduler_.entries_[worker.index].node.name,
                      static_cast<std::size_t>(stack_depth_),
                      sizeof(StackType_t), worker.created,
                      worker.minimum_free_stack_words});
  return result;
}
void FreeRtosTaskRunner::fail(std::exception_ptr failure) {
  {
    std::lock_guard lock(failure_mutex_);
    if (!failure_) failure_ = failure;
  }
  request_stop();
}
void FreeRtosTaskRunner::entry(void* argument) {
  auto& worker = *static_cast<Worker*>(argument);
  auto& owner = *worker.owner;
  try {
    while (!owner.stopping_.load() && !ulTaskNotifyTake(pdTRUE, 1)) {
    }
    if (!owner.stopping_.load()) {
      owner.scheduler_.step_node(worker.index, freertos_now(), freertos_now);
      {
        std::lock_guard lock(owner.scheduler_.mutex_);
        if (owner.scheduler_.entries_[worker.index].health.state ==
            State::faulted)
          throw std::runtime_error("FreeRTOS launch callback failed");
      }
      if (worker.index + 1 < owner.workers_.size())
        xTaskNotifyGive(owner.workers_[worker.index + 1].handle);
      while (!owner.stopping_.load()) {
        vTaskDelay(1);
        if (!owner.stopping_.load())
          owner.scheduler_.step_node(worker.index, freertos_now(),
                                     freertos_now);
      }
    }
  } catch (...) {
    owner.fail(std::current_exception());
  }
#if INCLUDE_uxTaskGetStackHighWaterMark == 1
  worker.minimum_free_stack_words = uxTaskGetStackHighWaterMark(nullptr);
#endif
  owner.live_.fetch_sub(1);
  vTaskDelete(nullptr);
}
void FreeRtosTaskRunner::run() {
  if (started_.exchange(true))
    throw std::logic_error("runner can only be started once");
  struct Completion {
    std::atomic<bool>& finished;
    ~Completion() { finished.store(true); }
  } completion{finished_};
  const auto supervisor_priority = uxTaskPriorityGet(nullptr);
  for (const auto& entry : scheduler_.entries_) {
    if (entry.node.priority < 1 ||
        static_cast<UBaseType_t>(entry.node.priority) >= supervisor_priority)
      throw std::invalid_argument(
          "node priority must be positive and below supervisor priority");
  }
  workers_.clear();
  workers_.reserve(scheduler_.entries_.size());
  for (std::size_t i = 0; i < scheduler_.entries_.size(); ++i)
    workers_.push_back({this, i});
  scheduler_.start(freertos_now());
  try {
    for (auto& worker : workers_) {
      if (stopping_.load()) break;
      live_.fetch_add(1);
      const auto& node = scheduler_.entries_[worker.index].node;
      if (xTaskCreate(entry, node.name.c_str(), stack_depth_, &worker,
                      static_cast<UBaseType_t>(node.priority),
                      &worker.handle) != pdPASS) {
        live_.fetch_sub(1);
        throw std::runtime_error("FreeRTOS task creation failed");
      }
      worker.created = true;
    }
    if (!stopping_.load() && !workers_.empty())
      xTaskNotifyGive(workers_[0].handle);
    while (!stopping_.load()) {
      scheduler_.inspect_watchdogs(freertos_now());
      vTaskDelay(1);
    }
  } catch (...) {
    fail(std::current_exception());
  }
  request_stop();
  while (live_.load()) vTaskDelay(1);
  scheduler_.stop();
  if (failure_) std::rethrow_exception(failure_);
}
}  // namespace rtx
