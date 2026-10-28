// Lock-free liveness table: nodes (or their executor) beat, the watchdog reads.
#pragma once

#include <atomic>
#include <cstddef>
#include <deque>

#include "rtsdk/osal.hpp"

namespace rtsdk {

class HeartbeatMonitor {
 public:
  // Registration must complete before concurrent beat()/expired() calls.
  std::size_t add(TimeUs timeout_us);

  void enable(std::size_t id, TimeUs now);  // starts supervision with a fresh beat
  void disable(std::size_t id);
  void beat(std::size_t id, TimeUs now);

  bool enabled(std::size_t id) const;
  TimeUs age(std::size_t id, TimeUs now) const;
  bool expired(std::size_t id, TimeUs now) const;
  // Returns true exactly once per expiry episode (re-armed by the next beat).
  bool latch_expiry(std::size_t id);
  std::size_t size() const { return entries_.size(); }

 private:
  struct Entry {
    explicit Entry(TimeUs t) : timeout(t) {}
    TimeUs timeout;
    std::atomic<TimeUs> last_beat{0};
    std::atomic<bool> enabled{false};
    std::atomic<bool> latched{false};
  };
  std::deque<Entry> entries_;
};

}  // namespace rtsdk
