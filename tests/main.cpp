// Test runner entry point: runs all registered TEST()s, optional name filter in argv[1].
#include <cstring>

#include "rt_test.hpp"

int main(int argc, char** argv) {
  const char* filter = argc > 1 ? argv[1] : nullptr;
  int ran = 0, bad = 0;
  for (const auto& c : rttest::registry()) {
    if (filter && !std::strstr(c.name, filter)) continue;
    const int before = rttest::failures();
    c.fn();
    const bool ok = rttest::failures() == before;
    std::printf("[%s] %s\n", ok ? " OK " : "FAIL", c.name);
    ++ran;
    bad += !ok;
  }
  std::printf("%d tests, %d failed\n", ran, bad);
  return bad ? 1 : 0;
}
