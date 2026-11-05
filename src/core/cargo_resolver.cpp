#include "core/cargo_resolver.h"

namespace nevr_cargo {

namespace {

constexpr std::uint64_t Mix(std::uint64_t x) noexcept {
  x ^= x >> 30;
  x *= 0xbf58476d1ce4e5b9ULL;
  x ^= x >> 27;
  x *= 0x94d049bb133111ebULL;
  x ^= x >> 31;
  return x;
}

}  // namespace

Resolver::Pin* Resolver::Claim(std::uint64_t thread) noexcept {
  const std::uint64_t key = thread != 0 ? thread : 1;  // 0 means a free slot
  const std::size_t home = static_cast<std::size_t>(Mix(key) % kPinSlots);
  for (unsigned probe = 0; probe < kPinProbe; ++probe) {
    Pin& pin = pins_[(home + probe) % kPinSlots];
    std::uint64_t held = pin.thread.load(std::memory_order_acquire);
    if (held == 0 && pin.thread.compare_exchange_strong(held, key, std::memory_order_acq_rel)) held = key;
    if (held == key) return &pin;
  }
  return nullptr;
}

const nevr_shadow::Shadow* Resolver::Select(std::uint64_t thread, std::uint64_t name, std::uint64_t index) noexcept {
  const nevr_shadow::Shadow* const published = published_.load(std::memory_order_acquire);
  Pin* const pin = Claim(thread);
  if (index == 0) {
    if (pin != nullptr) {
      pin->name.store(name, std::memory_order_relaxed);
      pin->shadow.store(published, std::memory_order_relaxed);
      pin->set.store(true, std::memory_order_relaxed);
    }
    return published;
  }
  if (pin != nullptr && pin->set.load(std::memory_order_relaxed) && pin->name.load(std::memory_order_relaxed) == name) {
    return pin->shadow.load(std::memory_order_relaxed);  // possibly null: the pair stays stock whole
  }
  // No pin for this name on this thread (index 0 was not seen, or the table is full): the published set, which is correct unless it
  // changed since index 0, and that needs a pin to be told.
  return published;
}

void Resolver::Reset() noexcept {
  published_.store(nullptr, std::memory_order_relaxed);
  for (Pin& pin : pins_) {
    pin.thread.store(0, std::memory_order_relaxed);
    pin.name.store(0, std::memory_order_relaxed);
    pin.shadow.store(nullptr, std::memory_order_relaxed);
    pin.set.store(false, std::memory_order_relaxed);
  }
}

std::size_t Resolver::pins_in_use() const noexcept {
  std::size_t n = 0;
  for (const Pin& pin : pins_) {
    if (pin.thread.load(std::memory_order_relaxed) != 0) ++n;
  }
  return n;
}

}  // namespace nevr_cargo
