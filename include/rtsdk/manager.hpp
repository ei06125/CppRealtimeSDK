// ExecutionManager: owns nodes, applies a LaunchConfig, wires the executor to
// the watchdog and applies per-node fault policies. Its status/command surface
// is what the gRPC control plane (or any other IPC front-end) exposes.
#pragma once

#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "rtsdk/executor.hpp"
#include "rtsdk/launch.hpp"

namespace rtsdk {

using NodeFactory = std::function<std::unique_ptr<Node>(const NodeConfig&)>;

enum class ManagerState : std::uint8_t { Idle, Running, SafeState, Stopped };
const char* to_string(ManagerState s);

struct NodeStatus {
  std::string name;
  NodeState state = NodeState::Created;
  Priority priority = 0;
  TimeUs period_us = 0;
  std::uint32_t restarts = 0;
  NodeStats stats;
};

struct Event {
  std::uint64_t seq = 0;
  TimeUs at_us = 0;
  std::string kind;  // launch | fault | action | state | command
  std::string node;  // empty for manager-wide events
  std::string detail;
};

class ExecutionManager {
 public:
  enum class Mode { Tick, Threads };

  ExecutionManager(IPlatform& platform, Mode mode, IHwWatchdog* hw = nullptr,
                   TimeUs tick_us = kMs);
  ~ExecutionManager();

  void register_factory(const std::string& type, NodeFactory f);
  void set_safe_state_hook(std::function<void()> hook) {
    safe_hook_ = std::move(hook);
  }

  // Builds the nodes described by `cfg` (does not run anything).
  bool configure(const LaunchConfig& cfg, std::string& error);
  // Runs the launch sequence. With run_executor=false in Tick mode the caller
  // drives tick_scheduler()->tick() itself (deterministic tests/simulation).
  bool start(std::string& error, bool run_executor = true);
  void stop();

  ManagerState state() const;
  std::vector<NodeStatus> nodes() const;
  bool node(const std::string& name, NodeStatus& out) const;
  bool restart_node(const std::string& name);
  bool stop_node(const std::string& name);
  std::vector<Event> events_since(
      std::uint64_t seq) const;  // events with seq > given
  TickScheduler* tick_scheduler() { return tick_.get(); }
  Watchdog& watchdog() { return wd_; }

 private:
  void on_fault(const Fault& f);
  void enter_safe_state(const std::string& reason);
  void log(const char* kind, const std::string& node,
           const std::string& detail);
  NodeRunner* find(const std::string& name) const;

  IPlatform& platform_;
  Mode mode_;
  TimeUs tick_us_;
  HeartbeatMonitor hb_;
  Watchdog wd_;
  std::map<std::string, NodeFactory> factories_;
  std::function<void()> safe_hook_;
  LaunchConfig cfg_;
  std::vector<std::unique_ptr<NodeRunner>> runners_;
  std::unique_ptr<TickScheduler> tick_;
  std::unique_ptr<ThreadExecutor> threads_;
  IExecutor* exec_ = nullptr;
  bool executor_running_ = false;

  mutable std::mutex mu_;  // guards state_, events_, fault handling
  ManagerState state_ = ManagerState::Idle;
  std::deque<Event> events_;
  std::uint64_t next_seq_ = 1;
};

}  // namespace rtsdk
