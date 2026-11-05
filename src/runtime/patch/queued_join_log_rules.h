#pragma once
// What the client did with a lobby session success, and what became of a join the game queued (#2163): the decision
// and the text of the [NEVR.MATCH] lines, apart from the hooks that read the game (patch/queued_join_log.cpp).
//
// The game's LobbySessionSuccessCB (echovr.exe 0x14017db90) reads the lobby status at +0x448. Outside 4, 5, 6 it returns
// with no log line. Inside, with the game in a game session (game state 9, flag bits 2 and 5 of [[game+0x2da0]] clear)
// it queues the join and switches the lobby to status 7; otherwise it starts the join and the status becomes 8. A
// message too large for the queue buffer leaves the status as it was. The hooks read the status before and after the
// original runs, so the verdict is what the game did, not a copy of its conditions.
//
// Platform neutral: no Windows headers, no logging, no game state.

#include <cstdint>
#include <string>

#include "runtime/compat/lobby_frame.h"

namespace nevr_queued_join_log {

constexpr std::int32_t kStatusQueued = 7;  // "Join Queued"
constexpr std::int32_t kStatusJoining = 8;

enum class Action { Joined, Queued, Ignored, Unclassified };

struct Verdict {
  Action action;
  const char* reason;  // a token, no spaces
};

// True when the game's own gate lets a success through: status - 4 < 3, unsigned.
constexpr bool GateOpen(std::int32_t status) { return static_cast<std::uint32_t>(status - 4) < 3U; }

inline Verdict Classify(std::int32_t before, std::int32_t after) {
  if (!GateOpen(before)) {
    return {Action::Ignored, before == kStatusQueued ? "already_queued" : "status_not_4_5_6"};
  }
  if (after == kStatusQueued) return {Action::Queued, "in_game_session"};
  if (after == kStatusJoining) return {Action::Joined, "no_game_session"};
  if (after == before) return {Action::Ignored, "message_too_large"};
  return {Action::Unclassified, "status_changed_again"};
}

inline const char* ActionName(Action a) {
  switch (a) {
    case Action::Joined:
      return "joined";
    case Action::Queued:
      return "queued";
    case Action::Ignored:
      return "ignored";
    case Action::Unclassified:
      return "unclassified";
  }
  return "unclassified";
}

// The consequence of each action, in the line itself (logging.md Category G).
inline const char* ActionConsequence(Action a) {
  switch (a) {
    case Action::Joined:
      return "the join request was sent";
    case Action::Queued:
      return "the join waits until the game leaves the current game session";
    case Action::Ignored:
      return "the game dropped this success and sends no join request for it";
    case Action::Unclassified:
      return "the status moved to a value this record does not name";
  }
  return "";
}

// The session id as text, the game's own spelling (%08X-%04hX-%04hX-%02hhX%02hhX-...), or "none" for the nil id.
inline std::string SessionText(const nevr_lobby_frame::Guid& id) { return nevr_lobby_frame::FormatGuidOrNone(id); }

inline std::string FormatSuccessLine(std::int32_t before, std::int32_t after, const nevr_lobby_frame::Guid& session) {
  const Verdict v = Classify(before, after);
  return "[NEVR.MATCH] session success status_at_arrival=" + std::to_string(before) +
         " status_after=" + std::to_string(after) + " session=" + SessionText(session) +
         " action=" + ActionName(v.action) + " reason=" + v.reason + " (" + ActionConsequence(v.action) + ")";
}

enum class LeaveResult { Started, NotStarted, NothingQueued };

inline const char* LeaveResultName(LeaveResult r) {
  switch (r) {
    case LeaveResult::Started:
      return "started";
    case LeaveResult::NotStarted:
      return "not_started";
    case LeaveResult::NothingQueued:
      return "nothing_queued";
  }
  return "not_started";
}

// queuedBefore / queuedAfter: whether the queue buffer held a session id (not the game's invalid id) before and after
// the game's "Leave For Match" ran.
inline LeaveResult ClassifyLeave(bool queuedBefore, bool queuedAfter) {
  if (!queuedBefore) return LeaveResult::NothingQueued;
  return queuedAfter ? LeaveResult::NotStarted : LeaveResult::Started;
}

inline const char* LeaveConsequence(LeaveResult r) {
  switch (r) {
    case LeaveResult::Started:
      return "the game left its session and sent the queued join request";
    case LeaveResult::NotStarted:
      return "the game's own conditions held the join back and it is still queued";
    case LeaveResult::NothingQueued:
      return "no join was queued, so nothing was sent";
  }
  return "";
}

inline std::string FormatLeaveLine(LeaveResult result, const nevr_lobby_frame::Guid& session, std::int32_t before,
                                   std::int32_t after) {
  return std::string("[NEVR.MATCH] queued join acted on session=") + SessionText(session) +
         " result=" + LeaveResultName(result) + " status_before=" + std::to_string(before) +
         " status_after=" + std::to_string(after) + " (" + LeaveConsequence(result) + ")";
}

// A queued join is dropped when the status leaves 7 for anything but 8 (8 is the join starting, the acted-on line).
constexpr bool IsDrop(std::int32_t from, std::int32_t to) {
  return from == kStatusQueued && to != kStatusQueued && to != kStatusJoining;
}

inline std::string FormatDropLine(const nevr_lobby_frame::Guid& session, std::int32_t to) {
  return std::string("[NEVR.MATCH] queued join dropped session=") + SessionText(session) +
         " status_to=" + std::to_string(to) + " (no join request was sent for the queued session)";
}

}  // namespace nevr_queued_join_log
