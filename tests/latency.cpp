// Measure callback-entry lateness and interval jitter using actual monotonic
// releases. Reserve sampling storage before execution and keep timing
// observations separate from realtime guarantees.
#include <sys/utsname.h>
#include <time.h>

#include <algorithm>
#include <charconv>
#include <condition_variable>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <thread>

#include "rtx/posix.hpp"
using namespace std::chrono_literals;
namespace {
struct Sample {
  rtx::Time scheduled, actual;
};
struct Summary {
  std::int64_t minimum, p50, p95, p99, maximum;
};
Summary summarize(std::vector<std::int64_t> values) {
  std::sort(values.begin(), values.end());
  auto percentile = [&](std::size_t p) {
    return values[(values.size() - 1) * p / 100];
  };
  return {values.front(), percentile(50), percentile(95), percentile(99),
          values.back()};
}
void json_summary(const char* name, const Summary& s) {
  std::cout << "  \"" << name << "\": {\"min\":" << s.minimum
            << ",\"p50\":" << s.p50 << ",\"p95\":" << s.p95
            << ",\"p99\":" << s.p99 << ",\"max\":" << s.maximum << "},\n";
}
}  // namespace
int main(int argc, char** argv) {
  try {
    const std::string_view mode = argc > 1 ? argv[1] : "cooperative";
    std::size_t requested = 200;
    if (argc > 3 ||
        (mode != "cooperative" && mode != "workers" && mode != "fifo"))
      throw std::invalid_argument(
          "usage: rtx_latency [cooperative|workers|fifo] [samples:2..100000]");
    if (argc > 2) {
      const std::string_view value = argv[2];
      auto parsed =
          std::from_chars(value.data(), value.data() + value.size(), requested);
      if (parsed.ec != std::errc{} ||
          parsed.ptr != value.data() + value.size() || requested < 2 ||
          requested > 100000)
        throw std::invalid_argument("samples must be between 2 and 100000");
    }
    std::vector<Sample> samples;
    samples.reserve(requested);
    std::function<void()> stop;
    rtx::Scheduler scheduler(
        {{"probe", 10, 1ms, 1ms, 0ns, {}, [&](const rtx::Context& c) {
            const auto actual = rtx::monotonic_now();
            if (samples.size() < requested)
              samples.push_back({c.scheduled, actual});
            c.heartbeat();
            if (samples.size() == requested) stop();
          }}});
    rtx::PosixRunner cooperative(scheduler, 100us);
    rtx::PosixTaskRunner workers(scheduler, mode == "fifo", 100us);
    stop = [&] {
      if (mode == "cooperative")
        cooperative.request_stop();
      else
        workers.request_stop();
    };
    // Bound failures without relying on the measurement callback to make
    // progress.
    std::jthread timeout([&](std::stop_token token) {
      std::mutex mutex;
      std::condition_variable_any wake;
      std::unique_lock lock(mutex);
      wake.wait_for(lock, token, requested * 10ms + 1s, [] { return false; });
      if (!token.stop_requested()) stop();
    });
    const auto begin = rtx::monotonic_now();
    if (mode == "cooperative")
      cooperative.run();
    else
      workers.run();
    const auto end = rtx::monotonic_now();
    timeout.request_stop();
    timeout.join();
    if (samples.size() != requested)
      throw std::runtime_error(
          "measurement timed out before collecting all samples");
    std::vector<std::int64_t> lateness, jitter;
    for (std::size_t i = 0; i < samples.size(); ++i) {
      if (samples[i].actual < samples[i].scheduled)
        throw std::runtime_error("release executed before its scheduled time");
      lateness.push_back((samples[i].actual - samples[i].scheduled).count());
      if (i) {
        const auto interval_error =
            (samples[i].actual - samples[i - 1].actual - 1ms).count();
        jitter.push_back(interval_error < 0 ? -interval_error : interval_error);
      }
    }
    const auto h = scheduler.health()[0];
    struct utsname platform {};
    if (uname(&platform) != 0) throw std::runtime_error("uname failed");
    timespec resolution{};
    if (clock_getres(CLOCK_MONOTONIC, &resolution) != 0)
      throw std::runtime_error("clock_getres failed");
    std::cout << "{\n  \"mode\":\"" << mode << "\",\n  \"architecture\":\""
              << platform.machine << "\",\n  \"samples\":" << samples.size()
              << ",\n  \"period_ns\":1000000,\n  "
                 "\"poll_resolution_ns\":100000,\n  \"clock_resolution_ns\":"
              << resolution.tv_sec * 1000000000LL + resolution.tv_nsec
              << ",\n  \"elapsed_ns\":" << (end - begin).count() << ",\n";
    json_summary("release_lateness_ns", summarize(std::move(lateness)));
    json_summary("absolute_interval_jitter_ns", summarize(std::move(jitter)));
    std::cout << "  \"deadline_misses\":" << h.deadline_misses
              << ",\n  \"skipped_releases\":" << h.skipped_releases << "\n}\n";
  } catch (const std::exception& ex) {
    std::cerr << ex.what() << '\n';
    return 1;
  }
}
