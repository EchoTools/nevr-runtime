/* The wrist HUD's weapon-slot heat fill stops being 0 x 0 (#2186, docs/quest/combat/hudmeter.md, docs/adr/0046).
 *
 * What the game does. The helmet's script sets the size of the element `weapon_meter` through CSetCanvasElementAttributesNode::Enter. On the
 * Quest the first write runs with a zero-seeded start state and writes 0 x 0, and the next node reads weapon_meter's own size (now 0 x 0) back as
 * its "full size" and keeps 0 for ever, so the heat bar never fills (hudmeter_targets.h, the reference build's note at its top).
 *
 * What this does. A thunk on Enter's JUMP_SLOT (hudmeter_targets.h). A size write of exactly 0 x 0 to the item `weapon_meter` becomes a write of
 * the authored full size 86 x 88 for that one call: the node's data buffer is set, the game's Enter runs, and the 0 x 0 is put back. Every other
 * call goes to the game's function with its own arguments. Any canvas component is guarded, not only the lobby helmet's: the first other one is
 * named once.
 *
 * Arena (same APK). Arena and combat run on the same APK, and Enter is a generic script node that arena's scripts call too. The handler tests
 * `scope::SeenCombatOrLobby()` first (combat_scope.h: a combat map or the combat lobby was loaded at the last look, no Echo Arena level), and outside
 * it calls the game's function with the same arguments and does nothing else: no read of the data, no write, no counter, no line. The combat body
 * is another function (`ActInCombat`, not inlined), which first checks that the look is fresh (`scope::LookIsFresh()`: a tick that stopped makes a
 * true answer false after kStaleAfterMs). So an arena match on the combat APK costs this hook its call through the thunk and, in the handler, this
 * (arm64, from the object file):
 *   adrp x8, g_bits; mov x3, x0; ldr w8, [x8]; tbnz w8, #2, pass; and w8, w8, #3; cbz w8, pass; mov x0, x3; b ActInCombat
 *   pass: mov x0, x1; mov x1, x2; br x3
 * (NDK r26d clang -O2: no frame, no stack canary, no call; the arena path is the tbnz or the cbz, then the `br`.)
 * where the q15t plugin it replaces read 0x40 bytes of the data with process_vm_readv on every call, in every level.
 *
 * Owner (ADR 0039). This file owns the size of `weapon_meter` the game's Enter is called with; the q15t plugin no longer hooks this slot.
 *
 * Which APKs (ADR 0046 rule 5). The normal step of the q15t and combat variants (combat::Active()); the release APK installs nothing.
 *
 * Switch (rule 4). `combat.hudmeter`, default true. Off: Info `combat_hudmeter_install` with `reason: off_config`, nothing installed. The q15t
 * plugin's `hudmeter.on` file is not read.
 *
 * Frequency (ADR 0038). Enter runs once per script update of a canvas-attribute node: its rate per frame is NOT measured (no headset run; the
 * reference build's lobby log says the weapon_meter writes are a few per second while the HUD fills, and no count of the other nodes). The handler's
 * cost on a call that is not a combat level's is the thunk plus the instructions above. In combat it adds one process_vm_readv of 0x40 bytes per call,
 * which is the cost the plugin had in every level.
 *
 * Origin. The q15t plugin's `hudmeter.cpp`, which is our implementation of the reference build's `runtime/native/afkxr/afk_hudmeter.cpp` (see
 * hudmeter_targets.h for the lines); the rev 4 "old bar" part of the reference build is not built. Imported in #1591 from kotorvr's
 * echoquest-combat work.
 *
 * Lines (sentinel log):
 *   install  {"event":"combat_hudmeter_install","result":"installed|refused|off","reason":"ok|off_config|<GotStatus>|<token>","msg":".."}
 *   full     {"event":"combat_hudmeter_full","n":<n>,"to_x":"86","to_y":"88","comp":"0x..","item":"0x..","msg":".."}  the 1st, 2nd, 4th, 8th .. write
 *   pass     {"event":"combat_hudmeter_pass","x":"..","y":"..","full_writes_before":<n>,"msg":".."}   once per helmet start
 *   fill     {"event":"combat_hudmeter_fill","x":"..","y":"..","msg":".."}   the first 8 changes of the height, then none
 *   comp     {"event":"combat_hudmeter_component","comp":"0x..","lobby_helmet_comp":"0x..","msg":".."}   once
 * and the manager's own `got_hook` line for the slot. Nothing is logged per call.
 *
 * Built with -fno-exceptions, like every translation unit that includes callback_thunk.h.
 */
#pragma once

#include <cstdint>

#include "quest/combat/combat_install.h"
#include "quest/sentinel/got_hook.h"

namespace nevr_quest::combat::hudmeter {

// The module's normal step, with its switch (combat_install.cpp reads it from the configuration): enrols the combat level answer
// (combat_scope.h), then installs the thunk on the loaded libr15 and writes the install line.
ModuleResult Install(bool on) noexcept;
// The same on the libr15 the lookup finds (the host test supplies an image built in memory).
ModuleResult InstallAt(sentinel::ImageLookup lookup, bool on) noexcept;

struct Stats {
  std::uint32_t callsGuarded;  // size writes to weapon_meter seen, in a combat level (any value)
  std::uint32_t fullWrites;    // 0 x 0 writes made 86 x 88
  std::uint32_t unreadable;    // data arguments that could not be read (passed on untouched)
};
Stats CurrentStats() noexcept;

// ---- test support ------------------------------------------------------------------------------------------------------

// Forgets the hook, the thunk's state, the counters and the logged-once flags.
void ResetForTest() noexcept;

}  // namespace nevr_quest::combat::hudmeter
