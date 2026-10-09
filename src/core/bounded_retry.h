#pragma once
// Retry a fallible step a bounded number of times with a fixed pause between attempts.
// Used where one transient failure (a network timeout) must not discard good state (#202).

#include <chrono>
#include <thread>

namespace nevr {

struct RetryResult {
  bool ok;
  int attempts;  // calls made, 1..max_attempts
};

// attempt() returns true on success. sleep(ms) is called between attempts, never after the last one
// and never after a success. max_attempts below 1 is treated as 1.
template <typename Attempt, typename Sleep>
RetryResult RetryBounded(int max_attempts, int pause_ms, Attempt&& attempt, Sleep&& sleep) {
  if (max_attempts < 1) max_attempts = 1;
  for (int i = 1; i <= max_attempts; ++i) {
    if (attempt()) return {true, i};
    if (i < max_attempts) sleep(pause_ms);
  }
  return {false, max_attempts};
}

template <typename Attempt>
RetryResult RetryBounded(int max_attempts, int pause_ms, Attempt&& attempt) {
  return RetryBounded(max_attempts, pause_ms, static_cast<Attempt&&>(attempt),
                      [](int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); });
}

}  // namespace nevr
