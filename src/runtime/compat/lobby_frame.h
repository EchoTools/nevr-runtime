#pragma once
// The lobby messages a client's runtime log records at Info (#929): the find and join requests the game
// sends, and the session success and failure frames the service answers with. This file is the decode, the
// text of the fields and the rate limit; ws_bridge.cpp writes the lines (ObserveSocialFrames).
//
// Platform neutral by construction, like evr_codec.h: no Windows headers, no logging, no game state.
//
// Payload layouts (current family; compat/legacy_codec.cpp FindToLegacy, JoinToLegacy, SuccessV5ToLegacy and
// FailureToLegacy read the same bytes), all integers little endian, a GUID in the Windows layout:
//   find     version lock(8) mode(8) level(8) platform(8) login session(16) entrant count(1) flags(4) pad(3)
//            current lobby(16) group(16) settings JSON NUL, then count x (platform(8) account(8))
//   join     lobby(16) version lock(8) platform(8) login session(16) flags(4, entrant count in the low byte)
//            pad(4) pad(8) settings JSON NUL, then count x (platform(8) account(8)), then an optional team(2)
//   success  mode(8) lobby(16) group(16) endpoint, flags and keys (not read here)
//   failure  mode(8) group(16) code(4) pad(4) message(64) expiry(8); the game reads the code as a byte at +0x18
//            (echovr.exe 0x14017d380 CR15NetClientLobby::LobbySessionFailureCB)
// The settings JSON and the failure message are remote text and are never read out (logging.md Rule 4): a failure's
// message is counted (messageBytes), never copied.

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

#include "runtime/compat/evr_codec.h"

