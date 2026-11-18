#pragma once

// The UMBRAL VEIL wall (#2242, H-WALL-a and H-WALL-b): the barrier wall script (ac4e9374a146d3a4, symcache name
// mp_combat_ability_barrier_actor) that takes a veil token and becomes the veil, and in point builds takes a point token
// and goes off as POINT, a stun field. Ported from he_is_the_cat's veil_fx.py (read as text at 82ad7ba; never run). The
// stock wall's show and physics nodes ask VeilNet's token blocks whether this wall is the veil or POINT; this file holds the
// decisions, POINT's state, the mute rule, the FX queue writer and the renew rule as pure C++, and the byte sites Cat's tool
// writes in place.
//
// NAMED DEBT (PLAN-v7 section 16): the sites below are NOT installed by this module. Cat's tool reaches them with code
// caves (new executable sections and jumps into them), and a cave needs new executable memory this tree has no pattern
// for; his stub bytes are his output and are not copied. The debt is paid by reconstructing FUN_180006D30 and
// FUN_180007190 through nevr-giveyouup after the release, then hooking them (PLAN-v7 4.3 and 8). KNOWN-WRONG #2760: the
// named debt above is open and tracked by #2760.
//
// Sites (veil_fx.py, the line numbers are his tool's):
//   0x5CC0  global data length, `mov eax,158h; ret` -> `mov eax,160h; ret` (:140, :195). PORTED here as PatchGlobalLen,
//           a checked patch. Not applied at load: its consumer is the show site below, which is not installed, so the
//           patch alone would only grow every wall's global block. Deleted when the side table lands (PLAN-v7 section 8).
//   0x6D78  ShowModel node, `mov byte [rsp+48h],1` -> call the decision (:141, :196). NOT DONE: a mid-function write of a
//           stack local in FUN_180006D30 (0x6D30..0x6E00). A function-entry hook cannot change it; it needs the body
//           reconstructed, or a cave (no pattern in the tree). The point build's show dispatch (show_dispatch, :625) is at
//           the same site and is NOT DONE for the same reason.
//   0x6D30  ShowModel node entry -> the renew stub (:1097). NOT DONE: a cave. RenewOnEntry is its logic; a hook on the
//           entry (a detour row) would carry it once the show site exists.
//   0x7190  EnablePhysics node entry -> the renew stub with RENEW_AFTER_PHYS (:1098). NOT DONE: a cave.
//   0x71E9  EnablePhysics's `mov byte [rsp+48h],1` -> call the physics decision (:1101). NOT DONE: mid-function, as 0x6D78.
//   0x3DE0  setup_bindings' call to a node's Bind, rewired to a bind stub (:1095). NOT DONE: a cave, and it binds the
//           wall's own sub-node.
//   0x27D0  the wall's PLAYSOUNDNODE exec (:145, :614 mute_stub, :1105). NOT DONE: the mute stub runs from a cave. MutesSound
//           is its decision, and the stub is written only in point builds (veil_fx.py:1002-1003, :1105).
//
// H-WALL-b NOT DONE, besides the sites above (all of them: no node body or engine call is written here):
//   - point_block (:700), the EnablePhysics node of a POINT wall: its wait, its fire, its sound carry-on and its charge-up
//     indicator run the stun mine's burst, the stun explosion and the sound through engine calls from inside the node. The
//     stun explosion is R15NETACTIVATEEXPLOSIONNODE, transplanted from the stun mine's script (ACTIVATE_EXPLOSION, :527), and
//     needs that script's code in a cave. NOT DONE, KNOWN-WRONG #2760.
//   - the point wall's four FX stand-ins (defer_stub, :662, for d_charge, d_burst, d_boom, d_sound) and the spawn stubs
//     (spawn_stub, point_spawn, point_charge, point_boom, point_sound): caves. EnqueueWallFx is the queue writer they call.
//   - the adopt window (decide_code, :841, LAST_TAKE_RVA 0x2104FE0 at :833, ADOPT_WINDOW at :838): a wall with no token
//     within one second of the last take makes the same decision. Decide and DecideWithPoint do NOT port it, because H-WALL-a
//     landed without it and changing that decision is not part of this piece. The cell 0x2104FE0 is not in PLAN-v7 section 7.
//     NOT DONE, KNOWN-WRONG #2760.
//
// The decision: a wall takes at most one token of each kind. A point build tries a point token first (DecideWithPoint),
// then a veil token (Decide). Taking one needs an unexpired token with a count above 0; the count goes down by one
// (compare-and-swap) and the +10 counter of that token's block goes up. A wall with no token becomes a barrier when it is
// decided for good (the veil block's +14 goes up), and stays undecided when only looked at. The decision is kept per wall in
// the wall's global data (the decision cell at +158h); a pooled wall that starts a new life after RENEW_AFTER without a run
// clears it.
//
// ADR 0038: every function here runs once per wall spawn or per queued effect (per event), never per frame. Nothing here is
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
// byte each does not write. The cell ends at +15Bh: +15Ch is POINT's state (kPointStateOffset), a dword of its own.
inline constexpr std::uint32_t kCellOffset = kGlobalLenStock;
inline constexpr std::uint32_t kCellClockShift = 8;
inline constexpr std::uint32_t kDecisionMask = 0xFF;

