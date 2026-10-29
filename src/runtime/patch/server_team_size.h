#pragma once

// #1422: a dedicated server's private-match team size (network.max_team_size, flat key nevr_max_team_size).
//
// The game's own team assignment, ServerAssignTeam (echovr.exe 0x1401AB730, by its "[NETGAME] \tServerAssignTeam:"
// lines), holds a team to min(Lobby::maxEntrants / 2, 5) outside the AI modes: 0x1401AB8B2 reads maxEntrants
// (0x1400d3420 on [this+0x40]+0x18 is `mov eax, [rcx+0x18]; ret`), and the 5 is an immediate at the cap site,
// 0x1401AB8C2, twice: `mov eax, 5; shr rsi, 1; cmp rsi, 5; cmova rsi, rax`. Every "Too many team members" test
// compares a team's count with that register. The AI modes take a cap of 3 on a branch before it and are not touched.
// A private match on the community service admits up to 16 a team, so this immediate is what holds a team to five.
//
// The team slot site, 0x1401ABC8A (the "Grabbing last teamslot" branch), stores the team's count as the member's
// 5-bit slot, so a sixth member would get slot 5. Above 5, heisthecat31's EchoRelay rule (d2d1a86) is written there
// first: a count of 5 or more takes slot count - 5. Its 18 bytes replace the game's 18; the register and flag
// effects after the site are the game's (the next instruction reads ax only, and ecx is reloaded before it is read).
//
// Both sites are checked patches (checked_patch.h), not a hook: the bound is an immediate in the middle of a
// 0x71B-byte function, and a hook would have to restate the whole function to change two bytes. What is pure:
// everything here; Apply writes only through the writer it is handed, so a test drives it on buffers. Above 5 the
// slot rule is written before the cap and taken out again if the cap write fails, so a server is never left with
// a cap above 5 and the game's slot rule. A site that holds neither the game's bytes nor ours is left as found and
// the team stays at 5 (another writer was there first; the image guard pins the build).

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#include "runtime/patch/checked_patch.h"

