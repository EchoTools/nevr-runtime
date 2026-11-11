/* The pinned slots, functions and code word of POWER BOOST (powerboost.h).
 *
 * Every address is an ELF virtual address in the store libr15 (build ID in pinned_targets.h, `LibR15PatchedBuildId`); the disassembly's
 * address is the ELF address plus 0x100000. src/quest/tests/got_pinned_test.cpp checks every relocation, every symbol's address and the
 * code word against the real file (`just test-quest-hooks-pinned`, which needs the store APK). `readelf -rW` of the store libr15:
 *
 *   R51  CR15NetDamageableHealNode::Enter            JUMP_SLOT 0x36e3868 -> 0x23207b0  (one caller: BindNode<..HealNode>, 0x23c2c6c)
 *   R52  CR15NetDamageableCS::Heal                   JUMP_SLOT 0x36f7820 -> 0x2137e74  (two callers: ClientDamageDealtCB 0x2137a44 and
 *                                                    HealNode::Enter 0x23208b0)
 *        the same function's GLOB_DAT               0x3725b80 -> 0x2137e74  (one reader: HealNode::Enter's queued heal, `ldr x8,[x8,#0xb80]` at 0x2320904)
 *   R53  CR15NetHealBubbleCS::SetTargetInsideBubble  JUMP_SLOT 0x36d7d08 -> 0x2158954  (one caller: UpdateActiveHealBubble 0x2157b98)
 *   R54  CR15NetDamageableCS::DamageFromClient       JUMP_SLOT 0x36e5778 -> 0x213e80c  (called, not hooked: the slot is only read)
 *   R55  CPhTarget::ResolveBody                      JUMP_SLOT 0x36ca098 -> 0x19a27ac  (771 `bl` to its PLT stub 0xee5360, no `b`)
 *        CPhysicsCS::PrimaryBody                     JUMP_SLOT 0x36f6110 -> 0x1677d34  (called, not hooked)
 *   R56  UpdateActiveHealBubble's per-target `bl 0xee5360` at 0x2157998 (word 0x97b63672), which returns to 0x215799c
 *
 * Origin (docs/audits/2026-11-05-q15t-origin-citations.md rows 104, 245, 246): the reference build's runtime/native/afkxr/afk_powerboost.cpp:80
 * (kGotHealData) and runtime/native/afkxr/afk_flowdiag.cpp:91,93 (the damageable system's key and vptr), at 1357409.
 *
 * Built with -fno-exceptions, like every translation unit that includes callback_thunk.h.
 */
#pragma once

#include <cstddef>
#include <cstdint>

#include "quest/sentinel/callback_thunk.h"
#include "quest/sentinel/got_hook.h"
#include "quest/sentinel/pinned_targets.h"

