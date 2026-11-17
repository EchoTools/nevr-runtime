#pragma once

// The UMBRAL VEIL wall (#2242, H-WALL-a): the barrier wall script (ac4e9374a146d3a4, symcache name
// mp_combat_ability_barrier_actor) that takes a veil token and becomes the veil. Ported from he_is_the_cat's veil_fx.py
// (read as text at 82ad7ba; never run). The stock wall's show and physics nodes ask VeilNet's token block whether this
// wall is the veil; this file holds the decision and the renew rule as pure C++, and the one byte site Cat's tool writes
// in place.
//
// NAMED DEBT (PLAN-v7 section 16): the sites below are NOT installed by this module. Cat's tool reaches them with code
// caves (new executable sections and jumps into them), and a cave needs new executable memory this tree has no pattern
// for; his stub bytes are his output and are not copied. The debt is paid by reconstructing FUN_180006D30 and
// FUN_180007190 through nevr-giveyouup after the release, then hooking them (PLAN-v7 4.3 and 8). KNOWN-WRONG #TBD: the
// issue number for this marker is owed by the dispatcher.
//
// Sites (veil_fx.py, the line numbers are his tool's):
//   0x5CC0  global data length, `mov eax,158h; ret` -> `mov eax,160h; ret` (:140, :195). PORTED here as PatchGlobalLen,
//           a checked patch. Not applied at load: its consumer is the show site below, which is not installed, so the
//           patch alone would only grow every wall's global block.
//   0x6D78  ShowModel node, `mov byte [rsp+48h],1` -> call the decision (:141, :196). NOT DONE: a mid-function write of a
//           stack local in FUN_180006D30 (0x6D30..0x6E00). A function-entry hook cannot change it; it needs the body
//           reconstructed, or a cave (no pattern in the tree).
//   0x6D30  ShowModel node entry -> the renew stub (:1097). NOT DONE: a cave. RenewOnEntry is its logic; a hook on the
//           entry (a detour row) would carry it once the show site exists.
//   0x7190  EnablePhysics node entry -> the renew stub with RENEW_AFTER_PHYS (:1098). NOT DONE: a cave.
//   0x71E9  EnablePhysics's `mov byte [rsp+48h],1` -> call the physics decision (:1101). NOT DONE: mid-function, as 0x6D78.
//   0x3DE0  setup_bindings' call to a node's Bind, rewired to a bind stub (:1095). NOT DONE: a cave, and it binds the
//           wall's own sub-node.
//   0x27D0  the wall's PLAYSOUNDNODE exec (:1105, mute). H-WALL-b's row, not this piece's.
//
// The decision: a wall takes at most one veil token. Taking one needs an unexpired token with a count above 0; the count
// goes down by one (compare-and-swap) and the veil counter at +10 goes up. A wall with no token becomes a barrier when it is
// decided for good (+14 goes up), and stays undecided when only looked at. The decision is kept per wall in the wall's
// global data (the decision cell at +158h); a pooled wall that starts a new life after RENEW_AFTER without a run clears it.
//
// ADR 0038: the decision runs once per wall spawn (the show node, a per-event entry), never per frame. Nothing here is
// hooked, so nothing runs per frame and no hook cost is measured; no measurement is taken (no live run).
//
// Quest: not applicable in this change (PLAN-v5 decision 3 puts Quest out of the plan; the Quest wall is the sentinel's
// ecfx path, quest/combat/ecfx.cpp, which this module does not touch).

#include <cstddef>
#include <cstdint>

#include "runtime/patch/checked_patch.h"
#include "runtime/patch/ec_rules.h"

