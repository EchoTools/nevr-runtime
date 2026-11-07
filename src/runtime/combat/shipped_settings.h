// shipped_settings.h: the PowerBoost settings he_is_the_cat's release ships, for a key config.yaml does not set (#600).
//
// His release writes PowerBoost.ini from his mp_weapon_settings.json (tools/echocombat_config.py, powerboost_ini() and
// medigun_ini() in heisthecat31/EchoCombat at v2.0.0). The port reads the same keys from config.yaml
// (`combat.settings.<section>.<key>`, cat_compat.h), and without this a key config.yaml leaves out fell back to the code's
// own literal, which in several places is not his shipped value (OVERCHARGE self-damage 2 against his 5; MEDIGUN beam 40 s
// against 15). ShippedSetting gives the value his release would have written, from the same JSON the runtime embeds
// (balance_values.h, kCatWeaponSettings), by his mapping:
//
//   [buff]    body_bonus <- ability_properties.buff.damage_bonus_body, head_bonus <- .damage_bonus_head,
//             head_part <- .head_body_part, self_damage <- .self_damage_per_tick,
//             self_damage_tick_ms <- round(.self_damage_tick_seconds * 1000), self_damage_lethal <- .self_damage_lethal (1/0)
//   [rez]     seconds <- .cheat_death_seconds, grace_ms <- .cheat_death_grace_ms
//   [cone]    axis <- "+z", aim <- .aim, link_window_ms <- .link_window_ms
//   [medigun] heal_per_second <- the Repair Matrix rate (kRepairMatrixHealPerSecond) * medigun.heal_scale; beam_seconds,
//             overheat_seconds, cool_seconds, cone_angle, range, cone_slack, hand, gun_axis, hand_offset, aim, beam_bone
//             <- medigun.<same key>; hand_offset_left <- medigun.hand_offset_left, else medigun.hand_offset
//
// A key his mapping writes from a missing JSON key gets his own default (the second argument of each `b.get` / `m.get`).
// Any other section or key has no shipped value (nullopt), and the caller keeps its literal. Header-only and pure.
#pragma once

#include <cmath>
#include <cstdio>
#include <optional>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

namespace nevr_combat {

// Repair Matrix's heal per second, heal_amt / heal_duration = 114 / 12, from the weapon settings his build starts from
// (medigun_ini() reads it from the combat gun patch, `ability_properties.heal`); his MEDIGUN heals that times heal_scale.
inline constexpr double kRepairMatrixHealPerSecond = 114.0 / 12.0;

namespace shipped_detail {

inline std::string Text(const nlohmann::json& v) {
  if (v.is_string()) return v.get<std::string>();
  if (v.is_boolean()) return v.get<bool>() ? "True" : "False";  // as Python's f-string writes a bool
  if (v.is_number_integer()) return std::to_string(v.get<long long>());
  if (v.is_number_float()) {
    const double d = v.get<double>();
    if (d == std::floor(d) && std::fabs(d) < 1e15) {  // Python writes 1.0 as "1.0"
      char b[32];
      std::snprintf(b, sizeof b, "%.1f", d);
      return b;
    }
    return v.dump();
  }
  return v.dump();
}

inline nlohmann::json At(const nlohmann::json& o, std::string_view key) {
  if (!o.is_object()) return nlohmann::json();
  const auto it = o.find(std::string(key));
  return it == o.end() ? nlohmann::json() : *it;
}

// `o.get(key, fallback)`, as his mapping writes it: the JSON value as text, or the fallback.
inline std::string Get(const nlohmann::json& o, std::string_view key, std::string_view fallback) {
  const nlohmann::json v = At(o, key);
  return v.is_null() ? std::string(fallback) : Text(v);
}

inline double Number(const nlohmann::json& o, std::string_view key, double fallback) {
  const nlohmann::json v = At(o, key);
  return v.is_number() ? v.get<double>() : fallback;
}

inline bool Truthy(const nlohmann::json& o, std::string_view key) {
  const nlohmann::json v = At(o, key);
  if (v.is_boolean()) return v.get<bool>();
  if (v.is_number()) return v.get<double>() != 0.0;
  return false;
}

}  // namespace shipped_detail

// The value his release writes for [section] key, from `settings` (his mp_weapon_settings.json, parsed); nullopt for a key
// his PowerBoost.ini does not carry.
inline std::optional<std::string> ShippedSetting(const nlohmann::json& settings, std::string_view section, std::string_view key) {
  using namespace shipped_detail;
  const nlohmann::json buff = At(At(settings, "ability_properties"), "buff");
  const nlohmann::json medi = At(settings, "medigun");
  if (section == "buff") {
    if (key == "body_bonus") return Get(buff, "damage_bonus_body", "1");
    if (key == "head_bonus") return Get(buff, "damage_bonus_head", "2");
    if (key == "head_part") return Get(buff, "head_body_part", "5");
    if (key == "self_damage") return Get(buff, "self_damage_per_tick", "5");
    if (key == "self_damage_tick_ms") {
      return std::to_string(static_cast<long long>(std::llround(Number(buff, "self_damage_tick_seconds", 1.0) * 1000.0)));
    }
    if (key == "self_damage_lethal") return Truthy(buff, "self_damage_lethal") ? "1" : "0";
    return std::nullopt;
  }
  if (section == "rez") {
    if (key == "seconds") return Get(buff, "cheat_death_seconds", "5");
    if (key == "grace_ms") return Get(buff, "cheat_death_grace_ms", "1500");
    return std::nullopt;
  }
  if (section == "cone") {
    if (key == "axis") return std::string("+z");
    if (key == "aim") return Get(buff, "aim", "head");
    if (key == "link_window_ms") return Get(buff, "link_window_ms", "500");
    return std::nullopt;
  }
  if (section == "medigun") {
    if (key == "heal_per_second") {
      char b[32];
      std::snprintf(b, sizeof b, "%.4f", kRepairMatrixHealPerSecond * Number(medi, "heal_scale", 0.85));
      return std::string(b);
    }
    if (key == "beam_seconds") return Get(medi, "beam_seconds", "40");
    if (key == "overheat_seconds") return Get(medi, "overheat_seconds", "3");
    if (key == "cool_seconds") return Get(medi, "cool_seconds", "5");
    if (key == "cone_angle") return Get(medi, "cone_angle", "20");
    if (key == "range") return Get(medi, "range", "20");
    if (key == "cone_slack") return Get(medi, "cone_slack", "5");
    if (key == "hand") return Get(medi, "hand", "auto");
    if (key == "gun_axis") return Get(medi, "gun_axis", "+z");
    if (key == "hand_offset") return Get(medi, "hand_offset", "0,0,0");
    if (key == "hand_offset_left") return Get(medi, "hand_offset_left", Get(medi, "hand_offset", "0,0,0"));
    if (key == "aim") return Get(medi, "aim", "head");
    if (key == "beam_bone") return Get(medi, "beam_bone", "ToolAttach1");
    return std::nullopt;
  }
  return std::nullopt;
}

}  // namespace nevr_combat
