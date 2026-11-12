/* Which of the three headset gates a run installs: the party-permissions gate (`combat.gate_perm`), the bot-body gate (`combat.gate_bots`)
 * and the menu gate (`combat.gate_link`), from the `combat:` keys.
 *
 * The three gates depend on no other switch. The bot-body gate also has a condition that is not a switch: the combat bot (body 1) exists only in the
 * full-size global_mp, which loads only where levelname's hook is on the game's LoadLevelName slot. The gate asks the game, once per bot call
 * (headset_gates.h "The bot gate"), so it holds whichever module installs levelname and whatever switch turns it off.
 *
 * Origin. The combat plugin's `combat_gates.h` (`DecideCombatGates`, imported in #1591), with its build options and kill files replaced by the `combat:` keys
 * (docs/adr/0046 rule 3). docs/audits/2026-11-05-q15t-origin-citations.md lists no reference-build file or EchoCombat line for these gates.
 *
 * Header only, constexpr, built with -fno-exceptions like every combat file.
 */
#pragma once

namespace nevr_quest::combat::gates {

// The three keys.
struct Inputs {
  bool perm;  // combat.gate_perm
  bool bots;  // combat.gate_bots
  bool link;  // combat.gate_link
};

// The decision, with the token each gate's `combat_gates_install` field carries when it is not installed: "" when it is installed, "off_config"
// (the key is false).
struct Choice {
  bool perm;
  bool bots;
  bool link;
  const char* permReason;
  const char* botsReason;
  const char* linkReason;
};

constexpr Choice Decide(const Inputs& in) {
  return {in.perm, in.bots, in.link, in.perm ? "" : "off_config", in.bots ? "" : "off_config", in.link ? "" : "off_config"};
}

}  // namespace nevr_quest::combat::gates
