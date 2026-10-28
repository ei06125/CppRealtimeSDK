// Shared-memory communication tests: SPSC ring and seqlock mailbox,
// including cross-thread and cross-process (fork) use.
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <thread>

#include "rt_test.hpp"
#include "rtsdk/comms.hpp"

using namespace rtsdk;

// FIFO order; push fails when full or oversized, pop fails when empty.
TEST(ring_fifo_full_empty_and_oversize) {
  HeapRegion mem(SpscRing::required_bytes(4, 16));
  SpscRing r = SpscRing::create(mem.data(), 4, 16);
  unsigned char buf[16];
  std::uint32_t len = 0;
  CHECK(!r.try_pop(buf, &len));
  for (std::uint32_t i = 0; i < 4; ++i) CHECK(r.try_push(&i, sizeof(i)));
  std::uint32_t extra = 99;
  CHECK(!r.try_push(&extra, sizeof(extra)));  // full
  CHECK(!r.try_push(buf, 17));                // too large
  for (std::uint32_t i = 0; i < 4; ++i) {
    CHECK(r.try_pop(buf, &len));
    std::uint32_t v;
    std::memcpy(&v, buf, 4);
    CHECK(len == 4 && v == i);
  }
  CHECK(!r.try_pop(buf, &len));
  // wrap-around after many cycles
  for (std::uint32_t i = 0; i < 1000; ++i) {
    CHECK(r.try_push(&i, 4));
    CHECK(r.try_pop(buf, &len));
  }
}

// One producer thread and one consumer thread see messages in order, none lost.
TEST(ring_spsc_threads_preserve_order) {
  constexpr std::uint32_t kN = 200000;
  HeapRegion mem(SpscRing::required_bytes(256, 8));
  SpscRing ring = SpscRing::create(mem.data(), 256, 8);
  std::thread prod([&] {
    for (std::uint32_t i = 0; i < kN;) {
      std::uint64_t v = i;
      if (ring.try_push(&v, sizeof(v))) ++i;
    }
  });
  std::uint64_t expect = 0;
  bool ordered = true;
  while (expect < kN) {
    std::uint64_t v;
    std::uint32_t len;
    if (ring.try_pop(&v, &len)) {
      ordered &= (v == expect);
      ++expect;
    }
  }
  prod.join();
  CHECK(ordered);
}

namespace {
struct Pose {
  std::uint64_t a, b, c, d;  // invariant: all equal
};
}  // namespace

// A reader never observes a half-written value, only whole snapshots.
TEST(mailbox_latest_value_never_torn) {
  HeapRegion mem(Mailbox<Pose>::required_bytes());
  auto mb = Mailbox<Pose>::create(mem.data());
  Pose p{};
  CHECK(!mb.read(p));
  std::atomic<bool> stop{false};
  std::thread w([&] {
    for (std::uint64_t i = 1; !stop.load(); ++i) mb.write(Pose{i, i, i, i});
  });
  bool torn = false;
  std::uint64_t last = 0;
  bool monotonic = true;
  for (int i = 0; i < 200000; ++i) {
    if (mb.read(p)) {
      torn |= !(p.a == p.b && p.b == p.c && p.c == p.d);
      monotonic &= (p.a >= last);
      last = p.a;
    }
  }
  stop = true;
  w.join();
  CHECK(!torn);
  CHECK(monotonic);
  CHECK(last > 0);
}

// Two processes exchange messages through real POSIX shared memory.
TEST(shm_ring_between_processes) {
  const std::string name = "/rtsdk_test_" + std::to_string(getpid());
  const std::size_t bytes = SpscRing::required_bytes(64, 32);
  std::string err;
  auto shm = PosixShm::open(name, bytes, true, &err);
  if (!shm) {
    std::printf("    SKIP shm unavailable: %s\n", err.c_str());
    return;
  }
  SpscRing tx = SpscRing::create(shm->data(), 64, 32);
  const pid_t pid = fork();
  if (pid == 0) {  // child: attach by name and consume 1000 messages
    auto cs = PosixShm::open(name, bytes, false);
    bool ok = false;
    SpscRing rx = SpscRing::attach(cs->data(), &ok);
    int status = ok ? 0 : 2;
    for (std::uint32_t expect = 0; ok && expect < 1000;) {
      std::uint32_t v, len;
      if (rx.try_pop(&v, &len)) {
        if (v != expect) status = 3;
        ++expect;
      }
    }
    _exit(status);
  }
  for (std::uint32_t i = 0; i < 1000;) {
    if (tx.try_push(&i, sizeof(i))) ++i;
  }
  int st = 0;
  waitpid(pid, &st, 0);
  PosixShm::unlink(name);
  CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0);
}

// attach() refuses memory that is not an initialised ring.
TEST(ring_attach_rejects_garbage) {
  HeapRegion mem(256);
  bool ok = true;
  SpscRing::attach(mem.data(), &ok);
  CHECK(!ok);
}
