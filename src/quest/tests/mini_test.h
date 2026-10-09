#pragma once
// Minimal test harness for the host and Android Quest test executables (no gtest on the
// NDK build). Every test runs on its own thread under a deadline: a hung test (a
// Start() that blocks its caller, a join that never returns) is reported as a failure
// and ends the run, instead of hanging it.

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <functional>
#include <future>
#include <string>
#include <thread>
#include <vector>

namespace mini_test {

struct TestCase {
  const char* name;
  void (*fn)();
};
inline std::vector<TestCase>& Registry() {
  static std::vector<TestCase> r;
  return r;
}
inline int& Failures() {
  static int n = 0;
  return n;
}
struct Registrar {
  Registrar(const char* n, void (*f)()) { Registry().push_back({n, f}); }
};

#define TEST(name)                                           \
  void name();                                               \
  const ::mini_test::Registrar name##_registrar(#name, &name); \
  void name()
#define CHECK(cond)                                                                    \
  do {                                                                                 \
    if (!(cond)) {                                                                     \
      std::fprintf(stderr, "  CHECK failed %s:%d: %s\n", __FILE__, __LINE__, #cond);   \
      ++::mini_test::Failures();                                                       \
    }                                                                                  \
  } while (0)
#define CHECK_EQ(a, b)                                                                              \
  do {                                                                                              \
    const auto va_ = (a);                                                                           \
    const auto vb_ = (b);                                                                           \
    if (!(va_ == vb_)) {                                                                            \
      std::fprintf(stderr, "  CHECK_EQ failed %s:%d: %s == %s\n", __FILE__, __LINE__, #a, #b);      \
      ++::mini_test::Failures();                                                                    \
    }                                                                                               \
  } while (0)

inline bool WaitUntil(const std::function<bool()>& pred, int timeout_ms = 5000) {
  for (int i = 0; i < timeout_ms; ++i) {
    if (pred()) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return pred();
}

// Runs the registered tests whose name contains argv[1] (all when absent).
inline int RunAll(int argc, char** argv, std::chrono::seconds per_test_deadline = std::chrono::seconds(30)) {
  const std::string filter = argc > 1 ? argv[1] : "";
  int ran = 0;
  for (const TestCase& t : Registry()) {
    if (!filter.empty() && std::string(t.name).find(filter) == std::string::npos) continue;
    const int before = Failures();
    std::promise<void> done;
    std::future<void> finished = done.get_future();
    std::thread runner([&t, &done] {
      try {
        t.fn();
      } catch (const std::exception& e) {
        std::fprintf(stderr, "  exception: %s\n", e.what());
        ++Failures();
      }
      done.set_value();
    });
    if (finished.wait_for(per_test_deadline) != std::future_status::ready) {
      std::fprintf(stderr, "  TIMEOUT after %lld s\n", static_cast<long long>(per_test_deadline.count()));
      std::printf("FAIL %s (timeout)\n", t.name);
      std::printf("%d tests run, run aborted by a timeout\n", ran + 1);
      std::fflush(stdout);
      std::_Exit(1);  // the hung test still owns stack objects; no destructors, no join
    }
    runner.join();
    std::printf("%s %s\n", Failures() == before ? "PASS" : "FAIL", t.name);
    ++ran;
  }
  std::printf("%d tests, %d failed checks\n", ran, Failures());
  return Failures() == 0 ? 0 : 1;
}

}  // namespace mini_test
