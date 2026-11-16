#pragma once

// The session-start hold of a dedicated server (PLAN-v5 R6-a-2b; docs/adr/0045 decision 10; N152). When the service starts a session whose
// level and mode the server must load, the start frame is held while one worker resolves the session's content (cdn_start.h,
// ResolveDemandForSession), and the game's game loop forwards the frame only when that resolve published its set. A failed or late
// resolve ends the session; the server stays up. The rules (the hold's answers, the deadline) are session_demand_rules.h.
//
// The hold's state (the hold, the held frame, its accepts, the session id) is touched on the game thread, with one exception: OnSessionEnd
// on the shutdown thread (see OnSessionEnd below). The worker touches only its mailbox, under one mutex, and nothing else.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace nevr_session_demand {

// An accept that arrived while the start was held. Forwarded after the start, in arrival order (gameserver_callbacks.cpp).
struct QueuedAccept {
  std::vector<std::uint8_t> encoded;
  std::size_t entrants = 0;
  std::string sessionId;
};

enum class DueAction : std::uint8_t { kNone, kRelease, kFail };

// What the game loop must do this tick.
struct Due {
  DueAction action = DueAction::kNone;
  std::vector<std::uint8_t> frame;    // kRelease: the session start, forwarded first
  std::vector<QueuedAccept> accepts;  // kRelease: forwarded after the start; kFail: dropped (their count is logged)
  std::string reason;                 // kFail: the demand's token, or "deadline"
};

// Holds a copy of the session start `frame` for `sessionId` until its content is mounted, and starts the resolve. False when the frame
// is not readable: nothing is held and the caller forwards it at once (logged as a skip).
bool HoldSessionStart(const std::uint8_t* frame, std::size_t size, const std::string& sessionId);

// True while a session start is held.
bool Active();

// An accept that arrives while a start is held: queued (true), and the caller does not forward it. False: forward it now.
bool HoldAccept(const void* encoded, std::size_t size, std::size_t entrants, const char* sessionId);

// Once per game-loop tick: the worker's answer, or the deadline. Acts on nothing itself; the caller forwards or ends the session.
Due Poll();

// The held session ended and `sessionId` names it: its held start and its accepts are dropped, never forwarded later (EndsHeldSession).
// An end of any other session changes nothing. Two callers: the service's CODE_ENDED on the game thread (gameserver_callbacks.cpp), and
// EndServerSession(kShutdown). The game's own end (kGameEnded) is not a caller (F1: the held start is not yet the game's session), and
// kContentFailed needs none (Poll released the hold). A reconnect is not an end: a held start is kept across it (ProcessPendingReconnect).
// NOT locked. EndServerSession(kShutdown) also runs on the shutdown thread when the game thread did not run the shutdown handoff
// (gameserver.cpp, BeginGracefulShutdown); there a concurrent Poll on the game thread can race this function's writes. Nothing here
// protects that. The process is exiting, and the race is not fixed by this code: it needs its own issue.
void OnSessionEnd(const char* cause, const char* sessionId);

// Server shutdown: the worker stops at its next check. Does not join; the worker holds no game object.
void Shutdown();

}  // namespace nevr_session_demand
