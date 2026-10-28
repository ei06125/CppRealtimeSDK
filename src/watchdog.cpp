#include "rtsdk/watchdog.hpp"

namespace rtsdk {

const char* to_string(FaultKind k) {
  switch (k) {
    case FaultKind::HeartbeatTimeout:
      return "heartbeat-timeout";
    case FaultKind::DeadlineMiss:
      return "deadline-miss";
    case FaultKind::StepFailure:
      return "step-failure";
  }
  return "?";
}

// Single entry point for all fault sources; counts then forwards to the
// handler.
void Watchdog::report(std::size_t node, FaultKind kind) {
  faults_.fetch_add(1, std::memory_order_relaxed);
  if (handler_) handler_(Fault{node, kind, clock_.now_us()});
}

// Scans heartbeats and reports newly expired ones. The hardware watchdog is
// kicked only if every monitored node is healthy, so a stuck node lets the HW
// watchdog reset the ECU. Returns the number of newly detected expiries.
std::size_t Watchdog::check() {
  const TimeUs now = clock_.now_us();
  std::size_t fresh = 0;
  bool healthy = true;
  for (std::size_t i = 0; i < hb_.size(); ++i) {
    if (!hb_.expired(i, now)) continue;
    healthy = false;
    if (hb_.latch_expiry(i)) {
      ++fresh;
      report(i, FaultKind::HeartbeatTimeout);
    }
  }
  if (healthy && hw_) {
    hw_->kick();
    kicks_.fetch_add(1, std::memory_order_relaxed);
  }
  return fresh;
}

}  // namespace rtsdk
