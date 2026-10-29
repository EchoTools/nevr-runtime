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
// Pure: the decisions, the staging algorithm and the lobby-id layout, with the game, the thread, the clock and the log behind
// a Seam. The platform glue is patch/invite_join.cpp; src/runtime/tests/test_invite_join.cpp runs every case on the host.
//
// What a line may carry (docs/standards/logging.md, [NEVR.INVITE]): the lobby id (a match id, not the code) and the state the
// join came from. Never the code or any other text of the answer.

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "abi/echovr.h"
#include "runtime/patch/invite_worker.h"

namespace nevr_invite_join {

/// The lobby id as the game stores it: a Windows GUID, 16 bytes (Data1 u32 little-endian, Data2 u16, Data3 u16, Data4 8 bytes).
using LobbyBytes = std::array<std::uint8_t, 16>;

/// Offset within the game object of the staged lobby id (16 bytes): echovr.exe 0x1400fdc3d (`lea 0x8540(%rbx)`), 0x1400fdd84.
inline constexpr std::uintptr_t kStagedLobbyOffset = 0x8540;
/// Offset within the game object of the staged team word (16 bits): echovr.exe 0x1400fdc56 (`movzwl 0x8560(%rbx)`), 0x1400fdd8b.
inline constexpr std::uintptr_t kStagedTeamOffset = 0x8560;
/// The team word a join by code stages: 0xFFFF, the "no team" value the game's join-by-id path also uses (docs/adr/0017).
inline constexpr std::uint16_t kTeamWord = 0xFFFF;
/// Milliseconds after a join begins during which another join is refused as busy. A bound on a join that never shows: the
/// answer after it begins again. The value is a choice, not a measurement; the live run measures how long a join takes.
inline constexpr std::int64_t kJoinWindowMs = 15000;

/// Why a join did not start. None: the join began.
enum class Reason { None, Off, WrongThread, Prologue, Busy, NotAtMenu, StageFailed };

/// The token the log line carries for a reason (the issue's list, `reason=<...>`).
inline const char* ReasonName(Reason reason) {
  switch (reason) {
    case Reason::None:
      return "none";
    case Reason::Off:
      return "off";
    case Reason::WrongThread:
      return "wrong_thread";
    case Reason::Prologue:
      return "prologue";
    case Reason::Busy:
      return "busy";
    case Reason::NotAtMenu:
      return "not_at_menu";
    case Reason::StageFailed:
      return "stage_failed";
  }
  return "unknown";
}

/// The level of a not-started line: Warning for a game binary that does not match (prologue) and a stage that could not
/// be verified (both degrade the feature); Info for the rest, which are the expected reasons a click does nothing.
inline nevr_invite::LogLevel LevelOf(Reason reason) {
  return reason == Reason::Prologue || reason == Reason::StageFailed ? nevr_invite::LogLevel::Warning
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

/// Stages the lobby id and the team word, reads both back, and returns true only when both read back as written. On any
/// failure the bytes that were there before are written back, so nothing is left staged for a PLAY to consume.
inline bool StageAndVerify(StagingMemory& memory, const LobbyBytes& lobby, std::uint16_t team) {
  LobbyBytes priorLobby{};
  std::uint16_t priorTeam = 0;
  if (!memory.Read(kStagedLobbyOffset, priorLobby.data(), priorLobby.size()) ||
      !memory.Read(kStagedTeamOffset, &priorTeam, sizeof(priorTeam))) {
    return false;
  }
  const auto undo = [&] {
    memory.Write(kStagedLobbyOffset, priorLobby.data(), priorLobby.size());
    memory.Write(kStagedTeamOffset, &priorTeam, sizeof(priorTeam));
  };
  if (!memory.Write(kStagedLobbyOffset, lobby.data(), lobby.size()) ||
      !memory.Write(kStagedTeamOffset, &team, sizeof(team))) {
    undo();
    return false;
  }
  LobbyBytes readLobby{};
  std::uint16_t readTeam = 0;
  if (!memory.Read(kStagedLobbyOffset, readLobby.data(), readLobby.size()) ||
      !memory.Read(kStagedTeamOffset, &readTeam, sizeof(readTeam)) || readLobby != lobby || readTeam != team) {
    undo();
    return false;
  }
  return true;
}

/// The game-side seam the join runs through. The platform glue fills it; the host tests fill it with fakes.
struct Seam {
  std::function<bool()> on_game_thread;                                           // the calling thread runs the game's main loop
  std::function<bool()> prologues_valid;                                          // BeginMultiplayerLobby and QueueGameThreadCall
  std::function<std::optional<EchoVR::NetGameState>()> net_state;                 // nullopt: no net game, or unreadable
  std::function<bool(const LobbyBytes&, std::uint16_t)> stage_lobby;              // StageAndVerify on the game object
  std::function<void()> begin;                                                    // BeginMultiplayerLobby(game)
  std::function<std::int64_t()> now_ms;                                           // monotonic milliseconds
  std::function<void(nevr_invite::LogLevel, const std::string&)> log;             // [NEVR.INVITE] lines
};

/// One join per resolved answer, from the game's thread. The window state is the one this object keeps.
class Joiner {
 public:
  explicit Joiner(Seam seam) : seam_(std::move(seam)) {}

  /// Offers the match the answer names. Returns Reason::None when the join began (the game's begin ran once), otherwise the
  /// reason it did not, which is also logged. `feature_on` is the invite feature's switch (network.invite_codes).
  /// The gates are checked in this order, so one reason is logged: off, the thread, the prologues, the window, the menu,
  /// then the stage. The window is checked before the menu, so a repeat during a join reads busy, not not_at_menu.
  Reason Offer(std::string_view match_id, bool feature_on) {
    LobbyBytes lobby{};
    const bool parsed = ParseLobbyBytes(match_id, lobby);
    const std::string id = parsed ? UpperUuid(match_id) : std::string("invalid");
    const auto refuse = [&](Reason reason, const std::string& extra) {
      seam_.log(LevelOf(reason), std::string("[NEVR.INVITE] join not started reason=") + ReasonName(reason) + " lobby=" + id + extra);
      return reason;
    };
    if (!feature_on) return refuse(Reason::Off, "");
    if (!seam_.on_game_thread()) return refuse(Reason::WrongThread, "");
    if (!seam_.prologues_valid()) return refuse(Reason::Prologue, "");
    const std::int64_t now = seam_.now_ms();
    if (open_ && now - opened_ms_ < kJoinWindowMs) return refuse(Reason::Busy, "");
    const std::optional<EchoVR::NetGameState> state = seam_.net_state();
    if (!state.has_value() || *state != EchoVR::NetGameState::LoggedIn) {
      return refuse(Reason::NotAtMenu, std::string(" state_from=") + (state.has_value() ? std::to_string(static_cast<int>(*state)) : "none"));
    }
    if (!parsed || !seam_.stage_lobby(lobby, kTeamWord)) return refuse(Reason::StageFailed, "");
    open_ = true;
    opened_ms_ = now;
    seam_.log(nevr_invite::LogLevel::Info,
              "[NEVR.INVITE] join started route=stage lobby=" + id + " state_from=" + std::to_string(static_cast<int>(*state)));
    seam_.begin();
    return Reason::None;
  }

 private:
  Seam seam_;
  bool open_ = false;
  std::int64_t opened_ms_ = 0;
};

}  // namespace nevr_invite_join
