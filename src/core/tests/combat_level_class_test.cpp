// The shared combat level class rule (core/combat_level_class.h, PLAN-v7 R7a). The ids are the CSymbol64 of their names, in the order
// VeilNet's map switch indexes them (`LEVEL_SYM[k]` with `EVT_MAPSWITCH[k]`), which is the order of the podium's button names
// (match_control::kMapNames). combat_level_test (the Quest) checks membership only, so this file is the one that pins the order.

#include "core/combat_level_class.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>

#include "nevr/symbol_hash.h"
#include "runtime/combat/match_control_rules.h"

namespace {

namespace cl = nevr_combat_level;
using EchoVR::CSymbol64Hash;

TEST(CombatLevelClass, CombatMapsAreTheHashesOfTheirNamesInOrder) {
  constexpr const char* kExpected[cl::kCombatMapCount] = {"mpl_combat_dyson", "mpl_combat_combustion", "mpl_combat_fission",
                                                          "mpl_combat_gauss"};
  for (std::size_t i = 0; i < cl::kCombatMapCount; ++i) {
    EXPECT_STREQ(cl::kCombatMapNames[i], kExpected[i]) << "index " << i;
    EXPECT_EQ(cl::kCombatMaps[i], CSymbol64Hash(kExpected[i])) << "index " << i;
  }
}

TEST(CombatLevelClass, MapIndexMatchesThePodiumButtonNames) {
  // kMapNames is DYSON, COMBUSTION, FISSION, SURGE (SURGE = mpl_combat_gauss); index k of both is one map.
  ASSERT_EQ(static_cast<std::size_t>(nevr_combat::match_control::kMapCount), cl::kCombatMapCount);
  constexpr const char* kButtons[cl::kCombatMapCount] = {"DYSON", "COMBUSTION", "FISSION", "SURGE"};
  for (std::size_t i = 0; i < cl::kCombatMapCount; ++i) {
    EXPECT_STREQ(nevr_combat::match_control::kMapNames[i], kButtons[i]) << "index " << i;
  }
}

TEST(CombatLevelClass, TheOtherClassesAreTheHashesOfTheirNames) {
  EXPECT_EQ(cl::kCombatLobby, CSymbol64Hash("mpl_lobby_b_combat"));
  constexpr const char* kArena[4] = {"mpl_arena_a", "mpl_arena_a_lowspec", "mpl_arena_b", "mpl_tutorial_a"};
  for (std::size_t i = 0; i < 4; ++i) {
    EXPECT_EQ(cl::kEchoArenaLevels[i], CSymbol64Hash(kArena[i])) << "index " << i;
  }
}

TEST(CombatLevelClass, LobbyAndTutorialConstantsAreTheHashesOfTheirNames) {
  constexpr const char* kLobbies[cl::kLobbyLevelCount] = {"mpl_lobby_b",       "mpl_lobby_b2",     "mpl_lobby_b2_lowspec",
                                                          "mpl_lobby_b_arena", "mpl_lobby_b_social", "mpl_lobby_b_combat"};
  for (std::size_t i = 0; i < cl::kLobbyLevelCount; ++i) {
    EXPECT_EQ(cl::kLobbyLevels[i], CSymbol64Hash(kLobbies[i])) << "index " << i;
  }
  EXPECT_EQ(cl::kTutorialLobbyLevel, CSymbol64Hash("mpl_tutorial_lobby"));
}

TEST(CombatLevelClass, EachLevelIsInItsOwnClassOnly) {
  for (const char* name : cl::kCombatMapNames) {
    const std::uint64_t id = CSymbol64Hash(name);
    EXPECT_TRUE(cl::IsCombatMap(id)) << name;
    EXPECT_FALSE(cl::IsCombatLobby(id)) << name;
    EXPECT_FALSE(cl::IsEchoArenaLevel(id)) << name;
  }
  EXPECT_TRUE(cl::IsCombatLobby(CSymbol64Hash("mpl_lobby_b_combat")));
  EXPECT_FALSE(cl::IsCombatMap(CSymbol64Hash("mpl_lobby_b_combat")));
  for (const std::uint64_t id : cl::kEchoArenaLevels) {
    EXPECT_TRUE(cl::IsEchoArenaLevel(id));
    EXPECT_FALSE(cl::IsCombatMap(id));
    EXPECT_FALSE(cl::IsCombatLobby(id));
  }
}

TEST(CombatLevelClass, AnUnknownLevelIsOther) {
  for (const char* name : {"mpl_lobby_b2", "mpl_lobby_b", "mpl_combat_pebbles", "pty_mpl_combat_pebbles", "not_a_level"}) {
    const std::uint64_t id = CSymbol64Hash(name);
    EXPECT_FALSE(cl::IsCombatMap(id)) << name;
    EXPECT_FALSE(cl::IsCombatLobby(id)) << name;
    EXPECT_FALSE(cl::IsEchoArenaLevel(id)) << name;
  }
  EXPECT_FALSE(cl::IsCombatMap(0));
  EXPECT_FALSE(cl::IsCombatLobby(0));
  EXPECT_FALSE(cl::IsEchoArenaLevel(0));
}

}  // namespace
