// Launch configuration: which nodes exist, their parameters, and the order /
// dependencies / delays in which they start. Text format, one directive per
// line:
//
//   node <name> type=<factory> priority=<0-255> period_ms=<n> [deadline_ms=<n>]
//        [hb_timeout_ms=<n>] [on_fault=ignore|restart|stop|safe]
//        [max_restarts=<n>] [fault_threshold=<n>]
//   start <name> [after=<dep>[,<dep>...]] [delay_ms=<n>]
//
// '#' starts a comment. Nodes without a 'start' line are not launched.
#pragma once

#include <string>
#include <vector>

#include "rtsdk/node.hpp"

namespace rtsdk {

struct NodeSpec {
  NodeConfig config;
  std::string type;
};

struct LaunchStep {
  std::string node;
  std::vector<std::string> after;
  TimeUs delay_us = 0;  // additional delay after all dependencies are up
};

struct LaunchConfig {
  std::vector<NodeSpec> nodes;
  std::vector<LaunchStep> steps;

  // Parses text; on error returns false and fills `error` (with line number).
  static bool parse(const std::string& text, LaunchConfig& out,
                    std::string& error);
  // Checks references and dependency cycles; on success `order` is a valid
  // start order (indices into `steps`).
  bool resolve_order(std::vector<std::size_t>& order, std::string& error) const;
};

}  // namespace rtsdk
