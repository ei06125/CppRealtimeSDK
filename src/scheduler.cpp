// Validate launch dependencies and maintain release, heartbeat and fault state.
// Callbacks run outside the state lock so supervision and health inspection
// remain independent.
#include "rtx/scheduler.hpp"

#include <algorithm>
#include <stdexcept>
#include <unordered_map>

#include "time_math.hpp"

namespace rtx {
Scheduler::Scheduler(std::vector<Node> nodes) {
  std::unordered_map<std::string, std::size_t> ids;
  for (std::size_t i = 0; i < nodes.size(); ++i) {
    const auto& n = nodes[i];
    if (n.name.empty() || !ids.emplace(n.name, i).second || !n.execute ||
        n.period <= Duration::zero() || n.deadline <= Duration::zero() ||
        n.watchdog < Duration::zero())
      throw std::invalid_argument("invalid or duplicate node");
  }
  std::vector<bool> added(nodes.size());
  while (entries_.size() < nodes.size()) {
    bool progress = false;
    for (std::size_t i = 0; i < nodes.size(); ++i) {
      if (added[i]) continue;
      bool ready = true;
      for (const auto& dep : nodes[i].after) {
        auto it = ids.find(dep);
        if (it == ids.end())
          throw std::invalid_argument("unknown launch dependency: " + dep);
        ready = ready && added[it->second];
      }
      if (ready) {
        Health h;
        h.name = nodes[i].name;
        entries_.push_back({nodes[i], h, {}, i});
        added[i] = true;
        progress = true;
      }
    }
    if (!progress) throw std::invalid_argument("cyclic launch dependencies");
  }
  ready_.reserve(entries_.size());
}
void Scheduler::start(Time now) {
  detail::valid_time(now);
  std::lock_guard lock(mutex_);
  if (running_) throw std::logic_error("already running");
  running_ = true;
  for (auto& e : entries_) {
    e.health = Health{};
    e.health.name = e.node.name;
    e.health.state = State::running;
    e.health.heartbeat = now;
    e.health.next_release = now;
    e.last_dispatch = now;
  }
}
void Scheduler::stop() {
  std::lock_guard lock(mutex_);
  running_ = false;
  for (auto& e : entries_)
    if (e.health.state == State::running) e.health.state = State::stopped;
}
void Scheduler::inspect_watchdogs(Time now) {
  detail::valid_time(now);
  std::lock_guard lock(mutex_);
  for (auto& e : entries_)
    if (e.health.state == State::running && e.health.ticks > 0 &&
        e.node.watchdog > Duration::zero() &&
        now - e.health.heartbeat > e.node.watchdog) {
      e.health.state = State::faulted;
      e.health.fault = "heartbeat timeout";
    }
}
std::vector<Health> Scheduler::health() const {
  std::lock_guard lock(mutex_);
  std::vector<Health> out;
  for (const auto& e : entries_) out.push_back(e.health);
  return out;
}
void Scheduler::step(Time now, const std::function<Time()>& clock) {
  detail::valid_time(now);
  auto& ready = ready_;
  ready.clear();
  {
    std::lock_guard lock(mutex_);
    if (!running_) return;
    for (const auto& e : entries_)
      if (now < e.last_dispatch)
        throw std::invalid_argument("dispatch clock regressed");
    for (auto& e : entries_) e.last_dispatch = now;
    for (std::size_t i = 0; i < entries_.size(); ++i)
      if (entries_[i].health.state == State::running &&
          entries_[i].health.next_release <= now)
        ready.push_back(i);
    // Initial invocations follow the validated launch sequence. Subsequent
    // releases use priority.
    const bool launching = std::any_of(ready.begin(), ready.end(), [&](auto i) {
      return entries_[i].health.ticks == 0;
    });
    std::sort(ready.begin(), ready.end(), [&](auto a, auto b) {
      if (launching) return a < b;
      if (entries_[a].node.priority == entries_[b].node.priority)
        return entries_[a].configuration_index <
               entries_[b].configuration_index;
      return entries_[a].node.priority > entries_[b].node.priority;
    });
  }
  for (auto i : ready) {
    step_node(i, now, clock);
    // A preceding callback may have consumed substantial time. Use a fresh
    // dispatch timestamp for the next callback and its first watchdog interval.
    now = clock();
  }
}
void Scheduler::step_node(std::size_t i, Time now,
                          const std::function<Time()>& clock) {
  detail::valid_time(now);
  Time release;
  std::uint64_t tick;
  {
    std::lock_guard lock(mutex_);
    auto& e = entries_[i];
    if (!running_ || e.health.state != State::running) return;
    if (now < e.last_dispatch)
      throw std::invalid_argument("dispatch clock regressed");
    e.last_dispatch = now;
    if (e.health.next_release > now) return;
    if (e.health.ticks == 0) {
      for (const auto& dependency : e.node.after) {
        const auto predecessor = std::find_if(
            entries_.begin(), entries_.end(), [&](const auto& candidate) {
              return candidate.node.name == dependency;
            });
        if (predecessor->health.state == State::faulted) {
          e.health.state = State::faulted;
          e.health.fault = "launch dependency fault: " + dependency;
          return;
        }
        if (predecessor->health.ticks == 0) return;
      }
    }
    release = e.health.next_release;
    auto elapsed = (now - release) / e.node.period;
    try {
      e.health.next_release = detail::next_release(release, now, e.node.period);
    } catch (const std::overflow_error& ex) {
      e.health.state = State::faulted;
      e.health.fault = ex.what();
      return;
    }
    detail::increment(e.health.skipped_releases,
                      static_cast<std::uint64_t>(elapsed));
    if (e.health.ticks == 0) e.health.heartbeat = now;
    detail::increment(e.health.ticks);
    tick = e.health.ticks;
  }
  std::string error;
  bool callback_failed = false;
  auto heartbeat = [this, i, now, &clock] {
    const auto t = clock();
    detail::valid_time(t);
    std::lock_guard lock(mutex_);
    if (t < now || t < entries_[i].health.heartbeat)
      throw std::invalid_argument("heartbeat clock regressed");
    entries_[i].health.heartbeat = t;
  };
  // std::function construction from reference_wrapper does not allocate. The
  // callable and its borrowed clock remain alive through this callback.
  try {
    entries_[i].node.execute(Context{release, now, tick, std::ref(heartbeat)});
  } catch (const std::exception& ex) {
    callback_failed = true;
    error = ex.what();
  } catch (...) {
    callback_failed = true;
    error = "unknown callback exception";
  }
  const auto finish = clock();
  std::lock_guard lock(mutex_);
  auto& e = entries_[i];
  if (finish < now || finish < e.health.heartbeat) {
    if (e.health.state != State::faulted) {
      e.health.state = State::faulted;
      e.health.fault = "completion clock regressed";
    }
    return;
  }
  if (finish - release > e.node.deadline)
    detail::increment(e.health.deadline_misses);
  if (callback_failed && e.health.state != State::faulted) {
    e.health.state = State::faulted;
    e.health.fault =
        error.empty() ? "callback exception (empty message)" : error;
  }
}
}  // namespace rtx
