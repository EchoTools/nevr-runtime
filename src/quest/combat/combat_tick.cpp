#include "quest/combat/combat_tick.h"

#include <atomic>
#include <cstdint>

#include "quest/combat/combat_tick_targets.h"
#include "quest/combat/safe_read.h"
#include "quest/sentinel/hook_install.h"
#include "quest/sentinel/hook_log.h"

namespace nevr_quest::combat::tick {

namespace {

using Thunk = targets::Thunk;

sentinel::GotHook g_hook;
std::atomic<TickFn> g_ticks[kMaxCallbacks] = {};
std::atomic<std::size_t> g_count{0};
std::atomic<std::uint32_t> g_calls{0};
std::atomic<std::uint32_t> g_objectMisses{0};
std::atomic<std::uintptr_t> g_vptr{0};  // the value CR15Game's first word holds in this process: libr15's base + the pinned vaddr

// The game's function runs first (the slot's chain below this hook, the plugin's wrapper included); then the callbacks, on the game
// thread, in registration order, for an object that is the CR15Game. The log line is written after the original returned, never while a
// game call is on the stack above a callback's own work (callback_thunk.h rule 1).
void OnTick(Thunk::Fn original, void* game) noexcept {
  original(game);
  g_calls.fetch_add(1, std::memory_order_relaxed);
  const std::uintptr_t object = reinterpret_cast<std::uintptr_t>(game);
  std::uintptr_t vptr = 0;
  const bool readable = SafeRead(object, &vptr, sizeof(vptr));
  if (!readable || vptr != g_vptr.load(std::memory_order_relaxed)) {
    // Loud once: a wrong vptr means no callback ever runs and every module that asked for a tick silently does nothing.
    if (g_objectMisses.fetch_add(1, std::memory_order_relaxed) == 0) {
      sentinel::LogFields(sentinel::LogLevel::kError, "combat_tick_object",
                          {{"readable", readable ? 1 : 0},
                           {"msg", "the object the game's frame update was called with is not the CR15Game; the combat callbacks "
                                   "are skipped for it"}});
    }
    return;
  }
  const std::size_t n = g_count.load(std::memory_order_acquire);
  for (std::size_t i = 0; i < n && i < kMaxCallbacks; ++i) {
    const TickFn fn = g_ticks[i].load(std::memory_order_acquire);
    if (fn != nullptr) fn(object);
  }
}

NEVR_HOOK_RECORD(kTickHook, Thunk, &OnTick);

}  // namespace

bool Add(TickFn fn) noexcept {
  if (fn == nullptr) return false;
  std::size_t index = g_count.load(std::memory_order_relaxed);
  do {
    if (index >= kMaxCallbacks) return false;
  } while (!g_count.compare_exchange_weak(index, index + 1, std::memory_order_acq_rel));
  g_ticks[index].store(fn, std::memory_order_release);  // a frame that sees the count before this store skips the empty row
  return true;
}

std::size_t Count() noexcept { return g_count.load(std::memory_order_acquire); }

namespace {

// KNOWN-WRONG #2168: the q15t plugin installs its own wrapper on this slot until its last ticked module moves (F20); this install joins
// it through the manager's first-handler rule (combat_tick.h).
sentinel::GotStatus InstallStatusAt(sentinel::ImageLookup lookup) noexcept {
  Thunk::Arm(kTickHook);
  sentinel::ElfImage image{};
  sentinel::GotStatus status = lookup != nullptr && lookup(sentinel::pinned::kLibR15, &image) ? sentinel::GotStatus::kOk
                                                                                              : sentinel::GotStatus::kModuleNotLoaded;
  if (status == sentinel::GotStatus::kOk) {
    g_vptr.store(image.base + targets::kGameVptrVaddr, std::memory_order_release);
    status = sentinel::InstallThunk<Thunk>(g_hook, targets::Target(), lookup);
  }
  if (status != sentinel::GotStatus::kOk) Thunk::Disarm();  // a slot that did not take the thunk is never called; the handler stays off with it
  const bool ok = status == sentinel::GotStatus::kOk;
  sentinel::LogFields(ok ? sentinel::LogLevel::kInfo : sentinel::LogLevel::kError, "combat_tick_install",
                      {{"result", ok ? "installed" : "refused"},
                       {"status", sentinel::GotStatusName(status)},
                       {"callbacks", Count()},
                       {"msg", ok ? "the game-thread tick is installed; the combat modules' per-frame callbacks run after the game's frame update"
                                  : "the game-thread tick was not installed; the combat modules' per-frame callbacks will not run"}});
  return status;
}

}  // namespace

bool InstallAt(sentinel::ImageLookup lookup) noexcept { return InstallStatusAt(lookup) == sentinel::GotStatus::kOk; }

ModuleResult InstallNormal() noexcept {
  const sentinel::GotStatus status = InstallStatusAt(sentinel::FindLoadedImage);
  const bool ok = status == sentinel::GotStatus::kOk;
  return {ok, ok ? "installed" : sentinel::GotStatusName(status)};
}

std::uint32_t Calls() noexcept { return g_calls.load(std::memory_order_relaxed); }

void ResetForTest() noexcept {
  g_hook.Forget();
  Thunk::Reset();
  for (std::atomic<TickFn>& slot : g_ticks) slot.store(nullptr, std::memory_order_release);
  g_count.store(0, std::memory_order_release);
  g_calls.store(0, std::memory_order_relaxed);
  g_objectMisses.store(0, std::memory_order_relaxed);
  g_vptr.store(0, std::memory_order_relaxed);
}

}  // namespace nevr_quest::combat::tick
