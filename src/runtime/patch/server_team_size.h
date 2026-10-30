#pragma once

// #1422: a dedicated server's match team size (network.max_team_size, flat key nevr_max_team_size).
//
// The game's own team assignment, ServerAssignTeam (echovr.exe 0x1401AB730, by its "[NETGAME] \tServerAssignTeam:"
// lines), holds a team to min(Lobby::maxEntrants / 2, 5) in every mode but the AI and social modes: 0x1401AB8B2
// reads maxEntrants (0x1400d3420 on [this+0x40]+0x18 is `mov eax, [rcx+0x18]; ret`), and the 5 is an immediate at
// the cap site, 0x1401AB8C2, twice: `mov eax, 5; shr rsi, 1; cmp rsi, 5; cmova rsi, rax`. Every "Too many team
// members" test compares a team's count with that register. The AI modes take a cap of 3 on a branch before it, and
// the social modes skip the cap block; neither is touched. A lobby's own limit still bounds a team: a public match
// whose lobby holds 8 keeps teams of 4 at any size here, and a size of 1 to 4 lowers every lobby's teams.
//
// The team slot site, 0x1401ABC8A (the "Grabbing last teamslot" branch, taken when the team's free-slot heap is
// empty), stores the team's count as the member's 5-bit slot, then copies the word to r9d and eax. A sixth member of
// a match team would get slot 5. Every team reaches this site, the social lobby's team (3) and the spectators
// included, so the rule written there above 5 (from the reading here, after heisthecat31's EchoRelay found the site)
// applies to the two match teams only: team 0 or 1 with a count of 5 or more takes slot count - 5; every other team
// keeps slot = count. Its 26 bytes replace the game's 26 (the site and the two `movzx` copies after it) and leave
// r9d and eax as those copies do; no jump lands inside the range, ecx is reloaded and no flag is read before the
// next write. A count of 10 or more on a match team (the join check 0x140616920 can admit one past the cap) takes
// slot count - 5, which is 5 or more, as the game's own rule would give it 10 or more.
//
// Both sites are checked patches (checked_patch.h), not a hook: the bound is an immediate in the middle of a
// 0x71B-byte function, and MinHook replaces whole functions, so a hook would restate all of it to change two
// values. What is pure: everything here; Apply writes only through the writer it is handed, so a test drives it on
// buffers, and the slot rule's bytes run natively in the test. Above 5 the slot rule is written before the cap. Apply
// decides what to take out by the bytes at the two sites, not by what the writer said: a writer can report a failure
// after the bytes went in (ProcessMemcpy does when the protection restore fails). A slot site Apply changed is put
// back only when the cap site's bytes put a size of 5 or less in effect, whoever wrote them, and kept when they put a
// size above 5 in effect or are no cap this file can read (a cap write that landed in part may still raise the cap),
// so a server is never left with a cap above 5 and the game's slot rule. The size in effect is read back from the cap
// site's bytes, and above 5 it is a success only with the slot rule in; a cap site that holds neither the game's shape
// nor ours is left as found and reported as unknown.

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

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

/// SERVER_ASSIGN_TEAM_SLOT: the game's `shl ax, 5; mov ecx, 0x3e0; xor ax, si; and ax, cx; xor ax, si;
/// movzx r9d, ax; movzx eax, r9w` (ax is the team's count, si the member's word, bl the team), and the rule written
/// above 5: `cmp bl, 1; ja +6; cmp al, 5; jb +2; sub al, 5; shl eax, 5; xor eax, esi; and eax, 0x3e0;
/// xor eax, esi; mov r9d, eax`.
inline constexpr uint8_t kSlotExpected[26] = {0x66, 0xc1, 0xe0, 0x05, 0xb9, 0xe0, 0x03, 0x00, 0x00,
                                              0x66, 0x33, 0xc6, 0x66, 0x23, 0xc1, 0x66, 0x33, 0xc6,
                                              0x44, 0x0f, 0xb7, 0xc8, 0x41, 0x0f, 0xb7, 0xc1};
inline constexpr uint8_t kSlotPatched[26] = {0x80, 0xfb, 0x01, 0x77, 0x06, 0x3c, 0x05, 0x72, 0x02,
                                             0x2c, 0x05, 0xc1, 0xe0, 0x05, 0x31, 0xf0, 0x25, 0xe0,
                                             0x03, 0x00, 0x00, 0x31, 0xf0, 0x41, 0x89, 0xc1};

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

/// The size the cap site's bytes put in effect: the game's shape with both immediates equal, or 0 (unknown) for any
/// other bytes.
inline uint32_t CapInEffect(const uint8_t* cap) {
  CapBytes shape = CapBytesFor(cap[kCapImmediate8]);
  return std::memcmp(cap, shape.bytes, sizeof(kCapExpected)) == 0 ? cap[kCapImmediate8] : 0;
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
  bool slotRestored = false;  // the slot site was put back as found, and read back so, after the cap did not go in
  uint32_t inEffect = kGameTeamSize;  // read from the cap site's bytes when Apply returns; 0 is unknown
  bool slotRule = false;  // the slot site holds the rule written above 5 when Apply returns
  bool slotGame = false;  // the slot site holds the game's own bytes when Apply returns
};

