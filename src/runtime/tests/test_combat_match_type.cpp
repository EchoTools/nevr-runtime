// #2289: the combat module's match control acts on the session game type the runtime stores. A game type with "private"
// in its name is a private match, any other is public, and no session yet is unknown (runtime/combat/match_type.h); the
// value comes from nevr_arena_round_time::SessionGameType, the same store the server's session messages write.
#include <gtest/gtest.h>

#include <string>

#include "runtime/combat/match_type.h"
#include "runtime/lifecycle/arena_round_time.h"
#include "runtime/server/game_type_table.h"

namespace {

using nevr_combat::MatchPrivacy;
using nevr_combat::PrivacyOf;
using nevr_combat::PrivacyText;

TEST(CombatMatchType, NoSessionIsUnknown) {
  EXPECT_EQ(PrivacyOf(std::nullopt), MatchPrivacy::kUnknown);
  EXPECT_STREQ(PrivacyText(MatchPrivacy::kUnknown), "match type unknown");
}

TEST(CombatMatchType, EveryKnownGameTypeIsPrivateExactlyWhenItsNameSaysSo) {
  int privates = 0;
  for (const auto& t : nevr_game_type_table::kGameTypes) {
    const std::string name = t.name;
    const bool private_name = name.size() > 8 && name.compare(name.size() - 8, 8, "_private") == 0;
    privates += private_name;
    EXPECT_EQ(PrivacyOf(name), private_name ? MatchPrivacy::kPrivate : MatchPrivacy::kPublic) << name;
  }
  EXPECT_EQ(privates, 3);  // echo_arena_private, echo_combat_private, social_2.0_private
}

TEST(CombatMatchType, TheTypesTheServiceSendsAreNamed) {
  EXPECT_EQ(PrivacyOf(std::string("social_2.0_private")), MatchPrivacy::kPrivate);
  EXPECT_EQ(PrivacyOf(std::string("echo_combat_private")), MatchPrivacy::kPrivate);
  EXPECT_EQ(PrivacyOf(std::string("echo_combat")), MatchPrivacy::kPublic);
  EXPECT_EQ(PrivacyOf(std::string("social_2.0")), MatchPrivacy::kPublic);
  // The server stores "unknown" for a type it does not know, and "none" for a session with no type: neither is private.
  EXPECT_EQ(PrivacyOf(std::string("unknown")), MatchPrivacy::kPublic);
  EXPECT_EQ(PrivacyOf(std::string("none")), MatchPrivacy::kPublic);
  EXPECT_STREQ(PrivacyText(MatchPrivacy::kPrivate), "private match");
  EXPECT_STREQ(PrivacyText(MatchPrivacy::kPublic), "not a private match");
}

TEST(CombatMatchType, TheStoredSessionGameTypeDrivesIt) {
  nevr_arena_round_time::ResetForTest();
  EXPECT_EQ(PrivacyOf(nevr_arena_round_time::SessionGameType()), MatchPrivacy::kUnknown);
  nevr_arena_round_time::SetSessionGameType("social_2.0_private");
  EXPECT_EQ(PrivacyOf(nevr_arena_round_time::SessionGameType()), MatchPrivacy::kPrivate);
  nevr_arena_round_time::SetSessionGameType("echo_combat");
  EXPECT_EQ(PrivacyOf(nevr_arena_round_time::SessionGameType()), MatchPrivacy::kPublic);
  nevr_arena_round_time::ResetForTest();
}

}  // namespace
