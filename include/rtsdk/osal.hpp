// Operating-system abstraction layer. The core library depends only on this
// header; each target (POSIX/Yocto, FreeRTOS, simulation) supplies an IPlatform.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>

namespace rtsdk {

using TimeUs = std::uint64_t;

constexpr TimeUs kMs = 1000;
constexpr TimeUs kSec = 1000 * kMs;

// Priorities are portable: 0 (lowest) .. 255 (highest). Backends map the range
// onto the native one (SCHED_FIFO 1..99, FreeRTOS configMAX_PRIORITIES-1, ...).
using Priority = std::uint8_t;

class IClock {
 public:
  virtual ~IClock() = default;
  virtual TimeUs now_us() const = 0;
  // Absolute sleep: immune to drift between successive periods.
  virtual void sleep_until(TimeUs deadline_us) = 0;
};

class IThread {
 public:
  virtual ~IThread() = default;
  virtual void join() = 0;
};

struct ThreadAttr {
  const char* name = "rtsdk";
  Priority priority = 0;
  std::size_t stack_bytes = 0;  // 0 = backend default
};

class IPlatform {
 public:
  virtual ~IPlatform() = default;
  virtual IClock& clock() = 0;
  virtual std::unique_ptr<IThread> spawn(const ThreadAttr& attr, std::function<void()> body) = 0;
  // True if the backend honoured the requested real-time priorities (on POSIX
  // this needs CAP_SYS_NICE/root; otherwise threads fall back to SCHED_OTHER).
  virtual bool realtime_priorities_active() const = 0;
};

// Deterministic clock for tests and simulation: sleeping advances time.
class ManualClock final : public IClock {
 public:
  TimeUs now_us() const override { return now_.load(std::memory_order_acquire); }
  void sleep_until(TimeUs t) override {
    TimeUs cur = now_.load();
    while (t > cur && !now_.compare_exchange_weak(cur, t)) {
    }
  }
  void advance(TimeUs d) { now_.fetch_add(d); }
  void set(TimeUs t) { now_.store(t); }

 private:
  std::atomic<TimeUs> now_{0};
};

// Hardware (or external) watchdog; kicked only while the system is healthy.
class IHwWatchdog {
 public:
  virtual ~IHwWatchdog() = default;
  virtual void kick() = 0;
};

}  // namespace rtsdk
