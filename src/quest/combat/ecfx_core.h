/* The Quest's half of the NEUTRON effects deferral's rules (ecfx.h, docs/quest/combat/ec.md): where the queue and the gate sit on
 * re33_ec_page and how often the net tick drains it. The rules themselves (the queue's shape and kinds, Sounds::Again, Drain::Plan, the
 * exact-entry check as an Env input) are runtime/patch/ecfx_rules.h, the one definition PC's dronefix.cpp uses too (#2349); they are
 * named here so the Quest's code and tests keep their names. Built with -fno-exceptions, like every combat translation unit.
 */
#pragma once

#include <cstdint>

#include "runtime/patch/ecfx_rules.h"

namespace nevr_quest::combat::ecfx::core {

// page offsets (PC rva 0x2104000 base): veil_fx FX_QUEUE_RVA 0x2104DA0 (6 x { u64 ctx, u64 word }), FX_DEFER_RVA 0x2104E20
constexpr std::uint32_t kQueueOff = 0xda0, kGateOff = 0xe20;
constexpr std::uint64_t kEveryMs = 8;  // the net tick drains at most this often

using nevr_ecfx::Counts;
using nevr_ecfx::Ctx;
using nevr_ecfx::Drain;
using nevr_ecfx::Entries;
using nevr_ecfx::Env;
using nevr_ecfx::Free;
using nevr_ecfx::kBoom;
using nevr_ecfx::kBurst;
using nevr_ecfx::kCharge;
using nevr_ecfx::kSlots;
using nevr_ecfx::kSound;
using nevr_ecfx::kSoundMs;
using nevr_ecfx::kStaleMs;
using nevr_ecfx::Run;
using nevr_ecfx::Sounds;
using nevr_ecfx::Word;

}  // namespace nevr_quest::combat::ecfx::core
