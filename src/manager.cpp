#include "rtsdk/manager.hpp"

#include <algorithm>

namespace rtsdk {

// Human-readable manager state, used in logs and the gRPC status reply.
const char* to_string(ManagerState s) {
  switch (s) {
    case ManagerState::Idle:
      return "idle";
    case ManagerState::Running:
      return "running";
    case ManagerState::SafeState:
      return "safe-state";
    case ManagerState::Stopped:
      return "stopped";
  }
  return "?";
}

namespace {
// Event log is a bounded ring: oldest entries are dropped so memory stays
// fixed.
constexpr std::size_t kMaxEvents = 1024;
}  // namespace

ExecutionManager::ExecutionManager(IPlatform& platform, Mode mode,
                                   IHwWatchdog* hw, TimeUs tick_us)
    : platform_(platform),
      mode_(mode),
      tick_us_(tick_us),
      wd_(platform.clock(), hb_, hw) {
  // All watchdog faults (heartbeat, deadline, step failure) funnel into one
  // policy handler.
  wd_.set_handler([this](const Fault& f) { on_fault(f); });
}

ExecutionManager::~ExecutionManager() { stop(); }

void ExecutionManager::register_factory(const std::string& type,
                                        NodeFactory f) {
  factories_[type] = std::move(f);
}

void ExecutionManager::log(const char* kind, const std::string& node,
                           const std::string& detail) {
  // caller holds mu_ or is single-threaded setup
  events_.push_back(
      Event{next_seq_++, platform_.clock().now_us(), kind, node, detail});
  if (events_.size() > kMaxEvents) events_.pop_front();
}

NodeRunner* ExecutionManager::find(const std::string& name) const {
  for (const auto& r : runners_)
    if (r->config().name == name) return r.get();
  return nullptr;
}

// Validates the launch config (ordering, factories) and instantiates all nodes.
// Nothing runs yet; the heartbeat slot of each node is registered here so that
// the runner id equals its heartbeat index.
bool ExecutionManager::configure(const LaunchConfig& cfg, std::string& error) {
  // Fail early on cycles/unknown nodes, and on node types nobody can build.
  std::vector<std::size_t> order;
  if (!cfg.resolve_order(order, error)) return false;
  for (const auto& spec : cfg.nodes) {
    auto it = factories_.find(spec.type);
    if (it == factories_.end()) {
      error = "no factory registered for type '" + spec.type + "'";
      return false;
    }
  }
  cfg_ = cfg;
  runners_.clear();
  for (const auto& spec : cfg_.nodes) {
    NodeConfig nc = spec.config;
    // Default liveness budget: three missed periods.
    if (nc.heartbeat_timeout_us == 0)
      nc.heartbeat_timeout_us = 3 * nc.period_us;
    hb_.add(nc.heartbeat_timeout_us);
    auto node = factories_[spec.type](nc);
    if (!node) {
      error = "factory for '" + spec.type + "' returned null";
      return false;
    }
    runners_.push_back(
        std::make_unique<NodeRunner>(runners_.size(), nc, std::move(node)));
  }
  return true;
}

// Brings nodes up in dependency order. A node whose dependency failed to
// initialise is skipped; a node whose own on_init fails stays Faulted.
// With run_executor=false the caller drives the TickScheduler by hand (tests).
bool ExecutionManager::start(std::string& error, bool run_executor) {
  std::vector<std::size_t> order;
  if (!cfg_.resolve_order(order, error)) return false;
  {
    std::lock_guard<std::mutex> lk(mu_);
    if (state_ != ManagerState::Idle) {
      error = "manager already started";
      return false;
    }
  }
  // Pick the execution strategy: cooperative ticks or one OS thread per node.
  if (mode_ == Mode::Tick) {
    tick_ = std::make_unique<TickScheduler>(platform_, wd_, tick_us_);
    exec_ = tick_.get();
  } else {
    threads_ = std::make_unique<ThreadExecutor>(platform_, wd_);
    exec_ = threads_.get();
  }

  const TimeUs t0 = platform_.clock().now_us();
  std::map<std::string, TimeUs> up_at;  // node -> first release time
  std::map<std::string, bool> up;
  for (std::size_t idx : order) {
    const LaunchStep& step = cfg_.steps[idx];
    NodeRunner* r = find(step.node);
    // A node is released no earlier than all its predecessors plus its own
    // delay.
    TimeUs ready = t0;
    bool deps_ok = true;
    for (const auto& d : step.after) {
      deps_ok &= up[d];
      ready = std::max(ready, up_at[d]);
    }
    std::lock_guard<std::mutex> lk(mu_);
    if (!deps_ok) {
      r->set_state(NodeState::Stopped);
      log("launch", step.node, "skipped: dependency failed");
      continue;
    }
    if (!r->init()) {
      log("launch", step.node, "on_init failed");
      continue;
    }
    const TimeUs first = ready + step.delay_us;
    up[step.node] = true;
    up_at[step.node] = first;
    hb_.enable(r->id(), first);
    r->start();
    exec_->add(*r, first);
    log("launch", step.node,
        "started, first release +" + std::to_string(first - t0) + "us");
  }
  {
    std::lock_guard<std::mutex> lk(mu_);
    state_ = ManagerState::Running;
    log("state", "", "running");
  }
  if (run_executor) {
    exec_->start();
    executor_running_ = true;
  }
  return true;
}

// Idempotent shutdown: stops the executor first so no job races on_shutdown().
void ExecutionManager::stop() {
  if (executor_running_) {
    exec_->stop();
    executor_running_ = false;
  }
  std::lock_guard<std::mutex> lk(mu_);
  if (state_ == ManagerState::Stopped || state_ == ManagerState::Idle) return;
  for (auto& r : runners_) {
    hb_.disable(r->id());
    r->try_stop();
  }
  if (state_ == ManagerState::Running) state_ = ManagerState::Stopped;
  log("state", "", "stopped");
}

ManagerState ExecutionManager::state() const {
  std::lock_guard<std::mutex> lk(mu_);
  return state_;
}

// Snapshot of one runner built only from atomics, safe while nodes execute.
static NodeStatus status_of(const NodeRunner& r) {
  NodeStatus s;
  s.name = r.config().name;
  s.state = r.state();
  s.priority = r.config().priority;
  s.period_us = r.config().period_us;
  s.restarts = r.restarts();
  s.stats = r.stats();
  return s;
}

std::vector<NodeStatus> ExecutionManager::nodes() const {
  std::vector<NodeStatus> out;
  for (const auto& r : runners_) out.push_back(status_of(*r));
  return out;
}

bool ExecutionManager::node(const std::string& name, NodeStatus& out) const {
  const NodeRunner* r = find(name);
  if (!r) return false;
  out = status_of(*r);
  return true;
}

// Operator-initiated restart (e.g. via gRPC). Fails if the node is mid-step.
bool ExecutionManager::restart_node(const std::string& name) {
  std::lock_guard<std::mutex> lk(mu_);
  NodeRunner* r = find(name);
  if (!r || state_ != ManagerState::Running) return false;
  log("command", name, "restart");
  if (!r->try_restart()) return false;
  hb_.enable(r->id(), platform_.clock().now_us());
  return true;
}

bool ExecutionManager::stop_node(const std::string& name) {
  std::lock_guard<std::mutex> lk(mu_);
  NodeRunner* r = find(name);
  if (!r) return false;
  log("command", name, "stop");
  hb_.disable(r->id());
  return r->try_stop();
}

// Cursor-based event read so streaming clients never miss or repeat entries.
std::vector<Event> ExecutionManager::events_since(std::uint64_t seq) const {
  std::lock_guard<std::mutex> lk(mu_);
  std::vector<Event> out;
  for (const auto& e : events_)
    if (e.seq > seq) out.push_back(e);
  return out;
}

// Fault policy dispatcher, called from the watchdog (supervisor or tick
// thread). mu_ is released before entering safe state because that path takes
// it again.
void ExecutionManager::on_fault(const Fault& f) {
  std::unique_lock<std::mutex> lk(mu_);
  if (state_ != ManagerState::Running) return;
  NodeRunner& r = *runners_[f.node];
  const auto& cfg = r.config();
  log("fault", cfg.name, to_string(f.kind));
  switch (cfg.on_fault) {
    case FaultAction::Ignore:
      return;
    case FaultAction::RestartNode:
      if (r.restarts() >= cfg.max_restarts) {
        lk.unlock();
        enter_safe_state("restart budget exhausted for " + cfg.name);
        return;
      }
      if (!r.try_restart()) {  // stuck inside on_step: cannot recover in place
        lk.unlock();
        enter_safe_state(cfg.name + " unresponsive");
        return;
      }
      hb_.enable(r.id(), platform_.clock().now_us());
      log("action", cfg.name, "restarted");
      return;
    case FaultAction::StopNode:
      hb_.disable(r.id());
      r.try_stop();
      log("action", cfg.name, "stopped");
      return;
    case FaultAction::SafeState:
      lk.unlock();
      enter_safe_state("fault in " + cfg.name);
      return;
  }
}

// Latches the manager into SafeState, halts every node and fires the user hook
// (e.g. to drive actuators to a known-safe output). Runs at most once.
void ExecutionManager::enter_safe_state(const std::string& reason) {
  {
    std::lock_guard<std::mutex> lk(mu_);
    if (state_ != ManagerState::Running) return;
    state_ = ManagerState::SafeState;
    log("state", "", "safe-state: " + reason);
    for (auto& r : runners_) {
      hb_.disable(r->id());
      r->try_stop();  // never joins: may run on a node's own thread
    }
  }
  if (safe_hook_) safe_hook_();
}

}  // namespace rtsdk
