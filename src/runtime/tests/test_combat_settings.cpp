// The combat module's group switches and their defaults (runtime/combat/combat_settings.h), and the balance transform's
// merge (runtime/combat/balance_merge.h) over a synthetic CJsonResourceWin10, whose output is parsed back.

#include <cstdint>
#include <cstring>
#include <map>
#include <string>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "runtime/combat/balance_merge.h"
#include "runtime/combat/balance_values.h"
#include "runtime/combat/combat_settings.h"

namespace {

using nevr_combat::Group;
using nevr_combat::Source;

nevr_combat::Settings Read(const std::map<std::string, std::string>& file) {
  return nevr_combat::ReadSettings([&](const char* key) -> const char* {
    auto it = file.find(key);
    return it == file.end() ? nullptr : it->second.c_str();
  });
}

TEST(CombatSettings, TheDefaultsAreTheNineGroupTable) {
  const nevr_combat::Settings s = Read({});
  EXPECT_TRUE(s.On(Group::kDrone));
  EXPECT_TRUE(s.On(Group::kOvercharge));
  EXPECT_FALSE(s.On(Group::kMedigun));
  EXPECT_TRUE(s.On(Group::kVeil));
  EXPECT_TRUE(s.On(Group::kMatchControl));
  EXPECT_TRUE(s.On(Group::kBalance));
  EXPECT_FALSE(s.On(Group::kCleanExit));
  for (const auto& g : s.groups) EXPECT_EQ(g.source, Source::kDefault);
}

TEST(CombatSettings, ConfigTurnsAGroupOnOrOff) {
  const nevr_combat::Settings s = Read({{"medigun", "true"}, {"drone", "off"}, {"clean_exit", "YES"}});
  EXPECT_TRUE(s.On(Group::kMedigun));
  EXPECT_FALSE(s.On(Group::kDrone));
  EXPECT_TRUE(s.On(Group::kCleanExit));
  EXPECT_EQ(s.groups[static_cast<std::size_t>(Group::kMedigun)].source, Source::kConfig);
}

TEST(CombatSettings, AnUnrecognisedValueKeepsTheDefaultAndSaysSo) {
  const nevr_combat::Settings s = Read({{"clean_exit", "maybe"}, {"veil", ""}});
  EXPECT_FALSE(s.On(Group::kCleanExit));
  EXPECT_TRUE(s.On(Group::kVeil));
  EXPECT_EQ(s.groups[static_cast<std::size_t>(Group::kCleanExit)].source, Source::kUnrecognised);
  EXPECT_EQ(s.groups[static_cast<std::size_t>(Group::kVeil)].source, Source::kUnrecognised);
}

TEST(CombatSettings, TheUnfinishedAndUnsafeAreNeverOn) {
  ASSERT_EQ(nevr_combat::kNeverOn.size(), 4u);
  EXPECT_STREQ(nevr_combat::kNeverOn[0].name, "party guns");
  EXPECT_STREQ(nevr_combat::kNeverOn[1].name, "payload speed");
  EXPECT_STREQ(nevr_combat::kNeverOn[2].name, "client damage bonus");
  EXPECT_STREQ(nevr_combat::kNeverOn[3].name, "CombatBots");
}

std::string Resource(const std::string& body) {
  std::string blob(nevr_combat::balance::kHeaderSize, '\0');
  const char path[] = "rad15\\json\\balance\\mp_weapon_settings.json";
  std::memcpy(&blob[0x20], path, sizeof path - 1);
  const std::uint64_t n = body.size();
  std::memcpy(&blob[nevr_combat::balance::kLengthOffset], &n, sizeof n);
  return blob + body;
}

TEST(CombatBalance, MergesValuesIntoTheStockResourceAndKeepsItsHeader) {
  const std::string in = Resource(R"({"damage_table":{"magnum":{"head":1.5,"torso":1}},"weapon_properties":{}})");
  const char values[] = R"({"_about":"x","damage_table":{"magnum":{"_why":"y","head":1.5556}},"weapon_properties":{"smg":{"a":2}}})";
  std::string out;
  std::size_t changed = 0;
  ASSERT_EQ(nevr_combat::balance::Apply(reinterpret_cast<const std::uint8_t*>(in.data()), in.size(), values, &out, &changed),
            nevr_combat::balance::Result::kApplied);
  EXPECT_EQ(changed, 2u);
  ASSERT_GT(out.size(), nevr_combat::balance::kHeaderSize);
  EXPECT_EQ(0, std::memcmp(out.data() + 0x20, in.data() + 0x20, 0x200)) << "the source path is kept";
  std::uint64_t declared = 0;
  std::memcpy(&declared, out.data() + nevr_combat::balance::kLengthOffset, sizeof declared);
  EXPECT_EQ(declared, out.size() - nevr_combat::balance::kHeaderSize);
  const auto body = nlohmann::json::parse(out.substr(nevr_combat::balance::kHeaderSize));
  EXPECT_DOUBLE_EQ(body["damage_table"]["magnum"]["head"].get<double>(), 1.5556);
  EXPECT_EQ(body["damage_table"]["magnum"]["torso"].get<int>(), 1) << "a stock value it does not name stays";
  EXPECT_EQ(body["weapon_properties"]["smg"]["a"].get<int>(), 2);
  EXPECT_FALSE(body.contains("_about"));
  EXPECT_FALSE(body["damage_table"]["magnum"].contains("_why"));
}

TEST(CombatBalance, AResourceThatHoldsTheValuesAlreadyIsUnchanged) {
  const std::string in = Resource(R"({"a":{"b":1}})");
  std::string out = "untouched";
  std::size_t changed = 0;
  EXPECT_EQ(nevr_combat::balance::Apply(reinterpret_cast<const std::uint8_t*>(in.data()), in.size(), R"({"a":{"b":1}})", &out,
                                        &changed),
            nevr_combat::balance::Result::kUnchanged);
  EXPECT_EQ(out, "untouched");
}

TEST(CombatBalance, ABlobThatIsNotAJsonResourceIsLeft) {
  std::string in = Resource("{}");
  in.push_back('x');  // the declared length no longer matches
  std::string out;
  std::size_t changed = 0;
  EXPECT_EQ(nevr_combat::balance::Apply(reinterpret_cast<const std::uint8_t*>(in.data()), in.size(), "{}", &out, &changed),
            nevr_combat::balance::Result::kNotAResource);
  const std::string bad = Resource("{not json");
  EXPECT_EQ(nevr_combat::balance::Apply(reinterpret_cast<const std::uint8_t*>(bad.data()), bad.size(), "{}", &out, &changed),
            nevr_combat::balance::Result::kBodyUnparsed);
}

TEST(CombatBalance, CatsValuesParseAndSetSomething) {
  const std::string in = Resource("{}");
  std::string out;
  std::size_t changed = 0;
  EXPECT_EQ(nevr_combat::balance::Apply(reinterpret_cast<const std::uint8_t*>(in.data()), in.size(),
                                        nevr_combat::balance::kCatWeaponSettings, &out, &changed),
            nevr_combat::balance::Result::kApplied);
  EXPECT_GT(changed, 0u);
  const auto parsed = nlohmann::json::parse(out.substr(nevr_combat::balance::kHeaderSize), nullptr, false);
  EXPECT_FALSE(parsed.is_discarded());
}

}  // namespace
