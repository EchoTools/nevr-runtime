#pragma once

// Which of EchoCombat's feature groups run, from config.yaml's `combat` section, and the defaults in code. Pure (no
// Windows, no game): test_combat_settings.cpp drives it.
//
//   group                 key                    default
//   stock-game fixes      (none)                 always on: the drone follow crash guard (here); the equipment re-fire
//                                                (#2121, patch/equipment_refire.h) and the STARBURST log collapse
//                                                (log/collapse_format_rules.h) are the runtime's own and run anyway
//   DRONE                 combat.drone           on
//   OVERCHARGE            combat.overcharge      on
//   MEDIGUN               combat.medigun         off (and a client follows its server's answer)
//   UMBRAL VEIL + net     combat.veil            on
//   match control         combat.match_control   on (needs combat.veil: it rides the combat net)
//   balance values        combat.balance         on
//   clean exit            combat.clean_exit      on, as his loader always applies it (the TerminateProcess stub is never written while off)
//   unfinished / unsafe   (none)                 never: party guns, payload speed, a client's own damage bonus, CombatBots

#include <array>
#include <cstddef>
#include <cstring>

namespace nevr_combat {

enum class Group : int { kDrone, kOvercharge, kMedigun, kVeil, kMatchControl, kBalance, kCleanExit };
constexpr std::size_t kGroupCount = 7;

struct GroupSpec {
  Group group;
  const char* key;    // under `combat.`
  const char* name;   // as players know it
  bool fallback;      // the default in code
};

inline constexpr std::array<GroupSpec, kGroupCount> kGroups = {{
    {Group::kDrone, "drone", "DRONE", true},
    {Group::kOvercharge, "overcharge", "OVERCHARGE", true},
    {Group::kMedigun, "medigun", "MEDIGUN", false},
    {Group::kVeil, "veil", "UMBRAL VEIL and the combat net", true},
    {Group::kMatchControl, "match_control", "match control terminals", true},
    {Group::kBalance, "balance", "balance values", true},
    {Group::kCleanExit, "clean_exit", "clean exit", true},
}};

/// Never on, whatever the config says, and why (logged once each at install).
struct Disabled {
  const char* name;
  const char* reason;
};
inline constexpr std::array<Disabled, 4> kNeverOn = {{
    {"party guns", "unfinished: Cat hides them"},
    {"payload speed", "unfinished: Cat parked it"},
    {"client damage bonus", "unsafe: a client would raise its own damage"},
    {"CombatBots", "not in Cat's release (bot brain, ML port)"},
}};

/// Where a group's value came from.
enum class Source { kDefault, kConfig, kUnrecognised };

inline const char* SourceName(Source s) {
  switch (s) {
    case Source::kDefault: return "default";
    case Source::kConfig: return "config";
    case Source::kUnrecognised: return "unrecognised";
  }
  return "unknown";
}

struct Setting {
  bool on;
  Source source;
};

struct Settings {
  std::array<Setting, kGroupCount> groups;
  bool On(Group g) const { return groups[static_cast<std::size_t>(g)].on; }
};

/// true/yes/on/1 and false/no/off/0, any case; anything else is unrecognised.
inline bool ParseSwitch(const char* text, bool* out) {
  if (text == nullptr) return false;
  char t[8] = {};
  std::size_t n = 0;
  for (; text[n] != '\0'; ++n) {
    if (n + 1 >= sizeof t) return false;
    const char c = text[n];
    t[n] = (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
  }
  if (!std::strcmp(t, "true") || !std::strcmp(t, "yes") || !std::strcmp(t, "on") || !std::strcmp(t, "1")) {
    *out = true;
    return true;
  }
  if (!std::strcmp(t, "false") || !std::strcmp(t, "no") || !std::strcmp(t, "off") || !std::strcmp(t, "0")) {
    *out = false;
    return true;
  }
  return false;
}

/// The settings, given `get(key)`: the scalar at `combat.<key>`, or null when absent. An unrecognised value keeps the
/// default and is reported as such, so the line at install says the file was not obeyed.
template <typename Get>
Settings ReadSettings(Get&& get) {
  Settings s{};
  for (std::size_t i = 0; i < kGroupCount; ++i) {
    const GroupSpec& spec = kGroups[i];
    const char* text = get(spec.key);
    bool value = spec.fallback;
    Source source = Source::kDefault;
    if (text != nullptr) {
      if (ParseSwitch(text, &value)) {
        source = Source::kConfig;
      } else {
        value = spec.fallback;
        source = Source::kUnrecognised;
      }
    }
    s.groups[i] = Setting{value, source};
  }
  return s;
}

}  // namespace nevr_combat
