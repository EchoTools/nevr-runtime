#pragma once

// The script-hook measurement option (ADR 0037 rules 1, 4, 5; ADR 0038 rule 2; ADR 0040 rule 1).
//
// `NEVR_MEASURE_SCRIPT_HOOKS` (CMake option, off by default, set only by the `mingw-measure` preset) counts the calls of each
// script-hook row that is not plainly per event, so a measurement run can divide them by the frames of the same window
// (calls per frame, ADR 0038 rule 2). Without the option every name here compiles to nothing, so a release build carries no
// counter, no line and no variant word.
//
// A row counts a call with `NEVR_SCRIPT_HOOK_CALL("<row name>")` as its detour's first statement. The counter calls the row's
// original afterwards and returns its result: measuring changes nothing the game sees (ADR 0037 rule 6). The frame probe's
// 10 s window line is followed by one `[NEVR.PERF] script_hook calls=<n> frames=<n> row=<name>` line per counted row
// (FormatCallLine); the row's counter restarts with each window.
//
// Pure: std only, no windows.h and no game memory, so the host GTest drives it. A counter is one relaxed atomic add after a
// one-time lookup per call site; the lookup claims a slot with a compare-exchange, so a detour on any thread may count.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

namespace nevr_build_variant {

/// The variant word of a PC measurement build (ADR 0040 decision 1; a PC word only, the Quest repack does not take it).
inline constexpr const char* kScriptHookCallsWord = "script-hook-calls";

/// The runtime's first-log-line text for this build: `[NEVR.PATCH] info; variant=<word> (a measurement build: never a
/// release)` in a measurement build, empty in every other build (the line is written only when it is not empty).
inline std::string VariantLine() {
#ifdef NEVR_MEASURE_SCRIPT_HOOKS
  return std::string("[NEVR.PATCH] info; variant=") + kScriptHookCallsWord + " (a measurement build: never a release)";
#else
  return std::string();
#endif
}

}  // namespace nevr_build_variant

namespace nevr_script_hook_calls {

/// Rows one process can count. The rows of the plan's section 9 are far fewer; a row past this is not counted and Dropped()
/// says how many calls' sites were refused.
inline constexpr std::size_t kMaxRows = 64;
inline constexpr std::size_t kNoRow = kMaxRows;

struct Row {
  std::atomic<const char*> name{nullptr};
  std::atomic<std::uint64_t> calls{0};
};

class Counters {
 public:
  /// The slot of `name` (a string with static storage), claiming one the first time. kNoRow when the table is full.
  std::size_t IndexOf(const char* name) noexcept {
    for (std::size_t i = 0; i < kMaxRows; ++i) {
      const char* seen = rows_[i].name.load(std::memory_order_acquire);
      if (seen == nullptr) {
        const char* expected = nullptr;
        if (rows_[i].name.compare_exchange_strong(expected, name, std::memory_order_acq_rel)) return i;
        seen = expected;
      }
      if (seen == name || std::strcmp(seen, name) == 0) return i;
    }
    dropped_.fetch_add(1, std::memory_order_relaxed);
    return kNoRow;
  }

  void Count(std::size_t index) noexcept {
    if (index < kMaxRows) rows_[index].calls.fetch_add(1, std::memory_order_relaxed);
  }

  /// The calls of the row since the last Take, and the counter back to 0.
  std::uint64_t Take(std::size_t index) noexcept {
    return index < kMaxRows ? rows_[index].calls.exchange(0, std::memory_order_relaxed) : 0;
  }

  const char* NameOf(std::size_t index) const noexcept {
    return index < kMaxRows ? rows_[index].name.load(std::memory_order_acquire) : nullptr;
  }

  std::uint64_t Dropped() const noexcept { return dropped_.load(std::memory_order_relaxed); }

 private:
  Row rows_[kMaxRows];
  std::atomic<std::uint64_t> dropped_{0};
};

/// The process's counters (a function-local static: one table for every translation unit).
inline Counters& Global() {
  static Counters counters;
  return counters;
}

/// `[NEVR.PERF] script_hook calls=<n> frames=<n> row=<name>`: `calls` of the row in the window the frame probe just closed and
/// `frames` that window's frame count, so calls per frame is their quotient (ADR 0038 rule 2). Returns snprintf's result.
inline int FormatCallLine(char* buf, std::size_t cap, std::uint64_t calls, std::uint64_t frames, const char* row) {
  return std::snprintf(buf, cap, "[NEVR.PERF] script_hook calls=%llu frames=%llu row=%s",
                       static_cast<unsigned long long>(calls), static_cast<unsigned long long>(frames), row);
}

/// Calls `emit(line)` once for each row that has a name, with the row's calls since the last window and `frames`, and starts the
/// row's next window. A row that was not called in the window is reported with calls=0: a silent row is not a row that was left out.
template <typename Emit>
inline void ReportWindow(Counters& counters, std::uint64_t frames, Emit&& emit) {
  char line[256];
  for (std::size_t i = 0; i < kMaxRows; ++i) {
    const char* name = counters.NameOf(i);
    if (name == nullptr) break;
    FormatCallLine(line, sizeof(line), counters.Take(i), frames, name);
    emit(static_cast<const char*>(line));
  }
}

}  // namespace nevr_script_hook_calls

#ifdef NEVR_MEASURE_SCRIPT_HOOKS
#define NEVR_SCRIPT_HOOK_CALL(row_name)                                                                        \
  do {                                                                                                         \
    static const std::size_t nevr_script_hook_row_ = nevr_script_hook_calls::Global().IndexOf(row_name);       \
    nevr_script_hook_calls::Global().Count(nevr_script_hook_row_);                                             \
  } while (false)
#else
#define NEVR_SCRIPT_HOOK_CALL(row_name) static_cast<void>(0)
#endif
