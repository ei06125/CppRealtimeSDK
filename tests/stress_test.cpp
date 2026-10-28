// Run multiple producer/consumer workers, mapped channels and concurrent health
// readers. Check integrity and monotonic progress without treating host
// scheduling jitter as a functional failure.
#include <unistd.h>

#include <array>
#include <atomic>
#include <charconv>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <thread>

#include "rtx/posix.hpp"
#include "rtx/shared_memory.hpp"
using namespace std::chrono_literals;
namespace {
constexpr std::size_t pairs = 4, nodes = 2 * pairs;
struct Channels {
  std::array<std::string, pairs> names;
  std::array<std::unique_ptr<rtx::SharedMemory>, pairs> writers, readers;
  ~Channels() {
    for (const auto& name : names)
      if (!name.empty()) rtx::SharedMemory::unlink(name);
  }
};
}  // namespace
int main() {
  try {
    unsigned milliseconds = 500;
    if (const char* value = std::getenv("RTX_STRESS_MS")) {
      const auto end = value + std::strlen(value);
      const auto result = std::from_chars(value, end, milliseconds);
      if (result.ec != std::errc{} || result.ptr != end || milliseconds < 100 ||
          milliseconds > 60000)
        throw std::invalid_argument("RTX_STRESS_MS must be 100..60000");
    }
    Channels channels;
    for (std::size_t i = 0; i < pairs; ++i) {
      channels.names[i] =
          "/rtx-stress-" + std::to_string(getpid()) + "-" + std::to_string(i);
      channels.writers[i] =
          std::make_unique<rtx::SharedMemory>(channels.names[i], 16, true);
      channels.readers[i] =
          std::make_unique<rtx::SharedMemory>(channels.names[i], 16, false);
    }
    std::array<std::atomic<bool>, nodes> active{};
    std::array<std::uint64_t, nodes> calls{};
    std::array<std::uint64_t, pairs> received{}, last_sequence{};
    std::atomic<bool> invalid = false;
    std::vector<rtx::Node> config;
    for (std::size_t index = 0; index < nodes; ++index) {
      const auto pair = index / 2;
      const auto producer = "producer" + std::to_string(pair);
      config.push_back(
          {index % 2 ? "consumer" + std::to_string(pair) : producer,
           static_cast<int>(index + 1), 1ms, 1s, 0ns,
           index % 2 ? std::vector<std::string>{producer}
                     : std::vector<std::string>{},
           [&, index, pair](const rtx::Context& context) {
             if (active[index].exchange(true)) invalid = true;
             if (context.tick != ++calls[index]) invalid = true;
             if (index % 2 == 0) {
               const std::array<std::uint64_t, 2> message{context.tick,
                                                          ~context.tick};
               channels.writers[pair]->write(std::as_bytes(std::span(message)));
             } else {
               std::array<std::uint64_t, 2> message{};
               if (channels.readers[pair]->read(std::as_writable_bytes(
                       std::span(message))) != sizeof(message) ||
                   message[1] != ~message[0] ||
                   message[0] < last_sequence[pair] || message[0] == 0)
                 invalid = true;
               last_sequence[pair] = message[0];
               ++received[pair];
             }
             context.heartbeat();
             active[index] = false;
           }});
    }
    rtx::Scheduler scheduler(std::move(config));
    rtx::PosixTaskRunner runner(scheduler, false);
    std::atomic<bool> done = false;
    std::array<std::uint64_t, 2> snapshots{};
    std::array<std::jthread, 2> observers;
    for (std::size_t reader = 0; reader < observers.size(); ++reader)
      observers[reader] = std::jthread([&, reader] {
        std::array<std::uint64_t, nodes> ticks{};
        std::array<rtx::Time, nodes> heartbeats{};
        while (!done) {
          const auto health = scheduler.health();
          for (std::size_t i = 0; i < nodes; ++i) {
            if (health[i].ticks < ticks[i] ||
                health[i].heartbeat < heartbeats[i] ||
                health[i].state == rtx::State::faulted)
              invalid = true;
            ticks[i] = health[i].ticks;
            heartbeats[i] = health[i].heartbeat;
          }
          ++snapshots[reader];
          std::this_thread::sleep_for(100us);
        }
      });
    std::exception_ptr failure;
    std::jthread execution([&] {
      try {
        runner.run();
      } catch (...) {
        failure = std::current_exception();
      }
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(milliseconds));
    runner.request_stop();
    execution.join();
    done = true;
    for (auto& observer : observers) observer.join();
    if (failure) std::rethrow_exception(failure);
    const auto health = scheduler.health();
    for (std::size_t i = 0; i < nodes; ++i)
      if (calls[i] < 2 || health[i].ticks != calls[i] ||
          health[i].state != rtx::State::stopped || active[i])
        invalid = true;
    for (auto count : received)
      if (count < 2) invalid = true;
    for (auto count : snapshots)
      if (count < 2) invalid = true;
    if (invalid)
      throw std::runtime_error(
          "execution, message integrity, or health invariant failed");
    std::cout << "8 nodes, 4 mapped mailboxes, 2 health readers passed for "
              << milliseconds << " ms\n";
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