// POINT's state: the dword at +15Ch, the last of the 8 bytes the patch adds (veil_fx.py:570-576, POINT_STATE = GLOBAL_LEN + 4).
// 0 not started, 1 gone off, anything else the charge-up's start stamp (PointStartStamp). It is cleared by the renew rule
// with the decision (veil_fx.py renew_stub, `mov dword [r8+15Ch], 0`), which this module does not yet install.
inline constexpr std::uint32_t kPointStateOffset = kGlobalLenStock + 4;
static_assert(kPointStateOffset + sizeof(std::uint32_t) == kGlobalLenWithDecision,
              "POINT's state is the last dword of the 8 bytes the patch adds to the global block");
inline constexpr std::uint32_t kPointNotStarted = 0;
inline constexpr std::uint32_t kPointGoneOff = 1;
// The start stamp is the interrupt time's low dword with this bit set, so a stamp is never 0 or 1 (the two states above).
// veil_fx.py uses `| 1`, and a charge-up that started at a time whose low dword was 0 or 1 then read as gone off.
inline constexpr std::uint32_t kPointStampBit = 2;

// The veil token block: the EC page's cell (ec_rules.h kVeilTok, 0x2104CC0). Layout as VeilNet writes it: +00 count (LONG),
// +08 expiry (u64, interrupt time in 100 ns), +10 walls that became the veil, +14 walls that stayed barriers.
inline constexpr std::uint32_t kVeilTokenRva = nevr_ec::kPageRva + nevr_ec::kVeilTok;
static_assert(kVeilTokenRva == 0x2104CC0, "the veil token block is the EC page's kVeilTok");
// POINT's token block: the same layout, with +10 counting the walls that went off as POINT (no +14 is written for it).
inline constexpr std::uint32_t kPointTokenRva = nevr_ec::kPageRva + nevr_ec::kPointTok;
static_assert(kPointTokenRva == 0x2104CE0, "the point token block is the EC page's kPointTok");
inline constexpr std::uint32_t kTokenCount = 0x00;
inline constexpr std::uint32_t kTokenExpiry = 0x08;
inline constexpr std::uint32_t kTokenShown = 0x10;     // +10: walls that became the veil, or went off as POINT
inline constexpr std::uint32_t kTokenBarriers = 0x14;  // +14: walls that stayed barriers (the veil block only)

// The FX queue of a POINT wall (veil_fx.py FX_QUEUE_RVA, FX_DEFER_RVA, :656-658): 6 x { node, word } and the defer flag that
// DroneFix.dll sets on a client. The drain that runs the queue is runtime/patch/ecfx_rules.h (nevr_ecfx::Drain), called by
// runtime/combat/dronefix.cpp. POINT's own use counter and its fired counter are in the EC page too (ec_rules.h kPointUse,
// veil_fx.py:561 POINT_FIRED_RVA).
inline constexpr std::uint32_t kFxQueueRva = nevr_ec::kPageRva + 0xDA0;  // 6 x 16 bytes, to 0x2104E00
inline constexpr std::uint32_t kFxDeferRva = nevr_ec::kPageRva + 0xE20;  // dword, set by DroneFix.dll on clients
inline constexpr std::uint32_t kPointFiredRva = nevr_ec::kPageRva + 0xD44;  // POINTs that went off on this machine

// The renew clock: 2^20 x 100 ns units (about 0.105 s), 24 bits, wrapping after about 20 days (veil_fx.py:800-801).
inline constexpr std::uint32_t kTimeShift = 20;
inline constexpr std::uint32_t kClockMask = 0xFFFFFF;
inline constexpr std::uint64_t kHundredNsPerSecond = 10'000'000;
inline constexpr std::uint32_t kRenewAfterUnits = static_cast<std::uint32_t>(8 * kHundredNsPerSecond >> kTimeShift);     // 8 s
inline constexpr std::uint32_t kRenewAfterPhysUnits = static_cast<std::uint32_t>(14 * kHundredNsPerSecond >> kTimeShift);  // 14 s

/// The wall's decision: the low byte of its cell. kPoint is veil_fx.py's eax 3 (its DECIDED_POINT is 'P'); this module's
/// encoding is its own, and nothing reads Cat's bytes. A byte that holds no other value reads as undecided.
enum class Decision : std::uint32_t { kUndecided = 0, kVeil = 1, kBarrier = 2, kPoint = 3 };

