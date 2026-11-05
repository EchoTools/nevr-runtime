/* Which shadow a resolver request is filled from (docs/adr/0044-cdn-texture-delivery.md D1 and D3), decided without a lock or an
 * allocation, because it runs in the game's frame on whatever thread asks for a resource.
 *
 * A mounted set is published once and never changes; a newer set replaces it for later requests. A texture is two reads of one
 * name on one thread, index 0 and then index 1. A set published between the two must not pair a stock descriptor with a cargo
 * payload, so the set that answered index 0 is pinned for that thread and name, and index 1 of the same name uses it.
 */
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

#include "core/resource_shadow.h"

namespace nevr_cargo {

class Resolver {
 public:
  static constexpr std::size_t kPinSlots = 256;
  static constexpr unsigned kPinProbe = 8;

  // Publishes a shadow (release). Earlier shadows must stay valid: a request being filled from one is still reading it.
  void Publish(const nevr_shadow::Shadow* shadow) noexcept { published_.store(shadow, std::memory_order_release); }
  const nevr_shadow::Shadow* published() const noexcept { return published_.load(std::memory_order_acquire); }

  // The shadow the request (name, index) from thread `thread` is filled from, or nullptr for the live manager. Index 0 pins the
  // published shadow for (thread, name); any other index of that name on that thread uses the pin.
  const nevr_shadow::Shadow* Select(std::uint64_t thread, std::uint64_t name, std::uint64_t index) noexcept;

  // Test support.
  void Reset() noexcept;
  std::size_t pins_in_use() const noexcept;

 private:
  struct Pin {
    std::atomic<std::uint64_t> thread{0};  // 0: free; a thread's slot is written only by that thread once claimed
    std::atomic<std::uint64_t> name{0};
    std::atomic<const nevr_shadow::Shadow*> shadow{nullptr};  // may be null: index 0 saw no cargo, and index 1 must see none either
    std::atomic<bool> set{false};
  };
  Pin* Claim(std::uint64_t thread) noexcept;

  std::atomic<const nevr_shadow::Shadow*> published_{nullptr};
  Pin pins_[kPinSlots];
};

}  // namespace nevr_cargo
