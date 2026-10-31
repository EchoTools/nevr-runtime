#pragma once
// #1401 (D3 of #906), docs/adr/0017 ("Join"): starts the join of the match a resolved invite code names.
//
// The service answers `invite/resolve` with a match id (patch/invite_wizard.h ParseResolved). The client joins it by staging
// the lobby id in the game object, the block the game's own `-lobbyid` path uses, and then calling the game's begin on the
// game's thread:
//   - the 16-byte lobby id, in the Windows GUID layout, at game+0x8540 (kStagedLobbyOffset);
//   - the team word, 0xFFFF, at game+0x8560 (kStagedTeamOffset), a 16-bit field (`movzwl` and `mov %r15w` in
//     CR15Game::BeginMultiplayer, echovr.exe 0x1400fda70, measured in docs/adr/0017's Context and issue #1401);
//   - then BeginMultiplayerLobby(game) (echovr.exe 0x1400fddd0), which consumes the staged block and clears it.
// A staged block that is not consumed stays in the game object and a later PLAY would join that lobby, so a stage that
// does not verify is undone before anything else happens.
//
// The game's own menu join (the same function) joins elsewhere, and clears the fields, when another route is set: it takes the
// staged lobby at 0x8540 only when the lobby at game+0x8530 equals the game's invalid id (the compare at 0x1400fdbec-0x1400fdbff),
// and takes the 0x14015a400 route first when (game+0x8520 & 0xF) != 0 and [game+0x8528] != 0 (0x1400fdb99-0x1400fdbb8). So the
// join reads those three fields before it stages, and stages nothing when they would send the join elsewhere (CheckRoute).
// The invalid id is 16 zero bytes: 0x142100b20 lies past the raw data of .data (raw end 0x142098e00; zero-filled by the loader)
// and no instruction of echovr.exe writes it (its 133 instruction references are reads and address-of).
//
// Pure: the decisions, the staging algorithm and the lobby-id layout, with the game, the thread, the clock and the log behind
// a Seam. The platform glue is patch/invite_join_game.cpp; src/runtime/tests/test_invite_join.cpp runs every case on the host.
//
// What a line may carry (docs/standards/logging.md, [NEVR.INVITE]): the lobby id (a match id, not the code) and the state the
// join came from. Never the code or any other text of the answer.

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "abi/echovr.h"
#include "runtime/patch/invite_types.h"

