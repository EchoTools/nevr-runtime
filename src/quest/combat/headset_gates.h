/* The three headset gates (#2180, docs/adr/0046): the game decides "this is a Quest headset" with two imported calls, CVR::HeadsetType and
 * CR15NetGame::PartyMemberHeadsetType, and three places in the game's code branch on that answer in a way a Quest running combat has to skip.
 * A gate answers a different value to the one caller it is for, and the real answer to every other:
 *
 *   gate_perm  CR15NetPartyPermissionsExpression::operator() asks PartyMemberHeadsetType for each member of the party and clears the combat
 *              permission flag of a member whose kind is 3 (the Quest kind). The gate answers "not a Quest" (0) to that call.
 *   gate_bots  CR15NetGame::BotActorId asks HeadsetType and sends a Quest to bot body 0, the arena bot. The gate answers 0 to that call, so a
 *              combat match picks the combat bot (body 1) by the bot's symbol. Only where levelname is installed (see "The bot gate").
 *   gate_link  BindExpression<CR15HeadsetTypeExpression> turns HeadsetType's answer into the four flags the menu scripts read. The gate answers the
 *              PC build's Link kind (4), which sets the link flag and not the Quest flag, to the Bind when the script library that holds the
 *              Bind is in the policy (`combat.gate_link_libs`: by default the 21 libraries of the combat entries, kDefaultLinkLibs).
 *
 * No code page is written: each gate reads the return address of the call it is for, which is data. The two slots (PartyMemberHeadsetType's
 * and HeadsetType's JUMP_SLOTs) hold assembly entries (callback_thunk.h rule 7, headset_gates.cpp): a CallbackThunk's handler would see its own
 * thunk's return address, not the game's. HeadsetType's one entry serves both gates on its slot; a gate that is off has an empty range.
 *
 * Owner (ADR 0039). This file owns both slots. The kill files `gate_perm.off`, `gate_bots.off` and
 * the data file `gate_libs.txt` are no longer read: one found in the data directory is a Warning (`combat_gates_legacy_switch`) that
 * names the key that replaces it.
 *
 * Another handler on a slot. A gate acts on the game's own call site, so its entry stays the newest hook on its slot: the manager refuses a hook
 * that asks to join above it (`slot_held_by_entry`, a Warning naming both owners), and the entry installs above any handler already there and calls
 * it (kSlotHeldByEntry, #2219). `headset_gates_test` runs both orders.
 *
 * The bot gate. The combat bot (body 1) exists only in the full-size global_mp, which loads only where levelname's hook is on the game's
 * LoadLevelName slot (CR15Game's vtable word at +0x260, kPins.levelNameSlot: the stock function is `base + kPins.levelNameStock`, and a word that
 * is anything else, a hook, means levelname is on). The gate reads that word at each bot call (a data read with process_vm_readv, which never faults;
 * an unreadable word is "not installed") and answers the real value otherwise (`levelname_not_installed`). So it depends on no levelname switch, in
 * the levelname module or the runtime, and holds when levelname moves. Not the loaded level: the level table holds the name the game was asked for
 * (`global_mp`) before LoadLevelName appends `_lowspec`, so the table cannot tell the two apart (CncaGameSpace::LoadLevel stores its argument at gs+0x58
 * and then calls the virtual).
 *
 * Arena (same APK). Arena and combat run on the same APK, so `combat::Active()` does not say which one a player is in:
 *   - gate_bots acts only while a combat map or the combat lobby is loaded (combat_level.h); in an Echo Arena level, and in every other level, it
 *     answers the real value, because BotActorId chooses a bot body at random for a symbol it does not list, and body 1 does not exist in an arena
 *     level. The game's frame tick supplies the CR15Game
 *     (`Tick`); with no game seen yet the gate answers the real value.
 *   - gate_perm acts wherever the permissions expression runs: the flag it affects is the combat permission of a party member who bought combat.
 *   - gate_link acts for the script libraries of the policy, in the menus and the lobby script wherever they run. The lobby script is shared by
 *     the arena and combat lobbies (it loads the lobby's combat wing); its answer is the same in both.
 *
 * Which APKs (ADR 0046 rule 5). The combat variants only (the normal step of combat_install.h).
 *
 * Frequency (ADR 0038). An entry runs once per call of its function by anyone: PartyMemberHeadsetType once per party member each time the
 * expression is evaluated; HeadsetType once per Bind of the expression (a menu script's node) and once per BotActorId. None is a per-frame call
 * (not measured on a headset: `combat_gates_headset_bind` counts the binds per library, and the gate counters are in each answered line). Each call does
 * an acquire load, the real call, two relaxed counters and a range compare; a call in the link range also names the Bind's library (dladdr) and takes
 * a mutex once per bind; a bot call also reads the loaded levels (process_vm_readv, never faults).
 *
 * Origin. The combat plugin's `headset_gates.cpp` (imported in #1591) (return-address-gated wrappers; its trampoline reads the caller's sp and x30) and `combat_gates.h`,
 * reconstructed on the manager. docs/audits/2026-11-05-q15t-origin-citations.md lists no reference-build file or EchoCombat line for them.
 *
 * Lines (sentinel log):
 *   hook      {"event":"combat_gates_hook","slot":"party|headset","gates":"perm|bots,link","symbol":"..","status":"<GotStatus>"}  Info when installed
 *   word      {"event":"combat_gates_hook","gate":"..","status":"pinned_word_mismatch","vaddr":"0x..","have":"0x..|unreadable","want":"0x.."}  Warning
 *   policy    {"event":"combat_gates_link_policy","libs":"..","source":"default|config"}
 *   install   {"event":"combat_gates_install","result":"installed|partial|refused|off","gate_perm":"..","gate_bots":"..","gate_link":"..","consequence":".."}
 *   answered  {"event":"combat_gates_answered","gate":"perm|bots","n":<n>,"return_address":"0x..","real_answer":<n>}  the first 8 per gate
 *   scoped    {"event":"combat_gates_scoped_out","gate":"bots","n":<n>,"reason":"echo_arena|other_level|no_game"}  the first 8
 *   bind      {"event":"combat_gates_headset_bind","lib":"..","answered":"link|real","real_answer":<n>,"calls":<n>}  the first bind of a library and its 64th
 *   legacy    {"event":"combat_gates_legacy_switch","file":"..","replaced_by":".."}  Warning
 *
 * Built with -fno-exceptions, like every translation unit that includes callback_thunk.h.
 */
