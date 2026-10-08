#pragma once
// A bound on what a test's log sink may hold. The unit tests stub Log() to append every line to a
// vector; a loop under test that logs on every pass would otherwise grow that vector until the
// machine runs out of memory. Past kMaxLines the sink drops the line and calls the overflow handler,
// which by default prints the failure and ends the process so a spinning loop stops at once.
//
// Header only and free of gtest and Windows headers, so every test target can include it.

#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace TestLogCap {

inline constexpr std::size_t kMaxLines = 10000;
inline constexpr int kOverflowExitCode = 98;

using OverflowHandler = void (*)(std::size_t lines);

inline void EndProcessOnOverflow(std::size_t lines) {
  std::fprintf(stderr, "FAILED: log capture overflowed: %zu lines, a loop is spinning\n", lines);
  std::fflush(stderr);
  std::_Exit(kOverflowExitCode);
}

// A test replaces this to observe an overflow without ending the process.
inline OverflowHandler g_overflowHandler = EndProcessOnOverflow;

// The caller holds the sink's mutex.
inline void Append(std::vector<std::string>& sink, const char* line) {
  if (sink.size() >= kMaxLines) {
    g_overflowHandler(sink.size());
    return;
  }
  sink.emplace_back(line);
}

}  // namespace TestLogCap
