/* Which shadow a resolver request is filled from (docs/adr/0045-cdn-texture-delivery.md D1 and D3), decided without a lock or an
 * allocation, because it runs in the game's frame on whatever thread asks for a resource.
 *
 * A mounted set is published once and never changes; a newer set replaces it for later requests. A texture is two reads of one
 * name on one thread, index 0 and then index 1. A set published between the two must not pair a stock descriptor with a CDN
 * payload, so the set that answered index 0 is pinned for that thread and name, and index 1 of the same name uses it.
 *
 * The pin lives in the calling thread's own thread-local storage. That is the game's own method for per-thread state: its resource
 * request function keeps per-thread state in a TLS slot (CResourceManager::FillResourceFileRequest, echovr.exe 0x140fa1fe0: the slot is
 * looked up at 0x140fa2155, CThreadData::LookupSlot, read through CPool::GetOrCreate at 0x140fa2171, which calls TlsGetValue, and
 * allocated at 0x140fa218d, CThreadData::AllocState). It is the game's method for per-thread state, not a resource pin of its own. A table of pins keyed by thread would need a size, and any size is a limit on how
 * many threads read resources; thread-local storage has none, and nothing here is shared between threads but the published set.
 */
#pragma once

#include <atomic>
#include <cstdint>

#include "core/resource_shadow.h"

namespace nevr_cdn {

class Resolver {
 public:
  Resolver() noexcept;

  // Publishes a shadow (release). Earlier shadows must stay valid: a request being filled from one is still reading it.
  void Publish(const nevr_shadow::Shadow* shadow) noexcept { published_.store(shadow, std::memory_order_release); }
  const nevr_shadow::Shadow* published() const noexcept { return published_.load(std::memory_order_acquire); }

  // The shadow the request (name, index) is filled from on the calling thread, or nullptr for the live manager. Index 0 pins the
  // published shadow for this thread and name; any other index of that name on this thread uses the pin.
  const nevr_shadow::Shadow* Select(std::uint64_t name, std::uint64_t index) noexcept;

  // Test support: forgets the published set and every thread's pin.
  void Reset() noexcept;

 private:
  std::atomic<const nevr_shadow::Shadow*> published_{nullptr};
  std::atomic<std::uint64_t> epoch_{1};  // a Reset starts a new epoch, and a pin of an older one is no pin
  const std::uint64_t id_;               // one per Resolver ever made, so a pin of another resolver (or of a freed one's address) is no pin
};

}  // namespace nevr_cdn
