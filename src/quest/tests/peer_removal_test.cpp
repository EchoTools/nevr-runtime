// Host test of the Quest peer removal log (src/quest/integration/peer_removal_hook.cpp).
//
// What it pins: the table's thunk decodes to the GOT slot and the body the table names (the thunk is the four words read from libr15,
// peer_removal_targets.h); an install with no libr15 refuses with kModuleNotLoaded and installs nothing; the removal's level is the shared
// one (src/runtime/compat/peer_timeout_log.h). What it does not pin: a full install into a libr15 image (the slot's relocation and the pinned
// original are checked by the GOT backend's own tests, not here), and the handler's call to the game's function (no libr15 image here).
//
// Mutants (one line each, against peer_removal_hook.cpp and peer_removal_targets.h):
//   M1 kDestroyPeerSlotOffset 0xe0 -> 0xe8                  -> TheThunkDecodesToItsSlotAndBody fails.
//   M2 kDestroyPeerFnVa off by 0x10                         -> TheThunkDecodesToItsSlotAndBody fails (the body is not checked here: see the note).
//   M3 InstallPeerRemovalHookAt: null lookup -> kOk          -> ARefusedInstallWithoutLibR15 fails.

#include <cstdint>

#include "quest/integration/peer_removal_hook.h"
#include "quest/integration/peer_removal_targets.h"
#include "quest/tests/test_check.h"
#include "runtime/compat/peer_timeout_log.h"

namespace {

using nevr_quest::integration::peer_removal::InstallPeerRemovalHookAt;
using nevr_quest::integration::peer_removal::Installed;
namespace targets = nevr_quest::integration::peer_removal;

// The thunk's four words, read little-endian from the bytes revault printed at 0x10533f0:
//   70 3d 01 d0 -> adrp x16, page      11 72 40 f9 -> ldr x17, [x16, #0xe0]
//   10 82 03 91 -> add x16, x16, #0xe0 20 02 1f d6 -> br x17
constexpr std::uint32_t kAdrpWord = 0xD0013D70U;
constexpr std::uint32_t kLdrWord = 0xF9407211U;
constexpr std::uint32_t kAddWord = 0x91038210U;
constexpr std::uint32_t kBrWord = 0xD61F0220U;

// adrp: immlo = bits 29..30, immhi = bits 5..23, the page is (pc & ~0xfff) + imm << 12.
std::uintptr_t AdrpPage(std::uintptr_t pc, std::uint32_t word) {
  const std::uint32_t immlo = (word >> 29) & 0x3U;
  const std::uint32_t immhi = (word >> 5) & 0x7FFFFU;
  const std::uintptr_t imm = (static_cast<std::uintptr_t>(immhi) << 2) | immlo;
  return (pc & ~static_cast<std::uintptr_t>(0xFFF)) + (imm << 12);
}

void TheThunkDecodesToItsSlotAndBody() {
  const std::uintptr_t thunk = targets::kDestroyPeerThunkVa;
  QCHECK((kBrWord & 0xFFFFFC1FU) == 0xD61F0000U);  // br xN
  // The page of the adrp, and the ldr and add offsets (imm12 scaled by 8 for ldr of a 64-bit word).
  QCHECK(AdrpPage(thunk, kAdrpWord) == targets::kDestroyPeerSlotPageVa);
  const std::uint32_t ldrImm = (kLdrWord >> 10) & 0xFFFU;
  QCHECK(static_cast<std::uintptr_t>(ldrImm) * 8 == targets::kDestroyPeerSlotOffset);
  const std::uint32_t addImm = (kAddWord >> 10) & 0xFFFU;
  QCHECK(static_cast<std::uintptr_t>(addImm) == targets::kDestroyPeerSlotOffset);
  QCHECK(targets::kDestroyPeerSlotPageVa + targets::kDestroyPeerSlotOffset == targets::kDestroyPeerSlotVa);
  // The body the table names is the one the thunk's slot is pinned to (kDestroyPeerFnVa), and the thunk is the one the call sites
  // reach (CALLSITES-2: `bl 0x10533f0`); the body's own bytes are not in this repo, so only the table's relations are checked here.
  QCHECK(targets::kDestroyPeerFnVa == 0x2612CDCULL);
  QCHECK(targets::kDestroyPeerThunkVa == 0x10533F0ULL);
}

void ARefusedInstallWithoutLibR15() {
  QCHECK(!Installed());
  QCHECK_STATUS(InstallPeerRemovalHookAt(nullptr), sentinel::GotStatus::kModuleNotLoaded);
  QCHECK(!Installed());
}

void UnknownReasonsAreWarningsOnTheSharedRule() {
  // The Quest line takes its level from the shared function (LogRemoval): an unknown reason is a Warning, a timeout a Warning, a
  // disconnect Info.
  QCHECK(nevr_peer_timeout_log::LevelForRemovalReason("something new") == nevr_peer_timeout_log::RemovalLevel::kWarning);
  QCHECK(nevr_peer_timeout_log::LevelForRemovalReason("communication timeout") == nevr_peer_timeout_log::RemovalLevel::kWarning);
  QCHECK(nevr_peer_timeout_log::LevelForRemovalReason("disconnect received") == nevr_peer_timeout_log::RemovalLevel::kInfo);
  QCHECK(!nevr_peer_timeout_log::RecognizedRemovalReason("something new"));
}

}  // namespace

int main() {
  TheThunkDecodesToItsSlotAndBody();
  ARefusedInstallWithoutLibR15();
  UnknownReasonsAreWarningsOnTheSharedRule();
  if (quest_test::Failures() != 0) {
    std::fprintf(stderr, "peer_removal_test: %d failure(s)\n", quest_test::Failures());
    return 1;
  }
  std::printf("peer_removal_test: OK\n");
  return 0;
}
