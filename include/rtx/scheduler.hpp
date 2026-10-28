// OS-independent node configuration, callback context and observable execution
// health. Backend runners share this state model; they supply clocks and
// scheduling mechanisms.
#pragma once
#include <chrono>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "rtx/runtime_mutex.hpp"

namespace rtx {
using Duration = std::chrono::nanoseconds;
using Time = Duration;  // Monotonic time since the backend's epoch.
enum class State { configured, running, stopped, faulted };
struct Context {
  Time scheduled;
  Time now;
  std::uint64_t tick;
  // Invoke only during execute; this callable borrows the active dispatch.
  std::function<void()> heartbeat;
};
struct Node {
  std::string name;
  int priority =
      0;  // Cooperative releases: larger first, configuration order for ties.
  Duration period{};
  Duration deadline{};
  Duration watchdog{};
  std::vector<std::string> after;
  std::function<void(const Context&)> execute;
};
struct Health {
  std::string name;
  State state = State::configured;
  std::uint64_t ticks = 0, deadline_misses = 0, skipped_releases = 0;
  Time heartbeat{}, next_release{};
  std::string fault;
};
// Single dispatcher. Callbacks execute without the state lock; health and stop
// are concurrent-safe. step/start must be serialized by the owner. Dependencies
// govern launch order, not data flow.
class Scheduler {
 public:
  explicit Scheduler(std::vector<Node> nodes);
  void start(Time now);
  void stop();
  void step(Time now, const std::function<Time()>& clock);
  void inspect_watchdogs(Time now);
  std::vector<Health> health() const;

 private:
  friend class PosixTaskRunner;
  friend class FreeRtosTaskRunner;
  void step_node(std::size_t index, Time now,
                 const std::function<Time()>& clock);
  struct Entry {
    Node node;
    Health health;
    Time last_dispatch{};
    std::size_t configuration_index;
  };
  mutable RuntimeMutex mutex_;
  std::vector<Entry> entries_;
  std::vector<std::size_t> ready_;
  bool running_ = false;
};
}  // namespace rtx
