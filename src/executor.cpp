#include "rtsdk/executor.hpp"

#include <algorithm>

namespace rtsdk {

// ----------------------------------------------------------------
// TickScheduler

TickScheduler::TickScheduler(IPlatform& platform, Watchdog& wd, TimeUs tick_us)
    : platform_(platform), wd_(wd), tick_us_(tick_us) {}

void TickScheduler::add(NodeRunner& r, TimeUs first_release_us) {
  slots_.push_back(Slot{&r, first_release_us});
}

// Moves every slot whose period elapsed into "pending". A slot still pending
// from the previous period means the node overran: that release is counted as
// skipped instead of queued, which bounds backlog.
void TickScheduler::release_due(TimeUs now) {
  for (auto& s : slots_) {
    if (now < s.next_release) continue;
    if (s.runner->state() != NodeState::Running) {
      s.next_release =
          now + s.runner->config().period_us;  // no burst after (re)start
      continue;
    }
    if (s.pending) {
      s.runner->note_skipped_release();
    } else {
      s.pending = true;
      s.pending_release = s.next_release;
    }
    const TimeUs period = s.runner->config().period_us;
    s.next_release += period;
    while (
        now >=
        s.next_release) {  // starved for whole periods: those releases are lost
      s.runner->note_skipped_release();
      s.next_release += period;
    }
  }
}

// Fixed-priority choice: highest priority wins, earliest release breaks ties.
TickScheduler::Slot* TickScheduler::pick() {
  Slot* best = nullptr;
  for (auto& s : slots_) {
    if (!s.pending) continue;
    if (!best) {
      best = &s;
      continue;
    }
    const auto pa = s.runner->config().priority,
               pb = best->runner->config().priority;
    if (pa > pb || (pa == pb && s.pending_release < best->pending_release))
      best = &s;
  }
  return best;
}

// One scheduling quantum. Jobs run to completion (cooperative), so preemption
// happens at job boundaries: after each job the highest-priority pending job is
// re-picked. At least one job runs per tick; the budget stops further work.
void TickScheduler::tick() {
  IClock& clk = platform_.clock();
  const TimeUs budget_end = clk.now_us() + tick_us_;
  bool ran_any = false;
  for (;;) {
    const TimeUs now = clk.now_us();
    release_due(now);
    Slot* s = pick();
    if (!s || (ran_any && now >= budget_end)) break;
    s->pending = false;
    execute_job(*s->runner, clk, wd_, s->pending_release, s->cycle++);
    ran_any = true;
  }
  wd_.check();  // heartbeats are evaluated once per tick
  ticks_.fetch_add(1, std::memory_order_relaxed);
}

// Optional driver thread; tests instead call tick() directly with a
// ManualClock.
void TickScheduler::start() {
  stop_.store(false);
  ThreadAttr attr;
  attr.name = "rtsdk-tick";
  attr.priority = 128;
  thread_ = platform_.spawn(attr, [this] {
    IClock& clk = platform_.clock();
    TimeUs next = clk.now_us();
    while (!stop_.load(std::memory_order_acquire)) {
      tick();
      next += tick_us_;
      clk.sleep_until(next);
    }
  });
}

void TickScheduler::stop() {
  stop_.store(true, std::memory_order_release);
  if (thread_) {
    thread_->join();
    thread_.reset();
  }
}

// ---------------------------------------------------------------
// ThreadExecutor

ThreadExecutor::ThreadExecutor(IPlatform& platform, Watchdog& wd,
                               TimeUs supervisor_period_us,
                               Priority supervisor_priority)
    : platform_(platform),
      wd_(wd),
      supervisor_period_(supervisor_period_us),
      supervisor_priority_(supervisor_priority) {}

void ThreadExecutor::add(NodeRunner& r, TimeUs first_release_us) {
  nodes_.emplace_back(&r, first_release_us);
}

// Per-node thread body: absolute-time periodic loop (no drift accumulation).
void ThreadExecutor::node_loop(NodeRunner& r, TimeUs next) {
  IClock& clk = platform_.clock();
  const TimeUs period = r.config().period_us;
  constexpr TimeUs kStopPoll =
      10 * kMs;  // bounds shutdown latency for slow nodes
  std::uint64_t cycle = 0;
  while (!stop_.load(std::memory_order_acquire)) {
    TimeUs now = clk.now_us();
    if (now < next) {
      clk.sleep_until(std::min(next, now + kStopPoll));
      continue;
    }
    if (r.state() != NodeState::Running) {  // restarting/stopped: idle without
                                            // bursting later
      next = now + period;
      continue;
    }
    execute_job(r, clk, wd_, next, cycle++);
    next += period;
    now = clk.now_us();
    while (now >= next + period) {  // lost releases
      r.note_skipped_release();
      next += period;
    }
  }
}

// Watchdog thread, runs independently so a hung node cannot silence it.
void ThreadExecutor::supervisor_loop() {
  IClock& clk = platform_.clock();
  TimeUs next = clk.now_us();
  while (!stop_.load(std::memory_order_acquire)) {
    wd_.check();
    next += supervisor_period_;
    clk.sleep_until(next);
  }
}

// Preemption is delegated to the OS: each node thread gets its node priority.
void ThreadExecutor::start() {
  stop_.store(false);
  for (auto& n : nodes_) {
    ThreadAttr attr;
    attr.name = n.first->config().name.c_str();
    attr.priority = n.first->config().priority;
    NodeRunner* r = n.first;
    const TimeUs first = n.second;
    threads_.push_back(
        platform_.spawn(attr, [this, r, first] { node_loop(*r, first); }));
  }
  ThreadAttr sup;
  sup.name = "rtsdk-wd";
  sup.priority = supervisor_priority_;
  threads_.push_back(platform_.spawn(sup, [this] { supervisor_loop(); }));
}

void ThreadExecutor::stop() {
  stop_.store(true, std::memory_order_release);
  for (auto& t : threads_) t->join();
  threads_.clear();
}

}  // namespace rtsdk