namespace nevr_combat::wall_script {

// The script and its byte site (values from the records named in each comment).
inline constexpr std::uint64_t kScript = 0xac4e9374a146d3a4ULL;  // ac4e9374a146d3a4.dll (src/runtime/log/symcache_data.cpp:57032)
inline constexpr std::uint32_t kGlobalLenFnRva = 0x5CC0;         // script_get_global_data_len (veil_fx.py:140)
inline constexpr std::uint32_t kGlobalLenStock = 0x158;          // the stock global block: `mov eax, 158h` (veil_fx.py:140)
inline constexpr std::uint32_t kGlobalLenWithDecision = kGlobalLenStock + 8;  // the patch's block (:195)
inline constexpr std::size_t kGlobalLenSize = 6;                              // mov eax, imm32; ret

// The wall's cell: one u32 at +158h, the first of the 8 bytes the patch adds to the global block. Its low byte is the
// Decision of this wall; the upper 24 bits are its renew clock (renew units, kClockMask). Decide and the renew keep the
// byte each does not write. The cell ends at +15Bh: +15Ch is POINT's state (veil_fx.py POINT_STATE, H-WALL-b), and that
// dword is left to it.
inline constexpr std::uint32_t kCellOffset = kGlobalLenStock;
inline constexpr std::uint32_t kCellClockShift = 8;
inline constexpr std::uint32_t kDecisionMask = 0xFF;

// The veil token block: the EC page's cell (ec_rules.h kVeilTok, 0x2104CC0). Layout as VeilNet writes it: +00 count (LONG),
// +08 expiry (u64, interrupt time in 100 ns), +10 walls that became the veil, +14 walls that stayed barriers.
inline constexpr std::uint32_t kVeilTokenRva = nevr_ec::kPageRva + nevr_ec::kVeilTok;
static_assert(kVeilTokenRva == 0x2104CC0, "the veil token block is the EC page's kVeilTok");
inline constexpr std::uint32_t kTokenCount = 0x00;
inline constexpr std::uint32_t kTokenExpiry = 0x08;
inline constexpr std::uint32_t kTokenShown = 0x10;     // +10: walls that became the veil
inline constexpr std::uint32_t kTokenBarriers = 0x14;  // +14: walls that stayed barriers

// The renew clock: 2^20 x 100 ns units (about 0.105 s), 24 bits, wrapping after about 20 days (veil_fx.py:800-801).
inline constexpr std::uint32_t kTimeShift = 20;
inline constexpr std::uint32_t kClockMask = 0xFFFFFF;
inline constexpr std::uint64_t kHundredNsPerSecond = 10'000'000;
inline constexpr std::uint32_t kRenewAfterUnits = static_cast<std::uint32_t>(8 * kHundredNsPerSecond >> kTimeShift);     // 8 s
inline constexpr std::uint32_t kRenewAfterPhysUnits = static_cast<std::uint32_t>(14 * kHundredNsPerSecond >> kTimeShift);  // 14 s

/// The wall's decision: the low byte of its cell. A byte that holds no other value reads as undecided.
enum class Decision : std::uint32_t { kUndecided = 0, kVeil = 1, kBarrier = 2 };

/// The decision the cell holds.
Decision CellDecision(std::uint32_t cell);
/// The cell with its decision byte set to `decision` and its clock kept.
std::uint32_t WithDecision(std::uint32_t cell, Decision decision);
/// The renew clock the cell holds (renew units, 24 bits).
std::uint32_t CellClock(std::uint32_t cell);

/// What the show node asks: the veil (1), a barrier (0), or no decision yet (2), as the stock contract decide() gave.
enum class Verdict { kBarrier, kVeil, kUndecided };

/// Takes one veil token from `token` when one is held at `now`: the count is above 0 and `now` is before the expiry. A
/// compare-and-swap on the count, then the +10 counter goes up. Returns whether one was taken.
bool TakeVeilToken(std::uint8_t* token, std::uint64_t now);

/// The show node's decision for one wall. `cell` is the wall's cell (see kCellOffset). A decided wall keeps its decision and
/// takes nothing. An undecided one takes a held veil token (kVeil, +10); failing that, when `decideForGood` it becomes a
/// barrier (kBarrier, +14); when only looked at it stays undecided (kUndecided).
Verdict Decide(std::uint8_t* token, std::uint32_t& cell, std::uint64_t now, bool decideForGood);

/// A pooled wall's renew check, at a spawn node's entry. The cell's clock is moved to `now` on every call. Returns true when
/// the wall starts a new life (no run for at least `afterUnits`).
bool NewLife(std::uint32_t& cell, std::uint64_t now, std::uint32_t afterUnits);

/// NewLife for a wall: a new life clears the decision (to undecided). An `aborting` node (being torn down) touches neither the
/// clock nor the decision. Returns whether the wall started a new life.
bool RenewOnEntry(std::uint32_t& cell, bool aborting, std::uint64_t now, std::uint32_t afterUnits);

/// The 6-byte check-and-patch of the global data length (0x5CC0): stock `mov eax,158h; ret` to `mov eax,160h; ret`.
nevr_checked_patch::Site GlobalLenSite();

/// What one checked application of the length patch found and did. `found` is the six bytes the site held BEFORE any write
/// (on a success Apply writes over them, so the record must not read the site afterwards); `writeError` is the writer's Win32
/// error (WriteErrorFn; 0 when no write failed).
struct GlobalLenOutcome {
  nevr_checked_patch::Result result;
  std::uint8_t found[kGlobalLenSize];
  std::uint32_t writeError;
};

/// Applies GlobalLenSite to the function at `fn` and reports the outcome. Writes nothing on a mismatch. The caller resets the
/// writer's WriteErrors before the call (as mode_patches.cpp does); the error is read after the write.
GlobalLenOutcome ApplyGlobalLen(std::uint8_t* fn, nevr_checked_patch::WriteFn write, nevr_checked_patch::WriteErrorFn writeError);

/// The Rule 6 / Rule 10 record of a patch that did not apply (a mismatch, a failed write, or a write that reported an error),
/// as one `[NEVR.COMBAT]` line: name, va, expected, actual (the bytes found before the write), result, and write_error on a
/// write that failed or reported one, then a consequence. Written to `out` (NUL-terminated, truncated at `cap`); returns
/// snprintf's result.
int FormatGlobalLenRecord(char* out, std::size_t cap, std::uintptr_t va, const GlobalLenOutcome& outcome);

/// ApplyGlobalLen, and a Warning (FormatGlobalLenRecord) when the patch did not apply. Not called in a run: see the header note.
nevr_checked_patch::Result PatchGlobalLen(std::uint8_t* fn, std::uintptr_t va, nevr_checked_patch::WriteFn write,
                                          nevr_checked_patch::WriteErrorFn writeError);

}  // namespace nevr_combat::wall_script