namespace nevr_invite_join {

/// The lobby id as the game stores it: a Windows GUID, 16 bytes (Data1 u32 little-endian, Data2 u16, Data3 u16, Data4 8 bytes).
using LobbyBytes = std::array<std::uint8_t, 16>;

/// Offset within the game object of the staged lobby id (16 bytes): echovr.exe 0x1400fdc3d (`lea 0x8540(%rbx)`), 0x1400fdd84.
inline constexpr std::uintptr_t kStagedLobbyOffset = 0x8540;
/// Offset within the game object of the staged team word (16 bits): echovr.exe 0x1400fdc56 (`movzwl 0x8560(%rbx)`), 0x1400fdd8b.
inline constexpr std::uintptr_t kStagedTeamOffset = 0x8560;
/// The team word a join by code stages: 0xFFFF, the "no team" value the game's join-by-id path also uses (docs/adr/0017).
inline constexpr std::uint16_t kTeamWord = 0xFFFF;
/// Offset of the first route word (the low nibble of its first byte is tested): echovr.exe 0x1400fdb99 (`test byte [rbx+0x8520], 0xf`).
inline constexpr std::uintptr_t kRouteFlagsOffset = 0x8520;
/// Offset of the second route word (8 bytes, tested against zero): echovr.exe 0x1400fdbb0 (`cmp qword [rbx+0x8528], 0`).
inline constexpr std::uintptr_t kRouteSlotOffset = 0x8528;
/// Offset of the other lobby id (16 bytes, compared with the invalid id): echovr.exe 0x1400fdbf3 (`lea rcx, [rbx+0x8530]`).
inline constexpr std::uintptr_t kRouteLobbyOffset = 0x8530;
/// The game's invalid lobby id (echovr.exe 0x142100b20): 16 zero bytes.
inline constexpr std::array<std::uint8_t, 16> kInvalidLobby = {};
/// The offsets one build stages and reads. kPcLayout is echovr.exe's (the constants above); kQuestLayout is libr15's
/// CR15Game (docs/adr/0017, #966 S4): every field is 8 lower, and the route compare is on the second lobby word (libr15
/// 0x1301d88 BeginMultiplayer: byte +0x8518 & 0xf, qword +0x8520 gate, SUuid +0x8528 vs kInvalid; staged SUuid +0x8538,
/// team u16 +0x8558).
struct Layout {
  std::uintptr_t staged_lobby;
  std::uintptr_t staged_team;
  std::uintptr_t route_flags;
  std::uintptr_t route_slot;
  std::uintptr_t route_lobby;
};
inline constexpr Layout kPcLayout = {kStagedLobbyOffset, kStagedTeamOffset, kRouteFlagsOffset, kRouteSlotOffset, kRouteLobbyOffset};
inline constexpr Layout kQuestLayout = {0x8538, 0x8558, 0x8518, 0x8520, 0x8528};
/// Milliseconds after a join begins during which another join is refused as busy. A bound on a join that never shows: the
/// answer after it begins again. The value is a choice, not a measurement; the live run measures how long a join takes.
inline constexpr std::int64_t kJoinWindowMs = 15000;

/// Why a join did not start. None: the join began.
enum class Reason { None, Off, Server, WrongThread, Prologue, Busy, NotAtMenu, OtherRoute, StageFailed, NoGameObject };

/// The token the log line carries for a reason (the issue's list, `reason=<...>`).
inline const char* ReasonName(Reason reason) {
  switch (reason) {
    case Reason::None:
      return "none";
    case Reason::Off:
      return "off";
    case Reason::Server:
      return "server";
    case Reason::WrongThread:
      return "wrong_thread";
    case Reason::Prologue:
      return "prologue";
    case Reason::Busy:
      return "busy";
    case Reason::NotAtMenu:
      return "not_at_menu";
    case Reason::OtherRoute:
      return "other_route";
    case Reason::StageFailed:
      return "stage_failed";
    case Reason::NoGameObject:
      return "no_game_object";
  }
  return "unknown";
}

/// The level of a not-started line: Warning for a game binary that does not match (prologue), a stage that could not be
/// verified and a game object that vanished after the stage (all degrade the feature); Info for the rest, which are the
/// expected reasons a click does nothing.
inline nevr_invite::LogLevel LevelOf(Reason reason) {
  return reason == Reason::Prologue || reason == Reason::StageFailed || reason == Reason::NoGameObject
             ? nevr_invite::LogLevel::Warning
             : nevr_invite::LogLevel::Info;
}

namespace detail {

inline int HexValue(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

}  // namespace detail

/// The 36-character form `xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx` (hex digits, either case; hyphens at 8, 13, 18, 23) as the
/// Windows GUID bytes. False for any other text.
inline bool ParseLobbyBytes(std::string_view uuid, LobbyBytes& out) {
  if (uuid.size() != 36) return false;
  std::array<std::uint8_t, 32> nibbles{};
  std::size_t digits = 0;
  for (std::size_t i = 0; i < uuid.size(); ++i) {
    const char c = uuid[i];
    if (i == 8 || i == 13 || i == 18 || i == 23) {
      if (c != '-') return false;
      continue;
    }
    const int value = detail::HexValue(c);
    if (value < 0) return false;
    nibbles[digits++] = static_cast<std::uint8_t>(value);
  }
  // The text order: sixteen bytes, most significant first. The GUID stores Data1, Data2 and Data3 little-endian.
  std::array<std::uint8_t, 16> text{};
  for (std::size_t i = 0; i < text.size(); ++i) {
    text[i] = static_cast<std::uint8_t>((nibbles[2 * i] << 4) | nibbles[2 * i + 1]);
  }
  out[0] = text[3];
  out[1] = text[2];
  out[2] = text[1];
  out[3] = text[0];
  out[4] = text[5];
  out[5] = text[4];
  out[6] = text[7];
  out[7] = text[6];
  for (std::size_t i = 8; i < 16; ++i) out[i] = text[i];
  return true;
}

/// The id in upper case, for a log line (docs/standards/logging.md Rule 16: a lobby is its GUID in upper case).
inline std::string UpperUuid(std::string_view uuid) {
  std::string out(uuid);
  for (char& c : out) {
    if (c >= 'a' && c <= 'f') c = static_cast<char>(c - 'a' + 'A');
  }
  return out;
}

/// The game object's memory, as the staging writes and reads it. A false from either is a refusal (unreadable or unwritable).
class StagingMemory {
 public:
  virtual ~StagingMemory() = default;
  virtual bool Read(std::uintptr_t offset, void* out, std::size_t size) = 0;
  virtual bool Write(std::uintptr_t offset, const void* in, std::size_t size) = 0;
};

/// The lobby id as the text the service writes: upper case, 8-4-4-4-12, from the Windows GUID bytes (the inverse of ParseLobbyBytes).
inline std::string LobbyText(const LobbyBytes& bytes) {
  static const char kDigits[] = "0123456789ABCDEF";
  const std::array<std::uint8_t, 16> text = {bytes[3], bytes[2], bytes[1], bytes[0], bytes[5], bytes[4], bytes[7], bytes[6],
                                              bytes[8], bytes[9], bytes[10], bytes[11], bytes[12], bytes[13], bytes[14], bytes[15]};
  std::string out;
  for (std::size_t i = 0; i < text.size(); ++i) {
    if (i == 4 || i == 6 || i == 8 || i == 10) out.push_back('-');
    out.push_back(kDigits[text[i] >> 4]);
    out.push_back(kDigits[text[i] & 0x0F]);
  }
  return out;
}

/// The three fields the game's own join reads before it uses the staged lobby, as read.
struct RouteState {
  std::uint8_t flags = 0;
  std::uint64_t slot = 0;
  LobbyBytes lobby{};
};

/// Whether the game's own join would send a join elsewhere than the staged lobby (see the header comment).
inline bool RoutesElsewhere(const RouteState& route) {
  return ((route.flags & 0x0F) != 0 && route.slot != 0) || route.lobby != kInvalidLobby;
}

/// What a stage did. Ok: both fields read back as written. Otherwise the step that failed and the field it failed on, which the
/// not-started line names.
enum class StageStep { Ok, Read, Write, Mismatch, OtherRoute };
enum class StageField { None, Lobby, Team, RouteFlags, RouteSlot, RouteLobby };

inline const char* StepName(StageStep step) {
  switch (step) {
    case StageStep::Ok:
      return "ok";
    case StageStep::Read:
      return "read";
    case StageStep::Write:
      return "write";
    case StageStep::Mismatch:
      return "mismatch";
    case StageStep::OtherRoute:
      return "other_route";
  }
  return "unknown";
}

inline const char* FieldName(StageField field) {
  switch (field) {
    case StageField::None:
      return "none";
    case StageField::Lobby:
      return "lobby";
    case StageField::Team:
      return "team";
    case StageField::RouteFlags:
      return "route_flags";
    case StageField::RouteSlot:
      return "route_slot";
    case StageField::RouteLobby:
      return "route_lobby";
  }
  return "unknown";
}

/// The bytes the staged fields held before the stage, kept so a stage can be put back.
struct PriorStage {
  LobbyBytes lobby{};
  std::uint16_t team = 0;
};

struct StageOutcome {
  StageStep step = StageStep::Ok;
  StageField field = StageField::None;
  RouteState route;  // set when step is OtherRoute
  PriorStage prior;  // the bytes to put back; set when step is Ok
  bool ok() const { return step == StageStep::Ok; }
};

using LogFn = std::function<void(nevr_invite::LogLevel, const std::string&)>;

/// Puts the prior bytes back. A write that fails is logged at Error naming the field, because a staged lobby left in the game
/// object is what a later PLAY would join. True only when both fields were written back.
inline bool Unstage(StagingMemory& memory, const PriorStage& prior, const LogFn& log, const Layout& layout = kPcLayout) {
  bool restored = true;
  if (!memory.Write(layout.staged_lobby, prior.lobby.data(), prior.lobby.size())) {
    restored = false;
    log(nevr_invite::LogLevel::Error, "[NEVR.INVITE] join stage undo failed field=lobby step=write");
  }
  if (!memory.Write(layout.staged_team, &prior.team, sizeof(prior.team))) {
    restored = false;
    log(nevr_invite::LogLevel::Error, "[NEVR.INVITE] join stage undo failed field=team step=write");
  }
  return restored;
}

/// Reads the route fields the game's own join tests before it uses the staged lobby. False on a read that fails (the field is
/// named in `failed`).
inline bool ReadRoute(StagingMemory& memory, RouteState& route, StageField& failed, const Layout& layout = kPcLayout) {
  if (!memory.Read(layout.route_flags, &route.flags, sizeof(route.flags))) {
    failed = StageField::RouteFlags;
    return false;
  }
  if (!memory.Read(layout.route_slot, &route.slot, sizeof(route.slot))) {
    failed = StageField::RouteSlot;
    return false;
  }
  if (!memory.Read(layout.route_lobby, route.lobby.data(), route.lobby.size())) {
    failed = StageField::RouteLobby;
    return false;
  }
  return true;
}

/// Stages the lobby id and the team word, reads both back, and succeeds only when both read back as written. Nothing is staged
/// when the game's own join would go elsewhere (OtherRoute, with the values read). On any failure after the first write the bytes
/// that were there before are written back (a write-back that fails is logged at Error naming the field).
inline StageOutcome StageAndVerify(StagingMemory& memory, const LobbyBytes& lobby, std::uint16_t team, const LogFn& log,
                                   const Layout& layout = kPcLayout) {
  StageOutcome outcome;
  const auto fail = [&](StageStep step, StageField field) {
    outcome.step = step;
    outcome.field = field;
    return outcome;
  };
  RouteState route;
  StageField failedField = StageField::None;
  if (!ReadRoute(memory, route, failedField, layout)) return fail(StageStep::Read, failedField);
  if (RoutesElsewhere(route)) {
    outcome.route = route;
    return fail(StageStep::OtherRoute, StageField::RouteLobby);
  }
  PriorStage prior;
  if (!memory.Read(layout.staged_lobby, prior.lobby.data(), prior.lobby.size())) return fail(StageStep::Read, StageField::Lobby);
  if (!memory.Read(layout.staged_team, &prior.team, sizeof(prior.team))) return fail(StageStep::Read, StageField::Team);
  if (!memory.Write(layout.staged_lobby, lobby.data(), lobby.size())) {
    Unstage(memory, prior, log, layout);
    return fail(StageStep::Write, StageField::Lobby);
  }
  if (!memory.Write(layout.staged_team, &team, sizeof(team))) {
    Unstage(memory, prior, log, layout);
    return fail(StageStep::Write, StageField::Team);
  }
  LobbyBytes readLobby{};
  std::uint16_t readTeam = 0;
  if (!memory.Read(layout.staged_lobby, readLobby.data(), readLobby.size())) {
    Unstage(memory, prior, log, layout);
    return fail(StageStep::Read, StageField::Lobby);
  }
  if (!memory.Read(layout.staged_team, &readTeam, sizeof(readTeam))) {
    Unstage(memory, prior, log, layout);
    return fail(StageStep::Read, StageField::Team);
  }
  if (readLobby != lobby) {
    Unstage(memory, prior, log, layout);
    return fail(StageStep::Mismatch, StageField::Lobby);
  }
  if (readTeam != team) {
    Unstage(memory, prior, log, layout);
    return fail(StageStep::Mismatch, StageField::Team);
  }
  outcome.prior = prior;
  return outcome;
}

/// The game-side seam the join runs through. The platform glue fills it; the host tests fill it with fakes.
struct Seam {
  std::function<bool()> is_server;                                                // a dedicated server never joins by code
  std::function<bool()> on_game_thread;                                           // the calling thread runs the game's main loop
  std::function<bool()> prologues_valid;                                          // BeginMultiplayerLobby and QueueGameThreadCall
  std::function<std::optional<EchoVR::NetGameState>()> net_state;                 // nullopt: no net game, or unreadable
  std::function<StageOutcome(const LobbyBytes&, std::uint16_t)> stage_lobby;      // StageAndVerify on the game object
  std::function<bool()> begin;                                                    // BeginMultiplayerLobby(game); false: no game object, not called
  std::function<bool()> unstage;                                                  // put the last stage back; false: it could not be
  std::function<std::int64_t()> now_ms;                                           // monotonic milliseconds
  std::function<void(nevr_invite::LogLevel, const std::string&)> log;             // [NEVR.INVITE] lines
};

/// One join per resolved answer, from the game's thread. The window state is the one this object keeps.
class Joiner {
 public:
  explicit Joiner(Seam seam) : seam_(std::move(seam)) {}

