#pragma once

// he_is_the_cat's balance values for Echo Combat: config/mp_weapon_settings.json of heisthecat31/EchoCombat at nevr-pack
// c3ff4b54, embedded byte for byte. His tools/apply_config.py merged it into the game's mp_weapon_settings when he built
// his pack; the runtime merges it into the player's own stock resource as the game loads it (balance_merge.h), so no game
// bytes are shipped. Keys that start with `_` are his notes and are not merged.

namespace nevr_combat::balance {

inline constexpr const char kCatWeaponSettings[] = R"ec({
 "_about": "EchoCombat's values, in the shape of the game's mp_weapon_settings.json. Every deploy / release merges them into the game's own mp_weapon_settings (packed with the rest of the game data, so change a value here, then deploy or make the release again). Keys the game itself does not use (OVERCHARGE's damage_bonus_* / head_body_part / cheat_death_* / link_window_ms / heal_fraction, the veil_* smoke look, POINT's and the translocator's effect_* / sound_copies) are read by the build tools, and OVERCHARGE's by PowerBoost.dll through PowerBoost.ini.",
 "damage_table": {
  "magnum": {
   "_why": "PROTOSTAR head hit 70 (45 x 1.5556; stock 1.5 x 50 = 75).",
   "head": 1.5556
  }
 },
 "weapon_properties": {
  "smg": {
   "_why": "QUARK: stock instability cooled from 0.05 s after a shot while it fires every 0.06 s, so per shot it gained +0.2 and lost 15/s x ~0.01 s: a +0.05 margin that frame timing tipped either way (some players never got spread, others hit the 3.5 max in a few shots). Cooling now waits 0.1 s (only once you stop firing, as on the other guns), and +0.05 per shot keeps the intended ramp: full spread after ~20 shots.",
   "instability_cooldown_delay": 0.1,
   "instability_per_shot": 0.05,
   "_why_vent": "Venting from full heat takes 1.4 s (stock 120/s: ~0.8 s), with the heat cap of 100.",
   "heat_venting_rate": 71.43
  },
  "chain": {
   "_why": "BLAZAR: 60 shots before it overheats, 20% more (stock 2 heat a shot: 50), with the heat cap of 100.",
   "heat_per_shot": 1.667
  },
  "magnum": {
   "_why": "PROTOSTAR: 45 a body hit (stock 50; the head multiplier in damage_table makes 70 a head hit), 0.6 s between shots (stock 0.5), and 10% faster shots (44 m/s, stock 40).",
   "projectile_damage": 45,
   "refire_delay": 0.6,
   "projectile_velocity": 44
  }
 },
 "grenade_properties": {
  "drone": {
   "health": 70,
   "hit_scale": 1.0,
   "death_effect_size": 0.5,
   "fire_rate": 0.28,
   "max_target_distance": 20,
   "bullet_speed": 70,
   "bullet_damage": 2,
   "cooldown": 30
  },
  "dot": {
   "radius": 3.5,
   "cooldown": 30
  },
  "loc": {
   "effect_size": 2.0,
   "effect_lifetime": 2.5,
   "effect_time": 2.5,
   "sound_copies": 12
  }
 },
 "ability_properties": {
  "buff": {
   "dr": 0.09,
   "required_charge": 40,
   "do_rez_buff": 1,
   "heal_fraction": 0.05,
   "damage_bonus_body": 1,
   "damage_bonus_head": 2,
   "head_body_part": 5,
   "cheat_death_seconds": 5,
   "cheat_death_grace_ms": 1500,
   "link_window_ms": 500,
   "_why_self_damage": "OVERCHARGE costs its user self_damage_per_tick health every self_damage_tick_seconds while it runs (applied by the server; it never takes the last point of health unless self_damage_lethal). aim: head = the cone follows where the head looks, gun = where the gun points.",
   "self_damage_per_tick": 5,
   "self_damage_tick_seconds": 1.0,
   "self_damage_lethal": false,
   "aim": "head"
  },
  "smoke": {
   "_why_cost": "Each puff is a see-through layer, and in VR the cost goes with particle count x puff area. Emission 4 -> 1.25 with puffs 3.5 -> 4.0 is about 40% of the old cost; opacity 35 -> 70 so fewer puffs still hide as much. veil_cloud_lifetime is how long the veil lasts, so it stays.",
   "veil_cloud_count": 1,
   "veil_cloud_size": 5.0,
   "veil_cloud_lifetime": 8.0,
   "veil_cloud_spread": 0.0,
   "veil_cloud_emission": 1.25,
   "veil_puff_size": 4.0,
   "veil_opacity": 70.0,
   "veil_shade": 0.3
  },
  "point": {
   "radius": 2.5,
   "charge_required": 45,
   "charge_up_time": 1.25,
   "stun_duration": 1.25,
   "effect_size": 4.0,
   "effect_lifetime": 3.0,
   "effect_time": 5.0,
   "charge_indicator_repeat": 0.4,
   "sound_copies": 2
  }
 },
 "medigun": {
  "_about": "MEDIGUN (a COMET that heals one teammate with a blue beam). enabled: false = off: servers don't offer it and clients hide it (a selected one stays a plain COMET). A server offers it when its EchoCombat build was made with enabled: true (written into PowerBoost.ini [server] medigun, which VeilNet.dll reads and tells its clients). The rest is its tuning; the deploy copies it into ability_properties.medigun, where its script reads it.",
  "enabled": false,
  "heal_scale": 0.85,
  "beam_seconds": 15,
  "overheat_seconds": 4,
  "cool_seconds": 5,
  "cone_angle": 20,
  "range": 20,
  "cone_slack": 5,
  "hand": "auto",
  "_why_offset": "hand_offset: where the beam starts, in the gun bone's space (x, y, z metres): PowerBoost.dll writes it into the beam effect every frame and rereads PowerBoost.ini every second, so it can be tuned while the beam is on. (0.05, 0.07, 0.12): the average of where the muzzle worked out to from the hand bones.",
  "hand_offset": "0.064,0.000,0.203",
  "_why_offset_left": "hand_offset_left: the same for the left hand, whose bone is mirrored (the right hand's numbers left it about 6 cm low).",
  "hand_offset_left": "0.124,0,0.203",
  "_why_bone": "beam_bone: the hand bone the beam starts from (EXP_L1_ / EXP_R1_ + this, by the weapon arm): ToolAttach1, Hand1, HandPistol1, HandRifle1, HandBFG1, Index3, Middle3, ArmGameIkOppHandItem1, ... (PowerBoost.dll HAND_BONES). Reread every second; the next link uses it. The beam's own position offset moves nothing (tested).",
  "beam_bone": "ToolAttach1",
  "_why_aim": "aim: gun = the cone follows the gun (its transform's +z, gun_axis); head = where the head looks. The gun transform is on the gun hand (head-local -x is right: an earlier note had it on the off hand, a sign mistake).",
  "aim": "gun"
 }
}
)ec";

}  // namespace nevr_combat::balance
