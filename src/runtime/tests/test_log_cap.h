#pragma once
// A bound on what a test's log sink may hold. The unit tests stub Log() to append every line to a
// vector; a loop under test that logs on every pass would otherwise grow that vector until the
// machine runs out of memory. Past kMaxLines the sink drops the line and calls the overflow handler,
// which by default prints the failure, naming the running test, and ends the process so a spinning
// loop stops at once.
//
// Include after <gtest/gtest.h>. Header only; every test target that stubs Log() includes it.

#include <gtest/gtest.h>

#include <atomic>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace nevr_test_log_cap {

inline constexpr std::size_t kMaxLines = 10000;
inline constexpr int kOverflowExitCode = 98;

using OverflowHandler = void (*)(std::size_t lines);

inline void EndProcessOnOverflow(std::size_t lines) {
  const ::testing::TestInfo* info = ::testing::UnitTest::GetInstance()->current_test_info();
  const std::string name = info != nullptr ? std::string(info->test_suite_name()) + "." + info->name() : "<no test>";
  std::fprintf(stderr, "FAILED: log capture overflowed: %zu lines, a loop is spinning (in %s)\n", lines, name.c_str());
  std::fflush(stderr);
  std::_Exit(kOverflowExitCode);
}

// A test replaces this to observe an overflow without ending the process. Atomic because Append reads
// it from whichever thread logs.
inline std::atomic<OverflowHandler> g_overflowHandler{EndProcessOnOverflow};

// The caller holds the sink's mutex.
inline void Append(std::vector<std::string>& sink, const char* line) {
  if (sink.size() >= kMaxLines) {
    g_overflowHandler.load()(sink.size());
    return;
  }
  sink.emplace_back(line);
}

}  // namespace nevr_test_log_cap
