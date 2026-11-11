/* The CDN texture keys a read can be matched to (docs/adr/0045-cdn-texture-delivery.md; #2135, T2c).
 *
 * The resource request hook fills a request from a cargo shadow and logs `cargo_served`; that is where the request is filled,
 * not where the read ends. The read ends at CResourceManager::AsyncResourceIOCallback, which the menu cards hook owns (one
 * hook per GOT slot), and which sees the resource's name symbol and the buffer index but not the on-disk type the request was
 * keyed by. So the request hook records each served (name, index) here (NoteServed), and the callback asks (TouchRead) whether
 * the read it is finishing is one of them: the answer is how many reads of that key have finished, or 0 for a key never served
 * from cargo, so the callback writes `cargo_read` (status, bytes, the buffer's first bytes) at a key's 1st, 2nd, 4th ... read.
 *
 * Header-only, atomics only: no lock, no allocation, safe from the streaming thread and the callback's thread alike. A table
 * that is full leaves a key unrecorded, which only costs its `cargo_read` lines.
 */
#pragma once

#include <atomic>
#include <cstdint>

namespace nevr_quest::cargo_read {

inline constexpr unsigned kSlots = 256;
inline constexpr unsigned kProbeLimit = 32;

struct Slot {
  std::atomic<std::uint64_t> key{0};
  std::atomic<std::uint64_t> reads{0};
};

inline Slot g_slots[kSlots];

constexpr std::uint64_t Mix(std::uint64_t x) noexcept {
  x ^= x >> 30;
  x *= 0xbf58476d1ce4e5b9ULL;
  x ^= x >> 27;
  x *= 0x94d049bb133111ebULL;
  x ^= x >> 31;
  return x;
}

// The key of (name, buffer index). Never 0, which marks an empty slot.
constexpr std::uint64_t KeyOf(std::uint64_t name, std::uint64_t index) noexcept {
  const std::uint64_t h = Mix(Mix(name) ^ index);
  return h != 0 ? h : 1;
}

// Records that (name, index) was served from a cargo shadow. Idempotent.
inline void NoteServed(std::uint64_t name, std::uint64_t index) noexcept {
  const std::uint64_t key = KeyOf(name, index);
  const unsigned home = static_cast<unsigned>(key % kSlots);
  for (unsigned probe = 0; probe < kProbeLimit; ++probe) {
    Slot& slot = g_slots[(home + probe) % kSlots];
    std::uint64_t held = slot.key.load(std::memory_order_acquire);
    if (held == 0 && slot.key.compare_exchange_strong(held, key, std::memory_order_acq_rel)) return;
    if (held == key) return;
  }
}

// For a read that finished for (name, index): 0 when the key was never served from cargo, else how many reads of it have
// finished, this one included. Never records a key.
inline std::uint64_t TouchRead(std::uint64_t name, std::uint64_t index) noexcept {
  const std::uint64_t key = KeyOf(name, index);
  const unsigned home = static_cast<unsigned>(key % kSlots);
  for (unsigned probe = 0; probe < kProbeLimit; ++probe) {
    Slot& slot = g_slots[(home + probe) % kSlots];
    const std::uint64_t held = slot.key.load(std::memory_order_acquire);
    if (held == 0) return 0;  // the probe sequence ends at the first empty slot: keys are never removed
    if (held == key) return slot.reads.fetch_add(1, std::memory_order_relaxed) + 1;
  }
  return 0;
}

// Tests only.
inline void ResetForTest() noexcept {
  for (Slot& slot : g_slots) {
    slot.key.store(0, std::memory_order_relaxed);
    slot.reads.store(0, std::memory_order_relaxed);
  }
}

}  // namespace nevr_quest::cargo_read
