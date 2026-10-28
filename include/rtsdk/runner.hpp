// NodeRunner couples a Node with its config, lifecycle state and statistics.
// Executors (cooperative tick scheduler, thread executor) share execute_job().
#pragma once

#include <atomic>
#include <memory>
#include <mutex>

#include "rtsdk/node.hpp"
#include "rtsdk/watchdog.hpp"

namespace rtsdk {

class NodeRunner {
 public:
  NodeRunner(std::size_t id, NodeConfig cfg, std::unique_ptr<Node> node);

  std::size_t id() const { return id_; }
  const NodeConfig& config() const { return cfg_; }
  TimeUs deadline_us() const {
    return cfg_.deadline_us ? cfg_.deadline_us : cfg_.period_us;
  }
  NodeState state() const { return state_.load(std::memory_order_acquire); }
  NodeStats stats() const;
  std::uint32_t restarts() const { return restarts_.load(); }

  // Lifecycle (serialised against running jobs). try_* variants return false
  // if a job is stuck inside on_step - the caller should escalate.
  bool init();
  bool try_restart();
  bool try_stop();
  void set_state(NodeState s) { state_.store(s, std::memory_order_release); }
  void start() { set_state(NodeState::Running); }

  struct JobResult {
    bool ran = false;
    bool report_failure = false;
    bool report_miss = false;
    TimeUs end_us = 0;
  };
  JobResult run_job(IClock& clock, TimeUs release_us, std::uint64_t cycle);
  void note_skipped_release() {
    skipped_.fetch_add(1, std::memory_order_relaxed);
  }

 private:
  void reset_counters();

  std::size_t id_;
  NodeConfig cfg_;
  std::unique_ptr<Node> node_;
  std::atomic<NodeState> state_{NodeState::Created};
  std::atomic<std::uint32_t> restarts_{0};
  std::mutex mu_;

  std::atomic<std::uint64_t> cycles_{0}, misses_{0}, skipped_{0}, failures_{0};
  std::atomic<TimeUs> max_exec_{0}, max_latency_{0}, total_exec_{0};
  std::uint32_t consec_miss_ = 0, consec_fail_ = 0;  // guarded by mu_
};

// Runs one job, updates heartbeat, and reports threshold-crossing faults.
// Returns false if the job did not run (node not Running).
bool execute_job(NodeRunner& r, IClock& clock, Watchdog& wd, TimeUs release_us,
                 std::uint64_t cycle);

}  // namespace rtsdk