namespace nevr_lobby_frame {

enum class Kind : unsigned { Find = 0, Join = 1, Success = 2, Failure = 3 };
constexpr std::size_t kKindCount = 4;

// SNSLobbySessionSuccessv5 and SNSLobbySessionFailurev4: the CSymbol64 of those names (compat/legacy_codec.h
// pins the same values against the hash in LegacySymbols). The find and join symbols are in evr_codec.h.
inline constexpr uint64_t kSymSessionSuccess = 0x6d4de3650ee3110fULL;
inline constexpr uint64_t kSymSessionFailure = 0x4ae8365ebc45f96cULL;

// The most entrants a request is read with; a larger count is a payload that is not what this decode expects.
constexpr std::size_t kMaxEntrants = 16;
constexpr std::size_t kGuidSize = 16;
constexpr std::size_t kEntrantSize = 16;
// A failure's message field: after mode(8), group(16), code(4) and pad(4), 64 bytes.
constexpr std::size_t kFailureMessageAt = 8 + kGuidSize + 4 + 4;
constexpr std::size_t kFailureMessageSize = 64;

// The lobby message a symbol is when heard in a direction: requests only from the game, answers only from the
// service. nullopt for every other symbol, and for a lobby symbol heard the wrong way round.
inline std::optional<Kind> KindOf(uint64_t symbol, bool fromServer) {
  if (!fromServer) {
    if (symbol == nevr_evr_codec::kSymFindSessionRequest) return Kind::Find;
    if (symbol == nevr_evr_codec::kSymJoinSessionRequest) return Kind::Join;
    return std::nullopt;
  }
  if (symbol == kSymSessionSuccess) return Kind::Success;
  if (symbol == kSymSessionFailure) return Kind::Failure;
  return std::nullopt;
}

/// The words of "lobby <name> could not be read": what the message is called in a sentence.
inline const char* KindName(Kind kind) {
  switch (kind) {
    case Kind::Find: return "find request";
    case Kind::Join: return "join request";
    case Kind::Success: return "session success";
    case Kind::Failure: return "session failure";
  }
  return "message";
}

/// The value of a `kind=` field: one word, so the line stays key=value.
inline const char* KindToken(Kind kind) {
  switch (kind) {
    case Kind::Find: return "find";
    case Kind::Join: return "join";
    case Kind::Success: return "success";
    case Kind::Failure: return "failure";
  }
  return "unknown";
}

struct Entrant {
  uint64_t platform = 0;
  uint64_t account = 0;
};

using Guid = std::array<uint8_t, kGuidSize>;

struct Decoded {
  Kind kind = Kind::Find;
  uint64_t mode = 0;               // find, success, failure
  Guid lobby{};                    // join, success
  Guid currentLobby{};             // find: the lobby being left; all zero for none
  Guid group{};                    // find, success, failure
  std::vector<Entrant> entrants;   // find, join
  uint32_t code = 0;               // failure
  uint32_t messageBytes = 0;       // failure: bytes before the message's NUL, at most its field; never the text
};

namespace detail {

inline uint32_t ReadLE32(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) |
         (static_cast<uint32_t>(p[3]) << 24);
}

inline Guid ReadGuid(const uint8_t* p) {
  Guid g{};
  std::memcpy(g.data(), p, kGuidSize);
  return g;
}

// The entrants after the settings string that starts at `settingsAt`: `count` of them, all inside the payload.
inline bool ReadEntrants(const uint8_t* payload, std::size_t len, std::size_t settingsAt, std::size_t count,
                         std::vector<Entrant>* out) {
  if (count > kMaxEntrants || settingsAt > len) return false;
  const void* nul = std::memchr(payload + settingsAt, 0, len - settingsAt);
  if (nul == nullptr) return false;
  const std::size_t entrantsAt = static_cast<std::size_t>(static_cast<const uint8_t*>(nul) - payload) + 1;
  if (len - entrantsAt < count * kEntrantSize) return false;
  out->clear();
  for (std::size_t i = 0; i < count; ++i) {
    const uint8_t* at = payload + entrantsAt + i * kEntrantSize;
    out->push_back(Entrant{nevr_evr_codec::ReadLE64(at), nevr_evr_codec::ReadLE64(at + 8)});
  }
  return true;
}

// The bytes before the first NUL in the `size`-byte field at `at`, counted over the bytes the frame holds: the whole
// field when none of it is NUL, zero when the frame ends before the field. Nothing is read past `len`.
inline uint32_t FieldTextBytes(const uint8_t* payload, std::size_t len, std::size_t at, std::size_t size) {
  if (at >= len) return 0;
  const std::size_t held = len - at < size ? len - at : size;
  const void* nul = std::memchr(payload + at, 0, held);
  if (nul == nullptr) return static_cast<uint32_t>(held);
  return static_cast<uint32_t>(static_cast<const uint8_t*>(nul) - (payload + at));
}

}  // namespace detail

/// Decodes `payload` (`len` bytes, all inside the caller's frame) as `kind`. False when it is shorter than its
/// layout, has no end to the settings string, or names more entrants than it holds or than kMaxEntrants; nothing
/// is read past `len`.
inline bool Decode(Kind kind, const uint8_t* payload, std::size_t len, Decoded* out) {
  *out = Decoded{};
  out->kind = kind;
  switch (kind) {
    case Kind::Find: {
      constexpr std::size_t kFixed = 8 * 4 + kGuidSize + 1 + 4 + 3 + kGuidSize + kGuidSize;
      if (len < kFixed) return false;
      out->mode = nevr_evr_codec::ReadLE64(payload + 8);
      out->currentLobby = detail::ReadGuid(payload + 8 * 4 + kGuidSize + 1 + 4 + 3);
      out->group = detail::ReadGuid(payload + 8 * 4 + kGuidSize + 1 + 4 + 3 + kGuidSize);
      return detail::ReadEntrants(payload, len, kFixed, payload[8 * 4 + kGuidSize], &out->entrants);
    }
    case Kind::Join: {
      constexpr std::size_t kFlagsAt = kGuidSize + 8 + 8 + kGuidSize;
      constexpr std::size_t kFixed = kFlagsAt + 4 + 4 + 8;
      if (len < kFixed) return false;
      out->lobby = detail::ReadGuid(payload);
      return detail::ReadEntrants(payload, len, kFixed, payload[kFlagsAt], &out->entrants);
    }
    case Kind::Success: {
      if (len < 8 + kGuidSize + kGuidSize) return false;
      out->mode = nevr_evr_codec::ReadLE64(payload);
      out->lobby = detail::ReadGuid(payload + 8);
      out->group = detail::ReadGuid(payload + 8 + kGuidSize);
      return true;
    }
    case Kind::Failure: {
      if (len < 8 + kGuidSize + 4) return false;
      out->mode = nevr_evr_codec::ReadLE64(payload);
      out->group = detail::ReadGuid(payload + 8);
      out->code = detail::ReadLE32(payload + 8 + kGuidSize);
      out->messageBytes = detail::FieldTextBytes(payload, len, kFailureMessageAt, kFailureMessageSize);
      return true;
    }
  }
  return false;
}

