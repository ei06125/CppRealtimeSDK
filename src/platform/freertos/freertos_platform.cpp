#include "rtsdk/freertos_platform.hpp"

extern "C" {
#include "FreeRTOS.h"
#include "semphr.h"
#include "task.h"
}

namespace rtsdk {

namespace {
constexpr TimeUs kTickUs = 1000000u / configTICK_RATE_HZ;
}

TimeUs FreeRtosClock::now_us() const {
  const std::uint64_t t32 = static_cast<std::uint64_t>(xTaskGetTickCount());
  std::uint64_t prev = ticks_ext_.load(std::memory_order_relaxed);
  std::uint64_t cand = (prev & ~0xFFFFFFFFull) | t32;
  if (cand + 0x80000000ull < prev) cand += 0x100000000ull;  // wrapped (needs a call per half-wrap)
  while (cand > prev && !ticks_ext_.compare_exchange_weak(prev, cand, std::memory_order_relaxed)) {
  }
  return cand * kTickUs;
}

void FreeRtosClock::sleep_until(TimeUs deadline_us) {
  const TimeUs now = now_us();
  if (deadline_us <= now) return;
  const TimeUs delta = deadline_us - now;
  vTaskDelay(static_cast<TickType_t>((delta + kTickUs - 1) / kTickUs));  // round up: never wake early
}

unsigned FreeRtosPlatform::map_priority(Priority p) {
  // 0 stays above the idle task (priority 0 in FreeRTOS).
  return 1u + (static_cast<unsigned>(p) * (configMAX_PRIORITIES - 2)) / 255u;
}

namespace {

struct TaskCtx {
  std::function<void()> body;
  SemaphoreHandle_t done;
};

void task_entry(void* arg) {
  auto* ctx = static_cast<TaskCtx*>(arg);
  ctx->body();
  xSemaphoreGive(ctx->done);
  vTaskDelete(nullptr);
}

class RtosThread final : public IThread {
 public:
  explicit RtosThread(TaskCtx* c) : ctx_(c) {}
  ~RtosThread() override { join(); }
  void join() override {
    if (!ctx_) return;
    xSemaphoreTake(ctx_->done, portMAX_DELAY);
    vSemaphoreDelete(ctx_->done);
    delete ctx_;
    ctx_ = nullptr;
  }

 private:
  TaskCtx* ctx_;
};

}  // namespace

std::unique_ptr<IThread> FreeRtosPlatform::spawn(const ThreadAttr& attr, std::function<void()> body) {
  auto* ctx = new TaskCtx{std::move(body), xSemaphoreCreateBinary()};
  const configSTACK_DEPTH_TYPE depth =
      attr.stack_bytes ? static_cast<configSTACK_DEPTH_TYPE>(attr.stack_bytes / sizeof(StackType_t)) : 1024;
  if (xTaskCreate(task_entry, attr.name, depth, ctx, map_priority(attr.priority), nullptr) != pdPASS) {
    vSemaphoreDelete(ctx->done);
    delete ctx;
    return nullptr;
  }
  return std::make_unique<RtosThread>(ctx);
}

}  // namespace rtsdk
