/* SYNTHESIS -- custom tool code, not from binary */
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>

/* N78 — the rate limiter behind the built-in log filter's counted summaries.
 *
 * A line repeating more than kThreshold times in a kWindowSec window is collapsed: the first
 * kThreshold pass, the rest are counted, and the count is reported as one
 * `[NEVR.LOGFILTER] repeated count=<n> window_s=<t> msg="<sample>"` line instead of being deleted.
 *
 * Identity is a digit-insensitive hash: "ExitProcess(3) ... (call #41)" and "(call #42)" collapse
 * together. Fixed-size table, no heap, its own mutex (never the file mutex: a fault raised while that
 * lock was held would deadlock the crash path, see N70). The caller passes the time, so a test can
 * move it.
 *
 * A summary leaves the table two ways: Check() closes out a slot when a later line lands on it
 * (the window expired, or another line hashes there), and TakePending() closes out the slots no later
 * line will ever land on, so a burst that goes quiet still reports its count.
 */
namespace nevr_rate_limiter {

constexpr uint32_t kWindowSec = 5;
constexpr uint32_t kThreshold = 3; /* allow this many per window, then collapse */
constexpr size_t kTableSize = 128;

enum class RateVerdict { Emit, Collapse };

class RateLimiter {
 public:
  /* Decides whether this line is emitted or folded into a counter. When a window closes with folded
   * lines, `out_summary` is filled with the summary to emit (empty otherwise). */
  RateVerdict Check(const char* msg, int len, uint64_t now, char* out_summary, size_t summary_cap) {
    out_summary[0] = '\0';
    const uint64_t h = NormalizedHash(msg, len);

    std::lock_guard<std::mutex> lock(mutex_);
    Entry& e = table_[h % kTableSize];

    /* Different line hashing to this slot, or the window has expired: close out the
     * previous occupant first (emitting its summary if it collapsed anything). */
    if (e.hash != h || now - e.window_start >= kWindowSec) {
      if (e.hash != 0 && e.seen > e.emitted) {
        FormatSummary(e, now - e.window_start, out_summary, summary_cap);
      }
      e.hash = h;
      e.window_start = now;
      e.seen = 0;
      e.emitted = 0;
      int copy = len < static_cast<int>(sizeof(e.sample)) - 1 ? len : static_cast<int>(sizeof(e.sample)) - 1;
      std::memcpy(e.sample, msg, static_cast<size_t>(copy));
      e.sample[copy] = '\0';
    }

    e.seen++;
    if (e.emitted < kThreshold) {
      e.emitted++;
      return RateVerdict::Emit;
    }
    return RateVerdict::Collapse;
  }

  /* Closes out one slot that has collapsed lines nobody reported: fills `out_summary`, clears the
   * slot and returns true; false when none is left. `all` closes slots whose window is still open
   * (shutdown); otherwise only slots whose window has expired. Call until it returns false. */
  bool TakePending(uint64_t now, bool all, char* out_summary, size_t summary_cap) {
    out_summary[0] = '\0';
    std::lock_guard<std::mutex> lock(mutex_);
    for (Entry& e : table_) {
      if (e.hash == 0 || e.seen <= e.emitted) continue;
      /* A clock that moved back reads as an age of 0, never as a huge unsigned age. */
      const uint64_t age = now > e.window_start ? now - e.window_start : 0;
      if (!all && age < kWindowSec) continue;
      FormatSummary(e, age, out_summary, summary_cap);
      e = Entry{};
      return true;
    }
    return false;
  }

 private:
  struct Entry {
    uint64_t hash = 0;
    uint64_t window_start = 0;
    uint32_t seen = 0;    /* total occurrences this window */
    uint32_t emitted = 0; /* how many we let through this window */
    char sample[128] = {};
  };

  /* FNV-1a over the message with digit runs folded to a single 0x01 sentinel, so
   * lines differing only in counters/IDs share an identity. */
  static uint64_t NormalizedHash(const char* s, int len) {
    uint64_t h = 1469598103934665603ULL;
    bool in_digits = false;
    for (int i = 0; i < len; i++) {
      unsigned char c = static_cast<unsigned char>(s[i]);
      if (c >= '0' && c <= '9') {
        if (in_digits) continue;
        in_digits = true;
        c = 0x01;
      } else {
        in_digits = false;
      }
      h ^= c;
      h *= 1099511628211ULL;
    }
    return h;
  }

  static void FormatSummary(const Entry& e, uint64_t window_s, char* out, size_t cap) {
    std::snprintf(out, cap, "[NEVR.LOGFILTER] repeated count=%u window_s=%llu msg=\"%s\"", e.seen - e.emitted,
                  static_cast<unsigned long long>(window_s), e.sample);
  }

  Entry table_[kTableSize];
  std::mutex mutex_;
};

}  // namespace nevr_rate_limiter
