#pragma once

// The loads of pnsrad.dll and pnsradmatchmaking.dll that the loader's DLL notification has seen and that the
// runtime has not yet patched.
//
// LdrDllNotification callbacks run under the loader lock, where calling into another module is unsafe. So the
// notification only classifies the module by the name the loader hands it and copies the kind and base into a
// fixed slot here, with atomic operations and nothing else: no allocation, no thread-local storage, no call out
// of this module. The patches are applied by whoever drains the queue outside the lock: the DllLoadHook
// post-load observer when the game's LoadLibraryW returns, before the game resolves any export of the module
// (docs/reference/pnsrad-load-boundary.md), and the frame tick for a load that did not pass through
// LoadLibrary*.

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>

#include "runtime/patch/matchmaker_host_patch.h"

namespace nevr_pnsrad_load {

enum class Module : std::uint8_t {
  Pnsrad = 1,       // pnsrad.dll: the login and provider patches, applied once
  Matchmaking = 2,  // pnsradmatchmaking.dll: the matchmaker host default, applied on every load (#18)
};

constexpr char kPnsradName[] = "pnsrad.dll";

/// The module a loader notification's BaseDllName (`chars` UTF-16 code units, not NUL-terminated) names, or
/// nullopt for every other module. ASCII case-insensitive, whole name.
template <typename Ch>
std::optional<Module> Classify(const Ch* name, std::size_t chars) {
  if (nevr_matchmaker_host_patch::IsMatchmakingModule(name, chars)) return Module::Matchmaking;
  constexpr std::size_t kLength = sizeof(kPnsradName) - 1;
  if (name == nullptr || chars != kLength) return std::nullopt;
  for (std::size_t i = 0; i < kLength; ++i) {
    auto c = static_cast<unsigned>(name[i]);
    if (c >= 'A' && c <= 'Z') c = c - 'A' + 'a';
    if (c != static_cast<unsigned char>(kPnsradName[i])) return std::nullopt;
  }
  return Module::Pnsrad;
}

class PendingLoads {
 public:
  static constexpr std::size_t kCapacity = 16;

  /// Records one load. Runs inside the loader notification: lock-free, allocation-free, no calls.
  /// False when every slot is taken; the load is then counted in TakeDropped().
  bool Note(Module module, std::uintptr_t base) noexcept {
    for (auto& slot : slots_) {
      std::uint8_t expected = kFree;
      if (!slot.state.compare_exchange_strong(expected, kWriting, std::memory_order_acq_rel)) continue;
      slot.module.store(static_cast<std::uint8_t>(module), std::memory_order_relaxed);
      slot.base.store(base, std::memory_order_relaxed);
      slot.epoch.store(epoch_.load(std::memory_order_acquire), std::memory_order_relaxed);
      slot.state.store(kReady, std::memory_order_release);
      return true;
    }
    dropped_.fetch_add(1, std::memory_order_relaxed);
    return false;
  }

  /// Hands every recorded load to `apply(Module, base)` once and frees its slot. Returns how many it handed
  /// over. Safe against a concurrent Note() and a concurrent Drain(): each record is claimed by one drain.
  template <typename Apply>
  std::size_t Drain(Apply&& apply) {
    return DrainNotedBefore(UINT64_MAX, std::forward<Apply>(apply));
  }

  /// Ends the current epoch and returns it. A record Note() makes carries the epoch current at the time.
  std::uint64_t AdvanceEpoch() noexcept { return epoch_.fetch_add(1, std::memory_order_acq_rel); }

  /// Drain() limited to records noted in an epoch before `epoch`. The frame tick passes the epoch it just
  /// ended, so a record is old enough for it only after a whole tick interval: a load whose LoadLibrary is
  /// still returning on another thread is left to that return.
  template <typename Apply>
  std::size_t DrainNotedBefore(std::uint64_t epoch, Apply&& apply) {
    std::size_t drained = 0;
    for (auto& slot : slots_) {
      std::uint8_t expected = kReady;
      if (!slot.state.compare_exchange_strong(expected, kDraining, std::memory_order_acq_rel)) continue;
      if (slot.epoch.load(std::memory_order_relaxed) >= epoch) {
        slot.state.store(kReady, std::memory_order_release);
        continue;
      }
      const auto module = static_cast<Module>(slot.module.load(std::memory_order_relaxed));
      const std::uintptr_t base = slot.base.load(std::memory_order_relaxed);
      slot.state.store(kFree, std::memory_order_release);
      apply(module, base);
      ++drained;
    }
    return drained;
  }

  /// Loads Note() could not record since the last call (each one is a module left unpatched).
  std::uint64_t TakeDropped() noexcept { return dropped_.exchange(0, std::memory_order_acq_rel); }

 private:
  static constexpr std::uint8_t kFree = 0;
  static constexpr std::uint8_t kWriting = 1;
  static constexpr std::uint8_t kReady = 2;
  static constexpr std::uint8_t kDraining = 3;

  struct Slot {
    std::atomic<std::uint8_t> state{kFree};
    std::atomic<std::uint8_t> module{0};
    std::atomic<std::uintptr_t> base{0};
    std::atomic<std::uint64_t> epoch{0};
  };

  std::array<Slot, kCapacity> slots_{};
  std::atomic<std::uint64_t> dropped_{0};
  std::atomic<std::uint64_t> epoch_{0};
};

}  // namespace nevr_pnsrad_load
