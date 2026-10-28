#pragma once

// The decisions behind patch/arena_public_clock.cpp (#982): pure logic, no Windows or game headers, so it is
// unit-tested without a game image (src/runtime/tests/test_arena_public_clock.cpp).
//
// The Arena scripts stop the match clock after a goal in a PUBLIC match and keep it running in a private one.
// The script's updategametimer gate (lib08317e89cc3ae611.so 0x15c128, PC 08317e89cc3ae611.dll) lets the clock
// tick when the state is main_loop_update, or when the net flag 0x0494f0238b8ac3e6 is false and it is not the
// round's first launch. That flag is NOT(matchisprivate): "this match is public" (the store: node 0x2ff241f2,
// value expression 0x5814c). A public match therefore ticks only in main_loop_update, and the goal's SCORE state,
// the replay and the round-start countdown (34.6 s of 15 + 4.6 + 15 on the recorded public matches) stop the
// clock; a private match ticks through all of them. A server that stores the flag false for a public Arena
// session makes the gate pass, and the clock runs through goals exactly like a private match. matchisprivate
// itself is untouched, so the rules (round_time, the celebration time) stay the public ones.

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>

#include "runtime/lifecycle/arena_round_time.h"

namespace nevr_arena_public_clock {

// The net flag "match is public".
inline constexpr std::uint64_t kPublicMatchFlag = 0x0494f0238b8ac3e6ULL;

// The R15NETSETFLAG node state a script builds on its stack and hands to its bind function
// (rcx = state, rdx = node). The bind function registers the fields with the engine, which reads them while the
// node executes. Layout read from 08317e89cc3ae611.dll node 0x2ff241f2 (0x18001b3c0, the stores at rsp+0x20..0x48):
// +0x20 the flag id (u64), +0x28 the flag value (u8), +0x29 the "instanced" byte.
inline constexpr std::size_t kStateNameOffset = 0x20;
inline constexpr std::size_t kStateValueOffset = 0x28;
inline constexpr std::size_t kStateReadBytes = kStateValueOffset + 1;

struct FlagStoreDecision {
  bool forceFalse{false};
  std::string_view reason;  // "applied", "not_server", "no_session_type" or "non_arena"
};

// Decides one store of the public-match flag. Only a server with a stored public Arena session forces the flag
// false (the same gate as the 8:00 round default, arena_round_time.h); a client, a private Arena, every other game
// type and a store before any session is known keep the script's own value.
inline FlagStoreDecision DecideFlagStore(bool isServer, const std::optional<std::string>& sessionGameType) {
  if (!isServer) return {false, "not_server"};
  const auto decision = nevr_arena_round_time::DecideRoundTimeDefault(sessionGameType);
  return {decision.applied, decision.reason};
}

// Reads the flag id from a node state, or nullopt for a null state.
inline std::optional<std::uint64_t> StateFlagId(const std::uint8_t* state) {
  if (state == nullptr) return std::nullopt;
  std::uint64_t id = 0;
  std::memcpy(&id, state + kStateNameOffset, sizeof(id));
  return id;
}

// What one store did, for the log line.
struct FlagStoreOutcome {
  bool isPublicMatchFlag{false};  // false: a different flag, nothing was decided
  FlagStoreDecision decision;
  int valueBefore{0};
  int valueAfter{0};
};

// Applies the decision to a node state about to be bound: when the state stores the public-match flag and the
// decision forces it, the value byte becomes 0 (false). Any other flag is left alone, and so is a null state.
inline FlagStoreOutcome ApplyToFlagState(std::uint8_t* state, bool isServer,
                                         const std::optional<std::string>& sessionGameType) {
  FlagStoreOutcome outcome;
  const std::optional<std::uint64_t> id = StateFlagId(state);
  if (!id || *id != kPublicMatchFlag) return outcome;
  outcome.isPublicMatchFlag = true;
  outcome.decision = DecideFlagStore(isServer, sessionGameType);
  outcome.valueBefore = state[kStateValueOffset] != 0 ? 1 : 0;
  if (outcome.decision.forceFalse) state[kStateValueOffset] = 0;
  outcome.valueAfter = state[kStateValueOffset] != 0 ? 1 : 0;
  return outcome;
}

// ---- the script sites -----------------------------------------------------------------------------------------
// Two shipped scripts store the flag, each through the bind function every R15NETSETFLAG node of its DLL calls:
//   08317e89cc3ae611.dll (mp_game_server_state): node 0x2ff241f2 at RVA 0x1b3c0, bind function at RVA 0x1fa0.
//   fc4cbaae440e658c.dll (mpl_arena_rules_server): node 0x42ae9e11 at RVA 0x5900, bind function at RVA 0x1b20.
// The bind function is detoured, not the node: the node's own code is untouched and the hook only changes one
// byte of the state it is handed. A site is validated before it is hooked: the bind function's prologue, the
// `mov reg, imm64` that loads the flag id inside the node, and the node's `call rel32` to the bind function.

inline constexpr std::array<std::uint8_t, 16> kBindPrologue = {0x48, 0x89, 0x5c, 0x24, 0x08, 0x57, 0x48, 0x83,
                                                               0xec, 0x20, 0x48, 0x83, 0x7a, 0x18, 0x00, 0x48};

struct ScriptSite {
  const char* dll;                           // lowercase file name, as dll_load_hook matches it
  std::uint32_t bindRva;                     // the bind function
  std::uint32_t idRva;                       // the node's `mov reg, imm64` carrying the flag id
  std::array<std::uint8_t, 10> idInsn;       // REX.W B8+r, then the id little-endian
  std::uint32_t callRva;                     // the node's `call rel32` to the bind function
  std::array<std::uint8_t, 5> callInsn;
};

inline constexpr std::size_t kSiteCount = 2;
inline constexpr std::array<ScriptSite, kSiteCount> kSites = {{
    {"08317e89cc3ae611.dll", 0x1fa0, 0x1b3c9,
     {0x48, 0xb8, 0xe6, 0xc3, 0x8a, 0x8b, 0x23, 0xf0, 0x94, 0x04}, 0x1b488, {0xe8, 0x13, 0x6b, 0xfe, 0xff}},
    {"fc4cbaae440e658c.dll", 0x1b20, 0x5909,
     {0x48, 0xb9, 0xe6, 0xc3, 0x8a, 0x8b, 0x23, 0xf0, 0x94, 0x04}, 0x5966, {0xe8, 0xb5, 0xc1, 0xff, 0xff}},
}};

enum class SiteState {
  Unhooked,       // every byte is the shipped one: hook it
  Hooked,         // the bind function starts with a jmp (ours, from an earlier pass): leave it
  ImageTooSmall,  // the image does not reach the site
  IdMismatch,     // the node does not load the public-match flag id
  CallMismatch,   // the node does not call the bind function at bindRva
  BindMismatch,   // the bind function is neither the shipped prologue nor a jmp
};

inline const char* SiteStateName(SiteState state) {
  switch (state) {
    case SiteState::Unhooked: return "unhooked";
    case SiteState::Hooked: return "hooked";
    case SiteState::ImageTooSmall: return "image_too_small";
    case SiteState::IdMismatch: return "id_mismatch";
    case SiteState::CallMismatch: return "call_mismatch";
    case SiteState::BindMismatch: return "bind_mismatch";
  }
  return "unknown";
}

// Classifies the loaded image against one site. `image` is the module base, `imageSize` its SizeOfImage.
inline SiteState ClassifySite(const std::uint8_t* image, std::size_t imageSize, const ScriptSite& site) {
  const auto reaches = [&](std::uint32_t rva, std::size_t n) {
    return image != nullptr && static_cast<std::size_t>(rva) + n <= imageSize;
  };
  if (!reaches(site.bindRva, kBindPrologue.size()) || !reaches(site.idRva, site.idInsn.size()) ||
      !reaches(site.callRva, site.callInsn.size())) {
    return SiteState::ImageTooSmall;
  }
  if (std::memcmp(image + site.idRva, site.idInsn.data(), site.idInsn.size()) != 0) return SiteState::IdMismatch;
  if (std::memcmp(image + site.callRva, site.callInsn.data(), site.callInsn.size()) != 0) return SiteState::CallMismatch;
  if (std::memcmp(image + site.bindRva, kBindPrologue.data(), kBindPrologue.size()) == 0) return SiteState::Unhooked;
  if (image[site.bindRva] == 0xE9) return SiteState::Hooked;  // MinHook's jmp rel32
  return SiteState::BindMismatch;
}

}  // namespace nevr_arena_public_clock