inline bool Took(nevr_checked_patch::Result r) {
  return r == nevr_checked_patch::Result::Applied || r == nevr_checked_patch::Result::AlreadyApplied;
}

/// Puts `size` in effect at the two sites, through `write`. See the file comment for the order and the rules.
inline Outcome Apply(uint8_t* capSite, uint8_t* slotSite, uint32_t size, nevr_checked_patch::WriteFn write) {
  Outcome o;
  o.size = size;
  o.plan = PlanFor(size);
  std::memcpy(o.capBefore, capSite, sizeof(o.capBefore));
  std::memcpy(o.slotBefore, slotSite, sizeof(o.slotBefore));
  const auto done = [&]() {
    // By the bytes, not the writer's word (file comment): a slot site changed here goes back only while the cap site
    // puts a size of 5 or less in effect, not while its bytes are no cap this reads.
    const uint32_t capNow = CapInEffect(capSite);
    if (std::memcmp(slotSite, o.slotBefore, sizeof(o.slotBefore)) != 0 && capNow != 0 && capNow <= kGameTeamSize) {
      write(slotSite, o.slotBefore, sizeof(o.slotBefore));  // the read-back decides, not the writer's word
      o.slotRestored = std::memcmp(slotSite, o.slotBefore, sizeof(o.slotBefore)) == 0;
    }
    o.inEffect = capNow;
    o.slotRule = std::memcmp(slotSite, kSlotPatched, sizeof(kSlotPatched)) == 0;
    o.slotGame = std::memcmp(slotSite, kSlotExpected, sizeof(kSlotExpected)) == 0;
    return o;
  };
  if (o.plan == Plan::KeepGame) return done();

  const CapBytes capWanted = CapBytesFor(size);
  const nevr_checked_patch::Site capSpec{kCapExpected, capWanted.bytes, sizeof(kCapExpected)};

  if (o.plan == Plan::SlotThenCap) {
    // The cap is checked before the slot rule goes in, so a cap that cannot follow leaves both sites as found.
    const bool capReady = std::memcmp(capSite, kCapExpected, sizeof(kCapExpected)) == 0 ||
                          std::memcmp(capSite, capWanted.bytes, sizeof(kCapExpected)) == 0;
    if (!capReady) {
      o.capTried = true;  // `cap` keeps its Mismatch
      return done();
    }
    o.slotTried = true;
    o.slot = nevr_checked_patch::Apply(slotSite, SlotSite(), write);
    if (!Took(o.slot)) return done();
  }

  o.capTried = true;
  o.cap = nevr_checked_patch::Apply(capSite, capSpec, write);
  return done();
}

/// A writer reported a failure at a site (`cap` and `slot` stay Mismatch unless tried).
inline bool AWriteFailed(const Outcome& o) {
  using nevr_checked_patch::Result;
  const auto failed = [](Result r) { return r == Result::WriteFailed || r == Result::WrittenWithError; };
  return failed(o.cap) || failed(o.slot);
}

/// The cap runs above 5 with the game's slot rule, which gives a sixth member slot 5.
inline bool CapWithoutSlotRule(const Outcome& o) { return o.inEffect > kGameTeamSize && !o.slotRule; }

/// The size wanted is not the size in effect (a size asked for that did not go in, or, with nothing asked, a cap site
/// that does not hold the game's 5: another writer changed it), a cap above 5 runs without the slot rule, or a write
/// reported a failure, which the operator hears with its error even when the bytes went in.
inline bool IsFailure(const Outcome& o) {
  return AWriteFailed(o) || CapWithoutSlotRule(o) || o.inEffect != (o.plan == Plan::KeepGame ? kGameTeamSize : o.size);
}

inline const char* SiteResultName(bool tried, nevr_checked_patch::Result r) {
  return tried ? nevr_checked_patch::ResultName(r) : "not_tried";
}

inline void Hex(char* out, const uint8_t* bytes, size_t n) {
  for (size_t i = 0; i < n; ++i) std::snprintf(out + 2 * i, 3, "%02x", bytes[i]);
  out[2 * n] = '\0';
}

inline std::string InEffectText(uint32_t inEffect) {
  return inEffect == 0 ? std::string("unknown") : std::to_string(inEffect);
}

