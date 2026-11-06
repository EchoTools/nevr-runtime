/* The pinned slots, data words, code words and callee stubs of the impact-sound module (impactsnd.h).
 *
 * Every address is an ELF virtual address in the store libr15 (build ID in pinned_targets.h, `LibR15PatchedBuildId`); the disassembly's address
 * is the ELF address plus 0x100000. src/quest/tests/got_pinned_test.cpp checks the two relocations, the GLOB_DAT word of kSoundSettings, the
 * hit-code words, the stub words and D against the real file (`just test-quest-hooks-pinned`, which needs the store APK).
 *
 *   CR15NetBulletCS::LoadSoundSettings  JUMP_SLOT 0x36fa3c0 -> 0x2121d04   (`readelf -rW`; FUNC, 808 bytes; one caller, Initialize, through the PLT stub)
 *   CR15NetBulletCS::Shutdown           ABS64     0x36933e8 -> 0x21226f0   (a vtable word in .data.rel.ro; FUNC, 348 bytes)
 *   CR15NetBulletCS::kSoundSettings     GLOB_DAT  0x370fa70 -> 0x37af6a0   (OBJECT 8, .bss: the pointer to the impact table's map)
 *   D, the "default" symbol word the hit code XORs into the gun symbol   0x37af8d0 (.bss; the library's own static initialiser stores it)
 */
#pragma once

#include <cstddef>
#include <cstdint>

#include "quest/sentinel/got_hook.h"
#include "quest/sentinel/pinned_targets.h"

namespace nevr_quest::combat::impactsnd {

inline constexpr const char* kLoadSymbol = "_ZN10NRadEngine8NRadGame15CR15NetBulletCS17LoadSoundSettingsEv";
inline constexpr const char* kShutdownSymbol = "_ZN10NRadEngine8NRadGame15CR15NetBulletCS8ShutdownEv";

inline constexpr std::uintptr_t kDefaultSymVa = 0x37af8d0;      // D (.bss)
inline constexpr std::uintptr_t kSoundSettingsVa = 0x37af6a0;   // kSoundSettings (.bss)
inline constexpr std::uintptr_t kSoundSettingsGotVa = 0x370fa70;  // the GLOB_DAT slot of kSoundSettings
inline constexpr std::uintptr_t kLoadSlotVa = 0x36fa3c0;        // LoadSoundSettings' JUMP_SLOT
inline constexpr std::uintptr_t kLoadFnVa = 0x2121d04;          // LoadSoundSettings
inline constexpr std::uintptr_t kShutdownSlotVa = 0x36933e8;    // Shutdown's vtable word
inline constexpr std::uintptr_t kShutdownFnVa = 0x21226f0;      // Shutdown
inline constexpr std::uint64_t kDefaultSym = 0x2fd38a995f615b9eULL;  // CSymbol64("default"): the value D must hold

// The hit code that reads the row count and D (HandleBulletCollision): read-only guards, checked and never changed.
struct CodeWord {
  std::uintptr_t va;
  std::uint32_t want;
};
inline constexpr CodeWord kCode[4] = {
    {0x21208b4, 0xf9401ac1},  // ldr x1, [x22, #0x30]
    {0x21208b8, 0xb4000541},  // cbz x1, <log>
    {0x21208bc, 0xf000b468},  // adrp x8, <D page>
    {0x21208c8, 0xf944691b},  // ldr x27, [x8, #0x8d0]
};

// The stock loader's direct-call targets (ELF = disassembly VA - 0x100000). Each is a PLT stub that jumps on to the function, the same as the stock
// `bl`; the first word of each stub (its `adrp x16`) is pinned, and the rebuilt loader runs only when every one matches.
struct RebuildCallee {
  const char* name;
  std::uintptr_t elf;
  std::uint32_t firstWord;  // the stub's first instruction, little-endian
};
inline constexpr RebuildCallee kRebuildCallees[] = {
    {"CJson::CJson", 0xed7760, 0x90013f70u},
    {"CJson::~CJson", 0xedcae0, 0xb0013f50u},
    {"CJson::LoadFromProjectSourceDb", 0x0f00140, 0xf0013eb0u},
    {"CJson::Next", 0x0f28490, 0xf0013e10u},
    {"CJson::NameAt", 0x0f17d40, 0x90013e70u},
    {"CJson::TString", 0x0f237d0, 0xd0013e30u},
    {"CMemBlock::Resize", 0x0f3e400, 0x90013dd0u},
};

// The slot as a manager target on a libr15 loaded at `base`: the slot must hold the real function (base + fn) or the chain's oldest original.
inline sentinel::GotTarget LoadTarget(std::uintptr_t base) {
  sentinel::GotTarget target(sentinel::pinned::kLibR15, kLoadSymbol, sentinel::RelocKind::kJumpSlot, sentinel::pinned::LibR15PatchedBuildId(),
                             kLoadSlotVa);
  target.expectedOriginal = reinterpret_cast<const void*>(base + kLoadFnVa);
  return target.Owned("combat_impactsnd");
}
inline sentinel::GotTarget ShutdownTarget(std::uintptr_t base) {
  sentinel::GotTarget target(sentinel::pinned::kLibR15, kShutdownSymbol, sentinel::RelocKind::kAbs64, sentinel::pinned::LibR15PatchedBuildId(),
                             kShutdownSlotVa);
  target.expectedOriginal = reinterpret_cast<const void*>(base + kShutdownFnVa);
  return target.Owned("combat_impactsnd");
}

}  // namespace nevr_quest::combat::impactsnd
