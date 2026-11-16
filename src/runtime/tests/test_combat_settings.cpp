// The combat module's group switches and their defaults (runtime/combat/combat_settings.h), and the balance transform's
// merge (core/combat_balance/balance_merge.h) over a synthetic CJsonResourceWin10, whose output is parsed back.

#include <cstdint>
#include <cstring>
#include <map>
#include <set>
#include <string>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "core/combat_balance/balance_merge.h"
#include "core/combat_balance/balance_values_gunpatch.h"
#include "core/combat_balance/balance_values.h"
#include "core/combat_balance/balance_values_absent.h"
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
  EXPECT_TRUE(s.On(Group::kCleanExit));
  for (const auto& g : s.groups) EXPECT_EQ(g.source, Source::kDefault);
}

TEST(CombatSettings, ConfigTurnsAGroupOnOrOff) {
  const nevr_combat::Settings s = Read({{"medigun", "true"}, {"drone", "off"}, {"clean_exit", "NO"}});
  EXPECT_TRUE(s.On(Group::kMedigun));
  EXPECT_FALSE(s.On(Group::kDrone));
  EXPECT_FALSE(s.On(Group::kCleanExit));
  EXPECT_EQ(s.groups[static_cast<std::size_t>(Group::kMedigun)].source, Source::kConfig);
}

TEST(CombatSettings, AnUnrecognisedValueKeepsTheDefaultAndSaysSo) {
  const nevr_combat::Settings s = Read({{"clean_exit", "maybe"}, {"veil", ""}});
  EXPECT_TRUE(s.On(Group::kCleanExit));
  EXPECT_TRUE(s.On(Group::kVeil));
  EXPECT_EQ(s.groups[static_cast<std::size_t>(Group::kCleanExit)].source, Source::kUnrecognised);
  EXPECT_EQ(s.groups[static_cast<std::size_t>(Group::kVeil)].source, Source::kUnrecognised);
}

