// Minimal dependency-free test harness (keeps the library buildable on
// toolchains without gtest). TEST(name) { CHECK(...); CHECK_EQ(a, b); }
#pragma once

#include <cstdio>
#include <vector>

namespace rttest {

struct Case {
  const char* name;
  void (*fn)();
};
inline std::vector<Case>& registry() {
  static std::vector<Case> r;
  return r;
}
inline int& failures() {
  static int f = 0;
  return f;
}
struct Registrar {
  Registrar(const char* n, void (*f)()) { registry().push_back({n, f}); }
};

}  // namespace rttest

#define TEST(name)                                          \
  static void test_##name();                                \
  static rttest::Registrar reg_##name(#name, &test_##name); \
  static void test_##name()

#define CHECK(cond)                                                   \
  do {                                                                \
    if (!(cond)) {                                                    \
      std::printf("    FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      ++rttest::failures();                                           \
    }                                                                 \
  } while (0)

#define CHECK_EQ(a, b)                                                    \
  do {                                                                    \
    const auto va_ = (a);                                                 \
    const auto vb_ = (b);                                                 \
    if (!(va_ == vb_)) {                                                  \
      std::printf("    FAIL %s:%d: %s == %s (%llu vs %llu)\n", __FILE__,  \
                  __LINE__, #a, #b, static_cast<unsigned long long>(va_), \
                  static_cast<unsigned long long>(vb_));                  \
      ++rttest::failures();                                               \
    }                                                                     \
  } while (0)
