#pragma once

// The translocator's effect (#2242, H-LOC): the stun field's burst where a player lands after a translocator teleport.
// Ported from he_is_the_cat's fix_loc_fx.py (read as text at 82ad7ba). The script is the stock translocator script
// (9398a165c004a9c0), which the game runs unchanged; the effect is two hooks on that script's bytes, installed through the
// script hook table (runtime/hook/script_hooks.h), which checks each row's bytes before it installs anything.
//
// 1. Teleport (rva 0x7040, FUN_180007040, R15TELEPORTPLAYERNODE; four arguments, read in revault). On the first run of an
//    enabled teleport node (its state is 0 and its disabled byte at +0xDB is 0) the hook bumps the LOC use counter
//    (0x2104D40, which VeilNet's watcher sends to the other clients as a 'T' message) and spawns the stun field's burst
//    here with the detonator's spawn node (drone_script.h SpawnEffect). Then the original teleport runs with every argument
//    it was given.
// 2. Dispatch (rva 0x5910, execute_single_node, one argument). For every node the script runs, while a LOC token is held
//    (0x2104D20: count at +00, expiry at +08 in interrupt time, shown at +10), the hook takes one token, when the effect can
//    run (DetonatorFor), spawns the burst on that node's own puck, and restores the node's state, which the spawn clears.
//    A token is taken once; a refused effect takes none.
//
// The translocator's sound (LOC_EVENT, veil_sound.py) is not here: it is the sound piece's (PLAN-v7 row 13), so neither
// hook plays it yet. The mid-function bind rewire at 0x3725 (fix_loc_fx.py:52, :77, :103) is not ported: PLAN-v7 4.1 names
// no mechanism for a mid-function site and this port does not transplant node classes, so nothing needs binding.
//
// Owns FUN_180007040 (0x7040..0x7160) and execute_single_node (0x5910) of 9398a165c004a9c0. The token block and the use
// counter are the EC page's (runtime/patch/ec_rules.h, kLocTok and kLocUse), the same cells VeilNet's watcher reads.
//
// ADR 0038: the teleport hook runs once per teleport node start (per event). The dispatch hook runs once per node the
// translocator script executes (per node transition, a handful per throw, never per frame). Its work when no token is held
// is one interrupt-time read and one compare; the detonator guard and the spawn run only when a token is held. No
// measurement is taken here (no live run; see the commit message).
//
// Registration is a pure entry point (Register, called from combat.cpp by W-REG, not in this change). Nothing is logged per
// event; Register logs once with its consequence.

#include <cstddef>
#include <cstdint>

#include "runtime/combat/drone_script.h"
#include "runtime/patch/ec_rules.h"

namespace nevr_combat::loc_script {

inline constexpr std::uint64_t kScript = 0x9398a165c004a9c0ULL;  // the translocator script, 9398a165c004a9c0.dll
inline constexpr std::uint32_t kTeleportRva = 0x7040;            // FUN_180007040 (..0x7160)
inline constexpr std::uint32_t kDispatchRva = 0x5910;            // execute_single_node

// The stock bytes the hooks are written against (fix_loc_fx.py TELEPORT_OLD and DISPATCH_OLD): `mov r11,rsp; mov [r11+8],rbx;
// push rdi`, and `mov rax,[rcx+10h]; mov edx,0e039ee35h`. A script whose bytes differ is refused whole, with a Warning.
inline constexpr std::uint8_t kTeleportBytes[] = {0x4c, 0x8b, 0xdc, 0x49, 0x89, 0x5b, 0x08, 0x57};
inline constexpr std::uint8_t kDispatchBytes[] = {0x48, 0x8b, 0x41, 0x10, 0xba, 0x35, 0xee, 0x39, 0xe0};

// The teleport node's state and the byte that disables it (the same offsets the drone's stubs use, drone_script.h).
inline constexpr std::uint32_t kNodeState = drone_script::kCtxState;        // qword: 0 on the node's first run
inline constexpr std::uint32_t kNodeDisabled = drone_script::kCtxDisabled;  // byte: nonzero while the node aborts

// The EC page cells (ec_rules.h): the LOC token block and the LOC use counter, as echovr.exe RVAs.
inline constexpr std::uint32_t kTokenRva = nevr_ec::kPageRva + nevr_ec::kLocTok;     // 0x2104D20
inline constexpr std::uint32_t kCounterRva = nevr_ec::kPageRva + nevr_ec::kLocUse;   // 0x2104D40
static_assert(kTokenRva == 0x2104D20, "the LOC token block is the EC page's kLocTok");
static_assert(kCounterRva == 0x2104D40, "the LOC use counter is the EC page's kLocUse");

// The token block's fields (veilnet.cpp's Shared, which owns these cells: +00 count, +08 expiry, +10 shown).
inline constexpr std::uint32_t kTokenCount = 0x00;   // LONG: tokens held
inline constexpr std::uint32_t kTokenExpiry = 0x08;  // ULONGLONG: interrupt time (100 ns) after which no token is held
inline constexpr std::uint32_t kTokenShown = 0x10;   // LONG: effects shown here (+1 per token taken)

// The effect: pfx_wpn_combat_stun_field_pulse (symcache_data.cpp:26856), and fix_loc_fx.py's defaults for the config keys
// grenade_properties.loc.{effect_size, effect_lifetime, effect_time}, which are not read from config here.
inline constexpr std::uint64_t kLocFxAsset = 0x4CE12AE42C1B6269ULL;
inline constexpr float kLocFxScale = 2.0f;
inline constexpr float kLocFxLife = 2.5f;
inline constexpr float kLocFxDuration = 2.5f;  // seconds; the effect is killed after it
inline constexpr drone_script::SpawnSpec kLocSpawn{kLocFxAsset, kLocFxLife, kLocFxScale, kLocFxDuration};

/// Adds one to the LOC use counter: one teleport started on this machine (VeilNet's watcher sends it to the other clients).
void BumpTeleportCount(volatile std::int32_t* counter);

/// Whether the teleport node at `ctx` starts its effect: its state is 0 (the node's first run) and it is enabled.
bool TeleportEffectDue(const std::uint8_t* ctx);

/// Whether a LOC token is held at `token` at `now`: the count is above 0 and `now` is before the expiry.
bool TokenHeld(const std::uint8_t* token, std::uint64_t now);

/// Takes one LOC token from `token` (count down by one, shown up by one) when one is held at `now`. A compare-and-swap, so
/// two threads cannot take the same token. Returns whether one was taken.
bool TakeToken(std::uint8_t* token, std::uint64_t now);

/// Whether the dispatcher's effect runs for `node` now: a token is held, the effect can run (DetonatorFor), and the token
/// is taken. A token is taken only when the effect runs, so a refused effect takes none.
bool ClaimEffect(const std::uint8_t* node, std::uint8_t* token, std::uint64_t now);

/// Registers both hooks with the script hook table. Once. Called from combat.cpp by W-REG (not in this change).
void Register();

}  // namespace nevr_combat::loc_script
