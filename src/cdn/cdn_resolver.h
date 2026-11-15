/* Which shadow a resolver request is filled from (docs/adr/0045-cdn-texture-delivery.md D1 and D3), decided without a lock or an
 * allocation, because it runs in the game's frame on whatever thread asks for a resource.
 *
 * A mounted set is published once and never changes; a newer set replaces it for later requests. A texture is two reads of one
 * name on one thread, index 0 and then index 1. A set published between the two must not pair a stock descriptor with a CDN
 * payload, so the set that answered index 0 is pinned for that thread and name, and index 1 of the same name uses it.
 *
 * The pin lives in the calling thread's own storage, not in a table: a table of pins keyed by thread would need a size, and any size is
 * a limit on how many threads read resources. Per-thread state is also the game's own method: its resource request function keeps
 * per-thread state in a TLS slot (CResourceManager::FillResourceFileRequest, echovr.exe 0x140fa1fe0: the slot is looked up at
 * 0x140fa2155, CThreadData::LookupSlot, read through CPool::GetOrCreate at 0x140fa2171, which calls TlsGetValue, and allocated at
 * 0x140fa218d, CThreadData::AllocState). On POSIX the storage is three pthread keys (quest/combat/thread_word.h), because the
 * Quest build's thread_local is emulated and a thread's first use allocates; on PC it is thread_local, which the image's TLS holds.
 * Nothing here is shared between threads but the published set.
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

  // 0 when every thread keeps its own pin. Otherwise the error that refused a pin key (POSIX: pthread_key_create, EAGAIN when the
  // process has no key left): index 1 is then answered from the set published at that moment, which can pair a stock descriptor
  // with a CDN payload when a set is published between the two reads.
  int pin_error() const noexcept { return pin_error_; }

 private:
  std::atomic<const nevr_shadow::Shadow*> published_{nullptr};
  std::atomic<std::uint64_t> generation_;  // one per Resolver ever made and per Reset, so a pin of another resolver, of a freed one's
                                           // address or of an earlier epoch is no pin
  const int pin_error_;
};

}  // namespace nevr_cdn
