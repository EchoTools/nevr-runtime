/* A free that would overflow libr15's array of free GPU ranges is not run (#1759).
 *
 * What the game does and why it crashes: vk_hole_table.h. One GOT hook, on libr15's own NRadEngine::CGRawInterfaceVK::Free slot
 * (pinned_targets.h, LibR15VkFree). The handler reads the array's count (a 32-bit word in .bss) and, from kGuardFrom entries up,
 * asks vk_hole::Judge about the call. kRun calls the game's Free. kLeak does not: the range stays allocated inside its 32 MiB
 * block (a leak of GPU memory, counted and logged), which is the alternative to writing the 513th entry over the count and
 * crashing the next scan. kCorrupt means the count is already past the array, which only a write past the array could have done;
 * Free would scan past .bss, so it is not called either (the game's Allocate scans the same array and still faults, which the log
 * line names).
 *
 * The check is a read of the count and a compare; no lock is taken (the game's lock is inside Free), no entry is read below
 * kGuardFrom. The array is read, never written.
 *
 * Install is gated on the pinned build: the slot at the pinned link-time address, Free's first words, the words that load the
 * count and the scan start, and the words that append (pinned_targets.h) must all be the pinned image's; one that is not refuses
 * the hook and logs the address and the bytes. The hook is installed on every launch of a pinned build and does nothing until the
 * array is nearly full.
 *
 * Log lines (sentinel log, hook_log.h):
 *   install   {"event":"vk_free_guard_install","result":"installed|refused","status":...,"slot":...,"why":...}
 *   high water {"event":"vk_hole_table","level":"high_water","count":N,"capacity":512,...}
 *             once for each of 128, 256, 384 and kGuardFrom entries, so a crash report's last log lines say how full the array was
 *   leak      {"event":"vk_free_leak","verdict":"leak|corrupt","count":N,"memory":"0x...","begin":"0x...","end":"0x...","leaked":K}
 *             the first leak, then every 1024th
 *
 * Built with -fno-exceptions, like every translation unit that includes callback_thunk.h.
 */
#pragma once

#include <cstdint>

#include "pinned_targets.h"
#include "vk_hole_table.h"

namespace nevr_quest::vk_free_guard {

using Thunk = sentinel::pinned::LibR15VkFreeThunk;

struct Counts {
  std::uint64_t leaked;      // frees not run because they would have appended to a full array
  std::uint64_t corrupt;     // frees not run because the count was already past the array
  std::uint32_t high_water;  // the largest count the handler has read
};
Counts CurrentCounts() noexcept;

// Installs the hook into libr15's slot after proving the pinned words. True when the slot holds the thunk. Logs one
// vk_free_guard_install line either way.
bool Install() noexcept;

// The install on a given image and slot (the host test passes an image it laid out, and a slot that does not resolve).
bool InstallAt(unsigned char* base, const sentinel::GotTarget& target) noexcept;

// Test support: arms the handler on a count word and an array of entries, without touching a GOT slot.
// ResetForTest clears the counters and the once-only lines and disarms.
void ArmForTest(const volatile std::uint32_t* count, const vk_hole::Entry* table) noexcept;
void ResetForTest() noexcept;

}  // namespace nevr_quest::vk_free_guard
