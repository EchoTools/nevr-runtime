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

// Eight bytes of the game's data that a constant of ours is, read from the file: the CSymbol64 record a component system's DefaultName returns, for one.
struct Quad {
  const char* what;
  std::uint64_t vaddr;
  std::uint64_t value;
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
// `stur q, [x, #imm]`: the signed 9-bit offset in bits 12-20.
constexpr std::uintptr_t SturOffset(std::uint32_t word) { return static_cast<std::uintptr_t>((word >> 12) & 0x1ff); }
// `add x, x, #imm` and `mov w, #imm`.
constexpr std::uintptr_t AddImmediate(std::uint32_t word) { return static_cast<std::uintptr_t>((word >> 10) & 0xfff); }
constexpr std::uintptr_t MovImmediate(std::uint32_t word) { return static_cast<std::uintptr_t>((word >> 5) & 0xffff); }

// The lists one module pins: instruction words and 8-byte records.
struct Pins {
  const Word* words;
  std::size_t wordCount;
  const Quad* quads;
  std::size_t quadCount;
};

// What failed to hold: the word or record, what it holds (`have`, valid when `readable`) and what is pinned.
struct Failure {
  const char* what = "";
  std::uint64_t vaddr = 0;
  std::uint64_t have = 0;
  std::uint64_t want = 0;
  bool readable = true;
};

// The first pinned word or record that is not the file's in the libr15 loaded at `base`, over every list; false when all hold.
inline bool FirstFailure(std::uintptr_t base, const Pins* lists, std::size_t listCount, Failure* out) noexcept {
  for (std::size_t l = 0; l < listCount; ++l) {
    for (std::size_t i = 0; i < lists[l].wordCount; ++i) {
      const Word& w = lists[l].words[i];
      std::uint32_t have = 0;
      const bool readable = SafeRead(base + w.vaddr, &have, sizeof(have));
      if (!readable || have != w.word) {
        *out = Failure{w.what, w.vaddr, have, w.word, readable};
        return true;
      }
    }
    for (std::size_t i = 0; i < lists[l].quadCount; ++i) {
      const Quad& q = lists[l].quads[i];
      std::uint64_t have = 0;
      const bool readable = SafeRead(base + q.vaddr, &have, sizeof(have));
      if (!readable || have != q.value) {
        *out = Failure{q.what, q.vaddr, have, q.value, readable};
        return true;
      }
    }
  }
  return false;
}

}  // namespace nevr_quest::combat::pinned
