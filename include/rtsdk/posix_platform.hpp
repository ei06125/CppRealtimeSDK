// POSIX / Yocto-Linux backend: CLOCK_MONOTONIC + pthreads with SCHED_FIFO.
#pragma once

#include <atomic>

#include "rtsdk/comms.hpp"
#include "rtsdk/osal.hpp"

namespace rtsdk {

class PosixClock final : public IClock {
 public:
  TimeUs now_us() const override;
  void sleep_until(TimeUs deadline_us) override;
};

class PosixPlatform final : public IPlatform {
 public:
  IClock& clock() override { return clock_; }
  std::unique_ptr<IThread> spawn(const ThreadAttr& attr,
                                 std::function<void()> body) override;
  bool realtime_priorities_active() const override { return rt_ok_.load(); }
  // Maps portable 0..255 onto the SCHED_FIFO range.
  static int map_priority(Priority p);

 private:
  PosixClock clock_;
  std::atomic<bool> rt_ok_{false};
};

}  // namespace rtsdk
