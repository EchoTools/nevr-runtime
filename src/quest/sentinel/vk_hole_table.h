/* The array of free ranges libr15's Vulkan allocator keeps, and the rule that tells when a free would overflow it (#1759).
 *
 * What the game does (pinned libr15; pinned_targets.h has the addresses and the words they were read from).
 * NRadEngine::CGRawInterfaceVK::Allocate cuts an allocation out of a free range of a VkDeviceMemory block it already holds, and
 * makes a new block (at least 32 MiB) when none fits. NRadEngine::CGRawInterfaceVK::Free(memory, begin, _, end) gives a range
 * back. Both keep the free ranges in one array in .bss: 512 entries of 0x28 bytes, then a 32-bit count in the word right after
 * the last entry. An entry is {memory, begin, end, two more words}; the range is [begin, end).
 *
 * Free scans the entries of `memory` for one that touches the freed range: an entry whose begin is the freed end grows down to
 * the freed begin; an entry whose end is the freed begin grows up to the freed end. When none touches it, and `memory` has an
 * entry, it appends a new entry at index `count` and stores `count + 1`. It compares the count with nothing. When `memory` has no
 * entry at all the free is the whole block (vkFreeMemory), and nothing is appended. Allocate appends the same way for a new block.
 *
 * So the 513th entry is written over the count: the count becomes the low word of the VkDeviceMemory handle, and the next scan
 * of the array (Free's, and Allocate's) runs `count` entries past the end of .bss. The three crash offsets of #1759 are the loads
 * of those scans (libr15 +0x1885480 / +0x1885630 / +0x18856c8). That the count passes 512 in a real run is the claim the crash
 * reports have to show (their register x11 at the fault); this header is what makes it harmless when it does.
 *
 * The rule. `Judge` says whether Free may run: the answer is "leak" (do not call the game's Free, so the range stays allocated)
 * only when the call would append and the array has no room for it. The array's real limit is 512; the guard starts at kGuardFrom
 * so that several threads that read the same count at once (the game takes its lock inside Free, the guard cannot) still fit.
 *
 * Pure: no hook, no Android. The pinned layout is in pinned_targets.h (vk_hole_layout); the test checks the two agree.
 */
#pragma once

#include <cstddef>
#include <cstdint>

namespace nevr_quest::vk_hole {

inline constexpr std::uint32_t kCapacity = 512;
// Entries left for threads that passed the check together. The game's other append, Allocate's for a new block, is not guarded
// and uses them too: it adds at most one entry per block of 32 MiB or more.
inline constexpr std::uint32_t kHeadroom = 8;
inline constexpr std::uint32_t kGuardFrom = kCapacity - kHeadroom;

struct Entry {
  std::uint64_t memory;  // the VkDeviceMemory handle
  std::uint64_t begin;   // the free range is [begin, end)
  std::uint64_t end;
  std::uint64_t aux0;    // copied from the block's other entry on an append; not read here
  std::uint64_t aux1;
};
static_assert(sizeof(Entry) == 0x28, "the game's entry is 0x28 bytes");
static_assert(offsetof(Entry, begin) == 8 && offsetof(Entry, end) == 0x10, "Free reads begin at +8 and end at +0x10");

enum class Verdict : std::uint8_t {
  kRun,      // call the game's Free
  kLeak,     // the free would append to a full array: do not call it
  kCorrupt,  // the count is past the array already: every scan is out of bounds, and Free would run it
};

inline const char* VerdictName(Verdict verdict) noexcept {
  switch (verdict) {
    case Verdict::kRun: return "run";
    case Verdict::kLeak: return "leak";
    case Verdict::kCorrupt: return "corrupt";
  }
  return "run";
}

// Whether Free(memory, begin, _, end) would append an entry, by the rule above, over the first min(count, kCapacity) entries.
// A count of 0 never appends (Free takes the whole-block path).
inline bool WouldAppend(const Entry* table, std::uint32_t count, std::uint64_t memory, std::uint64_t begin,
                        std::uint64_t end) noexcept {
  const std::uint32_t scanned = count < kCapacity ? count : kCapacity;
  bool tracked = false;
  for (std::uint32_t i = 0; i < scanned; ++i) {
    if (table[i].memory != memory) continue;
    if (table[i].begin == end || table[i].end == begin) return false;  // the entry grows in place
    tracked = true;
  }
  return tracked;
}

// The verdict for one call. A count below kGuardFrom is always kRun and reads no entry.
inline Verdict Judge(const Entry* table, std::uint32_t count, std::uint64_t memory, std::uint64_t begin,
                     std::uint64_t end) noexcept {
  if (count < kGuardFrom) return Verdict::kRun;
  if (count > kCapacity) return Verdict::kCorrupt;
  return WouldAppend(table, count, memory, begin, end) ? Verdict::kLeak : Verdict::kRun;
}

}  // namespace nevr_quest::vk_hole
