/* Pinned instruction words and the fields they carry, for the combat modules that call game functions and read game objects without a hook
 * (docs/adr/0046 rule 2: nothing here writes a byte). A module's `<module>_targets.h` lists the words of the game functions that load or store each
 * offset the module reads, and ties the offset to the word at compile time (the decoders below), so a changed offset or a changed word fails the
 * build, and a different libr15 fails `WordsHold` at install (and got_pinned_test.cpp against the store file).
 *
 * Header only. Built with -fno-exceptions, like every combat translation unit.
 */
#pragma once

#include <cstddef>
#include <cstdint>

#include "quest/combat/safe_read.h"

namespace nevr_quest::combat::pinned {

// One instruction of the store libr15: what it is, its ELF virtual address (the disassembly's address less 0x100000) and its word.
struct Word {
  const char* what;
  std::uint64_t vaddr;
  std::uint32_t word;
};

// One function of the game, by its exported name and its ELF virtual address.
struct Symbol {
  const char* symbol;
  std::uint64_t vaddr;
};

// The immediate of an unsigned-offset load or store, in bytes: `ldr x, [x, #imm]` (scale 8), `ldr w, [x, #imm]` (4), `ldrh w, [x, #imm]` (2),
// `ldr q, [x, #imm]` (16), `ldrb`/`strb` (1). Bits 10-21 of the word, times the access size.
constexpr std::uintptr_t LdrXOffset(std::uint32_t word) { return static_cast<std::uintptr_t>((word >> 10) & 0xfff) * 8; }
constexpr std::uintptr_t LdrWOffset(std::uint32_t word) { return static_cast<std::uintptr_t>((word >> 10) & 0xfff) * 4; }
constexpr std::uintptr_t LdrhOffset(std::uint32_t word) { return static_cast<std::uintptr_t>((word >> 10) & 0xfff) * 2; }
constexpr std::uintptr_t LdrQOffset(std::uint32_t word) { return static_cast<std::uintptr_t>((word >> 10) & 0xfff) * 16; }
constexpr std::uintptr_t LdrbOffset(std::uint32_t word) { return static_cast<std::uintptr_t>((word >> 10) & 0xfff); }
// `add x, x, #imm` and `mov w, #imm`.
constexpr std::uintptr_t AddImmediate(std::uint32_t word) { return static_cast<std::uintptr_t>((word >> 10) & 0xfff); }
constexpr std::uintptr_t MovImmediate(std::uint32_t word) { return static_cast<std::uintptr_t>((word >> 5) & 0xffff); }

// The first word at which `have` differs from the pinned `words[0..count)` in the libr15 loaded at `base`, or null when every word holds. An
// unreadable word is a mismatch (`readable` false).
inline const Word* FirstMismatch(std::uintptr_t base, const Word* words, std::size_t count, std::uint32_t* have, bool* readable) noexcept {
  for (std::size_t i = 0; i < count; ++i) {
    std::uint32_t word = 0;
    *readable = SafeRead(base + words[i].vaddr, &word, sizeof(word));
    *have = word;
    if (!*readable || word != words[i].word) return &words[i];
  }
  return nullptr;
}

}  // namespace nevr_quest::combat::pinned
