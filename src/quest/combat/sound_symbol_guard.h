/* A sound post that can only fail is not made, at the two call sites where dropping it leaves nothing undone (#1737; docs/quest/combat/
 * sound_symbol_guard.md).
 *
 * What the game does. CSoundPlayer::Play3DSound(this, CSoundSymbol w1, emitter id w2, volume s0) hands the 32-bit symbol to
 * CWwiseEngine::PostSoundEvent as the Wwise event id. For an id no loaded bank defines, PostEvent answers 0, PostSoundEvent records the "not a
 * valid wwise identifier" error and Play3DSound passes its status to OutputError (sound_symbol_guard_targets.h, the paragraph that opens
 * "Where the "not a valid wwise identifier" text is made"). Two
 * classes of symbol take that path on every post: 0, Wwise's invalid id, and the bullet site's default 0x1406b30a, which no stock bank defines
 * (runtime/combat/sound_symbol_guard_rules.h, kUnregistered).
 *
 * What this does. A CallbackThunk with the caller (`kCaller`) on Play3DSound's JUMP_SLOT (sound_symbol_guard_targets.h kPins.play3DSlot).
 * PlaySound's direct call, its deferred call (the GLOB_DAT word is PlaySound itself) and the bullet site all reach Play3DSound through that one
 * slot's PLT stub, which leaves x30 alone (`br x17`), so the return address names the call site. In a combat map or the combat lobby, a post whose
 * symbol `nevr_combat::sound_symbol_guard::Drops` is dropped, returning 0 without calling Play3DSound, only when it returns to PlaySound (ELF
 * 0x170698c) or to the bullet site (0x21230d4):
 *   - 0 is Play3DSound's own answer when audio is disabled (`mov w0, wzr` at 0x134b488). PlaySound's `cmn w0, #1; b.ne` (0x1706990) takes
 *     any answer but -1 past its -1 branch; what PlaySound prints for 0 is inferred (PlaySound is not reconstructed;
 *     docs/quest/combat/sound_symbol_guard.md). The bullet site ignores the result.
 *   - Both sites post on an emitter created with CreateEmitter(name, 0, 0), so bit 0 of emitter+0xbc ("destroy the emitter when its sound does not
 *     start") is clear: a failed post there destroys nothing either, so a drop leaves nothing undone.
 *   - CRxMaterialFXCS::TriggerSingle posts on an emitter that carries the bit (CreateEmitter(name, 1, 0)): a drop there would skip the destroy the
 *     failed post makes and could leak the emitter. Every caller other than the two sites therefore gets Play3DSound unchanged, whatever the symbol
 *     (counted `kept_other_caller`).
 * Every post that is not dropped reaches Play3DSound with the same four arguments, and its answer is returned.
 *
 * Arena (same APK). A performance guard, not a crash guard: the reading of OutputError in sound_symbol_guard_targets.h found a fault-capable
 * path inside it but not a fault on this path (the two open questions are named in sound_symbol_guard.md, Switch and default), so README
 * rule 9's crash-only exception does not apply. The handler tests
 * `scope::SeenCombatOrLobby()` first and, outside a combat level, tail-calls Play3DSound with the same arguments: no counter, no read, no line.
 * The combat body is `[[gnu::noinline]]` and starts with `scope::LookIsFresh()`. The switch is in `ScopeNormal()`'s reader list, and the install
 * refuses `scope_not_live` when the answer cannot change.
 *
 * Which APKs (ADR 0046 rule 5). The combat variants (the normal step of combat_install.h). Switch: `combat.sound_symbol_guard`, default
 * false: upstream ships its sound guard off, and with no fault shown the guard is a cost saving a player opts into.
 *
 * Owner (ADR 0039). This file owns whether a post of an unplayable symbol from PlaySound or the bullet site reaches Play3DSound. Nothing else hooks
 * Play3DSound's slot (`git grep 0x3701090 -- src`).
 *
 * Origin. The fxsndguard switch of kotorvr's echoquest-combat, runtime/native/afkxr/afk_fx.cpp (the sound guards; the lines are named in
 * sound_symbol_guard_targets.h), read as text and rebuilt on this tree's hook manager. The symbol list, the call-site gate and the drop value are
 * this tree's, from its own measurements (docs/quest/combat/sound_symbol_guard.md, What it hooks).
 *
 * Lines (sentinel log):
 *   install  {"event":"combat_sound_symbol_guard_install","result":"installed|off|refused","reason":"ok|off_config|<token>","slot":"0x..","msg":".."}
 *            Info for installed and off, Error for a refusal (scope_not_live, module_not_loaded, no_build_id, build_id_mismatch,
 *            pinned_word_mismatch with "pin", "vaddr", "found", "expected"; or the manager's status by name: original_mismatch, ...)
 *   drop     {"event":"combat_sound_symbol_guard_drop","symbol":"0x..","class":"zero|unregistered","caller":"play_sound|bullet_site","n":<n>,"msg":".."}
 *            Debug, the first kDropLines drops of the process; then
 *   drops    {"event":"combat_sound_symbol_guard_drops","n":<n>,"since_last":<n>,"msg":".."}  Debug, one line saying later drops are counted
 *            (since_last 0), then the count at most every kDropEveryMs while drops continue (nevr::BurstThenCount)
 *   total    {"event":"combat_sound_symbol_guard_total","dropped_zero":<n>,"dropped_unregistered":<n>,"kept_other_caller":<n>,"calls":<n>,"msg":".."}
 *            Info at the end of the process, installed only; `calls` counts the posts seen in a combat level
 *
 * Built with -fno-exceptions, like every translation unit that includes callback_thunk.h.
 */
