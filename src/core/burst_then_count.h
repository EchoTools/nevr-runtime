#pragma once
// A line budget for a repeating event that is logged (docs/standards/logging.md Rule 5; the root "Loud" rule): an event that can come
// every frame is neither logged every time nor silenced after a cap. The first `burst` events are each written. The next one writes one
// line that says the later ones are counted, not logged. After it, a count line goes out on the first event at least `everyMs` after the
// last line, and `Flush`, called from a path that runs on its own (a frame or a tick), writes the count once no event has come for
// `everyMs`; `Final`, at the process's stop, writes it at once. So a log shows how many there were, whether it was 17 or a million.
// The counts are the caller's to print (`Line`).
//
// No clock and no lock: the caller passes the time and calls it from one thread, or holds its own lock. Header-only and dependency-free,
// so PC (runtime/combat/dronefix.cpp) and Quest (quest/combat/ecfx.cpp) use the same budget. Tests: runtime/tests/test_ecfx_rules.cpp
// (BurstThenCount), quest/tests/ecfx_test.cpp (the Quest's drain lines).
//
// Not the Quest combat guards' `nevr_quest::combat::LineBudget` (quest/combat/line_budget.h): that one is a cap with one notice and
// writes no count, because each of those modules' stats line counts every event. Here the count lines are the only count, and the
// header builds for PC as well (that one writes through the sentinel's log). Nor the per-window limiters (`RecordLimiter` in
// runtime/compat/lobby_frame.h, `RepeatLimiter` in quest/social/lobby_full_layout.h), which write `burst` lines in every window: here
// the burst is written once per process and every later line is a count.
#include <stdint.h>

namespace nevr {

class BurstThenCount {
 public:
  enum class Verdict {
    kCounted,  // counted only: no line now
    kWrite,    // one of the first `burst`: write this event's own line
    kCapped,   // the event after the first `burst`: write that later ones are counted, not logged
    kCount,    // write the count: `total` in all, `sinceLine` since the last line
  };
  struct Line {
    Verdict verdict;
    uint64_t total;      // events so far
    uint64_t sinceLine;  // events since the last line was written (kCount), else 0
  };

  constexpr BurstThenCount(unsigned burst, uint64_t everyMs) : burst_(burst), everyMs_(everyMs) {}

  // One event at nowMs.
  Line Event(uint64_t nowMs) {
    ++total_;
    lastEvent_ = nowMs;
    if (total_ <= burst_) return Wrote(Verdict::kWrite, nowMs, 0);
    if (total_ == uint64_t{burst_} + 1) return Wrote(Verdict::kCapped, nowMs, 0);
    if (nowMs - lastLine_ >= everyMs_) return Wrote(Verdict::kCount, nowMs, total_ - written_);
    return {Verdict::kCounted, total_, 0};
  }

  // No event: the count of the events no line covers yet, once none has come for everyMs.
  Line Flush(uint64_t nowMs) {
    if (total_ > written_ && nowMs - lastEvent_ >= everyMs_) return Wrote(Verdict::kCount, nowMs, total_ - written_);
    return {Verdict::kCounted, total_, 0};
  }

  // The count of the events no line covers yet, whatever the time: for the last line a process writes (its stop).
  Line Final(uint64_t nowMs) {
    if (total_ > written_) return Wrote(Verdict::kCount, nowMs, total_ - written_);
    return {Verdict::kCounted, total_, 0};
  }

  uint64_t Total() const { return total_; }
  // Events no line covers yet: Flush has a count to write once everyMs has passed with none.
  bool Pending() const { return total_ > written_; }

 private:
  Line Wrote(Verdict v, uint64_t nowMs, uint64_t since) {
    written_ = total_;
    lastLine_ = nowMs;
    return {v, total_, since};
  }

  unsigned burst_;
  uint64_t everyMs_;
  uint64_t total_ = 0, written_ = 0, lastEvent_ = 0, lastLine_ = 0;
};

}  // namespace nevr
