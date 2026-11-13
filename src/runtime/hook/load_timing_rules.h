#pragma once

// Load timing records (docs/standards/logging.md, tag [NEVR.BOOT]): one `level_load` record per CncaGameSpace::LoadLevel
// and one `boot_timing` record per process. Pure and header-only, shared by the PC runtime and the Quest sentinel: the
// platform supplies the clocks and the sink, this file supplies the record text, the constants and the wrapper.
// The PC half is load_timing.{h,cpp} beside it. The Quest half, a hook on LoadLevel, does not exist yet:
// KNOWN-WRONG #2729 (no src/quest file named load_timing; naming.md N-RULES-HOME).

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <type_traits>

namespace nevr_load_timing {

inline constexpr char kTag[] = "[NEVR.BOOT]";
inline constexpr char kLevelLoadEvent[] = "level_load";
inline constexpr char kBootTimingEvent[] = "boot_timing";
inline constexpr char kUnknownLevelName[] = "unknown";
/// The game's own line that marks the engine ready (kept by the built-in filter, N77).
inline constexpr char kEngineReadyMarker[] = "Finished initializing engine";
/// A mark that never happened prints as this.
inline constexpr std::int64_t kUnknownMsValue = -1;

struct Marks {
  std::int64_t engine_ready_ms = kUnknownMsValue;
  std::int64_t logged_in_ms = kUnknownMsValue;
};

/// printf into a std::string of exactly the size the text needs: no fixed buffer, so nothing is ever cut.
template <class... Args>
std::string Printf(const char* format, Args... args) {
  const int size = std::snprintf(nullptr, 0, format, args...);
  if (size < 0) return std::string();
  std::string out(static_cast<std::size_t>(size), '\0');
  std::snprintf(&out[0], out.size() + 1, format, args...);
  return out;
}

/// `[NEVR.BOOT] level_load level=<name> level_hash=0x<hex> ms=<n> start_ts_ms=<epoch ms> first_load=<true|false>`
/// first_load is true for the first level load since process start: a cold cache reads differently from per-start scanning.
/// `ms` and `start_epoch_ms` are signed: kUnknownMsValue (-1) is a value that was never measured.
inline std::string FormatLevelLoad(const char* level, std::uint64_t level_hash, std::int64_t ms,
                                   std::int64_t start_epoch_ms, bool first_load) {
  return Printf("%s %s level=%s level_hash=0x%llx ms=%lld start_ts_ms=%lld first_load=%s", kTag, kLevelLoadEvent,
                level != nullptr && level[0] != '\0' ? level : kUnknownLevelName,
                static_cast<unsigned long long>(level_hash), static_cast<long long>(ms),
                static_cast<long long>(start_epoch_ms), first_load ? "true" : "false");
}

/// `[NEVR.BOOT] boot_timing engine_ready_ms=<n> logged_in_ms=<n>`
inline std::string FormatBootTiming(const Marks& marks) {
  return Printf("%s %s engine_ready_ms=%lld logged_in_ms=%lld", kTag, kBootTimingEvent,
                static_cast<long long>(marks.engine_ready_ms), static_cast<long long>(marks.logged_in_ms));
}

/// One per process: Take() is true once, for the first level load to begin, and false after.
class FirstLoad {
 public:
  bool Take() { return !taken_.exchange(true, std::memory_order_acq_rel); }

 private:
  std::atomic<bool> taken_{false};
};

/// Two hex digits and the terminating NUL.
constexpr std::size_t kHexByteBufferSize = 3;

/// Lower-case hex of `count` bytes; "unreadable" for a null pointer.
inline std::string HexBytes(const std::uint8_t* bytes, std::size_t count) {
  if (bytes == nullptr) return "unreadable";
  std::string out;
  char byte[kHexByteBufferSize];
  for (std::size_t i = 0; i < count; ++i) {
    std::snprintf(byte, sizeof(byte), "%02x", bytes[i]);
    out += byte;
  }
  return out;
}

/// Milliseconds from `start_ms` to `now_ms` (both Unix epoch ms); kUnknownMsValue when the start is unreadable (0) or later than now.
inline std::int64_t ElapsedMs(std::uint64_t now_ms, std::uint64_t start_ms) {
  if (start_ms == 0 || now_ms < start_ms) return kUnknownMsValue;
  return static_cast<std::int64_t>(now_ms - start_ms);
}

/// The Warning a hook that did not arm logs: the reason, the address, the bytes the hook expects and the bytes found, and
/// what that costs (no detour is installed, so level loads are not timed).
inline std::string FormatHookNotArmed(const char* hook, const char* reason, std::uint64_t va, const std::uint8_t* expected,
                                      const std::uint8_t* actual, std::size_t count) {
  const std::string expectedHex = HexBytes(expected, count);
  const std::string actualHex = HexBytes(actual, count);
  return Printf("%s load timing not armed name=%s va=0x%llX reason=%s expected=%s actual=%s (no detour was installed; level loads are not timed)", kTag, hook,
                static_cast<unsigned long long>(va), reason, expectedHex.c_str(), actualHex.c_str());
}

/// True when a line the game logged is its engine-ready marker.
inline bool IsEngineReadyLine(const char* line) {
  return line != nullptr && std::strstr(line, kEngineReadyMarker) != nullptr;
}

/// Process-start to engine-ready to logged-in. Each mark is kept once; the record is written once, at logged in.
/// The engine-ready mark comes from the log thread and logged-in from the game's thread, so the marks are locked.
class BootTimer {
 public:
  void MarkEngineReady(std::int64_t elapsed_ms) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (marks_.engine_ready_ms == kUnknownMsValue && !written_ && elapsed_ms >= 0) marks_.engine_ready_ms = elapsed_ms;
  }
  /// True, with the record in `line`, only for the first logged-in mark.
  bool MarkLoggedIn(std::int64_t elapsed_ms, std::string* line) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (written_) return false;
    written_ = true;
    marks_.logged_in_ms = elapsed_ms >= 0 ? elapsed_ms : kUnknownMsValue;
    if (line != nullptr) *line = FormatBootTiming(marks_);
    return true;
  }

 private:
  std::mutex mutex_;
  Marks marks_;
  bool written_ = false;
};

/// Runs `call` and hands the record to `sink`. `clock` has MonotonicMs() and EpochMs(). The record is written after the
/// call returns; a load that never returns (the game ends the process) writes none. A clock that went backwards, or a start time
/// the clock could not read (0), is written as kUnknownMsValue.
template <class Clock, class Sink, class Call>
auto TimeLevelLoad(Clock& clock, Sink&& sink, FirstLoad& first, std::uint64_t level_hash, const char* name, Call&& call) {
  const bool first_load = first.Take();
  const std::uint64_t start_epoch = clock.EpochMs();
  const std::uint64_t start = clock.MonotonicMs();
  auto emit = [&] {
    const std::uint64_t end = clock.MonotonicMs();
    const std::int64_t ms = end >= start ? static_cast<std::int64_t>(end - start) : kUnknownMsValue;
    const std::int64_t startTs = start_epoch != 0 ? static_cast<std::int64_t>(start_epoch) : kUnknownMsValue;
    sink(FormatLevelLoad(name, level_hash, ms, startTs, first_load));
  };
  if constexpr (std::is_void_v<decltype(call())>) {
    call();
    emit();
  } else {
    auto result = call();
    emit();
    return result;
  }
}

}  // namespace nevr_load_timing
