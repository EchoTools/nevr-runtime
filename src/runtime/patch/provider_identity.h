#pragma once

// pnsrad's exported UserProviderID reports the "OVR" provider, so the game's provider checks agree
// with the OVR-ORG identity the runtime forces everywhere else.
//
// Measured 2026-10-01 (scenario run 20261001T1433, [NEVR.PARTY] invite handler trace): the friend
// row's invite handler (echovr.exe 0x14018aa90) compares the target's provider nibble with
// CSymbol64(CNSProvider::UserProviderID(local user)) and returns SILENTLY on a mismatch. pnsrad's
// UserProviderID (pnsrad.dll rva 0x88d70: mov rax,[rip+0x2ed819]; ret) returns the "RAD" symbol
// 0xc8e8d0b1a882e3ee. CSymbol64 (echovr.exe 0x1400f6b40) maps only STM 1, PSN 2, XBX 3, OVR 4, DMO 7,
// BOT 6, GST 0x10, so RAD maps to 0. Since f84f456 every id string the game builds is "OVR-ORG-<id>",
// which SNSUserID (0x1400f6c10) maps to 4. 0 != 4: every invite click was dropped with no event.
// Returning the OVR symbol makes the local side 4 as well. Only callers that go through the export
// see it (echovr.exe's 19 UserProviderID callers); pnsrad's own code reads its global directly.

#include <array>
#include <cstdint>

namespace ProviderIdentity {

/// pnsrad.dll rva of the exported UserProviderID.
constexpr std::uintptr_t kPnsradUserProviderIdRva = 0x88D70;

/// The function and the padding after it, as shipped (pnsrad.dll 2410095d3b43d408): 8 bytes of code,
/// 8 bytes of int3 padding. All 16 are validated before the write.
constexpr std::array<std::uint8_t, 16> kPnsradUserProviderIdExpected = {
    0x48, 0x8B, 0x05, 0x19, 0xD8, 0x2E, 0x00, 0xC3, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC};

/// CSymbol64 of "OVR". CSymbol64 (echovr.exe 0x1400f6b40) maps this symbol to provider code 4, and the
/// symbol sits in the provider table at .rdata 0x1416d0f00. The same code 4 is rendered "OVR-ORG" by
/// GetProviderPrefix (0x14060d640); the string "OVR" belongs to code 5, which CSymbol64 has no symbol for.
constexpr std::uint64_t kOvrProviderSymbol = 0xc8e8d0b1a89ff4f8ULL;

/// mov rax, imm64 ; ret
inline std::array<std::uint8_t, 11> ReturnConstant(std::uint64_t value) {
  std::array<std::uint8_t, 11> code{0x48, 0xB8};
  for (int i = 0; i < 8; ++i) code[2 + i] = static_cast<std::uint8_t>((value >> (8 * i)) & 0xFF);
  code[10] = 0xC3;
  return code;
}

}  // namespace ProviderIdentity