/// A GUID as text, upper case, in the Windows layout the wire and the game's own GUID text use
/// (XXXXXXXX-XXXX-XXXX-XXXX-XXXXXXXXXXXX, the first three groups little endian), so it equals the `lobbyid` the
/// service writes into the party data.
inline std::string FormatGuid(const Guid& g) {
  char text[40];
  std::snprintf(text, sizeof(text), "%02X%02X%02X%02X-%02X%02X-%02X%02X-%02X%02X-%02X%02X%02X%02X%02X%02X", g[3], g[2],
                g[1], g[0], g[5], g[4], g[7], g[6], g[8], g[9], g[10], g[11], g[12], g[13], g[14], g[15]);
  return text;
}

inline bool IsNil(const Guid& g) {
  for (const uint8_t b : g) {
    if (b != 0) return false;
  }
  return true;
}

/// FormatGuid, or "none" for the nil GUID (a find from no lobby).
inline std::string FormatGuidOrNone(const Guid& g) { return IsNil(g) ? std::string("none") : FormatGuid(g); }

/// The entrants as XPIDs (`OVR-ORG-<account>`), comma separated, in the order the request names them.
inline std::string FormatAccounts(const std::vector<Entrant>& entrants) {
  std::string text;
  for (const Entrant& entrant : entrants) {
    if (!text.empty()) text.push_back(',');
    text += nevr_evr_codec::PlatformPrefix(entrant.platform);
    text.push_back('-');
    text += std::to_string(entrant.account);
  }
  return text;
}

/// The game's own name for a failure code, as one word (echovr.exe 0x14017d380 switches on the byte at +0x18:
/// 0 unspecified, 1 unsupported version, 2 bad request, 3 timeout, 4 lobby invalid, 5 lobby incompatible,
/// 6 lobby no crossplay, 7 lobby locked, 8 lobby full, 9 internal error, 10 missing entitlement, 11 banned,
/// 12 kicked, 13 access denied); anything else is "unknown".
inline const char* FailureReason(uint32_t code) {
  static const char* const kNames[] = {"unspecified",        "unsupported_version", "bad_request",   "timeout",
                                       "lobby_invalid",      "lobby_incompatible",  "lobby_no_crossplay",
                                       "lobby_locked",       "lobby_full",          "internal_error",
                                       "missing_entitlement", "banned",             "kicked",         "access_denied"};
  return code < sizeof(kNames) / sizeof(kNames[0]) ? kNames[code] : "unknown";
}

/// Bounds how many lobby records a run writes. The bridge cannot bound how often the game sends a request
/// (its party-follow loop runs from the game's own update), so each kind may write `kBurst` records per
/// `kWindowMs` window and the rest are counted; the count is reported when the window has ended, by the next
/// lobby frame of any kind. The limiter has no clock and no lock: the caller passes the time and serializes
/// calls.
class RecordLimiter {
 public:
  static constexpr uint64_t kWindowMs = 10000;
  static constexpr unsigned kBurst = 10;

