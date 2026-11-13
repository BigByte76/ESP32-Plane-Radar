// Minimal self-registering test harness for the host-side unit tests.
// Each tests/host/test_*.cpp builds into its own executable; main() lives here.
#pragma once

#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <type_traits>
#include <vector>

namespace testfw {

struct TestCase {
  const char* name;
  std::function<void()> fn;
};

inline std::vector<TestCase>& registry() {
  static std::vector<TestCase> tests;
  return tests;
}

inline int& failures() {
  static int n = 0;
  return n;
}

inline bool& currentFailed() {
  static bool f = false;
  return f;
}

struct Registrar {
  Registrar(const char* name, std::function<void()> fn) {
    registry().push_back({name, std::move(fn)});
  }
};

inline void fail(const char* file, int line, const std::string& msg) {
  std::printf("    %s:%d: %s\n", file, line, msg.c_str());
  currentFailed() = true;
  ++failures();
}

inline std::string show(const std::string& v) { return "\"" + v + "\""; }
inline std::string show(const char* v) { return v ? show(std::string(v)) : "null"; }
inline std::string show(bool v) { return v ? "true" : "false"; }
template <typename T>
std::string show(const T& v) {
  if constexpr (std::is_enum_v<T>) {
    return std::to_string(static_cast<long long>(v));
  } else {
    return std::to_string(v);
  }
}

/** Optional per-binary hook run before every test (reset global state). */
inline std::function<void()>& setUpHook() {
  static std::function<void()> hook;
  return hook;
}

}  // namespace testfw

#define TF_CAT2(a, b) a##b
#define TF_CAT(a, b) TF_CAT2(a, b)

#define TEST(name)                                                   \
  static void name();                                                \
  static ::testfw::Registrar TF_CAT(tf_reg_, name)(#name, name);     \
  static void name()

#define SET_UP(body)                                                           \
  static ::testfw::Registrar* TF_CAT(tf_setup_, __LINE__) = []() {             \
    ::testfw::setUpHook() = [] body;                                           \
    return static_cast<::testfw::Registrar*>(nullptr);                         \
  }()

#define CHECK(cond)                                                  \
  do {                                                               \
    if (!(cond)) ::testfw::fail(__FILE__, __LINE__, "CHECK(" #cond ")"); \
  } while (0)

#define CHECK_EQ(a, b)                                                       \
  do {                                                                       \
    const auto tf_a = (a);                                                   \
    const auto tf_b = (b);                                                   \
    if (!(tf_a == tf_b)) {                                                   \
      ::testfw::fail(__FILE__, __LINE__,                                     \
                     std::string("CHECK_EQ(" #a ", " #b ") got ") +          \
                         ::testfw::show(tf_a) + " vs " + ::testfw::show(tf_b)); \
    }                                                                        \
  } while (0)

#define CHECK_NEAR(a, b, tol)                                                \
  do {                                                                       \
    const double tf_a = (a);                                                 \
    const double tf_b = (b);                                                 \
    if (std::fabs(tf_a - tf_b) > (tol)) {                                    \
      ::testfw::fail(__FILE__, __LINE__,                                     \
                     std::string("CHECK_NEAR(" #a ", " #b ") got ") +        \
                         std::to_string(tf_a) + " vs " + std::to_string(tf_b)); \
    }                                                                        \
  } while (0)

#define CHECK_STREQ(a, b)                                                    \
  do {                                                                       \
    const std::string tf_a = (a);                                            \
    const std::string tf_b = (b);                                            \
    if (tf_a != tf_b) {                                                      \
      ::testfw::fail(__FILE__, __LINE__,                                     \
                     std::string("CHECK_STREQ(" #a ", " #b ") got \"") +     \
                         tf_a + "\" vs \"" + tf_b + "\"");                   \
    }                                                                        \
  } while (0)

int main(int argc, char** argv) {
  const char* filter = argc > 1 ? argv[1] : nullptr;
  int run = 0;
  int failed_tests = 0;
  for (auto& t : ::testfw::registry()) {
    if (filter != nullptr && std::strstr(t.name, filter) == nullptr) continue;
    ::testfw::currentFailed() = false;
    if (::testfw::setUpHook()) ::testfw::setUpHook()();
    t.fn();
    ++run;
    if (::testfw::currentFailed()) {
      ++failed_tests;
      std::printf("  FAIL %s\n", t.name);
    } else {
      std::printf("  ok   %s\n", t.name);
    }
  }
  std::printf("%d tests, %d failed\n", run, failed_tests);
  return failed_tests == 0 ? 0 : 1;
}
