#include "rtsdk/runner.hpp"

#include <algorithm>

namespace rtsdk {

const char* to_string(NodeState s) {
  switch (s) {
    case NodeState::Created: return "created";
    case NodeState::Initialized: return "initialized";
    case NodeState::Running: return "running";
    case NodeState::Faulted: return "faulted";
    case NodeState::Stopped: return "stopped";
  }
  return "?";
}

const char* to_string(FaultAction a) {
  switch (a) {
    case FaultAction::Ignore: return "ignore";
    case FaultAction::RestartNode: return "restart";
    case FaultAction::StopNode: return "stop";
    case FaultAction::SafeState: return "safe-state";
  }
  return "?";
}

namespace {
// Lock-free running maximum (CAS loop) for exec-time / latency statistics.
void store_max(std::atomic<TimeUs>& a, TimeUs v) {
  TimeUs cur = a.load(std::memory_order_relaxed);
  while (v > cur && !a.compare_exchange_weak(cur, v, std::memory_order_relaxed)) {
  }
}
}  // namespace

NodeRunner::NodeRunner(std::size_t id, NodeConfig cfg, std::unique_ptr<Node> node)
    : id_(id), cfg_(std::move(cfg)), node_(std::move(node)) {}

NodeStats NodeRunner::stats() const {
  NodeStats s;
  s.cycles = cycles_.load(std::memory_order_relaxed);
  s.deadline_misses = misses_.load(std::memory_order_relaxed);
  s.skipped_releases = skipped_.load(std::memory_order_relaxed);
  s.step_failures = failures_.load(std::memory_order_relaxed);
  s.max_exec_us = max_exec_.load(std::memory_order_relaxed);
  s.max_latency_us = max_latency_.load(std::memory_order_relaxed);
  s.total_exec_us = total_exec_.load(std::memory_order_relaxed);
  return s;
}

// Consecutive-fault counters restart after every (re)initialisation.
void NodeRunner::reset_counters() {
  consec_miss_ = consec_fail_ = 0;
}

// Runs on_init once; failure latches the node as Faulted.
bool NodeRunner::init() {
  std::lock_guard<std::mutex> lk(mu_);
  reset_counters();
  if (node_->on_init()) {
    set_state(NodeState::Initialized);
    return true;
  }
  set_state(NodeState::Faulted);
  return false;
}

// Non-blocking restart: if the node is inside on_step the lock is busy and we
// report failure rather than stall the watchdog/supervisor thread.
bool NodeRunner::try_restart() {
  std::unique_lock<std::mutex> lk(mu_, std::try_to_lock);
  if (!lk) return false;
  node_->on_shutdown();
  restarts_.fetch_add(1, std::memory_order_relaxed);
  reset_counters();
  set_state(node_->on_init() ? NodeState::Running : NodeState::Faulted);
  return true;
}

// Non-blocking stop; see try_restart for why it never waits on the node lock.
bool NodeRunner::try_stop() {
  std::unique_lock<std::mutex> lk(mu_, std::try_to_lock);
  if (!lk) {  // stuck in on_step: at least stop further releases
    if (state() != NodeState::Faulted) set_state(NodeState::Stopped);
    return false;
  }
  if (state() == NodeState::Running || state() == NodeState::Initialized) node_->on_shutdown();
  if (state() != NodeState::Faulted) set_state(NodeState::Stopped);
  return true;
}

// Executes one release: times on_step, updates stats, and decides whether the
// consecutive miss/failure counters just crossed fault_threshold. The fault is
// reported only on the crossing (==), so one burst raises one fault.
NodeRunner::JobResult NodeRunner::run_job(IClock& clock, TimeUs release_us, std::uint64_t cycle) {
  JobResult r;
  std::lock_guard<std::mutex> lk(mu_);
  if (state() != NodeState::Running) return r;
  const TimeUs start = clock.now_us();
  const bool ok = node_->on_step(StepContext{start, release_us, cycle});
  const TimeUs end = clock.now_us();
  const TimeUs exec = end - start;

  r.ran = true;
  r.end_us = end;
  cycles_.fetch_add(1, std::memory_order_relaxed);
  total_exec_.fetch_add(exec, std::memory_order_relaxed);
  store_max(max_exec_, exec);
  store_max(max_latency_, start > release_us ? start - release_us : 0);

  // Deadline is measured from the release instant, not from when the job started.
  if (end > release_us + deadline_us()) {
    misses_.fetch_add(1, std::memory_order_relaxed);
    r.report_miss = (++consec_miss_ == cfg_.fault_threshold);
  } else {
    consec_miss_ = 0;
  }
  if (!ok) {
    failures_.fetch_add(1, std::memory_order_relaxed);
    r.report_failure = (++consec_fail_ == cfg_.fault_threshold);
  } else {
    consec_fail_ = 0;
  }
  return r;
}

// Glue shared by both executors: run the job, beat the heartbeat, then raise
// any fault outside the node lock.
bool execute_job(NodeRunner& r, IClock& clock, Watchdog& wd, TimeUs release_us, std::uint64_t cycle) {
  const auto res = r.run_job(clock, release_us, cycle);
  if (!res.ran) return false;
  wd.heartbeats().beat(r.id(), res.end_us);
  if (res.report_miss) wd.report(r.id(), FaultKind::DeadlineMiss);
  if (res.report_failure) wd.report(r.id(), FaultKind::StepFailure);
  return true;
}

}  // namespace rtsdk
