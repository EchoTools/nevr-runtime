#pragma once

// The decision behind the -noovr argument-lookup hook (patch/noovr_arglookup.cpp). Pure: no Windows and no game
// headers, so the rule is unit-tested without a game image (src/runtime/tests/test_cmdline_waiver.cpp).

#include <cstdint>

#include "nevr/symbol_hash.h"

namespace nevr_cmdline_waiver {

/// The symbol the game hashes "spectatorstream" to (case-insensitive, EchoVR::CSymbol64Hash).
inline constexpr std::uint64_t kSpectatorStreamSymbol = EchoVR::CSymbol64Hash("spectatorstream");

/// A site that may answer "found" for one symbol. The site is the return address of the lookup's call, as an offset
/// from the image base (VA 0x140116909 is RVA 0x116909).
struct Waiver {
  std::uint64_t symbol;
  std::uint64_t siteRva;
};

/// The one pinned site: the -noovr check in PreprocessCommandLine. The spectator-stream check's own site (RVA 0x116F39)
/// is not listed, so that lookup is never answered by this hook.
inline constexpr Waiver kWaivers[] = {{kSpectatorStreamSymbol, 0x116909}};

inline constexpr bool Waives(std::uint64_t symbol, std::uint64_t siteRva) {
  for (const Waiver& waiver : kWaivers) {
    if (waiver.symbol == symbol && waiver.siteRva == siteRva) return true;
  }
  return false;
}

struct Answer {
  std::int64_t value;
  bool waived;
};

/// What the hook returns to the game: the original answer, except that "absent" (-1) at a waived site is "found" (0).
/// The game's consumer is CMP RAX,-1 / JNE, so any value other than -1 skips the -spectatorstream assert.
inline constexpr Answer Decide(std::int64_t original, std::uint64_t symbol, std::uint64_t siteRva) {
  if (original == -1 && Waives(symbol, siteRva)) return Answer{0, true};
  return Answer{original, false};
}

}  // namespace nevr_cmdline_waiver
