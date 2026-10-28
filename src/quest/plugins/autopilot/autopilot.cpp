#include "quest/plugins/autopilot/autopilot.h"

namespace nevr_autopilot {

EventRing::EventRing() noexcept {
  for (std::size_t i = 0; i < kCells; ++i) cells_[i].seq.store(i, std::memory_order_relaxed);
}

bool EventRing::Push(const Event& e) noexcept {
  std::size_t pos = head_.load(std::memory_order_relaxed);
  for (;;) {
    Cell& cell = cells_[pos % kCells];
    const std::size_t seq = cell.seq.load(std::memory_order_acquire);
    if (seq == pos) {
      if (head_.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) {
        cell.event = e;
        cell.seq.store(pos + 1, std::memory_order_release);
        return true;
      }
    } else if (seq < pos) {
      return false;  // the cell still holds an event the consumer has not taken: full
    } else {
      pos = head_.load(std::memory_order_relaxed);
    }
  }
}

bool EventRing::Pop(Event* e) noexcept {
  Cell& cell = cells_[tail_ % kCells];
  if (cell.seq.load(std::memory_order_acquire) != tail_ + 1) return false;
  *e = cell.event;
  cell.seq.store(tail_ + kCells, std::memory_order_release);
  ++tail_;
  return true;
}

Run::Run(const Route& route, ClockFn clock, bool waitForTrigger, WakeFn wake, void* wakeUser) noexcept
    : route_(route), clock_(clock), waitForTrigger_(waitForTrigger), wake_(wake), wakeUser_(wakeUser) {}

void Run::FilterThunk(void* user, NevrQuestControllerState* state) noexcept {
  static_cast<Run*>(user)->Filter(state);
}

void Run::Emit(const Event& e) noexcept {
  if (!ring_.Push(e)) {
    counters_.dropped.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  if (wake_ != nullptr) wake_(wakeUser_);
}

void Run::CountGap(std::uint32_t hand, std::int64_t now) noexcept {
  const std::int64_t previous = lastFrameNs_[hand].exchange(now, std::memory_order_relaxed);
  if (previous == 0) return;
  const std::int64_t gap = now - previous;
  std::int64_t seen = counters_.maxGapNs.load(std::memory_order_relaxed);
  while (gap > seen && !counters_.maxGapNs.compare_exchange_weak(seen, gap, std::memory_order_relaxed)) {
  }
}

void Run::Filter(NevrQuestControllerState* state) noexcept {
  if (ended_.load(std::memory_order_acquire)) return;
  if (waitForTrigger_ && !triggered_.load(std::memory_order_acquire)) return;
  const Clocks now = clock_();
  std::int64_t startPoint = startPointNs_.load(std::memory_order_acquire);
  if (startPoint == 0) {
    std::int64_t expected = 0;
    startPoint = startPointNs_.compare_exchange_strong(expected, now.monotonicNs, std::memory_order_acq_rel)
                     ? now.monotonicNs
                     : expected;
  }
  const Position p = Locate(route_, now.monotonicNs - startPoint);
  if (p.phase == Position::kWaiting) return;
  if (p.phase == Position::kDone) {
    bool wasEnded = false;
    if (ended_.compare_exchange_strong(wasEnded, true, std::memory_order_acq_rel)) {
      // A route shorter than one frame never ran a frame: its start is reported at its end, so route_start precedes
      // route_end in every log.
      if (!started_.exchange(true, std::memory_order_acq_rel)) Emit(Event{Event::kStart, 0, 0, now});
      Emit(Event{Event::kEnd, 0, 0, now});
    }
    return;
  }
  if (!started_.exchange(true, std::memory_order_acq_rel)) Emit(Event{Event::kStart, 0, 0, now});
  // Both hands poll in one frame; whichever moves the key first reports the step, once.
  const std::uint64_t key = ((static_cast<std::uint64_t>(p.pass) << 32) | p.index) + 1;
  if (lastStep_.exchange(key, std::memory_order_acq_rel) != key) Emit(Event{Event::kStep, p.pass, p.index, now});
  const std::uint32_t hand = state->hand;
  if (hand != NEVR_QUEST_HAND_LEFT && hand != NEVR_QUEST_HAND_RIGHT) {
    counters_.framesUnknownHand.fetch_add(1, std::memory_order_relaxed);  // a hand we cannot name: counted, not driven
    return;
  }
  CountGap(hand, now.monotonicNs);
  (hand == NEVR_QUEST_HAND_LEFT ? counters_.framesLeft : counters_.framesRight).fetch_add(1, std::memory_order_relaxed);
  if (!route_.drives[hand]) return;
  const HandState& s = route_.steps[p.index].hands[hand];
  state->buttons = s.buttons;
  state->touches = s.touches;
  state->index_trigger = s.trigger;
  state->grip_trigger = s.grip;
  state->stick_x = s.stickX;
  state->stick_y = s.stickY;
  (hand == NEVR_QUEST_HAND_LEFT ? counters_.drivenLeft : counters_.drivenRight).fetch_add(1, std::memory_order_relaxed);
}

}  // namespace nevr_autopilot