namespace nevr_quest::combat::powerboost {

inline constexpr const char* kNodeSymbol =
    "_ZN10NRadEngine8NRadGame25CR15NetDamageableHealNode5EnterERKNS0_29SR15NetDamageableHealNodeDataE";
inline constexpr const char* kHealSymbol = "_ZN10NRadEngine8NRadGame19CR15NetDamageableCS4HealENS_18SCompactPoolHandleEfNS0_13SR15NetUserIdE";
inline constexpr const char* kLinkSymbol =
    "_ZN10NRadEngine8NRadGame19CR15NetHealBubbleCS21SetTargetInsideBubbleENS_18SCompactPoolHandleENS_16CComponentHandleINS0_25CR15NetHealBubbleTargetCSEEERKNS_8C3VectorEj";
inline constexpr const char* kSendSymbol =
    "_ZN10NRadEngine8NRadGame19CR15NetDamageableCS16DamageFromClientENS_18SCompactPoolHandleEfNS0_24ER15NetDamageableHitBodyENS_9CSymbol64E";
inline constexpr const char* kResolveSymbol = "_ZNK10NRadEngine9CPhTarget11ResolveBodyEv";
inline constexpr const char* kPrimarySymbol = "_ZNK10NRadEngine10CPhysicsCS11PrimaryBodyENS_18SCompactPoolHandleE";

// The slots, the functions and the cone's call site (ELF vaddrs).
struct Pins {
  std::uint64_t nodeSlot, nodeFn;
  std::uint64_t healSlot, healGlobDat, healFn;
  std::uint64_t linkSlot, linkFn;
  std::uint64_t sendSlot, sendFn;
  std::uint64_t resolveSlot, resolveFn;
  std::uint64_t primarySlot, primaryFn;
  std::uint64_t coneCall, coneRet;  // the `bl` and the instruction after it (the return address the entry acts on)
  std::uint64_t damageableVptr;     // CR15NetDamageableCS's vptr value (a live damageable system has it)
};
inline constexpr Pins kPins = {0x36e3868ULL, 0x23207b0ULL, 0x36f7820ULL, 0x3725b80ULL, 0x2137e74ULL, 0x36d7d08ULL, 0x2158954ULL,
                               0x36e5778ULL, 0x213e80cULL, 0x36ca098ULL, 0x19a27acULL, 0x36f6110ULL, 0x1677d34ULL, 0x2157998ULL,
                               0x215799cULL, 0x3693ea0ULL};

// `bl 0xee5360` (ResolveBody's PLT stub) at 0x2157998: imm26 = (0xee5360 - 0x2157998) / 4.
inline constexpr std::uint32_t kConeCallWord = 0x97b63672u;
// The damageable component system's key in a gamespace's system table (afk_flowdiag.cpp:91).
inline constexpr std::uint64_t kDamageableKey = 0xc9dc08cfc2f5920fULL;

// The hooked functions' signatures, as the game calls them (arm64: x0 this, x1 a compact pool handle, s0 the float, x2 the next word).
using NodeSig = void(void* node, const std::uint8_t* data);
using HealSig = void(void* cs, std::uint64_t handle, float amount, std::uint64_t user);
using LinkSig = void(void* cs, std::uint64_t bubble, const void* target, const void* pos, std::uint64_t arg);
struct NodeTag {};
struct HealTag {};
struct QueuedHealTag {};
struct LinkTag {};
// One thunk per hooked word (callback_thunk.h): Heal's JUMP_SLOT and its GLOB_DAT word are two slots, so two thunks.
using NodeThunk = sentinel::CallbackThunk<NodeTag, NodeSig>;
using HealThunk = sentinel::CallbackThunk<HealTag, HealSig>;
using QueuedHealThunk = sentinel::CallbackThunk<QueuedHealTag, HealSig>;
using LinkThunk = sentinel::CallbackThunk<LinkTag, LinkSig>;

// A slot as a manager target on a libr15 loaded at `base`: the slot must hold the real function (base + fn).
inline sentinel::GotTarget Target(const char* symbol, sentinel::RelocKind kind, std::uint64_t slot, std::uintptr_t base, std::uint64_t fn) {
  sentinel::GotTarget target(sentinel::pinned::kLibR15, symbol, kind, sentinel::pinned::LibR15PatchedBuildId(), slot);
  target.expectedOriginal = reinterpret_cast<const void*>(base + fn);
  return target.Owned("combat_powerboost");
}
inline sentinel::GotTarget NodeTarget(std::uintptr_t base, const Pins& p) {
  return Target(kNodeSymbol, sentinel::RelocKind::kJumpSlot, p.nodeSlot, base, p.nodeFn);
}
inline sentinel::GotTarget HealTarget(std::uintptr_t base, const Pins& p) {
  return Target(kHealSymbol, sentinel::RelocKind::kJumpSlot, p.healSlot, base, p.healFn);
}
inline sentinel::GotTarget QueuedHealTarget(std::uintptr_t base, const Pins& p) {
  return Target(kHealSymbol, sentinel::RelocKind::kGlobDat, p.healGlobDat, base, p.healFn);
}
inline sentinel::GotTarget LinkTarget(std::uintptr_t base, const Pins& p) {
  return Target(kLinkSymbol, sentinel::RelocKind::kJumpSlot, p.linkSlot, base, p.linkFn);
}
inline sentinel::GotTarget ResolveTarget(std::uintptr_t base, const Pins& p) {
  return Target(kResolveSymbol, sentinel::RelocKind::kJumpSlot, p.resolveSlot, base, p.resolveFn);
}

}  // namespace nevr_quest::combat::powerboost
