/* The spawn-tube windows and the barrier panes show the team colour as on the PC (#2186, docs/quest/combat/shieldgamma.md, docs/adr/0046).
 *
 * What the game does. The PC's pixel shader decodes every material colour with pow(saturate(x), 2.2); the Quest decodes only colourspace 1,
 * so a team colour written in linear space comes out yellow-orange instead of red-orange on the shield material `dbde1b05fa8f6200` (the spawn
 * shield and the dyson and gauss barrier panes). The launch-tube window scene binds another material, `b28e940eb21a5798`, so the script's
 * team-colour writes never reach the windows, which draw a fixed blue.
 *
 * What this does. A thunk on CSetMaterialPropertyNode::Update's JUMP_SLOT (shieldgamma_targets.h), acting on a vec4 write in colourspace 0 to
 * the shield material, and only then:
 *   gamma (`combat.shieldgamma_gamma`, default OFF, a test switch in the reference build): rgb := pow(sat(rgb), 2.2) for that one call; the node's own
 *     value is put back after it.
 *   tube mirror (`combat.shieldgamma_tube_mirror`, default ON): the ring colour (property `dd35e419672ee0ba`) written to the shield material is also
 *     written to the tube window material, by a second call of Update on a copy of the node data, rgb decoded the same way and whatever the
 *     gamma switch says. The original call's result is the one returned. The tube windows' own pack row (an asset change) is not this module's.
 *
 * Arena (same APK). Arena and combat run on the same APK, and Update is a generic script node that arena's scripts call too, every update while a
 * duration runs. The handler asks `scope::InCombat()` first (combat_scope.h: a combat map or the combat lobby is loaded, no Echo Arena level
 * is), and outside it calls the game's function with the same arguments and does nothing else: no read of the data, no write, no counter, no
 * line. An arena match on the combat APK costs this hook the call through the thunk and one relaxed load, where the q15t plugin it replaces
 * read 0x58 bytes with process_vm_readv on every call.
 *
 * Owner (ADR 0039). This file owns the colour the game's Update is called with for the shield material and the second write to the tube
 * window material; the q15t plugin no longer hooks this slot.
 *
 * Which APKs (ADR 0046 rule 5). The normal step of the q15t and combat variants (combat::Active()); the release APK installs nothing.
 *
 * Switches (rule 4). `combat.shieldgamma_gamma` (false) and `combat.shieldgamma_tube_mirror` (true). Both false: Info `combat_shieldgamma_install` with
 * `reason: off_config`, nothing installed. The q15t plugin's build bitmask `Q15T_SHIELDGAMMA` is gone; the switch files its header named are not read.
 *
 * Frequency (ADR 0038). Update runs once per update of a material-property node, every update while its duration runs; its rate per frame is NOT
 * measured (no headset run). The handler's cost on a call that is not a combat level's is the thunk plus one relaxed load. In combat each call adds
 * one process_vm_readv of 0x58 bytes, as the plugin's did in every level.
 *
 * Origin. The q15t plugin's `shieldgamma.cpp`, our implementation of the reference build's `runtime/native/afkxr/afk_shieldgamma.cpp` (see
 * shieldgamma_targets.h for the lines). Imported in #1591 from kotorvr's echoquest-combat work.
 *
 * Lines (sentinel log):
 *   install  {"event":"combat_shieldgamma_install","result":"installed|refused|off","reason":"ok|off_config|<GotStatus>|<token>","gamma":<0|1>,
 *             "tube_mirror":<0|1>,"material":"0x..","msg":".."}
 *   decode   {"event":"combat_shieldgamma_decode","n":<n>,"prop":"0x..","r":"..","g":"..","b":"..","to_r":"..","to_g":"..","to_b":"..","msg":".."}  1st, 2nd, 4th ..
 *   mirror   {"event":"combat_shieldgamma_mirror","n":<n>,...same fields...}  1st, 2nd, 4th ..
 * and the manager's own `got_hook` line for the slot. Nothing is logged per call.
 *
 * Built with -fno-exceptions, like every translation unit that includes callback_thunk.h.
 */
#pragma once

#include <cstdint>

#include "quest/combat/combat_install.h"
#include "quest/sentinel/got_hook.h"

namespace nevr_quest::combat::shieldgamma {

// The module's normal step, with its two switches: enrols the combat level answer (combat_scope.h), then installs the thunk and writes the
// install line. Both false installs nothing.
ModuleResult Install(bool gamma, bool tubeMirror) noexcept;
// The same on the libr15 the lookup finds (the host test supplies an image built in memory).
ModuleResult InstallAt(sentinel::ImageLookup lookup, bool gamma, bool tubeMirror) noexcept;

struct Stats {
  std::uint32_t decoded;
  std::uint32_t mirrored;
  std::uint32_t unreadable;
};
Stats CurrentStats() noexcept;

// ---- test support ------------------------------------------------------------------------------------------------------

// Forgets the hook, the thunk's state, the switches and the counters.
void ResetForTest() noexcept;

}  // namespace nevr_quest::combat::shieldgamma
