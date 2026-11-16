// The combat level class rule, one copy for the PC and the Quest (PLAN-v7 R7a; docs/adr/0046, docs/adr/0045 decision 10).
//
// A level is a CSymbol64 (`EchoVR::CSymbol64Hash` of its name). Three classes, moved without change from the Quest's
// combat_level.cpp: the four combat maps, the combat lobby (`mpl_lobby_b_combat`) and the Echo Arena levels. The order of
// `kCombatMaps` is the map index VeilNet's map switch uses on the PC (`LEVEL_SYM[k]` with `EVT_MAPSWITCH[k]` and
// `MAP_BUTTON_ACTOR[k]`), and it is the order of `nevr_combat::match_control::kMapNames`; `kCombatMapNames` is in that order, and
// core/tests/combat_level_class_test.cpp pins both orders by hashing the names.
//
// Header-only, constexpr and including `<cstdint>` only: every combat translation unit builds with -fno-exceptions, and this file
// needs no SDK include.
#pragma once

#include <cstddef>
#include <cstdint>

namespace nevr_combat_level {

inline constexpr std::size_t kCombatMapCount = 4;

inline constexpr std::uint64_t kCombatMaps[kCombatMapCount] = {0x43e2da7914642604ULL, 0x42670f2bed45703cULL, 0xdf5ca7b7dfa383d4ULL,
                                                               0x43e2da7a0c623a19ULL};
inline constexpr const char* kCombatMapNames[kCombatMapCount] = {"mpl_combat_dyson", "mpl_combat_combustion", "mpl_combat_fission",
                                                                 "mpl_combat_gauss"};
inline constexpr std::uint64_t kCombatLobby = 0xcb9977f7fc2b4526ULL;  // mpl_lobby_b_combat
inline constexpr std::uint64_t kEchoArenaLevels[4] = {0x576ed3f8428ebc4bULL, 0x7691d78d414c5701ULL, 0x576ed3f8428ebc48ULL,
                                                      0x3f20b99c97955721ULL};
// The lobby levels a session returns to (src/quest/combat/combat_level.cpp), the combat lobby among them (kCombatLobby).
inline constexpr std::size_t kLobbyLevelCount = 6;
inline constexpr std::uint64_t kLobbyLevels[kLobbyLevelCount] = {0x5763ceff4e96bc48ULL, 0xd09afd15b1c75c04ULL, 0x05361c73bb73db19ULL,
                                                                 0x6daa00a6d33d44b7ULL, 0xcb9967f7f220453eULL, kCombatLobby};
// mpl_tutorial_lobby, the lobby an NPE social session loads (symbol_corpus.cpp).
inline constexpr std::uint64_t kTutorialLobbyLevel = 0x3c8d74713ced8c3fULL;

namespace detail {
template <std::size_t N>
constexpr bool In(std::uint64_t level, const std::uint64_t (&set)[N]) noexcept {
  for (const std::uint64_t v : set) {
    if (level == v) return true;
  }
  return false;
}
}  // namespace detail

constexpr bool IsCombatMap(std::uint64_t level) noexcept { return detail::In(level, kCombatMaps); }
constexpr bool IsCombatLobby(std::uint64_t level) noexcept { return level == kCombatLobby; }
constexpr bool IsEchoArenaLevel(std::uint64_t level) noexcept { return detail::In(level, kEchoArenaLevels); }

}  // namespace nevr_combat_level
