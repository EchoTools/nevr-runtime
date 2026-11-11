/* The Quest headset identity and the settings each headset gets (ADR 0031): one function that names the headset from its
 * system properties, and one table of the settings each headset's row holds.
 *
 * Pure: no Android, no game header, no hook. The identity step (integration/headset_identity_step.cpp) and the combat modules
 * (combat/heaps.cpp, combat/refresh_rate.cpp) include it. The combat modules read ro.product.device themselves and call
 * IdentifyDevice, which is Identify with that one input, until the identity step's profile is handed to them (#2037). Identify
 * and IdentifyDevice agree on every rule (headset_profile_test.cpp). Each caller reads its own properties; this header never does.
 *
 * The rules are checked in order and the first match wins. A rule names its inputs and their exact values. A rule never
 * infers a headset from another quantity (an eye width, a refresh rate, a memory size; ADR 0031, R3). The last rule has an
 * empty device, so it matches every headset: a headset no rule names gets the generic profile, and the log says so.
 *
 * The settings a row holds (ADR 0031, "A profile holds settings only"):
 *   - eye: the per-eye size each texture-quality level asks the headset runtime for (texture_quality.h, ADR 0015).
 *   - systemHeapMiB: the System heap in MiB (combat/heaps.cpp); 0 keeps the row's own target.
 *   - levelHz: the refresh rate the combat maps (and, with combat.refresh_rate_lobby, the combat lobby) run at (combat/refresh_rate.cpp).
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace nevr_quest::headset_profile {

// What the identity step reads. An empty field is a property the device does not give (or one not read yet).
struct Inputs {
  std::string_view manufacturer;  // ro.product.manufacturer
  std::string_view model;         // ro.product.model
  std::string_view device;        // ro.product.device
  std::string_view systemName;    // XrSystemProperties.systemName: the next input (ADR 0031, R1), not read yet
};

enum class Id : std::uint8_t { kQuest2, kQuest3, kGeneric };

struct EyeSize {
  int width;
  int height;
};

// Indexed by texture_quality::Level's ordinal: 0 is `stock` (no size of its own), 1 very_low up to 6 ultra.
inline constexpr std::size_t kEyeLevelCount = 7;

struct Profile {
  Id id;
  const char* name;             // the profile's name in the headset_profile line
  const EyeSize* eye;           // kEyeLevelCount entries
  int systemHeapMiB;            // the System heap (combat/heaps.cpp); 0 keeps the row's own target
  float levelHz;                // the combat-map and lobby refresh rate
};

// Quest 2 (ro.product.device "hollywood"): the source build's memsys:253 and the "hollywood" check the heap and refresh-rate
// modules made before this table (docs/quest/combat/heaps.md R07, docs/quest/combat/adr/0002-level-refresh-rate-default.md). The eye sizes are
// the Quest 2 column of the community patch's Texture Resolution table (ADR 0015): measured on no headset here.
inline constexpr EyeSize kQuest2Eyes[kEyeLevelCount] = {
    {0, 0}, {1152, 1264}, {1440, 1584}, {1728, 1904}, {2016, 2224}, {2304, 2528}, {2592, 2848}};
inline constexpr Profile kQuest2 = {Id::kQuest2, "quest2", kQuest2Eyes, 512, 72.f};

// Quest 3 (ro.product.device "eureka"). The eye sizes are the Quest 3 column of the same table: the one the patch's values
// were measured on (ADR 0015). The heap keeps the row's target and the rate is the match rate, as the heap and refresh-rate modules gave
// every headset that is not hollywood before this table.
inline constexpr EyeSize kQuest3Eyes[kEyeLevelCount] = {
    {0, 0}, {1344, 1408}, {1680, 1760}, {2376, 2568}, {3072, 3380}, {3584, 3944}, {4096, 4506}};
inline constexpr Profile kQuest3 = {Id::kQuest3, "quest3", kQuest3Eyes, 0, 90.f};

// Any headset no rule names. Its eye table is the Quest 2 one: at every level that table is no larger than the Quest 3 one, so
// no such headset gets a bigger eye buffer than the Quest 3 values would give it. The heap and rate are the ones the heap and refresh-rate
// modules gave every headset that is not a Quest 2 (0 keeps the row's target; 90 Hz). None of these values is measured on a
// headset that no rule names. A generic headset's own size is not inferred (ADR 0031, R3); a headset that needs other values
// gets its own rule and row.
inline constexpr Profile kGeneric = {Id::kGeneric, "generic", kQuest2Eyes, 0, 90.f};

// The rules, in order. An empty device matches any headset: the last rule. The "eureka" value is not measured yet: no headset
// log in the tree names it, and the tests use it as a Quest 3 fixture only. The headset_profile line of a Quest 3 run states
// the device it reads; if that is not "eureka", the Quest 3 rule matches nothing and the headset takes the generic profile,
// whose eye table is the smaller Quest 2 one.
struct Rule {
  const char* name;         // the rule field of the headset_profile line
  std::string_view device;  // ro.product.device, exact; empty matches any device
  const Profile* profile;
};
inline constexpr Rule kRules[] = {
    {"quest2_device", "hollywood", &kQuest2},
    {"quest3_device", "eureka", &kQuest3},
    {"generic", "", &kGeneric},
};

struct Match {
  const char* rule;
  const Profile* profile;
};

// The first rule whose inputs match names the headset. Empty properties match the generic rule.
inline Match Identify(const Inputs& in) noexcept {
  for (const Rule& r : kRules) {
    if (r.device.empty() || in.device == r.device) return {r.name, r.profile};
  }
  return {"generic", &kGeneric};  // not reached: the last rule matches every device
}

// The match for ro.product.device alone, the one input the rules read today. The combat modules read only that property (#2037).
inline Match IdentifyDevice(std::string_view device) noexcept {
  Inputs inputs;
  inputs.device = device;
  return Identify(inputs);
}

}  // namespace nevr_quest::headset_profile
