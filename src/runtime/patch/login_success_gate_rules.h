#pragma once
// The decision behind patch/login_success_gate.cpp: which bits the hook must set around CR15NetGame::LogInSuccess.
// Pure, no Windows dependency, so the GTest drives the same function the hook calls.

#include <cstdint>

namespace nevr_login_success_gate {

// The one bit of the netgame flags qword that LogInSuccess tests (decompilation of its body, ReVault
// 0x14017f630). When it is clear, LogInSuccess skips writing social.community_values_version = 1 into the profile.
constexpr std::uint64_t kSocialCommunityBit = std::uint64_t{1} << 9;

// The bits the hook ORs in before the original runs: the social bit when it is clear, none when it is already set.
// The hook clears exactly these bits after the original returns, so a bit that was already set stays set and no
// other bit is ever written.
constexpr std::uint64_t BitsToForce(std::uint64_t flags) {
  return (flags & kSocialCommunityBit) != 0 ? 0 : kSocialCommunityBit;
}

}  // namespace nevr_login_success_gate
