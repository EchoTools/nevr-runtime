#pragma once

// Balance values that are added to the stock mp_weapon_settings only when the stock body lacks them (balance_merge.h,
// MergeAbsent). They are kept apart from balance_values.h, whose values overwrite and which is embedded byte for byte.
//
// Origin: he_is_the_cat's tools/fix_drone.py of heisthecat31/EchoCombat at 82ad7ba. DRONE_DAMAGE (lines 41-42) is
// {"head": 1, "arm": 1, "leg": 1, "torso": 1, "ordnance": 1, "barrier": 1, "ssi": 1}, "the same spread as the shipped
// bullet weapons: full body, light on barriers", and line 79 adds it with
// `settings["damage_table"].setdefault("drone", dict(DRONE_DAMAGE))`: a row the document already has is kept.
// Whether the stock mp_weapon_settings has a damage_table.drone row is not established here (it needs a stock data
// folder); the merge is correct either way.

namespace nevr_combat::balance {

inline constexpr const char kCatAbsentValues[] = R"ec({"damage_table":{"drone":{"head":1,"arm":1,"leg":1,"torso":1,"ordnance":1,"barrier":1,"ssi":1}}})ec";

}  // namespace nevr_combat::balance
