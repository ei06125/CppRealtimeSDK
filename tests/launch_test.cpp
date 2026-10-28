// Check launch parsing, callback binding and dependency-graph validation before
// execution. Malformed input must fail without invoking registered application
// callbacks.
#include "rtx/launch.hpp"

#include <iostream>
#include <sstream>
using namespace std::chrono_literals;
int main() {
  try {
    std::vector<std::string> observed;
    rtx::CallbackRegistry callbacks{{"capture",
                                     [&](auto& c) {
                                       observed.push_back("camera");
                                       c.heartbeat();
                                     }},
                                    {"infer", [&](auto& c) {
                                       observed.push_back("perception");
                                       c.heartbeat();
                                     }}};
    const std::string header = "rtx-launch 1\n";
    std::istringstream valid(
        header +
        "# reverse order exercises forward dependency resolution\nnode "
        "perception infer 10 20000000 15000000 100000000 camera\nnode camera "
        "capture 20 10000000 8000000 100000000 # comment\n");
    auto nodes = rtx::load_launch(valid, callbacks);
    if (!observed.empty() || nodes.size() != 2 || nodes[0].period != 20ms ||
        nodes[1].priority != 20)
      throw std::runtime_error("parsed configuration mismatch");
    rtx::Scheduler scheduler(std::move(nodes));
    scheduler.start(0ns);
    scheduler.step(0ns, [] { return 0ns; });
    if (observed != std::vector<std::string>{"camera", "perception"})
      throw std::runtime_error("configured launch sequence mismatch");
    const std::vector<std::string> invalid = {
        "",
        "rtx-launch 2\n",
        header,
        header + "node a missing 1 1 1 0\n",
        header + "node a capture 1 0 1 0\n",
        header + "node a capture 1 1 -1 0\n",
        header + "node a capture 1 1 1 -1\n",
        header + "node a capture 2147483648 1 1 0\n",
        header + "node a capture 1 9223372036854775808 1 0\n",
        header + "node a capture 1 1ms 1 0\n",
        header + "node a capture 1 1 1\n",
        header + "node a capture 1 1 1 0 b\n",
        header + "node a capture 1 1 1 0 a\n",
        header + "node a capture 1 1 1 0\nnode a infer 1 1 1 0\n",
        header + "node a capture 1 1 1 0 b b\nnode b infer 1 1 1 0\n",
        header + "node a capture 1 1 1 0 b\nnode b infer 1 1 1 0 a\n"};
    for (const auto& text : invalid) {
      bool rejected = false;
      std::istringstream input(text);
      try {
        rtx::load_launch(input, callbacks);
      } catch (const std::invalid_argument&) {
        rejected = true;
      }
      if (!rejected)
        throw std::runtime_error("malformed launch accepted: " + text);
    }
    std::cout << "launch parsing, graph execution and invalid configurations "
                 "passed\n";
  } catch (const std::exception& ex) {
    std::cerr << ex.what() << '\n';
    return 1;
  }
}
