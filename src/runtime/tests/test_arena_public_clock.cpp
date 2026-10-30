// #982: a public Arena match's clock keeps counting through goals on a server. The decisions and the script-site
// checks of patch/arena_public_clock_rules.h.

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

#include "runtime/patch/arena_public_clock_rules.h"

namespace {

using namespace nevr_arena_public_clock;

std::optional<std::string> Type(const char* t) { return std::optional<std::string>(t); }

// A node state as a script builds it: the flag id at +0x20, the value byte at +0x28 (08317e89cc3ae611.dll
// 0x18001b3c0 stores a 1 first, then the computed value).
std::array<std::uint8_t, 0x38> State(std::uint64_t flagId, std::uint8_t value) {
  std::array<std::uint8_t, 0x38> state{};
  std::memcpy(state.data() + kStateNameOffset, &flagId, sizeof(flagId));
  state[kStateValueOffset] = value;
  return state;
}

TEST(ArenaPublicClock, PublicArenaServerForcesTheFlagFalse) {
  auto state = State(kPublicMatchFlag, 1);
  const FlagStoreOutcome outcome = ApplyToFlagState(state.data(), true, Type("echo_arena"));
  EXPECT_TRUE(outcome.isPublicMatchFlag);
  EXPECT_TRUE(outcome.decision.forceFalse);
  EXPECT_EQ(outcome.decision.reason, "applied");
  EXPECT_EQ(outcome.valueBefore, 1);
  EXPECT_EQ(outcome.valueAfter, 0);
  EXPECT_EQ(state[kStateValueOffset], 0);
}

TEST(ArenaPublicClock, PrivateArenaKeepsTheScriptsValue) {
  auto state = State(kPublicMatchFlag, 1);
  const FlagStoreOutcome outcome = ApplyToFlagState(state.data(), true, Type("echo_arena_private"));
  EXPECT_FALSE(outcome.decision.forceFalse);
  EXPECT_EQ(outcome.decision.reason, "non_arena");
  EXPECT_EQ(state[kStateValueOffset], 1);
}

TEST(ArenaPublicClock, NonArenaKeepsTheScriptsValue) {
  auto state = State(kPublicMatchFlag, 1);
  const FlagStoreOutcome outcome = ApplyToFlagState(state.data(), true, Type("echo_combat"));
  EXPECT_FALSE(outcome.decision.forceFalse);
  EXPECT_EQ(outcome.decision.reason, "non_arena");
  EXPECT_EQ(state[kStateValueOffset], 1);
}

// #1493: a social session's flag store is left alone, as a combat session's is.
TEST(ArenaPublicClock, SocialSessionKeepsTheScriptsValue) {
  auto state = State(kPublicMatchFlag, 1);
  const FlagStoreOutcome outcome = ApplyToFlagState(state.data(), true, Type("social_2.0"));
  EXPECT_FALSE(outcome.decision.forceFalse);
  EXPECT_EQ(outcome.decision.reason, "non_arena");
  EXPECT_EQ(state[kStateValueOffset], 1);
}

TEST(ArenaPublicClock, NoStoredSessionTypeKeepsTheScriptsValue) {
  auto state = State(kPublicMatchFlag, 1);
  const FlagStoreOutcome outcome = ApplyToFlagState(state.data(), true, std::nullopt);
  EXPECT_FALSE(outcome.decision.forceFalse);
  EXPECT_EQ(outcome.decision.reason, "no_session_type");
  EXPECT_EQ(state[kStateValueOffset], 1);
}

TEST(ArenaPublicClock, AClientKeepsTheScriptsValueWhateverTheSessionType) {
  auto state = State(kPublicMatchFlag, 1);
  const FlagStoreOutcome outcome = ApplyToFlagState(state.data(), false, Type("echo_arena"));
  EXPECT_FALSE(outcome.decision.forceFalse);
  EXPECT_EQ(outcome.decision.reason, "not_server");
  EXPECT_EQ(state[kStateValueOffset], 1);
}

TEST(ArenaPublicClock, AFalseValueStaysFalseAndIsReportedAsSuch) {
  auto state = State(kPublicMatchFlag, 0);
  const FlagStoreOutcome outcome = ApplyToFlagState(state.data(), true, Type("echo_arena"));
  EXPECT_EQ(outcome.valueBefore, 0);
  EXPECT_EQ(outcome.valueAfter, 0);
}

TEST(ArenaPublicClock, AnotherFlagIsNeverTouched) {
  auto state = State(kPublicMatchFlag + 1, 1);
  const FlagStoreOutcome outcome = ApplyToFlagState(state.data(), true, Type("echo_arena"));
  EXPECT_FALSE(outcome.isPublicMatchFlag);
  EXPECT_FALSE(outcome.decision.forceFalse);
  EXPECT_EQ(state[kStateValueOffset], 1);
}

TEST(ArenaPublicClock, ANullStateIsLeftAlone) {
  const FlagStoreOutcome outcome = ApplyToFlagState(nullptr, true, Type("echo_arena"));
  EXPECT_FALSE(outcome.isPublicMatchFlag);
  EXPECT_FALSE(StateFlagId(nullptr).has_value());
}

TEST(ArenaPublicClock, EverySiteRowIsSelfConsistent) {
  for (const ScriptSite& site : kSites) {
    // The id the node loads is the public-match flag, little-endian after the 2-byte opcode.
    std::uint64_t id = 0;
    std::memcpy(&id, site.idInsn.data() + 2, sizeof(id));
    EXPECT_EQ(id, kPublicMatchFlag) << site.dll;
    // The node's call lands on the bind function: next-instruction address + rel32.
    std::int32_t rel = 0;
    std::memcpy(&rel, site.callInsn.data() + 1, sizeof(rel));
    EXPECT_EQ(static_cast<std::int64_t>(site.callRva) + 5 + rel, static_cast<std::int64_t>(site.bindRva)) << site.dll;
    EXPECT_EQ(site.callInsn[0], 0xE8) << site.dll;
  }
}

// An image the size of the largest site, with one site's bytes laid down.
std::vector<std::uint8_t> ImageWith(const ScriptSite& site) {
  std::vector<std::uint8_t> image(0x20000, 0xCC);
  std::memcpy(image.data() + site.bindRva, kBindPrologue.data(), kBindPrologue.size());
  std::memcpy(image.data() + site.idRva, site.idInsn.data(), site.idInsn.size());
  std::memcpy(image.data() + site.callRva, site.callInsn.data(), site.callInsn.size());
  return image;
}

TEST(ArenaPublicClock, AShippedImageIsUnhooked) {
  for (const ScriptSite& site : kSites) {
    const auto image = ImageWith(site);
    EXPECT_EQ(ClassifySite(image.data(), image.size(), site), SiteState::Unhooked) << site.dll;
  }
}

TEST(ArenaPublicClock, AJmpAtTheBindFunctionIsOurEarlierHook) {
  for (const ScriptSite& site : kSites) {
    auto image = ImageWith(site);
    image[site.bindRva] = 0xE9;
    EXPECT_EQ(ClassifySite(image.data(), image.size(), site), SiteState::Hooked) << site.dll;
  }
}

TEST(ArenaPublicClock, ADifferentBuildIsRefusedForEachByteTheHookRelies) {
  for (const ScriptSite& site : kSites) {
    auto badId = ImageWith(site);
    badId[site.idRva + 5] ^= 0x01;
    EXPECT_EQ(ClassifySite(badId.data(), badId.size(), site), SiteState::IdMismatch) << site.dll;

    auto badCall = ImageWith(site);
    badCall[site.callRva + 1] ^= 0x01;
    EXPECT_EQ(ClassifySite(badCall.data(), badCall.size(), site), SiteState::CallMismatch) << site.dll;

    auto badBind = ImageWith(site);
    badBind[site.bindRva + 3] ^= 0x01;
    EXPECT_EQ(ClassifySite(badBind.data(), badBind.size(), site), SiteState::BindMismatch) << site.dll;

    auto image = ImageWith(site);
    EXPECT_EQ(ClassifySite(image.data(), site.callRva, site), SiteState::ImageTooSmall) << site.dll;
    EXPECT_EQ(ClassifySite(nullptr, image.size(), site), SiteState::ImageTooSmall) << site.dll;
  }
}

TEST(ArenaPublicClock, EachSiteNamesAnotherScript) {
  EXPECT_STRNE(kSites[0].dll, kSites[1].dll);
  // A site's bytes do not validate against the other script's rows.
  const auto image = ImageWith(kSites[0]);
  EXPECT_NE(ClassifySite(image.data(), image.size(), kSites[1]), SiteState::Unhooked);
}

}  // namespace