TEST(CombatSettings, TheUnfinishedAndUnsafeAreNeverOn) {
  ASSERT_EQ(nevr_combat::kNeverOn.size(), 3u);
  EXPECT_STREQ(nevr_combat::kNeverOn[0].name, "party guns");
  EXPECT_STREQ(nevr_combat::kNeverOn[1].name, "client damage bonus");
  EXPECT_STREQ(nevr_combat::kNeverOn[2].name, "CombatBots");
  for (const nevr_combat::Disabled& d : nevr_combat::kNeverOn) EXPECT_STRNE(d.name, "payload speed");
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

TEST(CombatBalance, AStockDocumentWithoutTheDroneDamageRowGetsIt) {
  const std::string in = Resource(R"({"damage_table":{"magnum":{"head":1.5}}})");
  std::string out;
  std::size_t changed = 0;
  ASSERT_EQ(nevr_combat::balance::Apply(reinterpret_cast<const std::uint8_t*>(in.data()), in.size(), "{}", &out, &changed,
                                        nevr_combat::balance::kCatAbsentValues),
            nevr_combat::balance::Result::kApplied);
  EXPECT_EQ(changed, 7u);
  const auto body = nlohmann::json::parse(out.substr(nevr_combat::balance::kHeaderSize));
  for (const char* k : {"head", "arm", "leg", "torso", "ordnance", "barrier", "ssi"})
    EXPECT_EQ(body["damage_table"]["drone"][k].get<int>(), 1) << k;
  EXPECT_DOUBLE_EQ(body["damage_table"]["magnum"]["head"].get<double>(), 1.5);
}

TEST(CombatBalance, ADocumentWithAnotherDroneDamageRowKeepsIt) {
  const std::string in = Resource(R"({"damage_table":{"drone":{"head":2,"torso":0.5}}})");
  std::string out = "untouched";
  std::size_t changed = 0;
  const auto r = nevr_combat::balance::Apply(reinterpret_cast<const std::uint8_t*>(in.data()), in.size(), "{}", &out, &changed,
                                             nevr_combat::balance::kCatAbsentValues);
  ASSERT_EQ(r, nevr_combat::balance::Result::kApplied);  // the keys the row lacks are added, the ones it has are not touched
  const auto body = nlohmann::json::parse(out.substr(nevr_combat::balance::kHeaderSize));
  EXPECT_EQ(body["damage_table"]["drone"]["head"].get<int>(), 2);
  EXPECT_DOUBLE_EQ(body["damage_table"]["drone"]["torso"].get<double>(), 0.5);
  const std::string whole = Resource(R"({"damage_table":{"drone":{"head":2,"arm":2,"leg":2,"torso":2,"ordnance":2,"barrier":2,"ssi":2}}})");
  std::string out2 = "untouched";
  EXPECT_EQ(nevr_combat::balance::Apply(reinterpret_cast<const std::uint8_t*>(whole.data()), whole.size(), "{}", &out2, &changed,
                                        nevr_combat::balance::kCatAbsentValues),
            nevr_combat::balance::Result::kUnchanged);
  EXPECT_EQ(out2, "untouched");
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

// The proof line of both platforms reads these values back from the merged bytes: Cat's PROTOSTAR and QUARK numbers.
TEST(CombatBalance, TheProbeReadsCatsValuesBackFromTheMergedResource) {
  const std::string in = Resource(R"({"weapon_properties":{"magnum":{"projectile_damage":50}}})");
  std::string out;
  std::size_t changed = 0;
  ASSERT_EQ(nevr_combat::balance::Apply(reinterpret_cast<const std::uint8_t*>(in.data()), in.size(),
                                        nevr_combat::balance::kCatWeaponSettings, &out, &changed),
            nevr_combat::balance::Result::kApplied);
  const nevr_combat::balance::Probe p =
      nevr_combat::balance::ReadProbe(reinterpret_cast<const std::uint8_t*>(out.data()), out.size());
  EXPECT_EQ(p.magnumDamage, 45);
  EXPECT_EQ(p.magnumHead, 1.5556);
  EXPECT_EQ(p.magnumRefire, 0.6);
  EXPECT_EQ(p.smgVent, 71.43);
  EXPECT_EQ(nevr_combat::balance::ProbeText(p), "magnum_damage=45 magnum_head=1.5556 magnum_refire=0.6 smg_vent=71.43");
  const nevr_combat::balance::Probe stock =
      nevr_combat::balance::ReadProbe(reinterpret_cast<const std::uint8_t*>(in.data()), in.size());
  EXPECT_EQ(stock.magnumDamage, 50) << "the stock value reads back unmerged";
  EXPECT_EQ(stock.smgVent, -1);
}

TEST(CombatBalance, BothPlatformsJsonResourceClassesAreRecognised) {
  EXPECT_TRUE(nevr_combat::balance::IsJsonResourceType(nevr_combat::balance::kJsonResourceType));
  EXPECT_TRUE(nevr_combat::balance::IsJsonResourceType(nevr_combat::balance::kJsonResourceTypeAndroid));
  EXPECT_FALSE(nevr_combat::balance::IsJsonResourceType(0x1234));
}

// ---- the gun patch's differences (balance_values_gunpatch.h) -----------------------------------------------------

// A stock mp_weapon_settings as the shipped PC data has it: damage_table has no magnum, magnum_aoe, smg, chain or rifle row,
// grenade_properties has no loc, and both arm-grab switches are true (S-BASE 2.2). A synthetic body, not game bytes.
const char kStockBody[] =
    R"({"damage_table":{"assault":{"head":1.5,"arm":1}},"grenade_properties":{"drone":{"cooldown":1.0}},)"
    R"("player_properties":{"disable_arm_grab_team":true,"disable_arm_grab_enemy":true,"other":7},"weapon_properties":{}})";

nlohmann::json ShippedOver(const std::string& body) {
  const std::string in = Resource(body);
  std::string out;
  std::size_t changed = 0;
  EXPECT_EQ(nevr_combat::balance::ApplyShippedValues(reinterpret_cast<const std::uint8_t*>(in.data()), in.size(), &out, &changed),
            nevr_combat::balance::Result::kApplied);
  return nlohmann::json::parse(out.substr(nevr_combat::balance::kHeaderSize), nullptr, false);
}

// G-GUNPATCH-1..5: each damage row the gun patch has and the stock record lacks.
TEST(CombatGunPatch, StockWithoutTheDamageRowsGetsThemAsTheGunPatchHasThem) {
  const nlohmann::json body = ShippedOver(kStockBody);
  const auto& dt = body["damage_table"];
  EXPECT_EQ(dt["assault"]["head"].get<double>(), 1.5) << "a stock row stays";
  const struct {
    const char* row;
    double head, arm, leg, torso, ordnance, barrier, ssi;
  } want[] = {
      {"magnum", 1.5556, 1, 1, 1, 1, 1.75, 1},  // head is balance_values.h's (his config over the gun patch's 1.5)
      {"magnum_aoe", 1, 1, 1, 1, 1, 1, 1}, {"smg", 1.4, 1, 0.5, 1, 1, 0.5, 1}, {"chain", 1, 1, 1, 1, 1, 1, 1},
      {"rifle", 1.65, 1, 0.75, 1, 1, 0.5, 1},
  };
  for (const auto& w : want) {
    ASSERT_TRUE(dt.contains(w.row)) << w.row;
    const auto& r = dt[w.row];
    EXPECT_EQ(r["head"].get<double>(), w.head) << w.row;
    EXPECT_EQ(r["arm"].get<double>(), w.arm) << w.row;
    EXPECT_EQ(r["leg"].get<double>(), w.leg) << w.row;
    EXPECT_EQ(r["torso"].get<double>(), w.torso) << w.row;
    EXPECT_EQ(r["ordnance"].get<double>(), w.ordnance) << w.row;
    EXPECT_EQ(r["barrier"].get<double>(), w.barrier) << w.row;
    EXPECT_EQ(r["ssi"].get<double>(), w.ssi) << w.row;
    EXPECT_EQ(r.size(), 7u) << w.row << " has all seven multipliers (the defect: only head)";
  }
}

// G-GUNPATCH-6: the translocator's cooldown, beside the keys balance_values.h gives loc.
TEST(CombatGunPatch, TheTranslocatorGetsItsCooldownAndKeepsHisOtherKeys) {
  const nlohmann::json loc = ShippedOver(kStockBody)["grenade_properties"]["loc"];
  EXPECT_EQ(loc["cooldown"].get<int>(), 35);
  EXPECT_EQ(loc["effect_size"].get<double>(), 2.0);
  EXPECT_EQ(loc["sound_copies"].get<int>(), 12);
}

// G-GUNPATCH-7 and 8: arm grab is allowed, against the stock true.
TEST(CombatGunPatch, ArmGrabIsAllowedForTeamAndEnemy) {
  const nlohmann::json pp = ShippedOver(kStockBody)["player_properties"];
  EXPECT_FALSE(pp["disable_arm_grab_team"].get<bool>());
  EXPECT_FALSE(pp["disable_arm_grab_enemy"].get<bool>());
  EXPECT_EQ(pp["other"].get<int>(), 7) << "a key neither names stays";
}

// ADR 0039: no field has two writers. Every leaf the gun patch's values set is absent from balance_values.h.
void Leaves(const nlohmann::json& j, const std::string& path, std::set<std::string>* out) {
  for (auto it = j.begin(); it != j.end(); ++it) {
    if (it.key().empty() || it.key()[0] == '_') continue;
    if (it.value().is_object()) {
      Leaves(it.value(), path + "." + it.key(), out);
    } else {
      out->insert(path + "." + it.key());
    }
  }
}

TEST(CombatGunPatch, NoFieldIsSetByBothValueFiles) {
  std::set<std::string> gun, cat;
  Leaves(nlohmann::json::parse(nevr_combat::balance::kGunPatchWeaponSettings), "", &gun);
  Leaves(nlohmann::json::parse(nevr_combat::balance::kCatWeaponSettings), "", &cat);
  EXPECT_EQ(gun.size(), 4u * 7 + 6 + 1 + 2);  // four full rows, magnum without head, loc cooldown, two switches
  for (const std::string& leaf : gun) EXPECT_EQ(cat.count(leaf), 0u) << leaf << " has two writers";
}

TEST(CombatGunPatch, TheGunPatchLayerAloneLeavesTheMagnumHeadToBalanceValues) {
  const std::string in = Resource(kStockBody);
  std::string out;
  std::size_t changed = 0;
  ASSERT_EQ(nevr_combat::balance::Apply(reinterpret_cast<const std::uint8_t*>(in.data()), in.size(),
                                        nevr_combat::balance::kGunPatchWeaponSettings, &out, &changed),
            nevr_combat::balance::Result::kApplied);
  const auto body = nlohmann::json::parse(out.substr(nevr_combat::balance::kHeaderSize));
  EXPECT_FALSE(body["damage_table"]["magnum"].contains("head"));
}

// All three layers through the one entry point both edges call: the gun patch's row, his config's magnum head over it, and
// R9's drone row added because the layered stock body still lacks it.
TEST(CombatGunPatch, TheThreeLayersTogetherGiveTheMagnumAndDroneRows) {
  const nlohmann::json dt = ShippedOver(kStockBody)["damage_table"];
  const struct {
    const char* row;
    double head, arm, leg, torso, ordnance, barrier, ssi;
  } want[] = {
      {"magnum", 1.5556, 1, 1, 1, 1, 1.75, 1},  // head from balance_values.h, the other six from the gun patch
      {"drone", 1, 1, 1, 1, 1, 1, 1},           // balance_values_absent.h, added only when absent
  };
  for (const auto& w : want) {
    ASSERT_TRUE(dt.contains(w.row)) << w.row;
    const auto& r = dt[w.row];
    EXPECT_EQ(r["head"].get<double>(), w.head) << w.row;
    EXPECT_EQ(r["arm"].get<double>(), w.arm) << w.row;
    EXPECT_EQ(r["leg"].get<double>(), w.leg) << w.row;
    EXPECT_EQ(r["torso"].get<double>(), w.torso) << w.row;
    EXPECT_EQ(r["ordnance"].get<double>(), w.ordnance) << w.row;
    EXPECT_EQ(r["barrier"].get<double>(), w.barrier) << w.row;
    EXPECT_EQ(r["ssi"].get<double>(), w.ssi) << w.row;
    EXPECT_EQ(r.size(), 7u) << w.row;
  }
}

// The order ApplyLayers applies: each layer in turn (a later one wins), then the absent values, which fill only what the
// layered body still lacks. Applied first, the absent values would set x as well and count four.
TEST(CombatGunPatch, LayersApplyInTurnAndTheAbsentValuesLast) {
  const std::string in = Resource(R"({"a":{"z":0}})");
  std::string out;
  std::size_t changed = 0;
  ASSERT_EQ(nevr_combat::balance::ApplyLayers(reinterpret_cast<const std::uint8_t*>(in.data()), in.size(),
                                              {R"({"a":{"x":1}})", R"({"a":{"x":2}})"}, &out, &changed,
                                              R"({"a":{"x":9,"y":3,"z":9}})"),
            nevr_combat::balance::Result::kApplied);
  const auto a = nlohmann::json::parse(out.substr(nevr_combat::balance::kHeaderSize))["a"];
  EXPECT_EQ(a["x"].get<int>(), 2) << "the later layer wins and the absent values do not overwrite it";
  EXPECT_EQ(a["y"].get<int>(), 3) << "an absent key is added";
  EXPECT_EQ(a["z"].get<int>(), 0) << "a stock key is kept";
  EXPECT_EQ(changed, 3u);  // x set twice, y added
}

// A resource that already holds every value is left, and nothing is counted.
TEST(CombatGunPatch, AResourceThatHoldsTheValuesAlreadyIsUnchanged) {
  const nlohmann::json once = ShippedOver(kStockBody);
  const std::string in = Resource(once.dump());
  std::string out = "untouched";
  std::size_t changed = 0;
  EXPECT_EQ(nevr_combat::balance::ApplyShippedValues(reinterpret_cast<const std::uint8_t*>(in.data()), in.size(), &out, &changed),
            nevr_combat::balance::Result::kUnchanged);
  EXPECT_EQ(out, "untouched");
}

}  // namespace
