// FreeRTOS backend. Maps IClock to the kernel tick (resolution = 1/configTICK_RATE_HZ)
// and nodes to tasks. Build with -DRTSDK_WITH_FREERTOS and the kernel include path.
#pragma once

#include <atomic>

#include "rtsdk/osal.hpp"

namespace rtsdk {

class FreeRtosClock final : public IClock {
 public:
  TimeUs now_us() const override;
  void sleep_until(TimeUs deadline_us) override;

 private:
  mutable std::atomic<std::uint64_t> ticks_ext_{0};  // 32-bit tick counter extended to 64 bit
};

class FreeRtosPlatform final : public IPlatform {
 public:
  IClock& clock() override { return clock_; }
  std::unique_ptr<IThread> spawn(const ThreadAttr& attr, std::function<void()> body) override;
  bool realtime_priorities_active() const override { return true; }  // tasks are always priority-scheduled
  static unsigned map_priority(Priority p);

 private:
  FreeRtosClock clock_;
};

}  // namespace rtsdk
