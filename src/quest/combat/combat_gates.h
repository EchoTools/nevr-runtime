/* Which of the three headset gates a run installs: the party-permissions gate (`combat.gate_perm`), the bot-body gate (`combat.gate_bots`)
 * and the menu gate (`combat.gate_link`), from the `combat:` keys.
 *
 * One dependency is enforced here, not left to the operator: the bot-body gate makes a combat match spawn bot body 1, the combat bot, and only the
 * full-size global_mp level carries that body (the Quest's cut `global_mp_lowspec` has the arena bot only). The full level loads only while
 * levelname is on (`combat.levelname`), so the bot-body gate is off whenever levelname is off (reason `needs_levelname`). The permissions gate and
 * the menu gate depend on nothing.
 *
 * Origin. The q15t plugin's `combat_gates.h` (`DecideCombatGates`), with its build options and kill files replaced by the `combat:` keys
 * (docs/adr/0046 rule 3). docs/audits/2026-11-05-q15t-origin-citations.md lists no reference-build file or EchoCombat line for these gates.
 *
 * Header only, constexpr, built with -fno-exceptions like every combat file.
 */
#pragma once

namespace nevr_quest::combat::gates {

// The four keys.
struct Inputs {
  bool perm;       // combat.gate_perm
  bool bots;       // combat.gate_bots
  bool link;       // combat.gate_link
  bool levelname;  // combat.levelname
};

// The decision, with the token each gate's `combat_gates_install` field carries when it is not installed: "" when it is installed,
// "off_config" (the key is false) or, for the bot gate only, "needs_levelname".
struct Choice {
  bool perm;
  bool bots;
  bool link;
  const char* permReason;
  const char* botsReason;
  const char* linkReason;
};

constexpr Choice Decide(const Inputs& in) {
  Choice c{false, false, false, "", "", ""};
  c.perm = in.perm;
  c.permReason = c.perm ? "" : "off_config";
  c.link = in.link;
  c.linkReason = c.link ? "" : "off_config";
  c.bots = in.bots && in.levelname;
  c.botsReason = c.bots ? "" : !in.bots ? "off_config" : "needs_levelname";
  return c;
}

}  // namespace nevr_quest::combat::gates
