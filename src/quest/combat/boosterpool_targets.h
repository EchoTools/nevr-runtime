/* The pinned slot of the booster-pool module (boosterpool.h): the JUMP_SLOT libr15 calls CncaGameSpace::AllocatePooledActor through, and the
 * game code and data the module relies on.
 *
 * Every address is an ELF virtual address in the store libr15 (build ID b243509c...; the patched file the sentinel runs on has the same layout at
 * these words, ADR 0021). The disassembly's address is the ELF address plus 0x100000. src/quest/tests/got_pinned_test.cpp checks each row against the
 * real file.
 *
 *   CR15NetBoosterCS::AllocateExclusive 0x2110dd4 (size 1616), from the word it pins:
 *     0x2110f34 ldr  w10, [x10, x11]      x11 = 0x7ad8: the lowspec flags word of the game (this+0x80)->+8
 *     0x2110f3c ldr  x20, [x11, #0x868]   the booster pool actor's plain CSymbol64, the u64 at 0x37af868
 *     0x2110f40 tbz  w10, #0x1f, ...      bit 31 clear: ask for the plain name
 *     0x2110f58 bl   0xf31ea0             bit 31 set: CSymbol64::Append(plain, "_lowspec", 1, 1) (the PLT stub), the name to ask for
 *     0x2110f70 bl   0xeeb620             CncaGameSpace::AllocatePooledActor(st, name), through its PLT stub, whose JUMP_SLOT is 0x36cd1f8
 *   AllocatePooledActor has twelve callers in libr15 (all reach it through that stub): the booster's AllocateExclusive, the bullet and magazine
 *   spawns (FireBulletLocal, SpawnBulletExclusive, SpawnMagazine), AddBotUser, SetRemoteUserActor, AllocateMarkerActorsFromPool, two functions named
 *   ExclusiveUpdate, UpdateExclusive and two unnamed ones (the ReVault call graph of libr15; calls per frame were not measured).
 *
 * Built with -fno-exceptions, like every translation unit that includes callback_thunk.h.
 */
#pragma once

#include <cstddef>
#include <cstdint>

#include "quest/sentinel/callback_thunk.h"
#include "quest/sentinel/got_hook.h"
#include "quest/sentinel/pinned_targets.h"

namespace nevr_quest::combat::boosterpool::targets {

inline constexpr const char* kSymbol = "_ZN10NRadEngine13CncaGameSpace19AllocatePooledActorENS_9CSymbol64E";
inline constexpr std::uint64_t kSlotVaddr = 0x36cd1f8;   // the JUMP_SLOT of the PLT stub at 0xeeb620 (ELF; the disassembly's 0xfeb620)
inline constexpr std::uint64_t kFnVaddr = 0x18037c4;     // CncaGameSpace::AllocatePooledActor: what the word holds
inline constexpr std::uint64_t kStubVaddr = 0xeeb620;
inline constexpr const char* kAppendSymbol = "_ZN10NRadEngine9CSymbol646AppendES0_PKcjj";
inline constexpr std::uint64_t kAppendStubVaddr = 0xf31ea0;  // CSymbol64::Append, the PLT stub of JUMP_SLOT 0x36f0638
inline constexpr std::uint64_t kAppendSlotVaddr = 0x36f0638;
inline constexpr std::uint64_t kPlainNameVaddr = 0x37af868;  // the booster pool actor's plain CSymbol64 (a u64 in libr15's data)
inline constexpr std::uint64_t kAllocateExclusiveVaddr = 0x2110dd4;
inline constexpr const char kSuffix[] = "_lowspec";

struct PinnedWord {
  const char* what;
  std::uint64_t vaddr;
  std::uint32_t word;
};
// The words of AllocateExclusive that build the suffixed name the module recognises, as `llvm-objdump -d` shows them in the store file.
inline constexpr PinnedWord kCodeWords[] = {
    {"AllocateExclusive: the flags offset", 0x2110f24, 0x528f5b0bU},      // mov  w11, #0x7ad8
    {"AllocateExclusive: the flags load", 0x2110f34, 0xb86b694aU},        // ldr  w10, [x10, x11]
    {"AllocateExclusive: the plain name's page", 0x2110f38, 0xf000b4ebU},     // adrp x11, 0x37af000
    {"AllocateExclusive: the plain name load", 0x2110f3c, 0xf9443574U},   // ldr  x20, [x11, #0x868]  (x11 = page 0x37af000)
    {"AllocateExclusive: the lowspec test", 0x2110f40, 0x36f8012aU},      // tbz  w10, #0x1f, +0x24
    {"AllocateExclusive: Append(plain, _lowspec, 1, 1)", 0x2110f58, 0x97b883d2U},  // bl 0xf31ea0
    {"AllocateExclusive: the actor request", 0x2110f70, 0x97b769acU},     // bl   0xeeb620
};

// SActor: the 16 bytes AllocatePooledActor returns in x0 and x1.
struct ActorPair {
  std::uint64_t id;
  std::uint64_t gen;
};
using Sig = ActorPair(void* st, std::uint64_t name);
struct Tag {};
using Thunk = sentinel::CallbackThunk<Tag, Sig>;

// The word must hold the function itself (`expectedOriginal`, set by the install from the image's base): a word something else replaced is refused.
inline sentinel::GotTarget Target() {
  return sentinel::GotTarget{sentinel::pinned::kLibR15, kSymbol, sentinel::RelocKind::kJumpSlot, sentinel::pinned::LibR15PatchedBuildId(), kSlotVaddr}
      .Owned("combat/boosterpool");
}

}  // namespace nevr_quest::combat::boosterpool::targets
