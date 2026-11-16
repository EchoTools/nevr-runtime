#pragma once

// The differences between the stock mp_weapon_settings and the combat gun patch that he_is_the_cat's EchoCombat build starts
// from (heisthecat31/EchoCombat at nevr-pack 82ad7ba), as values merged into the player's own stock resource at run time
// (balance_merge.h), so no game bytes are shipped. His build reads the gun patch's mp_weapon_settings (tools/fix_dot.py:164)
// and stages it (tools/fix_dot.py:220-223), then merges config/mp_weapon_settings.json over it (tools/apply_config.py:33-35),
// so a key his config does not name stays as the gun patch has it. Those keys are these. The gun patch's record is
// data/gunpatch/607d858c90268f9a/11a8523ed4724ca7; against the stock record exactly these ten fields differ (eight rows here,
// drone's cooldown and damage row are `balance_values.h` and `balance_values_absent.h`):
//
//   damage_table.magnum          absent in stock; the gun patch has head 1.5, arm 1, leg 1, torso 1, ordnance 1, barrier 1.75, ssi 1
//   damage_table.magnum_aoe      absent in stock; seven 1s
//   damage_table.smg             absent in stock; head 1.4, arm 1, leg 0.5, torso 1, ordnance 1, barrier 0.5, ssi 1
//   damage_table.chain           absent in stock; seven 1s
//   damage_table.rifle           absent in stock; head 1.65, arm 1, leg 0.75, torso 1, ordnance 1, barrier 0.5, ssi 1
//   grenade_properties.loc       absent in stock; cooldown 35
//   player_properties.disable_arm_grab_team / _enemy   true in stock; false
//
// One owner per field (ADR 0039): `damage_table.magnum.head` is `balance_values.h`'s (his config sets 1.5556 over the gun
// patch's 1.5), so it is not here; `grenade_properties.loc` has no cooldown in `balance_values.h`, so `cooldown` is here and
// its other keys stay there. The rule: a value that is a field of mp_weapon_settings is a balance value, merged by the one
// merge both platforms run, and never a content-pack-build step (a step writing the same resource would be a second writer).
// A key named here is not in `balance_values.h`, and no key is in both (test_combat_settings.cpp pins that).

#include <cstddef>
#include <cstdint>
#include <string>

#include "core/combat_balance/balance_merge.h"
#include "core/combat_balance/balance_values.h"
#include "core/combat_balance/balance_values_absent.h"

namespace nevr_combat::balance {

inline constexpr char kGunPatchWeaponSettings[] = R"ec({
 "damage_table": {
  "magnum": {"arm": 1, "leg": 1, "torso": 1, "ordnance": 1, "barrier": 1.75, "ssi": 1},
  "magnum_aoe": {"head": 1, "arm": 1, "leg": 1, "torso": 1, "ordnance": 1, "barrier": 1, "ssi": 1},
  "smg": {"head": 1.4, "arm": 1, "leg": 0.5, "torso": 1, "ordnance": 1, "barrier": 0.5, "ssi": 1},
  "chain": {"head": 1, "arm": 1, "leg": 1, "torso": 1, "ordnance": 1, "barrier": 1, "ssi": 1},
  "rifle": {"head": 1.65, "arm": 1, "leg": 0.75, "torso": 1, "ordnance": 1, "barrier": 0.5, "ssi": 1}
 },
 "grenade_properties": {
  "loc": {"cooldown": 35}
 },
 "player_properties": {
  "disable_arm_grab_team": false,
  "disable_arm_grab_enemy": false
 }
}
)ec";

/// The values both edges merge into the stock mp_weapon_settings, in his build's order (tools/deploy_and_launch.py:5-8 at
/// 82ad7ba): the gun patch's (the base fix_dot.py stages), then his config's over it (apply_config.py:33-35), then the
/// values added only when that result still lacks them (fix_drone.py:79, `setdefault`, run on the merged JSON).
inline Result ApplyShippedValues(const std::uint8_t* blob, std::size_t size, std::string* out, std::size_t* changed) {
  return ApplyLayers(blob, size, {kGunPatchWeaponSettings, kCatWeaponSettings}, out, changed, kCatAbsentValues);
}

}  // namespace nevr_combat::balance
