#include "core/cargo_resolver.h"

namespace nevr_cargo {

namespace {

// What index 0 left on this thread: the resolver and epoch it was made for, the name, and the shadow that answered (possibly none: a
// thread that saw no cargo at index 0 must see none at index 1).
struct ThreadPin {
  std::uint64_t resolver = 0;  // 0: no pin
  std::uint64_t epoch = 0;
  std::uint64_t name = 0;
  const nevr_shadow::Shadow* shadow = nullptr;
};

thread_local ThreadPin t_pin;

constexpr std::uint64_t kFirstResolverId = 1;
std::atomic<std::uint64_t> g_nextResolverId{kFirstResolverId};

}  // namespace

Resolver::Resolver() noexcept : id_(g_nextResolverId.fetch_add(1, std::memory_order_relaxed)) {}

const nevr_shadow::Shadow* Resolver::Select(std::uint64_t name, std::uint64_t index) noexcept {
  const nevr_shadow::Shadow* const published = published_.load(std::memory_order_acquire);
  const std::uint64_t epoch = epoch_.load(std::memory_order_relaxed);
  if (index == 0) {
    t_pin = ThreadPin{id_, epoch, name, published};
    return published;
  }
  if (t_pin.resolver == id_ && t_pin.epoch == epoch && t_pin.name == name) {
    return t_pin.shadow;  // possibly null: the pair stays stock whole
  }
  // No pin for this name on this thread (index 0 was not seen on it): the published set, which is correct unless it changed since
  // index 0, and that needs a pin to be told.
  return published;
}

void Resolver::Reset() noexcept {
  published_.store(nullptr, std::memory_order_relaxed);
  epoch_.fetch_add(1, std::memory_order_relaxed);
}

}  // namespace nevr_cargo
