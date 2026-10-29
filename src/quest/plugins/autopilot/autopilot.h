/* The autopilot's run (#1202, docs/adr/0025-quest-autopilot-route.md): the input filter, its counters and the event ring
 * the reporter drains. No thread and no file here: plugin.cpp owns those. The host test drives Filter with a fake clock
 * (src/quest/tests/autopilot_run_test.cpp).
 *
 * Filter runs on the game's input-polling thread, once per tracked controller per frame (input_seam_hook.h). It never
 * logs, allocates, locks or throws: it reads the clocks, writes the state, bumps atomic counters and puts events into a
 * fixed ring. A full ring drops the event and counts it.
 */
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

#include "nevr/plugin_interface.h"
#include "quest/plugins/autopilot/route.h"

namespace nevr_autopilot {

struct Clocks {
  std::int64_t monotonicNs = 0;
  std::int64_t realtimeNs = 0;
  std::int64_t boottimeNs = 0;
};
using ClockFn = Clocks (*)() noexcept;
// Called after an event is queued, from the filter: it must not block or lock (plugin.cpp writes an eventfd).
using WakeFn = void (*)(void* user) noexcept;

struct Event {
  enum Kind : std::uint32_t { kStart, kStep, kEnd } kind = kStart;
  std::uint32_t pass = 0;
  std::uint32_t index = 0;
  Clocks at;
};

// A bounded multi-producer, single-consumer ring (one sequence number per cell). Push never blocks and never allocates.
class EventRing {
 public:
  static constexpr std::size_t kCells = 64;
  EventRing() noexcept;
  bool Push(const Event& e) noexcept;  // false when full
  bool Pop(Event* e) noexcept;         // the consumer only

 private:
  struct Cell {
    std::atomic<std::size_t> seq;
    Event event;
  };
  Cell cells_[kCells];
  std::atomic<std::size_t> head_{0};
  std::size_t tail_ = 0;
};

// The counters route_end reports. Written by the filter, read by the reporter.
struct Counters {
  std::atomic<std::uint64_t> framesLeft{0}, framesRight{0}, framesUnknownHand{0};
  std::atomic<std::uint64_t> drivenLeft{0}, drivenRight{0};
  std::atomic<std::int64_t> maxGapNs{0};
  std::atomic<std::uint64_t> dropped{0};
};

class Run {
 public:
  // `route` must outlive the run. `waitForTrigger`: the start point is the first frame after Trigger(), not the first frame.
  Run(const Route& route, ClockFn clock, bool waitForTrigger, WakeFn wake = nullptr, void* wakeUser = nullptr) noexcept;

  // The filter body (NevrQuestInputFilterFn's work, with `this` as the user pointer).
  void Filter(NevrQuestControllerState* state) noexcept;
  static void FilterThunk(void* user, NevrQuestControllerState* state) noexcept;

  // The start file was seen (reporter thread).
  void Trigger() noexcept { triggered_.store(true, std::memory_order_release); }
  bool Triggered() const noexcept { return triggered_.load(std::memory_order_acquire); }
  bool Ended() const noexcept { return ended_.load(std::memory_order_acquire); }
  // Ends the run without a route_end (the plugin is shutting down): the filter changes nothing from the next call.
  void Stop() noexcept { ended_.store(true, std::memory_order_release); }

  EventRing& Events() noexcept { return ring_; }
  const Counters& Count() const noexcept { return counters_; }

 private:
  void Emit(const Event& e) noexcept;
  void CountGap(std::uint32_t hand, std::int64_t now) noexcept;

  const Route& route_;
  ClockFn clock_;
  bool waitForTrigger_;
  WakeFn wake_;
  void* wakeUser_;
  std::atomic<bool> triggered_{false};
  std::atomic<std::int64_t> startPointNs_{0};  // 0: no start point yet (a real monotonic clock is never 0 here)
  std::atomic<bool> started_{false};
  std::atomic<bool> ended_{false};
  // The (pass << 32 | index) + 1 of the last step reported; 0 before the first.
  std::atomic<std::uint64_t> lastStep_{0};
  std::atomic<std::int64_t> lastFrameNs_[2] = {};  // per hand: the previous frame inside the route
  EventRing ring_;
  Counters counters_;
};

}  // namespace nevr_autopilot
