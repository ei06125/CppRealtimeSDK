// Supervision: collects faults (heartbeat timeouts, deadline misses, step
// failures) and kicks the hardware watchdog only while no heartbeat is expired.
#pragma once

#include <cstdint>
#include <functional>

#include "rtsdk/heartbeat.hpp"
#include "rtsdk/osal.hpp"

namespace rtsdk {

enum class FaultKind : std::uint8_t { HeartbeatTimeout, DeadlineMiss, StepFailure };
const char* to_string(FaultKind k);

struct Fault {
  std::size_t node;
  FaultKind kind;
  TimeUs at_us;
};

class Watchdog {
 public:
  using Handler = std::function<void(const Fault&)>;

  Watchdog(IClock& clock, HeartbeatMonitor& hb, IHwWatchdog* hw = nullptr)
      : clock_(clock), hb_(hb), hw_(hw) {}

  void set_handler(Handler h) { handler_ = std::move(h); }
  HeartbeatMonitor& heartbeats() { return hb_; }

  void report(std::size_t node, FaultKind kind);
  // Evaluates all heartbeats; returns the number of new expiries reported.
  std::size_t check();

  std::uint64_t faults_reported() const { return faults_; }
  std::uint64_t hw_kicks() const { return kicks_; }

 private:
  IClock& clock_;
  HeartbeatMonitor& hb_;
  IHwWatchdog* hw_;
  Handler handler_;
  std::atomic<std::uint64_t> faults_{0};
  std::atomic<std::uint64_t> kicks_{0};
};

}  // namespace rtsdk
