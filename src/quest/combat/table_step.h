/* What the four resource table-step modules (meshstep, instmodel, shaderstep, tablestep) share: the scope they act in, the code-word pin
 * check, the libr15 identity check and the warning for a plugin kill file (#2177, docs/adr/0046).
 *
 * The modules. Each gives the CTableXT of one resource class a growth step of 1 where the game gives 32, so a table holds the entries it has
 * and no slack (docs/quest/combat/heaps.md: the resource heap is raised for combat and these tables are wanted with it). They run when the game
 * constructs the resource, on whatever thread loads it.
 *
 * Scope (the arena rule of docs/adr/0046). Arena and combat run on the same APK, so `combat::Active()` does not say which one a player is in,
 * and a step of 1 costs a table a reallocation for every entry added, a price arena has no reason to pay. The modules act only while the
 * game's loaded levels say combat: `combat::level::Classify` (combat_level.h) over the CR15Game the combat tick verified (tick::Game), with
 * the lobby rule off. A combat map loaded and no Echo Arena level loaded is `kCombat`; an Echo Arena level wins over a combat map still loaded
 * during a transition, so the resources an arena match builds on the way out of combat are stock. Every other answer, the lobbies arena players
 * use included, is `kOther`, and a resource built before the tick has verified the game is `kUnknown`: both build the game's own object. The
 * answer is read at each construction and never cached, because the resources of a level are built right after the level is stored and a
 * cached answer is exactly the one that is wrong for them (the cost is in each module's header).
 *
 * The pins. A pinned code word is an instruction the module's reading of the game relies on; `FirstPinMismatch` reads each with
 * process_vm_readv (safe_read.h) and names the first that differs, so a build that moved one is refused whole with nothing hooked.
 *
 * The kill files. The plugin's `<module>.off` files are not read (docs/adr/0046 rule 3: a switch is a `combat:` key). `WarnLegacySwitch` looks
 * for one in the plugin's data directory and writes a Warning naming the key that replaces it.
 *
 * Built with -fno-exceptions, like every combat translation unit.
 */
#pragma once

#include <cstddef>
#include <cstdint>

#include "quest/sentinel/got_hook.h"

namespace nevr_quest::combat::table_step {

// One instruction word of libr15 the module's reading relies on: its ELF virtual address and its value in the store build.
struct PinWord {
  std::uint64_t vaddr;
  std::uint32_t word;
};

enum class Scope {
  kCombat,   // a combat map is loaded and no Echo Arena level is: the modules act
  kOther,    // anything else, the lobbies arena players use included: the game's own object
  kUnknown,  // no game verified yet, or the loaded levels are unreadable: the game's own object
};
const char* ScopeName(Scope scope) noexcept;

// What the loaded levels say right now. Reads the game's gamespace tables (a few process_vm_readv calls); never cached.
Scope CurrentScope() noexcept;

// The first of `pins[0..count)` whose word in the libr15 at `base` differs from the pinned one, or nullptr when every one holds. `have` gets the
// word read (0 when unreadable) and `readable` whether it could be read.
const PinWord* FirstPinMismatch(std::uintptr_t base, const PinWord* pins, std::size_t count, std::uint32_t* have, bool* readable) noexcept;

// Finds libr15 through `lookup` and checks its build ID against the patched build's. nullptr when it is the pinned build; otherwise the fixed
// token the module's install line carries: "module_not_loaded", "no_build_id" or "build_id_mismatch".
const char* FindPinnedLibR15(sentinel::ImageLookup lookup, sentinel::ElfImage* out) noexcept;

// What a shader-set resource's table step became: the correction both factory modules make (shaderstep.h, tablestep.h). The table is the
// CTableXT at object + 0x78, whose CMemBlock is first, so its growth step is at table + 0x20 = object + 0x98 (the layout custctx's DrawTable
// reads). kSet: the game wrote 0x20 and this wrote 1 (read back). kNotStock: the word was not 0x20, so nothing was written. kUnreadable: the
// object could not be read, or the write did not read back.
inline constexpr std::uintptr_t kShaderSetStepOffset = 0x98;
inline constexpr std::uint64_t kShaderSetStockStep = 0x20;
inline constexpr std::uint64_t kShaderSetStep = 1;
enum class StepFix { kSet, kNotStock, kUnreadable };
// Sets the step of a freshly constructed CGShaderSetResource at `obj` to 1 when it still holds the game's 0x20. `found` (optional) gets the word
// read. Reads with process_vm_readv, so a bad pointer is kUnreadable and never a fault.
StepFix ApplyShaderSetStep(void* obj, std::uint64_t* found = nullptr) noexcept;

// A Warning (event `combat_<module>_legacy_switch`) when the plugin's kill file `file` exists in its data directory, naming `key`. Writes nothing
// when it does not, or when the directory cannot be named.
void WarnLegacySwitch(const char* event, const char* file, const char* key) noexcept;

// ---- test support ------------------------------------------------------------------------------------------------------

// The CR15Game the scope reads (production: the object the combat tick verified). nullptr restores production.
using GameFn = std::uintptr_t (*)() noexcept;
void SetGameForTest(GameFn fn) noexcept;
// The directory the kill files are looked for in (production: /sdcard/Android/data/<package>/files/q15t). nullptr or "" restores production.
void SetLegacyDirForTest(const char* dir) noexcept;
// How many times CurrentScope read the game's levels (a call that found no game made no read).
std::uint32_t ScopeReadsForTest() noexcept;
void ResetForTest() noexcept;

}  // namespace nevr_quest::combat::table_step