/// The decision the cell holds.
Decision CellDecision(std::uint32_t cell);
/// The cell with its decision byte set to `decision` and its clock kept.
std::uint32_t WithDecision(std::uint32_t cell, Decision decision);
/// The renew clock the cell holds (renew units, 24 bits).
std::uint32_t CellClock(std::uint32_t cell);

/// What the show node asks: the veil (1), a barrier (0), POINT (3), or no decision yet (2), as the stock contract decide()
/// gave. kPoint is only returned by DecideWithPoint, or for a cell a point build decided.
enum class Verdict { kBarrier, kVeil, kUndecided, kPoint };

/// Takes one token from a token block with the shared layout (the veil's block at kVeilTokenRva, POINT's at kPointTokenRva)
/// when one is held at `now`: the count is above 0 and `now` is before the expiry. A compare-and-swap on the count, then the
/// +10 counter goes up. Returns whether one was taken.
bool TakeToken(std::uint8_t* token, std::uint64_t now);

/// The show node's decision for one wall. `cell` is the wall's cell (see kCellOffset). A decided wall keeps its decision and
/// takes nothing. An undecided one takes a held veil token (kVeil, +10); failing that, when `decideForGood` it becomes a
/// barrier (kBarrier, the veil block's +14); when only looked at it stays undecided (kUndecided).
Verdict Decide(std::uint8_t* token, std::uint32_t& cell, std::uint64_t now, bool decideForGood);

/// A point build's decision (veil_fx.py decide_code with point): an undecided wall takes a held point token first (kPoint, +10
/// of POINT's block); otherwise it is Decide with the veil token. A decided wall keeps its decision and takes nothing.
Verdict DecideWithPoint(std::uint8_t* pointToken, std::uint8_t* veilToken, std::uint32_t& cell, std::uint64_t now,
                        bool decideForGood);

/// A pooled wall's renew check, at a spawn node's entry. The cell's clock is moved to `now` on every call. Returns true when
/// the wall starts a new life (no run for at least `afterUnits`).
bool NewLife(std::uint32_t& cell, std::uint64_t now, std::uint32_t afterUnits);

/// NewLife for a wall: a new life clears the decision (to undecided). An `aborting` node (being torn down) touches neither the
/// clock nor the decision. Returns whether the wall started a new life.
bool RenewOnEntry(std::uint32_t& cell, bool aborting, std::uint64_t now, std::uint32_t afterUnits);

/// The mute rule for a wall's own sound (veil_fx.py:614-622, mute_stub): a veil wall's and a point wall's own sounds are done at
/// once, silently; a barrier's and an undecided wall's pass through. The stub's `cmp al,'P'; je` and `cmp al,'S'; jne` bytes silence
/// both decisions, so this is Cat's rule. The plan row ("mute only for a point wall") does not match it. The stub is written only
/// in point builds.
bool MutesSound(Decision decision);

/// POINT's start stamp for a charge-up that starts at `now` (interrupt time): its low dword with kPointStampBit set.
std::uint32_t PointStartStamp(std::uint64_t now);
/// Whether POINT is charging up, given its state dword: not started (0) and gone off (1) are the two states that are not.
bool PointIsCharging(std::uint32_t state);
/// The units (100 ns) since POINT's charge-up started, as point_block computes them: unsigned, in 32 bits.
std::uint32_t PointElapsed(std::uint32_t state, std::uint64_t now);
/// Whether the charge-up is over: its elapsed time is at least `delay` (100 ns).
bool PointChargeDone(std::uint32_t state, std::uint64_t now, std::uint32_t delay);

/// The result of writing one queued effect (EnqueueWallFx).
enum class EnqueueResult { kQueued, kFull, kNoNode, kNoEntry, kMisaligned, kBadKind };

/// Queues one effect for the drain (nevr_ecfx::Drain): claims a free slot of the 6 { node, word } queue at `queue` (a slot
/// is free when its node is 0) and writes word = entry | kind. Refused, and nothing written, when ctx is 0 (the slot would
/// read as free), entry is 0, entry is not 16-aligned (the drain takes the kind from the low bits, so an unaligned entry
/// would run as another kind), or kind is not one of the four kinds. The caller checks the defer flag first.
EnqueueResult EnqueueWallFx(std::uint64_t* queue, std::uint64_t ctx, std::uint64_t entry, int kind);

/// The NAMED DEBT record of this module's registration: it installs nothing (every POINT site is a NOT DONE above), so the
/// point token is never taken and no wall goes off as POINT in this build. It logs one Warning that says so. The W-REG piece
/// calls it from combat.cpp's veil group.
void Register();

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
