/* The code words a combat module's behaviour was derived from, pinned to the store libr15 and read from the loaded library before any slot is
 * touched (#2178). A module keeps its table in its own `<module>_targets.h`; this is the shared row type and the one check, so each module
 * says the same thing about a word that differs. There is no whole-file hash anywhere: the patched build ID the module's install checks, these
 * words and the slot's own expected word (`GotTarget::expectedOriginal`) are the whole gate, so a rebuilt or repacked binary that still carries
 * the same code is not refused and one that carries other code is, naming the word.
 *
 * A word is read with the never-faulting read (safe_read.h): a page that is not mapped is a mismatch that says `unreadable`, never a signal.
 * Header only.
 */
#pragma once

#include <cstddef>
#include <cstdint>

#include "quest/combat/safe_read.h"

namespace nevr_quest::combat {

// One pinned instruction word: its ELF vaddr in the store libr15 (the disassembly lists each 0x100000 higher) and the word.
struct PinnedWord {
  std::uint64_t vaddr;
  std::uint32_t word;
};

// The index of the first pin of `pins[0..count)` whose word at `base + vaddr` is not the pinned one, or `count` when every pin holds.
// `*have` gets the word read (0 when unreadable) and `*readable` whether it could be read; both are written for a mismatch only.
inline std::size_t FirstWordMismatch(std::uintptr_t base, const PinnedWord* pins, std::size_t count, std::uint32_t* have,
                                     bool* readable) noexcept {
  for (std::size_t i = 0; i < count; ++i) {
    std::uint32_t word = 0;
    const bool ok = SafeRead(base + static_cast<std::uintptr_t>(pins[i].vaddr), &word, sizeof(word));
    if (!ok || word != pins[i].word) {
      *have = ok ? word : 0;
      *readable = ok;
      return i;
    }
  }
  return count;
}

}  // namespace nevr_quest::combat
