// Count ordinary operator-new calls after configuration during repeated
// dispatch and mailbox use. Exclude setup and management snapshots so the
// assertion describes only the tested normal execution path.
#include <atomic>
#include <cstdlib>
#include <iostream>
#include <new>

#include "rtx/mailbox.hpp"
#include "rtx/scheduler.hpp"
using namespace std::chrono_literals;
namespace {
std::atomic<unsigned long> allocations = 0;
}
void* operator new(std::size_t size) {
  allocations.fetch_add(1, std::memory_order_relaxed);
  if (void* memory = std::malloc(size ? size : 1)) return memory;
  throw std::bad_alloc();
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete[](void* memory) noexcept { std::free(memory); }
void operator delete(void* memory, std::size_t) noexcept { std::free(memory); }
void operator delete[](void* memory, std::size_t) noexcept {
  std::free(memory);
}
int main() {
  std::uint64_t calls = 0;
  rtx::Mailbox<8> channel;
  std::array<std::byte, 8> payload{};
  rtx::Scheduler scheduler({{"producer",
                             1,
                             1ms,
                             1ms,
                             10ms,
                             {},
                             [&](const rtx::Context& context) {
                               ++calls;
                               context.heartbeat();
                               channel.write(payload);
                             }},
                            {"consumer",
                             2,
                             1ms,
                             1ms,
                             10ms,
                             {"producer"},
                             [&](const rtx::Context& context) {
                               ++calls;
                               context.heartbeat();
                               channel.read(payload);
                             }}});
  rtx::Time now = 0ns;
  const std::function<rtx::Time()> clock = [&] { return now; };
  scheduler.start(now);
  const auto before = allocations.load();
  for (unsigned i = 0; i < 10000; ++i) {
    now = i * 1ms;
    scheduler.step(now, clock);
    scheduler.inspect_watchdogs(now);
  }
  scheduler.stop();
  const auto count = allocations.load() - before;
  if (calls != 20000 || count != 0) {
    std::cerr << "dispatch calls=" << calls << ", allocations=" << count
              << '\n';
    return 1;
  }
  std::cout << "20000 callback/heartbeat dispatches without operator new "
               "allocations\n";
}
