// #600: a PowerBoost key config.yaml leaves out takes the value his release writes into PowerBoost.ini
// (runtime/combat/shipped_settings.h), from the settings the runtime embeds (balance_values.h, his mp_weapon_settings.json).
#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include <string>

#include "runtime/combat/balance_values.h"
#include "runtime/combat/shipped_settings.h"

namespace {

using nevr_combat::ShippedSetting;

nlohmann::json His() { return nlohmann::json::parse(nevr_combat::balance::kCatWeaponSettings); }

std::string At(const nlohmann::json& j, const char* section, const char* key) {
  const auto v = ShippedSetting(j, section, key);
  return v ? *v : std::string("<none>");
}

TEST(CombatShippedSettings, OverchargeTakesHisShippedValues) {
  const nlohmann::json j = His();
  EXPECT_EQ(At(j, "buff", "self_damage"), "5");  // the port's literal is 2
  EXPECT_EQ(At(j, "buff", "self_damage_tick_ms"), "1000");
  EXPECT_EQ(At(j, "buff", "self_damage_lethal"), "0");
  EXPECT_EQ(At(j, "buff", "body_bonus"), "1");
  EXPECT_EQ(At(j, "buff", "head_bonus"), "2");
  EXPECT_EQ(At(j, "buff", "head_part"), "5");
  EXPECT_EQ(At(j, "rez", "seconds"), "5");
  EXPECT_EQ(At(j, "rez", "grace_ms"), "1500");
  EXPECT_EQ(At(j, "cone", "axis"), "+z");
  EXPECT_EQ(At(j, "cone", "aim"), "head");
  EXPECT_EQ(At(j, "cone", "link_window_ms"), "500");
}

TEST(CombatShippedSettings, MedigunTakesHisShippedValues) {
  const nlohmann::json j = His();
  EXPECT_EQ(At(j, "medigun", "beam_seconds"), "15");     // the port's literal is 40
  EXPECT_EQ(At(j, "medigun", "overheat_seconds"), "4");  // 3
  EXPECT_EQ(At(j, "medigun", "aim"), "gun");             // "head"
  EXPECT_EQ(At(j, "medigun", "hand_offset"), "0.064,0.000,0.203");
  EXPECT_EQ(At(j, "medigun", "hand_offset_left"), "0.124,0,0.203");
  EXPECT_EQ(At(j, "medigun", "heal_per_second"), "8.0750");  // 114 / 12 * 0.85
  EXPECT_EQ(At(j, "medigun", "cool_seconds"), "5");
  EXPECT_EQ(At(j, "medigun", "cone_angle"), "20");
  EXPECT_EQ(At(j, "medigun", "range"), "20");
  EXPECT_EQ(At(j, "medigun", "cone_slack"), "5");
  EXPECT_EQ(At(j, "medigun", "hand"), "auto");
  EXPECT_EQ(At(j, "medigun", "gun_axis"), "+z");
  EXPECT_EQ(At(j, "medigun", "beam_bone"), "ToolAttach1");
}

TEST(CombatShippedSettings, AKeyHisJsonLeavesOutTakesHisMappingsDefault) {
  const nlohmann::json empty = nlohmann::json::object();
  EXPECT_EQ(At(empty, "buff", "self_damage"), "5");
  EXPECT_EQ(At(empty, "buff", "self_damage_tick_ms"), "1000");
  EXPECT_EQ(At(empty, "medigun", "beam_seconds"), "40");
  EXPECT_EQ(At(empty, "medigun", "aim"), "head");
  EXPECT_EQ(At(empty, "medigun", "hand_offset_left"), "0,0,0");
  EXPECT_EQ(At(empty, "medigun", "heal_per_second"), "8.0750");
  const nlohmann::json off = {{"medigun", {{"hand_offset", "1,2,3"}}},
                              {"ability_properties", {{"buff", {{"self_damage_tick_seconds", 0.25}, {"self_damage_lethal", true}}}}}};
  EXPECT_EQ(At(off, "medigun", "hand_offset_left"), "1,2,3");  // falls back to hand_offset
  EXPECT_EQ(At(off, "buff", "self_damage_tick_ms"), "250");
  EXPECT_EQ(At(off, "buff", "self_damage_lethal"), "1");
}

TEST(CombatShippedSettings, AKeyHisPowerBoostIniDoesNotCarryHasNone) {
  const nlohmann::json j = His();
  EXPECT_FALSE(ShippedSetting(j, "party", "rot"));
  EXPECT_FALSE(ShippedSetting(j, "buff", "damage_bonus"));  // a client's own damage bonus is never read
  EXPECT_FALSE(ShippedSetting(j, "server", "medigun"));     // combat.medigun decides it
  EXPECT_FALSE(ShippedSetting(j, "nosuch", "key"));
  // A key of a known section that his ini does not write (a JSON key, or none) has none either.
  EXPECT_FALSE(ShippedSetting(j, "medigun", "heal_scale"));
  EXPECT_FALSE(ShippedSetting(j, "medigun", "enabled"));
  EXPECT_FALSE(ShippedSetting(j, "buff", "dr"));
  EXPECT_FALSE(ShippedSetting(j, "rez", "nosuch"));
  EXPECT_FALSE(ShippedSetting(j, "cone", "nosuch"));
}

}  // namespace
