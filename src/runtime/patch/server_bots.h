#pragma once

// #1423: a dedicated server adds AI bots to its sessions (network.bots_per_session, flat key nevr_bots_per_session).
//
// heisthecat31's EchoRelay adds them through the game's own add-bot request; this is the same route, read
// again in our disassembly of the pinned echovr.exe:
// - The request is RequestAddBot (0x1401A0D70). Its one direct caller (`objdump -d` of the pinned echovr.exe, the
//   lines `call 0x1401a0d70`: one, at 0x140DE4FAA) is R15NetSpawnBotNode (0x140DE4CC0, by its
//   "R15NetSpawnBotNode used before getting in game" / "... while the lobby is full" / "RequestAddBot called more than
//   once before receiving a response" lines). The node takes the net game through GetNetGameFromContext (0x140113A90)
//   and passes it with a 0xD8-byte request. RequestAddBot returns 0 unless the net game's state is 9 (InGame), the
//   entrant count at +0xE2 is below 16 and the pending flag at +0x27D8 goes 0 -> 1; it then calls the lobby's
//   AddBot (0x140610F10) on [netGame+0x40]+0x18.
// - AddBot reads Lobby::hostingFlags (+0x1C): with bit 0 set it generates an offline bot session id and adds the
//   entrant itself ("generating offline bot session id"); clear, it asks the matchmaker through the lobby's service,
//   which the community service does not answer for a server. The driver sets bit 0 for the call only, as EchoRelay
//   does.
// - The node copies the request from its own instance data, which the binary does not hold. When the instance's
//   +0xD8 is 0 (the test at 0x140DE4E49) it then writes +0x10 with the identity quaternion (.rdata 0x1416D5A70),
//   +0x20 with the 12 bytes of the .data globals 0x14201D338 and 0x14201D340 (FLT_MAX three times: no spawn position)
//   and +0x2C with 1.0f; otherwise those come from the instance too. The request here takes the +0xD8 = 0 values.
//   The rest (+0x00 = -1, +0x08 = 0xFFFF for any team, +0x38..+0xCF = 0xFF, +0xD0 = 0) is EchoRelay's.
// - A bot takes a body from the net game's pools (+0x2B08). The game's own bot setup (0x140152210) picks body 0 or 1
//   of a table its static initializer fills (0x1400C3CE0: "bot_arena", "bot_combat", whose CSymbol64Hash values are
//   kBotBodies) and allocates through 0x1404E61E0, which finds the pool with FindPool (0x140502D80) and skips an
//   entry whose +0x30 is 0 ([owner+0x3E8] + index * 0x88), or above the entry's capacity at +0x82 (only memory that
//   is not a pool gives that; BodyFree does not read +0x82). A bot is requested only when every body pool the level
//   has has one free, so the body the game picks is there.
//
// The session settings: the game reads `numteams` and `players_per_team` from the session's settings (0x1401510D0
// and 0x14060A920, through 0x1406162A0, which returns 0 for an absent key, so no team table is built). The community
// service's session start carries neither (LobbySessionSettings: appid, gametype, level, features). With bots wanted,
// the session start is rewritten through nlohmann::ordered_json before it reaches the game; its 36-byte header is
// the service's encoder (GameServerSessionStart.Stream: two GUIDs, player limit, entrant count, lobby type, a pad
// byte), then the NUL-terminated JSON, then the entrants.
//
// Which sessions: an arena or combat match only, by the settings' `gametype`, which the service writes as the
// CSymbol64Hash of the mode's name (a JSON integer; a name is read too). A social lobby, a session whose game type is
// absent or not one of the matches, and a session start the rewrite could not read get no bots and keep their
// settings as they came.
//
// Where it runs: from GameServerLib::Update, which the game calls from its loop between its own updates, as the net
// game's return to lobby there is; on the game-loop thread only.
//
// What is here: the decisions, the lines, and the reads and calls on memory a test hands in (Drive, ReadNetGame,
// ReadBodyPools, WithOfflineBit, SessionIdOf). server_bots.cpp is the glue: it hands them the game's addresses and
// functions, the clock, the session and the thread, and logs the lines.

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "nevr/symbol_hash.h"
#include "runtime/compat/lobby_frame.h"
#include "runtime/server/game_type_table.h"

