/* The pinned slots of the global_mp level-name module (levelname.h): the four vtable words that hold CR15Game::LoadLevelName, and the
 * game code the module calls and the offset it reads.
 *
 * Every address is an ELF virtual address in the store libr15 (build ID b243509c...; the patched file the sentinel runs on has the same
 * layout at these words, ADR 0021). The disassembly's address is the ELF address plus 0x100000. src/quest/tests/got_pinned_test.cpp checks each
 * row against the real file: the four words are R_AARCH64_ABS64 relocations that name the symbol, the stub is the PLT stub of the symbol its
 * JUMP_SLOT names, and the code words below are the ones the module's reading of the lowspec bit rests on.
 *
 *   CR15Game::LoadLevelName 0x1202ecc, 44 bytes (`this` in x0, the level's CSymbol64 in x1, the name to load in x0):
 *     0x1202ecc mov  w8, #0x7ad8       the lowspec flags word is this+0x7ad8
 *     0x1202ed0 ldr  w8, [x0, x8]
 *     0x1202ed4 tbnz w8, #0x1f, ...    bit 31 set (a Quest's "lowspec"): append "_lowspec" to the name (CSymbol64::Append stub 0xf31ea0)
 *     0x1202ed8 b    0xf133d0          bit 31 clear: the plain function, CncaGame::LoadLevelName, through its PLT stub, which returns the name
 *   The four vtables of the CR15Game class tree (CR15Game, CR15NetDedicatedServer, CR15NetLanServer, CR15NetServer) hold the function at
 *   symbol + 0x270, as R_AARCH64_ABS64 relocations in .data.rel.ro; no class of the tree overrides it. CncaGameSpace::LoadLevel
 *   (0x17e5484 in the ELF's address, 0x18e5484 in the disassembly's) is the one virtual call through that slot (`ldr x8,[x8,#0x260]; blr x8`).
 *
 * Built with -fno-exceptions, like every translation unit that includes callback_thunk.h.
 */
#pragma once

#include <cstddef>
#include <cstdint>

#include "quest/sentinel/callback_thunk.h"
#include "quest/sentinel/got_hook.h"
#include "quest/sentinel/pinned_targets.h"

namespace nevr_quest::combat::levelname::targets {

inline constexpr const char* kSymbol = "_ZNK10NRadEngine8NRadGame8CR15Game13LoadLevelNameENS_9CSymbol64E";
inline constexpr std::uint64_t kFnVaddr = 0x1202ecc;  // CR15Game::LoadLevelName: what each word holds
inline constexpr std::size_t kSlotCount = 4;
inline constexpr std::uint64_t kSlots[kSlotCount] = {0x362ef90, 0x3630e58, 0x3631278, 0x36316f8};  // CR15Game, CR15NetDedicatedServer, CR15NetLanServer, CR15NetServer
inline constexpr const char* kSlotNames[kSlotCount] = {"cr15_game", "dedicated_server", "lan_server", "net_server"};

// The plain function the module calls for a lowspec global_mp: the PLT stub of CncaGame::LoadLevelName (JUMP_SLOT 0x36e10d0, defined at 0x17f162c,
// `mov x0, x1; ret`: it returns the name). A stub is a function pointer to call: it uses x16 and x17 only.
inline constexpr const char* kPlainSymbol = "_ZNK10NRadEngine8CncaGame13LoadLevelNameENS_9CSymbol64E";
inline constexpr std::uint64_t kPlainStubVaddr = 0xf133d0;
inline constexpr std::uint64_t kPlainSlotVaddr = 0x36e10d0;
inline constexpr std::uint64_t kPlainFnVaddr = 0x17f162c;

inline constexpr std::uint64_t kGlobalMp = 0x3F9915D3001DC28EULL;  // CSymbol64 of the level the stock cuts to global_mp_lowspec
inline constexpr std::uintptr_t kFlagOffset = 0x7ad8;               // CR15Game+0x7ad8: bit 31 = lowspec
inline constexpr std::uint32_t kLowspecMask = 0x80000000U;

// The instruction words that carry the two constants above, tied to them at compile time: a `mov w8, #imm` is 0x52800008 | imm << 5, and
// `tbnz w8, #31` tests bit b5:b40 with b5 in bit 31 (0) and b40 in bits 19-23 (0x1f), is the "not zero" form (bit 24) and tests w8.
struct PinnedWord {
  const char* what;
  std::uint64_t vaddr;
  std::uint32_t word;
};
inline constexpr PinnedWord kCodeWords[] = {
    {"LoadLevelName: the flags offset", 0x1202ecc, 0x528f5b08U},         // mov  w8, #0x7ad8
    {"LoadLevelName: the flags load", 0x1202ed0, 0xb8686808U},           // ldr  w8, [x0, x8]
    {"LoadLevelName: the lowspec test", 0x1202ed4, 0x37f80048U},         // tbnz w8, #0x1f, +8
    {"LoadLevelName: the plain tail call", 0x1202ed8, 0x17f4413eU},      // b    0xf133d0
    {"LoadLevelName: the suffixed tail call", 0x1202ef4, 0x17f4bbebU},   // b    0xf31ea0 (CSymbol64::Append)
};
constexpr std::uintptr_t MovImmediate(std::uint32_t word) { return static_cast<std::uintptr_t>((word >> 5) & 0xffff); }
static_assert(MovImmediate(kCodeWords[0].word) == kFlagOffset, "the pinned `mov w8, #imm` does not carry kFlagOffset");
static_assert(((kCodeWords[2].word >> 31) & 1U) == 0U && ((kCodeWords[2].word >> 24) & 0x7fU) == 0x37U && ((kCodeWords[2].word >> 19) & 0x1fU) == 0x1fU &&
                  (kCodeWords[2].word & 0x1fU) == 8U,
              "the pinned test is not `tbnz w8, #31`");
static_assert(kLowspecMask == 1U << 31, "kLowspecMask is bit 31");

// One thunk per hooked slot (callback_thunk.h): the four words hold one function, and each gets its own entry and original.
using Sig = std::uint64_t(void* game, std::uint64_t name);
template <std::size_t I>
struct Tag {};
template <std::size_t I>
using Thunk = sentinel::CallbackThunk<Tag<I>, Sig>;

// Slot `i`'s word as a manager target: the patched libr15's build ID and the pinned word. The word must hold the function itself
// (`expectedOriginal`, set by the install from the image's base): a word something else replaced is refused, not chained over.
inline sentinel::GotTarget Target(std::size_t i) {
  return sentinel::GotTarget{sentinel::pinned::kLibR15, kSymbol, sentinel::RelocKind::kAbs64, sentinel::pinned::LibR15PatchedBuildId(), kSlots[i]}
      .Owned("combat/levelname");
}

}  // namespace nevr_quest::combat::levelname::targets
