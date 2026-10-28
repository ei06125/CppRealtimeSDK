#include "rtsdk/heartbeat.hpp"

namespace rtsdk {

// Registration happens before threads start (vector is not resized later).
std::size_t HeartbeatMonitor::add(TimeUs timeout_us) {
  entries_.emplace_back(timeout_us);
  return entries_.size() - 1;
}

// Arms monitoring and clears any previous expiry latch.
void HeartbeatMonitor::enable(std::size_t id, TimeUs now) {
  auto& e = entries_[id];
  e.last_beat.store(now, std::memory_order_relaxed);
  e.latched.store(false, std::memory_order_relaxed);
  e.enabled.store(true, std::memory_order_release);
}

void HeartbeatMonitor::disable(std::size_t id) {
  entries_[id].enabled.store(false, std::memory_order_release);
}

// Hot path: called after every job, two relaxed stores, no locks.
void HeartbeatMonitor::beat(std::size_t id, TimeUs now) {
  auto& e = entries_[id];
  e.last_beat.store(now, std::memory_order_relaxed);
  e.latched.store(false, std::memory_order_relaxed);
}

bool HeartbeatMonitor::enabled(std::size_t id) const {
  return entries_[id].enabled.load(std::memory_order_acquire);
}

TimeUs HeartbeatMonitor::age(std::size_t id, TimeUs now) const {
  TimeUs last = entries_[id].last_beat.load(std::memory_order_relaxed);
  return now > last ? now - last : 0;
}

bool HeartbeatMonitor::expired(std::size_t id, TimeUs now) const {
  const auto& e = entries_[id];
  return e.enabled.load(std::memory_order_acquire) && age(id, now) > e.timeout;
}

// True only for the first caller after an expiry, so each outage is reported
// once.
bool HeartbeatMonitor::latch_expiry(std::size_t id) {
  return !entries_[id].latched.exchange(true, std::memory_order_acq_rel);
}

}  // namespace rtsdk