/// The one record for the outcome. A success says the size asked for, the size in effect and each site's result,
/// with no bytes (logging: no hex at Info). A failure is the Warning of logging Rule 6: each site's address, the
/// bytes the game holds there, the bytes found and its result. Written to `out`, NUL-terminated.
inline int FormatRecord(char* out, size_t cap, const Outcome& o, uintptr_t capAddr, uintptr_t slotAddr) {
  const std::string inEffect = InEffectText(o.inEffect);
  const char* const capResult = SiteResultName(o.capTried, o.cap);
  const char* const slotResult = SiteResultName(o.slotTried, o.slot);
  if (!IsFailure(o)) {
    const char* what = o.plan == Plan::KeepGame ? "the game's own cap, nothing written"
                       : o.size > kGameTeamSize
                           ? "a match team holds up to the size, within its lobby's limit; unmeasured: more than 10 "
                             "players in a match may stop the server in an arena level"
                           : "a match team holds up to the size";
    return std::snprintf(out, cap, "[NEVR.PATCH] server team size size=%u in_effect=%s (%s) cap=%s slot=%s",
                         static_cast<unsigned>(o.size), inEffect.c_str(), what, capResult, slotResult);
  }
  char capExpected[2 * sizeof(kCapExpected) + 1];
  char capActual[2 * sizeof(kCapExpected) + 1];
  char slotExpected[2 * sizeof(kSlotExpected) + 1];
  char slotActual[2 * sizeof(kSlotExpected) + 1];
  Hex(capExpected, kCapExpected, sizeof(kCapExpected));
  Hex(capActual, o.capBefore, sizeof(kCapExpected));
  Hex(slotExpected, kSlotExpected, sizeof(kSlotExpected));
  Hex(slotActual, o.slotBefore, sizeof(kSlotExpected));
  const char* what = o.plan == Plan::KeepGame ? "nothing written; the cap site holds another writer's bytes"
                     : o.slotRestored         ? "not in effect, the slot rule was put back"
                     : o.inEffect == 0 && o.slotRule
                         ? "the cap site holds bytes that are no cap this reads, so the slot rule stays in"
                     : o.inEffect != o.size ? "not in effect"
                     : CapWithoutSlotRule(o) && o.slotGame
                         ? "the cap is in without the slot rule, so a sixth member takes slot 5"
                     : CapWithoutSlotRule(o)
                         ? "the cap is in without our slot rule, and the slot site holds another writer's bytes"
                         : "in effect, but a write reported an error";
  return std::snprintf(out, cap,
                       "[NEVR.PATCH] server team size size=%u in_effect=%s (%s) cap_addr=0x%llx cap_expected=%s "
                       "cap_actual=%s cap=%s slot_addr=0x%llx slot_expected=%s slot_actual=%s slot=%s%s",
                       static_cast<unsigned>(o.size), inEffect.c_str(), what, static_cast<unsigned long long>(capAddr),
                       capExpected, capActual, capResult, static_cast<unsigned long long>(slotAddr), slotExpected,
                       slotActual, slotResult, o.slotRestored ? " slot_restored=true" : "");
}

/// One line Install logs.
struct Line {
  bool warning = false;
  std::string text;
};

// The error of a failed write is the shared shape in checked_patch.h (WriteErrorFn, WriteErrors). 0 when no write
// failed: then a write_failed site's writer reported success and its read-back is not the replacement.
using WriteErrorFn = nevr_checked_patch::WriteErrorFn;
using WriteErrors = nevr_checked_patch::WriteErrors;

/// Everything Install does, but the log and the process: a client's skip line; on a server, the config value's
/// problem (if any), then Apply on the two sites and its record with the value's source. A site that is write_failed
/// adds the writer's error (`writeError`). `text` is the config value (null when absent); a client never reads the
/// sites.
inline std::vector<Line> InstallLines(bool isServer, const char* text, uint8_t* capSite, uint8_t* slotSite,
                                      nevr_checked_patch::WriteFn write, WriteErrorFn writeError, uintptr_t capAddr,
                                      uintptr_t slotAddr) {
  std::vector<Line> lines;
  if (!isServer) {
    lines.push_back({false, "[NEVR.PATCH] server team size not applied: this is a client (the server assigns teams)"});
    return lines;
  }
  std::string problem;
  const uint32_t size = ParseMaxTeamSize(text, &problem);
  if (!problem.empty()) {
    lines.push_back({true, "[NEVR.PATCH] network.max_team_size=" + std::string(text) + " is " + problem +
                               " (allowed " + std::to_string(kMinTeamSize) + ".." + std::to_string(kMaxTeamSize) +
                               "); using the game's " + std::to_string(kGameTeamSize)});
  }
  const Outcome outcome = Apply(capSite, slotSite, size, write);
  char record[640];
  FormatRecord(record, sizeof(record), outcome, capAddr, slotAddr);
  std::string line(record);
  if (AWriteFailed(outcome)) line += " write_error=" + std::to_string(writeError());
  line += std::string(" source=") + (text != nullptr && text[0] != '\0' ? "config.yaml" : "default");
  lines.push_back({IsFailure(outcome), line});
  return lines;
}

}  // namespace nevr_server_team_size