namespace nevr_server_bots {

// ---- The config value -------------------------------------------------------------------------------------------

/// The most bots a session gets: the game's own entrant limit (RequestAddBot: +0xE2 below 16).
constexpr uint32_t kMaxBots = 16;

/// Parses network.bots_per_session. Absent or empty is 0 (off). A value that is not a plain whole number, or is above
/// kMaxBots, is 0 with `problem` set; it is never clamped to a count nobody wrote.
inline uint32_t ParseBotsPerSession(const char* text, std::string* problem) {
  if (problem != nullptr) problem->clear();
  if (text == nullptr || text[0] == '\0') return 0;
  uint32_t value = 0;
  for (const char* c = text; *c != '\0'; ++c) {
    if (*c < '0' || *c > '9') {
      if (problem != nullptr) *problem = "not a whole number";
      return 0;
    }
    value = value * 10 + static_cast<uint32_t>(*c - '0');
    if (value > kMaxBots) {
      if (problem != nullptr) *problem = "above the limit of " + std::to_string(kMaxBots);
      return 0;
    }
  }
  return value;
}

// ---- The session settings -----------------------------------------------------------------------------------------

using nevr_game_type_table::kSessionStartHeaderSize;
/// Two teams, as the service's arena and combat matches have.
constexpr int kNumTeams = 2;

// ---- The session's game type ------------------------------------------------------------------------------------
// The table (the service's eleven modes) and its reader live in server/game_type_table.h, shared with the telemetry
// header. Only a match (bots) gets bots.

using nevr_game_type_table::GameType;
using nevr_game_type_table::kGameTypes;
using nevr_game_type_table::ReadGameType;
using nevr_game_type_table::SessionGameType;

enum class SettingsResult {
  kNotWanted,       // no bots: the frame is forwarded as it came
  kNotAMatch,       // the game type is not an arena or combat match: forwarded as it came, no bots this session
  kAlreadyPresent,  // both keys are there: kept as they are
  kRewritten,       // the missing key or keys were added
  kTooShort,        // no room for the header and a settings string
  kNoTerminator,    // the settings string does not end inside the frame
  kNotJson,         // the settings string does not parse
  kNotObject,       // it parses, but not to an object
};

inline const char* SettingsResultName(SettingsResult r) {
  switch (r) {
    case SettingsResult::kNotWanted: return "not_wanted";
    case SettingsResult::kNotAMatch: return "not_a_match";
    case SettingsResult::kAlreadyPresent: return "already_present";
    case SettingsResult::kRewritten: return "rewritten";
    case SettingsResult::kTooShort: return "too_short";
    case SettingsResult::kNoTerminator: return "no_terminator";
    case SettingsResult::kNotJson: return "not_json";
    case SettingsResult::kNotObject: return "not_object";
  }
  return "unknown";
}

/// A result the operator must hear about: bots were wanted and the settings could not be read.
inline bool IsSettingsFailure(SettingsResult r) {
  return r == SettingsResult::kTooShort || r == SettingsResult::kNoTerminator || r == SettingsResult::kNotJson ||
         r == SettingsResult::kNotObject;
}

/// Whether the session the settings start gets bots: a match whose settings carry both keys.
inline bool BotsThisSession(SettingsResult r) {
  return r == SettingsResult::kRewritten || r == SettingsResult::kAlreadyPresent;
}

struct SettingsRewrite {
  SettingsResult result = SettingsResult::kNotWanted;
  std::string gameType = "none";  // ReadGameType's name, once the settings parsed to an object
  std::vector<uint8_t> frame;  // the frame to forward when result is kRewritten; empty otherwise
  size_t settingsBytes = 0;    // the settings string's length as it came (no NUL)
  size_t rewrittenBytes = 0;   // its length after the rewrite
  bool addedNumTeams = false;
  bool addedPlayersPerTeam = false;
  int playersPerTeamWritten = 0;  // the value written as players_per_team (network.max_team_size, #1422)
};

/// Adds `numteams` and `players_per_team` to a session start's settings when bots are wanted, the game type is a match
/// and either key is absent. A value already there is kept. The header and every byte after the settings' NUL are
/// copied unchanged.
inline SettingsRewrite RewriteSessionSettings(const uint8_t* frame, size_t size, uint32_t bots, int playersPerTeam) {
  SettingsRewrite out;
  if (bots == 0) return out;
  if (frame == nullptr || size <= kSessionStartHeaderSize) {
    out.result = SettingsResult::kTooShort;
    return out;
  }
  const uint8_t* settings = frame + kSessionStartHeaderSize;
  const void* nul = std::memchr(settings, 0, size - kSessionStartHeaderSize);
  if (nul == nullptr) {
    out.result = SettingsResult::kNoTerminator;
    return out;
  }
  out.settingsBytes = static_cast<size_t>(static_cast<const uint8_t*>(nul) - settings);
  nlohmann::ordered_json doc = nlohmann::ordered_json::parse(settings, settings + out.settingsBytes, nullptr, false);
  if (doc.is_discarded()) {
    out.result = SettingsResult::kNotJson;
    return out;
  }
  if (!doc.is_object()) {
    out.result = SettingsResult::kNotObject;
    return out;
  }
  const SessionGameType gameType = ReadGameType(doc);
  out.gameType = gameType.name;
  if (!gameType.bots) {
    out.result = SettingsResult::kNotAMatch;
    return out;
  }
  out.addedNumTeams = !doc.contains("numteams");
  out.addedPlayersPerTeam = !doc.contains("players_per_team");
  if (!out.addedNumTeams && !out.addedPlayersPerTeam) {
    out.result = SettingsResult::kAlreadyPresent;
    return out;
  }
  if (out.addedNumTeams) doc["numteams"] = kNumTeams;
  if (out.addedPlayersPerTeam) {
    doc["players_per_team"] = playersPerTeam;
    out.playersPerTeamWritten = playersPerTeam;
  }
  const std::string text = doc.dump();
  out.rewrittenBytes = text.size();
  const size_t tailAt = kSessionStartHeaderSize + out.settingsBytes;  // the NUL, then the entrants
  out.frame.reserve(kSessionStartHeaderSize + text.size() + (size - tailAt));
  out.frame.insert(out.frame.end(), frame, frame + kSessionStartHeaderSize);
  out.frame.insert(out.frame.end(), text.begin(), text.end());
  out.frame.insert(out.frame.end(), frame + tailAt, frame + size);
  out.result = SettingsResult::kRewritten;
  return out;
}

/// The session id the lines carry: the session start's match id, its first 16 bytes; "none" when it is shorter.
inline std::string SessionIdOf(const uint8_t* frame, uint64_t size) {
  return size >= nevr_lobby_frame::kGuidSize ? nevr_lobby_frame::FormatGuid(nevr_lobby_frame::detail::ReadGuid(frame))
                                             : std::string("none");
}

// ---- The request --------------------------------------------------------------------------------------------------

constexpr size_t kRequestSize = 0xD8;
/// The bytes of the two .data globals the spawn-bot node copies to +0x20 (0x14201D338, 8 bytes; 0x14201D340, 4).
constexpr size_t kNoPositionSize = 12;

struct Request {
  uint8_t bytes[kRequestSize];
};

/// The request as the spawn-bot node sets it up: any team, no spawn position. See the file comment for which fields
/// the binary gives and which are EchoRelay's.
inline Request BuildRequest(const uint8_t noPosition[kNoPositionSize]) {
  Request r{};
  const int64_t anyEntrant = -1;
  const uint32_t anyTeam = 0xFFFF;
  const float identity[4] = {0.0f, 0.0f, 0.0f, 1.0f};
  const float one = 1.0f;
  std::memcpy(r.bytes + 0x00, &anyEntrant, sizeof(anyEntrant));
  std::memcpy(r.bytes + 0x08, &anyTeam, sizeof(anyTeam));
  std::memcpy(r.bytes + 0x10, identity, sizeof(identity));
  std::memcpy(r.bytes + 0x20, noPosition, kNoPositionSize);
  std::memcpy(r.bytes + 0x2C, &one, sizeof(one));
  std::memset(r.bytes + 0x38, 0xFF, 0xD0 - 0x38);
  return r;
}

// ---- The schedule -------------------------------------------------------------------------------------------------

/// The game's entrant limit, and the time between two requests.
constexpr uint16_t kMaxEntrants = 16;
constexpr uint64_t kRequestIntervalMs = 1000;
constexpr uint64_t kNoBodyWarningIntervalMs = 60000;
constexpr uint64_t kPendingWarningMs = 10000;

struct Schedule {
  uint32_t requested = 0;     // requests RequestAddBot accepted in this session (not bots seen to join)
  uint64_t nextAskMs = 0;     // no request before this
  uint64_t lastNoBodyMs = 0;  // the last "no free body" warning
  bool warnedNoBody = false;
  bool pendingSeen = false;      // the pending flag is set, since pendingSinceMs
  uint64_t pendingSinceMs = 0;
  bool warnedPending = false;    // said once per stretch of the flag being set
};

enum class Gate {
  kNotInGame,  // the session is not in game: the count starts again
  kDone,       // the session has the bots it wants
  kWaiting,    // a request was made less than a second ago
  kLobbyFull,  // 16 entrants
  kPending,    // the game has not answered the last request
  kAsk,        // check for a free body, then request one
};

/// One frame's decision. A kAsk moves the next ask a second on, whatever the request then does.
inline Gate Decide(Schedule& s, uint32_t wanted, bool inGame, uint16_t entrants, bool pending, uint64_t nowMs) {
  if (!inGame) {
    s = Schedule{};
    return Gate::kNotInGame;
  }
  if (s.requested >= wanted) return Gate::kDone;
  if (nowMs < s.nextAskMs) return Gate::kWaiting;
  if (entrants >= kMaxEntrants) return Gate::kLobbyFull;
  if (pending) return Gate::kPending;
  s.nextAskMs = nowMs + kRequestIntervalMs;
  return Gate::kAsk;
}

/// RequestAddBot's answer to a kAsk.
inline void Requested(Schedule& s, bool accepted) {
  if (accepted) ++s.requested;
}

/// Whether a "no free body" warning is due: the first one, then at most one a minute.
inline bool NoBodyWarningDue(Schedule& s, uint64_t nowMs) {
  if (s.warnedNoBody && nowMs - s.lastNoBodyMs < kNoBodyWarningIntervalMs) return false;
  s.warnedNoBody = true;
  s.lastNoBodyMs = nowMs;
  return true;
}

/// Whether a "the game has not answered" warning is due: once, when the pending flag has stayed set for
/// kPendingWarningMs; a cleared flag starts the wait again.
inline bool PendingWarningDue(Schedule& s, bool pending, uint64_t nowMs) {
  if (!pending) {
    s.pendingSeen = false;
    s.warnedPending = false;
    return false;
  }
  if (!s.pendingSeen) {
    s.pendingSeen = true;
    s.pendingSinceMs = nowMs;
    return false;
  }
  if (s.warnedPending || nowMs - s.pendingSinceMs < kPendingWarningMs) return false;
  s.warnedPending = true;
  return true;
}

// ---- The body pools -----------------------------------------------------------------------------------------------

/// The two bot bodies of the game's table, by the hash of their names.
inline constexpr uint64_t kBotBodies[2] = {EchoVR::CSymbol64Hash("bot_arena"), EchoVR::CSymbol64Hash("bot_combat")};

struct BodyPool {
  bool found = false;  // FindPool found a pool for the body
  int64_t free = 0;    // the entry's +0x30, read only when found
};

/// A bot can be added: the level has at least one bot body pool, and every pool it has has a free body.
inline bool BodyFree(const BodyPool* pools, size_t count) {
  bool any = false;
  for (size_t i = 0; i < count; ++i) {
    if (!pools[i].found) continue;
    if (pools[i].free <= 0) return false;
    any = true;
  }
  return any;
}

// ---- The sites the driver relies on -------------------------------------------------------------------------------

/// Instructions of the game that hold the offsets and the call the driver relies on, each read from the pinned
/// echovr.exe. A mismatch at any of them leaves the driver off.
struct Witness {
  const char* name;
  uintptr_t rva;
  const uint8_t* bytes;
  size_t size;
};

// RequestAddBot 0x1401A0D70: its prologue, `cmp dword [rcx], 9`, `cmp word [rcx+0xE2], 0x10`, and the
// `lock cmpxchg [rcx+0x27D8]` that takes the pending flag.
inline constexpr uint8_t kRequestAddBotBytes[] = {
    0x48, 0x89, 0x5c, 0x24, 0x10, 0x56, 0x48, 0x81, 0xec, 0x80, 0x00, 0x00, 0x00, 0x83, 0x39, 0x09, 0x48, 0x8b,
    0xd9, 0x0f, 0x85, 0x4e, 0x01, 0x00, 0x00, 0x66, 0x83, 0xb9, 0xe2, 0x00, 0x00, 0x00, 0x10, 0x0f, 0x83, 0x40,
    0x01, 0x00, 0x00, 0xbe, 0x01, 0x00, 0x00, 0x00, 0x33, 0xc0, 0xf0, 0x0f, 0xb1, 0xb1, 0xd8, 0x27, 0x00, 0x00};
// RequestAddBot 0x1401A0DC2 `mov rdi, [rbx+0x40]` and 0x1401A0EAE `lea rcx, [rdi+0x18]`: the lobby AddBot gets.
inline constexpr uint8_t kRequestAddBotLobbyBytes[] = {0x48, 0x8b, 0x7b, 0x40};
inline constexpr uint8_t kRequestAddBotLobbyArgBytes[] = {0x48, 0x8d, 0x4f, 0x18};
// AddBot 0x140611096: `mov eax, [rdi+0x1c]; not eax; test al, 1`, the hostingFlags bit 0 test.
inline constexpr uint8_t kAddBotOfflineBytes[] = {0x8b, 0x47, 0x1c, 0xf7, 0xd0, 0xa8, 0x01};
// FindPool 0x140502D80: its prologue through the first `mov rbx, [r11+0x370]`.
inline constexpr uint8_t kFindPoolBytes[] = {
    0x48, 0x89, 0x5c, 0x24, 0x10, 0x48, 0x89, 0x6c, 0x24, 0x18, 0x48, 0x89, 0x74, 0x24, 0x20, 0x57, 0x41, 0x56,
    0x49, 0x89, 0x08, 0x4d, 0x8b, 0xf1, 0x49, 0x8b, 0xf0, 0x4c, 0x8b, 0xd9, 0x48, 0x85, 0xc9, 0x0f, 0x84, 0xa5,
    0x00, 0x00, 0x00, 0xbd, 0xff, 0xff, 0x00, 0x00, 0x0f, 0x1f, 0x40, 0x00, 0x49, 0x8b, 0x9b, 0x70, 0x03, 0x00,
    0x00, 0x48, 0x85, 0xdb, 0x74, 0x7d};
// 0x1404E6233, after FindPool in the game's body allocation: the entry at [owner+0x3E8] + index * 0x88 and its
// free count at +0x30.
inline constexpr uint8_t kBodyAllocBytes[] = {
    0x0f, 0xb7, 0x53, 0x08, 0x48, 0x69, 0xfa, 0x88, 0x00, 0x00, 0x00, 0x89, 0x74, 0x24, 0x2c, 0x48, 0x8b, 0x74,
    0x24, 0x48, 0x48, 0xc7, 0x44, 0x24, 0x20, 0xff, 0xff, 0xff, 0xff, 0xc7, 0x44, 0x24, 0x28, 0xff, 0xff, 0xff,
    0xff, 0x48, 0x03, 0xbe, 0xe8, 0x03, 0x00, 0x00, 0x0f, 0xb7, 0x8f, 0x82, 0x00, 0x00, 0x00, 0x8b, 0xc1, 0x48,
    0x2b, 0x47, 0x30, 0x48, 0x3b, 0xc1};
// 0x1401523DF in the game's bot setup: `mov rcx, [rdi+0x2B08]`, the net game's pools.
inline constexpr uint8_t kBotPoolsBytes[] = {0x48, 0x8b, 0x8f, 0x08, 0x2b, 0x00, 0x00};

/// RVAs (echovr.exe base 0x140000000). The two the driver calls are also in runtime/hook/addresses.h.
constexpr uintptr_t kRequestAddBotRva = 0x1A0D70;
constexpr uintptr_t kFindPoolRva = 0x502D80;

inline constexpr Witness kWitnesses[] = {
    {"RequestAddBot", kRequestAddBotRva, kRequestAddBotBytes, sizeof(kRequestAddBotBytes)},
    {"RequestAddBot lobby", 0x1A0DC2, kRequestAddBotLobbyBytes, sizeof(kRequestAddBotLobbyBytes)},
    {"RequestAddBot lobby argument", 0x1A0EAE, kRequestAddBotLobbyArgBytes, sizeof(kRequestAddBotLobbyArgBytes)},
    {"AddBot offline test", 0x611096, kAddBotOfflineBytes, sizeof(kAddBotOfflineBytes)},
    {"FindPool", kFindPoolRva, kFindPoolBytes, sizeof(kFindPoolBytes)},
    {"bot body allocation", 0x4E6233, kBodyAllocBytes, sizeof(kBodyAllocBytes)},
    {"bot body pools", 0x1523DF, kBotPoolsBytes, sizeof(kBotPoolsBytes)},
};
constexpr size_t kWitnessCount = sizeof(kWitnesses) / sizeof(kWitnesses[0]);

/// The index of the first witness whose bytes differ at `base + rva`, or kWitnessCount when all hold.
inline size_t FirstMismatch(const uint8_t* base) {
  for (size_t i = 0; i < kWitnessCount; ++i) {
    if (std::memcmp(base + kWitnesses[i].rva, kWitnesses[i].bytes, kWitnesses[i].size) != 0) return i;
  }
  return kWitnessCount;
}

// ---- The net game's fields the driver reads (each witnessed above) ------------------------------------------------

constexpr size_t kNetGameEntrantsOffset = 0xE2;
constexpr size_t kNetGamePendingOffset = 0x27D8;
constexpr size_t kNetGameLobbyOffset = 0x40;
constexpr size_t kLobbyFromHolderOffset = 0x18;
constexpr size_t kLobbyHostingFlagsOffset = 0x1C;
constexpr uint32_t kHostingFlagOffline = 0x1;
constexpr size_t kNetGamePoolsOffset = 0x2B08;
constexpr size_t kPoolOwnerEntriesOffset = 0x3E8;
constexpr size_t kPoolEntrySize = 0x88;
constexpr size_t kPoolEntryFreeOffset = 0x30;
constexpr int32_t kNetGameInGame = 9;

// ---- Reading the game's memory ------------------------------------------------------------------------------------

template <typename T>
T ReadAt(const void* base, size_t offset) {
  T value{};
  std::memcpy(&value, static_cast<const uint8_t*>(base) + offset, sizeof(value));
  return value;
}

/// The three net game fields the schedule reads.
struct NetGameView {
  bool inGame = false;
  uint16_t entrants = 0;
  bool pending = false;
};
inline NetGameView ReadNetGame(const void* netGame) {
  NetGameView v;
  v.inGame = ReadAt<int32_t>(netGame, 0) == kNetGameInGame;
  v.entrants = ReadAt<uint16_t>(netGame, kNetGameEntrantsOffset);
  v.pending = ReadAt<int32_t>(netGame, kNetGamePendingOffset) != 0;
  return v;
}

/// The free count of the pool entry FindPool named: entry (index & 0xFFFF) of the owner's entries, at +0x30.
inline int64_t PoolEntryFree(const void* entries, uint64_t index) {
  return ReadAt<int64_t>(entries, (index & 0xFFFF) * kPoolEntrySize + kPoolEntryFreeOffset);
}

/// The two bot-body pools of the level, as the game's body allocation reads them: FindPool (`findPool(pools, body,
/// &owner, &index)`, non-zero when found), then the entry `index` of [owner+0x3E8] and its free count. No pools, a
/// pool FindPool does not find, or an owner without entries leaves that body `found = false`.
template <typename FindPoolCall>
void ReadBodyPools(FindPoolCall findPool, void* pools, BodyPool body[2]) {
  for (size_t i = 0; i < 2; ++i) body[i] = BodyPool{};
  if (pools == nullptr) return;
  for (size_t i = 0; i < 2; ++i) {
    void* owner = nullptr;
    uint64_t index = 0;
    if (findPool(pools, kBotBodies[i], &owner, &index) == 0 || owner == nullptr) continue;
    const uint8_t* entries = ReadAt<const uint8_t*>(owner, kPoolOwnerEntriesOffset);
    if (entries == nullptr) continue;
    body[i].found = true;
    body[i].free = PoolEntryFree(entries, index);
  }
}

/// Lobby::hostingFlags of the lobby AddBot is called on ([netGame+0x40] + 0x18).
inline uint32_t* HostingFlags(uint8_t* lobbyHolder) {
  return reinterpret_cast<uint32_t*>(lobbyHolder + kLobbyFromHolderOffset + kLobbyHostingFlagsOffset);
}

/// Runs `call` with the offline bit set, and clears it after unless it was set before. Only bit 0 is touched, with an
/// atomic or and and, so a bit another thread or the call changes meanwhile is kept.
template <typename Call>
bool WithOfflineBit(uint32_t* flags, Call call) {
  const uint32_t before = __atomic_fetch_or(flags, kHostingFlagOffline, __ATOMIC_SEQ_CST);
  const bool result = call();
  if ((before & kHostingFlagOffline) == 0) __atomic_fetch_and(flags, ~kHostingFlagOffline, __ATOMIC_SEQ_CST);
  return result;
}

/// Points the frame the game is given at the rewritten one, kept in `storage`, when the rewrite made one; any other
/// result leaves the frame as it came. Returns whether it was swapped.
inline bool UseRewrite(SettingsRewrite& r, std::vector<uint8_t>& storage, void** frame, uint64_t* size) {
  if (r.result != SettingsResult::kRewritten) return false;
  storage = std::move(r.frame);
  *frame = storage.data();
  *size = storage.size();
  return true;
}

// ---- What is logged -----------------------------------------------------------------------------------------------

struct Line {
  bool warning = false;
  std::string text;
};

inline std::string Hex(const uint8_t* bytes, size_t n) {
  std::string s;
  char two[3];
  for (size_t i = 0; i < n; ++i) {
    std::snprintf(two, sizeof(two), "%02x", bytes[i]);
    s += two;
  }
  return s;
}

/// Install's decision without the process: `armed` is the count the driver runs with (0 is off), and the lines
/// Install logs. `text` is the config value (null when absent); `base` is the game image, read only on a server
/// with a count above 0.
struct InstallResult {
  uint32_t armed = 0;
  std::vector<Line> lines;
};
inline InstallResult InstallLines(bool isServer, const char* text, const uint8_t* base) {
  InstallResult out;
  if (!isServer) {
    out.lines.push_back({false, "[NEVR.GAMESERVER] server bots not applied: this is a client (the server adds bots)"});
    return out;
  }
  std::string problem;
  const uint32_t wanted = ParseBotsPerSession(text, &problem);
  if (!problem.empty()) {
    out.lines.push_back({true, "[NEVR.GAMESERVER] network.bots_per_session=" + std::string(text) + " is " + problem +
                                   " (allowed 0.." + std::to_string(kMaxBots) + "); server bots off"});
    return out;
  }
  if (wanted == 0) {
    out.lines.push_back({false, std::string("[NEVR.GAMESERVER] server bots off bots_per_session=0 source=") +
                                    (text != nullptr && text[0] != '\0' ? "config.yaml" : "default")});
    return out;
  }
  const size_t bad = FirstMismatch(base);
  if (bad != kWitnessCount) {
    const Witness& w = kWitnesses[bad];
    char va[24];
    std::snprintf(va, sizeof(va), "0x%llx", static_cast<unsigned long long>(0x140000000ULL + w.rva));
    out.lines.push_back({true, "[NEVR.GAMESERVER] server bots off bots_per_session=" + std::to_string(wanted) +
                                   ": the game's " + w.name + " at " + va +
                                   " is not the build this was read from expected=" + Hex(w.bytes, w.size) +
                                   " actual=" + Hex(base + w.rva, w.size)});
    return out;
  }
  out.armed = wanted;
  out.lines.push_back({false, "[NEVR.GAMESERVER] server bots armed bots_per_session=" + std::to_string(wanted) +
                                  " sites=" + std::to_string(kWitnessCount) +
                                  " (each arena or combat match in game gets up to " + std::to_string(wanted) +
                                  " AI bots, one a second)"});
  return out;
}

inline std::string PoolText(const BodyPool& p) { return p.found ? std::to_string(p.free) : std::string("none"); }

inline Line NoBodyLine(const std::string& session, uint32_t requested, uint32_t wanted, uint16_t entrants,
                       const BodyPool body[2]) {
  return {true, "[NEVR.GAMESERVER] server bots: no free bot body in this level session=" + session +
                    " requested=" + std::to_string(requested) + " wanted=" + std::to_string(wanted) +
                    " entrants=" + std::to_string(entrants) + " bot_arena_pool=" + PoolText(body[0]) +
                    " bot_combat_pool=" + PoolText(body[1]) + " (no more bots this session until one frees)"};
}

inline Line PendingLine(const std::string& session, uint32_t requested, uint32_t wanted) {
  return {true, "[NEVR.GAMESERVER] server bots: the game has not answered the last bot request for " +
                    std::to_string(kPendingWarningMs / 1000) + " s session=" + session + " requested=" +
                    std::to_string(requested) + " wanted=" + std::to_string(wanted) +
                    " (no more bots this session until it does)"};
}

inline Line RequestLine(bool accepted, const std::string& session, uint32_t requested, uint32_t wanted,
                        uint16_t entrants) {
  return {!accepted, std::string("[NEVR.GAMESERVER] server bot ") +
                         (accepted ? "requested" : "request refused by the game") + " session=" + session +
                         " n=" + std::to_string(requested) + " of " + std::to_string(wanted) +
                         " entrants=" + std::to_string(entrants)};
}

inline Line SettingsLine(const SettingsRewrite& r, const std::string& session, uint64_t frameBytes) {
  return {IsSettingsFailure(r.result),
          std::string("[NEVR.GAMESERVER] server bots session settings ") + SettingsResultName(r.result) +
              " session=" + session + " game_type=" + r.gameType +
              " numteams=" + (r.addedNumTeams ? std::to_string(kNumTeams) : "kept") +
              " players_per_team=" + (r.addedPlayersPerTeam ? std::to_string(r.playersPerTeamWritten) : "kept") +
              " settings_bytes=" + std::to_string(r.settingsBytes) + " rewritten_bytes=" +
              std::to_string(r.rewrittenBytes) + " frame_bytes=" + std::to_string(frameBytes) +
              (IsSettingsFailure(r.result) ? " (forwarded as it came; no bots this session)"
               : r.result == SettingsResult::kNotAMatch
                   ? " (forwarded as it came; bots join arena and combat matches only)"
                   : "")};
}

// ---- One server update ---------------------------------------------------------------------------------------------

/// The session the driver adds bots to: its id for the lines, and whether its settings made it a bot session.
struct SessionView {
  std::string id = "none";
  bool bots = false;
};

struct DriverState {
  Schedule schedule;
  std::string session = "none";  // the session the schedule counts for
  bool runningLogged = false;    // the first update's line is written
};

/// One update of the driver: the schedule over the net game's fields, then, when a request is due and the level has a
/// free bot body, one RequestAddBot with the offline bit set. `netGame` is the game's net game (null before it
/// exists), `noPosition` the 12 bytes the request carries at +0x20, `findPool` and `requestAddBot` the game's
/// functions. Returns the lines to log.
template <typename FindPoolCall, typename AddBotCall>
std::vector<Line> Drive(DriverState& d, uint32_t wanted, void* netGame, const uint8_t* noPosition, uint64_t nowMs,
                        const SessionView& session, unsigned long thread, FindPoolCall findPool,
                        AddBotCall requestAddBot) {
  std::vector<Line> lines;
  if (!d.runningLogged) {
    d.runningLogged = true;
    lines.push_back({false, "[NEVR.GAMESERVER] server bots driver running thread=" + std::to_string(thread) +
                                " (bots join a match from its session start on)"});
  }
  if (netGame == nullptr) return lines;
  if (session.id != d.session) {  // a new session counts from 0, even with no frame out of game between
    d.schedule = Schedule{};
    d.session = session.id;
  }
  if (!session.bots) {
    d.schedule = Schedule{};
    return lines;
  }
  const NetGameView view = ReadNetGame(netGame);
  const Gate gate = Decide(d.schedule, wanted, view.inGame, view.entrants, view.pending, nowMs);
  // The flag itself, not the gate: the gate says kDone before kPending, so the last request would go unsaid. Out of
  // game Decide has just cleared the schedule, so the flag starts no stretch there.
  if (PendingWarningDue(d.schedule, view.pending, nowMs)) {
    lines.push_back(PendingLine(session.id, d.schedule.requested, wanted));
  }
  if (gate != Gate::kAsk) return lines;
  BodyPool body[2];
  ReadBodyPools(findPool, ReadAt<void*>(netGame, kNetGamePoolsOffset), body);
  if (!BodyFree(body, 2)) {
    if (NoBodyWarningDue(d.schedule, nowMs)) {
      lines.push_back(NoBodyLine(session.id, d.schedule.requested, wanted, view.entrants, body));
    }
    return lines;
  }
  uint8_t* const lobby = ReadAt<uint8_t*>(netGame, kNetGameLobbyOffset);
  if (lobby == nullptr) return lines;
  const Request request = BuildRequest(noPosition);
  const bool accepted =
      WithOfflineBit(HostingFlags(lobby), [&] { return requestAddBot(netGame, request.bytes) != 0; });
  Requested(d.schedule, accepted);
  lines.push_back(RequestLine(accepted, session.id, d.schedule.requested, wanted, view.entrants));
  return lines;
}

}  // namespace nevr_server_bots
