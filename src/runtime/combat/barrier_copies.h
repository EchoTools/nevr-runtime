#pragma once

// NAMED DEBT (PLAN-v7 section 16): reconstruct through nevr-giveyouup after the release.
// KNOWN-WRONG #2760: the named debt above is open and tracked by #2760.
//
// The two barrier copies behind EchoCombat's UMBRAL VEIL (mp_combat_ability_smoke, 94ba7ca09dcf7b0b, from the stock
// barrier b87ba2039b049d5e; his fix_smoke.py:22, :374-377) and NEUTRON (mp_combat_ability_point, 94ba7ca39fc97e1a,
// fix_point.py:9, :130-137). The stock barrier, the energy barrier, is never touched here.
//
// Origin: he_is_the_cat's tools/veil_fx.py at 82ad7ba, read as text. It writes three value sites into each copy:
//   1. the param node's symbol, `mov qword [rbp-29h], -1` at 0x63FD (veil_fx.py:450-452): the imm32 becomes the copy's
//      marker, "SMOK" 0x534D4F4B for UMBRAL VEIL (:450) and "POIN" 0x4E494F50 for NEUTRON (:519, fix_point.py:134).
//      The node (FUN_1800063D0, 0x63D0..0x6520) sends the marker as the wall's symbol param, which Cat's tool notes is
//      harmless (veil_fx.py:477-483).
//   2 and 3. the copy's two ShowModel nodes of the priming hologram, `mov byte [rsp+48h], 1` at 0x6668 (FUN_1800065F0,
//      0x65F0..0x66F0) and 0x74F8 (FUN_180007480, 0x7480..0x7580), set to 0 so no hologram shows while the tac mod is
//      primed (veil_fx.py:105-109, :484-486).
//
// Smells in Cat's version, and what this one does instead: his writes are byte literals and the marker is a bare
// number inside the patch text (veil_fx.py:450-452); here each site's expected and replacement bytes are named constants
// with their source line, and the patched bytes are built from the marker by a constexpr function, so the pair cannot be
// assembled by hand wrong. The patch is applied only through the script hook table (the owner rule, the check before any
// write, Rule 6 on a refusal), never by a write at a fixed address at boot.
//
// NOT ported (PLAN-v7 section 16, row "b87ba203 copies 0x64F1"): the call at 0x64F1 that replaces the param node's Execute
// (`call 0x22A0`, veil_fx.py:468, checked at :477) with the use-counter stub (veil_fx.py:471-483), which bumps the record
// VeilNet watches (0x2104D00 + 0x200 for the smoke copy, + 0x280 for NEUTRON, veil_fx.py:521). The stub has to run from
// executable memory outside the copy's image (a new section, veil_fx.py:471-483 add_section): a code cave, and the
// checked-patch route carries no pattern for one. Until that is in, VeilNet counts no use of either copy through this
// path, and the copies' use-counter behaviour is not reproduced.
//
// Hot path (ADR 0038 rule 3): each site is an immediate or a flag byte inside a node that runs per node execution. A
// checked patch adds no instruction and no call, so it adds nothing to any call the game makes; the added time is 0 by
// construction, and no call-rate measurement was taken for it (the run of this change is unit tests only).
//
// Registration: Register() adds the six value rows (two copies, three sites each) to the script hook table at boot; the
// table applies a copy's rows when that copy loads, after its setup_bindings and only when every row's bytes are the
// ones above (nevr_script_hooks::OnLoaded). combat.cpp calls Register once, in the veil group (W-REG owns that call).
// Nothing is put back when a copy is freed: the copy's image goes with it.

#include <array>
#include <cstddef>
#include <cstdint>

namespace nevr_combat::barrier_copies {

/// The two copies (the device's copies of the stock barrier, made by R12; their hashes are the copies' names).
inline constexpr std::uint64_t kSmokeScript = 0x94ba7ca09dcf7b0bULL;  // mp_combat_ability_smoke: UMBRAL VEIL
inline constexpr std::uint64_t kPointScript = 0x94ba7ca39fc97e1aULL;  // mp_combat_ability_point: NEUTRON

/// The three sites, from the DLL's base (veil_fx.py:450-452, :108).
inline constexpr std::uint32_t kParamSymbolRva = 0x63FD;
inline constexpr std::uint32_t kPrimingShowRvaA = 0x6668;  // ShowModel node execute_node_b8a054bc (FUN_1800065F0)
inline constexpr std::uint32_t kPrimingShowRvaB = 0x74F8;  // ShowModel node execute_node_367b0674 (FUN_180007480)

/// The markers (veil_fx.py:450 "SMOK", :519 "POIN"), as the little-endian imm32 the param node sends.
inline constexpr std::uint32_t kSmokeMarker = 0x534D4F4BU;
inline constexpr std::uint32_t kPointMarker = 0x4E494F50U;

/// `mov qword [rbp-29h], imm32` (48 c7 45 d7 + imm32); the stock imm32 is -1 (veil_fx.py:452).
inline constexpr std::uint8_t kParamSymbolOpcode[4] = {0x48, 0xc7, 0x45, 0xd7};
inline constexpr std::uint8_t kParamSymbolStock[8] = {0x48, 0xc7, 0x45, 0xd7, 0xff, 0xff, 0xff, 0xff};

/// `mov byte ptr [rsp+48h], 1` (show) and `... 0` (hide), veil_fx.py:109.
inline constexpr std::uint8_t kShowOn[5] = {0xc6, 0x44, 0x24, 0x48, 0x01};
inline constexpr std::uint8_t kShowOff[5] = {0xc6, 0x44, 0x24, 0x48, 0x00};

/// The param symbol's length, its opcode's length, the marker's length and a byte's bits and mask.
inline constexpr std::size_t kParamSymbolLen = 8;
inline constexpr std::size_t kParamOpcodeLen = 4;
inline constexpr std::size_t kMarkerLen = 4;
inline constexpr unsigned kByteBits = 8;
inline constexpr std::uint32_t kByteMask = 0xFFU;

/// The param symbol's bytes with `marker` as its imm32, little-endian.
constexpr std::array<std::uint8_t, kParamSymbolLen> ParamSymbolWith(std::uint32_t marker) {
  std::array<std::uint8_t, kParamSymbolLen> out{};
  for (std::size_t i = 0; i < kParamOpcodeLen; ++i) out[i] = kParamSymbolOpcode[i];
  for (std::size_t i = 0; i < kMarkerLen; ++i) {
    out[kParamOpcodeLen + i] = static_cast<std::uint8_t>((marker >> (kByteBits * i)) & kByteMask);
  }
  return out;
}

inline constexpr std::array<std::uint8_t, kParamSymbolLen> kSmokeParamSymbol = ParamSymbolWith(kSmokeMarker);
inline constexpr std::array<std::uint8_t, kParamSymbolLen> kPointParamSymbol = ParamSymbolWith(kPointMarker);

/// The six rows' count (two copies x three sites).
inline constexpr std::size_t kRowCount = 6;

/// Adds the six value rows to the script hook table. Once per process: a second call does nothing.
void Register();

#ifdef NEVR_TEST_HOOKS
void ResetForTest();
#endif

}  // namespace nevr_combat::barrier_copies