  /// Offers the match the answer names. Returns Reason::None when the join began (the game's begin ran once), otherwise the
  /// reason it did not, which is also logged. `feature_on` is the invite feature's switch (network.invite_codes).
  /// The gates are checked in this order, so one reason is logged: off, the server, the thread, the prologues, the window, the
  /// menu, then the stage (other route, then the writes). The window is checked before the menu, so a repeat during a join reads busy, not not_at_menu.
  Reason Offer(std::string_view match_id, bool feature_on) {
    LobbyBytes lobby{};
    const bool parsed = ParseLobbyBytes(match_id, lobby);
    const std::string id = parsed ? UpperUuid(match_id) : std::string("invalid");
    const auto refuse = [&](Reason reason, const std::string& extra) {
      seam_.log(LevelOf(reason), std::string("[NEVR.INVITE] join not started reason=") + ReasonName(reason) + " lobby=" + id + extra);
      return reason;
    };
    if (!feature_on) return refuse(Reason::Off, "");
    if (seam_.is_server()) return refuse(Reason::Server, "");
    if (!seam_.on_game_thread()) return refuse(Reason::WrongThread, "");
    if (!seam_.prologues_valid()) return refuse(Reason::Prologue, "");
    const std::int64_t now = seam_.now_ms();
    if (open_ && now - opened_ms_ < kJoinWindowMs) return refuse(Reason::Busy, "");
    const std::optional<EchoVR::NetGameState> state = seam_.net_state();
    if (!state.has_value() || *state != EchoVR::NetGameState::LoggedIn) {
      return refuse(Reason::NotAtMenu, std::string(" state_from=") + (state.has_value() ? std::to_string(static_cast<int>(*state)) : "none"));
    }
    if (!parsed) return refuse(Reason::StageFailed, " step=parse field=lobby");
    const StageOutcome staged = seam_.stage_lobby(lobby, kTeamWord);
    if (staged.step == StageStep::OtherRoute) {
      char flags[8];
      std::snprintf(flags, sizeof(flags), "0x%02x", static_cast<unsigned>(staged.route.flags));
      char slot[24];
      std::snprintf(slot, sizeof(slot), "0x%llx", static_cast<unsigned long long>(staged.route.slot));
      return refuse(Reason::OtherRoute, std::string(" route_flags=") + flags + " route_slot=" + slot +
                                            " route_lobby=" + LobbyText(staged.route.lobby));
    }
    if (!staged.ok()) {
      return refuse(Reason::StageFailed, std::string(" step=") + StepName(staged.step) + " field=" + FieldName(staged.field));
    }
    open_ = true;
    opened_ms_ = now;
    seam_.log(nevr_invite::LogLevel::Info,
              "[NEVR.INVITE] join started route=stage lobby=" + id + " state_from=" + std::to_string(static_cast<int>(*state)));
    if (!seam_.begin()) {
      // No game object to begin on: the stage is put back, and the window closes so the next answer is not told busy.
      open_ = false;
      if (!seam_.unstage()) {
        seam_.log(nevr_invite::LogLevel::Error, "[NEVR.INVITE] join stage undo failed after begin step=unstage field=lobby");
      }
      return refuse(Reason::NoGameObject, "");
    }
    return Reason::None;
  }

 private:
  Seam seam_;
  bool open_ = false;
  std::int64_t opened_ms_ = 0;
};

}  // namespace nevr_invite_join