namespace nevr_server_team_size {

/// The game's own cap, and the sizes honoured.
constexpr uint32_t kGameTeamSize = 5;
constexpr uint32_t kMinTeamSize = 1;
constexpr uint32_t kMaxTeamSize = 8;

/// Parses network.max_team_size. Absent or empty is the game's 5. A value that is not a plain whole number, or is
/// outside [kMinTeamSize, kMaxTeamSize], is 5 with `problem` set; it is never clamped to a size nobody wrote.
inline uint32_t ParseMaxTeamSize(const char* text, std::string* problem) {
  if (problem != nullptr) problem->clear();
  if (text == nullptr || text[0] == '\0') return kGameTeamSize;
  uint32_t value = 0;
  for (const char* c = text; *c != '\0'; ++c) {
    if (*c < '0' || *c > '9') {
      if (problem != nullptr) *problem = "not a whole number";
      return kGameTeamSize;
    }
    value = value * 10 + static_cast<uint32_t>(*c - '0');
    if (value > kMaxTeamSize) {
      if (problem != nullptr) *problem = "above the limit of " + std::to_string(kMaxTeamSize);
      return kGameTeamSize;
    }
  }
  if (value < kMinTeamSize) {
    if (problem != nullptr) *problem = "below the limit of " + std::to_string(kMinTeamSize);
    return kGameTeamSize;
  }
  return value;
}

/// SERVER_ASSIGN_TEAM_CAP (runtime/hook/addresses.h): `mov eax, 5; shr rsi, 1; cmp rsi, 5`. The size is the
/// imm32 at kCapImmediate32 and the imm8 at kCapImmediate8.
inline constexpr uint8_t kCapExpected[12] = {0xb8, 0x05, 0x00, 0x00, 0x00, 0x48, 0xd1, 0xee, 0x48, 0x83, 0xfe, 0x05};
constexpr size_t kCapImmediate32 = 1;
constexpr size_t kCapImmediate8 = 11;

/// SERVER_ASSIGN_TEAM_SLOT: the game's `shl ax, 5; mov ecx, 0x3e0; xor ax, si; and ax, cx; xor ax, si`, and the
/// rule written above 5: `cmp al, 5; jb +2; sub al, 5; shl eax, 5; xor eax, esi; and ax, 0x3e0; xor eax, esi; nop`.
inline constexpr uint8_t kSlotExpected[18] = {0x66, 0xc1, 0xe0, 0x05, 0xb9, 0xe0, 0x03, 0x00, 0x00,
                                              0x66, 0x33, 0xc6, 0x66, 0x23, 0xc1, 0x66, 0x33, 0xc6};
inline constexpr uint8_t kSlotPatched[18] = {0x3c, 0x05, 0x72, 0x02, 0x2c, 0x05, 0xc1, 0xe0, 0x05,
                                             0x31, 0xf0, 0x66, 0x25, 0xe0, 0x03, 0x31, 0xf0, 0x90};

/// The cap site's bytes for `size`: the game's with both immediates set to it.
struct CapBytes {
  uint8_t bytes[sizeof(kCapExpected)];
};
inline CapBytes CapBytesFor(uint32_t size) {
  CapBytes out{};
  std::memcpy(out.bytes, kCapExpected, sizeof(kCapExpected));
  out.bytes[kCapImmediate32] = static_cast<uint8_t>(size);
  out.bytes[kCapImmediate8] = static_cast<uint8_t>(size);
  return out;
}

inline nevr_checked_patch::Site SlotSite() {
  return nevr_checked_patch::Site{kSlotExpected, kSlotPatched, sizeof(kSlotExpected)};
}

enum class Plan {
  KeepGame,     // 5, or a size outside the range: nothing is written
  CapOnly,      // 1-4: the cap site only; no member reaches slot 5
  SlotThenCap,  // 6-8: the slot rule, then the cap
};

inline Plan PlanFor(uint32_t size) {
  if (size < kMinTeamSize || size > kMaxTeamSize || size == kGameTeamSize) return Plan::KeepGame;
  return size < kGameTeamSize ? Plan::CapOnly : Plan::SlotThenCap;
}

/// What Apply did. `*Before` are the bytes found at a site before anything was written there.
struct Outcome {
  uint32_t size = kGameTeamSize;
  Plan plan = Plan::KeepGame;
  bool capTried = false;  // a site not tried is "not_tried" in the record, whatever its result field holds
  nevr_checked_patch::Result cap = nevr_checked_patch::Result::Mismatch;
  uint8_t capBefore[sizeof(kCapExpected)] = {};
  bool slotTried = false;
  nevr_checked_patch::Result slot = nevr_checked_patch::Result::Mismatch;
  uint8_t slotBefore[sizeof(kSlotExpected)] = {};
  bool slotRestored = false;  // the slot rule was taken out again after the cap failed
  uint32_t inEffect = kGameTeamSize;
};

inline bool Took(nevr_checked_patch::Result r) {
  return r == nevr_checked_patch::Result::Applied || r == nevr_checked_patch::Result::AlreadyApplied;
}

/// Puts `size` in effect at the two sites, through `write`. See the file comment for the order and the rules.
inline Outcome Apply(uint8_t* capSite, uint8_t* slotSite, uint32_t size, nevr_checked_patch::WriteFn write) {
  using nevr_checked_patch::Result;
  Outcome o;
  o.size = size;
  o.plan = PlanFor(size);
  std::memcpy(o.capBefore, capSite, sizeof(o.capBefore));
  std::memcpy(o.slotBefore, slotSite, sizeof(o.slotBefore));
  if (o.plan == Plan::KeepGame) return o;

  const CapBytes capWanted = CapBytesFor(size);
  const nevr_checked_patch::Site capSpec{kCapExpected, capWanted.bytes, sizeof(kCapExpected)};

  if (o.plan == Plan::SlotThenCap) {
    // The cap is checked before the slot rule goes in, so a cap that cannot follow leaves both sites as found.
    const bool capReady = std::memcmp(capSite, kCapExpected, sizeof(kCapExpected)) == 0 ||
                          std::memcmp(capSite, capWanted.bytes, sizeof(kCapExpected)) == 0;
    if (!capReady) {
      o.capTried = true;  // `cap` keeps its Mismatch
      return o;
    }
    o.slotTried = true;
    o.slot = nevr_checked_patch::Apply(slotSite, SlotSite(), write);
    if (!Took(o.slot)) return o;
  }

  o.capTried = true;
  o.cap = nevr_checked_patch::Apply(capSite, capSpec, write);
  if (Took(o.cap)) {
    o.inEffect = size;
    return o;
  }
  if (o.slotTried && o.slot == Result::Applied) {
    o.slotRestored = write(slotSite, kSlotExpected, sizeof(kSlotExpected)) &&
                     std::memcmp(slotSite, kSlotExpected, sizeof(kSlotExpected)) == 0;
  }
  return o;
}

/// A size was asked for and is not in effect.
inline bool IsFailure(const Outcome& o) { return o.plan != Plan::KeepGame && o.inEffect != o.size; }

inline const char* SiteResultName(bool tried, nevr_checked_patch::Result r) {
  return tried ? nevr_checked_patch::ResultName(r) : "not_tried";
}

inline void Hex(char* out, const uint8_t* bytes, size_t n) {
  for (size_t i = 0; i < n; ++i) std::snprintf(out + 2 * i, 3, "%02x", bytes[i]);
  out[2 * n] = '\0';
}

/// The one record for the outcome: the size asked for and the size in effect, then each site's address, the bytes
/// the game holds there, the bytes found and its result (logging Rule 6). Written to `out`, NUL-terminated.
inline int FormatRecord(char* out, size_t cap, const Outcome& o, uintptr_t capAddr, uintptr_t slotAddr) {
  char capExpected[2 * sizeof(kCapExpected) + 1];
  char capActual[2 * sizeof(kCapExpected) + 1];
  char slotExpected[2 * sizeof(kSlotExpected) + 1];
  char slotActual[2 * sizeof(kSlotExpected) + 1];
  Hex(capExpected, kCapExpected, sizeof(kCapExpected));
  Hex(capActual, o.capBefore, sizeof(kCapExpected));
  Hex(slotExpected, kSlotExpected, sizeof(kSlotExpected));
  Hex(slotActual, o.slotBefore, sizeof(kSlotExpected));
  const char* what = o.plan == Plan::KeepGame ? "the game's own cap, nothing written"
                     : !IsFailure(o)          ? "a private-match team holds up to the size"
                     : o.slotRestored         ? "not in effect, the slot rule was put back and a team stays at 5"
                                              : "not in effect, a team stays at 5";
  return std::snprintf(out, cap,
                       "[NEVR.PATCH] server team size size=%u in_effect=%u (%s) cap_addr=0x%llx cap_expected=%s "
                       "cap_actual=%s cap=%s slot_addr=0x%llx slot_expected=%s slot_actual=%s slot=%s%s",
                       static_cast<unsigned>(o.size), static_cast<unsigned>(o.inEffect), what,
                       static_cast<unsigned long long>(capAddr), capExpected, capActual,
                       SiteResultName(o.capTried, o.cap), static_cast<unsigned long long>(slotAddr), slotExpected,
                       slotActual, SiteResultName(o.slotTried, o.slot), o.slotRestored ? " slot_restored=true" : "");
}

}  // namespace nevr_server_team_size
