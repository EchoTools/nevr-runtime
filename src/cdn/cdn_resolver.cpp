#include "cdn/cdn_resolver.h"

#if !defined(_WIN32)
#include "quest/combat/thread_word.h"
#endif

namespace nevr_cdn {

namespace {

// Every Resolver and every Reset takes the next generation, so a pin of another resolver, of a freed one's address or of an earlier
// epoch is no pin. 0 is never taken: a thread that set no pin reads 0 and matches nothing.
constexpr std::uint64_t kFirstGeneration = 1;
std::atomic<std::uint64_t> g_nextGeneration{kFirstGeneration};

std::uint64_t NextGeneration() noexcept { return g_nextGeneration.fetch_add(1, std::memory_order_relaxed); }

// What index 0 left on this thread: the generation it was made for, the name, and the shadow that answered (possibly none: a thread
// that saw no CDN at index 0 must see none at index 1).
#if defined(_WIN32)
// The PC image's own TLS (the PE TLS directory, made when the thread starts): thread_local is not emulated, so nothing allocates here.
struct ThreadPin {
  std::uint64_t generation = 0;
  std::uint64_t name = 0;
  const nevr_shadow::Shadow* shadow = nullptr;
};
thread_local ThreadPin t_pin;

int PreparePins() noexcept { return 0; }
void StorePin(std::uint64_t generation, std::uint64_t name, const nevr_shadow::Shadow* shadow) noexcept {
  t_pin = ThreadPin{generation, name, shadow};
}
bool LoadPin(std::uint64_t generation, std::uint64_t name, const nevr_shadow::Shadow** shadow) noexcept {
  if (t_pin.generation != generation || t_pin.name != name) return false;
  *shadow = t_pin.shadow;
  return true;
}
#else
// Three pthread keys (quest/combat/thread_word.h), not thread_local: on Quest (API 26) thread_local is emulated, and a thread's
// first use allocates inside the game's resource request. A key's value sits in the thread's own static TLS block, so a read or a write never
// allocates or locks, and a key has no limit on the threads that use it.
static_assert(sizeof(std::uintptr_t) >= sizeof(std::uint64_t), "a pin word holds a 64-bit generation and name");
nevr_quest::combat::ThreadWord g_pinGeneration;
nevr_quest::combat::ThreadWord g_pinName;
nevr_quest::combat::ThreadWord g_pinShadow;

int PreparePins() noexcept {
  if (const int error = g_pinGeneration.Create(); error != 0) return error;
  if (const int error = g_pinName.Create(); error != 0) return error;
  return g_pinShadow.Create();
}
void StorePin(std::uint64_t generation, std::uint64_t name, const nevr_shadow::Shadow* shadow) noexcept {
  g_pinGeneration.Set(static_cast<std::uintptr_t>(generation));
  g_pinName.Set(static_cast<std::uintptr_t>(name));
  g_pinShadow.Set(reinterpret_cast<std::uintptr_t>(shadow));
}
bool LoadPin(std::uint64_t generation, std::uint64_t name, const nevr_shadow::Shadow** shadow) noexcept {
  if (g_pinGeneration.Get() != generation || g_pinName.Get() != name) return false;
  *shadow = reinterpret_cast<const nevr_shadow::Shadow*>(g_pinShadow.Get());
  return true;
}
#endif

}  // namespace

Resolver::Resolver() noexcept : generation_(NextGeneration()), pin_error_(PreparePins()) {}

const nevr_shadow::Shadow* Resolver::Select(std::uint64_t name, std::uint64_t index) noexcept {
  const nevr_shadow::Shadow* const published = published_.load(std::memory_order_acquire);
  const std::uint64_t generation = generation_.load(std::memory_order_relaxed);
  if (index == 0) {
    StorePin(generation, name, published);
    return published;
  }
  const nevr_shadow::Shadow* pinned = nullptr;
  if (LoadPin(generation, name, &pinned)) {
    return pinned;  // possibly null: the pair stays stock whole
  }
  // No pin for this name on this thread (index 0 was not seen on it): the published set, which is correct unless it changed since
  // index 0, and that needs a pin to be told.
  return published;
}

void Resolver::Reset() noexcept {
  published_.store(nullptr, std::memory_order_relaxed);
  generation_.store(NextGeneration(), std::memory_order_relaxed);
}

}  // namespace nevr_cdn