#pragma once

#include <cstdint>

#include "quest/combat/combat_install.h"
#include "quest/combat/sound_symbol_guard_targets.h"
#include "quest/sentinel/callback_thunk.h"
#include "quest/sentinel/got_hook.h"

namespace nevr_quest::combat::sound_symbol_guard {

inline constexpr unsigned kDropLines = 16;            // drop lines written one by one per process
inline constexpr std::uint64_t kDropEveryMs = 10000;  // then a counted line at most this often while drops continue

// Play3DSound(CSoundPlayer* x0, CSoundSymbol w1, unsigned emitter w2, float volume s0) -> int w0. The symbol is taken as the whole x1 register and
// passed on as it is; the rule reads its low 32 bits (the CSoundSymbol: PlaySound masks it, `and x1, x2, #0xffffffff` at ELF 0x1706980).
using Play3DSig = std::int32_t(void* player, std::uint64_t symbol, std::uint32_t emitter, float volume);
struct Play3DTag {};
using Play3DThunk = sentinel::CallbackThunk<Play3DTag, Play3DSig, /*kCaller=*/true>;

// The slot, the function it must hold, and the two return addresses where a drop leaves nothing undone (ELF addresses; the store libr15's are
// sound_symbol_guard_targets.h's pins: kPins, and the "the return address" words of kDropWords and kWords). The host test supplies its own.
struct Pins {
  std::uint64_t slot, fn;
  std::uint64_t playSoundReturn;   // CSoundCS::PlaySound, after its `bl` at 0x1706988
  std::uint64_t bulletSiteReturn;  // CR15NetBulletCS::Update, after its `bl` at 0x21230d0
};
inline constexpr Pins kPins = {targets::kPins.play3DSlot, targets::kPins.play3DFn, 0x170698cULL, 0x21230d4ULL};

enum class Caller : std::uint8_t { kPlaySound, kBulletSite, kOther };
// The call site of a post from its return address `ret`, for the libr15 loaded at `base`.
constexpr Caller CallerOf(std::uintptr_t ret, std::uintptr_t base, const Pins& pins) noexcept {
  return ret == base + pins.playSoundReturn ? Caller::kPlaySound : ret == base + pins.bulletSiteReturn ? Caller::kBulletSite : Caller::kOther;
}
constexpr const char* CallerName(Caller c) noexcept {
  return c == Caller::kPlaySound ? "play_sound" : c == Caller::kBulletSite ? "bullet_site" : "other";
}

// The step (combat_install.cpp): `on` is `combat.sound_symbol_guard`. Off answers {false, "off", off}.
ModuleResult Install(bool on) noexcept;
// The guard on the libr15 `lookup` finds, with `pins`: the combat level answer is live, build ID, the pinned words, then the slot.
ModuleResult InstallAt(sentinel::ImageLookup lookup, const Pins& pins) noexcept;

// The end-of-process step (the row's `end`): the total line, when the guard is installed.
void NoteEnd() noexcept;

struct Counts {
  std::uint64_t calls;                // posts seen in a combat level (fresh look)
  std::uint64_t droppedZero;          // dropped, symbol 0
  std::uint64_t droppedUnregistered;  // dropped, a kUnregistered symbol
  std::uint64_t keptOtherCaller;      // a symbol the rule drops, from a caller that is neither site: passed to Play3DSound
};
Counts CurrentCounts() noexcept;

// ---- test support ------------------------------------------------------------------------------------------------------
// How Install finds libr15, and the pins it uses; nullptr restores production.
void SetInstallForTest(sentinel::ImageLookup lookup, const Pins* pins) noexcept;
// The clock the drop lines' budget reads (ms); nullptr restores CLOCK_MONOTONIC.
void SetClockForTest(std::uint64_t (*nowMs)()) noexcept;
// The return address of the last post seen in a combat level, less the image base (0 before any).
std::uint64_t LastCallerForTest() noexcept;
// Forgets the hook, clears the thunk, the counters and the line budget.
void ResetForTest() noexcept;

}  // namespace nevr_quest::combat::sound_symbol_guard
