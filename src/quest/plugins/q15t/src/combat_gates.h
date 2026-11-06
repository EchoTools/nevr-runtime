// Which of the three combat gates a run installs: the party-permissions gate (the source build's G1), the bot-body gate (G6) and
// levelname (G7), which the runtime owns now (src/quest/combat/levelname.h, `combat.levelname`).
//
// The gates are compiled in by default (CMakeLists.txt) and the permissions and bot-body gates each have a kill file in the plugin data directory
// (module_switch.h): `gate_perm.off`, `gate_bots.off`. The `levelname` inputs stay in this decision so that the bot-body gate's dependency is stated in
// one place: the bot-body gate makes a combat match spawn bot body 1, the combat bot, and only the full-size global_mp level carries that body (the
// Quest's cut `global_mp_lowspec` has the arena bot only), so the gate is off whenever levelname is off (reason `needs_levelname`). The plugin
// takes levelname as met and does not read `levelname.off` (plugin.cpp, KNOWN-WRONG #2180: the bot gate moves into the runtime with that unit and then
// reads `combat.levelname`). The permissions gate depends on nothing.
//
// Header-only and constructor-free (the repack's static-init check refuses a plugin with a namespace-scope constructor).
#pragma once

#include "module_switch.h"

namespace q15t {

// What is compiled in (the three build options).
struct CombatGatesBuilt {
  bool perm;
  bool bots;
  bool levelname;
};

// Which kill files exist in the plugin data directory.
struct CombatGatesOffFiles {
  bool perm;
  bool bots;
  bool levelname;
};

// The decision, with the reason token each module's `module install` record carries when it is not installed: "" when it is
// installed, "not_built", "off_file" or (bots only) "needs_levelname".
struct CombatGatesChoice {
  bool perm;
  bool bots;
  bool levelname;
  const char* perm_reason;
  const char* bots_reason;
  const char* levelname_reason;
};

constexpr CombatGatesChoice DecideCombatGates(CombatGatesBuilt built, CombatGatesOffFiles off) {
  CombatGatesChoice c{false, false, false, "", "", ""};
  c.levelname = built.levelname && !off.levelname;
  c.levelname_reason = c.levelname ? "" : !built.levelname ? "not_built" : "off_file";
  c.perm = built.perm && !off.perm;
  c.perm_reason = c.perm ? "" : !built.perm ? "not_built" : "off_file";
  c.bots = built.bots && !off.bots && c.levelname;
  c.bots_reason = c.bots ? "" : !built.bots ? "not_built" : off.bots ? "off_file" : "needs_levelname";
  return c;
}

inline constexpr char kGatePermOffFile[] = "gate_perm.off";
inline constexpr char kGateBotsOffFile[] = "gate_bots.off";
inline constexpr char kLevelNameOffFile[] = "levelname.off";

// The kill files as they are now. Never creates anything.
inline CombatGatesOffFiles ReadCombatGatesOffFiles() {
  return {ModuleSwitchPresent(kGatePermOffFile), ModuleSwitchPresent(kGateBotsOffFile), ModuleSwitchPresent(kLevelNameOffFile)};
}

}  // namespace q15t
