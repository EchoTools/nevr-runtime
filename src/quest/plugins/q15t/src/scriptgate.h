// scriptgate (issue #1601): the menu scripts that lock combat to a Quest headset read "not a Quest" and every other script keeps the
// headset's real kind.
//
// The game library answers the headset-type expression with one kind for every script (CVR::HeadsetType: 3, a Quest). The script
// libraries each carry their own binding of that expression (R15HEADSETTYPEEXPRESSION::Bind): it asks the engine for the four
// outputs by name ("rift", "rifts", "quest", "link"), and the engine stores each output's slot in the library's own exported
// data word, `R15HEADSETTYPEEXPRESSION::output_<name>_offset` (rift 0, rifts 1, quest 2, link 3). A condition in the library reads
// that word every time it evaluates. So pointing a library's quest word at the rifts slot makes that library's "quest" output read
// the rifts flag, which a Quest never sets: its scripts take the PC branch, while every script in a library left alone still
// sees a Quest. An answer given from CVR::HeadsetType (headset_gates.h) cannot do this, because its caller is the same engine
// code for every script and the call carries no script.
//
// The libraries are the ones whose headset-type uses are all combat locks (kScriptGateLibs). The write is one aligned 32-bit
// store into the library's .bss, not a code word: no PROT_EXEC, no mprotect. It is made only where the quest word holds the 2 the
// engine stored and the rifts word holds 1; the engine stores the words when it binds a library, so the tick looks again until
// every library is bound and puts the word back should the engine bind a library again. Details, evidence and prediction:
// docs/quest/q15t/modules/scriptgate.md.
#pragma once

#include <cstddef>
#include <cstdint>

#include "import_hook.h"

namespace q15t {

// The script libraries (file names in the app's native library directory) whose every use of the headset-type expression is a
// combat lock: the combat buttons and sub-buttons, FIND COMBAT, the combat mode card, the combat level pages and text, and the
// "combat allowed" prefab.
inline constexpr const char* kScriptGateLibs[] = {
    "lib555db38933fef859.so", "lib1da85b18cbad6297.so", "lib4e984de06cafe2ab.so", "lib2d5ebd0e1895f269.so", "libcab86dd5921bafc7.so",
    "lib5e311be23be97835.so", "libbad7ce9b6c6df2cb.so", "libf36316e246f2ca21.so", "lib8325751e75df5860.so", "lib18000621255c6ea5.so",
    "lib1f28bd4fa7d724fb.so", "lib3ded824fccc2bf6d.so", "lib5ddc3dcdad4a3ed5.so", "lib8515ab072bc095a8.so", "libb56c58de2b68fc61.so",
    "libb7882b9d804f86c1.so", "libb8bd659eb6ed3927.so", "libbc170a382fda5499.so", "libd280bb8333ad45ed.so", "libfb4ad3853db57943.so",
};
inline constexpr std::size_t kScriptGateLibCount = sizeof kScriptGateLibs / sizeof kScriptGateLibs[0];

// The exported data words of a script library (Itanium names of R15HEADSETTYPEEXPRESSION::output_quest_offset / output_rifts_offset).
inline constexpr const char* kScriptGateQuestSym = "_ZN24R15HEADSETTYPEEXPRESSION19output_quest_offsetE";
inline constexpr const char* kScriptGateRiftsSym = "_ZN24R15HEADSETTYPEEXPRESSION19output_rifts_offsetE";
inline constexpr std::int32_t kQuestSlot = 2;  // what the engine stores for "quest"
inline constexpr std::int32_t kRiftsSlot = 1;  // what the engine stores for "rifts", and what the quest word is pointed at

// Gate order: `<q15t data dir>/scriptgate.off` present (reason "off_file", one INFO line). Writes nothing; the tick does the work.
InstallResult InstallScriptGate();

// A TickFn (game_tick.h), every frame on the game thread. Once every second until all libraries are bound it looks up the ones not
// yet found (dlopen RTLD_NOLOAD, the reference is kept); every frame it makes sure the quest word of each found library holds 1.
void ScriptGateTick(std::uintptr_t game);

struct ScriptGateStats {
  std::uint32_t bound = 0;     // libraries found and holding the rifts slot in their quest word
  std::uint32_t found = 0;     // libraries found
  std::uint32_t writes = 0;    // quest words written (a second write on one library means the engine bound it again)
  std::uint32_t refused = 0;   // libraries found whose words did not look as expected (nothing written)
};
ScriptGateStats GetScriptGateStats();

// Test seams: a resolver standing in for dlopen+dlsym (name -> the library's two words, or null words when not loaded), and a reset.
struct ScriptGateWords {
  std::int32_t* quest;
  std::int32_t* rifts;
  bool loaded;
};
void SetScriptGateResolverForTest(ScriptGateWords (*resolve)(const char* lib));
void SetScriptGatePeriodMsForTest(std::int64_t ms);  // the lookup period (default 1000)
void ResetScriptGateForTest();

}  // namespace q15t
