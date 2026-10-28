#include "rtsdk/launch.hpp"

#include <map>
#include <sstream>

namespace rtsdk {
namespace {

// Strict decimal parser (no sign, no overflow: at most 18 digits).
bool parse_u64(const std::string& s, std::uint64_t& v) {
  if (s.empty() || s.find_first_not_of("0123456789") != std::string::npos ||
      s.size() > 18)
    return false;
  v = std::stoull(s);
  return true;
}

// Splits on sep, dropping empty fields ("a,,b" -> a, b).
std::vector<std::string> split(const std::string& s, char sep) {
  std::vector<std::string> out;
  std::string cur;
  for (char c : s) {
    if (c == sep) {
      if (!cur.empty()) out.push_back(cur);
      cur.clear();
    } else {
      cur += c;
    }
  }
  if (!cur.empty()) out.push_back(cur);
  return out;
}

}  // namespace

// Line-oriented parser. Grammar:
//   node <name> type=<t> [priority= period_ms= deadline_ms= hb_timeout_ms=
//                         max_restarts= fault_threshold=
//                         on_fault=ignore|restart|stop|safe]
//   start <name> [after=a,b] [delay_ms=N]
// "#" starts a comment. Unknown keys are errors so typos never go unnoticed.
bool LaunchConfig::parse(const std::string& text, LaunchConfig& out,
                         std::string& error) {
  out = LaunchConfig{};
  std::istringstream in(text);
  std::string line;
  int lineno = 0;
  auto fail = [&](const std::string& msg) {
    error = "line " + std::to_string(lineno) + ": " + msg;
    return false;
  };
  while (std::getline(in, line)) {
    ++lineno;
    if (auto h = line.find('#'); h != std::string::npos) line.erase(h);
    std::istringstream ls(line);
    std::string word;
    if (!(ls >> word)) continue;
    std::string name;
    if (!(ls >> name)) return fail("missing node name");
    std::map<std::string, std::string> kv;
    std::string tok;
    while (ls >> tok) {
      auto eq = tok.find('=');
      if (eq == std::string::npos || eq == 0)
        return fail("expected key=value, got '" + tok + "'");
      kv[tok.substr(0, eq)] = tok.substr(eq + 1);
    }
    // Reads an optional numeric key, scaling units (ms -> us); absent keys keep
    // the default.
    auto num = [&](const char* key, std::uint64_t& v, std::uint64_t scale) {
      auto it = kv.find(key);
      if (it == kv.end()) return true;
      std::uint64_t raw;
      if (!parse_u64(it->second, raw)) return false;
      v = raw * scale;
      kv.erase(it);
      return true;
    };
    // "node" declares a task; "start" places it in the launch sequence.
    if (word == "node") {
      NodeSpec spec;
      spec.config.name = name;
      std::uint64_t prio = spec.config.priority;
      std::uint64_t restarts = spec.config.max_restarts,
                    thr = spec.config.fault_threshold;
      std::uint64_t dl_us = 0, hb_us = 0, period_us = spec.config.period_us;
      if (!num("priority", prio, 1) || prio > 255) return fail("bad priority");
      if (!num("period_ms", period_us, kMs)) return fail("bad period_ms");
      if (!num("deadline_ms", dl_us, kMs)) return fail("bad deadline_ms");
      if (!num("hb_timeout_ms", hb_us, kMs)) return fail("bad hb_timeout_ms");
      if (!num("max_restarts", restarts, 1)) return fail("bad max_restarts");
      if (!num("fault_threshold", thr, 1) || thr == 0)
        return fail("bad fault_threshold");
      if (period_us == 0) return fail("period_ms must be > 0");
      spec.config.priority = static_cast<Priority>(prio);
      spec.config.period_us = period_us;
      spec.config.deadline_us = dl_us;
      spec.config.heartbeat_timeout_us = hb_us;
      spec.config.max_restarts = static_cast<std::uint32_t>(restarts);
      spec.config.fault_threshold = static_cast<std::uint32_t>(thr);
      if (auto it = kv.find("on_fault"); it != kv.end()) {
        const auto& v = it->second;
        if (v == "ignore")
          spec.config.on_fault = FaultAction::Ignore;
        else if (v == "restart")
          spec.config.on_fault = FaultAction::RestartNode;
        else if (v == "stop")
          spec.config.on_fault = FaultAction::StopNode;
        else if (v == "safe")
          spec.config.on_fault = FaultAction::SafeState;
        else
          return fail("bad on_fault '" + v + "'");
        kv.erase(it);
      }
      if (auto it = kv.find("type"); it != kv.end()) {
        spec.type = it->second;
        kv.erase(it);
      } else {
        return fail("node requires type=");
      }
      if (!kv.empty()) return fail("unknown key '" + kv.begin()->first + "'");
      for (const auto& n : out.nodes)
        if (n.config.name == name) return fail("duplicate node '" + name + "'");
      out.nodes.push_back(std::move(spec));
    } else if (word == "start") {
      LaunchStep st;
      st.node = name;
      std::uint64_t delay_us = 0;
      if (!num("delay_ms", delay_us, kMs)) return fail("bad delay_ms");
      st.delay_us = delay_us;
      if (auto it = kv.find("after"); it != kv.end()) {
        st.after = split(it->second, ',');
        kv.erase(it);
      }
      if (!kv.empty()) return fail("unknown key '" + kv.begin()->first + "'");
      out.steps.push_back(std::move(st));
    } else {
      return fail("unknown directive '" + word + "'");
    }
  }
  return true;
}

// Validates the sequence and produces a start order honouring "after=".
bool LaunchConfig::resolve_order(std::vector<std::size_t>& order,
                                 std::string& error) const {
  order.clear();
  std::map<std::string, std::size_t> step_of;  // node name -> step index
  for (std::size_t i = 0; i < steps.size(); ++i) {
    bool declared = false;
    for (const auto& n : nodes) declared |= (n.config.name == steps[i].node);
    if (!declared) {
      error = "start references unknown node '" + steps[i].node + "'";
      return false;
    }
    if (!step_of.emplace(steps[i].node, i).second) {
      error = "node '" + steps[i].node + "' started twice";
      return false;
    }
  }
  for (const auto& s : steps)
    for (const auto& d : s.after)
      if (!step_of.count(d)) {
        error =
            "'" + s.node + "' depends on '" + d + "' which is never started";
        return false;
      }
  // Kahn's algorithm; stable w.r.t. declaration order. No progress in a full
  // pass means the remaining steps wait on each other (a cycle).
  std::vector<int> state(steps.size(), 0);  // 0 new, 1 done
  while (order.size() < steps.size()) {
    bool progress = false;
    for (std::size_t i = 0; i < steps.size(); ++i) {
      if (state[i]) continue;
      bool ready = true;
      for (const auto& d : steps[i].after) ready &= (state[step_of[d]] == 1);
      if (!ready) continue;
      state[i] = 1;
      order.push_back(i);
      progress = true;
    }
    if (!progress) {
      error = "dependency cycle in launch sequence";
      order.clear();
      return false;
    }
  }
  return true;
}

}  // namespace rtsdk
