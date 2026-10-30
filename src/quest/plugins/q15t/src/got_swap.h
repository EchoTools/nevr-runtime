// A checked swap of one pointer-sized data word (a GOT slot or any other data-page function pointer).
//
// Data pages only: the swap refuses a page that is executable, because the current Horizon OS denies execmod on the
// game's code pages and a write that leaves one non-executable crashes the game when that code runs.
// One writer at a time (process-wide lock); the word is aligned and pointer-sized, so a reader sees the old or the new
// value, never a mix.
#pragma once

#include <cstddef>
#include <cstdint>

namespace q15t {

enum class SwapStatus {
  kOk,
  kNullSlot,
  kNotAligned,
  kUnmapped,           // the slot is in no mapping of /proc/self/maps
  kExecutablePage,     // the slot's page is executable: never written
  kUnexpectedOriginal, // the word is not the value the caller expects
  kProtectFailed,      // the page-protection call to writable failed; nothing was written
  kRestoreFailed,      // the word was written and the old protection could not be put back (logged by the caller)
  kMapsUnreadable,
};

const char* StatusName(SwapStatus status);

struct Mapping {
  std::uintptr_t start = 0;
  std::uintptr_t end = 0;
  char perms[5] = {0, 0, 0, 0, 0};  // "rwxp"
  char path[256] = {0};             // empty for an anonymous mapping
};

// The mapping that holds `addr`, from /proc/self/maps. False when none does or the file cannot be read.
bool FindMapping(std::uintptr_t addr, Mapping* out);

struct SwapResult {
  SwapStatus status = SwapStatus::kNullSlot;
  std::uintptr_t original = 0;  // the word read under the lock (set whenever the slot was readable)
  int err = 0;                  // errno of the failing call, 0 otherwise
};

// Writes `replacement` into the word at `slot` if it currently equals `expected`. `expected` must not be zero.
SwapResult SwapWord(std::uintptr_t slot, std::uintptr_t expected, std::uintptr_t replacement);

}  // namespace q15t
