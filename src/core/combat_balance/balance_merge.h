#pragma once

// Balance values as a transform of the stock mp_weapon_settings (a CJsonResourceWin10 on PC, a CJsonResourceAndroid on Quest:
// both are CJsonResource, whose Attach reads a 0x220-byte header into the resource, libr15.so 0x2adb8c8; the u64 at +8 is the
// body's length, then the minified JSON body). One implementation for both platforms: the PC edge is the resource transform in
// runtime/combat/combat.cpp, the Quest edge the read-finished callback in quest/sentinel/menu_cards_hook.cpp. Pure:
// test_combat_settings.cpp and menu_cards_hook_test.cpp drive it on synthetic resources.
// The merge is the one Cat's tools/apply_config.py makes: every key of the values whose name does not start with `_` is
// set; a nested object is merged into the object of the same name (made when absent); a value equal to the stock one is
// left and not counted.

#include <cstddef>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>
#include <utility>

#include <nlohmann/json.hpp>

namespace nevr_combat::balance {

inline constexpr std::uint64_t kJsonResourceType = 0x607d858c90268f9aULL;  // CSymbol64("CJsonResourceWin10")
inline constexpr std::uint64_t kJsonResourceTypeAndroid = 0x921e9761b45fc6e4ULL;  // CSymbol64("CJsonResourceAndroid"), core/config_alias.h
inline constexpr std::uint64_t kWeaponSettingsName = 0x11a8523ed4724ca7ULL;  // CSymbol64("mp_weapon_settings")
inline constexpr std::size_t kHeaderSize = 0x220;
inline constexpr std::size_t kLengthOffset = 8;

/// True for the class of a JSON resource on either platform.
inline bool IsJsonResourceType(std::uint64_t type) {
  return type == kJsonResourceType || type == kJsonResourceTypeAndroid;
}

enum class Result { kApplied, kUnchanged, kNotAResource, kBodyUnparsed, kValuesUnparsed };

inline const char* ResultName(Result r) {
  switch (r) {
    case Result::kApplied: return "applied";
    case Result::kUnchanged: return "unchanged";
    case Result::kNotAResource: return "not_a_json_resource";
    case Result::kBodyUnparsed: return "body_unparsed";
    case Result::kValuesUnparsed: return "values_unparsed";
  }
  return "unknown";
}

/// Merges `src` into `into` as apply_config.py does; returns how many values it set.
inline std::size_t Merge(nlohmann::ordered_json& into, const nlohmann::ordered_json& src) {
  std::size_t changed = 0;
  for (auto it = src.begin(); it != src.end(); ++it) {
    const std::string& key = it.key();
    if (!key.empty() && key[0] == '_') continue;
    if (it.value().is_object()) {
      nlohmann::ordered_json& child = into[key];
      if (!child.is_object()) child = nlohmann::ordered_json::object();
      changed += Merge(child, it.value());
    } else if (!into.contains(key) || into[key] != it.value()) {
      into[key] = it.value();
      ++changed;
    }
  }
  return changed;
}

/// The resource `blob` with `values` merged into its body, in `*out` (header kept, length updated). `*changed` is the
/// number of values set. Nothing is thrown: a blob that is not a JSON resource, or a body or values that do not parse,
/// is a Result and `*out` is untouched.
inline Result Apply(const std::uint8_t* blob, std::size_t size, const char* values, std::string* out, std::size_t* changed) {
  *changed = 0;
  if (blob == nullptr || size < kHeaderSize) return Result::kNotAResource;
  std::uint64_t declared = 0;
  std::memcpy(&declared, blob + kLengthOffset, sizeof declared);
  if (declared != size - kHeaderSize) return Result::kNotAResource;
  nlohmann::ordered_json body = nlohmann::ordered_json::parse(blob + kHeaderSize, blob + size, nullptr, false);
  if (body.is_discarded() || !body.is_object()) return Result::kBodyUnparsed;
  nlohmann::ordered_json src = nlohmann::ordered_json::parse(values, nullptr, false);
  if (src.is_discarded() || !src.is_object()) return Result::kValuesUnparsed;
  *changed = Merge(body, src);
  if (*changed == 0) return Result::kUnchanged;
  const std::string text = body.dump();
  std::string result(reinterpret_cast<const char*>(blob), kHeaderSize);
  const std::uint64_t length = text.size();
  std::memcpy(&result[kLengthOffset], &length, sizeof length);
  result += text;
  *out = std::move(result);
  return Result::kApplied;
}

/// The values the log line of a merge reads back from the merged resource: Cat's PROTOSTAR (magnum) body damage, head
/// multiplier and refire delay, and QUARK's (smg) vent rate. Each is -1 when the body does not hold it. This is what a
/// tester compares with the table in shipped_settings.h.
struct Probe {
  double magnumDamage = -1;
  double magnumHead = -1;
  double magnumRefire = -1;
  double smgVent = -1;
};

inline double NumberAt(const nlohmann::ordered_json& body, const char* section, const char* weapon, const char* key) {
  if (!body.is_object() || !body.contains(section)) return -1;
  const nlohmann::ordered_json& s = body[section];
  if (!s.is_object() || !s.contains(weapon)) return -1;
  const nlohmann::ordered_json& w = s[weapon];
  if (!w.is_object() || !w.contains(key) || !w[key].is_number()) return -1;
  return w[key].get<double>();
}

/// Reads the probe values from a merged blob (header and body); all -1 when it is not one.
inline Probe ReadProbe(const std::uint8_t* blob, std::size_t size) {
  Probe p;
  if (blob == nullptr || size <= kHeaderSize) return p;
  const nlohmann::ordered_json body = nlohmann::ordered_json::parse(blob + kHeaderSize, blob + size, nullptr, false);
  if (body.is_discarded()) return p;
  p.magnumDamage = NumberAt(body, "weapon_properties", "magnum", "projectile_damage");
  p.magnumHead = NumberAt(body, "damage_table", "magnum", "head");
  p.magnumRefire = NumberAt(body, "weapon_properties", "magnum", "refire_delay");
  p.smgVent = NumberAt(body, "weapon_properties", "smg", "heat_venting_rate");
  return p;
}

/// One probe value as text ("45", "1.5556"; "-1" when the body does not hold it): the Quest record carries it as a string field.
inline std::string ValueText(double v) {
  char buf[32];
  std::snprintf(buf, sizeof buf, "%g", v);
  return buf;
}

/// The probe as the key=value tail both platforms' proof lines carry.
inline std::string ProbeText(const Probe& p) {
  char buf[160];
  std::snprintf(buf, sizeof buf, "magnum_damage=%g magnum_head=%g magnum_refire=%g smg_vent=%g", p.magnumDamage, p.magnumHead,
                p.magnumRefire, p.smgVent);
  return buf;
}

}  // namespace nevr_combat::balance
