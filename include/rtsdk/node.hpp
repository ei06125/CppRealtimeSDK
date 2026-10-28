// A Node is the unit of scheduling: a periodic job with a priority, a deadline
// and liveness supervision parameters.
#pragma once

#include <atomic>
#include <cstdint>
#include <string>

#include "rtsdk/osal.hpp"

namespace rtsdk {

enum class FaultAction : std::uint8_t {
  Ignore,       // log only
  RestartNode,  // on_shutdown() + on_init(), bounded by max_restarts
  StopNode,     // take the node out of service
  SafeState,    // stop everything and invoke the safe-state hook
};

struct NodeConfig {
  std::string name;
  Priority priority = 100;
  TimeUs period_us = 10 * kMs;
  TimeUs deadline_us = 0;           // relative to release; 0 = period
  TimeUs heartbeat_timeout_us = 0;  // 0 = 3 * period
  FaultAction on_fault = FaultAction::RestartNode;
  std::uint32_t max_restarts = 3;
  std::uint32_t fault_threshold = 3;  // consecutive misses/failures before the watchdog is told
};

enum class NodeState : std::uint8_t { Created, Initialized, Running, Faulted, Stopped };

const char* to_string(NodeState s);
const char* to_string(FaultAction a);

struct StepContext {
  TimeUs now_us;      // time the job started executing
  TimeUs release_us;  // nominal release time (start - release = latency)
  std::uint64_t cycle;
};

class Node {
 public:
  virtual ~Node() = default;
  virtual bool on_init() { return true; }
  // Return false to report a functional failure to the watchdog.
  virtual bool on_step(const StepContext& ctx) = 0;
  virtual void on_shutdown() {}
};

struct NodeStats {
  std::uint64_t cycles = 0;
  std::uint64_t deadline_misses = 0;
  std::uint64_t skipped_releases = 0;  // releases lost because the previous job was still pending
  std::uint64_t step_failures = 0;
  TimeUs max_exec_us = 0;
  TimeUs max_latency_us = 0;  // release -> start
  TimeUs total_exec_us = 0;
};

}  // namespace rtsdk