#pragma once

#include <cstddef>
#include <cstdint>

#include "quest/combat/combat_install.h"
#include "quest/combat/headset_gates_targets.h"
#include "quest/sentinel/got_hook.h"

namespace nevr_quest::combat::headset_gates {

// The answer of a gate that says "not a Quest": any value other than 3, which is all the three functions compare with (0).
inline constexpr std::int32_t kNotQuest = 0;
// The PC build's own kind for a Quest on Link: the Bind sets only its flag3, never the Quest flag (kind 3). It must be one of the kinds the Bind
// maps (0..4): an answer above 4 leaves all four flags clear.
inline constexpr std::int32_t kLinkHeadset = 4;

// The 21 script libraries whose headset-type expression decides a combat entry on a Quest: the 20 of the combat menu (the combat buttons, FIND
// COMBAT, the combat mode card, the level select pages, the "combat allowed" prefab) and the lobby script (libf808c072bb49da47.so), whose two
// LoadLevel branch nodes read the expression's quest output to pick the lobby wing that is loaded (a Quest gets the arena wing only; the Link
// answer takes the combat wing's pin). Every other script that binds the expression (47 libraries in all) gets the real answer.
inline constexpr char kDefaultLinkLibs[] =
    "lib555db38933fef859.so,lib1da85b18cbad6297.so,lib4e984de06cafe2ab.so,lib2d5ebd0e1895f269.so,libcab86dd5921bafc7.so,"
    "lib5e311be23be97835.so,libbad7ce9b6c6df2cb.so,libf36316e246f2ca21.so,lib8325751e75df5860.so,lib18000621255c6ea5.so,"
    "lib1f28bd4fa7d724fb.so,lib3ded824fccc2bf6d.so,lib5ddc3dcdad4a3ed5.so,lib8515ab072bc095a8.so,libb56c58de2b68fc61.so,"
    "libb7882b9d804f86c1.so,libb8bd659eb6ed3927.so,libbc170a382fda5499.so,libd280bb8333ad45ed.so,libfb4ad3853db57943.so,"
    "libf808c072bb49da47.so";
// The longest policy text (a NUL included): `combat.gate_link_libs` is read into a buffer of this size.
inline constexpr std::size_t kMaxLinkLibsText = 640;

// The `combat:` keys the step reads.
struct Switches {
  bool perm = true;       // combat.gate_perm
  bool bots = true;       // combat.gate_bots
  bool link = true;       // combat.gate_link
  const char* linkLibs = nullptr;  // combat.gate_link_libs; null: kDefaultLinkLibs
};

// The step on the loaded libr15. Fixed status tokens: "installed", "partial", "refused", "off", "already_ran", "module_not_loaded",
// "no_build_id", "build_id_mismatch".
ModuleResult Install(const Switches& switches) noexcept;
// The same on the image `lookup` finds, with `pins` (kPins in production; a host test points the ranges at its own code).
ModuleResult InstallAt(sentinel::ImageLookup lookup, const Pins& pins, const Switches& switches) noexcept;

// Which script libraries the link gate answers. `spec`: "all" (every caller from the link range), "none" or empty (no caller: the real answer
// everywhere), or space/comma separated name fragments ("0e4384a5 1703a89b"): a library whose file name contains one is answered, every other
// library gets the real answer. A library that cannot be named counts as not in the list. Called by the install before the entry goes in; on the
// host (no Bind frame) a call from the link range is a library that cannot be named.
void SetLinkGateLibs(const char* spec) noexcept;
// How many distinct libraries have bound the expression from the link range (each logged one `combat_gates_headset_bind` line).
std::uint32_t LinkGateLibsSeen() noexcept;

// The file name (no directory) of the library that holds `addr`; false when dladdr does not know it, and `out` is left alone then.
bool LibraryName(std::uintptr_t addr, char* out, std::size_t n) noexcept;

// The game's frame tick (combat_tick.h): remembers the CR15Game the bot gate reads the loaded levels of. Registered by the install when gate_bots
// is on. Exposed for the host test.
void Tick(std::uintptr_t game) noexcept;

struct Stats {
  std::uint32_t partyCalls, partyGated;
  std::uint32_t headsetCalls, botsGated, botsScopedOut, linkGated;
};
Stats CurrentStats() noexcept;

// ---- test support ------------------------------------------------------------------------------------------------------

// Whether `addr` is in [lo, hi): the range compare of every gate, with the upper edge exclusive (the instruction at `hi` is not the caller).
bool InRange(std::uintptr_t addr, std::uintptr_t lo, std::uintptr_t hi) noexcept;

// The caller ranges the install published, as absolute addresses (an empty range, lo == hi, gates nothing): the host cannot call the production
// ranges, so a test compares them with the pins.
struct Ranges {
  std::uintptr_t permLo, permHi, botsLo, botsHi, linkLo, linkHi;
};
Ranges RangesForTest() noexcept;
// The image lookup Install uses; nullptr restores sentinel::FindLoadedImage.
void SetImageLookupForTest(sentinel::ImageLookup lookup) noexcept;
// The pins Install uses; nullptr restores kPins.
void SetPinsForTest(const Pins* pins) noexcept;
// Forgets the hooks, the ranges, the policy, the game and the counters.
void ResetForTest() noexcept;

}  // namespace nevr_quest::combat::headset_gates