  struct Verdict {
    bool log = false;                              // write this frame's record
    std::array<uint64_t, kKindCount> flushed{};    // per kind: records its ended window did not write
  };

  Verdict Admit(Kind kind, uint64_t nowMs) {
    Verdict verdict;
    for (std::size_t i = 0; i < kKindCount; ++i) {
      Slot& slot = slots_[i];
      if (slot.open && nowMs - slot.start >= kWindowMs) {
        verdict.flushed[i] = slot.suppressed;
        slot = Slot{};
      }
    }
    Slot& slot = slots_[static_cast<std::size_t>(kind)];
    if (!slot.open) {
      slot.open = true;
      slot.start = nowMs;
    }
    if (slot.logged < kBurst) {
      ++slot.logged;
      verdict.log = true;
    } else {
      ++slot.suppressed;
    }
    return verdict;
  }

  void Reset() { slots_ = {}; }

 private:
  struct Slot {
    bool open = false;
    uint64_t start = 0;
    unsigned logged = 0;
    uint64_t suppressed = 0;
  };
  std::array<Slot, kKindCount> slots_{};
};

/// The request a session record names: the find or join the game sent last on this client, its age when it was
/// answered, and whether that answer is the first one to it. `sent` is false until a request is sent after a Reset.
struct RequestContext {
  bool sent = false;
  Kind kind = Kind::Find;
  uint64_t ageMs = 0;
  bool outstanding = false;
};

/// The last find or join the game sent. An answer takes the request it answers: outstanding for the first answer
/// after that request, not for any later one. Process-wide, not per connection: the tracker keeps no connection index,
/// so an answer on one connection can name a request sent on another. The caller passes the time and serializes
/// calls, as RecordLimiter.
class RequestTracker {
 public:
  void Sent(Kind kind, uint64_t nowMs) {
    sent_ = true;
    answered_ = false;
    kind_ = kind;
    sentAtMs_ = nowMs;
  }

  RequestContext Answer(uint64_t nowMs) {
    RequestContext context;
    if (!sent_) return context;
    context.sent = true;
    context.kind = kind_;
    context.ageMs = nowMs >= sentAtMs_ ? nowMs - sentAtMs_ : 0;
    context.outstanding = !answered_;
    answered_ = true;
    return context;
  }

  void Reset() { *this = RequestTracker{}; }

 private:
  bool sent_ = false;
  bool answered_ = false;
  Kind kind_ = Kind::Find;
  uint64_t sentAtMs_ = 0;
};

/// The request as the record's fields: `last_request=find last_request_age_ms=250 request_outstanding=yes`, or `none`
/// in the first two and `no` in the third when no request was sent since the last Reset.
inline std::string FormatRequestFields(const RequestContext& request) {
  if (!request.sent) return "last_request=none last_request_age_ms=none request_outstanding=no";
  return std::string("last_request=") + KindToken(request.kind) +
         " last_request_age_ms=" + std::to_string(request.ageMs) +
         " request_outstanding=" + (request.outstanding ? "yes" : "no");
}

/// The party as the record's fields: `party_id=7 party_size=3 party_leader=yes`. Outside a party it is
/// `party_id=none party_size=0 party_leader=no`. The leader is `yes` when the owner is the local account, `no` when
/// another account owns the party, and `unknown` while either id is still zero: a value never stands in for an id
/// the client does not have yet.
inline std::string FormatPartyFields(uint64_t partyId, uint64_t memberCount, uint64_t ownerId, uint64_t selfId) {
  if (partyId == 0) return "party_id=none party_size=0 party_leader=no";
  const char* leader = "unknown";
  if (ownerId != 0 && selfId != 0) leader = ownerId == selfId ? "yes" : "no";
  return "party_id=" + std::to_string(partyId) + " party_size=" + std::to_string(memberCount) +
         " party_leader=" + leader;
}

}  // namespace nevr_lobby_frame
